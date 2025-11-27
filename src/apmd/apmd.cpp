/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: apmd.cpp
 * Purpose: Bootstrap apmd, configure logging, and run the IPC server event
 * loop. Last Modified: November 18th, 2025. - 3:00 PM Eastern Time. Author:
 * Matthew DaLuz - RedHead Founder
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
#include "export_path.hpp"
#include "ipc_server.hpp"
#include "logger.hpp"

#include <iostream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace {

bool isDataAccessible() {
  struct stat st{};
  if (stat("/data", &st) != 0) {
    return false;
  }
  return access("/data", R_OK | W_OK | X_OK) == 0;
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
// IPC server loop.
int runDaemon(const std::string &socketPath) {
  waitForDataReady();

  apm::logger::setLogFile("/data/apm/logs/apmd.log");
  apm::logger::setMinLogLevel(apm::logger::Level::Info);

  apm::logger::info("apmd starting, socket: " + socketPath);

  apm::daemon::path::ensureProfileLoaded();

  apm::ams::ModuleManager moduleManager;
  std::string moduleErr;
  if (!moduleManager.applyEnabledModules(&moduleErr)) {
    apm::logger::warn("runDaemon: module initialization failed: " + moduleErr);
  } else {
    moduleManager.startEnabledModules();
  }

  apm::ipc::IpcServer server(socketPath, moduleManager);
  if (!server.start()) {
    apm::logger::error("runDaemon: failed to start IPC server");
    return 1;
  }

  server.run();
  apm::logger::info("apmd exiting");
  return 0;
}

} // namespace apm::daemon

// Thin wrapper around runDaemon that parses --socket/--help flags.
int main(int argc, char **argv) {
  std::string socketPath = apm::daemon::DEFAULT_SOCKET_PATH;

  // Simple arg parser: apmd [--socket /path/to.sock]
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--socket" && i + 1 < argc) {
      socketPath = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "apmd - APM daemon\n"
                << "Usage: apmd [--socket <path>]\n";
      return 0;
    }
  }

  return apm::daemon::runDaemon(socketPath);
}
