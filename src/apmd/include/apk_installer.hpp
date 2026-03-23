/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: apk_installer.hpp
 * Purpose: Declare helpers for installing and uninstalling Android APKs via the daemon.
 * Last Modified: 2026-03-16 16:20:25.942817484 -0400.
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

// src/core/include/apk_installer.hpp
#pragma once

#include <string>

namespace apm::apk {

struct ApkInstallOptions {
  bool installAsSystem = false;
};

struct ApkInstallResult {
  bool ok = false;
  std::string message;
};

struct ApkUninstallResult {
  bool ok = false;
  std::string message;
};

// Install an APK from a local .apk file.
// If opts.installAsSystem == true, we stage the APK in the AMS overlay module
// path under /data/ams/modules.
bool installApk(const std::string &apkPath, const ApkInstallOptions &opts,
                ApkInstallResult &result);

// Uninstall an APK by its package name.
//
// For normal apps:    pm uninstall <pkg>
// For system apps:    fallback to pm uninstall --user 0 <pkg>
// Additionally: best-effort cleanup of AMS/legacy overlay directories that match.
bool uninstallApk(const std::string &packageName, ApkUninstallResult &result);

} // namespace apm::apk
