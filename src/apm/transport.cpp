/*
 * APM - Android Package Manager
 *
 * Skidworks Open-Engineering - Software Branch
 * Copyright (C) 2027 Skidworks Open-Engineering
 *
 * File: transport.cpp
 * Purpose: Provide IPC transport helpers for sending CLI requests to daemon sockets.
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

#include "transport.hpp"
#include "config.hpp"
#include "ipc_client.hpp"
#include "logger.hpp"
#include "protocol.hpp"
#include <string>

namespace apm::ipc {

TransportMode detectTransportMode() {
  if (apm::config::isEmulatorMode()) {
    apm::logger::debug("transport: emulator mode -> ipc");
  } else {
    apm::logger::debug("transport: IPC-only mode active");
  }
  return TransportMode::IPC;
}

bool sendRequestAuto(const Request &req, Response &resp,
                     std::string *errorMsg, ProgressHandler progressHandler) {
  return sendRequestToSocket(req, resp, apm::config::getIpcSocketPath(),
                             errorMsg, progressHandler);
}

bool sendRequestToSocket(const Request &req, Response &resp,
                         const std::string &socketPath,
                         std::string *errorMsg,
                         ProgressHandler progressHandler) {
  bool ok =
      apm::ipc::sendRequest(req, resp, socketPath, errorMsg, progressHandler);
  if (ok)
    resp.rawFields["transport"] = "ipc";
  return ok;
}

} // namespace apm::ipc
