/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: dependency.cpp
 * Purpose: Implement dependency resolution and cycle/missing detection for packages.
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

#include "dependency.hpp"
#include "logger.hpp"

#include <sstream>
#include <unordered_set>

namespace apm::dep {

// Build an ordered install plan for the requested root package. Because APM
// focuses on the first alternative only, the resolver walks direct dependencies
// and filters out ones already installed.
bool resolveDependencies(const apm::repo::PackageList &repoPackages,
                         const std::string &rootPackage,
                         const std::string &arch, ResolutionResult &out,
                         const std::vector<std::string> &alreadyInstalled,
                         std::string *errorMsg) {
  out = ResolutionResult{};

  if (rootPackage.empty()) {
    if (errorMsg)
      *errorMsg = "Root package name is empty";
    apm::logger::error("resolveDependencies: root package is empty");
    return false;
  }

  // For fast "is installed" checks.
  std::unordered_set<std::string> installedSet;
  for (const auto &name : alreadyInstalled) {
    installedSet.insert(name);
  }

  // Track missing deps without duplicates.
  std::unordered_set<std::string> missingSet;

  std::vector<const apm::repo::PackageEntry *> order;

  // Helper to look up a package in the repo by name/arch.
  auto findPkg =
      [&](const std::string &name) -> const apm::repo::PackageEntry * {
    return apm::repo::findPackage(repoPackages, name, arch);
  };

  const apm::repo::PackageEntry *rootPkg = findPkg(rootPackage);
  if (!rootPkg) {
    std::string msg = "Package not found in repositories: " + rootPackage;
    if (errorMsg)
      *errorMsg = msg;
    apm::logger::error("resolveDependencies: " + msg);
    return false;
  }

  std::unordered_set<std::string> queuedDeps;

  // Only consider the direct dependencies declared by the package. We do not
  // attempt to resolve further nested dependencies; those will be satisfied
  // later by explicit installs if needed.
  for (const auto &depRaw : rootPkg->depends) {
    if (depRaw.empty())
      continue;

    std::string depName = depRaw;
    auto colonPos = depName.find(':');
    if (colonPos != std::string::npos) {
      depName = depName.substr(0, colonPos);
    }

    if (depName.empty() || depName == rootPackage)
      continue;

    if (!queuedDeps.insert(depName).second)
      continue;

    if (installedSet.find(depName) != installedSet.end())
      continue;

    const apm::repo::PackageEntry *depPkg = findPkg(depName);
    if (!depPkg) {
      if (missingSet.insert(depName).second) {
        apm::logger::warn("resolveDependencies: missing dependency: " +
                          depName);
      }
      continue;
    }

    order.push_back(depPkg);
  }

  if (installedSet.find(rootPackage) == installedSet.end()) {
    order.push_back(rootPkg);
  }

  // Populate result structure
  out.installOrder = std::move(order);

  // Copy missingSet/cycleSet to vectors for stable consumption
  out.missing.reserve(missingSet.size());
  for (const auto &name : missingSet) {
    out.missing.push_back(name);
  }

  // Cycles only happen with the recursive resolver; keep API behavior by
  // reporting an empty vector.
  out.cycles.clear();

  // If anything went wrong, build a summary error message (currently only
  // missing dependencies).
  if (!out.success()) {
    std::ostringstream ss;
    if (!out.missing.empty()) {
      ss << "Missing dependencies:";
      for (const auto &m : out.missing) {
        ss << " " << m;
      }
    }
    if (errorMsg) {
      *errorMsg = ss.str();
    }
    apm::logger::error("resolveDependencies: " + ss.str());
    return false;
  }

  // Success; log basic info
  apm::logger::info("resolveDependencies: resolved '" + rootPackage +
                    "' with " + std::to_string(out.installOrder.size()) +
                    " packages in install order");

  return true;
}

} // namespace apm::dep
