/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: apmd.hpp
 * Purpose: Declare the apmd entry point along with the default Binder service
 * name constant.
 * Last Modified: November 28th, 2025. - 8:59 AM Eastern Time.
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

#include "binder_defs.hpp"
#include <string>

namespace apm::daemon {

// Default Binder service name for apmd.
inline constexpr const char *DEFAULT_SERVICE_NAME = apm::binder::SERVICE_NAME;

// Main daemon runner.
// - serviceName: Binder service instance name (Android).
// - debugMode: Enable verbose debug logging.
// - emulatorMode: Run in x86_64 emulator mode (requires APM_EMULATOR_MODE
// compile flag).
// - socketPath: IPC socket path for fallback transport.
// Returns exit code (0 = OK).
int runDaemon(const std::string &serviceName, bool debugMode = false,
              bool emulatorMode = false, const std::string &socketPath = "");

} // namespace apm::daemon
