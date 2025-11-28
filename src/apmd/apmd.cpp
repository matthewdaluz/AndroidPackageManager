/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: apmd.cpp
 * Purpose: Bootstrap apmd, configure logging, and run the Binder-backed
 * service loop.
 * Last Modified: November 28th, 2025. - 8:59 AM Eastern Time. Author: Matthew
 * DaLuz - RedHead Founder
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
#include "binder_service.hpp"
#include "export_path.hpp"
#include "logger.hpp"
#include "security_manager.hpp"

#include <atomic>
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

// Keep PATH/profile helpers fresh in the background so Termux commands stay
// reachable even if shells clear ENV. Runs until the daemon shuts down.
static void startPathMaintenance(std::atomic<bool> &runFlag,
                                 std::thread &worker) {
  runFlag.store(true);
  worker = std::thread([&runFlag]() {
    using namespace std::chrono_literals;
    while (runFlag.load()) {
      apm::daemon::path::refreshPathEnvironment();
      for (int i = 0; i < 5 && runFlag.load(); ++i) {
        std::this_thread::sleep_for(1s);
      }
    }
  });
}

// Configure logging, ensure the PATH/profile helper is loaded, and launch the
// Binder service loop.
int runDaemon(const std::string &serviceName) {
  waitForDataReady();

  apm::logger::setLogFile("/data/apm/logs/apmd.log");
  apm::logger::setMinLogLevel(apm::logger::Level::Info);

  apm::logger::info("apmd starting, service: " + serviceName);

  apm::daemon::path::ensureProfileLoaded();

  std::atomic<bool> pathLoopRun{false};
  std::thread pathLoopThread;
  startPathMaintenance(pathLoopRun, pathLoopThread);

  apm::ams::ModuleManager moduleManager;
  std::string moduleErr;
  if (!moduleManager.applyEnabledModules(&moduleErr)) {
    apm::logger::warn("runDaemon: module initialization failed: " + moduleErr);
  } else {
    moduleManager.startEnabledModules();
  }

  apm::daemon::SecurityManager securityManager;
  apm::ipc::BinderService binderService(serviceName, moduleManager,
                                        securityManager);
  std::string binderErr;
  if (binderService.start(&binderErr)) {
    binderService.joinThreadPool();
    pathLoopRun.store(false);
    if (pathLoopThread.joinable()) {
      pathLoopThread.join();
    }
    apm::logger::info("apmd exiting Binder thread pool");
    return 0;
  }

  apm::logger::error("runDaemon: failed to start Binder service: " +
                     (binderErr.empty() ? "unknown error" : binderErr));
  pathLoopRun.store(false);
  if (pathLoopThread.joinable()) {
    pathLoopThread.join();
  }
  return 1;
}

} // namespace apm::daemon

// Thin wrapper around runDaemon that parses --socket/--help flags.
int main(int argc, char **argv) {
  std::string serviceName = apm::daemon::DEFAULT_SERVICE_NAME;

  // Simple arg parser: apmd [--service name]
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if ((arg == "--service" || arg == "--svc") && i + 1 < argc) {
      serviceName = argv[++i];
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "apmd - APM daemon\n"
                << "Usage: apmd [--service <name>]\n";
      return 0;
    }
  }

  return apm::daemon::runDaemon(serviceName);
}
