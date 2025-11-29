/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: info.cpp
 * Purpose: Implement the local info command summarizing installed/candidate metadata.
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

#include "config.hpp"
#include "repo_index.hpp"
#include "status_db.hpp"

#include <iostream>
#include <string>

namespace apm::cmd {

// Combine local status details with repo metadata to show a package summary.
int run_info(int argc, char **argv) {
  if (argc < 1) {
    std::cerr << "Usage: apm info <package>\n";
    return 1;
  }

  std::string name = argv[0];

  // ---------------- Installed info ----------------
  apm::status::InstalledPackage installed;
  bool isInstalled = apm::status::isInstalled(name, &installed, nullptr);

  if (!isInstalled) {
    std::cout << "Package: " << name << "\n";
    std::cout << "Installed: no\n";
  } else {
    std::cout << "Package: " << installed.name << "\n";
    std::cout << "Installed: yes\n";
    if (!installed.version.empty()) {
      std::cout << "Installed-Version: " << installed.version << "\n";
    }
    if (!installed.architecture.empty()) {
      std::cout << "Architecture: " << installed.architecture << "\n";
    }
    if (!installed.installRoot.empty()) {
      std::cout << "Installed-Root: " << installed.installRoot << "\n";
    }
    if (!installed.status.empty()) {
      std::cout << "Status: " << installed.status << "\n";
    }
    if (!installed.repoUri.empty()) {
      std::cout << "Installed-From: " << installed.repoUri;
      if (!installed.repoDist.empty()) {
        std::cout << " " << installed.repoDist;
      }
      if (!installed.repoComponent.empty()) {
        std::cout << " " << installed.repoComponent;
      }
      std::cout << "\n";
    }
    std::cout << "\n";
  }

  // ---------------- Candidate / repo info ----------------
  apm::repo::RepoIndexList indices;
  std::string err;

  if (!apm::repo::buildRepoIndices(apm::config::getSourcesList(),
                                   apm::config::getListsDir(),
                                   apm::config::getDefaultArch(), indices, &err)) {
    std::cout << "Repository info: unavailable";
    if (!err.empty()) {
      std::cout << " (run 'apm update'? " << err << ")";
    }
    std::cout << "\n";
    return 0; // not a hard failure; installed info may still be useful
  }

  const apm::repo::PackageEntry *candidate = nullptr;

  for (const auto &idx : indices) {
    const auto *found = apm::repo::findPackage(idx.packages, name, "");
    if (found) {
      candidate = found;
      break;
    }
  }

  if (!candidate) {
    std::cout << "Repository info: no candidate found in configured sources\n";
    return 0;
  }

  std::cout << "Candidate-Version: " << candidate->version << "\n";
  if (!candidate->architecture.empty()) {
    std::cout << "Candidate-Architecture: " << candidate->architecture << "\n";
  }

  if (!candidate->repoUri.empty()) {
    std::cout << "Repo: " << candidate->repoUri;
    if (!candidate->repoDist.empty()) {
      std::cout << " " << candidate->repoDist;
    }
    if (!candidate->repoComponent.empty()) {
      std::cout << " " << candidate->repoComponent;
    }
    std::cout << "\n";
  }

  if (!candidate->filename.empty()) {
    std::cout << "Filename: " << candidate->filename << "\n";
  }

  // Slight compare if both installed + candidate
  if (isInstalled && !installed.version.empty() &&
      !candidate->version.empty()) {
    if (installed.version == candidate->version) {
      std::cout << "\nStatus: up-to-date (installed == candidate)\n";
    } else {
      std::cout << "\nStatus: upgrade available\n";
      std::cout << "  Installed: " << installed.version << "\n";
      std::cout << "  Candidate: " << candidate->version << "\n";
    }
  }

  return 0;
}

} // namespace apm::cmd
