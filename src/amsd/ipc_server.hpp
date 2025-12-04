/*
 * AMSD - APM Module System Daemon
 *
 * Simple UNIX domain socket server for module IPC.
 */

/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: ipc_server.hpp
 * Purpose: Declare the AMSD UNIX domain socket server used for module IPC.
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

#pragma once

#include "request_dispatcher.hpp"

#include <atomic>
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

  void handleClient(int clientFd);
};

} // namespace apm::amsd
