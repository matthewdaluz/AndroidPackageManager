/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: amsd.cpp
 * Purpose: Implement the AMS daemon entrypoint, including boot counter handling,
 *          safe mode detection, overlay application, and IPC server startup.
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
#include "ipc_server.hpp"
#include "logger.hpp"
#include "request_dispatcher.hpp"
#include "security_manager.hpp"

#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

apm::amsd::IpcServer *g_server = nullptr;

bool isDataAccessible() {
  struct stat st{};
  if (stat("/data", &st) != 0) {
    return false;
  }
  return access("/data", R_OK | W_OK | X_OK) == 0;
}

void waitForDataReady() {
  using namespace std::chrono_literals;
  while (!isDataAccessible()) {
    std::cerr << "amsd: waiting for /data to become available..." << std::endl;
    std::this_thread::sleep_for(5s);
  }
}

void handleSignal(int) {
  if (g_server)
    g_server->stop();
}

std::string buildLogFilePath() {
  // Place amsd.log next to the module log directory (../logs/amsd.log)
  std::string logDir =
      apm::fs::joinPath(apm::config::getModuleLogsDir(), "..");
  apm::fs::createDirs(logDir);
  return apm::fs::joinPath(logDir, "amsd.log");
}

} // namespace

int main(int argc, char **argv) {
  std::string socketPathOverride;
  bool debugMode = false;
  bool emulatorMode = false;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--socket" && i + 1 < argc) {
      socketPathOverride = argv[++i];
    } else if (arg == "--debug" || arg == "-d") {
      debugMode = true;
    } else if (arg == "--emulator") {
#ifndef APM_EMULATOR_MODE
      std::cerr << "Error: This binary was not compiled with emulator support.\n";
      return 1;
#else
      emulatorMode = true;
#endif
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "amsd - APM Module System daemon\n"
                << "Usage: amsd [--socket <path>] [--debug]";
#ifdef APM_EMULATOR_MODE
      std::cout << " [--emulator]";
#endif
      std::cout << "\n";
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
    std::cerr << "Error: Emulator mode requested but not compiled in.\n";
    return 1;
  }
#endif

  apm::config::setEmulatorMode(emulatorMode);

  const std::string socketPath = socketPathOverride.empty()
                                     ? apm::config::getAmsdSocketPath()
                                     : socketPathOverride;

  if (!emulatorMode)
    waitForDataReady();

  apm::logger::setLogFile(buildLogFilePath());
  apm::logger::setMinLogLevel(debugMode ? apm::logger::Level::Debug
                                        : apm::logger::Level::Info);

  apm::logger::info("amsd starting");
  if (debugMode)
    apm::logger::info("amsd: DEBUG mode enabled");
  if (emulatorMode)
    apm::logger::info("amsd: EMULATOR mode enabled");

  apm::ams::ModuleManager moduleManager;
  apm::amsd::SecurityManager securityManager;

  const std::string counterPath = "/ams/.amsd_boot_counter";
  const std::string thresholdPath = "/ams/.amsd_safe_mode_threshold";
  const std::string safeModeFlag = "/ams/.amsd_safe_mode";

  std::uint64_t threshold =
      apm::ams::ModuleManager::getBootThreshold(thresholdPath, 3);
  std::uint64_t bootCount =
      apm::ams::ModuleManager::getBootCounter(counterPath);
  bool safeMode = (bootCount >= threshold) ||
                  apm::ams::ModuleManager::isSafeModeActive(safeModeFlag);

  apm::ams::ModuleManager::incrementBootCounter(counterPath, &bootCount);

  if (safeMode) {
    apm::logger::warn("amsd: safe mode active (boot counter=" +
                      std::to_string(bootCount) +
                      ", threshold=" + std::to_string(threshold) + ")");
    apm::ams::ModuleManager::enterSafeMode(safeModeFlag);
  }

  bool overlaysOk = false;
  if (!safeMode) {
    std::string overlayErr;
    overlaysOk = moduleManager.applyEnabledModules(&overlayErr);
    if (!overlaysOk && !overlayErr.empty()) {
      apm::logger::error("amsd: failed to apply overlays: " + overlayErr);
    }

    moduleManager.startPartitionMonitor();

    if (overlaysOk) {
      moduleManager.startEnabledModules();
    }
  } else {
    apm::logger::info("amsd: skipping module loading due to safe mode");
  }

  apm::amsd::RequestDispatcher dispatcher(moduleManager, securityManager);
  apm::amsd::IpcServer server(socketPath, dispatcher);
  g_server = &server;
  std::signal(SIGTERM, handleSignal);
  std::signal(SIGINT, handleSignal);

  if (!server.start()) {
    apm::logger::error("amsd: failed to start IPC server");
    moduleManager.stopPartitionMonitor();
    return 1;
  }

  if (overlaysOk && !safeMode) {
    apm::ams::ModuleManager::resetBootCounter(counterPath);
    apm::ams::ModuleManager::clearSafeMode(safeModeFlag);
  }

  ::system("setprop amsd.ready 1");

  server.run();
  moduleManager.stopPartitionMonitor();
  g_server = nullptr;

  return 0;
}
