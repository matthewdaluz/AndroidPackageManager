/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: list.cpp
 * Purpose: Implement the local list command that enumerates installed packages.
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

#include "status_db.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace apm::cmd {

// Print every package stored in the status DB along with a few key fields.
int run_list(int argc, char **argv) {
  (void)argc;
  (void)argv;

  apm::status::InstalledDb db;
  std::string err;
  if (!apm::status::loadStatus(db, &err)) {
    std::cerr << "apm list: failed to load status DB: " << err << "\n";
    return 1;
  }

  if (db.empty()) {
    std::cout << "No packages installed.\n";
    return 0;
  }

  // Make a sorted list of package names for nicer output
  std::vector<std::string> names;
  names.reserve(db.size());
  for (const auto &kv : db) {
    names.push_back(kv.first);
  }
  std::sort(names.begin(), names.end());

  std::cout << "Installed packages:\n";
  for (const auto &name : names) {
    const auto &pkg = db.at(name);
    std::cout << "  " << pkg.name;

    if (!pkg.version.empty()) {
      std::cout << " " << pkg.version;
    }

    if (!pkg.architecture.empty()) {
      std::cout << " [" << pkg.architecture << "]";
    }

    if (!pkg.status.empty()) {
      std::cout << "  (" << pkg.status << ")";
    }

    std::cout << "\n";
  }

  return 0;
}

} // namespace apm::cmd
