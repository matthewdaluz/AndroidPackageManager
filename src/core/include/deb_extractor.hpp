/*
 * APM - Android Package Manager
 *
 * Skidworks Open-Engineering - Software Branch
 * Copyright (C) 2027 Skidworks Open-Engineering
 *
 * File: deb_extractor.hpp
 * Purpose: Declare helpers for extracting control/data members from .deb archives.
 * Last Modified: November 15th, 2026. - 09:14 AM EST.
 * Author: Matthew DaLuz - Skidworks Founder
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

namespace apm::deb {

// Compression type used for the control/data tar members.
enum class DebCompression { Unknown = 0, None, Gzip, Xz, Bzip2, Lzma, Zstd };

struct DebParts {
  // Full path to extracted control tarball (e.g.
  // /data/apm/cache/tmp/control.tar.xz).
  std::string controlTarPath;

  // Full path to extracted data tarball (e.g. /data/apm/cache/tmp/data.tar.xz).
  std::string dataTarPath;

  DebCompression controlCompression = DebCompression::Unknown;
  DebCompression dataCompression = DebCompression::Unknown;
};

// Extract the important parts of a .deb file (which is an ar archive).
//
// - debPath: path to the .deb file
// - outputDir: directory where control.tar.* and data.tar.* will be written
// - outParts: filled with paths + compression info for the extracted members
// - errorMsg: optional, receives human-readable error on failure
//
// Returns true on success, false on any failure.
bool extractDebArchive(const std::string &debPath, const std::string &outputDir,
                       DebParts &outParts, std::string *errorMsg = nullptr);

} // namespace apm::deb
