/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: deb_extractor.cpp
 * Purpose: Implement .deb archive parsing and control/data extraction.
 * Last Modified: November 25th, 2025. - 11:35 AM Eastern Time.
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

#include "deb_extractor.hpp"
#include "fs.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace apm::deb {

// -------------------------------------------------------------
// Helpers
// -------------------------------------------------------------

static bool startsWith(const std::string &s, const std::string &prefix) {
  if (s.size() < prefix.size())
    return false;
  return std::equal(prefix.begin(), prefix.end(), s.begin());
}

static bool endsWith(const std::string &s, const std::string &suffix) {
  if (s.size() < suffix.size())
    return false;
  return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
}

static std::string trimRightSpaces(const std::string &s) {
  std::string out = s;
  while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) {
    out.pop_back();
  }
  return out;
}

static DebCompression detectCompressionFromName(const std::string &name) {
  if (endsWith(name, ".gz"))
    return DebCompression::Gzip;
  if (endsWith(name, ".xz"))
    return DebCompression::Xz;
  if (endsWith(name, ".bz2"))
    return DebCompression::Bzip2;
  // If no known extension, assume "none" (rare for .deb, but possible).
  return DebCompression::None;
}

// -------------------------------------------------------------
// Core extraction
// -------------------------------------------------------------

// Parse an ar-formatted .deb, extract control.tar.* and data.tar.*, and track
// the compression type for each member.
bool extractDebArchive(const std::string &debPath, const std::string &outputDir,
                       DebParts &outParts, std::string *errorMsg) {
  outParts = DebParts{}; // reset

  if (!apm::fs::pathExists(debPath)) {
    if (errorMsg)
      *errorMsg = "Deb file does not exist: " + debPath;
    apm::logger::error("extractDebArchive: file not found: " + debPath);
    return false;
  }

  // Make sure output dir exists
  if (!apm::fs::createDirs(outputDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create output directory: " + outputDir;
    apm::logger::error("extractDebArchive: cannot create output dir: " +
                       outputDir);
    return false;
  }

  std::ifstream in(debPath, std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    if (errorMsg)
      *errorMsg = "Failed to open deb file: " + debPath;
    apm::logger::error("extractDebArchive: cannot open: " + debPath);
    return false;
  }

  // Verify global ar magic: "!<arch>\n"
  char magic[8] = {};
  in.read(magic, 8);
  if (!in || std::memcmp(magic, "!<arch>\n", 8) != 0) {
    if (errorMsg)
      *errorMsg = "Invalid .deb: missing ar magic header";
    apm::logger::error("extractDebArchive: invalid ar magic");
    return false;
  }

  // Each file entry has a 60-byte header.
  const std::size_t AR_HEADER_SIZE = 60;
  char header[AR_HEADER_SIZE];

  // We'll keep reading headers until EOF.
  while (true) {
    // Peek next byte; if EOF, break cleanly.
    int c = in.peek();
    if (c == EOF) {
      break;
    }

    in.read(header, AR_HEADER_SIZE);
    if (!in) {
      // If we can't read a full header but not at EOF,
      // it's a malformed archive.
      if (!in.eof()) {
        if (errorMsg)
          *errorMsg = "Truncated ar header in .deb";
        apm::logger::error("extractDebArchive: truncated ar header");
        return false;
      }
      break;
    }

    // ar header layout:
    // 0  - 15: file identifier
    // 16 - 27: file modification timestamp
    // 28 - 33: owner id
    // 34 - 39: group id
    // 40 - 47: file mode
    // 48 - 57: file size in bytes
    // 58 - 59: magic (0x60 0x0a) = "`\n"

    // Check the header magic
    if (header[58] != '`' || header[59] != '\n') {
      if (errorMsg)
        *errorMsg = "Invalid ar file member magic in .deb";
      apm::logger::error("extractDebArchive: invalid member magic");
      return false;
    }

    // Extract and clean up file name (16 bytes, space-padded)
    std::string name(header, header + 16);
    name = trimRightSpaces(name);

    // Some ar implementations append a '/' to the name
    if (!name.empty() && name.back() == '/') {
      name.pop_back();
    }

    // Extract size (10 bytes, space-padded decimal)
    std::string sizeStr(header + 48, header + 58);
    sizeStr = trimRightSpaces(sizeStr);
    std::size_t memberSize = 0;
    errno = 0;
    char *end = nullptr;
    unsigned long parsed = std::strtoul(sizeStr.c_str(), &end, 10);
    if (errno != 0 || end == sizeStr.c_str()) {
      if (errorMsg)
        *errorMsg = "Invalid member size in .deb";
      apm::logger::error("extractDebArchive: invalid member size");
      return false;
    }
    memberSize = static_cast<std::size_t>(parsed);

    // Decide if we care about this member.
    bool isControl = startsWith(name, "control.tar");
    bool isData = startsWith(name, "data.tar");

    std::string outPath;
    std::ofstream outFile;

    if (isControl || isData) {
      outPath = apm::fs::joinPath(outputDir, name);

      // Remember paths + compression types
      if (isControl) {
        outParts.controlTarPath = outPath;
        outParts.controlCompression = detectCompressionFromName(name);
        apm::logger::debug("Found control member: " + name);
      } else if (isData) {
        outParts.dataTarPath = outPath;
        outParts.dataCompression = detectCompressionFromName(name);
        apm::logger::debug("Found data member: " + name);
      }

      outFile.open(outPath, std::ios::out | std::ios::binary | std::ios::trunc);
      if (!outFile.is_open()) {
        if (errorMsg)
          *errorMsg = "Failed to create output file: " + outPath;
        apm::logger::error("extractDebArchive: cannot create: " + outPath);
        return false;
      }
    }

    // Stream member contents (or just skip if we don't care about it)
    std::size_t remaining = memberSize;
    const std::size_t BUF_SIZE = 8192;
    char buffer[BUF_SIZE];

    while (remaining > 0) {
      std::size_t toRead = remaining < BUF_SIZE ? remaining : BUF_SIZE;
      in.read(buffer, static_cast<std::streamsize>(toRead));
      if (!in) {
        if (errorMsg)
          *errorMsg = "Truncated member data in .deb";
        apm::logger::error("extractDebArchive: truncated member data");
        return false;
      }

      if (outFile.is_open()) {
        outFile.write(buffer, static_cast<std::streamsize>(toRead));
        if (!outFile) {
          if (errorMsg)
            *errorMsg = "Failed to write file: " + outPath;
          apm::logger::error("extractDebArchive: write failed: " + outPath);
          return false;
        }
      }

      remaining -= toRead;
    }

    if (outFile.is_open()) {
      outFile.flush();
      outFile.close();
    }

    // ar members are 2-byte aligned: if size is odd, there's a pad byte
    if (memberSize % 2 != 0) {
      char pad;
      in.read(&pad, 1);
      if (!in) {
        if (errorMsg)
          *errorMsg = "Truncated padding byte in .deb";
        apm::logger::error("extractDebArchive: truncated pad byte");
        return false;
      }
    }
  }

  // Basic validation: we expect at least control and data tar members
  if (outParts.controlTarPath.empty()) {
    apm::logger::warn("extractDebArchive: no control.tar.* member found");
  }
  if (outParts.dataTarPath.empty()) {
    apm::logger::warn("extractDebArchive: no data.tar.* member found");
  }

  return true;
}

} // namespace apm::deb
