/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: binder_client.hpp
 * Purpose: Declare the Binder client used by the CLI to talk to apmd.
 * Last Modified: 2026-03-15 11:56:16.536347864 -0400.
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

/*
 * NOTICE: Binder transport deprecated
 * ----------------------------------
 * Binder is no longer used by default. IPC over UNIX socket is the
 * only active transport. This header remains for reference so future
 * contributors can re-enable Binder if desired.
 */
#include "protocol.hpp"

#include <functional>
#include <string>

namespace apm::ipc {

using ProgressHandler = std::function<void(const Response &)>;

// Send a Request to apmd over Binder and receive a Response directly using
// the Binder transport. Prefer using transport::sendRequestAuto() instead of
// calling this directly unless you explicitly need Binder-only behavior.
//
// Returns true on success, false if connection failed / parse error / etc.
bool sendRequestBinder(const Request &req, Response &resp,
                       const std::string &serviceName,
                       std::string *errorMsg = nullptr,
                       ProgressHandler progressHandler = {});

} // namespace apm::ipc
