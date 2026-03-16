/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: apmd.cpp
 * Purpose: Bootstrap apmd, configure logging, and run the IPC-backed service loop.
 * Last Modified: March 15th, 2026. - 10:51 PM EDT.
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
#include "process_lock.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

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

  std::string logFile = emulatorMode
                            ? apm::config::getLogsDir() + "/apmd-emulator.log"
                            : "/data/apm/logs/apmd.log";
  apm::logger::setLogFile(logFile);
  apm::logger::setMinLogLevel(debugMode ? apm::logger::Level::Debug
                                        : apm::logger::Level::Info);

  apm::logger::info("apmd starting (IPC transport)");
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
  std::string moduleErr;
  if (!moduleManager.applyEnabledModules(&moduleErr)) {
    apm::logger::warn("runDaemon: module initialization failed: " + moduleErr);
  } else {
    moduleManager.startEnabledModules();
  }

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
