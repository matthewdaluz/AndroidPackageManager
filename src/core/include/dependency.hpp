/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: dependency.hpp
 * Purpose: Declare dependency resolver interfaces and result structures.
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
#include <vector>

#include "repo_index.hpp"

namespace apm::dep {

// Result of resolving dependencies for a package.
struct ResolutionResult {
  // Packages to install, in order (dependencies first, root last).
  std::vector<const apm::repo::PackageEntry *> installOrder;

  // Names of dependencies that could not be found in any repo.
  std::vector<std::string> missing;

  // Names detected as part of at least one dependency cycle.
  std::vector<std::string> cycles;

  bool success() const { return missing.empty() && cycles.empty(); }
};

// Resolve dependencies for a root package name against a repo PackageList.
//
// - repoPackages: merged list of all repo Packages entries
// - rootPackage:  name of the package the user wants to install
// - arch:         target architecture (e.g. "arm64"); used by findPackage()
// - alreadyInstalled: optional list of package names already installed;
//                     they will not be added to installOrder.
//
// On success:
//   - returns true
//   - 'out.installOrder' contains the installation order (deps first)
//
// On failure:
//   - returns false
//   - 'out.missing' and/or 'out.cycles' explain why
//   - errorMsg (if non-null) contains a human-readable message
bool resolveDependencies(const apm::repo::PackageList &repoPackages,
                         const std::string &rootPackage,
                         const std::string &arch, ResolutionResult &out,
                         const std::vector<std::string> &alreadyInstalled = {},
                         std::string *errorMsg = nullptr);

bool resolveTermuxDependencies(
    const apm::repo::PackageList &repoPackages,
    const std::string &rootPackage, ResolutionResult &out,
    const std::vector<std::string> &alreadyInstalled = {},
    std::string *errorMsg = nullptr);

} // namespace apm::dep
