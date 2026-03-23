/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: status_db.hpp
 * Purpose: Declare dpkg-style status database helpers for installed packages.
 * Last Modified: 2026-03-15 11:56:16.537911560 -0400.
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
#include <unordered_map>
#include <vector>

namespace apm::status {

struct InstalledPackage {
  std::string name;
  std::string version;
  std::string architecture;
  std::string status;      // e.g. "install ok installed"
  std::string installRoot; // e.g. /data/apm/installed/nano

  std::string repoUri;       // e.g. https://deb.debian.org/debian
  std::string repoDist;      // e.g. bookworm
  std::string repoComponent; // e.g. main

  // Direct dependencies (first alternative only, like repo_index)
  std::vector<std::string> depends;

  // Whether this package was auto-installed as a dependency
  bool autoInstalled = false;

  bool termuxPackage = false;
  std::string installPrefix;
};

using InstalledDb = std::unordered_map<std::string, InstalledPackage>;

bool loadStatusFile(const std::string &path, InstalledDb &out,
                    std::string *errorMsg = nullptr);

bool writeStatusFile(const std::string &path, const InstalledDb &db,
                     std::string *errorMsg = nullptr);

bool loadStatus(InstalledDb &out, std::string *errorMsg = nullptr);
bool writeStatus(const InstalledDb &db, std::string *errorMsg = nullptr);

bool isInstalled(const std::string &name, InstalledPackage *pkgOut = nullptr,
                 std::string *errorMsg = nullptr);

bool recordInstalled(const InstalledPackage &pkg,
                     std::string *errorMsg = nullptr);

bool removeInstalled(const std::string &name, std::string *errorMsg = nullptr);

} // namespace apm::status
