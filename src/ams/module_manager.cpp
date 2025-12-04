/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: module_manager.cpp
 * Purpose: Implement AMS module lifecycle management plus OverlayFS
 * orchestration.
 * Last Modified: December 4th, 2025. - 09:07 AM Eastern Time
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

#include "ams/module_manager.hpp"

#include "config.hpp"
#include "fs.hpp"
#include "logger.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits.h>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct ScopedTempDir {
  explicit ScopedTempDir(std::string p) : path(std::move(p)) {}
  ~ScopedTempDir() {
    if (!path.empty()) {
      apm::fs::removeDirRecursive(path);
    }
  }
  void release() { path.clear(); }
  std::string path;
};

struct OverlayTarget {
  const char *name;
  const char *mountPoint;
};

constexpr OverlayTarget kOverlayTargets[] = {
    {"system", "/system"},
    {"vendor", "/vendor"},
    {"product", "/product"},
};

std::string baseMirrorPath(const OverlayTarget &target) {
  return apm::fs::joinPath(apm::config::getModuleRuntimeBaseDir(),
                           target.name);
}

std::string normalizeMountPath(const std::string &p) {
  if (p.size() > 1 && p.back() == '/')
    return p.substr(0, p.size() - 1);
  return p;
}

std::string resolveForMount(const std::string &path) {
  char resolvedBuf[PATH_MAX];
  if (::realpath(path.c_str(), resolvedBuf)) {
    return normalizeMountPath(resolvedBuf);
  }
  return normalizeMountPath(path);
}

bool isRegularFile(const std::string &path) { return apm::fs::isFile(path); }

bool ensureDir(const std::string &path) { return apm::fs::createDirs(path); }

std::string shellEscapeSingleQuotes(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char c : value) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool runCommand(const std::string &cmd, std::string *errorMsg) {
  int rc = ::system(cmd.c_str());
  if (rc != 0) {
    if (errorMsg)
      *errorMsg =
          "Command failed: " + cmd + " (exit code " + std::to_string(rc) + ")";
    return false;
  }
  return true;
}

bool unmountPath(const std::string &path, std::string *errorMsg) {
  if (::umount2(path.c_str(), MNT_DETACH) == 0)
    return true;

  int err = errno;
  if (err == EINVAL || err == ENOENT)
    return true;

  if (errorMsg)
    *errorMsg = "Failed to unmount " + path + ": " + std::strerror(err);
  return false;
}

bool readMountType(const std::string &path, std::string &type) {
  std::ifstream mounts("/proc/self/mounts");
  if (!mounts.is_open())
    return false;

  std::string resolved = resolveForMount(path);

  auto isWithinMount = [](const std::string &p,
                          const std::string &mountPoint) {
    if (mountPoint == "/")
      return true;
    if (p.size() < mountPoint.size())
      return false;
    if (p.compare(0, mountPoint.size(), mountPoint) != 0)
      return false;
    return p.size() == mountPoint.size() || p[mountPoint.size()] == '/';
  };

  std::string bestType;
  size_t bestMatchLen = 0;
  std::string line;
  while (std::getline(mounts, line)) {
    std::istringstream iss(line);
    std::string source;
    std::string target;
    std::string fsType;
    if (!(iss >> source >> target >> fsType))
      continue;
    target = resolveForMount(target);
    if (!isWithinMount(resolved, target))
      continue;
    if (target.size() > bestMatchLen) {
      bestMatchLen = target.size();
      bestType = fsType;
    }
  }
  if (bestMatchLen == 0)
    return false;

  type = bestType;
  return true;
}

bool isOverlayMounted(const OverlayTarget &target) {
  std::string type;
  if (!readMountType(target.mountPoint, type))
    return false;
  return type == "overlay";
}

bool isPathMounted(const std::string &path) {
  std::ifstream mounts("/proc/self/mounts");
  if (!mounts.is_open())
    return false;

  std::string resolved = resolveForMount(path);
  std::string line;
  while (std::getline(mounts, line)) {
    std::istringstream iss(line);
    std::string source;
    std::string target;
    std::string fsType;
    if (!(iss >> source >> target >> fsType))
      continue;
    if (resolveForMount(target) == resolved)
      return true;
  }
  return false;
}

bool ensureBaseMirrorForTarget(const OverlayTarget &target,
                               std::string *errorMsg) {
  std::string baseDir = baseMirrorPath(target);
  if (!ensureDir(baseDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create base mirror directory for " +
                  std::string(target.name);
    return false;
  }

  if (isPathMounted(baseDir))
    return true;

  std::string currentType;
  if (!readMountType(target.mountPoint, currentType)) {
    if (errorMsg)
      *errorMsg = "Unable to determine mount type for " +
                  std::string(target.mountPoint);
    return false;
  }

  if (currentType == "overlay") {
    if (errorMsg)
      *errorMsg =
          std::string("Cannot snapshot base for ") + target.mountPoint +
          " because it is already overlay-mounted. Reboot into a clean state "
          "before enabling AMS modules.";
    return false;
  }

  if (::mount(target.mountPoint, baseDir.c_str(), nullptr,
              MS_BIND | MS_REC, nullptr) != 0) {
    if (errorMsg)
      *errorMsg = "Failed to mirror " + std::string(target.mountPoint) + ": " +
                  std::strerror(errno);
    return false;
  }

  if (::mount(nullptr, baseDir.c_str(), nullptr,
              MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr) != 0) {
    apm::logger::warn("Failed to remount base mirror " + baseDir +
                      " read-only: " + std::strerror(errno));
  }

  apm::logger::info("AMS captured base mount for " +
                    std::string(target.mountPoint));
  return true;
}

bool ensureBaseMirrors(std::string *errorMsg) {
  for (const auto &target : kOverlayTargets) {
    if (!ensureBaseMirrorForTarget(target, errorMsg))
      return false;
  }
  return true;
}

bool mountBaseOnly(const OverlayTarget &target, std::string *errorMsg) {
  std::string baseDir = baseMirrorPath(target);
  if (!isPathMounted(baseDir)) {
    if (errorMsg)
      *errorMsg = "Missing base mirror for " + std::string(target.name);
    return false;
  }

  if (::mount(baseDir.c_str(), target.mountPoint, nullptr,
              MS_BIND | MS_REC, nullptr) != 0) {
    if (errorMsg)
      *errorMsg = "Failed to restore base mount for " +
                  std::string(target.mountPoint) + ": " +
                  std::strerror(errno);
    return false;
  }

  if (::mount(nullptr, target.mountPoint, nullptr,
              MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr) != 0) {
    apm::logger::warn("Failed to remount " + std::string(target.mountPoint) +
                      " read-only after restoring base: " +
                      std::strerror(errno));
  }

  apm::logger::info("AMS restored stock " +
                    std::string(target.mountPoint) + " mount");
  return true;
}

} // namespace

namespace apm::ams {

ModuleManager::ModuleManager()
    : modulesRoot_(apm::config::getModulesDir()),
      logsRoot_(apm::config::getModuleLogsDir()) {
  apm::fs::createDirs(modulesRoot_);
  apm::fs::createDirs(logsRoot_);
}

ModuleManager::~ModuleManager() { stopPartitionMonitor(); }

std::string ModuleManager::modulePath(const std::string &name) const {
  return apm::fs::joinPath(modulesRoot_, name);
}

std::string ModuleManager::moduleLogPath(const std::string &name) const {
  return apm::fs::joinPath(logsRoot_, name + ".log");
}

bool ModuleManager::extractZip(const std::string &zipPath,
                               const std::string &dest,
                               std::string *errorMsg) const {
  if (!ensureDir(dest)) {
    if (errorMsg)
      *errorMsg = "Failed to create temp directory: " + dest;
    return false;
  }

  std::ostringstream cmd;
  cmd << "unzip -oq '" << shellEscapeSingleQuotes(zipPath) << "' -d '"
      << shellEscapeSingleQuotes(dest) << "'";

  if (!runCommand(cmd.str(), errorMsg)) {
    return false;
  }

  return true;
}

std::string ModuleManager::makeTempDir(const std::string &tag) const {
  using namespace std::chrono;
  static std::uint64_t counter = 0;
  auto now = duration_cast<microseconds>(steady_clock::now().time_since_epoch())
                 .count();
  std::ostringstream name;
  name << "ams-" << tag << "-" << now << "-" << ++counter;
  auto path = apm::fs::joinPath(apm::config::getCacheDir(), name.str());
  if (!ensureDir(path))
    return {};
  return path;
}

void ModuleManager::logModuleEvent(const std::string &name,
                                   const std::string &message) const {
  apm::fs::createDirs(logsRoot_);
  std::ostringstream line;
  line << "[" << makeIsoTimestamp() << "] " << message << "\n";
  apm::fs::appendFile(moduleLogPath(name), line.str());
  apm::logger::info("AMS(" + name + "): " + message);
}

bool ModuleManager::installFromZip(const std::string &zipPath,
                                   ModuleOperationResult &out) {
  out = ModuleOperationResult{};
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (zipPath.empty()) {
    out.message = "Module zip path is empty";
    return false;
  }
  if (!isRegularFile(zipPath)) {
    out.message = "Module zip not found: " + zipPath;
    return false;
  }

  std::string tempDir = makeTempDir("zip");
  if (tempDir.empty()) {
    out.message = "Failed to create temporary directory";
    return false;
  }
  ScopedTempDir cleanupTemp(tempDir);

  if (!extractZip(zipPath, tempDir, &out.message)) {
    return false;
  }

  std::string moduleRoot = tempDir;
  std::string infoPath = apm::fs::joinPath(moduleRoot, "module-info.json");
  if (!isRegularFile(infoPath)) {
    auto entries = apm::fs::listDir(tempDir, false);
    if (entries.size() == 1) {
      std::string candidate = apm::fs::joinPath(tempDir, entries[0]);
      std::string nestedInfo = apm::fs::joinPath(candidate, "module-info.json");
      if (apm::fs::isDirectory(candidate) && isRegularFile(nestedInfo)) {
        moduleRoot = candidate;
        infoPath = nestedInfo;
      }
    }
  }

  ModuleInfo info;
  std::string err;
  if (!readModuleInfoFile(infoPath, info, &err)) {
    out.message = err;
    return false;
  }

  std::string overlayRoot = apm::fs::joinPath(moduleRoot, "overlay");
  if (!apm::fs::isDirectory(overlayRoot)) {
    out.message = "Module overlay directory missing";
    return false;
  }

  std::string finalPath = modulePath(info.name);
  apm::fs::removeDirRecursive(finalPath);

  apm::fs::createDirs(modulesRoot_);
  if (::rename(moduleRoot.c_str(), finalPath.c_str()) != 0) {
    out.message = std::string("Failed to move module into place: ") +
                  std::strerror(errno);
    return false;
  }

  // moduleRoot has been moved; prevent cleanup from deleting the new root.
  cleanupTemp.release();
  apm::fs::removeDirRecursive(tempDir);

  std::string workDir = apm::fs::joinPath(finalPath, "workdir");
  apm::fs::createDirs(workDir);
  for (const auto &target : kOverlayTargets) {
    apm::fs::createDirs(apm::fs::joinPath(workDir, target.name));
  }

  ModuleState state;
  state.enabled = true;
  state.installedAt = makeIsoTimestamp();
  state.updatedAt = state.installedAt;
  state.lastError.clear();

  if (!writeState(finalPath, state, &err)) {
    out.message = err;
    return false;
  }

  logModuleEvent(info.name, "Installed module from " + zipPath);

  if (!applyEnabledModules(&err)) {
    state.lastError = err;
    writeState(finalPath, state, nullptr);
    out.message = err;
    return false;
  }

  runLifecycleScripts(info, finalPath, false);

  out.ok = true;
  out.message = "Installed module '" + info.name + "'";
  return true;
}

bool ModuleManager::loadModule(const std::string &name, ModuleInfo &info,
                               ModuleState &state,
                               std::string *errorMsg) const {
  std::string dir = modulePath(name);
  if (!apm::fs::isDirectory(dir)) {
    if (errorMsg)
      *errorMsg = "Module not found: " + name;
    return false;
  }

  std::string infoPath = apm::fs::joinPath(dir, "module-info.json");
  if (!readModuleInfoFile(infoPath, info, errorMsg))
    return false;

  std::string statePath = apm::fs::joinPath(dir, "state.json");
  if (!readModuleState(statePath, state, errorMsg))
    return false;

  if (state.installedAt.empty())
    state.installedAt = makeIsoTimestamp();
  return true;
}

bool ModuleManager::writeState(const std::string &moduleDir, ModuleState &state,
                               std::string *errorMsg) const {
  state.updatedAt = makeIsoTimestamp();
  std::string path = apm::fs::joinPath(moduleDir, "state.json");
  return writeModuleState(path, state, errorMsg);
}

bool ModuleManager::enableModule(const std::string &name,
                                 ModuleOperationResult &out) {
  out = ModuleOperationResult{};
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ModuleInfo info;
  ModuleState state;
  std::string err;
  if (!loadModule(name, info, state, &err)) {
    out.message = err;
    return false;
  }
  if (state.enabled) {
    out.ok = true;
    out.message = "Module already enabled";
    return true;
  }

  state.enabled = true;
  state.lastError.clear();
  std::string dir = modulePath(name);
  if (!writeState(dir, state, &err)) {
    out.message = err;
    return false;
  }

  logModuleEvent(name, "Module enabled");

  if (!applyEnabledModules(&err)) {
    state.lastError = err;
    writeState(dir, state, nullptr);
    out.message = err;
    return false;
  }

  runLifecycleScripts(info, dir, false);

  out.ok = true;
  out.message = "Module enabled";
  return true;
}

bool ModuleManager::disableModule(const std::string &name,
                                  ModuleOperationResult &out) {
  out = ModuleOperationResult{};
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ModuleInfo info;
  ModuleState state;
  std::string err;
  if (!loadModule(name, info, state, &err)) {
    out.message = err;
    return false;
  }
  if (!state.enabled) {
    out.ok = true;
    out.message = "Module already disabled";
    return true;
  }

  state.enabled = false;
  std::string dir = modulePath(name);
  if (!writeState(dir, state, &err)) {
    out.message = err;
    return false;
  }

  logModuleEvent(name, "Module disabled");

  if (!applyEnabledModules(&err)) {
    state.lastError = err;
    writeState(dir, state, nullptr);
    out.message = err;
    return false;
  }

  out.ok = true;
  out.message = "Module disabled";
  return true;
}

bool ModuleManager::removeModule(const std::string &name,
                                 ModuleOperationResult &out) {
  out = ModuleOperationResult{};
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ModuleInfo info;
  ModuleState state;
  std::string err;
  if (!loadModule(name, info, state, &err)) {
    out.message = err;
    return false;
  }

  if (state.enabled) {
    ModuleOperationResult dummy;
    disableModule(name, dummy);
  }

  std::string dir = modulePath(name);
  apm::fs::removeDirRecursive(dir);
  apm::fs::removeFile(moduleLogPath(name));

  logModuleEvent(name, "Module removed");

  applyEnabledModules(nullptr);

  out.ok = true;
  out.message = "Module removed";
  return true;
}

bool ModuleManager::ensureRuntimeDirs(std::string *errorMsg) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!ensureDir(apm::config::getModuleRuntimeDir())) {
    if (errorMsg)
      *errorMsg = "Failed to create runtime dir: " +
                  apm::config::getModuleRuntimeDir();
    return false;
  }
  if (!ensureDir(apm::config::getModuleRuntimeUpperDir())) {
    if (errorMsg)
      *errorMsg = "Failed to create runtime upper dir";
    return false;
  }
  if (!ensureDir(apm::config::getModuleRuntimeWorkDir())) {
    if (errorMsg)
      *errorMsg = "Failed to create runtime work dir";
    return false;
  }
  if (!ensureDir(apm::config::getModuleRuntimeBaseDir())) {
    if (errorMsg)
      *errorMsg = "Failed to create runtime base dir";
    return false;
  }
  for (const auto &target : kOverlayTargets) {
    if (!ensureDir(apm::fs::joinPath(apm::config::getModuleRuntimeUpperDir(),
                                     target.name))) {
      if (errorMsg)
        *errorMsg =
            "Failed to prepare upper dir for " + std::string(target.name);
      return false;
    }
    if (!ensureDir(apm::fs::joinPath(apm::config::getModuleRuntimeWorkDir(),
                                     target.name))) {
      if (errorMsg)
        *errorMsg =
            "Failed to prepare work dir for " + std::string(target.name);
      return false;
    }
    if (!ensureDir(apm::fs::joinPath(apm::config::getModuleRuntimeBaseDir(),
                                     target.name))) {
      if (errorMsg)
        *errorMsg =
            "Failed to prepare base dir for " + std::string(target.name);
      return false;
    }
  }
  return true;
}

ModuleManager::OverlayStacks ModuleManager::buildOverlayStacks() const {
  OverlayStacks stacks;

  std::vector<std::string> modules = apm::fs::listDir(modulesRoot_, false);
  for (const auto &name : modules) {
    std::string dir = modulePath(name);
    if (!apm::fs::isDirectory(dir))
      continue;

    ModuleInfo info;
    ModuleState state;
    if (!loadModule(name, info, state, nullptr))
      continue;

    if (!state.enabled || !info.mount)
      continue;

    std::string overlayRoot = apm::fs::joinPath(dir, "overlay");
    if (!apm::fs::isDirectory(overlayRoot))
      continue;

    for (const auto &target : kOverlayTargets) {
      std::string subdir =
          apm::fs::joinPath(overlayRoot, std::string(target.name));
      if (apm::fs::isDirectory(subdir)) {
        stacks[target.name].push_back(subdir);
      }
    }
  }

  return stacks;
}

bool ModuleManager::applyOverlayForTarget(std::size_t targetIndex,
                                          const std::vector<std::string> &layers,
                                          std::string *errorMsg) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (targetIndex >= std::size(kOverlayTargets)) {
    if (errorMsg)
      *errorMsg = "Invalid overlay target index";
    return false;
  }

  const auto &target = kOverlayTargets[targetIndex];
  std::string localErr;

  if (!ensureRuntimeDirs(&localErr)) {
    apm::logger::error("AMS overlay: " + localErr);
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }

  if (!ensureBaseMirrorForTarget(target, &localErr)) {
    apm::logger::error("AMS overlay: " + localErr);
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }

  bool overlayActive = isOverlayMounted(target);

  if (layers.empty() && !overlayActive)
    return true;

  if (!unmountPath(target.mountPoint, &localErr)) {
    apm::logger::error("AMS overlay: " + localErr);
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }

  if (layers.empty()) {
    if (!mountBaseOnly(target, &localErr)) {
      apm::logger::error("AMS overlay: " + localErr);
      if (errorMsg)
        *errorMsg = localErr;
      return false;
    }
    apm::logger::info("AMS restored stock " +
                      std::string(target.mountPoint) + " mount");
    if (errorMsg)
      errorMsg->clear();
    return true;
  }

  std::ostringstream lower;
  for (size_t i = 0; i < layers.size(); ++i) {
    if (i > 0)
      lower << ':';
    lower << layers[i];
  }
  lower << ':' << baseMirrorPath(target);

  std::string upperDir = apm::fs::joinPath(
      apm::config::getModuleRuntimeUpperDir(), target.name);
  std::string workDir = apm::fs::joinPath(apm::config::getModuleRuntimeWorkDir(),
                                          target.name);

  std::ostringstream opts;
  opts << "lowerdir=" << lower.str() << ",upperdir=" << upperDir
       << ",workdir=" << workDir;

  if (::mount("overlay", target.mountPoint, "overlay", 0,
              opts.str().c_str()) != 0) {
    localErr = std::string("Overlay mount failed for ") +
               target.mountPoint + ": " + std::strerror(errno);
    mountBaseOnly(target, nullptr);
    apm::logger::error("AMS overlay: " + localErr);
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }

  apm::logger::info("AMS overlay updated for " +
                    std::string(target.mountPoint));
  if (errorMsg)
    errorMsg->clear();
  return true;
}

bool ModuleManager::rebuildOverlays(std::string *errorMsg) {
  if (!ensureRuntimeDirs(errorMsg))
    return false;

  OverlayStacks stacks = buildOverlayStacks();
  bool ok = true;
  std::string firstErr;

  for (std::size_t idx = 0; idx < std::size(kOverlayTargets); ++idx) {
    std::vector<std::string> layers;
    auto it = stacks.find(kOverlayTargets[idx].name);
    if (it != stacks.end())
      layers = it->second;

    std::string perErr;
    if (!applyOverlayForTarget(idx, layers, &perErr)) {
      ok = false;
      if (firstErr.empty() && !perErr.empty())
        firstErr = perErr;
      else if (perErr.empty() && firstErr.empty())
        firstErr = "Overlay apply failed for " +
                   std::string(kOverlayTargets[idx].mountPoint);
    }
  }

  if (errorMsg) {
    if (!firstErr.empty())
      *errorMsg = firstErr;
    else
      errorMsg->clear();
  }

  return ok;
}

bool ModuleManager::runScript(const std::string &path,
                              const std::string &moduleName,
                              bool background) const {
  if (!isRegularFile(path))
    return true;

  std::string logPath = moduleLogPath(moduleName);
  std::ostringstream cmd;
  cmd << "/system/bin/sh '" << shellEscapeSingleQuotes(path) << "' >>'"
      << shellEscapeSingleQuotes(logPath) << "' 2>&1";
  if (background)
    cmd << " &";

  int rc = ::system(cmd.str().c_str());
  if (rc != 0) {
    logModuleEvent(moduleName, "Script failed: " + path +
                                   " (rc=" + std::to_string(rc) + ")");
    return false;
  }

  logModuleEvent(moduleName, std::string("Executed script: ") + path +
                                 (background ? " (background)" : ""));
  return true;
}

bool ModuleManager::runLifecycleScripts(const ModuleInfo &info,
                                        const std::string &moduleDir,
                                        bool isStartup) const {
  (void)isStartup;
  bool ok = true;
  if (info.runPostFsData) {
    std::string script = apm::fs::joinPath(moduleDir, "post-fs-data.sh");
    ok &= runScript(script, info.name, false);
  }
  if (info.runService) {
    std::string script = apm::fs::joinPath(moduleDir, "service.sh");
    ok &= runScript(script, info.name, true);
  }
  return ok;
}

bool ModuleManager::applyEnabledModules(std::string *errorMsg) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return rebuildOverlays(errorMsg);
}

void ModuleManager::startEnabledModules() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto modules = apm::fs::listDir(modulesRoot_, false);
  for (const auto &name : modules) {
    ModuleInfo info;
    ModuleState state;
    if (!loadModule(name, info, state, nullptr))
      continue;
    if (!state.enabled)
      continue;
    runLifecycleScripts(info, modulePath(name), true);
  }
}

bool ModuleManager::listModules(std::vector<ModuleStatusEntry> &out,
                                std::string *errorMsg) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  out.clear();
  auto modules = apm::fs::listDir(modulesRoot_, false);
  for (const auto &name : modules) {
    ModuleInfo info;
    ModuleState state;
    if (!loadModule(name, info, state, errorMsg))
      return false;
    ModuleStatusEntry entry;
    entry.info = info;
    entry.state = state;
    entry.path = modulePath(name);
    out.push_back(std::move(entry));
  }
  if (errorMsg)
    errorMsg->clear();
  return true;
}

bool ModuleManager::isPartitionMounted(const std::string &mountPoint) const {
  std::ifstream mounts("/proc/mounts");
  if (!mounts.is_open())
    return false;

  std::string normalized = resolveForMount(mountPoint);
  std::string line;
  while (std::getline(mounts, line)) {
    std::istringstream iss(line);
    std::string source;
    std::string target;
    std::string fsType;
    if (!(iss >> source >> target >> fsType))
      continue;
    if (resolveForMount(target) == normalized)
      return true;
  }

  return false;
}

void ModuleManager::monitorPartitions() {
  using namespace std::chrono_literals;
  std::set<std::size_t> observed;

  constexpr int kMaxIterations = 30;
  constexpr auto kSleep = 5s;

  for (int attempt = 0; attempt < kMaxIterations && !monitorStop_.load();
       ++attempt) {
    OverlayStacks stacks;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      stacks = buildOverlayStacks();
    }

    for (std::size_t idx = 0; idx < std::size(kOverlayTargets); ++idx) {
      if (observed.count(idx))
        continue;
      if (!isPartitionMounted(kOverlayTargets[idx].mountPoint))
        continue;

      std::vector<std::string> layers;
      auto it = stacks.find(kOverlayTargets[idx].name);
      if (it != stacks.end())
        layers = it->second;

      std::string err;
      if (!applyOverlayForTarget(idx, layers, &err)) {
        apm::logger::error(
            "Partition monitor: failed to apply overlay for " +
            std::string(kOverlayTargets[idx].mountPoint) +
            (err.empty() ? "" : (": " + err)));
      }
      observed.insert(idx);
    }

    if (observed.size() == std::size(kOverlayTargets) || monitorStop_.load())
      break;

    std::this_thread::sleep_for(kSleep);
  }

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  monitorRunning_ = false;
}

void ModuleManager::startPartitionMonitor() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (monitorRunning_)
    return;

  monitorStop_.store(false);
  monitorRunning_ = true;
  monitorThread_ = std::thread([this]() { monitorPartitions(); });
}

void ModuleManager::stopMonitorLocked() { monitorStop_.store(true); }

void ModuleManager::stopPartitionMonitor() {
  std::thread worker;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stopMonitorLocked();
    worker.swap(monitorThread_);
    monitorRunning_ = false;
  }

  if (worker.joinable())
    worker.join();
}

bool ModuleManager::incrementBootCounter(const std::string &path,
                                         std::uint64_t *newValue) {
  std::uint64_t current = getBootCounter(path);
  if (current == UINT64_MAX)
    current = 0;
  ++current;

  if (!apm::fs::writeFile(path, std::to_string(current), true))
    return false;

  ::chmod(path.c_str(), 0600);
  if (newValue)
    *newValue = current;
  return true;
}

std::uint64_t ModuleManager::getBootCounter(const std::string &path) {
  std::string raw;
  if (!apm::fs::readFile(path, raw))
    return 0;

  errno = 0;
  char *end = nullptr;
  unsigned long long val = std::strtoull(raw.c_str(), &end, 10);
  if (errno != 0 || end == raw.c_str())
    return 0;
  return static_cast<std::uint64_t>(val);
}

bool ModuleManager::resetBootCounter(const std::string &path) {
  if (!apm::fs::writeFile(path, "0", true))
    return false;
  ::chmod(path.c_str(), 0600);
  return true;
}

std::uint64_t ModuleManager::getBootThreshold(const std::string &path,
                                              std::uint64_t defaultValue) {
  std::string raw;
  if (!apm::fs::readFile(path, raw))
    return defaultValue;

  errno = 0;
  char *end = nullptr;
  unsigned long long val = std::strtoull(raw.c_str(), &end, 10);
  if (errno != 0 || end == raw.c_str())
    return defaultValue;
  return val == 0 ? defaultValue : static_cast<std::uint64_t>(val);
}

bool ModuleManager::enterSafeMode(const std::string &path) {
  if (!apm::fs::writeFile(path, "1", true))
    return false;
  ::chmod(path.c_str(), 0600);
  return true;
}

bool ModuleManager::isSafeModeActive(const std::string &path) {
  return apm::fs::isFile(path);
}

bool ModuleManager::clearSafeMode(const std::string &path) {
  return apm::fs::removeFile(path);
}

} // namespace apm::ams
