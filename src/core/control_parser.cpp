/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: control_parser.cpp
 * Purpose: Implement Debian control file parsing from disk or memory.
 * Last Modified: 2026-03-15 11:56:16.537647032 -0400.
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

#include "control_parser.hpp"
#include "fs.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace apm::control {

// -------------------------------------------------------------
// String helpers
// -------------------------------------------------------------

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

// Split a string by a delimiter into a vector of trimmed parts.
static std::vector<std::string> splitAndTrim(const std::string &input,
                                             char delim) {
  std::vector<std::string> parts;
  std::string current;
  std::istringstream ss(input);

  while (std::getline(ss, current, delim)) {
    trim(current);
    if (!current.empty()) {
      parts.push_back(current);
    }
  }

  return parts;
}

// -------------------------------------------------------------
// Depends: field parsing
// -------------------------------------------------------------

// Parse a Depends: field like:
//   "libc6 (>= 2.31), libncursesw6 (>= 6.2) | libncursesw5, foo"
// into a list of package names:
//   ["libc6", "libncursesw6", "foo"]
//
// For now we ignore version constraints and alternatives,
// and just pick the first package in each comma group.
static std::vector<std::string>
parseDependsField(const std::string &dependsRaw) {
  std::vector<std::string> result;
  if (dependsRaw.empty()) {
    return result;
  }

  // Split by comma to get dependency groups
  auto groups = splitAndTrim(dependsRaw, ',');

  for (const auto &group : groups) {
    if (group.empty())
      continue;

    // Each group may have alternatives separated by '|'
    // e.g., "libncursesw6 (>= 6.2) | libncursesw5"
    // We'll take the first alternative only.
    std::string firstAlt = group;
    auto pipePos = firstAlt.find('|');
    if (pipePos != std::string::npos) {
      firstAlt = firstAlt.substr(0, pipePos);
      trim(firstAlt);
    }

    // Now extract the package name at the start of firstAlt,
    // up to the first space or '(' character.
    std::string name;
    for (char ch : firstAlt) {
      if (std::isspace(static_cast<unsigned char>(ch)) || ch == '(') {
        break;
      }
      name.push_back(ch);
    }

    trim(name);
    if (!name.empty()) {
      result.push_back(name);
    }
  }

  return result;
}

// -------------------------------------------------------------
// Core parsing logic (RFC822-style control file)
// -------------------------------------------------------------

// Parse the provided control stanza into the ControlFile structure. We only
// care about the first paragraph since .deb control files typically have one.
ControlFile parseControlString(const std::string &content) {
  ControlFile cf;
  if (content.empty()) {
    return cf;
  }

  std::unordered_map<std::string, std::string> fields;

  std::istringstream in(content);
  std::string line;
  std::string currentKey;

  while (std::getline(in, line)) {
    // Strip trailing '\r' if present (e.g., from Windows-style CRLF)
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }

    // Empty line typically separates stanzas.
    // For APM we only care about the first stanza, so we stop here.
    if (line.empty()) {
      break;
    }

    if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
      // Continuation line for the previous key.
      // Append with a newline to preserve readability.
      if (!currentKey.empty()) {
        std::string value = line;
        trim(value);
        auto it = fields.find(currentKey);
        if (it != fields.end()) {
          it->second.append("\n");
          it->second.append(value);
        }
      }
    } else {
      // New key: value line
      auto colonPos = line.find(':');
      if (colonPos == std::string::npos) {
        // Malformed line; skip.
        continue;
      }

      std::string key = line.substr(0, colonPos);
      std::string value = line.substr(colonPos + 1);

      trim(key);
      trim(value);

      if (!key.empty()) {
        currentKey = key;
        fields[key] = value;
      }
    }
  }

  // Fill ControlFile struct from parsed fields
  cf.rawFields = fields;

  auto getField = [&](const std::string &key) -> std::string {
    auto it = fields.find(key);
    if (it != fields.end()) {
      return it->second;
    }
    return {};
  };

  cf.packageName = getField("Package");
  cf.version = getField("Version");
  cf.architecture = getField("Architecture");

  const std::string dependsRaw = getField("Depends");
  cf.depends = parseDependsField(dependsRaw);

  return cf;
}

// Convenience wrapper that reads a control file from disk before delegating to
// parseControlString.
ControlFile parseControlFile(const std::string &path) {
  std::string content;
  if (!apm::fs::readFile(path, content)) {
    // Return an empty ControlFile if we can't read the file.
    return ControlFile{};
  }
  return parseControlString(content);
}

} // namespace apm::control
