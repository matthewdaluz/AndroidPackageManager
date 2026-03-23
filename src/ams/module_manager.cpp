/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: module_manager.cpp
 * Purpose: Implement AMS module lifecycle management plus OverlayFS
 * orchestration.
 * Last Modified: December 4th, 2025. - 09:07 AM Eastern Time
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

#include "ams/module_manager.hpp"

#include "config.hpp"
#include "fs.hpp"
#include "logger.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits.h>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <sys/mount.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr const char *kLogFileTag = "module_manager.cpp";
std::atomic<bool> gSelinuxMountContextDisabled{false};

struct ScopedTempDir {
  explicit ScopedTempDir(std::string p) : path(std::move(p)) {}
  ~ScopedTempDir() {
    if (!path.empty()) {
      apm::fs::removeDirRecursive(path);
    }
  }
  void release() { path.clear(); }
  std::string path;
};

struct OverlayTarget {
  const char *name;
  const char *mountPoint;
};

constexpr OverlayTarget kOverlayTargets[] = {
    {"system", "/system"},
    {"vendor", "/vendor"},
    {"product", "/product"},
};

std::string baseMirrorPath(const OverlayTarget &target) {
  return apm::fs::joinPath(apm::config::getModuleRuntimeBaseDir(),
                           target.name);
}

std::string baseMirrorSourcePath(const OverlayTarget &target) {
  return apm::fs::joinPath(apm::config::getModuleRuntimeDir(),
                           std::string("base-source-") + target.name);
}

std::string normalizeMountPath(const std::string &p) {
  if (p.size() > 1 && p.back() == '/')
    return p.substr(0, p.size() - 1);
  return p;
}

std::string resolveForMount(const std::string &path) {
  char resolvedBuf[PATH_MAX];
  if (::realpath(path.c_str(), resolvedBuf)) {
    return normalizeMountPath(resolvedBuf);
  }
  return normalizeMountPath(path);
}

bool isRegularFile(const std::string &path) { return apm::fs::isFile(path); }

bool ensureDir(const std::string &path) { return apm::fs::createDirs(path); }

std::string shellEscapeSingleQuotes(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char c : value) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool runCommand(const std::string &cmd, std::string *errorMsg) {
  if (apm::logger::isDebugEnabled()) {
    apm::logger::debug(std::string(kLogFileTag) + ": runCommand exec='" + cmd +
                       "'");
  }

  int rc = ::system(cmd.c_str());
  if (apm::logger::isDebugEnabled()) {
    apm::logger::debug(std::string(kLogFileTag) +
                       ": runCommand result rc=" + std::to_string(rc));
  }

  if (rc != 0) {
    if (errorMsg)
      *errorMsg =
          "Command failed: " + cmd + " (exit code " + std::to_string(rc) + ")";
    return false;
  }
  return true;
}

bool unmountPath(const std::string &path, std::string *errorMsg) {
  if (::umount2(path.c_str(), MNT_DETACH) == 0)
    return true;

  int err = errno;
  if (err == EINVAL || err == ENOENT)
    return true;

  if (errorMsg)
    *errorMsg = "Failed to unmount " + path + ": " + std::strerror(err);
  return false;
}

bool shouldTrySelinuxMountContext() {
  return !gSelinuxMountContextDisabled.load(std::memory_order_relaxed);
}

void disableSelinuxMountContextForBoot(const std::string &mountPoint,
                                       const std::string &mountKind,
                                       int err) {
  if (err != EACCES && err != EPERM)
    return;

  bool expected = false;
  if (gSelinuxMountContextDisabled.compare_exchange_strong(
          expected, true, std::memory_order_relaxed,
          std::memory_order_relaxed)) {
    apm::logger::warn(
        "AMS overlay: SELinux denied " + mountKind + " context mount at " +
        mountPoint + "; disabling context= mount attempts for this boot");
  }
}

bool readMountType(const std::string &path, std::string &type) {
  std::ifstream mounts("/proc/self/mounts");
  if (!mounts.is_open())
    return false;

  std::string resolved = resolveForMount(path);

  auto isWithinMount = [](const std::string &p,
                          const std::string &mountPoint) {
    if (mountPoint == "/")
      return true;
    if (p.size() < mountPoint.size())
      return false;
    if (p.compare(0, mountPoint.size(), mountPoint) != 0)
      return false;
    return p.size() == mountPoint.size() || p[mountPoint.size()] == '/';
  };

  std::string bestType;
  size_t bestMatchLen = 0;
  std::string line;
  while (std::getline(mounts, line)) {
    std::istringstream iss(line);
    std::string source;
    std::string target;
    std::string fsType;
    if (!(iss >> source >> target >> fsType))
      continue;
    target = resolveForMount(target);
    if (!isWithinMount(resolved, target))
      continue;
    if (target.size() > bestMatchLen) {
      bestMatchLen = target.size();
      bestType = fsType;
    }
  }
  if (bestMatchLen == 0)
    return false;

  type = bestType;
  return true;
}

std::vector<std::string> mountCandidatesForTarget(const OverlayTarget &target) {
  std::vector<std::string> out;
  auto addCandidate = [&](const std::string &candidate) {
    if (candidate.empty() || !apm::fs::isDirectory(candidate))
      return;
    std::string fsType;
    if (!readMountType(candidate, fsType))
      return;
    std::string resolved = resolveForMount(candidate);
    if (resolved.empty())
      return;
    if (std::find(out.begin(), out.end(), resolved) == out.end())
      out.push_back(resolved);
  };

  if (std::strcmp(target.name, "system") == 0) {
    // Ordered SAR/system-as-root candidates (AOSP-like preference).
    addCandidate("/system_root/system");
    addCandidate("/system");
    addCandidate("/");
  } else {
    addCandidate(target.mountPoint);
  }

  if (out.empty()) {
    out.push_back(resolveForMount(target.mountPoint));
  }
  return out;
}

std::string effectiveMountPoint(const OverlayTarget &target) {
  auto candidates = mountCandidatesForTarget(target);
  if (!candidates.empty())
    return candidates.front();
  return resolveForMount(target.mountPoint);
}

struct MountInfoEntry {
  std::string mountPoint;
  bool shared = false;
};

std::string decodeMountInfoToken(const std::string &token) {
  std::string out;
  out.reserve(token.size());
  for (size_t i = 0; i < token.size(); ++i) {
    if (token[i] == '\\' && i + 3 < token.size() &&
        std::isdigit(static_cast<unsigned char>(token[i + 1])) &&
        std::isdigit(static_cast<unsigned char>(token[i + 2])) &&
        std::isdigit(static_cast<unsigned char>(token[i + 3]))) {
      int value = (token[i + 1] - '0') * 64 + (token[i + 2] - '0') * 8 +
                  (token[i + 3] - '0');
      out.push_back(static_cast<char>(value));
      i += 3;
      continue;
    }
    out.push_back(token[i]);
  }
  return out;
}

bool readMountInfo(std::vector<MountInfoEntry> &entries) {
  std::ifstream in("/proc/self/mountinfo");
  if (!in.is_open())
    return false;

  entries.clear();
  std::string line;
  while (std::getline(in, line)) {
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string token;
    while (iss >> token) {
      tokens.push_back(token);
    }
    if (tokens.size() < 7)
      continue;

    size_t sep = 0;
    while (sep < tokens.size() && tokens[sep] != "-")
      ++sep;
    if (sep == tokens.size() || sep < 6)
      continue;

    MountInfoEntry entry;
    entry.mountPoint = resolveForMount(decodeMountInfoToken(tokens[4]));
    for (size_t i = 6; i < sep; ++i) {
      if (tokens[i].rfind("shared:", 0) == 0) {
        entry.shared = true;
      }
    }
    entries.push_back(std::move(entry));
  }

  return true;
}

std::string parentPath(const std::string &path) {
  if (path.empty() || path == "/")
    return {};
  size_t slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return {};
  if (slash == 0)
    return "/";
  return path.substr(0, slash);
}

std::vector<std::string> immediateSubmounts(const std::string &target) {
  std::vector<MountInfoEntry> entries;
  std::vector<std::string> out;
  if (!readMountInfo(entries))
    return out;

  const std::string normalized = resolveForMount(target);
  for (const auto &entry : entries) {
    if (entry.mountPoint == normalized)
      continue;
    if (parentPath(entry.mountPoint) == normalized) {
      out.push_back(entry.mountPoint);
    }
  }

  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool isMountShared(const std::string &path) {
  std::vector<MountInfoEntry> entries;
  if (!readMountInfo(entries))
    return false;
  const std::string normalized = resolveForMount(path);
  for (const auto &entry : entries) {
    if (entry.mountPoint == normalized) {
      return entry.shared;
    }
  }
  return false;
}

bool isPathMounted(const std::string &path) {
  std::ifstream mounts("/proc/self/mounts");
  if (!mounts.is_open())
    return false;

  std::string resolved = resolveForMount(path);
  std::string line;
  while (std::getline(mounts, line)) {
    std::istringstream iss(line);
    std::string source;
    std::string target;
    std::string fsType;
    if (!(iss >> source >> target >> fsType))
      continue;
    if (resolveForMount(target) == resolved)
      return true;
  }
  return false;
}

bool isOverlayCandidateReady(const OverlayTarget &target,
                             const std::string &mountPoint) {
  if (std::strcmp(target.name, "system") == 0) {
    std::string fsType;
    return readMountType(mountPoint, fsType);
  }
  return isPathMounted(mountPoint);
}

bool isOverlayMounted(const OverlayTarget &target) {
  std::string type;
  if (!readMountType(effectiveMountPoint(target), type))
    return false;
  return type == "overlay";
}

bool ensureBaseMirrorForTarget(const OverlayTarget &target,
                               const std::string &mountPoint,
                               std::string *errorMsg) {
  std::string baseDir = baseMirrorPath(target);
  std::string sourcePath = baseMirrorSourcePath(target);
  if (!ensureDir(baseDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create base mirror directory for " +
                  std::string(target.name);
    return false;
  }

  if (isPathMounted(baseDir)) {
    std::string previous;
    apm::fs::readFile(sourcePath, previous);
    while (!previous.empty() &&
           (previous.back() == '\n' || previous.back() == '\r' ||
            previous.back() == ' ' || previous.back() == '\t')) {
      previous.pop_back();
    }
    previous = normalizeMountPath(previous);
    if (previous == mountPoint)
      return true;
    std::string unmountErr;
    if (!unmountPath(baseDir, &unmountErr)) {
      if (errorMsg)
        *errorMsg =
            "Failed to refresh base mirror for " + mountPoint + ": " + unmountErr;
      return false;
    }
  }

  std::string currentType;
  if (!readMountType(mountPoint, currentType)) {
    if (errorMsg)
      *errorMsg = "Unable to determine mount type for " +
                  mountPoint;
    return false;
  }

  if (currentType == "overlay") {
    if (errorMsg)
      *errorMsg = std::string("Cannot snapshot base for ") + mountPoint +
          " because it is already overlay-mounted. Reboot into a clean state "
          "before enabling AMS modules.";
    return false;
  }

  if (::mount(mountPoint.c_str(), baseDir.c_str(), nullptr,
              MS_BIND | MS_REC, nullptr) != 0) {
    if (errorMsg)
      *errorMsg = "Failed to mirror " + mountPoint + ": " + std::strerror(errno);
    return false;
  }

  if (::mount(nullptr, baseDir.c_str(), nullptr,
              MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr) != 0) {
    apm::logger::warn("Failed to remount base mirror " + baseDir +
                      " read-only: " + std::strerror(errno));
  }

  apm::fs::writeFile(sourcePath, mountPoint, true);
  ::chmod(sourcePath.c_str(), 0644);
  apm::logger::info("AMS captured base mount for " + mountPoint);
  return true;
}

bool ensureBaseMirrors(std::string *errorMsg) {
  for (const auto &target : kOverlayTargets) {
    if (!ensureBaseMirrorForTarget(target, effectiveMountPoint(target),
                                   errorMsg))
      return false;
  }
  return true;
}

bool mountBaseOnly(const OverlayTarget &target, const std::string &mountPoint,
                   std::string *errorMsg) {
  if (mountPoint == "/") {
    apm::logger::warn(
        "AMS overlay: skipping base restore bind on '/' for safety");
    if (errorMsg)
      errorMsg->clear();
    return true;
  }
  std::string baseDir = baseMirrorPath(target);
  if (!isPathMounted(baseDir)) {
    if (errorMsg)
      *errorMsg = "Missing base mirror for " + std::string(target.name);
    return false;
  }

  if (::mount(baseDir.c_str(), mountPoint.c_str(), nullptr,
              MS_BIND | MS_REC, nullptr) != 0) {
    if (errorMsg)
      *errorMsg = "Failed to restore base mount for " +
                  mountPoint + ": " + std::strerror(errno);
    return false;
  }

  if (::mount(nullptr, mountPoint.c_str(), nullptr,
              MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr) != 0) {
    apm::logger::warn("Failed to remount " + mountPoint +
                      " read-only after restoring base: " +
                      std::strerror(errno));
  }

  apm::logger::info("AMS restored stock " + mountPoint + " mount");
  return true;
}

bool setMountPropagation(const std::string &path, unsigned long flags,
                         std::string *errorMsg, int *errorCode = nullptr) {
  if (::mount(nullptr, path.c_str(), nullptr, flags | MS_REC, nullptr) == 0) {
    return true;
  }
  if (errorCode)
    *errorCode = errno;
  if (errorMsg) {
    *errorMsg = "Failed to change mount propagation for " + path + ": " +
                std::strerror(errno);
  }
  return false;
}

std::string sanitizeForPath(const std::string &in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
      out.push_back(c);
    } else {
      out.push_back('_');
    }
  }
  if (out.empty())
    out = "root";
  return out;
}

struct MovedSubmount {
  std::string from;
  std::string to;
};

bool restoreMovedSubmounts(std::vector<MovedSubmount> &moved,
                           std::string *errorMsg);

bool moveImmediateSubmounts(const std::string &targetMountPoint,
                            const std::string &stagingRoot,
                            std::vector<MovedSubmount> &moved,
                            std::string *errorMsg) {
  moved.clear();
  auto rollback = [&]() {
    std::string ignored;
    restoreMovedSubmounts(moved, &ignored);
  };
  if (!ensureDir(stagingRoot)) {
    if (errorMsg)
      *errorMsg = "Failed to create submount staging dir: " + stagingRoot;
    return false;
  }

  std::vector<std::string> submounts = immediateSubmounts(targetMountPoint);
  for (size_t i = 0; i < submounts.size(); ++i) {
    const std::string &from = submounts[i];
    std::string to = apm::fs::joinPath(
        stagingRoot, sanitizeForPath(std::to_string(i) + "-" + from));
    if (!ensureDir(to)) {
      if (errorMsg)
        *errorMsg = "Failed to create submount staging target: " + to;
      rollback();
      return false;
    }
    if (::mount(from.c_str(), to.c_str(), nullptr, MS_MOVE, nullptr) != 0) {
      if (errorMsg)
        *errorMsg = "Failed to move submount " + from + " to " + to + ": " +
                    std::strerror(errno);
      rollback();
      return false;
    }
    moved.push_back({from, to});
  }
  return true;
}

bool restoreMovedSubmounts(std::vector<MovedSubmount> &moved,
                           std::string *errorMsg) {
  bool ok = true;
  std::string firstErr;
  for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
    if (::mount(it->to.c_str(), it->from.c_str(), nullptr, MS_MOVE, nullptr) !=
        0) {
      ok = false;
      if (firstErr.empty()) {
        firstErr = "Failed to restore submount " + it->to + " -> " + it->from +
                   ": " + std::strerror(errno);
      }
    }
  }
  moved.clear();
  if (!ok && errorMsg) {
    *errorMsg = firstErr;
  }
  return ok;
}

bool resetOverlayScratchDir(const std::string &path, std::string *errorMsg) {
  if (!apm::fs::removeDirRecursive(path)) {
    if (errorMsg)
      *errorMsg = "Failed to clear overlay scratch dir: " + path;
    return false;
  }

  if (!ensureDir(path)) {
    if (errorMsg)
      *errorMsg = "Failed to recreate overlay scratch dir: " + path;
    return false;
  }

  return true;
}

bool kernelParamEnabled(const std::string &name) {
  std::string path = "/sys/module/overlay/parameters/" + name;
  std::string value;
  if (!apm::fs::readFile(path, value))
    return false;
  for (char c : value) {
    if (c == 'Y' || c == 'y' || c == '1' || c == 'T' || c == 't')
      return true;
  }
  return false;
}

std::vector<std::string> buildOverlayOptionSuffixes(bool readOnly) {
  std::vector<std::string> suffixes = {
      "",
      ",override_creds=off",
      ",index=off",
      ",index=off,override_creds=off",
      ",index=off,xino=off",
      ",redirect_dir=off,index=off",
      ",redirect_dir=off,index=off,xino=off",
      ",redirect_dir=off,index=off,xino=off,metacopy=off"};

  if (readOnly) {
    suffixes.push_back(",redirect_dir=off");
    suffixes.push_back(",metacopy=off");
  }

  if (kernelParamEnabled("userxattr")) {
    const size_t existing = suffixes.size();
    for (size_t i = 0; i < existing; ++i) {
      suffixes.push_back(suffixes[i] + ",userxattr");
    }
  }

  return suffixes;
}

bool mountOverlayCompat(const std::string &mountPoint,
                        const std::string &lowerDir,
                        const std::string &upperDir,
                        const std::string &workDir, std::string *errorMsg) {
  std::string firstErr;
  const auto suffixes = buildOverlayOptionSuffixes(false);
  for (const auto &suffix : suffixes) {
    std::ostringstream opts;
    opts << "lowerdir=" << lowerDir << ",upperdir=" << upperDir
         << ",workdir=" << workDir << suffix;
    if (::mount("overlay", mountPoint.c_str(), "overlay", 0,
                opts.str().c_str()) == 0) {
      apm::logger::debug("AMS overlay: mounted " + mountPoint + " with opts: " +
                         opts.str());
      if (errorMsg)
        errorMsg->clear();
      return true;
    }

    std::string attemptErr = std::strerror(errno);
    if (firstErr.empty()) {
      firstErr = std::string("Overlay mount failed for ") + mountPoint + ": " +
                 attemptErr;
    }
    apm::logger::warn("AMS overlay: mount attempt failed for " + mountPoint +
                      " with opts '" + opts.str() + "': " + attemptErr);
  }

  if (errorMsg) {
    *errorMsg = firstErr.empty() ? std::string("Overlay mount failed for ") +
                                       mountPoint
                                 : firstErr;
  }
  return false;
}

bool mountOverlayReadOnlyCompat(const std::string &mountPoint,
                                const std::string &lowerDir,
                                const std::string &mountContext,
                                std::string *errorMsg) {
  const std::array<unsigned long, 2> mountFlags = {MS_RDONLY, 0};
  const auto suffixes = buildOverlayOptionSuffixes(true);
  std::string firstErr;
  for (unsigned long flags : mountFlags) {
    for (const auto &suffix : suffixes) {
      if (!mountContext.empty() && shouldTrySelinuxMountContext()) {
        std::ostringstream optsWithContext;
        optsWithContext << "lowerdir=" << lowerDir << suffix
                        << ",context=" << mountContext;
        if (::mount("overlay", mountPoint.c_str(), "overlay", flags,
                    optsWithContext.str().c_str()) == 0) {
          apm::logger::debug(
              "AMS overlay: mounted " + mountPoint +
              " in read-only mode with flags=" + std::to_string(flags) +
              " opts: " + optsWithContext.str());
          if (errorMsg)
            errorMsg->clear();
          return true;
        }

        const int err = errno;
        disableSelinuxMountContextForBoot(mountPoint, "overlay", err);

        std::string attemptErr = std::strerror(err);
        if (firstErr.empty()) {
          firstErr = std::string("Read-only overlay mount failed for ") +
                     mountPoint + ": " + attemptErr;
        }

        apm::logger::warn("AMS overlay: read-only mount attempt failed for " +
                          mountPoint + " with flags=" +
                          std::to_string(flags) + " opts '" +
                          optsWithContext.str() + "': " + attemptErr);
      }

      std::ostringstream opts;
      opts << "lowerdir=" << lowerDir << suffix;
      if (::mount("overlay", mountPoint.c_str(), "overlay", flags,
                  opts.str().c_str()) == 0) {
        apm::logger::debug("AMS overlay: mounted " + mountPoint +
                           " in read-only mode with flags=" +
                           std::to_string(flags) + " opts: " + opts.str());
        if (errorMsg)
          errorMsg->clear();
        return true;
      }

      std::string attemptErr = std::strerror(errno);
      if (firstErr.empty()) {
        firstErr = std::string("Read-only overlay mount failed for ") +
                   mountPoint + ": " + attemptErr;
      }

      apm::logger::warn("AMS overlay: read-only mount attempt failed for " +
                        mountPoint + " with flags=" +
                        std::to_string(flags) + " opts '" + opts.str() +
                        "': " + attemptErr);
    }
  }

  if (errorMsg) {
    *errorMsg = firstErr.empty()
                    ? std::string("Read-only overlay mount failed for ") +
                          mountPoint
                    : firstErr;
  }
  return false;
}

bool mountOverlayReadOnlyCompat(const std::string &mountPoint,
                                const std::string &lowerDir,
                                std::string *errorMsg) {
  return mountOverlayReadOnlyCompat(mountPoint, lowerDir, "", errorMsg);
}

bool copyLayerIntoUpper(const std::string &layerPath, const std::string &upperDir,
                        std::string *errorMsg) {
  if (!apm::fs::isDirectory(layerPath)) {
    return true;
  }

  std::ostringstream cmd;
  cmd << "cp -a '" << shellEscapeSingleQuotes(layerPath) << "/.' '"
      << shellEscapeSingleQuotes(upperDir) << "'";
  if (!runCommand(cmd.str(), errorMsg)) {
    if (errorMsg && !errorMsg->empty()) {
      *errorMsg = "Failed to compose module overlay layer " + layerPath +
                  " into upperdir: " + *errorMsg;
    }
    return false;
  }
  return true;
}

bool relabelTreeForTarget(const std::string &path, const std::string &context) {
  if (context.empty() || path.empty())
    return true;
  if (!apm::fs::isDirectory(path) && !apm::fs::isFile(path))
    return true;

  std::ostringstream cmd;
  cmd << "chcon -hR '" << shellEscapeSingleQuotes(context) << "' '"
      << shellEscapeSingleQuotes(path) << "'";

  std::string err;
  if (runCommand(cmd.str(), &err))
    return true;

  apm::logger::warn("AMS overlay: failed to relabel staged path " + path +
                    " to " + context + ": " + err);
  return false;
}

bool pathExistsNoFollow(const std::string &path) {
  struct stat st {};
  return ::lstat(path.c_str(), &st) == 0;
}

bool relabelPathFromReference(const std::string &path,
                              const std::string &referencePath) {
  if (path.empty() || referencePath.empty())
    return false;
  if (!pathExistsNoFollow(path) || !pathExistsNoFollow(referencePath))
    return false;

  std::ostringstream readCmd;
  readCmd << "ls -Zd '" << shellEscapeSingleQuotes(referencePath) << "'";
  FILE *pipe = ::popen(readCmd.str().c_str(), "r");
  if (!pipe) {
    apm::logger::warn("AMS overlay: failed to query SELinux label for " +
                      referencePath + ": " + std::strerror(errno));
    return false;
  }

  std::string line;
  char buf[512];
  while (::fgets(buf, sizeof(buf), pipe)) {
    line.append(buf);
  }
  int closeRc = ::pclose(pipe);
  if (closeRc != 0) {
    apm::logger::warn("AMS overlay: failed to read SELinux label for " +
                      referencePath + " (rc=" + std::to_string(closeRc) + ")");
    return false;
  }

  std::istringstream iss(line);
  std::string context;
  if (!(iss >> context) || context.empty() || context == "?") {
    apm::logger::warn("AMS overlay: could not parse SELinux label for " +
                      referencePath + " from: " + line);
    return false;
  }

  std::ostringstream cmd;
  cmd << "chcon -h '" << shellEscapeSingleQuotes(context) << "' '"
      << shellEscapeSingleQuotes(path) << "'";

  std::string err;
  if (runCommand(cmd.str(), &err))
    return true;

  apm::logger::warn("AMS overlay: failed to align staged label for " + path +
                    " from reference " + referencePath + ": " + err);
  return false;
}

void alignStagedTreeLabelsWithBase(const std::string &stagedRoot,
                                   const std::string &baseRoot) {
  if (!pathExistsNoFollow(stagedRoot))
    return;

  struct PendingPath {
    std::string staged;
    std::string base;
  };

  std::vector<PendingPath> stack;
  const auto rootChildren = apm::fs::listDir(stagedRoot, false);
  for (const auto &name : rootChildren) {
    stack.push_back(
        {apm::fs::joinPath(stagedRoot, name), apm::fs::joinPath(baseRoot, name)});
  }

  while (!stack.empty()) {
    PendingPath current = std::move(stack.back());
    stack.pop_back();

    relabelPathFromReference(current.staged, current.base);

    struct stat stagedStat {};
    if (::lstat(current.staged.c_str(), &stagedStat) != 0)
      continue;
    if (!S_ISDIR(stagedStat.st_mode))
      continue;

    const auto children = apm::fs::listDir(current.staged, false);
    for (const auto &name : children) {
      stack.push_back(
          {apm::fs::joinPath(current.staged, name),
           apm::fs::joinPath(current.base, name)});
    }
  }
}

bool mountOverlayWithUpperComposition(const std::string &mountPoint,
                                      const std::vector<std::string> &layers,
                                      const std::string &baseLowerDir,
                                      const std::string &upperDir,
                                      const std::string &workDir,
                                      std::string *errorMsg) {
  std::string localErr;

  if (!resetOverlayScratchDir(upperDir, &localErr)) {
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }
  if (!resetOverlayScratchDir(workDir, &localErr)) {
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }

  // Compose enabled module payloads into upperdir so we can mount overlayfs
  // with a single lowerdir (more compatible on older Android kernels).
  for (const auto &layer : layers) {
    if (!copyLayerIntoUpper(layer, upperDir, &localErr)) {
      if (errorMsg)
        *errorMsg = localErr;
      return false;
    }
  }

  return mountOverlayCompat(mountPoint, baseLowerDir, upperDir, workDir,
                            errorMsg);
}

std::string selinuxContextForTarget(const OverlayTarget &target) {
  if (std::strcmp(target.name, "system") == 0)
    return "u:object_r:system_file:s0";
  if (std::strcmp(target.name, "vendor") == 0)
    return "u:object_r:vendor_file:s0";
  if (std::strcmp(target.name, "product") == 0)
    return "u:object_r:product_file:s0";
  return {};
}

bool ensureTmpfsOverlayStagingRoot(const std::string &root,
                                   const std::string &selinuxContext,
                                   std::string *errorMsg) {
  if (!ensureDir(root)) {
    if (errorMsg)
      *errorMsg = "Failed to create tmpfs overlay staging root: " + root;
    return false;
  }

  // Always ensure a dedicated tmpfs mount at this exact path so SELinux
  // mount options can take effect. Inherited parent tmpfs mounts are not
  // sufficient for labeled system content.
  if (isPathMounted(root)) {
    std::string unmountErr;
    if (!unmountPath(root, &unmountErr)) {
      if (errorMsg)
        *errorMsg = "Failed to reset tmpfs staging root " + root + ": " +
                    unmountErr;
      return false;
    }
  }

  bool mountSucceeded = false;
  std::string firstErr;
  const unsigned long tmpfsFlags = MS_NOSUID | MS_NODEV;

  if (!selinuxContext.empty() && shouldTrySelinuxMountContext()) {
    const std::string optWithContext = "mode=0755,context=" + selinuxContext;
    if (::mount("tmpfs", root.c_str(), "tmpfs", tmpfsFlags,
                optWithContext.c_str()) == 0) {
      mountSucceeded = true;
    } else {
      const int err = errno;
      disableSelinuxMountContextForBoot(root, "tmpfs", err);
      firstErr = "Failed to mount tmpfs staging root " + root + " with opts '" +
                 optWithContext + "': " + std::strerror(err);
      apm::logger::warn(firstErr);
    }
  }

  if (!mountSucceeded) {
    const std::string opt = "mode=0755";
    if (::mount("tmpfs", root.c_str(), "tmpfs", tmpfsFlags, opt.c_str()) ==
        0) {
      mountSucceeded = true;
    } else if (firstErr.empty()) {
      const int err = errno;
      firstErr = "Failed to mount tmpfs staging root " + root + " with opts '" +
                 opt + "': " + std::strerror(err);
    }
  }

  if (!mountSucceeded) {
    if (errorMsg)
      *errorMsg = firstErr.empty()
                      ? ("Failed to mount tmpfs staging root " + root)
                      : firstErr;
    return false;
  }

  std::string fsType;
  fsType.clear();
  if (!readMountType(root, fsType) || fsType != "tmpfs") {
    if (errorMsg)
      *errorMsg = "Overlay staging root " + root +
                  " is not tmpfs (detected " +
                  (fsType.empty() ? std::string("unknown") : fsType) + ")";
    return false;
  }
  return true;
}

bool mountOverlayWithTmpfsStagedLower(const OverlayTarget &target,
                                      const std::string &mountPoint,
                                      const std::vector<std::string> &layers,
                                      std::string *errorMsg) {
  if (layers.empty()) {
    if (errorMsg)
      *errorMsg = "No overlay layers to stage into tmpfs";
    return false;
  }

  std::string localErr;
  const std::string stagingRoot =
      "/mnt/ams-overlay-" + std::string(target.name);
  if (!ensureTmpfsOverlayStagingRoot(stagingRoot,
                                     selinuxContextForTarget(target),
                                     &localErr)) {
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }

  const std::string targetRoot = apm::fs::joinPath(stagingRoot, target.name);
  const std::string stagedLayer = apm::fs::joinPath(targetRoot, "layer");
  if (!resetOverlayScratchDir(stagedLayer, &localErr)) {
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }

  for (const auto &layer : layers) {
    if (!copyLayerIntoUpper(layer, stagedLayer, &localErr)) {
      if (errorMsg) {
        *errorMsg = "Failed to stage module layer into tmpfs for " +
                    std::string(target.name) + ": " + localErr;
      }
      return false;
    }
  }

  relabelTreeForTarget(stagedLayer, selinuxContextForTarget(target));
  alignStagedTreeLabelsWithBase(stagedLayer, baseMirrorPath(target));

  const std::string lowerDir = stagedLayer + ":" + baseMirrorPath(target);
  if (!mountOverlayReadOnlyCompat(mountPoint, lowerDir,
                                  selinuxContextForTarget(target), &localErr)) {
    if (errorMsg)
      *errorMsg = "tmpfs staged lowerdir fallback failed for " +
                  std::string(target.name) + ": " + localErr;
    return false;
  }

  apm::logger::info("AMS overlay: mounted " + mountPoint +
                    " using tmpfs-staged read-only lowerdir fallback");
  if (errorMsg)
    errorMsg->clear();
  return true;
}

std::string bindMountStatePath(const OverlayTarget &target) {
  return apm::fs::joinPath(apm::config::getModuleRuntimeDir(),
                           std::string("bind-mounts-") + target.name + ".txt");
}

bool saveBindMountState(const OverlayTarget &target,
                        const std::vector<std::string> &mountedTargets,
                        std::string *errorMsg) {
  std::ostringstream out;
  for (const auto &path : mountedTargets) {
    out << path << "\n";
  }

  const std::string statePath = bindMountStatePath(target);
  if (!apm::fs::writeFile(statePath, out.str(), true)) {
    if (errorMsg)
      *errorMsg = "Failed to persist bind mount state at " + statePath;
    return false;
  }
  ::chmod(statePath.c_str(), 0600);
  return true;
}

bool loadBindMountState(const OverlayTarget &target,
                        std::vector<std::string> &mountedTargets) {
  mountedTargets.clear();
  std::string raw;
  if (!apm::fs::readFile(bindMountStatePath(target), raw))
    return true;

  std::istringstream in(raw);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() &&
           (line.back() == '\n' || line.back() == '\r' || line.back() == ' ' ||
            line.back() == '\t')) {
      line.pop_back();
    }
    if (!line.empty())
      mountedTargets.push_back(line);
  }

  std::sort(mountedTargets.begin(), mountedTargets.end());
  mountedTargets.erase(std::unique(mountedTargets.begin(), mountedTargets.end()),
                       mountedTargets.end());
  return true;
}

bool hasTrackedBindMounts(const OverlayTarget &target) {
  std::vector<std::string> mountedTargets;
  if (!loadBindMountState(target, mountedTargets))
    return false;
  return !mountedTargets.empty();
}

bool unmountTrackedBindMounts(const OverlayTarget &target, std::string *errorMsg) {
  std::vector<std::string> mountedTargets;
  if (!loadBindMountState(target, mountedTargets)) {
    if (errorMsg)
      *errorMsg = "Failed to load bind mount state for " + std::string(target.name);
    return false;
  }

  for (auto it = mountedTargets.rbegin(); it != mountedTargets.rend(); ++it) {
    std::string unmountErr;
    if (!unmountPath(*it, &unmountErr)) {
      if (errorMsg)
        *errorMsg = "Failed to unmount tracked bind mount " + *it + ": " +
                    unmountErr;
      return false;
    }
  }

  if (!saveBindMountState(target, {}, errorMsg))
    return false;
  return true;
}

bool copyDirectoryContents(const std::string &srcDir, const std::string &dstDir,
                           std::string *errorMsg) {
  if (!apm::fs::isDirectory(srcDir))
    return true;
  if (!ensureDir(dstDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create directory " + dstDir;
    return false;
  }

  std::ostringstream cmd;
  cmd << "cp -a '" << shellEscapeSingleQuotes(srcDir) << "/.' '"
      << shellEscapeSingleQuotes(dstDir) << "'";
  if (!runCommand(cmd.str(), errorMsg)) {
    if (errorMsg && !errorMsg->empty()) {
      *errorMsg = "Failed to copy directory contents from " + srcDir + " to " +
                  dstDir + ": " + *errorMsg;
    }
    return false;
  }
  return true;
}

bool bindMountReadOnly(const std::string &source, const std::string &target,
                       std::string *errorMsg) {
  if (::mount(source.c_str(), target.c_str(), nullptr, MS_BIND, nullptr) != 0) {
    if (errorMsg) {
      *errorMsg = "Failed to bind mount " + source + " -> " + target + ": " +
                  std::strerror(errno);
    }
    return false;
  }

  if (::mount(nullptr, target.c_str(), nullptr,
              MS_REMOUNT | MS_BIND | MS_RDONLY, nullptr) != 0) {
    apm::logger::warn("Failed to remount bind target read-only " + target +
                      ": " + std::strerror(errno));
  }
  apm::logger::debug("AMS overlay: bind backend file target " + target +
                     " source=" + source);
  return true;
}

enum class PathNodeKind { Missing, Directory, Regular, Other };

PathNodeKind pathNodeKindNoFollow(const std::string &path) {
  struct stat st {};
  if (::lstat(path.c_str(), &st) != 0)
    return PathNodeKind::Missing;
  if (S_ISDIR(st.st_mode))
    return PathNodeKind::Directory;
  if (S_ISREG(st.st_mode))
    return PathNodeKind::Regular;
  return PathNodeKind::Other;
}

bool mountBindBackendSubdirOverlay(const OverlayTarget &target,
                                   const std::string &stagedDir,
                                   const std::string &baseDir,
                                   const std::string &targetDir,
                                   std::vector<std::string> &mountedNow,
                                   std::string *errorMsg) {
  const std::string lowerDir = stagedDir + ":" + baseDir;
  apm::logger::debug("AMS overlay: bind backend overlay target " + targetDir +
                     " lowerdir=" + lowerDir);
  if (!mountOverlayReadOnlyCompat(targetDir, lowerDir,
                                  selinuxContextForTarget(target), errorMsg)) {
    return false;
  }
  mountedNow.push_back(targetDir);
  return true;
}

bool applyBindBackendRecursively(const OverlayTarget &target,
                                 const std::string &stagedDir,
                                 const std::string &baseDir,
                                 const std::string &targetDir,
                                 std::vector<std::string> &mountedNow,
                                 std::string *errorMsg) {
  if (!apm::fs::isDirectory(stagedDir) || !apm::fs::isDirectory(baseDir) ||
      !apm::fs::isDirectory(targetDir)) {
    if (errorMsg) {
      *errorMsg = "Bind backend requires directory triad staged/base/target at " +
                  stagedDir + ", " + baseDir + ", " + targetDir;
    }
    return false;
  }

  const std::vector<std::string> entries = apm::fs::listDir(stagedDir, false);
  if (entries.empty())
    return true;

  bool needOverlayHere = false;
  for (const auto &entry : entries) {
    const std::string stagedEntry = apm::fs::joinPath(stagedDir, entry);
    const std::string baseEntry = apm::fs::joinPath(baseDir, entry);
    const std::string targetEntry = apm::fs::joinPath(targetDir, entry);

    struct stat stagedStat {};
    if (::lstat(stagedEntry.c_str(), &stagedStat) != 0) {
      if (errorMsg) {
        *errorMsg = "Failed to stat staged entry " + stagedEntry + ": " +
                    std::strerror(errno);
      }
      return false;
    }

    const PathNodeKind baseKind = pathNodeKindNoFollow(baseEntry);
    const PathNodeKind targetKind = pathNodeKindNoFollow(targetEntry);

    if (S_ISDIR(stagedStat.st_mode)) {
      if (baseKind != PathNodeKind::Directory ||
          targetKind != PathNodeKind::Directory) {
        needOverlayHere = true;
        break;
      }
      continue;
    }

    if (S_ISREG(stagedStat.st_mode)) {
      if (baseKind != PathNodeKind::Regular ||
          targetKind != PathNodeKind::Regular) {
        needOverlayHere = true;
        break;
      }
      continue;
    }

    // Symlinks/special files need overlay semantics.
    needOverlayHere = true;
    break;
  }

  if (needOverlayHere) {
    return mountBindBackendSubdirOverlay(target, stagedDir, baseDir, targetDir,
                                         mountedNow, errorMsg);
  }

  for (const auto &entry : entries) {
    const std::string stagedEntry = apm::fs::joinPath(stagedDir, entry);
    const std::string baseEntry = apm::fs::joinPath(baseDir, entry);
    const std::string targetEntry = apm::fs::joinPath(targetDir, entry);

    struct stat stagedStat {};
    if (::lstat(stagedEntry.c_str(), &stagedStat) != 0) {
      if (errorMsg) {
        *errorMsg = "Failed to stat staged entry " + stagedEntry + ": " +
                    std::strerror(errno);
      }
      return false;
    }

    if (S_ISDIR(stagedStat.st_mode)) {
      if (!applyBindBackendRecursively(target, stagedEntry, baseEntry,
                                       targetEntry, mountedNow, errorMsg)) {
        return false;
      }
      continue;
    }

    if (S_ISREG(stagedStat.st_mode)) {
      std::string localErr;
      if (!bindMountReadOnly(stagedEntry, targetEntry, &localErr)) {
        if (errorMsg)
          *errorMsg = localErr;
        return false;
      }
      mountedNow.push_back(targetEntry);
    }
  }

  return true;
}

bool applyBindMountBackendForTarget(const OverlayTarget &target,
                                    const std::string &mountPoint,
                                    const std::vector<std::string> &layers,
                                    std::string *errorMsg) {
  std::string localErr;
  if (!unmountTrackedBindMounts(target, &localErr)) {
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }

  if (layers.empty()) {
    if (errorMsg)
      errorMsg->clear();
    return true;
  }

  const std::string stagingRoot =
      "/mnt/ams-overlay-" + std::string(target.name);
  if (!ensureTmpfsOverlayStagingRoot(stagingRoot,
                                     selinuxContextForTarget(target),
                                     &localErr)) {
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }

  const std::string targetRoot = apm::fs::joinPath(stagingRoot, target.name);
  const std::string stagedLayer = apm::fs::joinPath(targetRoot, "bind-layer");
  if (!resetOverlayScratchDir(stagedLayer, &localErr)) {
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }

  for (const auto &layer : layers) {
    if (!copyLayerIntoUpper(layer, stagedLayer, &localErr)) {
      if (errorMsg)
        *errorMsg = "Failed to stage bind layer for " +
                    std::string(target.name) + ": " + localErr;
      return false;
    }
  }

  const std::string baseTargetRoot = baseMirrorPath(target);
  relabelTreeForTarget(stagedLayer, selinuxContextForTarget(target));
  alignStagedTreeLabelsWithBase(stagedLayer, baseTargetRoot);

  std::vector<std::string> entries = apm::fs::listDir(stagedLayer, false);
  std::vector<std::string> mountedNow;
  auto rollback = [&]() {
    for (auto it = mountedNow.rbegin(); it != mountedNow.rend(); ++it) {
      std::string ignored;
      unmountPath(*it, &ignored);
    }
  };

  for (const auto &entry : entries) {
    const std::string stagedEntry = apm::fs::joinPath(stagedLayer, entry);
    const std::string baseEntry = apm::fs::joinPath(baseTargetRoot, entry);
    const std::string targetEntry = apm::fs::joinPath(mountPoint, entry);

    struct stat stagedStat {};
    if (::lstat(stagedEntry.c_str(), &stagedStat) != 0) {
      localErr = "Failed to stat staged entry " + stagedEntry + ": " +
                 std::strerror(errno);
      rollback();
      if (errorMsg)
        *errorMsg = localErr;
      return false;
    }

    if (S_ISDIR(stagedStat.st_mode)) {
      if (!apm::fs::isDirectory(baseEntry) || !apm::fs::isDirectory(targetEntry)) {
        localErr =
            "Bind backend requires existing top-level directories at " +
            baseEntry + " and " + targetEntry;
        rollback();
        if (errorMsg)
          *errorMsg = localErr;
        return false;
      }

      if (!applyBindBackendRecursively(target, stagedEntry, baseEntry, targetEntry,
                                       mountedNow, &localErr)) {
        rollback();
        if (errorMsg)
          *errorMsg = localErr;
        return false;
      }
      continue;
    }

    if (S_ISREG(stagedStat.st_mode)) {
      if (!apm::fs::isFile(baseEntry) || !apm::fs::isFile(targetEntry)) {
        localErr = "Bind backend requires existing top-level files at " +
                   baseEntry + " and " + targetEntry;
        rollback();
        if (errorMsg)
          *errorMsg = localErr;
        return false;
      }
      if (!bindMountReadOnly(stagedEntry, targetEntry, &localErr)) {
        rollback();
        if (errorMsg)
          *errorMsg = localErr;
        return false;
      }
      mountedNow.push_back(targetEntry);
      continue;
    }

    if (!mountBindBackendSubdirOverlay(target, stagedLayer, baseTargetRoot,
                                       mountPoint, mountedNow, &localErr)) {
      rollback();
      if (errorMsg)
        *errorMsg = "Failed to mount fallback read-only overlay for " +
                    mountPoint + ": " + localErr;
      return false;
    }
    break;
  }

  if (!saveBindMountState(target, mountedNow, &localErr)) {
    rollback();
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }

  apm::logger::info("AMS overlay: mounted " + mountPoint +
                    " using bind backend");
  if (errorMsg)
    errorMsg->clear();
  return true;
}

} // namespace

namespace apm::ams {

ModuleManager::ModuleManager()
    : modulesRoot_(apm::config::getModulesDir()),
      logsRoot_(apm::config::getModuleLogsDir()) {
  apm::fs::createDirs(modulesRoot_);
  apm::fs::createDirs(logsRoot_);
}

ModuleManager::~ModuleManager() { stopPartitionMonitor(); }

std::string ModuleManager::modulePath(const std::string &name) const {
  return apm::fs::joinPath(modulesRoot_, name);
}

std::string ModuleManager::moduleLogPath(const std::string &name) const {
  return apm::fs::joinPath(logsRoot_, name + ".log");
}

bool ModuleManager::extractZip(const std::string &zipPath,
                               const std::string &dest,
                               std::string *errorMsg) const {
  if (!ensureDir(dest)) {
    if (errorMsg)
      *errorMsg = "Failed to create temp directory: " + dest;
    return false;
  }

  std::ostringstream cmd;
  cmd << "unzip -oq '" << shellEscapeSingleQuotes(zipPath) << "' -d '"
      << shellEscapeSingleQuotes(dest) << "'";

  if (!runCommand(cmd.str(), errorMsg)) {
    return false;
  }

  return true;
}

std::string ModuleManager::makeTempDir(const std::string &tag) const {
  using namespace std::chrono;
  static std::uint64_t counter = 0;
  auto now = duration_cast<microseconds>(steady_clock::now().time_since_epoch())
                 .count();
  std::ostringstream name;
  name << "ams-" << tag << "-" << now << "-" << ++counter;
  auto path = apm::fs::joinPath(apm::config::getCacheDir(), name.str());
  if (!ensureDir(path))
    return {};
  return path;
}

void ModuleManager::logModuleEvent(const std::string &name,
                                   const std::string &message) const {
  apm::fs::createDirs(logsRoot_);
  std::ostringstream line;
  line << "[" << makeIsoTimestamp() << "] " << message << "\n";
  apm::fs::appendFile(moduleLogPath(name), line.str());
  apm::logger::info("AMS(" + name + "): " + message);
}

bool ModuleManager::installFromZip(const std::string &zipPath,
                                   ModuleOperationResult &out) {
  out = ModuleOperationResult{};
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (zipPath.empty()) {
    out.message = "Module zip path is empty";
    return false;
  }
  if (!isRegularFile(zipPath)) {
    out.message = "Module zip not found: " + zipPath;
    return false;
  }

  std::string tempDir = makeTempDir("zip");
  if (tempDir.empty()) {
    out.message = "Failed to create temporary directory";
    return false;
  }
  ScopedTempDir cleanupTemp(tempDir);

  if (!extractZip(zipPath, tempDir, &out.message)) {
    return false;
  }

  std::string moduleRoot = tempDir;
  std::string infoPath = apm::fs::joinPath(moduleRoot, "module-info.json");
  if (!isRegularFile(infoPath)) {
    auto entries = apm::fs::listDir(tempDir, false);
    if (entries.size() == 1) {
      std::string candidate = apm::fs::joinPath(tempDir, entries[0]);
      std::string nestedInfo = apm::fs::joinPath(candidate, "module-info.json");
      if (apm::fs::isDirectory(candidate) && isRegularFile(nestedInfo)) {
        moduleRoot = candidate;
        infoPath = nestedInfo;
      }
    }
  }

  ModuleInfo info;
  std::string err;
  if (!readModuleInfoFile(infoPath, info, &err)) {
    out.message = err;
    return false;
  }

  std::string overlayRoot = apm::fs::joinPath(moduleRoot, "overlay");
  if (!apm::fs::isDirectory(overlayRoot)) {
    out.message = "Module overlay directory missing";
    return false;
  }

  std::string finalPath = modulePath(info.name);
  apm::fs::removeDirRecursive(finalPath);

  apm::fs::createDirs(modulesRoot_);
  if (::rename(moduleRoot.c_str(), finalPath.c_str()) != 0) {
    out.message = std::string("Failed to move module into place: ") +
                  std::strerror(errno);
    return false;
  }

  // moduleRoot has been moved; prevent cleanup from deleting the new root.
  cleanupTemp.release();
  apm::fs::removeDirRecursive(tempDir);

  std::string workDir = apm::fs::joinPath(finalPath, "workdir");
  apm::fs::createDirs(workDir);
  for (const auto &target : kOverlayTargets) {
    apm::fs::createDirs(apm::fs::joinPath(workDir, target.name));
  }

  ModuleState state;
  state.enabled = true;
  state.installedAt = makeIsoTimestamp();
  state.updatedAt = state.installedAt;
  state.lastError.clear();

  if (!writeState(finalPath, state, &err)) {
    out.message = err;
    return false;
  }

  logModuleEvent(info.name, "Installed module from " + zipPath);

  if (info.runInstallSh) {
    const std::string installScript = apm::fs::joinPath(finalPath, "install.sh");
    std::string installErr;
    if (!runScript(installScript, info.name, false, true, &installErr)) {
      const std::string scriptFailure =
          installErr.empty() ? "install.sh failed during module install"
                             : installErr;
      const bool removedModule = apm::fs::removeDirRecursive(finalPath);
      const bool removedLog = apm::fs::removeFile(moduleLogPath(info.name));

      std::string rollbackErr;
      if (!applyEnabledModules(&rollbackErr) && !rollbackErr.empty()) {
        apm::logger::warn("AMS rollback: failed to rebuild overlays after "
                          "install.sh failure for " +
                          info.name + ": " + rollbackErr);
      }

      std::ostringstream message;
      message << scriptFailure << "; module installation rolled back";
      if (!removedModule)
        message << "; rollback failed to remove module directory";
      if (!removedLog)
        message << "; rollback failed to remove module log";
      if (!rollbackErr.empty())
        message << "; overlay rebuild warning: " << rollbackErr;
      out.message = message.str();
      return false;
    }
  }

  if (!applyEnabledModules(&err)) {
    state.lastError = err;
    writeState(finalPath, state, nullptr);
    out.message = err;
    return false;
  }

  runLifecycleScripts(info, finalPath, false);

  out.ok = true;
  out.message = "Installed module '" + info.name + "'";
  return true;
}

bool ModuleManager::loadModule(const std::string &name, ModuleInfo &info,
                               ModuleState &state,
                               std::string *errorMsg) const {
  std::string dir = modulePath(name);
  if (!apm::fs::isDirectory(dir)) {
    if (errorMsg)
      *errorMsg = "Module not found: " + name;
    return false;
  }

  std::string infoPath = apm::fs::joinPath(dir, "module-info.json");
  if (!readModuleInfoFile(infoPath, info, errorMsg))
    return false;

  std::string statePath = apm::fs::joinPath(dir, "state.json");
  if (!readModuleState(statePath, state, errorMsg))
    return false;

  if (state.installedAt.empty())
    state.installedAt = makeIsoTimestamp();
  return true;
}

bool ModuleManager::writeState(const std::string &moduleDir, ModuleState &state,
                               std::string *errorMsg) const {
  state.updatedAt = makeIsoTimestamp();
  std::string path = apm::fs::joinPath(moduleDir, "state.json");
  return writeModuleState(path, state, errorMsg);
}

bool ModuleManager::enableModule(const std::string &name,
                                 ModuleOperationResult &out) {
  out = ModuleOperationResult{};
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ModuleInfo info;
  ModuleState state;
  std::string err;
  if (!loadModule(name, info, state, &err)) {
    out.message = err;
    return false;
  }
  if (state.enabled) {
    out.ok = true;
    out.message = "Module already enabled";
    return true;
  }

  state.enabled = true;
  state.lastError.clear();
  std::string dir = modulePath(name);
  if (!writeState(dir, state, &err)) {
    out.message = err;
    return false;
  }

  logModuleEvent(name, "Module enabled");

  if (!applyEnabledModules(&err)) {
    state.lastError = err;
    writeState(dir, state, nullptr);
    out.message = err;
    return false;
  }

  runLifecycleScripts(info, dir, false);

  out.ok = true;
  out.message = "Module enabled";
  return true;
}

bool ModuleManager::disableModule(const std::string &name,
                                  ModuleOperationResult &out) {
  out = ModuleOperationResult{};
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ModuleInfo info;
  ModuleState state;
  std::string err;
  if (!loadModule(name, info, state, &err)) {
    out.message = err;
    return false;
  }
  if (!state.enabled) {
    out.ok = true;
    out.message = "Module already disabled";
    return true;
  }

  state.enabled = false;
  std::string dir = modulePath(name);
  if (!writeState(dir, state, &err)) {
    out.message = err;
    return false;
  }

  logModuleEvent(name, "Module disabled");

  if (!applyEnabledModules(&err)) {
    state.lastError = err;
    writeState(dir, state, nullptr);
    out.message = err;
    return false;
  }

  out.ok = true;
  out.message = "Module disabled";
  return true;
}

bool ModuleManager::removeModule(const std::string &name,
                                 ModuleOperationResult &out) {
  out = ModuleOperationResult{};
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  ModuleInfo info;
  ModuleState state;
  std::string err;
  if (!loadModule(name, info, state, &err)) {
    out.message = err;
    return false;
  }

  if (state.enabled) {
    ModuleOperationResult dummy;
    disableModule(name, dummy);
  }

  std::string dir = modulePath(name);
  apm::fs::removeDirRecursive(dir);
  apm::fs::removeFile(moduleLogPath(name));

  logModuleEvent(name, "Module removed");

  applyEnabledModules(nullptr);

  out.ok = true;
  out.message = "Module removed";
  return true;
}

bool ModuleManager::ensureRuntimeDirs(std::string *errorMsg) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (!ensureDir(apm::config::getModuleRuntimeDir())) {
    if (errorMsg)
      *errorMsg = "Failed to create runtime dir: " +
                  apm::config::getModuleRuntimeDir();
    return false;
  }
  if (!ensureDir(apm::config::getModuleRuntimeUpperDir())) {
    if (errorMsg)
      *errorMsg = "Failed to create runtime upper dir";
    return false;
  }
  if (!ensureDir(apm::config::getModuleRuntimeWorkDir())) {
    if (errorMsg)
      *errorMsg = "Failed to create runtime work dir";
    return false;
  }
  if (!ensureDir(apm::config::getModuleRuntimeBaseDir())) {
    if (errorMsg)
      *errorMsg = "Failed to create runtime base dir";
    return false;
  }
  for (const auto &target : kOverlayTargets) {
    if (!ensureDir(apm::fs::joinPath(apm::config::getModuleRuntimeUpperDir(),
                                     target.name))) {
      if (errorMsg)
        *errorMsg =
            "Failed to prepare upper dir for " + std::string(target.name);
      return false;
    }
    if (!ensureDir(apm::fs::joinPath(apm::config::getModuleRuntimeWorkDir(),
                                     target.name))) {
      if (errorMsg)
        *errorMsg =
            "Failed to prepare work dir for " + std::string(target.name);
      return false;
    }
    if (!ensureDir(apm::fs::joinPath(apm::config::getModuleRuntimeBaseDir(),
                                     target.name))) {
      if (errorMsg)
        *errorMsg =
            "Failed to prepare base dir for " + std::string(target.name);
      return false;
    }
  }
  return true;
}

ModuleManager::OverlayStacks ModuleManager::buildOverlayStacks() const {
  OverlayStacks stacks;

  std::vector<std::string> modules = apm::fs::listDir(modulesRoot_, false);
  for (const auto &name : modules) {
    std::string dir = modulePath(name);
    if (!apm::fs::isDirectory(dir))
      continue;

    ModuleInfo info;
    ModuleState state;
    if (!loadModule(name, info, state, nullptr))
      continue;

    if (!state.enabled || !info.mount)
      continue;

    std::string overlayRoot = apm::fs::joinPath(dir, "overlay");
    if (!apm::fs::isDirectory(overlayRoot))
      continue;

    for (const auto &target : kOverlayTargets) {
      std::string subdir =
          apm::fs::joinPath(overlayRoot, std::string(target.name));
      if (apm::fs::isDirectory(subdir)) {
        stacks[target.name].push_back(subdir);
      }
    }
  }

  return stacks;
}

bool ModuleManager::applyOverlayForTarget(std::size_t targetIndex,
                                          const std::vector<std::string> &layers,
                                          std::string *errorMsg) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (targetIndex >= std::size(kOverlayTargets)) {
    if (errorMsg)
      *errorMsg = "Invalid overlay target index";
    return false;
  }

  const auto &target = kOverlayTargets[targetIndex];
  std::string localErr;

  if (!ensureRuntimeDirs(&localErr)) {
    apm::logger::error("AMS overlay: " + localErr);
    if (errorMsg)
      *errorMsg = localErr;
    return false;
  }

  const auto candidates = mountCandidatesForTarget(target);
  std::string firstErr;
  bool triedAnyMountedCandidate = false;

  for (const auto &mountPoint : candidates) {
    if (!isOverlayCandidateReady(target, mountPoint)) {
      apm::logger::info("AMS overlay: deferring " + mountPoint +
                        " until partition is mounted");
      continue;
    }
    triedAnyMountedCandidate = true;
    apm::logger::info("AMS overlay: trying target '" + mountPoint +
                      "' for " + std::string(target.name));

    auto failCandidate = [&](const std::string &stage,
                             const std::string &detail) {
      std::string msg = stage + ": " + detail;
      if (firstErr.empty())
        firstErr = msg;
      apm::logger::warn("AMS overlay: candidate " + mountPoint + " failed at " +
                        stage + ": " + detail);
    };

    if (!ensureBaseMirrorForTarget(target, mountPoint, &localErr)) {
      failCandidate("pre-base", localErr);
      continue;
    }

    std::string mountType;
    const bool overlayActive =
        readMountType(mountPoint, mountType) && mountType == "overlay";

    if (overlayActive) {
      if (!mountBaseOnly(target, mountPoint, &localErr)) {
        failCandidate("pre-restore-base", localErr);
        continue;
      }
    }

    if (!applyBindMountBackendForTarget(target, mountPoint, layers, &localErr)) {
      failCandidate("bind-backend", localErr);
      continue;
    }
    apm::logger::info("AMS overlay updated for " + mountPoint);
    if (errorMsg)
      errorMsg->clear();
    return true;

    std::string propagationPath = mountPoint;
    bool wasShared = isMountShared(propagationPath);
    bool propagationChanged = false;
    int propErrno = 0;
    if (!setMountPropagation(propagationPath, MS_PRIVATE, &localErr,
                             &propErrno)) {
      if (mountPoint == "/system" && propErrno == EINVAL) {
        propagationPath = "/";
        wasShared = isMountShared(propagationPath);
        if (!setMountPropagation(propagationPath, MS_PRIVATE, &localErr,
                                 &propErrno)) {
          failCandidate("pre-private", localErr);
          continue;
        }
      } else {
        failCandidate("pre-private", localErr);
        continue;
      }
    }
    propagationChanged = true;

    std::vector<MovedSubmount> moved;
    const std::string moveRoot = apm::fs::joinPath(
        apm::config::getModuleRuntimeDir(),
        "move-" + std::string(target.name) + "-" + sanitizeForPath(mountPoint));
    if (mountPoint == "/") {
      apm::logger::info(
          "AMS overlay: skipping submount move path for '/' candidate");
    } else if (!moveImmediateSubmounts(mountPoint, moveRoot, moved, &localErr)) {
      failCandidate("move-submount", localErr);
      if (propagationChanged && wasShared) {
        setMountPropagation(propagationPath, MS_SHARED, nullptr);
      }
      continue;
    }

    if (mountPoint != "/" && !unmountPath(mountPoint, &localErr)) {
      failCandidate("pre-unmount", localErr);
      std::string restoreErr;
      restoreMovedSubmounts(moved, &restoreErr);
      if (propagationChanged && wasShared) {
        setMountPropagation(propagationPath, MS_SHARED, nullptr);
      }
      continue;
    }

    bool mounted = false;
    if (layers.empty()) {
      mounted = mountBaseOnly(target, mountPoint, &localErr);
    } else {
      if (std::strcmp(target.name, "system") == 0 &&
          mountPoint == "/system") {
        if (mountOverlayWithTmpfsStagedLower(target, mountPoint, layers,
                                             &localErr)) {
          mounted = true;
        } else {
          apm::logger::warn("AMS overlay: tmpfs staged fallback precheck failed "
                            "for /system, continuing with compatibility "
                            "strategy chain: " +
                            localErr);
        }
      }

      std::ostringstream lower;
      for (size_t i = 0; i < layers.size(); ++i) {
        if (i > 0)
          lower << ':';
        lower << layers[i];
      }
      lower << ':' << baseMirrorPath(target);

      std::string upperDir =
          apm::fs::joinPath(apm::config::getModuleRuntimeUpperDir(), target.name);
      std::string workDir =
          apm::fs::joinPath(apm::config::getModuleRuntimeWorkDir(), target.name);

      if (!mounted) {
        if (!resetOverlayScratchDir(upperDir, &localErr) ||
            !resetOverlayScratchDir(workDir, &localErr)) {
          mounted = false;
        } else if (mountOverlayCompat(mountPoint, lower.str(), upperDir, workDir,
                                      &localErr)) {
          mounted = true;
        } else if (mountOverlayReadOnlyCompat(
                       mountPoint, lower.str(), selinuxContextForTarget(target),
                       &localErr)) {
          mounted = true;
        } else if (mountOverlayWithTmpfsStagedLower(target, mountPoint, layers,
                                                    &localErr)) {
          mounted = true;
        } else {
          const std::string baseOnlyLower = baseMirrorPath(target);
          mounted = mountOverlayWithUpperComposition(mountPoint, layers,
                                                     baseOnlyLower, upperDir,
                                                     workDir, &localErr);
        }
      }
    }

    std::string restoreErr;
    if (!restoreMovedSubmounts(moved, &restoreErr)) {
      failCandidate("restore-submount", restoreErr);
      mounted = false;
    }

    if (propagationChanged && wasShared) {
      std::string propRestoreErr;
      if (!setMountPropagation(propagationPath, MS_SHARED, &propRestoreErr)) {
        failCandidate("restore-propagation", propRestoreErr);
      }
    }

    if (mounted) {
      apm::logger::info("AMS overlay updated for " + mountPoint);
      if (errorMsg)
        errorMsg->clear();
      return true;
    }

    failCandidate("overlay-mount", localErr);
    mountBaseOnly(target, mountPoint, nullptr);
  }

  if (!triedAnyMountedCandidate) {
    if (errorMsg)
      errorMsg->clear();
    return true;
  }

  if (errorMsg) {
    *errorMsg = firstErr.empty() ? "Overlay mount failed for target " +
                                       std::string(target.name)
                                 : firstErr;
  }
  return false;
}

bool ModuleManager::rebuildOverlays(std::string *errorMsg) {
  if (!ensureRuntimeDirs(errorMsg))
    return false;

  OverlayStacks stacks = buildOverlayStacks();
  bool ok = true;
  std::string firstErr;

  for (std::size_t idx = 0; idx < std::size(kOverlayTargets); ++idx) {
    std::vector<std::string> layers;
    auto it = stacks.find(kOverlayTargets[idx].name);
    if (it != stacks.end())
      layers = it->second;

    std::string perErr;
    if (!applyOverlayForTarget(idx, layers, &perErr)) {
      ok = false;
      if (firstErr.empty() && !perErr.empty())
        firstErr = perErr;
      else if (perErr.empty() && firstErr.empty())
        firstErr = "Overlay apply failed for " +
                   effectiveMountPoint(kOverlayTargets[idx]);
    }
  }

  if (errorMsg) {
    if (!firstErr.empty())
      *errorMsg = firstErr;
    else
      errorMsg->clear();
  }

  return ok;
}

bool ModuleManager::runScript(const std::string &path,
                              const std::string &moduleName,
                              bool background, bool requireExists,
                              std::string *errorMsg) const {
  if (!isRegularFile(path)) {
    if (requireExists) {
      std::string msg = "Required script missing: " + path;
      logModuleEvent(moduleName, msg);
      if (errorMsg)
        *errorMsg = msg;
      return false;
    }
    return true;
  }

  std::string logPath = moduleLogPath(moduleName);
  std::ostringstream cmd;
  cmd << "/system/bin/sh '" << shellEscapeSingleQuotes(path) << "' >>'"
      << shellEscapeSingleQuotes(logPath) << "' 2>&1";
  if (background)
    cmd << " &";

  if (apm::logger::isDebugEnabled()) {
    apm::logger::debug(std::string(kLogFileTag) + ": runScript module='" +
                       moduleName + "' cmd='" + cmd.str() + "'");
  }

  int rc = ::system(cmd.str().c_str());
  if (apm::logger::isDebugEnabled()) {
    apm::logger::debug(std::string(kLogFileTag) +
                       ": runScript result module='" + moduleName +
                       "' rc=" + std::to_string(rc));
  }
  if (rc != 0) {
    std::string msg = "Script failed: " + path + " (rc=" + std::to_string(rc) + ")";
    logModuleEvent(moduleName, msg);
    if (errorMsg)
      *errorMsg = msg;
    return false;
  }

  logModuleEvent(moduleName, std::string("Executed script: ") + path +
                                 (background ? " (background)" : ""));
  if (errorMsg)
    errorMsg->clear();
  return true;
}

bool ModuleManager::runLifecycleScripts(const ModuleInfo &info,
                                        const std::string &moduleDir,
                                        bool isStartup) const {
  (void)isStartup;
  bool ok = true;
  if (info.runPostFsData) {
    std::string script = apm::fs::joinPath(moduleDir, "post-fs-data.sh");
    ok &= runScript(script, info.name, false);
  }
  if (info.runService) {
    std::string script = apm::fs::joinPath(moduleDir, "service.sh");
    ok &= runScript(script, info.name, true);
  }
  return ok;
}

bool ModuleManager::applyEnabledModules(std::string *errorMsg) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return rebuildOverlays(errorMsg);
}

void ModuleManager::startEnabledModules() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto modules = apm::fs::listDir(modulesRoot_, false);
  for (const auto &name : modules) {
    ModuleInfo info;
    ModuleState state;
    if (!loadModule(name, info, state, nullptr))
      continue;
    if (!state.enabled)
      continue;
    runLifecycleScripts(info, modulePath(name), true);
  }
}

bool ModuleManager::listModules(std::vector<ModuleStatusEntry> &out,
                                std::string *errorMsg) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  out.clear();
  auto modules = apm::fs::listDir(modulesRoot_, false);
  for (const auto &name : modules) {
    ModuleInfo info;
    ModuleState state;
    if (!loadModule(name, info, state, errorMsg))
      return false;
    ModuleStatusEntry entry;
    entry.info = info;
    entry.state = state;
    entry.path = modulePath(name);
    out.push_back(std::move(entry));
  }
  if (errorMsg)
    errorMsg->clear();
  return true;
}

bool ModuleManager::isPartitionMounted(const std::string &mountPoint) const {
  std::ifstream mounts("/proc/mounts");
  if (!mounts.is_open())
    return false;

  std::string normalized = resolveForMount(mountPoint);
  std::string line;
  while (std::getline(mounts, line)) {
    std::istringstream iss(line);
    std::string source;
    std::string target;
    std::string fsType;
    if (!(iss >> source >> target >> fsType))
      continue;
    if (resolveForMount(target) == normalized)
      return true;
  }

  return false;
}

void ModuleManager::monitorPartitions() {
  using namespace std::chrono_literals;
  std::set<std::size_t> observed;

  constexpr int kMaxIterations = 30;
  constexpr auto kSleep = 5s;

  for (int attempt = 0; attempt < kMaxIterations && !monitorStop_.load();
       ++attempt) {
    OverlayStacks stacks;
    {
      std::lock_guard<std::recursive_mutex> lock(mutex_);
      stacks = buildOverlayStacks();
    }

    for (std::size_t idx = 0; idx < std::size(kOverlayTargets); ++idx) {
      if (observed.count(idx))
        continue;
      if (hasTrackedBindMounts(kOverlayTargets[idx])) {
        observed.insert(idx);
        continue;
      }
      if (isOverlayMounted(kOverlayTargets[idx])) {
        observed.insert(idx);
        continue;
      }
      const auto candidates = mountCandidatesForTarget(kOverlayTargets[idx]);
      std::string mountPoint =
          candidates.empty() ? kOverlayTargets[idx].mountPoint : candidates[0];
      bool candidateMounted = false;
      for (const auto &candidate : candidates) {
        if (isOverlayCandidateReady(kOverlayTargets[idx], candidate)) {
          mountPoint = candidate;
          candidateMounted = true;
          break;
        }
      }
      if (!candidateMounted)
        continue;

      std::vector<std::string> layers;
      auto it = stacks.find(kOverlayTargets[idx].name);
      if (it != stacks.end())
        layers = it->second;

      std::string err;
      if (!applyOverlayForTarget(idx, layers, &err)) {
        apm::logger::error(
            "Partition monitor: failed to apply overlay for " +
            mountPoint +
            (err.empty() ? "" : (": " + err)));
      }
      observed.insert(idx);
    }

    if (observed.size() == std::size(kOverlayTargets) || monitorStop_.load())
      break;

    std::this_thread::sleep_for(kSleep);
  }

  std::lock_guard<std::recursive_mutex> lock(mutex_);
  monitorRunning_ = false;
}

void ModuleManager::startPartitionMonitor() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (monitorRunning_)
    return;

  monitorStop_.store(false);
  monitorRunning_ = true;
  monitorThread_ = std::thread([this]() { monitorPartitions(); });
}

void ModuleManager::stopMonitorLocked() { monitorStop_.store(true); }

void ModuleManager::stopPartitionMonitor() {
  std::thread worker;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    stopMonitorLocked();
    worker.swap(monitorThread_);
    monitorRunning_ = false;
  }

  if (worker.joinable())
    worker.join();
}

bool ModuleManager::incrementBootCounter(const std::string &path,
                                         std::uint64_t *newValue) {
  std::uint64_t current = getBootCounter(path);
  if (current == UINT64_MAX)
    current = 0;
  ++current;

  if (!apm::fs::writeFile(path, std::to_string(current), true))
    return false;

  ::chmod(path.c_str(), 0600);
  if (newValue)
    *newValue = current;
  return true;
}

std::uint64_t ModuleManager::getBootCounter(const std::string &path) {
  std::string raw;
  if (!apm::fs::readFile(path, raw))
    return 0;

  errno = 0;
  char *end = nullptr;
  unsigned long long val = std::strtoull(raw.c_str(), &end, 10);
  if (errno != 0 || end == raw.c_str())
    return 0;
  return static_cast<std::uint64_t>(val);
}

bool ModuleManager::resetBootCounter(const std::string &path) {
  if (!apm::fs::writeFile(path, "0", true))
    return false;
  ::chmod(path.c_str(), 0600);
  return true;
}

std::uint64_t ModuleManager::getBootThreshold(const std::string &path,
                                              std::uint64_t defaultValue) {
  std::string raw;
  if (!apm::fs::readFile(path, raw))
    return defaultValue;

  errno = 0;
  char *end = nullptr;
  unsigned long long val = std::strtoull(raw.c_str(), &end, 10);
  if (errno != 0 || end == raw.c_str())
    return defaultValue;
  return val == 0 ? defaultValue : static_cast<std::uint64_t>(val);
}

bool ModuleManager::enterSafeMode(const std::string &path) {
  if (!apm::fs::writeFile(path, "1", true))
    return false;
  ::chmod(path.c_str(), 0600);
  return true;
}

bool ModuleManager::isSafeModeActive(const std::string &path) {
  return apm::fs::isFile(path);
}

bool ModuleManager::clearSafeMode(const std::string &path) {
  return apm::fs::removeFile(path);
}

} // namespace apm::ams
