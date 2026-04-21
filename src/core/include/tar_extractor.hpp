/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: tar_extractor.hpp
 * Purpose: Declare the helper for extracting compressed tar archives.
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
#include <vector>

namespace apm::tar {

struct ExtractOptions {
  std::vector<std::string> allowedAbsoluteSymlinkTargetPrefixes;
};

// Extract a tar archive (optionally compressed: .gz, .xz, .bz2, etc.)
// into the given destination directory.
//
// - tarPath: path to the tar file (control.tar.*, data.tar.*, etc.)
// - destDir: directory to extract into. Will be created if needed.
// - errorMsg: optional, filled with a human-readable error on failure.
//
// Returns true on success, false on failure.
bool extractTar(const std::string &tarPath, const std::string &destDir,
                std::string *errorMsg = nullptr);

bool extractTar(const std::string &tarPath, const std::string &destDir,
                const ExtractOptions &options,
                std::string *errorMsg = nullptr);

} // namespace apm::tar
