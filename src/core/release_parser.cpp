/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: release_parser.cpp
 * Purpose: Implement parsing of Release files and SHA256 lookups.
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

#include "release_parser.hpp"

#include "fs.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace apm::repo {

static inline void ltrim(std::string &s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
            return !std::isspace(ch);
          }));
}

static inline void rtrim(std::string &s) {
  s.erase(std::find_if(s.rbegin(), s.rend(),
                       [](unsigned char ch) { return !std::isspace(ch); })
              .base(),
          s.end());
}

static inline void trim(std::string &s) {
  ltrim(s);
  rtrim(s);
}

// Parse a Release file and capture the SHA256 stanza lines so we can validate
// the Packages metadata.
bool parseReleaseFile(const std::string &path, ReleaseInfo &out,
                      std::string *errorMsg) {
  out.sha256.clear();

  std::string content;
  if (!apm::fs::readFile(path, content)) {
    if (errorMsg)
      *errorMsg = "Failed to read Release file: " + path;
    apm::logger::error("parseReleaseFile: cannot read " + path);
    return false;
  }

  std::istringstream in(content);
  std::string line;
  bool inSha256Section = false;

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (line.empty()) {
      // Blank line ends any current section
      inSha256Section = false;
      continue;
    }

    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
      // Continuation of previous section
      if (!inSha256Section) {
        continue;
      }

      // Format: <hash><space><size><space><name>
      std::string trimmed = line;
      trim(trimmed);
      if (trimmed.empty())
        continue;

      std::istringstream ls(trimmed);
      std::string hash, sizeStr, name;
      if (!(ls >> hash >> sizeStr)) {
        continue;
      }
      std::getline(ls, name);
      trim(name);
      if (hash.empty() || name.empty())
        continue;

      ReleaseChecksumEntry entry;
      entry.hash = hash;
      entry.name = name;

      try {
        entry.size = static_cast<std::size_t>(std::stoull(sizeStr));
      } catch (...) {
        entry.size = 0;
      }

      out.sha256.push_back(std::move(entry));
      continue;
    }

    // New top-level field
    std::string key = line;
    std::string value;

    auto colonPos = line.find(':');
    if (colonPos != std::string::npos) {
      key = line.substr(0, colonPos);
      value = line.substr(colonPos + 1);
      trim(key);
      trim(value);
    } else {
      trim(key);
    }

    inSha256Section = (key == "SHA256");
  }

  if (out.sha256.empty()) {
    if (errorMsg) {
      *errorMsg = "Release file has no SHA256 section";
    }
    apm::logger::warn("parseReleaseFile: no SHA256 section in " + path);
    return false;
  }

  apm::logger::info("parseReleaseFile: parsed " +
                    std::to_string(out.sha256.size()) +
                    " SHA256 entries from " + path);
  return true;
}

// Find the SHA256 hash for a specific Packages path.
bool findSha256ForPath(const ReleaseInfo &info, const std::string &name,
                       std::string &outHash) {
  for (const auto &e : info.sha256) {
    if (e.name == name) {
      outHash = e.hash;
      return true;
    }
  }
  return false;
}

} // namespace apm::repo
