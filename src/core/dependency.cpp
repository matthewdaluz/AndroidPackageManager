/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: dependency.cpp
 * Purpose: Implement dependency resolution and cycle/missing detection for packages.
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

#include "dependency.hpp"
#include "logger.hpp"

#include <functional>
#include <sstream>
#include <unordered_set>

namespace apm::dep {

namespace {

std::string normalizeDependencyName(std::string name) {
  auto colonPos = name.find(':');
  if (colonPos != std::string::npos) {
    name = name.substr(0, colonPos);
  }
  return name;
}

void appendUnique(std::vector<std::string> &out,
                  std::unordered_set<std::string> &seen,
                  const std::string &name) {
  if (!name.empty() && seen.insert(name).second) {
    out.push_back(name);
  }
}

std::string buildResolutionError(const ResolutionResult &out) {
  std::ostringstream ss;
  bool wrote = false;

  if (!out.missing.empty()) {
    ss << "Missing dependencies:";
    for (const auto &m : out.missing) {
      ss << " " << m;
    }
    wrote = true;
  }

  if (!out.cycles.empty()) {
    if (wrote) {
      ss << "; ";
    }
    ss << "Dependency cycles:";
    for (const auto &c : out.cycles) {
      ss << " " << c;
    }
  }

  return ss.str();
}

bool resolveRecursive(
    const std::string &rootPackage, ResolutionResult &out,
    const std::vector<std::string> &alreadyInstalled,
    const std::function<const apm::repo::PackageEntry *(const std::string &)>
        &findPkg,
    const std::string &logPrefix, const std::string &notFoundPrefix,
    std::string *errorMsg) {
  out = ResolutionResult{};

  if (rootPackage.empty()) {
    if (errorMsg)
      *errorMsg = "Root package name is empty";
    apm::logger::error(logPrefix + ": root package is empty");
    return false;
  }

  std::unordered_set<std::string> installedSet;
  for (const auto &name : alreadyInstalled) {
    installedSet.insert(name);
  }

  const apm::repo::PackageEntry *rootPkg = findPkg(rootPackage);
  if (!rootPkg) {
    std::string msg = notFoundPrefix + rootPackage;
    if (errorMsg)
      *errorMsg = msg;
    apm::logger::error(logPrefix + ": " + msg);
    return false;
  }

  std::unordered_set<std::string> visiting;
  std::unordered_set<std::string> visited;
  std::unordered_set<std::string> plannedSet;
  std::unordered_set<std::string> missingSet;
  std::unordered_set<std::string> cycleSet;

  std::function<void(const apm::repo::PackageEntry *)> visit =
      [&](const apm::repo::PackageEntry *pkg) {
        if (!pkg) {
          return;
        }

        const std::string &pkgName = pkg->packageName;
        if (visited.count(pkgName)) {
          return;
        }

        if (visiting.count(pkgName)) {
          appendUnique(out.cycles, cycleSet, pkgName);
          apm::logger::warn(logPrefix + ": dependency cycle involving " +
                            pkgName);
          return;
        }

        visiting.insert(pkgName);

        for (const auto &depRaw : pkg->depends) {
          std::string depName = normalizeDependencyName(depRaw);
          if (depName.empty() || depName == pkgName) {
            continue;
          }

          const apm::repo::PackageEntry *depPkg = findPkg(depName);
          if (!depPkg) {
            appendUnique(out.missing, missingSet, depName);
            apm::logger::warn(logPrefix + ": missing dependency: " + depName);
            continue;
          }

          visit(depPkg);
        }

        visiting.erase(pkgName);
        visited.insert(pkgName);

        if (installedSet.count(pkgName)) {
          return;
        }

        if (plannedSet.insert(pkgName).second) {
          out.installOrder.push_back(pkg);
        }
      };

  visit(rootPkg);

  if (!out.success()) {
    std::string msg = buildResolutionError(out);
    if (errorMsg) {
      *errorMsg = msg;
    }
    apm::logger::error(logPrefix + ": " + msg);
    return false;
  }

  apm::logger::info(logPrefix + ": resolved '" + rootPackage + "' with " +
                    std::to_string(out.installOrder.size()) +
                    " packages in install order");

  return true;
}

} // namespace

// Build an ordered install plan for the requested root package. The resolver
// walks dependencies recursively and only filters packages from the final plan
// after their dependency tree has been inspected.
bool resolveDependencies(const apm::repo::PackageList &repoPackages,
                         const std::string &rootPackage,
                         const std::string &arch, ResolutionResult &out,
                         const std::vector<std::string> &alreadyInstalled,
                         std::string *errorMsg) {
  auto findPkg =
      [&](const std::string &name) -> const apm::repo::PackageEntry * {
    return apm::repo::findPackage(repoPackages, name, arch);
  };

  return resolveRecursive(rootPackage, out, alreadyInstalled, findPkg,
                          "resolveDependencies",
                          "Package not found in repositories: ", errorMsg);
}

bool resolveTermuxDependencies(const apm::repo::PackageList &repoPackages,
                               const std::string &rootPackage,
                               ResolutionResult &out,
                               const std::vector<std::string> &alreadyInstalled,
                               std::string *errorMsg) {
  auto findPkg =
      [&](const std::string &name) -> const apm::repo::PackageEntry * {
    for (const auto &pkg : repoPackages) {
      if (!pkg.isTermuxPackage)
        continue;
      if (pkg.packageName == name)
        return &pkg;
    }
    return nullptr;
  };

  return resolveRecursive(rootPackage, out, alreadyInstalled, findPkg,
                          "resolveTermuxDependencies",
                          "Package not found in Termux repositories: ",
                          errorMsg);
}

} // namespace apm::dep
