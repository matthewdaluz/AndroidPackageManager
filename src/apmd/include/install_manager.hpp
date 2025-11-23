/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: install_manager.hpp
 * Purpose: Declare orchestration APIs for installing, removing, and upgrading Debian-style packages.
 * Last Modified: November 22nd, 2025. - 10:30 PM Eastern Time.
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

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace apm::repo {
struct RepoIndex;
using RepoIndexList = std::vector<RepoIndex>;
} // namespace apm::repo

namespace apm::install {

enum class InstallProgressEvent {
  Download
};

struct InstallProgress {
  InstallProgressEvent event = InstallProgressEvent::Download;
  std::string packageName;
  std::string url;
  std::string destination;
  std::uint64_t downloadedBytes = 0;
  std::uint64_t totalBytes = 0;
  std::uint64_t uploadedBytes = 0;
  std::uint64_t uploadTotal = 0;
  double downloadSpeedBytesPerSec = 0.0;
  double uploadSpeedBytesPerSec = 0.0;
  bool finished = false;
};

using InstallProgressCallback = std::function<void(const InstallProgress &)>;

struct InstallOptions {
  bool simulate = false;
  bool reinstall = false;
  bool isTermuxPackage = false;
};

struct InstallResult {
  bool ok = false;
  std::string message;
  std::vector<std::string> installedPackages;
  std::vector<std::string> skippedPackages;
};

bool installWithDeps(const apm::repo::RepoIndexList &repoIndices,
                     const std::string &rootPackage, const InstallOptions &opts,
                     InstallResult &result,
                     InstallProgressCallback progressCb = {});

// -------------------- Removal --------------------

struct RemoveOptions {
  bool purge = false;
  bool force = false; // ignore reverse deps (used internally)
};

struct RemoveResult {
  bool ok = false;
  std::string message;
  std::vector<std::string> removedPackages;
};

bool removePackage(const std::string &packageName, const RemoveOptions &opts,
                   RemoveResult &result);

// -------------------- Autoremove --------------------

struct AutoremoveResult {
  bool ok = false;
  std::string message;
  std::vector<std::string> removedPackages;
};

bool autoremove(const RemoveOptions &opts, AutoremoveResult &result);

// -------------------- Upgrade --------------------

struct UpgradeOptions {
  bool simulate = false;
  bool distUpgrade = false; // reserved for future
};

struct UpgradeResult {
  bool ok = false;
  std::string message;
  std::vector<std::string> upgradedPackages;
  std::vector<std::string> skippedPackages;
};

// If 'targets' is empty → upgrade all installed packages.
// Otherwise → upgrade only packages in 'targets'.
bool upgradePackages(const apm::repo::RepoIndexList &repoIndices,
                     const std::vector<std::string> &targets,
                     const UpgradeOptions &opts, UpgradeResult &result);

} // namespace apm::install
