/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: control_parser.hpp
 * Purpose: Declare the representation of Debian control stanzas and parsing helpers.
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

#include <string>
#include <unordered_map>
#include <vector>

namespace apm::control {

// Parsed representation of a Debian control file (single stanza).
struct ControlFile {
  std::string packageName;
  std::string version;
  std::string architecture;
  std::vector<std::string> depends;

  // Raw key -> value map for any other fields (Description, Maintainer, etc.)
  std::unordered_map<std::string, std::string> rawFields;
};

// Parse a control file from disk.
// Returns a ControlFile with fields filled where available.
// On failure (e.g., file not readable), returns an object with empty
// strings/vectors.
ControlFile parseControlFile(const std::string &path);

// Parse a control file from a raw string (for testing / in-memory use).
ControlFile parseControlString(const std::string &content);

} // namespace apm::control
