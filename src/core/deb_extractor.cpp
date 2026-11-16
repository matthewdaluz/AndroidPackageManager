/*
 * APM - Android Package Manager
 *
 * Skidworks Open-Engineering - Software Branch
 * Copyright (C) 2027 Skidworks Open-Engineering
 *
 * File: deb_extractor.cpp
 * Purpose: Implement .deb archive parsing and control/data extraction.
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

#include "deb_extractor.hpp"
#include "fs.hpp"
#include "logger.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace apm::deb {
namespace {
struct StagingDirectory {
  std::string path;
  ~StagingDirectory() {
    if (!path.empty())
      apm::fs::removeDirRecursive(path);
  }
};
std::string trimmed(std::string value) {
  const auto first = value.find_first_not_of(' ');
  return first == std::string::npos
             ? std::string{}
             : value.substr(first, value.find_last_not_of(' ') - first + 1);
}
bool compression(const std::string &name, const std::string &base,
                 DebCompression &out) {
  if (name == base)
    out = DebCompression::None;
  else if (name == base + ".gz")
    out = DebCompression::Gzip;
  else if (name == base + ".xz")
    out = DebCompression::Xz;
  else if (name == base + ".bz2")
    out = DebCompression::Bzip2;
  else if (name == base + ".lzma")
    out = DebCompression::Lzma;
  else if (name == base + ".zst")
    out = DebCompression::Zstd;
  else
    return false;
  return true;
}
} // namespace

bool extractDebArchive(const std::string &debPath, const std::string &outputDir,
                       DebParts &outParts, std::string *errorMsg) {
  outParts = {};
  auto fail = [&](const std::string &message) {
    if (errorMsg)
      *errorMsg = message;
    apm::logger::error("extractDebArchive: " + message);
    return false;
  };
  if (!apm::fs::isFile(debPath))
    return fail("Deb file is not a regular file: " + debPath);
  std::ifstream input(debPath, std::ios::binary);
  if (!input)
    return fail("Cannot open .deb: " + debPath);
  input.seekg(0, std::ios::end);
  const auto endPosition = input.tellg();
  if (endPosition < 8)
    return fail("Invalid .deb: missing ar magic header");
  const auto length = static_cast<std::uint64_t>(endPosition);
  input.seekg(0);
  std::array<char, 8> magic{};
  if (!input.read(magic.data(), magic.size()) ||
      std::memcmp(magic.data(), "!<arch>\n", 8) != 0)
    return fail("Invalid .deb: missing ar magic header");
  if (!apm::fs::createDirs(outputDir))
    return fail("Cannot create .deb output directory: " + outputDir);
  const auto pattern = apm::fs::joinPath(outputDir, ".deb-XXXXXX");
  std::vector<char> bytes(pattern.begin(), pattern.end());
  bytes.push_back('\0');
  char *created = ::mkdtemp(bytes.data());
  if (!created)
    return fail("Cannot create private .deb staging directory");
  StagingDirectory staging{created};
  std::uint64_t position = 8;
  bool versionSeen = false, controlSeen = false, dataSeen = false;
  std::string controlName, dataName;
  DebParts parts;
  while (position < length) {
    std::array<char, 60> header{};
    if (length - position < header.size() ||
        !input.read(header.data(), header.size()))
      return fail("Truncated ar member header in .deb");
    position += header.size();
    if (header[58] != '`' || header[59] != '\n')
      return fail("Invalid ar member magic in .deb");
    std::string name = trimmed(std::string(header.data(), 16));
    if (!name.empty() && name.back() == '/')
      name.pop_back();
    const auto sizeText = trimmed(std::string(header.data() + 48, 10));
    std::uint64_t size = 0;
    const auto parsed = std::from_chars(
        sizeText.data(), sizeText.data() + sizeText.size(), size);
    if (sizeText.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != sizeText.data() + sizeText.size())
      return fail("Invalid ar member size in .deb");
    if (size > length - position || (size & 1) > length - position - size)
      return fail("Truncated ar member data or padding in .deb");
    if (!versionSeen) {
      if (name != "debian-binary" || size > 4096)
        return fail("Invalid .deb: first member must be debian-binary");
      std::string version(static_cast<std::size_t>(size), '\0');
      if (!input.read(version.data(), static_cast<std::streamsize>(size)))
        return fail("Truncated debian-binary member");
      const auto newline = version.find('\n');
      const auto firstLine = version.substr(0, newline);
      if (newline == std::string::npos || firstLine.size() < 3 ||
          firstLine.compare(0, 2, "2.") != 0 ||
          !std::all_of(firstLine.begin() + 2, firstLine.end(),
                       [](unsigned char c) { return c >= '0' && c <= '9'; }))
        return fail("Unsupported .deb format version (expected 2.x)");
      versionSeen = true;
    } else {
      if (name == "debian-binary")
        return fail("Duplicate debian-binary member");
      DebCompression codec = DebCompression::Unknown;
      const bool control = name.compare(0, 11, "control.tar") == 0;
      const bool data = name.compare(0, 8, "data.tar") == 0;
      if (control || data) {
        if (!compression(name, control ? "control.tar" : "data.tar", codec))
          return fail("Unsupported or unsafe .deb tar member: " + name);
        if ((control && controlSeen) || (data && dataSeen))
          return fail("Duplicate control/data member in .deb");
        if (data && !controlSeen)
          return fail("Invalid .deb: control.tar must precede data.tar");
        const auto path = apm::fs::joinPath(staging.path, name);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output)
          return fail("Cannot write extracted member: " + name);
        std::array<char, 65536> buffer{};
        for (std::uint64_t left = size; left;) {
          const auto count = static_cast<std::size_t>(
              std::min<std::uint64_t>(left, buffer.size()));
          if (!input.read(buffer.data(), static_cast<std::streamsize>(count)))
            return fail("Truncated .deb member: " + name);
          output.write(buffer.data(), static_cast<std::streamsize>(count));
          if (!output)
            return fail("Cannot write .deb member: " + name);
          left -= count;
        }
        output.close();
        if (!output)
          return fail("Cannot finish writing .deb member: " + name);
        if (control) {
          controlSeen = true;
          controlName = name;
          parts.controlCompression = codec;
        } else {
          dataSeen = true;
          dataName = name;
          parts.dataCompression = codec;
        }
      } else {
        input.seekg(static_cast<std::streamoff>(size), std::ios::cur);
        if (!input)
          return fail("Cannot skip .deb member: " + name);
      }
    }
    position += size;
    if (size & 1) {
      char pad;
      if (!input.get(pad))
        return fail("Missing ar padding in .deb");
      ++position;
    }
  }
  if (!versionSeen || !controlSeen || !dataSeen)
    return fail("Invalid .deb: debian-binary, control.tar and data.tar members "
                "are required");
  parts.controlTarPath = apm::fs::joinPath(outputDir, controlName);
  parts.dataTarPath = apm::fs::joinPath(outputDir, dataName);
  if (::rename(apm::fs::joinPath(staging.path, controlName).c_str(),
               parts.controlTarPath.c_str()) != 0 ||
      ::rename(apm::fs::joinPath(staging.path, dataName).c_str(),
               parts.dataTarPath.c_str()) != 0)
    return fail("Cannot publish extracted .deb members");
  outParts = std::move(parts);
  return true;
}
} // namespace apm::deb
