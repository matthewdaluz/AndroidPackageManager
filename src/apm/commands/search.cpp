/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: search.cpp
 * Purpose: Implement client-side repository search and result printing.
 * Last Modified: 2026-03-15 11:56:16.536347864 -0400.
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

#include "search.hpp"

#include "config.hpp"
#include "repo_index.hpp"

#include <cctype>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

// Simple lowercase helper for case-insensitive matching
std::string toLower(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

} // namespace

namespace apm::cli {

// Search the merged repo indices for the provided pattern list and emit human
// readable matches.
int searchPackages(const std::vector<std::string> &patternsIn) {
  if (patternsIn.empty()) {
    std::cerr << "apm: 'search' requires a pattern\n";
    return 1;
  }

  // Lowercase patterns
  std::vector<std::string> patterns;
  patterns.reserve(patternsIn.size());
  for (const auto &p : patternsIn) {
    patterns.push_back(toLower(p));
  }

  apm::repo::RepoIndexList indices;
  std::string err;
  if (!apm::repo::buildRepoIndices(apm::config::getSourcesList(),
                                   apm::config::getListsDir(),
                                   apm::config::getDefaultArch(), indices, &err)) {
    std::cerr << "apm search: failed to load repo indices";
    if (!err.empty())
      std::cerr << ": " << err;
    std::cerr << "\n";
    return 1;
  }

  std::unordered_set<std::string> seen;
  std::size_t matchCount = 0;

  for (const auto &idx : indices) {
    for (const auto &pkg : idx.packages) {

      // Extract description from rawFields if present
      std::string desc;
      auto it = pkg.rawFields.find("Description");
      if (it != pkg.rawFields.end()) {
        desc = it->second;
      }

      // Build searchable text
      std::string searchText = pkg.packageName;
      if (!desc.empty()) {
        searchText.push_back(' ');
        searchText += desc;
      }

      std::string hay = toLower(searchText);

      bool hit = false;
      for (const auto &pat : patterns) {
        if (hay.find(pat) != std::string::npos) {
          hit = true;
          break;
        }
      }

      if (!hit)
        continue;

      // Avoid duplicates across repos
      if (!seen.insert(pkg.packageName).second)
        continue;

      ++matchCount;

      // Display result
      std::cout << pkg.packageName;
      if (!pkg.version.empty())
        std::cout << " " << pkg.version;
      if (!pkg.architecture.empty())
        std::cout << " [" << pkg.architecture << "]";

      if (!desc.empty())
        std::cout << " - " << desc;

      std::cout << "\n";
    }
  }

  if (matchCount == 0) {
    std::cout << "No packages found matching the given pattern(s).\n";
  }

  return 0;
}

} // namespace apm::cli
