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

} // namespace apm::daemon
