/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: export_path.hpp
 * Purpose: Declare helper APIs for generating PATH export scripts and sourcing
 * them for newly installed commands. Last Modified: March 15th, 2026. - 10:51
 * PM EDT. Author: Matthew DaLuz - RedHead Founder
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
#include <vector>

namespace apm::daemon::path {

enum class CommandCollisionResult { Canonical, Namespaced, Skipped };

struct CommandHotloadSummary {
  bool ok = false;
  std::string triggerReason;
  std::string message;
  std::vector<std::string> activatedCommands;
  std::vector<std::string> namespacedCommands;
  std::vector<std::string> collisionWarnings;
};

// Resolve a command name for shim creation:
// - Canonical: use the command as-is.
// - Namespaced: use "<package>-<command>".
// - Skipped: do not create a shim.
CommandCollisionResult
resolve_command_collision(const std::string &packageName,
                          const std::string &commandName,
                          std::string &resolvedShimName);

// Rebuild command index + shim set and refresh PATH env file/hooks.
// Trigger should be a short reason string such as "install", "remove", etc.
bool rebuild_command_index_and_shims(const std::string &triggerReason,
                                     CommandHotloadSummary *summary = nullptr);

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
