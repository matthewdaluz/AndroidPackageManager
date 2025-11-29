/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: release_parser.cpp
 * Purpose: Implement parsing of Release files and SHA256 lookups.
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

#include "release_parser.hpp"

#include "fs.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
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

// Internal: core text parser used by both file and in-memory paths.
static bool parseReleaseCore(std::istream &in, const std::string &sourceLabel,
                             ReleaseInfo &out, std::string *errorMsg) {
  out.sha256.clear();
  out.md5.clear();

  enum class Section { None, SHA256, MD5 };
  Section section = Section::None;

  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    if (line.empty()) {
      section = Section::None;
      continue;
    }

    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
      // Continuation line for current section, if any
      if (section == Section::None) {
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

      errno = 0;
      char *end = nullptr;
      unsigned long long parsed = std::strtoull(sizeStr.c_str(), &end, 10);
      if (errno != 0 || end == sizeStr.c_str()) {
        entry.size = 0;
      } else {
        entry.size = static_cast<std::size_t>(parsed);
      }

      if (section == Section::SHA256) {
        out.sha256.push_back(std::move(entry));
      } else if (section == Section::MD5) {
        out.md5.push_back(std::move(entry));
      }
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

    if (key == "SHA256") {
      section = Section::SHA256;
    } else if (key == "MD5Sum") {
      section = Section::MD5;
    } else {
      section = Section::None;
    }
  }

  if (out.sha256.empty() && out.md5.empty()) {
    if (errorMsg) {
      *errorMsg = "Release content has no SHA256 or MD5Sum sections";
    }
    apm::logger::warn("parseRelease: no checksum sections in " + sourceLabel);
    return false;
  }

  apm::logger::info("parseRelease: parsed " +
                    std::to_string(out.sha256.size()) + " SHA256 entries and " +
                    std::to_string(out.md5.size()) + " MD5 entries from " +
                    sourceLabel);
  return true;
}

// Parse a Release file and capture checksum stanzas (SHA256/MD5).
bool parseReleaseFile(const std::string &path, ReleaseInfo &out,
                      std::string *errorMsg) {
  std::string content;
  if (!apm::fs::readFile(path, content)) {
    if (errorMsg)
      *errorMsg = "Failed to read Release file: " + path;
    apm::logger::error("parseReleaseFile: cannot read " + path);
    return false;
  }

  std::istringstream in(content);
  return parseReleaseCore(in, path, out, errorMsg);
}

// Parse already-loaded Release content (e.g., from a clearsigned InRelease).
bool parseReleaseText(const std::string &text, ReleaseInfo &out,
                      std::string *errorMsg) {
  std::istringstream in(text);
  return parseReleaseCore(in, "<memory>", out, errorMsg);
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

// Find the MD5 hash for a specific Packages path.
bool findMd5ForPath(const ReleaseInfo &info, const std::string &name,
                    std::string &outHash) {
  for (const auto &e : info.md5) {
    if (e.name == name) {
      outHash = e.hash;
      return true;
    }
  }
  return false;
}

} // namespace apm::repo
