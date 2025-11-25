/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: install_manager.cpp
 * Purpose: Implement package downloading, dependency resolution, and install/remove/upgrade workflows.
 * Last Modified: November 25th, 2025. - 11:35 AM Eastern Time.
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

#include "install_manager.hpp"

#include "config.hpp"
#include "control_parser.hpp"
#include "deb_extractor.hpp"
#include "dependency.hpp"
#include "downloader.hpp"
#include "export_path.hpp"
#include "fs.hpp"
#include "logger.hpp"
#include "repo_index.hpp"
#include "status_db.hpp"
#include "tar_extractor.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <fstream>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace apm::install {

using apm::repo::PackageEntry;
using apm::repo::PackageList;
using apm::repo::RepoIndexList;

// ---------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------

static std::string makeDebFileName(const PackageEntry &pkg) {
  // e.g. nano_7.2_arm64.deb
  std::string name = pkg.packageName;
  if (!pkg.version.empty()) {
    name += "_" + pkg.version;
  }
  if (!pkg.architecture.empty()) {
    name += "_" + pkg.architecture;
  }
  name += ".deb";
  return name;
}

static bool startsWith(const std::string &s, const std::string &prefix) {
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Determine which architecture should be used for resolver/installer logic.
// Honor any explicit repo overrides, fall back to the global default otherwise.
static std::string determineRepoArch(const RepoIndexList &repoIndices) {
  std::string detected;

  for (const auto &idx : repoIndices) {
    if (idx.arch.empty())
      continue;

    if (detected.empty()) {
      detected = idx.arch;
      continue;
    }

    if (detected != idx.arch) {
      apm::logger::warn(
          "determineRepoArch: multiple repo architectures detected (" +
          detected + " vs " + idx.arch +
          "); falling back to default: " + apm::config::DEFAULT_ARCH);
      return apm::config::DEFAULT_ARCH;
    }
  }

  if (!detected.empty())
    return detected;

  return apm::config::DEFAULT_ARCH;
}

// Ensure we have a .deb for this package in PKGS_DIR.
// Uses proper repo mapping:
//   - If existing .deb in PKGS_DIR, use that
//   - Else if Filename is full URL, download from it
//   - Else if repoUri is set, download repoUri + "/" + Filename
//   - Else ask user to place .deb manually
static bool ensureDebForPackage(const PackageEntry &pkg, std::string &debPath,
                                std::string *errorMsg,
                                const InstallProgressCallback &progressCb) {
  std::string debName = makeDebFileName(pkg);
  debPath = apm::fs::joinPath(apm::config::PKGS_DIR, debName);

  // Ensure PKGS_DIR exists
  if (!apm::fs::createDirs(apm::config::PKGS_DIR)) {
    if (errorMsg)
      *errorMsg =
          "Failed to create PKGS_DIR: " + std::string(apm::config::PKGS_DIR);
    apm::logger::error("ensureDebForPackage: cannot create PKGS_DIR");
    return false;
  }

  // If it's already present locally, we are done.
  if (apm::fs::pathExists(debPath)) {
    apm::logger::info("ensureDebForPackage: using existing local .deb: " +
                      debPath);
    return true;
  }

  if (pkg.filename.empty()) {
    std::string msg = "Package " + pkg.packageName +
                      " has no Filename field; please place .deb at " + debPath;
    if (errorMsg)
      *errorMsg = msg;
    apm::logger::warn("ensureDebForPackage: " + msg);
    return false;
  }

  // If user manually dropped a file with the filename basename, use it.
  std::string fname = pkg.filename;
  auto slashPos = fname.find_last_of('/');
  if (slashPos != std::string::npos) {
    fname = fname.substr(slashPos + 1);
  }

  std::string altLocalPath = apm::fs::joinPath(apm::config::PKGS_DIR, fname);
  if (apm::fs::pathExists(altLocalPath)) {
    debPath = altLocalPath;
    apm::logger::info("ensureDebForPackage: using local .deb: " + debPath);
    return true;
  }

  // Build URL
  std::string url;
  if (startsWith(pkg.filename, "http://") ||
      startsWith(pkg.filename, "https://")) {
    url = pkg.filename;
  } else if (!pkg.repoUri.empty()) {
    url = pkg.repoUri;
    if (!url.empty() && url.back() != '/') {
      url.push_back('/');
    }
    url += pkg.filename;
  } else {
    std::string msg = "Cannot construct download URL for package " +
                      pkg.packageName + "; Filename=" + pkg.filename +
                      ", repoUri is empty. Place the .deb manually at " +
                      debPath;
    if (errorMsg)
      *errorMsg = msg;
    apm::logger::warn("ensureDebForPackage: " + msg);
    return false;
  }

  apm::logger::info("ensureDebForPackage: downloading " + url + " -> " +
                    debPath);

  std::string dlErr;
  apm::net::TransferProgressCallback downloadProgress;
  if (progressCb) {
    const std::string pkgName = pkg.packageName;
    downloadProgress = [progressCb,
                        pkgName](const apm::net::TransferProgress &tp) {
      InstallProgress prog;
      prog.event = InstallProgressEvent::Download;
      prog.packageName = pkgName;
      prog.url = tp.url;
      prog.destination = tp.destination;
      prog.downloadedBytes = tp.downloadedBytes;
      prog.totalBytes = tp.downloadTotal;
      prog.uploadedBytes = tp.uploadedBytes;
      prog.uploadTotal = tp.uploadTotal;
      prog.downloadSpeedBytesPerSec = tp.downloadSpeedBytesPerSec;
      prog.uploadSpeedBytesPerSec = tp.uploadSpeedBytesPerSec;
      prog.finished = tp.finished;
      progressCb(prog);
    };
  }

  if (!apm::net::downloadFile(url, debPath, &dlErr, downloadProgress)) {
    std::string msg = "Download failed for " + url + ": " + dlErr;
    if (errorMsg)
      *errorMsg = msg;
    apm::logger::error("ensureDebForPackage: " + msg);
    return false;
  }

  return true;
}

// Recursively delete a directory tree (best-effort)
static void removeDirRecursive(const std::string &path) {
  DIR *dir = ::opendir(path.c_str());
  if (!dir) {
    ::unlink(path.c_str());
    return;
  }

  struct dirent *ent;
  while ((ent = ::readdir(dir)) != nullptr) {
    std::string name = ent->d_name;
    if (name == "." || name == "..")
      continue;

    std::string child = apm::fs::joinPath(path, name);

    struct stat st{};
    if (::lstat(child.c_str(), &st) != 0) {
      continue;
    }

    if (S_ISDIR(st.st_mode)) {
      removeDirRecursive(child);
      ::rmdir(child.c_str());
    } else {
      ::unlink(child.c_str());
    }
  }

  ::closedir(dir);
  ::rmdir(path.c_str());
}

static bool ensureTermuxRuntimeDirs() {
  bool ok = true;
  ok = apm::fs::createDirs(apm::config::TERMUX_ROOT) && ok;
  ok = apm::fs::createDirs(apm::config::TERMUX_PREFIX) && ok;
  ok = apm::fs::createDirs(apm::fs::joinPath(apm::config::TERMUX_PREFIX,
                                             "bin")) &&
       ok;
  ok = apm::fs::createDirs(apm::fs::joinPath(apm::config::TERMUX_PREFIX,
                                             "lib")) &&
       ok;
  ok = apm::fs::createDirs(apm::fs::joinPath(apm::config::TERMUX_PREFIX,
                                             "share")) &&
       ok;
  ok = apm::fs::createDirs(apm::config::TERMUX_HOME_DIR) && ok;
  ok = apm::fs::createDirs(apm::config::TERMUX_TMP_DIR) && ok;
  ok = apm::fs::createDirs(apm::config::APM_BIN_DIR) && ok;
  return ok;
}

static bool createTermuxEnvFileIfMissing() {
  if (apm::fs::pathExists(apm::config::TERMUX_ENV_FILE)) {
    return true;
  }

  std::ostringstream env;
  env << "export PREFIX=" << apm::config::TERMUX_PREFIX << "\n";
  env << "export HOME=" << apm::config::TERMUX_HOME_DIR << "\n";
  env << "export PATH=$PREFIX/bin:$PATH\n";
  env << "export LD_LIBRARY_PATH=$PREFIX/lib\n";
  env << "export TMPDIR=" << apm::config::TERMUX_TMP_DIR << "\n";
  env << "export TERM=xterm-256color\n";
  env << "export LANG=en_US.UTF-8\n";
  env << "export SHELL=/system/bin/sh\n";

  if (!apm::fs::writeFile(apm::config::TERMUX_ENV_FILE, env.str(), true)) {
    apm::logger::warn("install_manager: failed to write Termux env file");
    return false;
  }

  ::chmod(apm::config::TERMUX_ENV_FILE, 0644);
  return true;
}

static bool copyFilePreserveMode(const std::string &src,
                                 const std::string &dst) {
  std::ifstream in(src, std::ios::binary);
  if (!in.is_open()) {
    apm::logger::warn("install_manager: cannot open source file " + src);
    return false;
  }

  auto pos = dst.find_last_of('/');
  if (pos != std::string::npos) {
    std::string parent = dst.substr(0, pos);
    if (!parent.empty()) {
      apm::fs::createDirs(parent);
    }
  }

  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    apm::logger::warn("install_manager: cannot open destination file " + dst);
    return false;
  }

  out << in.rdbuf();
  if (!out.good()) {
    apm::logger::warn("install_manager: failed to copy file " + src);
    return false;
  }

  struct stat st{};
  if (::stat(src.c_str(), &st) == 0) {
    ::chmod(dst.c_str(), st.st_mode & 07777);
  }

  return true;
}

static bool copySymlink(const std::string &src, const std::string &dst) {
  std::vector<char> buf(4096, 0);
  ssize_t len = ::readlink(src.c_str(), buf.data(), buf.size() - 1);
  if (len < 0) {
    apm::logger::warn("install_manager: readlink failed for " + src);
    return false;
  }
  buf[static_cast<std::size_t>(len)] = '\0';

  auto pos = dst.find_last_of('/');
  if (pos != std::string::npos) {
    std::string parent = dst.substr(0, pos);
    if (!parent.empty()) {
      apm::fs::createDirs(parent);
    }
  }

  ::unlink(dst.c_str());
  if (::symlink(buf.data(), dst.c_str()) != 0) {
    apm::logger::warn("install_manager: symlink failed for " + dst);
    return false;
  }

  return true;
}

static bool copyTermuxTree(const std::string &srcDir, const std::string &dstDir,
                           const std::string &relativeBase,
                           std::vector<std::string> &installedPaths) {
  if (!apm::fs::createDirs(dstDir)) {
    apm::logger::warn("install_manager: failed to create Termux dest " +
                      dstDir);
    return false;
  }

  DIR *dir = ::opendir(srcDir.c_str());
  if (!dir) {
    apm::logger::warn("install_manager: cannot open Termux source " + srcDir);
    return false;
  }

  struct dirent *ent;
  while ((ent = ::readdir(dir)) != nullptr) {
    std::string name = ent->d_name;
    if (name == "." || name == "..")
      continue;

    std::string srcPath = apm::fs::joinPath(srcDir, name);
    std::string dstPath = apm::fs::joinPath(dstDir, name);
    std::string relPath =
        relativeBase.empty() ? name : relativeBase + "/" + name;

    struct stat st{};
    if (::lstat(srcPath.c_str(), &st) != 0) {
      continue;
    }

    if (S_ISDIR(st.st_mode)) {
      if (!copyTermuxTree(srcPath, dstPath, relPath, installedPaths)) {
        ::closedir(dir);
        return false;
      }
      continue;
    }

    bool ok = false;
    if (S_ISLNK(st.st_mode)) {
      ok = copySymlink(srcPath, dstPath);
    } else {
      ok = copyFilePreserveMode(srcPath, dstPath);
    }

    if (!ok) {
      ::closedir(dir);
      return false;
    }

    installedPaths.push_back(relPath);
  }

  ::closedir(dir);
  return true;
}

static bool writeTermuxManifest(const std::string &installRoot,
                                const std::vector<std::string> &paths) {
  std::ostringstream ss;
  for (const auto &p : paths) {
    ss << p << "\n";
  }
  return apm::fs::writeFile(apm::fs::joinPath(installRoot, "files.list"),
                            ss.str(), true);
}

static bool readTermuxManifest(const std::string &installRoot,
                               std::vector<std::string> &paths) {
  std::string content;
  if (!apm::fs::readFile(apm::fs::joinPath(installRoot, "files.list"),
                         content)) {
    return false;
  }

  std::istringstream in(content);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty())
      continue;
    paths.push_back(line);
  }
  return true;
}

static bool createTermuxWrapper(const std::string &commandName) {
  if (commandName.empty())
    return false;

  if (!apm::fs::createDirs(apm::config::APM_BIN_DIR)) {
    apm::logger::warn("install_manager: failed to create wrapper dir");
    return false;
  }

  std::string target =
      apm::fs::joinPath(apm::config::APM_BIN_DIR, commandName);
  std::ostringstream script;
  script << "#!/system/bin/sh\n";
  script << ". " << apm::config::TERMUX_ENV_FILE << "\n";
  script << "exec " << apm::config::TERMUX_PREFIX << "/bin/" << commandName
         << " \"$@\"\n";

  if (!apm::fs::writeFile(target, script.str())) {
    apm::logger::warn("install_manager: failed to write wrapper " + target);
    return false;
  }

  if (::chmod(target.c_str(), 0755) != 0) {
    apm::logger::warn("install_manager: chmod failed for wrapper " + target);
  }

  return true;
}

static bool removeTermuxWrapper(const std::string &commandName) {
  if (commandName.empty())
    return true;
  std::string target =
      apm::fs::joinPath(apm::config::APM_BIN_DIR, commandName);
  return apm::fs::removeFile(target);
}

static bool detectTermuxLayout(const std::string &dataDir) {
  std::string termuxUsr =
      apm::fs::joinPath(dataDir, "data/data/com.termux/files/usr");
  return apm::fs::isDirectory(termuxUsr);
}

static bool rewriteTermuxPathsDuringExtraction(
    const std::string &dataDir, const std::string &installRoot,
    std::vector<std::string> &installedPaths, std::string *errorMsg) {
  installedPaths.clear();

  std::string termuxUsr =
      apm::fs::joinPath(dataDir, "data/data/com.termux/files/usr");
  if (!apm::fs::isDirectory(termuxUsr)) {
    if (errorMsg) {
      *errorMsg = "Termux layout not found in package payload";
    }
    return false;
  }

  if (!ensureTermuxRuntimeDirs() || !createTermuxEnvFileIfMissing()) {
    if (errorMsg)
      *errorMsg = "Failed to prepare Termux runtime directories";
    return false;
  }

  apm::fs::removeDirRecursive(installRoot);
  if (!apm::fs::createDirs(installRoot)) {
    if (errorMsg)
      *errorMsg = "Failed to prepare Termux install root: " + installRoot;
    return false;
  }

  if (!copyTermuxTree(termuxUsr, apm::config::TERMUX_PREFIX, "usr",
                      installedPaths)) {
    if (errorMsg)
      *errorMsg = "Failed to rewrite Termux paths during extraction";
    return false;
  }

  if (!writeTermuxManifest(installRoot, installedPaths)) {
    if (errorMsg)
      *errorMsg = "Failed to write Termux manifest";
    return false;
  }

  return true;
}

static bool pruneEmptyDirs(const std::string &root, bool keepRoot = true) {
  DIR *dir = ::opendir(root.c_str());
  if (!dir) {
    return false;
  }

  bool empty = true;
  struct dirent *ent;
  while ((ent = ::readdir(dir)) != nullptr) {
    std::string name = ent->d_name;
    if (name == "." || name == "..")
      continue;

    std::string child = apm::fs::joinPath(root, name);
    struct stat st{};
    if (::lstat(child.c_str(), &st) != 0) {
      continue;
    }

    if (S_ISDIR(st.st_mode)) {
      bool childEmpty = pruneEmptyDirs(child, false);
      if (!childEmpty) {
        empty = false;
      }
    } else {
      empty = false;
    }
  }

  ::closedir(dir);

  if (empty && !keepRoot) {
    ::rmdir(root.c_str());
  }

  return empty;
}

static bool removeTermuxPackageFiles(const apm::status::InstalledPackage &ip,
                                     std::string *errorMsg) {
  std::vector<std::string> manifest;
  if (!readTermuxManifest(ip.installRoot, manifest)) {
    apm::logger::warn("removeTermuxPackageFiles: missing manifest at " +
                      ip.installRoot);
  }

  for (const auto &rel : manifest) {
    std::string full =
        apm::fs::joinPath(apm::config::TERMUX_ROOT, rel);
    apm::fs::removeFile(full);

    static const std::string kBinPrefix = "usr/bin/";
    if (rel.compare(0, kBinPrefix.size(), kBinPrefix) == 0) {
      std::string cmd = rel.substr(kBinPrefix.size());
      auto slash = cmd.find('/');
      if (slash == std::string::npos && !cmd.empty()) {
        removeTermuxWrapper(cmd);
      }
    }
  }

  pruneEmptyDirs(apm::fs::joinPath(apm::config::TERMUX_ROOT, "usr"), true);
  apm::fs::removeDirRecursive(ip.installRoot);

  if (errorMsg) {
    *errorMsg = "";
  }
  return true;
}

// Install a single package from a .deb into INSTALLED_DIR.
static bool installSinglePackage(const PackageEntry &pkg,
                                 const std::string &debPath,
                                 const InstallOptions &opts,
                                 const std::string &installRoot,
                                 std::string *errorMsg) {
  apm::logger::info("installSinglePackage: installing " + pkg.packageName +
                    " from " + debPath);

  std::string tmpRoot =
      apm::fs::joinPath(apm::config::CACHE_DIR,
                        "apm-install-" + pkg.packageName + "-" + pkg.version);

  if (!apm::fs::createDirs(tmpRoot)) {
    if (errorMsg)
      *errorMsg = "Failed to create temp directory: " + tmpRoot;
    apm::logger::error("installSinglePackage: cannot create " + tmpRoot);
    return false;
  }

  std::string workDir = apm::fs::joinPath(tmpRoot, "work");
  if (!apm::fs::createDirs(workDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create work directory: " + workDir;
    apm::logger::error("installSinglePackage: cannot create " + workDir);
    removeDirRecursive(tmpRoot);
    return false;
  }

  // Extract .deb
  apm::deb::DebParts parts;
  std::string err;
  if (!apm::deb::extractDebArchive(debPath, workDir, parts, &err)) {
    if (errorMsg)
      *errorMsg = "extractDebArchive failed: " + err;
    apm::logger::error("installSinglePackage: extractDebArchive failed: " +
                       err);
    removeDirRecursive(tmpRoot);
    return false;
  }

  // Extract control and data tars
  std::string controlDir = apm::fs::joinPath(workDir, "control");
  std::string dataDir = apm::fs::joinPath(workDir, "data");

  if (!apm::fs::createDirs(controlDir) || !apm::fs::createDirs(dataDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create control/data dirs in " + workDir;
    apm::logger::error("installSinglePackage: cannot create control/data dirs");
    removeDirRecursive(tmpRoot);
    return false;
  }

  if (!parts.controlTarPath.empty()) {
    if (!apm::tar::extractTar(parts.controlTarPath, controlDir, &err)) {
      if (errorMsg)
        *errorMsg = "Failed to extract control tar: " + err;
      apm::logger::error("installSinglePackage: control tar extract failed: " +
                         err);
      removeDirRecursive(tmpRoot);
      return false;
    }
  }

  if (!parts.dataTarPath.empty()) {
    if (!apm::tar::extractTar(parts.dataTarPath, dataDir, &err)) {
      if (errorMsg)
        *errorMsg = "Failed to extract data tar: " + err;
      apm::logger::error("installSinglePackage: data tar extract failed: " +
                         err);
      removeDirRecursive(tmpRoot);
      return false;
    }
  }

  // Parse control file (sanity check, non-fatal)
  std::string controlPath = apm::fs::joinPath(controlDir, "control");
  auto cf = apm::control::parseControlFile(controlPath);
  if (!cf.packageName.empty() && cf.packageName != pkg.packageName) {
    apm::logger::warn("installSinglePackage: control file package name (" +
                      cf.packageName + ") != repo package name (" +
                      pkg.packageName + ")");
  }

  if (!apm::fs::createDirs(apm::config::INSTALLED_DIR)) {
    if (errorMsg)
      *errorMsg = "Failed to create INSTALLED_DIR: " +
                  std::string(apm::config::INSTALLED_DIR);
    apm::logger::error("installSinglePackage: cannot create INSTALLED_DIR");
    removeDirRecursive(tmpRoot);
    return false;
  }

  auto lastSlash = installRoot.find_last_of('/');
  if (lastSlash != std::string::npos) {
    std::string parent = installRoot.substr(0, lastSlash);
    if (!apm::fs::createDirs(parent)) {
      if (errorMsg)
        *errorMsg = "Failed to create install parent: " + parent;
      apm::logger::error("installSinglePackage: cannot create parent dir: " +
                         parent);
      removeDirRecursive(tmpRoot);
      return false;
    }
  }

  if (apm::fs::pathExists(installRoot)) {
    if (!opts.reinstall) {
      if (errorMsg)
        *errorMsg = "Install root already exists: " + installRoot;
      apm::logger::error("installSinglePackage: install root exists: " +
                         installRoot);
      removeDirRecursive(tmpRoot);
      return false;
    }
    apm::fs::removeDirRecursive(installRoot);
  }

  const bool termuxLayout = detectTermuxLayout(dataDir);
  const bool termuxMode = opts.isTermuxPackage || termuxLayout;

  if (termuxMode) {
    if (!termuxLayout) {
      if (errorMsg)
        *errorMsg = "Termux package flag set but layout not detected";
      apm::logger::error(
          "installSinglePackage: Termux flag set but no Termux layout found");
      removeDirRecursive(tmpRoot);
      return false;
    }

    std::vector<std::string> installedPaths;
    if (!rewriteTermuxPathsDuringExtraction(dataDir, installRoot,
                                            installedPaths, errorMsg)) {
      if (errorMsg && errorMsg->empty()) {
        *errorMsg = "Failed to install Termux payload";
      }
      removeDirRecursive(tmpRoot);
      return false;
    }

    for (const auto &rel : installedPaths) {
      static const std::string kBinPrefix = "usr/bin/";
      if (rel.compare(0, kBinPrefix.size(), kBinPrefix) != 0) {
        continue;
      }
      std::string cmd = rel.substr(kBinPrefix.size());
      auto slashPos = cmd.find('/');
      if (slashPos != std::string::npos || cmd.empty())
        continue;
      createTermuxWrapper(cmd);
    }

    removeDirRecursive(tmpRoot);
    apm::logger::info("installSinglePackage: installed Termux payload for " +
                      pkg.packageName + " into " +
                      std::string(apm::config::TERMUX_PREFIX));
    return true;
  }

  if (::rename(dataDir.c_str(), installRoot.c_str()) < 0) {
    std::string msg = "Failed to move data dir " + dataDir + " -> " +
                      installRoot + ": " + std::strerror(errno);
    if (errorMsg)
      *errorMsg = msg;
    apm::logger::error("installSinglePackage: " + msg);
    removeDirRecursive(tmpRoot);
    return false;
  }

  removeDirRecursive(tmpRoot);

  apm::logger::info("installSinglePackage: installed " + pkg.packageName +
                    " to " + installRoot);
  return true;
}

// Compare version strings in a natural-ish way (digits vs digits, text vs
// text). Not a full dpkg implementation, but good enough for most SemVer-ish
// versions.
static int compareVersionSimple(const std::string &a, const std::string &b) {
  std::size_t i = 0, j = 0;
  const std::size_t na = a.size();
  const std::size_t nb = b.size();

  while (i < na || j < nb) {
    // If one string ended, shorter is "smaller"
    if (i >= na)
      return -1;
    if (j >= nb)
      return 1;

    // Decide chunk type
    bool aIsDigit = std::isdigit(static_cast<unsigned char>(a[i])) != 0;
    bool bIsDigit = std::isdigit(static_cast<unsigned char>(b[j])) != 0;

    // Extract chunk from a
    std::size_t iStart = i;
    while (i < na &&
           (std::isdigit(static_cast<unsigned char>(a[i])) != 0) == aIsDigit) {
      ++i;
    }
    std::string aChunk = a.substr(iStart, i - iStart);

    // Extract chunk from b
    std::size_t jStart = j;
    while (j < nb &&
           (std::isdigit(static_cast<unsigned char>(b[j])) != 0) == bIsDigit) {
      ++j;
    }
    std::string bChunk = b.substr(jStart, j - jStart);

    if (aIsDigit && bIsDigit) {
      // Strip leading zeros
      auto stripZeros = [](const std::string &s) -> std::string {
        std::size_t pos = 0;
        while (pos < s.size() && s[pos] == '0')
          ++pos;
        std::string out = s.substr(pos);
        return out.empty() ? "0" : out;
      };

      std::string an = stripZeros(aChunk);
      std::string bn = stripZeros(bChunk);

      if (an.size() < bn.size())
        return -1;
      if (an.size() > bn.size())
        return 1;
      int cmp = an.compare(bn);
      if (cmp != 0)
        return (cmp < 0) ? -1 : 1;
    } else if (aIsDigit != bIsDigit) {
      // Arbitrary but stable: treat digit run > non-digit
      return aIsDigit ? 1 : -1;
    } else {
      int cmp = aChunk.compare(bChunk);
      if (cmp != 0)
        return (cmp < 0) ? -1 : 1;
    }
  }

  return 0;
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

// Resolve dependencies, download payloads, and apply packages in order. The
// optional progress callback receives download bytes as we fetch artifacts.
bool installWithDeps(const RepoIndexList &repoIndices,
                     const std::string &rootPackage, const InstallOptions &opts,
                     InstallResult &result,
                     InstallProgressCallback progressCb) {
  result = InstallResult{};

  if (rootPackage.empty()) {
    result.ok = false;
    result.message = "Root package name is empty";
    apm::logger::error("installWithDeps: root package is empty");
    return false;
  }

  const std::string arch = determineRepoArch(repoIndices);

  // Merge all repo indices into a single PackageList view for the resolver.
  PackageList repoPkgs;
  std::size_t totalPkgs = 0;
  for (const auto &idx : repoIndices) {
    totalPkgs += idx.packages.size();
  }
  repoPkgs.reserve(totalPkgs);

  for (const auto &idx : repoIndices) {
    bool idxTermux =
        idx.source.isTermuxRepo || idx.source.format == apm::repo::RepoFormat::Termux;
    for (const auto &pkg : idx.packages) {
      PackageEntry copy = pkg;
      if (idxTermux) {
        copy.isTermuxPackage = true;
      }
      repoPkgs.push_back(copy);
    }
  }

  int rootDebIndex = -1;
  int rootTermuxIndex = -1;
  for (std::size_t i = 0; i < repoPkgs.size(); ++i) {
    if (repoPkgs[i].packageName != rootPackage)
      continue;
    if (repoPkgs[i].isTermuxPackage) {
      if (rootTermuxIndex < 0)
        rootTermuxIndex = static_cast<int>(i);
    } else {
      if (rootDebIndex < 0)
        rootDebIndex = static_cast<int>(i);
    }
  }

  const apm::repo::PackageEntry *rootDeb =
      (rootDebIndex >= 0) ? &repoPkgs[static_cast<std::size_t>(rootDebIndex)]
                          : nullptr;
  const apm::repo::PackageEntry *rootTermux =
      (rootTermuxIndex >= 0)
          ? &repoPkgs[static_cast<std::size_t>(rootTermuxIndex)]
          : nullptr;

  bool termuxMode = opts.isTermuxPackage;
  if (!termuxMode && rootTermux && !rootDeb) {
    termuxMode = true;
  }

  if (!(termuxMode ? rootTermux : rootDeb)) {
    result.ok = false;
    result.message = "Package not found in " +
                     std::string(termuxMode ? "Termux" : "Debian") +
                     " repositories: " + rootPackage;
    apm::logger::error("installWithDeps: " + result.message);
    return false;
  }

  PackageList resolverPkgs;
  resolverPkgs.reserve(repoPkgs.size());
  for (const auto &pkg : repoPkgs) {
    if (termuxMode) {
      if (pkg.isTermuxPackage)
        resolverPkgs.push_back(pkg);
    } else {
      if (!pkg.isTermuxPackage)
        resolverPkgs.push_back(pkg);
    }
  }

  // Load current installed DB and prepare alreadyInstalled list for resolver.
  apm::status::InstalledDb installedDb;
  std::string dbErr;
  if (!apm::status::loadStatus(installedDb, &dbErr)) {
    apm::logger::warn("installWithDeps: failed to load status DB: " + dbErr);
    // continue with empty DB; worst case, resolver thinks nothing is installed
  }

  std::vector<std::string> alreadyInstalled;
  alreadyInstalled.reserve(installedDb.size());
  for (const auto &kv : installedDb) {
    if (kv.second.termuxPackage == termuxMode) {
      alreadyInstalled.push_back(kv.first);
    }
  }

  apm::dep::ResolutionResult res;
  std::string err;

  InstallOptions effectiveOpts = opts;
  effectiveOpts.isTermuxPackage = termuxMode;

  if (termuxMode) {
    if (!apm::dep::resolveTermuxDependencies(resolverPkgs, rootPackage, res,
                                             alreadyInstalled, &err)) {
      result.ok = false;
      result.message = "Dependency resolution failed: " + err;
      apm::logger::error("installWithDeps: " + result.message);
      return false;
    }
  } else {
    if (!apm::dep::resolveDependencies(resolverPkgs, rootPackage, arch, res,
                                       alreadyInstalled, &err)) {
      result.ok = false;
      result.message = "Dependency resolution failed: " + err;
      apm::logger::error("installWithDeps: " + result.message);
      return false;
    }
  }

  if (opts.simulate) {
    for (const auto *pkg : res.installOrder) {
      if (!pkg)
        continue;
      result.installedPackages.push_back(pkg->packageName);
    }

    std::ostringstream ss;
    ss << "Simulated install of " << rootPackage << ". Plan:";
    if (result.installedPackages.empty()) {
      ss << " (nothing)";
    } else {
      for (const auto &name : result.installedPackages) {
        ss << " " << name;
      }
    }
    result.ok = true;
    result.message = ss.str();
    apm::logger::info("installWithDeps (simulate): " + result.message);
    return true;
  }

  // Real install
  for (const auto *pkg : res.installOrder) {
    if (!pkg)
      continue;

    auto existing = installedDb.find(pkg->packageName);
    if (!effectiveOpts.reinstall && existing != installedDb.end() &&
        existing->second.termuxPackage == termuxMode) {
      apm::logger::info("installWithDeps: skipping already installed " +
                        pkg->packageName);
      result.skippedPackages.push_back(pkg->packageName);
      continue;
    }
    if (effectiveOpts.reinstall && existing != installedDb.end() &&
        termuxMode && existing->second.termuxPackage) {
      removeTermuxPackageFiles(existing->second, nullptr);
    }

    std::string debPath;
    if (!ensureDebForPackage(*pkg, debPath, &err, progressCb)) {
      result.ok = false;
      result.message =
          "Failed to ensure .deb for " + pkg->packageName + ": " + err;
      apm::logger::error("installWithDeps: " + result.message);
      return false;
    }

    // ---------------------------------------------------------
    // SHA256 verification (strict mode)
    // ---------------------------------------------------------
    // SHA256 for .deb disabled (dev mode)
    apm::logger::warn(
        "SHA256 verification DISABLED (dev mode): accepting .deb " + debPath);

    bool installAsDependency = (pkg->packageName != rootPackage);
    std::string installRoot;
    if (termuxMode) {
      installRoot =
          apm::fs::joinPath(apm::config::TERMUX_INSTALLED_DIR, pkg->packageName);
    } else {
      const char *baseDir = installAsDependency
                                ? apm::config::DEPENDENCIES_DIR
                                : apm::config::COMMANDS_DIR;
      installRoot = apm::fs::joinPath(baseDir, pkg->packageName);
    }

    if (!installSinglePackage(*pkg, debPath, effectiveOpts, installRoot,
                              &err)) {
      result.ok = false;
      result.message = "Failed to install " + pkg->packageName + ": " + err;
      apm::logger::error("installWithDeps: " + result.message);
      return false;
    }

    // Update status DB in-memory
    apm::status::InstalledPackage ip;
    ip.name = pkg->packageName;
    ip.version = pkg->version;
    ip.architecture = pkg->architecture;
    ip.status = "install ok installed";
    ip.installRoot = installRoot;
    ip.repoUri = pkg->repoUri;
    ip.repoDist = pkg->repoDist;
    ip.repoComponent = pkg->repoComponent;
    ip.depends =
        pkg->depends; // store dependencies for reverse-dep + autoremove
    ip.termuxPackage = termuxMode;
    if (termuxMode) {
      ip.installPrefix = apm::config::TERMUX_PREFIX;
    }

    // Auto-Installed flag:
    //
    // - New root package of this install => manual (autoInstalled=false)
    // - New dependencies => autoInstalled=true
    // - If something was already installed, keep its current flag,
    //   but if the user explicitly installed it as the root, flip to manual.
    auto existing_auto_installed = installedDb.find(pkg->packageName);

    if (existing_auto_installed != installedDb.end()) {
      // Keep prior autoInstalled flag
      ip.autoInstalled = existing_auto_installed->second.autoInstalled;

      // If user explicitly installed this as the root, it's not auto
      if (pkg->packageName == rootPackage) {
        ip.autoInstalled = false;
      }

    } else {
      // New install:
      // - root package = manual
      // - anything else = auto-installed
      ip.autoInstalled = (pkg->packageName != rootPackage);
    }

    // Save into DB
    installedDb[ip.name] = ip;

    // Flush to disk (dpkg-style)
    std::string writeErr;
    if (!apm::status::writeStatus(installedDb, &writeErr)) {
      apm::logger::warn("installWithDeps: failed to update status DB: " +
                        writeErr);
    }

    result.installedPackages.push_back(pkg->packageName);
  }

  apm::daemon::path::refreshPathEnvironment();

  std::ostringstream ss;
  ss << "Installed " << result.installedPackages.size() << " package(s)";
  if (!result.installedPackages.empty()) {
    ss << ":";
    for (const auto &name : result.installedPackages) {
      ss << " " << name;
    }
  }

  result.ok = true;
  result.message = ss.str();
  apm::logger::info("installWithDeps: " + result.message);
  return true;
}

// ---------------------------------------------------------------------
// Public removal API
// ---------------------------------------------------------------------

// Remove an installed package, optionally purging metadata or forcing removal
// even when reverse dependencies exist.
bool removePackage(const std::string &packageName, const RemoveOptions &opts,
                   RemoveResult &result) {
  (void)opts; // currently unused, reserved for purge/etc.

  result = RemoveResult{};
  if (packageName.empty()) {
    result.ok = false;
    result.message = "Package name is empty";
    apm::logger::error("removePackage: package name is empty");
    return false;
  }

  apm::status::InstalledDb db;
  std::string dbErr;
  if (!apm::status::loadStatus(db, &dbErr)) {
    result.ok = false;
    result.message = "Failed to load status DB: " + dbErr;
    apm::logger::error("removePackage: " + result.message);
    return false;
  }

  auto it = db.find(packageName);
  if (it == db.end()) {
    result.ok = true;
    result.message = "Package '" + packageName + "' is not installed";
    apm::logger::info("removePackage: " + result.message);
    return true;
  }

  // Reverse dependency protection: refuse to remove if other installed
  // packages depend on this one, unless opts.force is true.
  if (!opts.force) {
    std::vector<std::string> dependents;

    for (const auto &kv : db) {
      const std::string &otherName = kv.first;
      const apm::status::InstalledPackage &otherPkg = kv.second;

      if (otherName == packageName) {
        continue;
      }
      if (otherPkg.termuxPackage != it->second.termuxPackage) {
        continue;
      }

      for (const auto &depName : otherPkg.depends) {
        if (depName == packageName) {
          dependents.push_back(otherName);
          break;
        }
      }
    }

    if (!dependents.empty()) {
      std::ostringstream oss;
      oss << "Cannot remove '" << packageName << "': required by ";

      std::size_t maxShow = 10;
      for (std::size_t i = 0; i < dependents.size() && i < maxShow; ++i) {
        if (i > 0)
          oss << ", ";
        oss << dependents[i];
      }
      if (dependents.size() > maxShow) {
        oss << " and " << (dependents.size() - maxShow) << " more";
      }

      result.ok = false;
      result.message = oss.str();
      apm::logger::warn("removePackage: " + result.message);
      return false;
    }
  }

  const apm::status::InstalledPackage &ip = it->second;

  std::string installRoot = ip.installRoot;
  if (installRoot.empty()) {
    if (ip.termuxPackage) {
      installRoot =
          apm::fs::joinPath(apm::config::TERMUX_INSTALLED_DIR, packageName);
    } else {
      installRoot = apm::fs::joinPath(apm::config::INSTALLED_DIR, packageName);
    }
  }

  apm::logger::info("removePackage: removing package '" + packageName +
                    "' from " + installRoot);

  if (ip.termuxPackage) {
    std::string removeErr;
    if (!removeTermuxPackageFiles(ip, &removeErr)) {
      result.ok = false;
      result.message = removeErr.empty()
                           ? "Failed to remove Termux package payload"
                           : removeErr;
      apm::logger::error("removePackage: " + result.message);
      return false;
    }
  } else {
    // Remove installed root directory tree
    if (apm::fs::pathExists(installRoot)) {
      removeDirRecursive(installRoot);
    } else {
      apm::logger::warn("removePackage: installRoot does not exist: " +
                        installRoot);
    }
  }

  // Remove from status DB
  db.erase(it);
  std::string writeErr;
  if (!apm::status::writeStatus(db, &writeErr)) {
    apm::logger::warn("removePackage: failed to update status DB: " + writeErr);
    // Non-fatal: files are gone, DB just slightly out of sync
  }

  apm::daemon::path::refreshPathEnvironment();

  result.ok = true;
  result.removedPackages.push_back(packageName);
  result.message = "Removed package: " + packageName;

  apm::logger::info("removePackage: " + result.message);
  return true;
  apm::daemon::path::refreshPathEnvironment();

  result.ok = true;
  result.removedPackages.push_back(packageName);
  result.message = "Removed package: " + packageName;

  apm::logger::info("removePackage: " + result.message);
  return true;
}

// ---------------------------------------------------------------------
// Upgrade
// ---------------------------------------------------------------------

// Rebuild a target upgrade set (either provided or inferred) and reuse
// installWithDeps to perform each upgrade with dependency resolution.
bool upgradePackages(const apm::repo::RepoIndexList &repoIndices,
                     const std::vector<std::string> &targets,
                     const UpgradeOptions &opts, UpgradeResult &result) {
  result = UpgradeResult{};

  if (repoIndices.empty()) {
    result.ok = false;
    result.message = "No repository indices loaded (run 'apm update')";
    apm::logger::error("upgradePackages: " + result.message);
    return false;
  }

  const std::string arch = determineRepoArch(repoIndices);

  // Load current installed DB
  apm::status::InstalledDb db;
  std::string dbErr;
  if (!apm::status::loadStatus(db, &dbErr)) {
    result.ok = false;
    result.message = "Failed to load status DB: " + dbErr;
    apm::logger::error("upgradePackages: " + result.message);
    return false;
  }

  // Build list of packages to consider
  std::vector<std::string> toCheck;

  if (!targets.empty()) {
    // Partial upgrade: only these packages
    for (const auto &name : targets) {
      auto it = db.find(name);
      if (it == db.end()) {
        result.skippedPackages.push_back(name + " (not installed)");
        continue;
      }
      toCheck.push_back(name);
    }
  } else {
    // Full upgrade: all installed packages
    toCheck.reserve(db.size());
    for (const auto &kv : db) {
      toCheck.push_back(kv.first);
    }
  }

  if (toCheck.empty()) {
    if (targets.empty()) {
      result.ok = true;
      result.message = "No installed packages to upgrade";
    } else {
      result.ok = true;
      result.message = "No matching installed packages to upgrade";
    }
    return true;
  }

  // Helper to find candidate package entry
  auto findCandidate = [&](const std::string &name,
                           bool termux) -> const apm::repo::PackageEntry * {
    for (const auto &idx : repoIndices) {
      bool idxTermux =
          idx.source.isTermuxRepo ||
          idx.source.format == apm::repo::RepoFormat::Termux;
      if (idxTermux != termux) {
        continue;
      }

      if (termux) {
        for (const auto &pkg : idx.packages) {
          if (!pkg.isTermuxPackage)
            continue;
          if (pkg.packageName == name)
            return &pkg;
        }
      } else {
        const auto *p = apm::repo::findPackage(idx.packages, name, arch);
        if (!p) {
          p = apm::repo::findPackage(idx.packages, name, "all");
        }
        if (p)
          return p;
      }
    }
    return nullptr;
  };

  // Iterate and upgrade
  for (const auto &name : toCheck) {
    auto it = db.find(name);
    if (it == db.end()) {
      // should not happen, but be safe
      result.skippedPackages.push_back(name + " (not installed anymore)");
      continue;
    }

    const auto &installedPkg = it->second;
    const auto *candidate = findCandidate(name, installedPkg.termuxPackage);

    if (!candidate) {
      result.skippedPackages.push_back(name + " (no candidate in repos)");
      continue;
    }

    // Compare versions
    if (!installedPkg.version.empty()) {
      int cmp = compareVersionSimple(candidate->version, installedPkg.version);
      if (cmp <= 0) {
        // candidate <= installed => nothing to upgrade
        result.skippedPackages.push_back(name + " (up-to-date)");
        continue;
      }
    }

    if (opts.simulate) {
      result.upgradedPackages.push_back(name);
      continue;
    }

    // Perform the actual upgrade via installWithDeps.
    InstallOptions iopts;
    iopts.simulate = false;
    iopts.reinstall = true;
    iopts.isTermuxPackage = installedPkg.termuxPackage;

    InstallResult ires;
    if (!installWithDeps(repoIndices, name, iopts, ires)) {
      result.ok = false;
      result.message = "Failed to upgrade " + name + ": " + ires.message;
      apm::logger::error("upgradePackages: " + result.message);
      return false;
    }

    // Record upgraded root package; dependencies will come along implicitly
    result.upgradedPackages.push_back(name);
  }

  result.ok = true;

  if (result.upgradedPackages.empty()) {
    result.message = "No packages were upgraded";
  } else {
    std::ostringstream ss;
    ss << "Upgraded " << result.upgradedPackages.size() << " package(s)";
    result.message = ss.str();
  }

  return true;
}

} // namespace apm::install
