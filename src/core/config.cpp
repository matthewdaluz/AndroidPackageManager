/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: config.cpp
 * Purpose: Runtime configuration for emulator mode path management.
 * Last Modified: 2026-03-22 13:03:10.296767976 -0400.
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

#include "config.hpp"
#include <cstdlib>
#include <string>

namespace apm::config {

namespace {
bool g_emulatorMode = false;
std::string g_emulatorRoot;
std::string g_emulatorAmsRoot;

std::string getHomeDir() {
  const char *home = ::getenv("HOME");
  if (home && *home)
    return std::string(home);
  return "/tmp"; // Fallback
}

std::string buildEmulatorPath(const char *suffix) {
  if (g_emulatorRoot.empty()) {
    g_emulatorRoot = getHomeDir() + "/APMEmulator/data/apm";
  }
  if (!suffix || !*suffix)
    return g_emulatorRoot;
  std::string result = g_emulatorRoot;
  if (result.back() != '/' && suffix[0] != '/')
    result += '/';
  result += suffix;
  return result;
}

std::string buildEmulatorAmsPath(const char *suffix) {
  if (g_emulatorAmsRoot.empty()) {
    g_emulatorAmsRoot = getHomeDir() + "/APMEmulator/ams";
  }
  if (!suffix || !*suffix)
    return g_emulatorAmsRoot;
  std::string result = g_emulatorAmsRoot;
  if (result.back() != '/' && suffix[0] != '/')
    result += '/';
  result += suffix;
  return result;
}
} // namespace

void setEmulatorMode(bool enabled) { g_emulatorMode = enabled; }

bool isEmulatorMode() { return g_emulatorMode; }

std::string getEmulatorRoot() { return buildEmulatorPath(""); }

std::string getApmRoot() {
  if (g_emulatorMode)
    return buildEmulatorPath("");
  return "/data/apm";
}

std::string getInstalledDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("installed");
  return "/data/apm/installed";
}

std::string getCommandsDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("installed/commands");
  return "/data/apm/installed/commands";
}

std::string getDependenciesDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("installed/dependencies");
  return "/data/apm/installed/dependencies";
}

std::string getTermuxInstalledDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("installed/termux/usr/.apm-installed");
  return "/data/apm/installed/termux/usr/.apm-installed";
}

std::string getCommandsPathHelper() {
  if (g_emulatorMode)
    return buildEmulatorPath("installed/commands/apm-path.sh");
  return "/data/apm/installed/commands/apm-path.sh";
}

std::string getCommandsExportScript() {
  if (g_emulatorMode)
    return buildEmulatorPath("installed/commands/export-path.sh");
  return "/data/apm/installed/commands/export-path.sh";
}

std::string getGlobalProfileFile() {
  if (g_emulatorMode)
    return buildEmulatorPath(".apm_profile");
  return "/data/local/tmp/.apm_profile";
}

std::string getGlobalProfileSourcedMark() {
  if (g_emulatorMode)
    return buildEmulatorPath(".apm_profile.sourced");
  return "/data/local/tmp/.apm_profile.sourced";
}

std::string getCacheDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("cache");
  return "/data/apm/cache";
}

std::string getListsDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("lists");
  return "/data/apm/lists";
}

std::string getPkgsDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("pkgs");
  return "/data/apm/pkgs";
}

std::string getLogsDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("logs");
  return "/data/apm/logs";
}

std::string getDebugFlagFile() {
  if (g_emulatorMode)
    return buildEmulatorPath("debug.txt");
  return "/data/apm/debug.txt";
}

std::string getManualPackagesDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("manual-packages");
  return "/data/apm/manual-packages";
}

std::string getApmBinDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("bin");
  return "/data/local/tmp/apm/bin";
}

std::string getPathDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("path");
  return "/data/local/tmp/apm/path";
}

std::string getShPathFile() {
  if (g_emulatorMode)
    return buildEmulatorPath("path/sh-path.sh");
  return "/data/local/tmp/apm/path/sh-path.sh";
}

std::string getBashPathFile() {
  if (g_emulatorMode)
    return buildEmulatorPath("path/bash-path.sh");
  return "/data/local/tmp/apm/path/bash-path.sh";
}

std::string getSandboxRoot() {
  if (g_emulatorMode)
    return buildEmulatorPath("sandbox");
  return "/data/apm/sandbox";
}

std::string getSandboxStateDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("sandbox/state");
  return "/data/apm/sandbox/state";
}

std::string getSandboxEnvDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("sandbox/env");
  return "/data/apm/sandbox/env";
}

std::string getSandboxMountsDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("sandbox/mounts");
  return "/data/apm/sandbox/mounts";
}

std::string getCommandIndexFile() {
  if (g_emulatorMode)
    return buildEmulatorPath("sandbox/state/command-index.json");
  return "/data/apm/sandbox/state/command-index.json";
}

std::string getSandboxPathEnvFile() {
  if (g_emulatorMode)
    return buildEmulatorPath("sandbox/env/apm-path.env");
  return "/data/apm/sandbox/env/apm-path.env";
}

std::string getTermuxRoot() {
  if (g_emulatorMode)
    return buildEmulatorPath("installed/termux");
  return "/data/apm/installed/termux";
}

std::string getTermuxPrefix() {
  if (g_emulatorMode)
    return buildEmulatorPath("installed/termux/usr");
  return "/data/apm/installed/termux/usr";
}

std::string getTermuxEnvFile() {
  if (g_emulatorMode)
    return buildEmulatorPath("installed/termux/env.sh");
  return "/data/apm/installed/termux/env.sh";
}

std::string getTermuxHomeDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("installed/termux/home");
  return "/data/apm/installed/termux/home";
}

std::string getTermuxTmpDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("installed/termux/tmp");
  return "/data/apm/installed/termux/tmp";
}

std::string getModulesDir() {
  if (g_emulatorMode)
    return buildEmulatorAmsPath("modules");
  return "/data/ams/modules";
}

std::string getModuleLogsDir() {
  if (g_emulatorMode)
    return buildEmulatorAmsPath("logs");
  return "/data/ams/logs";
}

std::string getModuleRuntimeDir() {
  if (g_emulatorMode)
    return buildEmulatorAmsPath(".runtime");
  return "/data/ams/.runtime";
}

std::string getModuleRuntimeUpperDir() {
  if (g_emulatorMode)
    return buildEmulatorAmsPath(".runtime/upper");
  return "/data/ams/.runtime/upper";
}

std::string getModuleRuntimeWorkDir() {
  if (g_emulatorMode)
    return buildEmulatorAmsPath(".runtime/work");
  return "/data/ams/.runtime/work";
}

std::string getModuleRuntimeBaseDir() {
  if (g_emulatorMode)
    return buildEmulatorAmsPath(".runtime/base");
  return "/data/ams/.runtime/base";
}

std::string getStatusFile() {
  if (g_emulatorMode)
    return buildEmulatorPath("status");
  return "/data/apm/status";
}

std::string getSourcesDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("sources");
  return "/data/apm/sources";
}

std::string getSourcesMain() {
  if (g_emulatorMode)
    return buildEmulatorPath("sources/sources.list");
  return "/data/apm/sources/sources.list";
}

std::string getSourcesListD() {
  if (g_emulatorMode)
    return buildEmulatorPath("sources/sources.list.d");
  return "/data/apm/sources/sources.list.d";
}

std::string getSourcesList() { return getSourcesDir(); }

std::string getIpcSocketPath() {
  if (g_emulatorMode)
    return buildEmulatorPath("apmd.socket");
  return "@apmd";
}

std::string getAmsdSocketPath() {
  if (g_emulatorMode)
    return buildEmulatorAmsPath("amsd.socket");
  return "/data/ams/amsd.sock";
}

std::string getTrustedKeysDir() {
  if (g_emulatorMode)
    return buildEmulatorPath("keys");
  return "/data/apm/keys";
}

std::string getSecurityDir() {
  if (g_emulatorMode)
    return buildEmulatorPath(".security");
  return "/data/apm/.security";
}

std::string getMasterKeyFile() {
  if (g_emulatorMode)
    return buildEmulatorPath(".security/masterkey.bin");
  return "/data/apm/.security/masterkey.bin";
}

std::string getPassPinFile() {
  if (g_emulatorMode)
    return buildEmulatorPath(".security/passpin.bin");
  return "/data/apm/.security/passpin.bin";
}

std::string getSessionFile() {
  if (g_emulatorMode)
    return buildEmulatorPath(".security/session.bin");
  return "/data/apm/.security/session.bin";
}

std::string getSecurityQaFile() {
  if (g_emulatorMode)
    return buildEmulatorPath(".security/security-questions.bin");
  return "/data/apm/.security/security-questions.bin";
}

std::string getResetLockoutFile() {
  if (g_emulatorMode)
    return buildEmulatorPath(".security/reset-lockout.txt");
  return "/data/apm/.security/reset-lockout.txt";
}

std::string getDefaultArch() {
#ifdef APM_EMULATOR_MODE
  return "x86_64";
#else
  return "arm64";
#endif
}

} // namespace apm::config
