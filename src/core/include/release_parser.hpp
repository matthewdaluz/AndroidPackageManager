/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: release_parser.hpp
 * Purpose: Declare helpers for parsing Release files and looking up checksum
 * entries. Last Modified: 2026-03-15 11:56:16.537911560 -0400. Author:
 * Matthew DaLuz - RedHead Founder
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
  std::string hash; // hex digest (e.g., SHA256 or MD5)
  std::string name; // path like "dists/bookworm/main/binary-arm64/Packages"
  std::size_t size = 0;
};

struct ReleaseInfo {
  std::vector<ReleaseChecksumEntry> sha256;
  std::vector<ReleaseChecksumEntry> md5;
};

// Parse a Debian-style Release file and extract SHA256/MD5 sections.
// Returns true on success; false on failure (e.g. no checksum sections).
bool parseReleaseFile(const std::string &path, ReleaseInfo &out,
                      std::string *errorMsg = nullptr);

// Parse a Debian-style Release content (already loaded into memory) and
// extract SHA256/MD5 sections. Returns true if at least one section found.
bool parseReleaseText(const std::string &text, ReleaseInfo &out,
                      std::string *errorMsg = nullptr);

// Look up SHA256 hash for a given path (as listed in Release's SHA256 section).
// Returns true and fills outHash on success; false if not found.
bool findSha256ForPath(const ReleaseInfo &info, const std::string &name,
                       std::string &outHash);

// Look up MD5 hash for a given path (as listed in Release's MD5Sum section).
// Returns true and fills outHash on success; false if not found.
bool findMd5ForPath(const ReleaseInfo &info, const std::string &name,
                    std::string &outHash);

} // namespace apm::repo
