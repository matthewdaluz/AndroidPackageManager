/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: ipc_server.hpp
 * Purpose: Declare the UNIX domain socket server that accepts CLI connections for apmd.
 * Last Modified: November 18th, 2025. - 3:00 PM Eastern Time.
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

#include "ams/module_manager.hpp"

#include <string>

namespace apm::ipc {

class IpcServer {
public:
  IpcServer(const std::string &socketPath,
            apm::ams::ModuleManager &moduleManager);
  ~IpcServer();

  // Create/bind/listen on the UNIX socket.
  bool start();

  // Blocking event loop: accept + handle clients until stop() is called
  // from another thread or process exit.
  void run();

  // Signal the server to stop after the current iteration.
  void stop();

private:
  int m_listenFd;
  std::string m_socketPath;
  bool m_running;
  apm::ams::ModuleManager &m_moduleManager;

  void handleClient(int clientFd);
};

} // namespace apm::ipc
