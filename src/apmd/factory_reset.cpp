/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: factory_reset.cpp
 * Purpose: Implement daemon-side factory reset handling that wipes installed
 * content, credentials, modules, and system app overlays.
 * Last Modified: 2026-03-15 11:56:16.536679330 -0400.
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

#include "include/factory_reset.hpp"

#include "config.hpp"
#include "fs.hpp"
#include "logger.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

std::string exitStatusString(int rc) {
  if (rc == -1) {
    return "command failed";
  }
  if (WIFEXITED(rc)) {
    return "exit code " + std::to_string(WEXITSTATUS(rc));
  }
  if (WIFSIGNALED(rc)) {
    return "signal " + std::to_string(WTERMSIG(rc));
  }
  return "status " + std::to_string(rc);
}

int runPmCommand(const std::vector<std::string> &args,
                 std::string *errorMsg = nullptr) {
  if (args.empty()) {
    if (errorMsg) {
      *errorMsg = "pm args are empty";
    }
    return -1;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    if (errorMsg) {
      *errorMsg = "fork() failed: " + std::string(std::strerror(errno));
    }
    return -1;
  }

  if (pid == 0) {
    std::vector<char *> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char *>("pm"));
    for (const auto &arg : args) {
      argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp("pm", argv.data());
    _exit(127);
  }

  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    if (errorMsg) {
      *errorMsg = "waitpid() failed: " + std::string(std::strerror(errno));
    }
    return -1;
  }

  return status;
}

bool removePathRecursive(const std::string &path, const std::string &label,
                         std::vector<std::string> &errors) {
  if (path.empty())
    return true;

  if (!apm::fs::removeDirRecursive(path)) {
    std::string msg = "Failed to remove " + label + " at " + path;
    errors.push_back(msg);
    apm::logger::warn("factory_reset: " + msg);
    return false;
  }

  apm::logger::info("factory_reset: removed " + label + ": " + path);
  return true;
}

bool removeFilePath(const std::string &path, const std::string &label,
                    std::vector<std::string> &errors) {
  if (path.empty())
    return true;

  if (!apm::fs::removeFile(path)) {
    std::string msg = "Failed to remove " + label + " at " + path;
    errors.push_back(msg);
    apm::logger::warn("factory_reset: " + msg);
    return false;
  }

  apm::logger::info("factory_reset: removed " + label + ": " + path);
  return true;
}

bool ensureDirPath(const std::string &path, const std::string &label,
                   std::vector<std::string> &errors) {
  if (path.empty())
    return true;

  if (apm::fs::createDirs(path))
    return true;

  std::string msg = "Failed to recreate " + label + " at " + path;
  errors.push_back(msg);
  apm::logger::warn("wipe_cache: " + msg);
  return false;
}

bool hasSelectedCaches(const apm::daemon::WipeCacheSelection &selection) {
  return selection.apmGeneral || selection.repoLists ||
         selection.packageDownloads || selection.signatureCache ||
         selection.amsRuntime;
}

std::vector<std::string>
selectedCacheLabels(const apm::daemon::WipeCacheSelection &selection) {
  std::vector<std::string> labels;
  if (selection.apmGeneral)
    labels.emplace_back("APM general cache");
  if (selection.repoLists)
    labels.emplace_back("repository lists");
  if (selection.packageDownloads)
    labels.emplace_back("package downloads");
  if (selection.signatureCache)
    labels.emplace_back("signature cache");
  if (selection.amsRuntime)
    labels.emplace_back("AMS runtime cache");
  return labels;
}

std::string joinLabels(const std::vector<std::string> &labels) {
  std::ostringstream ss;
  for (std::size_t i = 0; i < labels.size(); ++i) {
    if (i > 0)
      ss << (i + 1 == labels.size() ? " and " : ", ");
    ss << labels[i];
  }
  return ss.str();
}

bool shouldPreserveEntry(const std::string &name,
                         const std::vector<std::string> &preserveNames) {
  for (const auto &entry : preserveNames) {
    if (entry == name)
      return true;
  }
  return false;
}

bool clearDirectoryContents(const std::string &path, const std::string &label,
                            std::vector<std::string> &errors,
                            const std::vector<std::string> &preserveNames = {}) {
  if (path.empty())
    return true;

  bool ok = true;
  if (apm::fs::pathExists(path)) {
    if (!apm::fs::isDirectory(path)) {
      ok &= removePathRecursive(path, label, errors);
    } else {
      auto entries = apm::fs::listDir(path, true);
      for (const auto &entry : entries) {
        if (shouldPreserveEntry(entry, preserveNames))
          continue;

        std::string childPath = apm::fs::joinPath(path, entry);
        if (!removePathRecursive(childPath, label + " entry", errors))
          ok = false;
      }
    }
  }

  if (!ensureDirPath(path, label, errors))
    ok = false;

  if (ok)
    apm::logger::info("wipe_cache: cleared " + label + ": " + path);
  return ok;
}

bool clearSignatureCache(std::vector<std::string> &errors) {
  return removeFilePath(
      apm::fs::joinPath(apm::config::getPkgsDir(), "sig-cache.json"),
      "signature cache", errors);
}

bool removeAllModules(apm::ams::ModuleManager &moduleManager,
                      std::vector<std::string> &errors) {
  std::vector<apm::ams::ModuleStatusEntry> modules;
  std::string listErr;
  if (!moduleManager.listModules(modules, &listErr)) {
    std::string msg = listErr.empty()
                          ? "Failed to enumerate AMS modules"
                          : ("Failed to enumerate AMS modules: " + listErr);
    errors.push_back(msg);
    apm::logger::warn("factory_reset: " + msg);
    return false;
  }

  bool ok = true;
  for (const auto &entry : modules) {
    apm::ams::ModuleOperationResult result;
    if (!moduleManager.removeModule(entry.info.name, result) || !result.ok) {
      ok = false;
      std::string msg = result.message.empty()
                            ? "Failed to remove module " + entry.info.name
                            : result.message;
      errors.push_back(msg);
      apm::logger::warn("factory_reset: " + msg);
    } else {
      apm::logger::info("factory_reset: removed module " + entry.info.name);
    }
  }

  return ok;
}

bool cleanupSystemApps(std::vector<std::string> &errors) {
  static const char *kModuleBaseDir = "/data/adb/modules/apm-system-apps";
  static const char *kModuleUpdateDir =
      "/data/adb/modules_update/apm-system-apps";
  static const char *kSystemAppRoot =
      "/data/adb/modules/apm-system-apps/system/app";

  if (apm::fs::isDirectory(kSystemAppRoot)) {
    auto overlays = apm::fs::listDir(kSystemAppRoot, false);
    for (const auto &dir : overlays) {
      std::string pmErr;
      int rc = runPmCommand({"uninstall", "--user", "0", dir}, &pmErr);
      if (rc == 0) {
        apm::logger::info("factory_reset: uninstalled system app package " +
                          dir);
      } else {
        std::string msg = "Failed to uninstall system app package " + dir +
                          " (pm uninstall --user 0: " + exitStatusString(rc) +
                          ")";
        if (!pmErr.empty()) {
          msg += ": " + pmErr;
        }
        errors.push_back(msg);
        apm::logger::warn("factory_reset: " + msg);
      }
    }
  }

  bool ok = true;
  if (!removePathRecursive(kModuleBaseDir, "system app module", errors))
    ok = false;
  if (!removePathRecursive(kModuleUpdateDir, "system app module update",
                           errors))
    ok = false;
  return ok;
}

std::string joinErrors(const std::vector<std::string> &errors) {
  std::ostringstream ss;
  for (std::size_t i = 0; i < errors.size(); ++i) {
    if (i > 0)
      ss << "; ";
    ss << errors[i];
  }
  return ss.str();
}

} // namespace

namespace apm::daemon {

bool performFactoryReset(apm::ams::ModuleManager &moduleManager,
                         FactoryResetResult &out) {
  out = FactoryResetResult{};
  std::vector<std::string> errors;

  removePathRecursive(apm::config::getInstalledDir(), "installed content", errors);
  removePathRecursive(apm::config::APM_BIN_DIR, "APM shim bin", errors);
  removePathRecursive(apm::config::MANUAL_PACKAGES_DIR,
                      "manual package metadata", errors);

  removePathRecursive(apm::config::getSecurityDir(), "security data", errors);
  removeFilePath(apm::config::getStatusFile(), "status database", errors);

  removeAllModules(moduleManager, errors);
  removePathRecursive(apm::config::getModulesDir(), "AMS modules", errors);
  removePathRecursive(apm::config::getListsDir(), "repository lists", errors);

  cleanupSystemApps(errors);

  if (errors.empty()) {
    out.ok = true;
    out.message = "Factory reset completed.";
  } else {
    out.ok = false;
    out.message = "Factory reset completed with issues: " + joinErrors(errors);
  }

  return out.ok;
}

bool performWipeCache(apm::ams::ModuleManager &moduleManager,
                      const WipeCacheSelection &selection,
                      WipeCacheResult &out) {
  out = WipeCacheResult{};
  if (!hasSelectedCaches(selection)) {
    out.message = "No cache targets selected.";
    return false;
  }

  std::vector<std::string> errors;

  if (selection.apmGeneral) {
    clearDirectoryContents(apm::config::getCacheDir(), "APM general cache",
                           errors);
  }

  if (selection.repoLists) {
    clearDirectoryContents(apm::config::getListsDir(), "repository lists",
                           errors);
  }

  if (selection.packageDownloads) {
    std::vector<std::string> preserveNames;
    if (!selection.signatureCache)
      preserveNames.emplace_back("sig-cache.json");
    clearDirectoryContents(apm::config::getPkgsDir(), "package download cache",
                           errors, preserveNames);
  }

  if (selection.signatureCache)
    clearSignatureCache(errors);

  if (selection.amsRuntime) {
    clearDirectoryContents(apm::config::getModuleRuntimeDir(),
                           "AMS runtime cache", errors);

    std::string rebuildErr;
    if (!moduleManager.applyEnabledModules(&rebuildErr)) {
      std::string msg =
          rebuildErr.empty()
              ? "Failed to rebuild AMS runtime after cache wipe"
              : "Failed to rebuild AMS runtime after cache wipe: " + rebuildErr;
      errors.push_back(msg);
      apm::logger::warn("wipe_cache: " + msg);
    }
  }

  const std::string targets = joinLabels(selectedCacheLabels(selection));
  if (errors.empty()) {
    out.ok = true;
    out.message = "Cleared " + targets + ".";
  } else {
    out.ok = false;
    out.message = "Cache wipe completed with issues while clearing " + targets +
                  ": " + joinErrors(errors);
  }

  return out.ok;
}

} // namespace apm::daemon
