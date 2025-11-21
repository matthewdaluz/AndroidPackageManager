/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: apmd.hpp
 * Purpose: Declare the apmd entry point along with the default socket path constant.
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

#include <string>

namespace apm::daemon {

// Default UNIX socket path for apmd.
// On Android this should be /data/apm/apmd.sock (adjustable if needed).
inline constexpr const char *DEFAULT_SOCKET_PATH = "/data/apm/apmd.sock";

// Main daemon runner.
// - socketPath: path to the UNIX domain socket to bind to.
// Returns exit code (0 = OK).
int runDaemon(const std::string &socketPath);

} // namespace apm::daemon
