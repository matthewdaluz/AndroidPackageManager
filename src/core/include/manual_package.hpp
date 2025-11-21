/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: manual_package.hpp
 * Purpose: Declare manual package metadata structures plus serialization helpers.
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

#include <cstdint>
#include <string>
#include <vector>

namespace apm::manual {

struct PackageInfo {
  std::string name;
  std::string version;
  std::uint64_t installTime = 0;
  std::string prefix;
  std::vector<std::string> installedFiles;
};

// Parse a JSON string describing a package (see fastfetch-gz-package example).
bool parsePackageInfo(const std::string &json, PackageInfo &info,
                      std::string *errorMsg = nullptr);

// Serialize a PackageInfo struct into the canonical JSON representation.
std::string serializePackageInfo(const PackageInfo &info);

// Convenience helpers for reading/writing package-info JSON files.
bool readPackageInfoFile(const std::string &path, PackageInfo &info,
                         std::string *errorMsg = nullptr);
bool writePackageInfoFile(const std::string &path, const PackageInfo &info,
                          std::string *errorMsg = nullptr);

// Installed manual packages live under MANUAL_PACKAGES_DIR/<name>.json.
std::string installedInfoPath(const std::string &name);

bool loadInstalledPackage(const std::string &name, PackageInfo &info,
                          std::string *errorMsg = nullptr);
bool saveInstalledPackage(const PackageInfo &info,
                          std::string *errorMsg = nullptr);
bool removeInstalledPackage(const std::string &name,
                            std::string *errorMsg = nullptr);
bool listInstalledPackages(std::vector<PackageInfo> &out,
                           std::string *errorMsg = nullptr);
bool isInstalled(const std::string &name);

} // namespace apm::manual

