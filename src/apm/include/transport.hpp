/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: transport.hpp
 * Purpose: Declare IPC transport helpers used by the CLI to talk to daemon sockets.
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

enum class TransportMode { IPC };

// Mirror ProgressHandler definition from client headers to avoid including
// transport-specific headers here.
using ProgressHandler = std::function<void(const Response &)>;

// Detect desired transport mode (IPC-only).
TransportMode detectTransportMode();

// Dispatch request over the IPC socket transport.
bool sendRequestAuto(const Request &req, Response &resp,
                     std::string *errorMsg = nullptr,
                     ProgressHandler progressHandler = {});
bool sendRequestToSocket(const Request &req, Response &resp,
                         const std::string &socketPath,
                         std::string *errorMsg = nullptr,
                         ProgressHandler progressHandler = {});

} // namespace apm::ipc
