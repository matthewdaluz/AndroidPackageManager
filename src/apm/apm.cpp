/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: apm.cpp
 * Purpose: Implement the apm CLI, including local commands and IPC-backed
 * operations.
 * Last Modified: November 27th, 2025. - 8:10 AM Eastern Time.
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
#include "control_parser.hpp"
#include "deb_extractor.hpp"
#include "export_path.hpp"
#include "fs.hpp"
#include "gpg_verify.hpp"
#include "ipc_client.hpp"
#include "logger.hpp"
#include "manual_package.hpp"
#include "repo_index.hpp"
#include "search.hpp"
#include "security.hpp"
#include "status_db.hpp"
#include "tar_extractor.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

// Editable CLI metadata.
static constexpr const char *kApmVersion = "1.4.0b - Closed Beta";
static constexpr const char *kApmBuildDate = "November 27th, 2025 - 8:10 AM ET";

// -----------------------------------------------------------------------------
// Progress formatting + helper utilities
// -----------------------------------------------------------------------------

static std::string humanReadableBytes(double bytes) {
  static const char *kUnits[] = {"B", "KB", "MB", "GB", "TB"};
  size_t unitIdx = 0;
  constexpr size_t kMaxUnit = 4;
  while (bytes >= 1024.0 && unitIdx < kMaxUnit) {
    bytes /= 1024.0;
    ++unitIdx;
  }

  std::ostringstream ss;
  if (unitIdx == 0)
    ss << static_cast<std::uint64_t>(bytes);
  else
    ss << std::fixed << std::setprecision(bytes >= 10.0 ? 0 : 1) << bytes;
  ss << kUnits[unitIdx];
  return ss.str();
}

static std::string formatBytes(std::uint64_t bytes) {
  return humanReadableBytes(static_cast<double>(bytes));
}

static std::string formatSpeed(double bytesPerSec) {
  if (bytesPerSec <= 0.0)
    return "0B/s";
  return humanReadableBytes(bytesPerSec) + "/s";
}

// Parse unsigned integers safely without exceptions (Android builds disable
// C++ exceptions).
static std::uint64_t parseUintSafe(const std::string &value) {
  if (value.empty())
    return 0;

  errno = 0;
  char *end = nullptr;
  unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
  if (errno != 0 || end == value.c_str())
    return 0;
  return static_cast<std::uint64_t>(parsed);
}

// Parse doubles safely without exceptions.
static double parseDoubleSafe(const std::string &value) {
  if (value.empty())
    return 0.0;

  errno = 0;
  char *end = nullptr;
  double parsed = std::strtod(value.c_str(), &end);
  if (errno != 0 || end == value.c_str())
    return 0.0;
  return parsed;
}

static std::string buildProgressBar(double ratio) {
  const int width = 30;
  if (ratio < 0.0)
    ratio = 0.0;
  if (ratio > 1.0)
    ratio = 1.0;

  int filled = static_cast<int>(ratio * width);
  if (filled > width)
    filled = width;

  std::string bar = "[";
  for (int i = 0; i < width; ++i) {
    if (i < filled)
      bar.push_back('=');
    else if (i == filled)
      bar.push_back('>');
    else
      bar.push_back(' ');
  }
  bar.push_back(']');
  int percent = static_cast<int>(ratio * 100.0);
  if (percent > 100)
    percent = 100;
  bar += " ";
  bar += std::to_string(percent);
  bar += "%";
  return bar;
}

// Manual package helpers make it possible to install standalone archives
// without hitting the daemon or repo metadata.
enum class ManualArchiveType { Deb, Tarball };

// RAII helper that deletes temp directories created for unpacking manual
// artifacts when the guard leaves scope.
struct ScopedTempDir {
  explicit ScopedTempDir(std::string dir) : path(std::move(dir)) {}
  ~ScopedTempDir() {
    if (!path.empty())
      apm::fs::removeDirRecursive(path);
  }
  void reset() { path.clear(); }
  std::string path;
};

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static bool endsWithIgnoreCase(const std::string &value,
                               const std::string &suffix) {
  if (suffix.size() > value.size())
    return false;
  std::size_t offset = value.size() - suffix.size();
  for (std::size_t i = 0; i < suffix.size(); ++i) {
    char vc = value[offset + i];
    char sc = suffix[i];
    if (std::tolower(static_cast<unsigned char>(vc)) !=
        std::tolower(static_cast<unsigned char>(sc))) {
      return false;
    }
  }
  return true;
}

static bool detectManualArchiveType(const std::string &path,
                                    ManualArchiveType &type,
                                    std::string *errorMsg) {
  std::string lower = toLower(path);
  if (endsWithIgnoreCase(lower, ".deb")) {
    type = ManualArchiveType::Deb;
    return true;
  }

  static const char *kTarSuffixes[] = {".tar.gz", ".tgz", ".tar.xz", ".txz",
                                       ".tar",    ".gz",  ".xz"};
  for (const char *suffix : kTarSuffixes) {
    if (endsWithIgnoreCase(lower, suffix)) {
      type = ManualArchiveType::Tarball;
      return true;
    }
  }

  if (errorMsg)
    *errorMsg = "Unsupported file extension for package-install: " + path;
  return false;
}

static std::string createTempDir(const std::string &tag,
                                 std::string *errorMsg) {
  static std::uint64_t counter = 0;
  auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  std::ostringstream name;
  name << "manual-" << tag << "-" << now << "-" << (++counter);
  std::string tempPath = apm::fs::joinPath(apm::config::CACHE_DIR, name.str());
  if (!apm::fs::createDirs(tempPath)) {
    if (errorMsg)
      *errorMsg = "Failed to create temp directory: " + tempPath;
    return {};
  }
  return tempPath;
}

static bool ensureParentDirectory(const std::string &path,
                                  std::string *errorMsg) {
  auto pos = path.find_last_of('/');
  if (pos == std::string::npos)
    return true;
  std::string parent = path.substr(0, pos);
  if (parent.empty())
    return true;
  if (!apm::fs::createDirs(parent)) {
    if (errorMsg)
      *errorMsg = "Failed to create directory: " + parent;
    return false;
  }
  return true;
}

static std::uint64_t currentUnixTimestamp() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<seconds>(system_clock::now().time_since_epoch()).count());
}

// -----------------------------------------------------------------------------
// Session + authentication helpers
// -----------------------------------------------------------------------------

static bool loadActiveSessionToken(std::string &tokenOut, bool &hadSession) {
  hadSession = false;
  apm::security::SessionState state;
  if (!apm::security::loadSession(state, nullptr))
    return false;

  hadSession = true;
  if (apm::security::isSessionExpired(state,
                                      apm::security::currentUnixSeconds()))
    return false;

  tokenOut = state.token;
  return true;
}

static bool promptForSecret(const std::string &prompt, std::string &secretOut) {
  std::cout << prompt << std::flush;
  if (!std::getline(std::cin, secretOut))
    return false;
  return !secretOut.empty();
}

static bool promptForNewSecret(std::string &secretOut) {
  std::string first;
  if (!promptForSecret("Set a new APM password/PIN: ", first))
    return false;

  std::string confirm;
  if (!promptForSecret("Confirm password/PIN: ", confirm))
    return false;

  if (first != confirm) {
    std::cerr << "Entries did not match. Please try again.\n";
    return false;
  }

  secretOut = first;
  return true;
}

static bool requestSessionUnlock(const std::string &socketPath,
                                 const std::string &action,
                                 const std::string &secret,
                                 std::string &sessionTokenOut) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Authenticate;
  req.id = "authenticate-1";
  req.authAction = action;
  req.authSecret = secret;

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequest(req, resp, socketPath, &err)) {
    std::cerr << "Error: " << err << "\n";
    return false;
  }

  if (!resp.success) {
    std::cerr << (resp.message.empty() ? "Authentication failed" : resp.message)
              << "\n";
    return false;
  }

  if (!resp.message.empty())
    std::cout << resp.message << "\n";

  auto itToken = resp.rawFields.find("session_token");
  if (itToken != resp.rawFields.end()) {
    sessionTokenOut = itToken->second;
    return true;
  }

  bool hadSession = false;
  if (loadActiveSessionToken(sessionTokenOut, hadSession))
    return true;

  std::cerr << "Authentication succeeded but no active session token was found."
            << "\n";
  return false;
}

static bool ensureAuthenticatedSession(const std::string &socketPath,
                                       std::string &sessionTokenOut) {
  bool hadSession = false;
  if (loadActiveSessionToken(sessionTokenOut, hadSession))
    return true;

  if (hadSession)
    std::cout << "APM security session expired. Please re-authenticate.\n";

  const bool hasPasspin = apm::fs::isFile(apm::config::PASS_PIN_FILE);

  std::string secret;
  if (hasPasspin) {
    if (!promptForSecret("Enter APM password/PIN: ", secret)) {
      std::cerr << "Failed to read password/PIN input.\n";
      return false;
    }
    return requestSessionUnlock(socketPath, "unlock", secret, sessionTokenOut);
  }

  if (!promptForNewSecret(secret)) {
    std::cerr << "Password/PIN setup aborted.\n";
    return false;
  }

  return requestSessionUnlock(socketPath, "set", secret, sessionTokenOut);
}

static void attachSession(apm::ipc::Request &req,
                          const std::string &sessionToken) {
  if (!sessionToken.empty())
    req.sessionToken = sessionToken;
}

static bool requiresAuthSession(const std::string &cmd) {
  return cmd == "update" || cmd == "install" || cmd == "remove" ||
         cmd == "upgrade" || cmd == "autoremove" || cmd == "module-install" ||
         cmd == "module-list" || cmd == "module-enable" ||
         cmd == "module-disable" || cmd == "module-remove" ||
         cmd == "apk-install" || cmd == "apk-uninstall";
}

static bool ensureManualSlotAvailable(const std::string &pkgName,
                                      std::string *errorMsg) {
  if (pkgName.empty()) {
    if (errorMsg)
      *errorMsg = "Package name is empty";
    return false;
  }
  if (apm::manual::isInstalled(pkgName)) {
    if (errorMsg)
      *errorMsg = "Manual package '" + pkgName + "' is already installed";
    return false;
  }
  if (apm::status::isInstalled(pkgName, nullptr, nullptr)) {
    if (errorMsg)
      *errorMsg =
          "Package '" + pkgName + "' is already installed via 'apm install'";
    return false;
  }
  return true;
}

// Build a flat list of files under `root` so we can capture everything a
// manual archive drops onto disk (used for uninstall bookkeeping).
static bool collectFilesRecursive(const std::string &root,
                                  const std::string &relative,
                                  std::vector<std::string> &out,
                                  std::string *errorMsg) {
  std::string absPath =
      relative.empty() ? root : apm::fs::joinPath(root, relative);

  struct stat st{};
  if (::lstat(absPath.c_str(), &st) != 0) {
    if (errorMsg)
      *errorMsg = "Failed to stat " + absPath + ": " + std::strerror(errno);
    return false;
  }

  if (S_ISDIR(st.st_mode)) {
    DIR *dir = ::opendir(absPath.c_str());
    if (!dir) {
      if (errorMsg)
        *errorMsg =
            "Failed to open directory " + absPath + ": " + std::strerror(errno);
      return false;
    }

    struct dirent *ent;
    while ((ent = ::readdir(dir)) != nullptr) {
      const char *name = ent->d_name;
      if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0)
        continue;
      std::string childRel =
          relative.empty() ? std::string(name) : relative + "/" + name;
      if (!collectFilesRecursive(root, childRel, out, errorMsg)) {
        ::closedir(dir);
        return false;
      }
    }
    ::closedir(dir);
    return true;
  }

  // Only record files, links, etc. Skip the root directory itself.
  if (!relative.empty())
    out.push_back(relative);
  return true;
}

// Enumerate files underneath the manual install prefix so we can produce a
// deterministic manifest for removal.
static bool collectInstalledFiles(const std::string &root,
                                  std::vector<std::string> &out,
                                  std::string *errorMsg) {
  out.clear();
  if (!apm::fs::isDirectory(root)) {
    if (errorMsg)
      *errorMsg = "Install root does not exist: " + root;
    return false;
  }
  if (!collectFilesRecursive(root, "", out, errorMsg))
    return false;
  std::sort(out.begin(), out.end());
  return true;
}

// Discover which directory within an extracted archive actually contains the
// package-info.json metadata we expect.
static bool findPackageInfoRoot(const std::string &baseDir, std::string &out,
                                std::string *errorMsg) {
  std::string infoAtRoot = apm::fs::joinPath(baseDir, "package-info.json");
  if (apm::fs::isFile(infoAtRoot)) {
    out = baseDir;
    return true;
  }

  std::string found;
  auto entries = apm::fs::listDir(baseDir, false);
  for (const auto &entry : entries) {
    if (entry.empty() || entry[0] == '.')
      continue;
    std::string subdir = apm::fs::joinPath(baseDir, entry);
    if (!apm::fs::isDirectory(subdir))
      continue;
    std::string candidate = apm::fs::joinPath(subdir, "package-info.json");
    if (apm::fs::isFile(candidate)) {
      if (!found.empty()) {
        if (errorMsg)
          *errorMsg = "Multiple package-info.json files found in archive";
        return false;
      }
      found = subdir;
    }
  }

  if (found.empty()) {
    if (errorMsg)
      *errorMsg = "package-info.json not found in archive";
    return false;
  }

  out = found;
  return true;
}

// Ensure the manual install prefix sits somewhere under
// apm::config::INSTALLED_DIR before touching the filesystem.
static bool prefixWithinInstalledRoot(const std::string &prefix) {
  const std::string root = apm::config::INSTALLED_DIR;
  if (prefix.size() < root.size())
    return false;
  if (prefix.compare(0, root.size(), root) != 0)
    return false;
  if (prefix.size() == root.size())
    return true;
  char next = prefix[root.size()];
  return next == '/' || root.back() == '/';
}

static bool installManualPackageFromDeb(const std::string &debPath,
                                        std::string &pkgNameOut,
                                        std::string *errorMsg);

static bool installManualPackageFromArchive(const std::string &archivePath,
                                            std::string &pkgNameOut,
                                            std::string *errorMsg);

// Entry point that detects whether the manual payload is a .deb or tarball
// and forwards to the appropriate extraction/registration path.
static bool installManualPackage(const std::string &path,
                                 std::string &pkgNameOut,
                                 std::string *errorMsg) {
  ManualArchiveType type;
  if (!detectManualArchiveType(path, type, errorMsg))
    return false;
  switch (type) {
  case ManualArchiveType::Deb:
    return installManualPackageFromDeb(path, pkgNameOut, errorMsg);
  case ManualArchiveType::Tarball:
    return installManualPackageFromArchive(path, pkgNameOut, errorMsg);
  default:
    break;
  }
  if (errorMsg)
    *errorMsg = "Unsupported manual package type";
  return false;
}

// Handle manual installs sourced from a .deb by extracting its control/data
// members and turning them into an APM manual prefix.
static bool installManualPackageFromDeb(const std::string &debPath,
                                        std::string &pkgNameOut,
                                        std::string *errorMsg) {
  std::string tmpRoot = createTempDir("deb", errorMsg);
  if (tmpRoot.empty())
    return false;
  ScopedTempDir cleanup(tmpRoot);

  std::string workDir = apm::fs::joinPath(tmpRoot, "work");
  if (!apm::fs::createDirs(workDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create work directory: " + workDir;
    return false;
  }

  apm::deb::DebParts parts;
  std::string err;
  if (!apm::deb::extractDebArchive(debPath, workDir, parts, &err)) {
    if (errorMsg)
      *errorMsg = "Failed to extract .deb: " + err;
    return false;
  }

  std::string controlDir = apm::fs::joinPath(workDir, "control");
  std::string dataDir = apm::fs::joinPath(workDir, "data");
  if (!apm::fs::createDirs(controlDir) || !apm::fs::createDirs(dataDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create control/data directories";
    return false;
  }

  if (!parts.controlTarPath.empty()) {
    if (!apm::tar::extractTar(parts.controlTarPath, controlDir, &err)) {
      if (errorMsg)
        *errorMsg = "Failed to extract control tar: " + err;
      return false;
    }
  }
  if (!parts.dataTarPath.empty()) {
    if (!apm::tar::extractTar(parts.dataTarPath, dataDir, &err)) {
      if (errorMsg)
        *errorMsg = "Failed to extract data tar: " + err;
      return false;
    }
  }

  std::string controlPath = apm::fs::joinPath(controlDir, "control");
  auto cf = apm::control::parseControlFile(controlPath);
  std::string pkgName = cf.packageName;
  if (pkgName.empty()) {
    if (errorMsg)
      *errorMsg = "control file inside .deb is missing Package field";
    return false;
  }

  if (!ensureManualSlotAvailable(pkgName, errorMsg))
    return false;

  std::string installRoot =
      apm::fs::joinPath(apm::config::COMMANDS_DIR, pkgName);
  if (apm::fs::pathExists(installRoot)) {
    if (errorMsg)
      *errorMsg = "Install directory already exists: " + installRoot;
    return false;
  }

  if (!ensureParentDirectory(installRoot, errorMsg))
    return false;

  if (::rename(dataDir.c_str(), installRoot.c_str()) < 0) {
    if (errorMsg)
      *errorMsg = "Failed to move data dir into place: " +
                  std::string(std::strerror(errno));
    return false;
  }

  apm::manual::PackageInfo info;
  info.name = pkgName;
  info.version = cf.version.empty() ? "manual" : cf.version;
  info.prefix = installRoot;
  info.installTime = currentUnixTimestamp();

  if (!collectInstalledFiles(installRoot, info.installedFiles, errorMsg)) {
    apm::fs::removeDirRecursive(installRoot);
    return false;
  }

  if (!apm::manual::saveInstalledPackage(info, errorMsg)) {
    apm::fs::removeDirRecursive(installRoot);
    return false;
  }

  pkgNameOut = pkgName;
  return true;
}

// Handle manual installs packaged as tar archives that already contain
// package-info.json metadata at their root.
static bool installManualPackageFromArchive(const std::string &archivePath,
                                            std::string &pkgNameOut,
                                            std::string *errorMsg) {
  std::string tmpRoot = createTempDir("archive", errorMsg);
  if (tmpRoot.empty())
    return false;
  ScopedTempDir cleanup(tmpRoot);

  std::string extractDir = apm::fs::joinPath(tmpRoot, "extract");
  if (!apm::fs::createDirs(extractDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create extract directory: " + extractDir;
    return false;
  }

  std::string err;
  if (!apm::tar::extractTar(archivePath, extractDir, &err)) {
    if (errorMsg)
      *errorMsg = "Failed to extract archive: " + err;
    return false;
  }

  std::string packageRoot;
  if (!findPackageInfoRoot(extractDir, packageRoot, errorMsg))
    return false;

  std::string infoPath = apm::fs::joinPath(packageRoot, "package-info.json");
  apm::manual::PackageInfo info;
  if (!apm::manual::readPackageInfoFile(infoPath, info, errorMsg))
    return false;

  if (!ensureManualSlotAvailable(info.name, errorMsg))
    return false;

  if (info.prefix.empty()) {
    if (errorMsg)
      *errorMsg = "package-info.json is missing prefix";
    return false;
  }

  if (!prefixWithinInstalledRoot(info.prefix)) {
    if (errorMsg)
      *errorMsg =
          "prefix must be under " + std::string(apm::config::INSTALLED_DIR);
    return false;
  }

  if (info.prefix == apm::config::INSTALLED_DIR) {
    if (errorMsg)
      *errorMsg = "prefix must include a package-specific subdirectory";
    return false;
  }

  if (apm::fs::pathExists(info.prefix)) {
    if (errorMsg)
      *errorMsg = "Install directory already exists: " + info.prefix;
    return false;
  }

  if (!ensureParentDirectory(info.prefix, errorMsg))
    return false;

  if (::rename(packageRoot.c_str(), info.prefix.c_str()) < 0) {
    if (errorMsg)
      *errorMsg = "Failed to move package contents: " +
                  std::string(std::strerror(errno));
    return false;
  }

  info.installTime = currentUnixTimestamp();
  if (!collectInstalledFiles(info.prefix, info.installedFiles, errorMsg)) {
    apm::fs::removeDirRecursive(info.prefix);
    return false;
  }

  if (!apm::manual::saveInstalledPackage(info, errorMsg)) {
    apm::fs::removeDirRecursive(info.prefix);
    return false;
  }

  pkgNameOut = info.name;
  return true;
}

// Result of attempting to delete a manual package before falling back to
// daemon-backed removal.
enum class ManualRemoveState { NotManual, Removed, Failed };

// Remove a manual package by replaying the recorded manifest of installed
// files before deleting the JSON metadata.
static ManualRemoveState tryRemoveManualPackage(const std::string &name,
                                                std::string &message,
                                                std::string *errorMsg) {
  if (!apm::manual::isInstalled(name))
    return ManualRemoveState::NotManual;

  apm::manual::PackageInfo info;
  if (!apm::manual::loadInstalledPackage(name, info, errorMsg))
    return ManualRemoveState::Failed;

  if (!info.prefix.empty() && apm::fs::pathExists(info.prefix)) {
    if (!apm::fs::removeDirRecursive(info.prefix)) {
      if (errorMsg)
        *errorMsg = "Failed to remove " + info.prefix;
      return ManualRemoveState::Failed;
    }
  }

  if (!apm::manual::removeInstalledPackage(name, errorMsg))
    return ManualRemoveState::Failed;

  apm::daemon::path::refreshPathEnvironment();
  message = "Removed manual package: " + name;
  return ManualRemoveState::Removed;
}

// Print CLI usage summary and describe the available high-level commands.
void printUsage() {
  std::cout
      << "APM - Android Package Manager\n"
      << "Usage:\n"
      << "  apm [--socket <path>] <command> [args...]\n"
      << "\nCommands:\n"
      << "  ping                        Check connection to apmd\n"
      << "  update                      Update repository metadata\n"
      << "  install <pkg>               Install a package from repo\n"
      << "  package-install <file>      Install a local .deb/.gz/.xz\n"
      << "  remove <pkg>                Remove an installed package\n"
      << "  upgrade [pkgs...]           Upgrade all or selected packages\n"
      << "  autoremove                  Remove unused auto-installed deps\n"
      << "  module-list                 List installed AMS modules\n"
      << "  module-install <zip>        Install an AMS module from a ZIP\n"
      << "  module-enable <name>        Enable an installed AMS module\n"
      << "  module-disable <name>       Disable an AMS module\n"
      << "  module-remove <name>        Remove an AMS module\n"
      << "\n"
      << "  apk-install <apk> [--install-as-system]\n"
      << "                              Install APK (normal or system app)\n"
      << "  apk-uninstall <package>     Uninstall APK by package name\n"
      << "\n"
      << "  list                        Show installed packages\n"
      << "  info <pkg>                  Show detailed package information\n"
      << "  search <pattern>            Search available repo packages\n"
      << "  version                     Show APM version and build date\n"
      << "  key-add <file.asc|.gpg>     Import a trusted public key\n"
      << "\n"
      << "  help                        Show this help\n"
      << std::endl;
}

//
// ============================================================
// IPC COMMANDS
// ============================================================
//

// Send a Ping request over IPC to confirm the daemon is reachable.
int cmdPing(const std::string &socketPath) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Ping;
  req.id = "ping-1";

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequest(req, resp, socketPath, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "OK: " : "ERROR: ")
            << (resp.message.empty() ? "(no message)" : resp.message) << "\n";
  return resp.success ? 0 : 1;
}

// Trigger repository metadata refresh via the daemon so Packages lists stay
// current.
int cmdUpdate(const std::string &socketPath, const std::string &sessionToken) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Update;
  req.id = "update-1";
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;
  struct ProgressUiState {
    std::string activeKey;
    bool lineActive = false;
  } ui;

  auto onProgress = [&](const apm::ipc::Response &chunk) {
    auto eventIt = chunk.rawFields.find("event");
    if (eventIt == chunk.rawFields.end() || eventIt->second != "repo-update")
      return;

    auto getField = [&](const std::string &key) -> std::string {
      auto it = chunk.rawFields.find(key);
      return it != chunk.rawFields.end() ? it->second : "";
    };

    const std::string stage = getField("stage");
    const std::string remote = getField("remote");
    const std::string component = getField("component");
    const std::string key = stage + ":" + remote + ":" + component;

    if (ui.activeKey != key) {
      if (ui.lineActive)
        std::cout << std::endl;
      ui.activeKey = key;
      ui.lineActive = true;
    }

    std::string description = getField("description");
    if (description.empty())
      description = stage.empty() ? "update" : stage;

    std::string label = description;
    if (!component.empty()) {
      label = component + " - " + description;
    } else if (!getField("repo").empty()) {
      label = getField("repo") + " - " + description;
    }

    const std::uint64_t current = parseUintSafe(getField("bytes"));
    const std::uint64_t total = parseUintSafe(getField("total"));
    const double dlSpeed = parseDoubleSafe(getField("dl_speed"));
    const double ulSpeed = parseDoubleSafe(getField("ul_speed"));
    const bool finished = getField("finished") == "1";
    const double ratio =
        total > 0 ? static_cast<double>(current) / static_cast<double>(total)
                  : 0.0;

    std::ostringstream line;
    line << "\r" << label << " " << buildProgressBar(ratio) << " "
         << formatBytes(current) << "/";
    if (total > 0)
      line << formatBytes(total);
    else
      line << "??";
    line << "  " << formatSpeed(dlSpeed) << "↓ " << formatSpeed(ulSpeed) << "↑";

    std::cout << line.str() << std::flush;

    if (finished) {
      std::cout << std::endl;
      ui.lineActive = false;
    }
  };

  if (!apm::ipc::sendRequest(req, resp, socketPath, &err, onProgress)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  if (ui.lineActive)
    std::cout << std::endl;

  std::cout << (resp.success ? "Update succeeded" : "Update failed");
  if (!resp.message.empty())
    std::cout << ": " << resp.message;
  std::cout << "\n";
  return resp.success ? 0 : 1;
}

// Ask the daemon to install a package along with its dependencies.
int cmdInstall(const std::string &socketPath, const std::string &sessionToken,
               const std::string &pkg) {
  auto parsePackageList = [](const apm::ipc::Response &resp,
                             const std::string &field) {
    std::vector<std::string> pkgs;
    auto it = resp.rawFields.find(field);
    if (it == resp.rawFields.end())
      return pkgs;
    std::istringstream ss(it->second);
    std::string name;
    while (ss >> name)
      pkgs.push_back(name);
    return pkgs;
  };

  apm::ipc::Request planReq;
  planReq.type = apm::ipc::RequestType::Install;
  planReq.id = "install-plan-1";
  planReq.packageName = pkg;
  attachSession(planReq, sessionToken);
  planReq.rawFields["simulate"] = "1";

  apm::ipc::Response planResp;
  std::string err;

  if (!apm::ipc::sendRequest(planReq, planResp, socketPath, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  if (!planResp.success) {
    std::cerr << "Install failed: "
              << (planResp.message.empty() ? "unknown error" : planResp.message)
              << "\n";
    return 1;
  }

  const auto planList = parsePackageList(planResp, "packages");

  if (planList.empty()) {
    std::cout << "All dependencies already satisfied for '" << pkg << "'.\n";
    return 0;
  }

  std::cout << "The following packages will be installed:\n";
  for (const auto &name : planList) {
    bool isRoot = (name == pkg);
    std::cout << "  - " << name;
    if (isRoot)
      std::cout << " (target)";
    else
      std::cout << " (dependency)";
    std::cout << "\n";
  }

  std::cout << "Proceed with installation? [y/N]: " << std::flush;
  std::string response;
  if (!std::getline(std::cin, response)) {
    response.clear();
  }

  bool confirmed = false;
  for (char ch : response) {
    if (std::isspace(static_cast<unsigned char>(ch)))
      continue;
    if (ch == 'y' || ch == 'Y')
      confirmed = true;
    break;
  }

  if (!confirmed) {
    std::cout << "Installation aborted.\n";
    return 0;
  }

  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Install;
  req.id = "install-run-1";
  req.packageName = pkg;
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  err.clear();

  struct DownloadUiState {
    std::string activeKey;
    bool lineActive = false;
  } ui;

  auto onProgress = [&](const apm::ipc::Response &chunk) {
    auto eventIt = chunk.rawFields.find("event");
    if (eventIt == chunk.rawFields.end() ||
        eventIt->second != "install-download")
      return;

    auto getField = [&](const std::string &key) -> std::string {
      auto it = chunk.rawFields.find(key);
      return it != chunk.rawFields.end() ? it->second : "";
    };

    const std::string pkgName = getField("package");
    std::string fileLabel = getField("file");
    const std::string dest = getField("destination");
    if (fileLabel.empty()) {
      auto pos = dest.find_last_of('/');
      if (pos == std::string::npos)
        fileLabel = dest;
      else if (pos + 1 < dest.size())
        fileLabel = dest.substr(pos + 1);
    }
    if (fileLabel.empty())
      fileLabel = pkgName;

    const std::uint64_t current = parseUintSafe(getField("bytes"));
    const std::uint64_t total = parseUintSafe(getField("total"));
    const double dlSpeed = parseDoubleSafe(getField("dl_speed"));
    const double ulSpeed = parseDoubleSafe(getField("ul_speed"));
    const bool finished = getField("finished") == "1";
    const double ratio =
        total > 0 ? static_cast<double>(current) / static_cast<double>(total)
                  : 0.0;

    const std::string key = pkgName + ":" + dest;
    if (ui.activeKey != key) {
      if (ui.lineActive)
        std::cout << std::endl;
      ui.activeKey = key;
      ui.lineActive = true;
    }

    std::string label =
        pkgName.empty() ? fileLabel : pkgName + " - " + fileLabel;

    std::ostringstream line;
    line << "\r" << label << " " << buildProgressBar(ratio) << " "
         << formatBytes(current) << "/";
    if (total > 0)
      line << formatBytes(total);
    else
      line << "??";
    line << "  " << formatSpeed(dlSpeed) << "↓ " << formatSpeed(ulSpeed) << "↑";

    std::cout << line.str() << std::flush;

    if (finished) {
      std::cout << std::endl;
      ui.lineActive = false;
    }
  };

  if (!apm::ipc::sendRequest(req, resp, socketPath, &err, onProgress)) {
    if (ui.lineActive)
      std::cout << std::endl;
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  if (ui.lineActive)
    std::cout << std::endl;

  std::cout << (resp.success ? "Install succeeded: " : "Install failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

// Install a manually provided archive (.deb/.tar.*) entirely from the CLI
// without daemon IPC.
int cmdPackageInstall(const std::string &packagePath) {
  if (packagePath.empty()) {
    std::cerr << "apm: 'package-install' requires a file path\n";
    return 1;
  }

  if (!apm::fs::isFile(packagePath)) {
    std::cerr << "apm package-install: file not found: " << packagePath << "\n";
    return 1;
  }

  std::string pkgName;
  std::string err;
  if (!installManualPackage(packagePath, pkgName, &err)) {
    std::cerr << "apm package-install failed: " << err << "\n";
    return 1;
  }

  apm::daemon::path::refreshPathEnvironment();
  std::cout << "Manual package installed: " << pkgName << "\n";
  return 0;
}

// Ask the daemon to remove a package and clean up metadata.
int cmdRemove(const std::string &socketPath, const std::string &sessionToken,
              const std::string &pkg) {
  std::string manualMsg;
  std::string manualErr;
  ManualRemoveState manualState =
      tryRemoveManualPackage(pkg, manualMsg, &manualErr);
  if (manualState == ManualRemoveState::Removed) {
    std::cout << manualMsg << "\n";
    return 0;
  }
  if (manualState == ManualRemoveState::Failed) {
    std::cerr << "apm remove failed: " << manualErr << "\n";
    return 1;
  }

  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Remove;
  req.id = "remove-1";
  req.packageName = pkg;
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequest(req, resp, socketPath, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "Remove succeeded: " : "Remove failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

// Forward an APK install request to the daemon, optionally forcing a system
// install overlay.
int cmdApkInstall(const std::string &socketPath,
                  const std::string &sessionToken, const std::string &apk,
                  bool installAsSystem) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::ApkInstall;
  req.id = "apk-install-1";
  attachSession(req, sessionToken);

  req.apkPath = apk;
  req.installAsSystem = installAsSystem;

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequest(req, resp, socketPath, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "APK install succeeded: "
                             : "APK install failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

// Forward an APK uninstall request to the daemon for a given package name.
int cmdApkUninstall(const std::string &socketPath,
                    const std::string &sessionToken,
                    const std::string &pkgName) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::ApkUninstall;
  req.id = "apk-uninstall-1";
  req.packageName = pkgName;
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequest(req, resp, socketPath, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "APK uninstall succeeded: "
                             : "APK uninstall failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

// Request upgrades for either all installed packages or a provided subset.
int cmdUpgrade(const std::string &socketPath, const std::string &sessionToken,
               int argc, char **argv) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Upgrade;
  req.id = "upgrade-1";
  attachSession(req, sessionToken);

  if (argc > 0) {
    std::ostringstream ss;
    for (int i = 0; i < argc; ++i) {
      if (i > 0)
        ss << ' ';
      ss << argv[i];
    }
    req.rawFields["targets"] = ss.str();
  }

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequest(req, resp, socketPath, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "Upgrade succeeded: " : "Upgrade failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

// Ask the daemon to remove auto-installed dependencies no longer needed.
int cmdAutoremove(const std::string &socketPath,
                  const std::string &sessionToken) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Autoremove;
  req.id = "autoremove-1";
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequest(req, resp, socketPath, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "Autoremove succeeded: " : "Autoremove failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

int cmdModuleInstall(const std::string &socketPath,
                     const std::string &sessionToken,
                     const std::string &zipPath) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::ModuleInstall;
  req.id = "module-install-1";
  req.modulePath = zipPath;
  req.rawFields["module_path"] = zipPath;
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequest(req, resp, socketPath, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "Module install succeeded: "
                             : "Module install failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

static int runModuleToggle(const std::string &socketPath,
                           const std::string &sessionToken,
                           apm::ipc::RequestType type, const std::string &name,
                           const std::string &verb) {
  apm::ipc::Request req;
  req.type = type;
  req.id = verb + "-module-1";
  req.moduleName = name;
  req.packageName = name;
  req.rawFields["module"] = name;
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequest(req, resp, socketPath, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? verb + " succeeded: " : verb + " failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

int cmdModuleEnable(const std::string &socketPath,
                    const std::string &sessionToken, const std::string &name) {
  return runModuleToggle(socketPath, sessionToken,
                         apm::ipc::RequestType::ModuleEnable, name,
                         "Module enable");
}

int cmdModuleDisable(const std::string &socketPath,
                     const std::string &sessionToken, const std::string &name) {
  return runModuleToggle(socketPath, sessionToken,
                         apm::ipc::RequestType::ModuleDisable, name,
                         "Module disable");
}

int cmdModuleRemove(const std::string &socketPath,
                    const std::string &sessionToken, const std::string &name) {
  return runModuleToggle(socketPath, sessionToken,
                         apm::ipc::RequestType::ModuleRemove, name,
                         "Module remove");
}

int cmdModuleList(const std::string &socketPath,
                  const std::string &sessionToken) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::ModuleList;
  req.id = "module-list-1";
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequest(req, resp, socketPath, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  if (!resp.message.empty())
    std::cout << resp.message << "\n";
  else
    std::cout << (resp.success ? "No modules installed."
                               : "Module list unavailable")
              << "\n";
  return resp.success ? 0 : 1;
}

//
// ============================================================
// LOCAL COMMANDS
// ============================================================
//

// List installed packages by reading the local status database directly.
int cmdListLocal() {
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

  std::cout << "Installed packages:\n";
  for (const auto &kv : db) {
    const auto &pkg = kv.second;
    std::cout << "  " << pkg.name;
    if (!pkg.version.empty())
      std::cout << " " << pkg.version;
    if (!pkg.architecture.empty())
      std::cout << " [" << pkg.architecture << "]";
    if (pkg.autoInstalled)
      std::cout << " (auto)";
    std::cout << "\n";
  }

  return 0;
}

// Display local installation info plus candidate repo metadata for a package.
int cmdInfoLocal(const std::string &name) {
  using namespace apm;

  status::InstalledPackage inst;
  bool isInstalled = status::isInstalled(name, &inst, nullptr);

  std::cout << "Package: " << name << "\n";
  std::cout << "Installed: " << (isInstalled ? "yes" : "no") << "\n";

  if (isInstalled) {
    if (!inst.version.empty())
      std::cout << "Installed-Version: " << inst.version << "\n";
    if (!inst.architecture.empty())
      std::cout << "Architecture: " << inst.architecture << "\n";
    if (!inst.installRoot.empty())
      std::cout << "Installed-Root: " << inst.installRoot << "\n";
    if (!inst.status.empty())
      std::cout << "Status: " << inst.status << "\n";
    if (inst.autoInstalled)
      std::cout << "Auto-Installed: yes\n";
    std::cout << "\n";
  }

  repo::RepoIndexList indices;
  std::string err;

  if (!repo::buildRepoIndices(config::SOURCES_LIST, config::LISTS_DIR,
                              config::DEFAULT_ARCH, indices, &err)) {
    std::cout << "Repository info unavailable";
    if (!err.empty())
      std::cout << ": " << err;
    std::cout << "\n";
    return 0;
  }

  const repo::PackageEntry *cand = nullptr;
  for (const auto &idx : indices) {
    auto *f = repo::findPackage(idx.packages, name, "");
    if (f) {
      cand = f;
      break;
    }
  }

  if (!cand) {
    std::cout << "Not found in repositories.\n";
    return 0;
  }

  std::cout << "Candidate-Version: " << cand->version << "\n";
  if (!cand->architecture.empty())
    std::cout << "Candidate-Architecture: " << cand->architecture << "\n";

  if (!cand->repoUri.empty()) {
    std::cout << "Repository: " << cand->repoUri;
    if (!cand->repoDist.empty())
      std::cout << " " << cand->repoDist;
    if (!cand->repoComponent.empty())
      std::cout << " " << cand->repoComponent;
    std::cout << "\n";
  }

  if (isInstalled && inst.version != cand->version) {
    std::cout << "Upgrade-Available: yes\n";
    std::cout << "  Installed: " << inst.version << "\n";
    std::cout << "  Candidate: " << cand->version << "\n";
  }

  std::cout << "\n=== Metadata ===\n";
  for (const auto &kv : cand->rawFields) {
    if (kv.first == "Description") {
      std::cout << "Description: " << kv.second << "\n";
      auto itLong = cand->rawFields.find("Description-long");
      if (itLong != cand->rawFields.end())
        std::cout << "\n" << itLong->second << "\n";
      continue;
    }
    std::cout << kv.first << ": " << kv.second << "\n";
  }

  std::cout << "\n";
  return 0;
}

// Perform a read-only search across cached repo indices from the CLI only path.
int cmdSearchLocal(int argc, char **argv) {
  std::vector<std::string> patterns;
  patterns.reserve(argc);
  for (int i = 0; i < argc; i++)
    patterns.emplace_back(argv[i]);
  return apm::cli::searchPackages(patterns);
}

// Print the CLI version/build metadata.
int cmdVersion() {
  std::cout << "APM version " << kApmVersion;
  if (kApmBuildDate && std::strlen(kApmBuildDate) > 0)
    std::cout << " (built " << kApmBuildDate << ")";
  std::cout << "\n";
  return 0;
}

// Import an ASCII-armored public key into the trusted key directory.
int cmdKeyAdd(const std::string &path) {
  std::string fingerprint;
  std::string storedPath;
  std::string error;

  if (!apm::crypto::importTrustedPublicKey(path, apm::config::TRUSTED_KEYS_DIR,
                                           &fingerprint, &storedPath, &error)) {
    std::cerr << "Failed to import key: " << error << "\n";
    return 1;
  }

  std::cout << "Trusted key imported: " << fingerprint;
  if (!storedPath.empty())
    std::cout << " -> " << storedPath;
  std::cout << "\n";
  return 0;
}

} // namespace

//
// ============================================================
// MAIN
// ============================================================
//

// Entry point that parses global options, dispatches to subcommands, and wires
// IPC/socket overrides.
int main(int argc, char **argv) {
  apm::logger::setLogFile("apm-cli.log");
  apm::logger::setMinLogLevel(apm::logger::Level::Debug);

  std::string socketPath = apm::config::SOCKET_PATH;

  int i = 1;
  while (i < argc) {
    std::string a = argv[i];
    if (a == "--socket" && i + 1 < argc) {
      socketPath = argv[i + 1];
      i += 2;
    } else if (a == "--help" || a == "-h") {
      printUsage();
      return 0;
    } else
      break;
  }

  if (i >= argc) {
    printUsage();
    return 1;
  }

  std::string cmd = argv[i++];

  std::string sessionToken;
  if (requiresAuthSession(cmd)) {
    if (!ensureAuthenticatedSession(socketPath, sessionToken))
      return 1;
  }

  //
  // ============================
  //   IPC COMMAND DISPATCH
  // ============================
  //

  if (cmd == "ping")
    return cmdPing(socketPath);
  if (cmd == "update")
    return cmdUpdate(socketPath, sessionToken);

  if (cmd == "install") {
    if (i >= argc) {
      std::cerr << "apm: 'install' requires a package\n";
      return 1;
    }
    return cmdInstall(socketPath, sessionToken, argv[i]);
  }

  if (cmd == "package-install") {
    if (i >= argc) {
      std::cerr << "apm: 'package-install' requires a file path\n";
      return 1;
    }
    return cmdPackageInstall(argv[i]);
  }

  if (cmd == "module-install") {
    if (i >= argc) {
      std::cerr << "apm: 'module-install' requires a ZIP path\n";
      return 1;
    }
    return cmdModuleInstall(socketPath, sessionToken, argv[i]);
  }

  if (cmd == "module-list") {
    return cmdModuleList(socketPath, sessionToken);
  }

  if (cmd == "module-enable") {
    if (i >= argc) {
      std::cerr << "apm: 'module-enable' requires a module name\n";
      return 1;
    }
    return cmdModuleEnable(socketPath, sessionToken, argv[i]);
  }

  if (cmd == "module-disable") {
    if (i >= argc) {
      std::cerr << "apm: 'module-disable' requires a module name\n";
      return 1;
    }
    return cmdModuleDisable(socketPath, sessionToken, argv[i]);
  }

  if (cmd == "module-remove") {
    if (i >= argc) {
      std::cerr << "apm: 'module-remove' requires a module name\n";
      return 1;
    }
    return cmdModuleRemove(socketPath, sessionToken, argv[i]);
  }

  if (cmd == "remove") {
    if (i >= argc) {
      std::cerr << "apm: 'remove' requires a package\n";
      return 1;
    }
    return cmdRemove(socketPath, sessionToken, argv[i]);
  }

  if (cmd == "apk-install") {
    if (i >= argc) {
      std::cerr << "apm: 'apk-install' requires an APK path\n";
      return 1;
    }
    std::string apk = argv[i++];
    bool sys = false;
    while (i < argc) {
      std::string o = argv[i++];
      if (o == "--install-as-system")
        sys = true;
      else {
        std::cerr << "apm: unknown option for apk-install: " << o << "\n";
        return 1;
      }
    }
    return cmdApkInstall(socketPath, sessionToken, apk, sys);
  }

  if (cmd == "apk-uninstall") {
    if (i >= argc) {
      std::cerr << "apm: 'apk-uninstall' requires a package name\n";
      return 1;
    }
    return cmdApkUninstall(socketPath, sessionToken, argv[i]);
  }

  if (cmd == "upgrade") {
    int remaining = argc - i;
    return cmdUpgrade(socketPath, sessionToken, remaining, argv + i);
  }

  if (cmd == "autoremove")
    return cmdAutoremove(socketPath, sessionToken);

  //
  // ============================
  //   LOCAL COMMAND DISPATCH
  // ============================
  //

  if (cmd == "list")
    return cmdListLocal();
  if (cmd == "info") {
    if (i >= argc) {
      std::cerr << "apm: 'info' requires a package\n";
      return 1;
    }
    return cmdInfoLocal(argv[i]);
  }
  if (cmd == "search") {
    int remaining = argc - i;
    return cmdSearchLocal(remaining, argv + i);
  }
  if (cmd == "version")
    return cmdVersion();
  if (cmd == "key-add") {
    if (i >= argc) {
      std::cerr << "apm: 'key-add' requires a .asc file\n";
      return 1;
    }
    return cmdKeyAdd(argv[i]);
  }
  if (cmd == "help")
    return printUsage(), 0;

  std::cerr << "apm: unknown command '" << cmd << "'\n";
  printUsage();
  return 1;
}
