/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: repo_index.hpp
 * Purpose: Declare repository source/index structures plus update and parsing APIs.
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
#include <unordered_map>
#include <vector>

namespace apm::repo {

enum class RepoFormat {
  Debian,
  Termux,
};

struct PackageEntry {
  std::string packageName;
  std::string version;
  std::string architecture;
  std::string filename; // "pool/..." or similar
  std::string sha256;   // optional

  std::vector<std::string> depends;
  std::unordered_map<std::string, std::string> rawFields;
  bool isTermuxPackage = false;

  // Repository metadata filled when we build indices
  std::string repoUri;       // e.g. https://deb.debian.org/debian
  std::string repoDist;      // e.g. bookworm
  std::string repoComponent; // e.g. main
};

using PackageList = std::vector<PackageEntry>;

// One 'deb' entry from sources.list
//
// Example:
//   deb [arch=arm64] https://deb.debian.org/debian bookworm main contrib
//
struct RepoSource {
  std::string type;                    // "deb"
  std::string uri;                     // base URI
  std::string dist;                    // suite / dist (stable, bookworm, etc.)
  std::vector<std::string> components; // main, contrib, ...

  // Optional per-repo arch override (like apt's [arch=...])
  // e.g. "arm64" for Debian, "aarch64" for Termux.
  std::string arch;

  RepoFormat format = RepoFormat::Debian;
  bool isTermuxRepo = false;
};

using RepoSourceList = std::vector<RepoSource>;

// A specific repo+component+arch index
struct RepoIndex {
  RepoSource source;
  std::string component;    // single component from source.components
  std::string arch;         // resolved arch (override or default)
  std::string packagesPath; // local Lists path
  PackageList packages;     // parsed packages with repo* filled
};

using RepoIndexList = std::vector<RepoIndex>;

enum class RepoUpdateStage {
  DownloadRelease = 0,
  DownloadReleaseSignature,
  DownloadPackages,
  ParsePackages
};

struct RepoUpdateProgress {
  RepoUpdateStage stage = RepoUpdateStage::DownloadRelease;
  std::string repoUri;
  std::string dist;
  std::string component;

  std::string remotePath;
  std::string localPath;
  std::string description;

  std::uint64_t currentBytes = 0;
  std::uint64_t totalBytes = 0;

  double downloadSpeed = 0.0;
  double uploadSpeed = 0.0;
  bool finished = false;
};

using RepoUpdateProgressCallback =
    std::function<void(const RepoUpdateProgress &)>;

// ---------------------------------------------------------
// Sources.list & indices
// ---------------------------------------------------------

// Load sources from either:
//   - a single file: "deb ..." lines
//   - a directory: treats it like /etc/apt:
//       <dir>/sources.list
//       <dir>/sources.list.d/*.list
//
// Ignores comments (#...) and blank lines.
// Only 'deb' entries are kept, 'deb-src' etc are ignored.
//
bool loadSourcesList(const std::string &path, RepoSourceList &out,
                     std::string *errorMsg = nullptr);

// Download Packages indices for all sources+components into listsDir.
//
// For each RepoSource (with optional arch override / format-specific arch):
//
//   archToUse = (source.arch.empty() ? defaultArch : source.arch)
//               adjusted per repo format (e.g. Debian arm64 → Termux aarch64)
//   URL = <uri>/dists/<dist>/<component>/binary-<archToUse>/Packages
//
bool updateFromSourcesList(const std::string &sourcesPath,
                           const std::string &listsDir,
                           const std::string &defaultArch,
                           std::string *summaryMsg = nullptr,
                           std::string *errorMsg = nullptr,
                           RepoUpdateProgressCallback progressCb = {});

// Build RepoIndexList from sourcesPath + downloaded Packages in listsDir.
//
// - sourcesPath: file OR directory (same semantics as loadSourcesList)
// - defaultArch: fallback architecture (e.g. "arm64")
//
bool buildRepoIndices(const std::string &sourcesPath,
                      const std::string &listsDir,
                      const std::string &defaultArch, RepoIndexList &out,
                      std::string *errorMsg = nullptr);

// ---------------------------------------------------------
// Packages parsing
// ---------------------------------------------------------

bool parsePackagesFile(const std::string &path, PackageList &out,
                       std::string *errorMsg = nullptr);

bool parsePackagesString(const std::string &content, PackageList &out,
                         std::string *errorMsg = nullptr);

const PackageEntry *findPackage(const PackageList &list,
                                const std::string &name,
                                const std::string &arch = {});

} // namespace apm::repo
