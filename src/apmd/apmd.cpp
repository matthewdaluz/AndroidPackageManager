/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: apmd.cpp
 * Purpose: Bootstrap apmd, configure logging, and run the IPC-backed service loop.
 * Last Modified: 2026-03-18 10:55:01.568628991 -0400.
 * Author: Matthew DaLuz - RedHead Founder
 *
 * APM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * APM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with APM. If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "apmd.hpp"
#include "ams/module_manager.hpp"
#include "config.hpp"
#include "export_path.hpp"
#include "fs.hpp"
#include "ipc_server.hpp"
#include "logger.hpp"
#include "manual_package.hpp"
#include "process_lock.hpp"
#include "status_db.hpp"

#include <chrono>
#include <cerrno>
#include <fstream>
#include <grp.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char *kLegacyInstalledDir = "/data/apm/installed";
constexpr const char *kCurrentStagingInstalledDir = "/data/local/tmp/apm/installed";
constexpr const char *kLegacyManualPackagesDir = "/data/apm/manual-packages";
constexpr const char *kCurrentStagingManualPackagesDir =
    "/data/local/tmp/apm/manual-packages";
constexpr const char *kCurrentRuntimeLogsDir = "/data/local/tmp/apm/runtime/logs";
constexpr const char *kLegacyLogsDir = "/data/apm/logs";
constexpr const char *kCurrentStagingLogsDir = "/data/local/tmp/apm/logs";
constexpr const char *kCurrentRuntimeModuleLogsDir =
    "/data/local/tmp/apm/runtime/ams/logs";
constexpr const char *kLegacyModuleLogsDir = "/data/ams/logs";
constexpr const char *kCurrentStagingModuleLogsDir = "/data/local/tmp/apm/ams/logs";
constexpr const char *kLegacyTermuxPrefix = "/data/apm/installed/termux/usr";
constexpr const char *kCurrentStagingTermuxPrefix =
    "/data/local/tmp/apm/installed/termux/usr";

bool isDataAccessible() {
  struct stat st{};
  if (stat("/data", &st) != 0) {
    return false;
  }
  return access("/data", R_OK | W_OK | X_OK) == 0;
}

bool hasLegacyModules(std::string &detail) {
  const std::string legacyRoot = "/data/apm/modules";
  detail.clear();
  if (!apm::fs::isDirectory(legacyRoot))
    return false;

  auto entries = apm::fs::listDir(legacyRoot, false);
  for (const auto &name : entries) {
    std::string infoPath =
        apm::fs::joinPath(apm::fs::joinPath(legacyRoot, name),
                          "module-info.json");
    if (apm::fs::isFile(infoPath)) {
      detail = infoPath;
      return true;
    }
  }
  return false;
}

// Poll until /data is mounted and accessible to avoid crashing on startup.
void waitForDataReady() {
  using namespace std::chrono_literals;
  while (!isDataAccessible()) {
    std::cerr << "apmd: waiting for /data to become available..." << std::endl;
    std::this_thread::sleep_for(5s);
  }
}

bool hasPathPrefix(const std::string &path, const std::string &prefix) {
  return path.size() >= prefix.size() &&
         path.compare(0, prefix.size(), prefix) == 0;
}

std::string parentDir(const std::string &path) {
  auto slash = path.find_last_of('/');
  if (slash == std::string::npos) {
    return {};
  }
  return path.substr(0, slash);
}

std::string remapPathPrefix(const std::string &path, const std::string &oldPrefix,
                            const std::string &newPrefix) {
  if (!hasPathPrefix(path, oldPrefix)) {
    return path;
  }
  return newPrefix + path.substr(oldPrefix.size());
}

std::string remapAnyPrefix(
    const std::string &path,
    const std::vector<std::pair<std::string, std::string>> &prefixes) {
  for (const auto &entry : prefixes) {
    if (hasPathPrefix(path, entry.first)) {
      return entry.second + path.substr(entry.first.size());
    }
  }
  return path;
}

bool lookupShellGroup(gid_t &gidOut) {
  struct group *shellGroup = ::getgrnam("shell");
  if (!shellGroup) {
    return false;
  }
  gidOut = shellGroup->gr_gid;
  return true;
}

bool copyFileWithMode(const std::string &src, const std::string &dst) {
  std::ifstream in(src, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }

  auto slash = dst.find_last_of('/');
  if (slash != std::string::npos) {
    std::string parent = dst.substr(0, slash);
    if (!parent.empty() && !apm::fs::createDirs(parent)) {
      return false;
    }
  }

  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    return false;
  }

  out << in.rdbuf();
  if (!out.good()) {
    return false;
  }

  struct stat st{};
  if (::stat(src.c_str(), &st) == 0) {
    ::chmod(dst.c_str(), st.st_mode & 07777);
  }

  return true;
}

bool copyTreeRecursive(const std::string &src, const std::string &dst) {
  struct stat st{};
  if (::lstat(src.c_str(), &st) != 0) {
    return false;
  }

  if (S_ISDIR(st.st_mode)) {
    if (!apm::fs::createDirs(dst)) {
      return false;
    }
    auto entries = apm::fs::listDir(src, true);
    for (const auto &entry : entries) {
      if (entry == "." || entry == "..") {
        continue;
      }
      if (!copyTreeRecursive(apm::fs::joinPath(src, entry),
                             apm::fs::joinPath(dst, entry))) {
        return false;
      }
    }
    ::chmod(dst.c_str(), st.st_mode & 07777 ? st.st_mode & 07777 : 0775);
    return true;
  }

  if (S_ISLNK(st.st_mode)) {
    std::vector<char> buf(static_cast<std::size_t>(st.st_size > 0 ? st.st_size + 1 : 256), '\0');
    ssize_t len = ::readlink(src.c_str(), buf.data(), buf.size() - 1);
    if (len < 0) {
      return false;
    }
    auto slash = dst.find_last_of('/');
    if (slash != std::string::npos) {
      std::string parent = dst.substr(0, slash);
      if (!parent.empty() && !apm::fs::createDirs(parent)) {
        return false;
      }
    }
    ::unlink(dst.c_str());
    return ::symlink(buf.data(), dst.c_str()) == 0;
  }

  if (S_ISREG(st.st_mode)) {
    return copyFileWithMode(src, dst);
  }

  return true;
}

void normalizeShellReadableTree(const std::string &path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) {
    return;
  }

  gid_t shellGid = 0;
  const bool haveShellGid = lookupShellGroup(shellGid);

  if (S_ISDIR(st.st_mode)) {
    if (haveShellGid) {
      ::chown(path.c_str(), 0, shellGid);
    }
    mode_t mode = st.st_mode & 07777;
    mode |= S_IRGRP | S_IXGRP;
    if ((mode & S_IWUSR) != 0) {
      mode |= S_IWGRP;
    }
    ::chmod(path.c_str(), mode);

    for (const auto &entry : apm::fs::listDir(path, true)) {
      if (entry == "." || entry == "..") {
        continue;
      }
      normalizeShellReadableTree(apm::fs::joinPath(path, entry));
    }
    return;
  }

  if (S_ISREG(st.st_mode)) {
    if (haveShellGid) {
      ::chown(path.c_str(), 0, shellGid);
    }
    mode_t mode = st.st_mode & 07777;
    if ((mode & S_IRUSR) != 0) {
      mode |= S_IRGRP;
    }
    if ((mode & S_IWUSR) != 0) {
      mode |= S_IWGRP;
    }
    if ((mode & S_IXUSR) != 0) {
      mode |= S_IXGRP;
    }
    ::chmod(path.c_str(), mode);
  }
}

void migrateDirectoryIfNeeded(const std::string &legacyPath,
                              const std::string &newPath,
                              bool removeLegacyOnSuccess = false) {
  if (legacyPath.empty() || newPath.empty() || legacyPath == newPath) {
    return;
  }
  if (!apm::fs::pathExists(legacyPath)) {
    return;
  }

  if (apm::fs::pathExists(newPath) && !apm::fs::removeDirRecursive(newPath)) {
    apm::logger::warn("apmd: failed to clear stale migrated path " + newPath);
    return;
  }

  auto slash = newPath.find_last_of('/');
  if (slash != std::string::npos) {
    std::string parent = newPath.substr(0, slash);
    if (!parent.empty()) {
      apm::fs::createDirs(parent);
    }
  }

  if (!copyTreeRecursive(legacyPath, newPath)) {
    apm::logger::warn("apmd: failed to copy-migrate " + legacyPath + " to " +
                      newPath);
    return;
  }

  if (removeLegacyOnSuccess && !apm::fs::removeDirRecursive(legacyPath)) {
    apm::logger::warn("apmd: migrated " + legacyPath + " to " + newPath +
                      " but failed to remove legacy path");
  } else {
    apm::logger::info("apmd: migrated " + legacyPath + " to " + newPath);
  }
}

void migrateStatusPaths() {
  apm::status::InstalledDb db;
  std::string err;
  if (!apm::status::loadStatus(db, &err)) {
    apm::logger::warn("apmd: failed to load status DB for path migration: " +
                      err);
    return;
  }

  bool changed = false;
  for (auto &[_, pkg] : db) {
    std::string newInstallRoot = remapAnyPrefix(
        pkg.installRoot,
        {{kCurrentStagingInstalledDir, apm::config::getInstalledDir()},
         {kLegacyInstalledDir, apm::config::getInstalledDir()}});
    if (newInstallRoot != pkg.installRoot) {
      pkg.installRoot = std::move(newInstallRoot);
      changed = true;
    }

    std::string newInstallPrefix = remapAnyPrefix(
        pkg.installPrefix,
        {{kCurrentStagingTermuxPrefix, apm::config::getTermuxPrefix()},
         {kLegacyTermuxPrefix, apm::config::getTermuxPrefix()}});
    if (newInstallPrefix != pkg.installPrefix) {
      pkg.installPrefix = std::move(newInstallPrefix);
      changed = true;
    }
  }

  if (changed) {
    std::string writeErr;
    if (!apm::status::writeStatus(db, &writeErr)) {
      apm::logger::warn("apmd: failed to rewrite migrated status DB paths: " +
                        writeErr);
    } else {
      apm::logger::info("apmd: migrated stored package install paths");
    }
  }
}

void migrateManualPackageMetadata() {
  std::vector<apm::manual::PackageInfo> packages;
  std::string err;
  if (!apm::manual::listInstalledPackages(packages, &err)) {
    apm::logger::warn("apmd: failed to load manual package metadata for path "
                      "migration: " + err);
    return;
  }

  for (auto &pkg : packages) {
    std::string newPrefix = remapAnyPrefix(
        pkg.prefix,
        {{kCurrentStagingInstalledDir, apm::config::getInstalledDir()},
         {kLegacyInstalledDir, apm::config::getInstalledDir()}});
    if (newPrefix == pkg.prefix) {
      continue;
    }
    pkg.prefix = std::move(newPrefix);
    std::string saveErr;
    if (!apm::manual::saveInstalledPackage(pkg, &saveErr)) {
      apm::logger::warn("apmd: failed to rewrite manual package metadata for " +
                        pkg.name + ": " + saveErr);
    }
  }
}

void migrateShellAccessibleRuntime() {
  if (apm::config::isEmulatorMode()) {
    return;
  }

  migrateDirectoryIfNeeded(kCurrentStagingInstalledDir,
                           apm::config::getInstalledDir());
  migrateDirectoryIfNeeded(kLegacyInstalledDir, apm::config::getInstalledDir());
  migrateDirectoryIfNeeded(kCurrentStagingManualPackagesDir,
                           apm::config::getManualPackagesDir());
  migrateDirectoryIfNeeded(kLegacyManualPackagesDir,
                           apm::config::getManualPackagesDir());
  migrateDirectoryIfNeeded(kCurrentRuntimeLogsDir, apm::config::getLogsDir());
  migrateDirectoryIfNeeded(kCurrentStagingLogsDir, apm::config::getLogsDir());
  migrateDirectoryIfNeeded(kLegacyLogsDir, apm::config::getLogsDir());
  migrateDirectoryIfNeeded(kCurrentRuntimeModuleLogsDir,
                           apm::config::getModuleLogsDir());
  migrateDirectoryIfNeeded(kCurrentStagingModuleLogsDir,
                           apm::config::getModuleLogsDir());
  migrateDirectoryIfNeeded(kLegacyModuleLogsDir,
                           apm::config::getModuleLogsDir());

  migrateStatusPaths();
  migrateManualPackageMetadata();

  const std::vector<std::string> shellRuntimeDirs = {
      parentDir(apm::config::getInstalledDir()),
      apm::config::getInstalledDir(),
      apm::config::getCommandsDir(),
      apm::config::getDependenciesDir(),
      apm::config::getTermuxRoot(),
      apm::config::getTermuxPrefix(),
      apm::config::getTermuxInstalledDir(),
      apm::config::getTermuxHomeDir(),
      apm::config::getTermuxTmpDir(),
      apm::config::getLogsDir(),
      apm::config::getManualPackagesDir(),
      parentDir(apm::config::getModuleLogsDir()),
      apm::config::getModuleLogsDir(),
  };

  for (const auto &dir : shellRuntimeDirs) {
    if (!dir.empty()) {
      apm::fs::createDirs(dir);
    }
  }

  const std::string runtimeRoot = parentDir(apm::config::getInstalledDir());
  if (!runtimeRoot.empty() && apm::fs::pathExists(runtimeRoot)) {
    normalizeShellReadableTree(runtimeRoot);
  }

  const std::string runtimeBase = parentDir(runtimeRoot);
  if (!runtimeBase.empty() && apm::fs::pathExists(runtimeBase)) {
    normalizeShellReadableTree(runtimeBase);
  }
}

} // namespace

namespace apm::daemon {

// Configure logging, ensure the PATH/profile helper is loaded, and launch the
// IPC service loop.
int runDaemon(bool debugMode, bool emulatorMode,
              const std::string &socketPath) {
  // Set emulator mode early so path getters work correctly
  apm::config::setEmulatorMode(emulatorMode);

  // Process lock for emulator mode
  std::unique_ptr<apm::lock::ProcessLock> processLock;

  if (!emulatorMode) {
    waitForDataReady();
  } else {
    // Create emulator directory structure
    const std::vector<std::string> emulatorDirs = {
        apm::config::getApmRoot(),      apm::config::getInstalledDir(),
        apm::config::getCommandsDir(),  apm::config::getDependenciesDir(),
        apm::config::getCacheDir(),     apm::config::getPkgsDir(),
        apm::config::getLogsDir(),      apm::config::getModulesDir(),
        apm::config::getListsDir(),     apm::config::getManualPackagesDir(),
        apm::config::getApmBinDir(),    apm::config::getSourcesDir(),
        apm::config::getSourcesListD(), apm::config::getTrustedKeysDir(),
        apm::config::getSecurityDir()};

    for (const auto &dir : emulatorDirs) {
      if (!apm::fs::createDirs(dir)) {
        std::cerr << "Error: Failed to create emulator directory: " << dir
                  << std::endl;
        return 1;
      }
    }

    // Acquire process lock
    std::string lockPath = apm::config::getApmRoot() + "/apmd.lock";
    processLock = std::make_unique<apm::lock::ProcessLock>(lockPath);
    std::string lockErr;
    if (!processLock->acquire(&lockErr)) {
      std::cerr << "Error: " << lockErr << std::endl;
      return 1;
    }

    apm::logger::info("apmd: Created emulator directory structure at " +
                      apm::config::getApmRoot());
  }

  migrateShellAccessibleRuntime();

  std::string logFile = emulatorMode
                            ? apm::config::getLogsDir() + "/apmd-emulator.log"
                            : apm::config::getLogsDir() + "/apmd.log";
  apm::logger::setLogFile(logFile);
  apm::logger::setDebugControlFile(apm::config::getDebugFlagFile());
  apm::logger::setMinLogLevel(debugMode ? apm::logger::Level::Debug
                                        : apm::logger::Level::Info);

  apm::logger::info("apmd starting (IPC transport)");
  apm::logger::info("apmd: debug control file = " +
                    apm::config::getDebugFlagFile());
  if (debugMode) {
    apm::logger::info("apmd: DEBUG mode enabled");
  }
  if (emulatorMode) {
    apm::logger::info("apmd: EMULATOR mode enabled");
  }

  std::string legacyDetail;
  if (hasLegacyModules(legacyDetail)) {
    std::string msg =
        "Legacy modules detected under /data/apm/modules. Factory reset is "
        "required before running the AMSD-enabled release.";
    apm::logger::error("apmd: " + msg);
    if (!legacyDetail.empty())
      apm::logger::error("apmd: found module-info.json at " + legacyDetail);
    std::cerr << msg << std::endl;
    return 1;
  }

  apm::daemon::path::CommandHotloadSummary startupHotload;
  if (!apm::daemon::path::rebuild_command_index_and_shims("daemon-start",
                                                           &startupHotload)) {
    apm::logger::warn("apmd: startup hotload rebuild completed with warnings");
  }
  apm::daemon::path::ensureProfileLoaded();

  apm::ams::ModuleManager moduleManager;
  // AMS boot lifecycle is owned by amsd, which starts earlier during boot.
  // apmd keeps a ModuleManager only to service explicit module IPC requests
  // (install/list/enable/disable/remove) without re-applying overlays or
  // re-running module scripts on every apmd startup.

  std::string resolvedSocket =
      socketPath.empty() ? apm::config::getIpcSocketPath() : socketPath;
  apm::logger::info("apmd: transport mode = ipc");
  apm::logger::info("apmd: starting IPC server on " + resolvedSocket);
  apm::ipc::IpcServer server(resolvedSocket, moduleManager);
  if (!server.start()) {
    apm::logger::error("apmd: failed to start IPC server");
    return 1;
  }
  server.run();
  apm::logger::info("apmd: IPC server loop exited");
  return 0;
}

} // namespace apm::daemon

// Thin wrapper around runDaemon that parses --socket/--help flags.
int main(int argc, char **argv) {
  std::string socketPath = "";
  bool debugMode = false;
  bool emulatorMode = false;

  // Simple arg parser: apmd [--socket path] [--debug] [--emulator]
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--socket" && i + 1 < argc) {
      socketPath = argv[++i];
    } else if (arg == "--debug" || arg == "-d") {
      debugMode = true;
    } else if (arg == "--emulator") {
#ifndef APM_EMULATOR_MODE
      std::cerr
          << "Error: This binary was not compiled with emulator support.\n"
          << "Please rebuild with -DAPM_EMULATOR_MODE=ON.\n";
      return 1;
#else
      emulatorMode = true;
#endif
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "apmd - APM daemon\n"
                << "Usage: apmd [--socket <path>] [--debug]";
#ifdef APM_EMULATOR_MODE
      std::cout << " [--emulator]";
#endif
      std::cout << "\n"
                << "  --socket       Override IPC socket path\n"
                << "  --debug, -d    Enable verbose debug logging\n";
#ifdef APM_EMULATOR_MODE
      std::cout << "  --emulator     Run in x86_64 emulator mode\n";
#endif
      return 0;
    }
  }

#ifdef APM_EMULATOR_MODE
  if (!emulatorMode) {
    std::cerr << "Error: This binary was compiled with emulator support.\n"
              << "You must run it with the --emulator flag.\n";
    return 1;
  }
#else
  if (emulatorMode) {
    std::cerr << "Error: Emulator mode requested but binary not compiled with "
                 "emulator support.\n";
    return 1;
  }
#endif

  return apm::daemon::runDaemon(debugMode, emulatorMode, socketPath);
}
