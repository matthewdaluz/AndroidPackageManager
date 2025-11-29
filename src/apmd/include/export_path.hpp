/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: export_path.hpp
 * Purpose: Declare helper APIs for generating PATH export scripts and sourcing
 * them for newly installed commands. Last Modified: November 18th, 2025. - 3:00
 * PM Eastern Time. Author: Matthew DaLuz - RedHead Founder
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

namespace apm::daemon::path {

// Ensure helper scripts exist and run the export script so new commands from
// /data/apm/installed/commands are immediately available on PATH.
void refreshPathEnvironment();

// Ensure the global /data/local/tmp/.apm_profile script has been sourced at
// least once since boot so that shells inheriting ENV see the updated PATH.
void ensureProfileLoaded();

// Generate apm-env.sh for emulator mode with atomic write.
// Only active when isEmulatorMode() is true.
void generateEmulatorEnv();

} // namespace apm::daemon::path
