/*
 * AMSD - APM Module System Daemon
 *
 * Simple UNIX domain socket server for module IPC.
 */

/*
 * APM - Android Package Manager
 *
 * Skidworks Open-Engineering - Software Branch
 * Copyright (C) 2027 Skidworks Open-Engineering
 *
 * File: ipc_server.hpp
 * Purpose: Declare the AMSD UNIX domain socket server used for module IPC.
 * Last Modified: November 15th, 2026. - 09:14 AM EST.
 * Author: Matthew DaLuz - Skidworks Founder
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

#include "request_dispatcher.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>

namespace apm::amsd {

class IpcServer {
public:
  IpcServer(const std::string &socketPath, RequestDispatcher &dispatcher);
  ~IpcServer();

  bool start();
  void run();
  void stop();

private:
  int listenFd_;
  std::string socketPath_;
  std::atomic<bool> running_;
  RequestDispatcher &dispatcher_;
  std::mutex dispatchMutex_;
  std::mutex clientMutex_;
  std::condition_variable clientCv_;
  std::size_t activeClients_ = 0;

  bool startClientWorker(int clientFd);
  void waitForClients();
  void handleClient(int clientFd);
};

} // namespace apm::amsd
