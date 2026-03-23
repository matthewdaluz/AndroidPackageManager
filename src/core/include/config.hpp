/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: config.hpp
 * Purpose: Declare filesystem layout constants used by both CLI and daemon.
 * Last Modified: 2026-03-22 13:03:04.992683537 -0400.
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

namespace apm::config {

// ============================================================================
// Emulator Mode Control
// ============================================================================

// Enable or disable emulator mode at runtime (must be called before accessing
// paths)
void setEmulatorMode(bool enabled);

// Check if emulator mode is currently active
bool isEmulatorMode();

// Get the emulator root directory ($HOME/APMEmulator/data/apm)
std::string getEmulatorRoot();

// ============================================================================
// Runtime Path Getters (emulator-aware)
// ============================================================================

std::string getApmRoot();
std::string getInstalledDir();
std::string getCommandsDir();
std::string getDependenciesDir();
std::string getTermuxInstalledDir();
std::string getCommandsPathHelper();
std::string getCommandsExportScript();
std::string getGlobalProfileFile();
std::string getGlobalProfileSourcedMark();
std::string getCacheDir();
std::string getListsDir();
std::string getPkgsDir();
std::string getLogsDir();
std::string getDebugFlagFile();
std::string getManualPackagesDir();
std::string getApmBinDir();
std::string getPathDir();
std::string getShPathFile();
std::string getBashPathFile();
std::string getSandboxRoot();
std::string getSandboxStateDir();
std::string getSandboxEnvDir();
std::string getSandboxMountsDir();
std::string getCommandIndexFile();
std::string getSandboxPathEnvFile();
std::string getTermuxRoot();
std::string getTermuxPrefix();
std::string getTermuxEnvFile();
std::string getTermuxHomeDir();
std::string getTermuxTmpDir();
std::string getModulesDir();
std::string getModuleLogsDir();
std::string getModuleRuntimeDir();
std::string getModuleRuntimeUpperDir();
std::string getModuleRuntimeWorkDir();
std::string getModuleRuntimeBaseDir();
std::string getStatusFile();
std::string getSourcesDir();
std::string getSourcesMain();
std::string getSourcesListD();
std::string getSourcesList();
std::string getIpcSocketPath();
std::string getAmsdSocketPath();
std::string getTrustedKeysDir();
std::string getSecurityDir();
std::string getMasterKeyFile();
std::string getPassPinFile();
std::string getSessionFile();
std::string getSecurityQaFile();
std::string getResetLockoutFile();
std::string getDefaultArch();

// ============================================================================
// Backward Compatibility Constants (use getter functions in new code)
// ============================================================================

// Base root for all APM data on Android data partition.
inline constexpr const char *APM_ROOT = "/data/apm";

inline constexpr const char *INSTALLED_DIR = "/data/apm/installed";
inline constexpr const char *COMMANDS_DIR = "/data/apm/installed/commands";
inline constexpr const char *DEPENDENCIES_DIR =
    "/data/apm/installed/dependencies";
inline constexpr const char *TERMUX_INSTALLED_DIR =
    "/data/apm/installed/termux/usr/.apm-installed";
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
inline constexpr const char *PATH_DIR = "/data/apm/path";
inline constexpr const char *SH_PATH_FILE = "/data/apm/path/sh-path.sh";
inline constexpr const char *BASH_PATH_FILE = "/data/apm/path/bash-path.sh";
inline constexpr const char *SANDBOX_ROOT = "/data/apm/sandbox";
inline constexpr const char *SANDBOX_STATE_DIR = "/data/apm/sandbox/state";
inline constexpr const char *SANDBOX_ENV_DIR = "/data/apm/sandbox/env";
inline constexpr const char *SANDBOX_MOUNTS_DIR = "/data/apm/sandbox/mounts";
inline constexpr const char *COMMAND_INDEX_FILE =
    "/data/apm/sandbox/state/command-index.json";
inline constexpr const char *SANDBOX_PATH_ENV_FILE =
    "/data/apm/sandbox/env/apm-path.env";
inline constexpr const char *TERMUX_ROOT = "/data/apm/installed/termux";
inline constexpr const char *TERMUX_PREFIX = "/data/apm/installed/termux/usr";
inline constexpr const char *TERMUX_ENV_FILE =
    "/data/apm/installed/termux/env.sh";
inline constexpr const char *TERMUX_HOME_DIR =
    "/data/apm/installed/termux/home";
inline constexpr const char *TERMUX_TMP_DIR = "/data/apm/installed/termux/tmp";

inline constexpr const char *MODULES_DIR = "/data/ams/modules";
inline constexpr const char *MODULE_LOGS_DIR = "/data/ams/logs";
inline constexpr const char *MODULE_RUNTIME_DIR = "/data/ams/.runtime";
inline constexpr const char *MODULE_RUNTIME_UPPER_DIR =
    "/data/ams/.runtime/upper";
inline constexpr const char *MODULE_RUNTIME_WORK_DIR =
    "/data/ams/.runtime/work";
inline constexpr const char *MODULE_RUNTIME_BASE_DIR =
    "/data/ams/.runtime/base";

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

// IPC (UNIX domain) socket path used when running as Magisk-style system-wide
// (/data/apm/bin). The daemon binds here and the CLI connects here.
inline constexpr const char *IPC_SOCKET_PATH = "/data/apm/apmd.sock";

// Default architecture for Debian-style repos
inline constexpr const char *DEFAULT_ARCH = "arm64";

// Directory containing trusted GPG keyring files (.gpg)
inline constexpr const char *TRUSTED_KEYS_DIR = "/data/apm/keys";

inline constexpr const char *SECURITY_DIR = "/data/apm/.security";
inline constexpr const char *MASTER_KEY_FILE =
    "/data/apm/.security/masterkey.bin";
inline constexpr const char *PASS_PIN_FILE = "/data/apm/.security/passpin.bin";
inline constexpr const char *SESSION_FILE = "/data/apm/.security/session.bin";
inline constexpr const char *SECURITY_QA_FILE =
    "/data/apm/.security/security-questions.bin";
inline constexpr const char *RESET_LOCKOUT_FILE =
    "/data/apm/.security/reset-lockout.txt";
inline constexpr const char *PASS_KEY_ALIAS = "apm_passkey";

} // namespace apm::config
