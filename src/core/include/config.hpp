/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: config.hpp
 * Purpose: Declare filesystem layout constants used by both CLI and daemon.
 * Last Modified: November 23rd, 2025. - 12:06 PM Eastern Time.
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

namespace apm::config {

// Base root for all APM data on Android data partition.
inline constexpr const char *APM_ROOT = "/data/apm";

inline constexpr const char *INSTALLED_DIR = "/data/apm/installed";
inline constexpr const char *COMMANDS_DIR = "/data/apm/installed/commands";
inline constexpr const char *DEPENDENCIES_DIR =
    "/data/apm/installed/dependencies";
inline constexpr const char *TERMUX_INSTALLED_DIR = "/data/apm/installed/termux";
inline constexpr const char *COMMANDS_PATH_HELPER =
    "/data/apm/installed/commands/apm-path.sh";
inline constexpr const char *COMMANDS_EXPORT_SCRIPT =
    "/data/apm/installed/commands/export-path.sh";
inline constexpr const char *GLOBAL_PROFILE_FILE =
    "/data/local/tmp/.apm_profile";
inline constexpr const char *GLOBAL_PROFILE_SOURCED_MARK =
    "/data/local/tmp/.apm_profile.sourced";
inline constexpr const char *CACHE_DIR = "/data/apm/cache";
inline constexpr const char *LISTS_DIR = "/data/apm/lists";
inline constexpr const char *PKGS_DIR = "/data/apm/pkgs";
inline constexpr const char *LOGS_DIR = "/data/apm/logs";
inline constexpr const char *MANUAL_PACKAGES_DIR = "/data/apm/manual-packages";
inline constexpr const char *APM_BIN_DIR = "/data/apm/bin";
inline constexpr const char *TERMUX_ROOT = "/data/apm/termux";
inline constexpr const char *TERMUX_PREFIX = "/data/apm/termux/usr";
inline constexpr const char *TERMUX_ENV_FILE = "/data/apm/termux/env.sh";
inline constexpr const char *TERMUX_HOME_DIR = "/data/apm/termux/home";
inline constexpr const char *TERMUX_TMP_DIR = "/data/apm/termux/tmp";

inline constexpr const char *MODULES_DIR = "/data/apm/modules";
inline constexpr const char *MODULE_LOGS_DIR = "/data/apm/logs/modules";
inline constexpr const char *MODULE_RUNTIME_DIR = "/data/apm/modules/.runtime";
inline constexpr const char *MODULE_RUNTIME_UPPER_DIR =
    "/data/apm/modules/.runtime/upper";
inline constexpr const char *MODULE_RUNTIME_WORK_DIR =
    "/data/apm/modules/.runtime/work";
inline constexpr const char *MODULE_RUNTIME_BASE_DIR =
    "/data/apm/modules/.runtime/base";

// Status DB (dpkg-style)
inline constexpr const char *STATUS_FILE = "/data/apm/status";

// Sources layout:
//
//   /data/apm/sources/
//     ├── sources.list
//     └── sources.list.d/*.list
//
inline constexpr const char *SOURCES_DIR = "/data/apm/sources";
inline constexpr const char *SOURCES_MAIN = "/data/apm/sources/sources.list";
inline constexpr const char *SOURCES_LIST_D =
    "/data/apm/sources/sources.list.d";

// For repo_index, this is treated as the “sources root” (dir).
inline constexpr const char *SOURCES_LIST = SOURCES_DIR;

// apmd UNIX socket
inline constexpr const char *SOCKET_PATH = "/data/apm/apmd.sock";

// Default architecture for Debian-style repos
inline constexpr const char *DEFAULT_ARCH = "arm64";

// Directory containing trusted GPG keyring files (.gpg)
inline constexpr const char *TRUSTED_KEYS_DIR = "/data/apm/keys";

inline constexpr const char *SECURITY_DIR = "/data/apm/.security";
inline constexpr const char *MASTER_KEY_FILE = "/data/apm/.security/masterkey.bin";
inline constexpr const char *PASS_PIN_FILE = "/data/apm/.security/passpin.bin";
inline constexpr const char *SESSION_FILE = "/data/apm/.security/session.bin";
inline constexpr const char *PASS_KEY_ALIAS = "apm_passkey";

} // namespace apm::config
