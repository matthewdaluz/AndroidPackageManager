/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: module_manager.hpp
 * Purpose: Declare the AMS module lifecycle manager responsible for install/enable/disable
 *          and overlay management within apmd.
 * Last Modified: 2026-03-22 12:40:07.402606623 -0400.
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

#pragma once

#include "ams/module_info.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace apm::ams {

struct ModuleOperationResult {
  bool ok = false;
  std::string message;
};

struct ModuleStatusEntry {
  ModuleInfo info;
  ModuleState state;
  std::string path;
};

class ModuleManager {
public:
  ModuleManager();
  ~ModuleManager();

  bool installFromZip(const std::string &zipPath, ModuleOperationResult &out);
  bool enableModule(const std::string &name, ModuleOperationResult &out);
  bool disableModule(const std::string &name, ModuleOperationResult &out);
  bool removeModule(const std::string &name, ModuleOperationResult &out);

  // Called during daemon start to ensure overlays/scripts are applied.
  bool applyEnabledModules(std::string *errorMsg = nullptr);
  void startEnabledModules();
  bool listModules(std::vector<ModuleStatusEntry> &out,
                   std::string *errorMsg = nullptr) const;

  // Background monitoring for late-mounted partitions (e.g., vendor/product).
  void startPartitionMonitor();
  void stopPartitionMonitor();

  // Safe mode helpers (file-based counters/flags)
  static bool incrementBootCounter(const std::string &path,
                                   std::uint64_t *newValue = nullptr);
  static std::uint64_t getBootCounter(const std::string &path);
  static bool resetBootCounter(const std::string &path);
  static std::uint64_t getBootThreshold(const std::string &path,
                                        std::uint64_t defaultValue);
  static bool enterSafeMode(const std::string &path);
  static bool isSafeModeActive(const std::string &path);
  static bool clearSafeMode(const std::string &path);

  bool isPartitionMounted(const std::string &mountPoint) const;

private:
  bool loadModule(const std::string &name, ModuleInfo &info, ModuleState &state,
                  std::string *errorMsg) const;
  bool writeState(const std::string &moduleDir, ModuleState &state,
                  std::string *errorMsg) const;
  bool ensureRuntimeDirs(std::string *errorMsg) const;
  bool applyOverlayForTarget(std::size_t targetIndex,
                             const std::vector<std::string> &layers,
                             std::string *errorMsg);
  bool rebuildOverlays(std::string *errorMsg);
  bool runLifecycleScripts(const ModuleInfo &info, const std::string &moduleDir,
                           bool isStartup) const;
  bool runScript(const std::string &path, const std::string &moduleName,
                 bool background, bool requireExists = false,
                 std::string *errorMsg = nullptr) const;
  void logModuleEvent(const std::string &name, const std::string &message) const;

  bool extractZip(const std::string &zipPath, const std::string &dest,
                  std::string *errorMsg) const;
  std::string makeTempDir(const std::string &tag) const;

  std::string modulePath(const std::string &name) const;
  std::string moduleLogPath(const std::string &name) const;

  using OverlayStacks = std::map<std::string, std::vector<std::string>>;
  OverlayStacks buildOverlayStacks() const;

  void monitorPartitions();
  void stopMonitorLocked();

  std::string modulesRoot_;
  std::string logsRoot_;
  mutable std::recursive_mutex mutex_;
  std::thread monitorThread_;
  std::atomic<bool> monitorStop_{false};
  bool monitorRunning_ = false;
};

} // namespace apm::ams
