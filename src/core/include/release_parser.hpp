/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: release_parser.hpp
 * Purpose: Declare helpers for parsing Release files and looking up SHA256 entries.
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

#include <cstddef>
#include <string>
#include <vector>

namespace apm::repo {

struct ReleaseChecksumEntry {
  std::string hash; // hex SHA256
  std::string name; // path like "dists/bookworm/main/binary-arm64/Packages"
  std::size_t size = 0;
};

struct ReleaseInfo {
  std::vector<ReleaseChecksumEntry> sha256;
};

// Parse a Debian-style Release file and extract the SHA256 section.
// Returns true on success; false on failure (e.g. no SHA256 section).
bool parseReleaseFile(const std::string &path, ReleaseInfo &out,
                      std::string *errorMsg = nullptr);

// Look up SHA256 hash for a given path (as listed in Release's SHA256 section).
// Returns true and fills outHash on success; false if not found.
bool findSha256ForPath(const ReleaseInfo &info, const std::string &name,
                       std::string &outHash);

} // namespace apm::repo
