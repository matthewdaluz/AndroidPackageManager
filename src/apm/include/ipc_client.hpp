/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: ipc_client.hpp
 * Purpose: Declare the UNIX socket IPC client used by the CLI to talk to apmd.
 * Last Modified: 2026-03-15 11:56:16.536580531 -0400.
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

#include "protocol.hpp"
#include <functional>
#include <string>

namespace apm::ipc {

using ProgressHandler = std::function<void(const Response &)>;

// Send a Request to apmd over a UNIX socket and receive a Response.
//
// - req:      Request to send
// - resp:     Filled with parsed response
// - socketPath: UNIX domain socket endpoint (e.g. @apmd or /tmp/apmd.sock)
// - errorMsg: optional, filled on failure
//
// Returns true on success, false if connection failed / parse error / etc.
bool sendRequest(const Request &req, Response &resp,
                 const std::string &socketPath,
                 std::string *errorMsg = nullptr,
                 ProgressHandler progressHandler = {});

} // namespace apm::ipc
