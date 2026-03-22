/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: apk_installer.cpp
 * Purpose: Implement APK install/uninstall flows including AMS overlay handling for system installs.
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

#include "ams/module_info.hpp"
#include "config.hpp"
#include "fs.hpp"
#include "logger.hpp"
#include "security.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace apm::apk {

namespace {

constexpr const char *kLogFileTag = "apk_installer.cpp";

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
    return "command failed";
  }
  if (WIFEXITED(rc)) {
    return "exit code " + std::to_string(WEXITSTATUS(rc));
  }
  if (WIFSIGNALED(rc)) {
    return "signal " + std::to_string(WTERMSIG(rc));
  }
  return "status " + std::to_string(rc);
}

int runPmCommand(const std::vector<std::string> &args,
                 std::string *errorMsg = nullptr) {
  if (args.empty()) {
    if (errorMsg) {
      *errorMsg = "pm args are empty";
    }
    return -1;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    if (errorMsg) {
      *errorMsg = "fork() failed: " + std::string(std::strerror(errno));
    }
    return -1;
  }

  if (pid == 0) {
    std::vector<char *> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char *>("pm"));
    for (const auto &arg : args) {
      argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp("pm", argv.data());
    _exit(127);
  }

  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    if (errorMsg) {
      *errorMsg = "waitpid() failed: " + std::string(std::strerror(errno));
    }
    return -1;
  }

  return status;
}

// ---------------------------------------------------------------------
// AMS module info for system APK overlays
// ---------------------------------------------------------------------

static const char *kSystemApkModuleName = "apm-system-apps";
static const char *kLegacyMagiskSystemAppRoot =
    "/data/adb/modules/apm-system-apps/system/app";
static const char *kUserInstallStagingDir = "/data/local/tmp/apm-apk-staging";

std::string systemApkModuleRoot() {
  return apm::fs::joinPath(apm::config::getModulesDir(), kSystemApkModuleName);
}

std::string systemApkOverlayRoot() {
  return apm::fs::joinPath(systemApkModuleRoot(), "overlay/system/app");
}

void setRootOwnerPerms(const std::string &path, mode_t mode) {
  ::chown(path.c_str(), 0, 0);
  ::chmod(path.c_str(), mode);
}

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

// Ensure the AMS module skeleton exists so amsd can mount system APK overlays
// from /data/ams/modules at boot.
bool ensureAmsModuleSkeleton(std::string *err) {
  const std::string moduleRoot = systemApkModuleRoot();
  const std::string overlayRoot = systemApkOverlayRoot();
  const std::string workdirRoot = apm::fs::joinPath(moduleRoot, "workdir");
  const std::string infoPath = apm::fs::joinPath(moduleRoot, "module-info.json");
  const std::string statePath = apm::fs::joinPath(moduleRoot, "state.json");

  if (!apm::fs::createDirs(overlayRoot)) {
    if (err) {
      *err = "failed to create module system/app dir: " + overlayRoot;
    }
    return false;
  }

  // Keep module layout aligned with normal AMS modules.
  if (!apm::fs::createDirs(apm::fs::joinPath(workdirRoot, "system")) ||
      !apm::fs::createDirs(apm::fs::joinPath(workdirRoot, "vendor")) ||
      !apm::fs::createDirs(apm::fs::joinPath(workdirRoot, "product"))) {
    if (err) {
      *err = "failed to create AMS workdir structure for system APK module";
    }
    return false;
  }

  if (!apm::fs::pathExists(infoPath)) {
    const std::string infoJson =
        "{\n"
        "  \"name\": \"apm-system-apps\",\n"
        "  \"version\": \"1.0\",\n"
        "  \"author\": \"APM\",\n"
        "  \"description\": \"System APK overlays staged by APM\",\n"
        "  \"mount\": true,\n"
        "  \"post_fs_data\": false,\n"
        "  \"service\": false,\n"
        "  \"install-sh\": false\n"
        "}\n";

    std::string werr;
    if (!writeFileSimple(infoPath, infoJson, &werr)) {
      if (err) {
        *err = "failed to write module-info.json: " + werr;
      }
      return false;
    }
  }

  apm::ams::ModuleState state;
  std::string readErr;
  if (!apm::ams::readModuleState(statePath, state, &readErr)) {
    apm::logger::warn("apk_install (system): failed to read module state at " +
                      statePath + ": " + readErr + "; recreating state file");
    state = apm::ams::ModuleState{};
  }
  if (state.installedAt.empty()) {
    state.installedAt = apm::ams::makeIsoTimestamp();
  }
  state.enabled = true;
  state.lastError.clear();

  std::string stateErr;
  if (!apm::ams::writeModuleState(statePath, state, &stateErr)) {
    if (err) {
      *err = "failed to write state.json: " + stateErr;
    }
    return false;
  }

  setRootOwnerPerms(moduleRoot, 0755);
  setRootOwnerPerms(apm::fs::joinPath(moduleRoot, "overlay"), 0755);
  setRootOwnerPerms(apm::fs::joinPath(moduleRoot, "overlay/system"), 0755);
  setRootOwnerPerms(overlayRoot, 0755);
  setRootOwnerPerms(workdirRoot, 0755);
  setRootOwnerPerms(apm::fs::joinPath(workdirRoot, "system"), 0755);
  setRootOwnerPerms(apm::fs::joinPath(workdirRoot, "vendor"), 0755);
  setRootOwnerPerms(apm::fs::joinPath(workdirRoot, "product"), 0755);
  setRootOwnerPerms(infoPath, 0644);
  setRootOwnerPerms(statePath, 0644);

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
  const std::vector<std::string> roots = {systemApkOverlayRoot(),
                                          kLegacyMagiskSystemAppRoot};
  for (const auto &root : roots) {
    std::string cand = apm::fs::joinPath(root, dirName);
    if (apm::fs::isDirectory(cand)) {
      apm::logger::info("apk_uninstall: removing overlay dir " + cand);
      if (!apm::fs::removeDirRecursive(cand)) {
        apm::logger::warn("apk_uninstall: failed removing overlay dir " + cand);
      }
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
  if (apm::logger::isDebugEnabled()) {
    apm::logger::debug(std::string(kLogFileTag) +
                       ": runCommandCaptureOutput exec='" + cmd + "'");
  }

  FILE *pipe = ::popen(cmd.c_str(), "r");
  if (!pipe) {
    if (apm::logger::isDebugEnabled()) {
      apm::logger::debug(std::string(kLogFileTag) +
                         ": runCommandCaptureOutput popen failed");
    }
    return -1;
  }

  char buf[256];
  while (fgets(buf, sizeof(buf), pipe)) {
    output.append(buf);
  }

  const int rc = ::pclose(pipe);
  if (apm::logger::isDebugEnabled()) {
    std::string debugOutput = output;
    if (debugOutput.size() > 512) {
      debugOutput = debugOutput.substr(0, 512) + "...(truncated)";
    }
    apm::logger::debug(std::string(kLogFileTag) +
                       ": runCommandCaptureOutput rc=" + std::to_string(rc) +
                       " output='" + debugOutput + "'");
  }

  return rc;
}

} // namespace

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

// Install an APK either via a standard `pm install` or by staging an AMS
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
        "System app install requires root (run apmd as root)";
    apm::logger::error(
        "apk_install (system): requested but process is not root");
    return false;
  }

  std::string err;
  if (!ensureAmsModuleSkeleton(&err)) {
    result.ok = false;
    result.message = "Failed to prepare AMS module for system apps: " + err;
    apm::logger::error("apk_install (system): " + result.message);
    return false;
  }

  std::string appDirName = basenameNoExt(apkPath);
  std::string destDir = apm::fs::joinPath(systemApkOverlayRoot(), appDirName);
  std::string destApk = destDir + "/base.apk";

  if (!apm::fs::createDirs(destDir)) {
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

  setRootOwnerPerms(destDir, 0755);
  setRootOwnerPerms(destApk, 0644);

  apm::logger::info("apk_install (system): staged " + apkPath +
                    " as system app at " + destApk);

  result.ok = true;
  result.message =
      "APK staged in AMS system overlay at: " + destApk +
      " (reboot required for Android to recognize it as a system app)";
  return true;
}

// ---------------------------------------------------------------------
// Uninstall APK (user apps + system apps + system overlay cleanup)
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

  std::string nameErr;
  if (!apm::security::validatePackageName(packageName, &nameErr)) {
    result.ok = false;
    result.message = nameErr;
    apm::logger::error("apk_uninstall: " + nameErr);
    return false;
  }

  apm::logger::info("apk_uninstall: uninstalling " + packageName);

  // 1) First try normal uninstall.
  {
    apm::logger::info("apk_uninstall: running: pm uninstall " + packageName);
    std::string pmErr;
    int rc = runPmCommand({"uninstall", packageName}, &pmErr);
    if (rc == 0) {
      result.ok = true;
      result.message = "Package uninstalled: " + packageName;
      apm::logger::info("apk_uninstall: pm uninstall succeeded");
    } else {
      std::string reason = exitCodeString(rc);
      if (!pmErr.empty()) {
        reason += ": " + pmErr;
      }
      apm::logger::warn("apk_uninstall: pm uninstall failed (" + reason +
                        "), trying system-app fallback...");
    }
  }

  // 2) System app fallback: uninstall for user 0.
  if (!result.ok) {
    apm::logger::info("apk_uninstall: running fallback: pm uninstall --user 0 " +
                      packageName);
    std::string pmErr;
    int rc = runPmCommand({"uninstall", "--user", "0", packageName}, &pmErr);
    if (rc == 0) {
      result.ok = true;
      result.message =
          "Package uninstalled for user 0 (system app): " + packageName;
      apm::logger::info("apk_uninstall: pm uninstall --user 0 succeeded");
    } else {
      if (!pmErr.empty()) {
        apm::logger::warn("apk_uninstall: pm uninstall --user 0 error: " +
                          pmErr);
      }
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
