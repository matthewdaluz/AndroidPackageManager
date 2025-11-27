/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: factory_reset.cpp
 * Purpose: Implement daemon-side factory reset handling that wipes installed
 * content, credentials, modules, and system app overlays.
 * Last Modified: November 27th, 2025. - 11:30 AM Eastern Time.
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

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace {

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
      std::string cmd = "pm uninstall --user 0 " + dir + " >/dev/null 2>&1";
      int rc = ::system(cmd.c_str());
      if (rc == 0) {
        apm::logger::info("factory_reset: uninstalled system app package " +
                          dir);
      } else {
        std::string msg = "Failed to uninstall system app package " + dir +
                          " (pm uninstall --user 0)";
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

  removePathRecursive(apm::config::INSTALLED_DIR, "installed content", errors);
  removePathRecursive(apm::config::APM_BIN_DIR, "APM shim bin", errors);
  removePathRecursive(apm::config::MANUAL_PACKAGES_DIR,
                      "manual package metadata", errors);

  removePathRecursive(apm::config::SECURITY_DIR, "security data", errors);
  removeFilePath(apm::config::STATUS_FILE, "status database", errors);

  removeAllModules(moduleManager, errors);
  removePathRecursive(apm::config::MODULES_DIR, "AMS modules", errors);
  removePathRecursive(apm::config::LISTS_DIR, "repository lists", errors);

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
