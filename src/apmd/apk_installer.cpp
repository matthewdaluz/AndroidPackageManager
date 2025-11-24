/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: apk_installer.cpp
 * Purpose: Implement APK install/uninstall flows including Magisk overlay handling for system installs.
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

#include "apk_installer.hpp"

#include "fs.hpp"
#include "logger.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace apm::apk {

namespace {

std::string shellEscapeSingleQuotes(const std::string &in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  return out;
}

bool isRoot() { return (::geteuid() == 0); }

std::string exitCodeString(int rc) {
  if (rc == -1) {
    return "system() failed";
  }
  if (WIFEXITED(rc)) {
    return "exit code " + std::to_string(WEXITSTATUS(rc));
  }
  if (WIFSIGNALED(rc)) {
    return "signal " + std::to_string(WTERMSIG(rc));
  }
  return "status " + std::to_string(rc);
}

// ---------------------------------------------------------------------
// Magisk module info for system apps
// ---------------------------------------------------------------------

static const char *kModuleId = "apm-system-apps";
static const char *kModuleBaseDir = "/data/adb/modules/apm-system-apps";
static const char *kModuleSystemAppRoot =
    "/data/adb/modules/apm-system-apps/system/app";
static const char *kUserInstallStagingDir = "/data/local/tmp/apm-apk-staging";

bool writeFileSimple(const std::string &path, const std::string &data,
                     std::string *err) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (err) {
      *err =
          "failed to open '" + path + "' for writing: " + std::strerror(errno);
    }
    return false;
  }
  out << data;
  if (!out) {
    if (err) {
      *err = "failed to write to '" + path + "'";
    }
    return false;
  }
  return true;
}

bool copyFileSimple(const std::string &src, const std::string &dst,
                    std::string *err) {
  std::ifstream in(src, std::ios::binary);
  if (!in) {
    if (err) {
      *err = "failed to open source '" + src + "': " + std::strerror(errno);
    }
    return false;
  }
  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out) {
    if (err) {
      *err = "failed to open dest '" + dst + "': " + std::strerror(errno);
    }
    return false;
  }
  out << in.rdbuf();
  if (!out) {
    if (err) {
      *err = "failed to copy data to '" + dst + "'";
    }
    return false;
  }
  return true;
}

// Ensure the Magisk module skeleton exists so Magisk sees it as a valid module
bool ensureModuleSkeleton(std::string *err) {
  if (!apm::fs::pathExists(kModuleBaseDir)) {
    if (!apm::fs::mkdirs(kModuleBaseDir)) {
      if (err) {
        *err =
            "failed to create module base dir: " + std::string(kModuleBaseDir);
      }
      return false;
    }
  }

  if (!apm::fs::mkdirs(kModuleSystemAppRoot)) {
    if (err) {
      *err = "failed to create module system/app dir: " +
             std::string(kModuleSystemAppRoot);
    }
    return false;
  }

  std::string modulePropPath = std::string(kModuleBaseDir) + "/module.prop";
  if (!apm::fs::pathExists(modulePropPath)) {
    std::ostringstream mp;
    mp << "id=" << kModuleId << "\n"
       << "name=APM System Apps\n"
       << "version=1.0\n"
       << "versionCode=1\n"
       << "author=APM\n"
       << "description=System apps installed via Android Package Manager\n";

    std::string werr;
    if (!writeFileSimple(modulePropPath, mp.str(), &werr)) {
      if (err) {
        *err = "failed to write module.prop: " + werr;
      }
      return false;
    }
  }

  ::chmod(kModuleBaseDir, 0755);
  ::chmod(kModuleSystemAppRoot, 0755);

  return true;
}

std::string basenameNoExt(const std::string &path) {
  std::string::size_type slash = path.find_last_of('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);

  std::string::size_type dot = base.find_last_of('.');
  if (dot != std::string::npos) {
    base = base.substr(0, dot);
  }

  for (char &c : base) {
    if (c == ' ')
      c = '_';
  }

  if (base.empty()) {
    base = "apk";
  }

  return base;
}

// Best-effort cleanup for system app overlay
void cleanupSystemOverlay(const std::string &dirName) {
  std::string cand = std::string(kModuleSystemAppRoot) + "/" + dirName;
  if (apm::fs::isDirectory(cand)) {
    apm::logger::info("apk_uninstall: removing overlay dir " + cand);
    if (!apm::fs::removeDirRecursive(cand)) {
      apm::logger::warn("apk_uninstall: failed removing overlay dir " + cand);
    }
  }
}

// Return the basename (with extension) of a path; fallback to a stable name.
std::string basenameWithExt(const std::string &path) {
  if (path.empty()) {
    return "apk.apk";
  }
  auto slash = path.find_last_of('/');
  std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
  if (base.empty()) {
    return "apk.apk";
  }
  return base;
}

// Stage APK under /data/local/tmp so pm (running as shell/system) can read it.
bool stageApkForUserInstall(const std::string &srcPath, std::string &dstPath,
                            std::string *err) {
  if (!apm::fs::mkdirs(kUserInstallStagingDir)) {
    if (err) {
      *err = "failed to create staging dir: " + std::string(kUserInstallStagingDir);
    }
    return false;
  }

  dstPath = std::string(kUserInstallStagingDir) + "/" + basenameWithExt(srcPath);
  apm::fs::removeFile(dstPath); // best effort

  std::string cperr;
  if (!copyFileSimple(srcPath, dstPath, &cperr)) {
    if (err) {
      *err = "failed to stage APK: " + cperr;
    }
    return false;
  }

  ::chmod(dstPath.c_str(), 0644);
  return true;
}

// Run a shell command while capturing stdout/stderr.
int runCommandCaptureOutput(const std::string &cmd, std::string &output) {
  FILE *pipe = ::popen(cmd.c_str(), "r");
  if (!pipe) {
    return -1;
  }

  char buf[256];
  while (fgets(buf, sizeof(buf), pipe)) {
    output.append(buf);
  }

  return ::pclose(pipe);
}

} // namespace

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

// Install an APK either via a standard `pm install` or by staging a Magisk
// overlay when opts.installAsSystem is requested.
bool installApk(const std::string &apkPath, const ApkInstallOptions &opts,
                ApkInstallResult &result) {
  result = ApkInstallResult{};

  if (apkPath.empty()) {
    result.ok = false;
    result.message = "APK path is empty";
    apm::logger::error("apk_install: empty apkPath");
    return false;
  }

  if (!apm::fs::pathExists(apkPath)) {
    result.ok = false;
    result.message = "APK file does not exist: " + apkPath;
    apm::logger::error("apk_install: file not found: " + apkPath);
    return false;
  }

  if (!apm::fs::isRegularFile(apkPath)) {
    result.ok = false;
    result.message = "APK path is not a regular file: " + apkPath;
    apm::logger::error("apk_install: not a regular file: " + apkPath);
    return false;
  }

  if (!opts.installAsSystem) {
    std::string stagedPath;
    bool staged = false;

    {
      std::string stageErr;
      if (!stageApkForUserInstall(apkPath, stagedPath, &stageErr)) {
        result.ok = false;
        result.message = stageErr;
        apm::logger::error("apk_install (user): " + stageErr);
        return false;
      }
      staged = true;
    }

    auto cleanupStage = [&]() {
      if (staged && !stagedPath.empty()) {
        apm::fs::removeFile(stagedPath);
      }
    };

    std::string escaped = shellEscapeSingleQuotes(stagedPath);
    std::ostringstream cmd;
    cmd << "pm install --user 0 -r '" << escaped << "' 2>&1";

    apm::logger::info("apk_install (user): running: " + cmd.str());

    std::string output;
    int rc = runCommandCaptureOutput(cmd.str(), output);
    std::string exitStr = exitCodeString(rc);

    if (rc != 0) {
      std::ostringstream msg;
      msg << "pm install failed (" << exitStr << ")";
      if (!output.empty()) {
        if (output.back() == '\n') {
          output.pop_back();
        }
        msg << ": " << output;
      }
      result.ok = false;
      result.message = msg.str();
      apm::logger::error("apk_install (user): " + msg.str());
      cleanupStage();
      return false;
    }

    cleanupStage();
    result.ok = true;
    result.message = "APK installed as a user app";
    apm::logger::info("apk_install (user): success for " + apkPath);
    return true;
  }

  if (!isRoot()) {
    result.ok = false;
    result.message =
        "System app install requires root (run apmd as root / via Magisk)";
    apm::logger::error(
        "apk_install (system): requested but process is not root");
    return false;
  }

  std::string err;
  if (!ensureModuleSkeleton(&err)) {
    result.ok = false;
    result.message = "Failed to prepare Magisk module for system apps: " + err;
    apm::logger::error("apk_install (system): " + result.message);
    return false;
  }

  std::string appDirName = basenameNoExt(apkPath);
  std::string destDir = std::string(kModuleSystemAppRoot) + "/" + appDirName;
  std::string destApk = destDir + "/base.apk";

  if (!apm::fs::mkdirs(destDir)) {
    result.ok = false;
    result.message = "Failed to create system app dir: " + destDir;
    apm::logger::error("apk_install (system): " + result.message);
    return false;
  }

  std::string cperr;
  if (!copyFileSimple(apkPath, destApk, &cperr)) {
    result.ok = false;
    result.message = "Failed to copy APK into system overlay: " + cperr;
    apm::logger::error("apk_install (system): " + result.message);
    return false;
  }

  ::chmod(destDir.c_str(), 0755);
  ::chmod(destApk.c_str(), 0644);
  ::chown(destDir.c_str(), 0, 0);
  ::chown(destApk.c_str(), 0, 0);

  apm::logger::info("apk_install (system): staged " + apkPath +
                    " as system app at " + destApk);

  result.ok = true;
  result.message =
      "APK staged as a system app at: " + destApk +
      " (reboot required for Android to recognize it as a system app)";
  return true;
}

// ---------------------------------------------------------------------
// Uninstall APK (user apps + system apps + Magisk overlay)
// ---------------------------------------------------------------------

// Attempt a normal uninstall, then fall back to system-app removal and overlay
// cleanup so the daemon can remove both user and system installs uniformly.
bool uninstallApk(const std::string &packageName, ApkUninstallResult &result) {
  result = ApkUninstallResult{};

  if (packageName.empty()) {
    result.ok = false;
    result.message = "Package name is empty";
    apm::logger::error("apk_uninstall: empty package name");
    return false;
  }

  apm::logger::info("apk_uninstall: uninstalling " + packageName);

  // 1) First try normal uninstall.
  {
    std::string cmd = "pm uninstall " + packageName;
    apm::logger::info("apk_uninstall: running: " + cmd);

    int rc = ::system(cmd.c_str());
    if (rc == 0) {
      result.ok = true;
      result.message = "Package uninstalled: " + packageName;
      apm::logger::info("apk_uninstall: pm uninstall succeeded");
    } else {
      apm::logger::warn("apk_uninstall: pm uninstall failed (" +
                        exitCodeString(rc) +
                        "), trying system-app fallback...");
    }
  }

  // 2) System app fallback: uninstall for user 0.
  if (!result.ok) {
    std::string cmd = "pm uninstall --user 0 " + packageName;
    apm::logger::info("apk_uninstall: running fallback: " + cmd);

    int rc = ::system(cmd.c_str());
    if (rc == 0) {
      result.ok = true;
      result.message =
          "Package uninstalled for user 0 (system app): " + packageName;
      apm::logger::info("apk_uninstall: pm uninstall --user 0 succeeded");
    } else {
      result.ok = false;
      result.message = "Failed to uninstall " + packageName +
                       " (pm uninstall and --user 0 both failed)";
      apm::logger::error("apk_uninstall: both uninstall methods failed");
    }
  }

  // 3) Cleanup system overlay (best effort)
  cleanupSystemOverlay(packageName);

  return result.ok;
}

} // namespace apm::apk
