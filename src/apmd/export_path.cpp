/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: export_path.cpp
 * Purpose: Event-driven command hotload support (command index + shim
 * generation + PATH environment integration).
 * Last Modified: March 15th, 2026. - 10:51 PM EDT.
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

#include "export_path.hpp"

#include "config.hpp"
#include "fs.hpp"
#include "logger.hpp"
#include "manual_package.hpp"
#include "status_db.hpp"

#include <grp.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

struct CommandCandidate {
  std::string packageName;
  std::string commandName;
  std::string binaryPath;
  bool termuxPackage = false;
};

struct ShimDecision {
  std::string packageName;
  std::string commandName;
  std::string binaryPath;
  std::string shimName;
  apm::daemon::path::CommandCollisionResult mode =
      apm::daemon::path::CommandCollisionResult::Skipped;
  std::string warning;
  bool sourceTermuxEnv = false;
};

bool ensureParentDir(const std::string &path) {
  auto pos = path.find_last_of('/');
  if (pos == std::string::npos)
    return true;
  std::string dir = path.substr(0, pos);
  if (dir.empty())
    return true;
  return apm::fs::createDirs(dir);
}

bool writeAtomicFile(const std::string &path, const std::string &content,
                     mode_t mode = 0, bool setMode = false) {
  if (!ensureParentDir(path)) {
    return false;
  }

  std::string tmpPath = path + ".tmp";
  if (!apm::fs::writeFile(tmpPath, content, true)) {
    return false;
  }

  if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
    ::unlink(tmpPath.c_str());
    return false;
  }

  if (setMode) {
    ::chmod(path.c_str(), mode);
  }

  return true;
}

bool applyRootShellOwnership(const std::string &path,
                             std::vector<std::string> &warnings) {
  if (apm::config::isEmulatorMode()) {
    return true;
  }

  struct group *shellGroup = ::getgrnam("shell");
  if (!shellGroup) {
    warnings.push_back("Group 'shell' not found; ownership not set for " + path);
    return true;
  }

  if (::chown(path.c_str(), 0, shellGroup->gr_gid) != 0) {
    warnings.push_back("Failed to chown root:shell for " + path + ": " +
                       std::strerror(errno));
    return false;
  }

  return true;
}

bool applyFileModeAndOwnership(const std::string &path, mode_t mode,
                               std::vector<std::string> &warnings) {
  bool ok = true;
  if (::chmod(path.c_str(), mode) != 0) {
    warnings.push_back("Failed to chmod " + path + ": " + std::strerror(errno));
    ok = false;
  }
  if (!applyRootShellOwnership(path, warnings)) {
    ok = false;
  }
  return ok;
}

std::string trim(std::string s) {
  auto isSpace = [](unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
  };

  while (!s.empty() && isSpace(static_cast<unsigned char>(s.front()))) {
    s.erase(s.begin());
  }
  while (!s.empty() && isSpace(static_cast<unsigned char>(s.back()))) {
    s.pop_back();
  }
  return s;
}

bool hasPrefix(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

bool isLikelyCommandName(const std::string &name) {
  if (name.empty() || name == "." || name == "..") {
    return false;
  }
  if (name.find('/') != std::string::npos) {
    return false;
  }
  for (char c : name) {
    unsigned char uc = static_cast<unsigned char>(c);
    if (uc <= 0x20 || uc == 0x7f) {
      return false;
    }
  }
  return true;
}

bool isExecutableFile(const std::string &path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    return false;
  }

  if (!S_ISREG(st.st_mode)) {
    return false;
  }

  return (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

void discoverExecutablesInDir(const std::string &packageName,
                              const std::string &dir, bool termuxPackage,
                              std::vector<CommandCandidate> &out,
                              std::unordered_set<std::string> &seenKeys) {
  if (!apm::fs::isDirectory(dir)) {
    return;
  }

  auto entries = apm::fs::listDir(dir, false);
  for (const auto &entry : entries) {
    if (!isLikelyCommandName(entry)) {
      continue;
    }

    std::string fullPath = apm::fs::joinPath(dir, entry);
    if (!isExecutableFile(fullPath)) {
      continue;
    }

    std::string dedupeKey = packageName + "\n" + entry;
    if (!seenKeys.insert(dedupeKey).second) {
      continue;
    }

    out.push_back(
        {packageName, entry, std::move(fullPath), termuxPackage});
  }
}

std::string fallbackInstallRoot(const apm::status::InstalledPackage &pkg) {
  if (!pkg.installRoot.empty()) {
    return pkg.installRoot;
  }

  if (pkg.termuxPackage) {
    return apm::fs::joinPath(apm::config::getTermuxInstalledDir(), pkg.name);
  }

  std::string cmdRoot =
      apm::fs::joinPath(apm::config::getCommandsDir(), pkg.name);
  if (apm::fs::pathExists(cmdRoot)) {
    return cmdRoot;
  }

  std::string depRoot =
      apm::fs::joinPath(apm::config::getDependenciesDir(), pkg.name);
  if (apm::fs::pathExists(depRoot)) {
    return depRoot;
  }

  return apm::fs::joinPath(apm::config::getInstalledDir(), pkg.name);
}

void discoverTermuxPackageCommands(const apm::status::InstalledPackage &pkg,
                                   std::vector<CommandCandidate> &out,
                                   std::unordered_set<std::string> &seenKeys) {
  std::string manifestPath =
      apm::fs::joinPath(fallbackInstallRoot(pkg), "files.list");

  std::string content;
  if (!apm::fs::readFile(manifestPath, content)) {
    return;
  }

  std::istringstream in(content);
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    static const std::string kBinPrefix = "usr/bin/";
    if (!hasPrefix(line, kBinPrefix)) {
      continue;
    }

    std::string commandName = line.substr(kBinPrefix.size());
    if (commandName.find('/') != std::string::npos ||
        !isLikelyCommandName(commandName)) {
      continue;
    }

    std::string binaryPath =
        apm::fs::joinPath(apm::config::getTermuxRoot(), line);
    if (!isExecutableFile(binaryPath)) {
      continue;
    }

    std::string dedupeKey = pkg.name + "\n" + commandName;
    if (!seenKeys.insert(dedupeKey).second) {
      continue;
    }

    out.push_back({pkg.name, std::move(commandName), std::move(binaryPath),
                   true});
  }
}

void discoverRegularPackageCommands(const std::string &packageName,
                                    const std::string &installRoot,
                                    std::vector<CommandCandidate> &out,
                                    std::unordered_set<std::string> &seenKeys) {
  static const char *kCandidateDirs[] = {"usr/bin",
                                         "bin",
                                         "usr/sbin",
                                         "sbin",
                                         "data/data/com.termux/files/usr/bin"};

  for (const auto *relDir : kCandidateDirs) {
    discoverExecutablesInDir(packageName,
                             apm::fs::joinPath(installRoot, relDir), false,
                             out, seenKeys);
  }
}

void discoverStatusCommands(std::vector<CommandCandidate> &out,
                            std::vector<std::string> &warnings) {
  apm::status::InstalledDb db;
  std::string err;
  if (!apm::status::loadStatus(db, &err)) {
    warnings.push_back("Failed to load status DB for command indexing: " +
                       err);
    return;
  }

  std::vector<std::string> names;
  names.reserve(db.size());
  for (const auto &kv : db) {
    names.push_back(kv.first);
  }
  std::sort(names.begin(), names.end());

  std::unordered_set<std::string> seenKeys;
  for (const auto &name : names) {
    auto it = db.find(name);
    if (it == db.end()) {
      continue;
    }

    const auto &pkg = it->second;
    if (pkg.termuxPackage) {
      discoverTermuxPackageCommands(pkg, out, seenKeys);
      continue;
    }

    discoverRegularPackageCommands(name, fallbackInstallRoot(pkg), out,
                                   seenKeys);
  }
}

void discoverManualCommands(std::vector<CommandCandidate> &out,
                            std::vector<std::string> &warnings) {
  std::vector<apm::manual::PackageInfo> pkgs;
  std::string err;
  if (!apm::manual::listInstalledPackages(pkgs, &err)) {
    warnings.push_back("Failed to load manual package metadata: " + err);
    return;
  }

  std::unordered_set<std::string> seenKeys;
  for (const auto &pkg : pkgs) {
    if (pkg.name.empty() || pkg.prefix.empty()) {
      continue;
    }
    discoverRegularPackageCommands(pkg.name, pkg.prefix, out, seenKeys);
  }
}

std::vector<std::string> buildSystemSearchDirs() {
  std::set<std::string> unique;

  const char *pathEnv = ::getenv("PATH");
  if (pathEnv && *pathEnv) {
    std::string path(pathEnv);
    std::size_t start = 0;
    while (start <= path.size()) {
      std::size_t end = path.find(':', start);
      std::string part =
          (end == std::string::npos) ? path.substr(start)
                                     : path.substr(start, end - start);
      part = trim(part);
      if (!part.empty() && part != apm::config::getApmBinDir()) {
        unique.insert(part);
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
  }

  static const char *kDefaultSystemDirs[] = {
      "/system/bin",          "/system/xbin",  "/vendor/bin",
      "/odm/bin",             "/product/bin",  "/apex/com.android.runtime/bin",
      "/apex/com.android.art/bin", "/usr/bin",      "/bin"};

  for (const auto *dir : kDefaultSystemDirs) {
    unique.insert(dir);
  }

  std::vector<std::string> out;
  out.reserve(unique.size());
  for (const auto &dir : unique) {
    if (dir == apm::config::getApmBinDir()) {
      continue;
    }
    if (!apm::fs::isDirectory(dir)) {
      continue;
    }
    out.push_back(dir);
  }
  return out;
}

const std::vector<std::string> &systemSearchDirs() {
  static const std::vector<std::string> dirs = buildSystemSearchDirs();
  return dirs;
}

bool commandExistsInSystemPaths(const std::string &commandName) {
  if (!isLikelyCommandName(commandName)) {
    return false;
  }

  const auto &dirs = systemSearchDirs();
  for (const auto &dir : dirs) {
    std::string full = apm::fs::joinPath(dir, commandName);
    if (isExecutableFile(full)) {
      return true;
    }
  }

  return false;
}

std::string makeNamespacedCommand(const std::string &packageName,
                                  const std::string &commandName) {
  if (packageName.empty() || commandName.empty()) {
    return {};
  }
  return packageName + "-" + commandName;
}

std::string joinList(const std::vector<std::string> &items,
                     const std::string &delim) {
  std::ostringstream out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      out << delim;
    }
    out << items[i];
  }
  return out.str();
}

bool readFileContains(const std::string &path, const std::string &needle) {
  std::string content;
  if (!apm::fs::readFile(path, content)) {
    return false;
  }
  return content.find(needle) != std::string::npos;
}

std::unordered_set<std::string> loadShimsFromManifest(const std::string &path) {
  std::unordered_set<std::string> shims;
  std::string content;
  if (!apm::fs::readFile(path, content)) {
    return shims;
  }

  std::size_t pos = 0;
  while (true) {
    pos = content.find("\"shim\"", pos);
    if (pos == std::string::npos) {
      break;
    }

    std::size_t colon = content.find(':', pos);
    if (colon == std::string::npos) {
      break;
    }

    std::size_t quoteStart = content.find('"', colon);
    if (quoteStart == std::string::npos) {
      break;
    }

    std::size_t quoteEnd = content.find('"', quoteStart + 1);
    if (quoteEnd == std::string::npos) {
      break;
    }

    std::string value = content.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
    if (!value.empty()) {
      shims.insert(value);
    }
    pos = quoteEnd + 1;
  }

  return shims;
}

std::unordered_set<std::string> loadManagedShimsFromBinDir() {
  std::unordered_set<std::string> out;
  const std::string binDir = apm::config::getApmBinDir();

  if (!apm::fs::isDirectory(binDir)) {
    return out;
  }

  auto entries = apm::fs::listDir(binDir, false);
  for (const auto &entry : entries) {
    if (!isLikelyCommandName(entry)) {
      continue;
    }

    std::string fullPath = apm::fs::joinPath(binDir, entry);
    if (!apm::fs::isFile(fullPath)) {
      continue;
    }

    if (!readFileContains(fullPath, "# apm-managed-shim")) {
      continue;
    }

    out.insert(entry);
  }

  return out;
}

std::string shellQuote(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('\'');
  for (char c : value) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

bool writeShimFile(const ShimDecision &decision, std::string &errorOut) {
  const std::string target =
      apm::fs::joinPath(apm::config::getApmBinDir(), decision.shimName);

  std::ostringstream script;
  script << "#!/system/bin/sh\n";
  script << "# apm-managed-shim\n";
  if (decision.sourceTermuxEnv) {
    const std::string termuxEnv = apm::config::getTermuxEnvFile();
    script << "if [ -f " << shellQuote(termuxEnv) << " ]; then\n";
    script << "  . " << shellQuote(termuxEnv) << "\n";
    script << "fi\n";
  }
  script << "exec " << shellQuote(decision.binaryPath) << " \"$@\"\n";

  if (!writeAtomicFile(target, script.str(), 0755, true)) {
    errorOut = "Failed to write shim: " + target;
    return false;
  }

  return true;
}

std::string jsonEscape(const std::string &in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  return out;
}

const char *modeToString(apm::daemon::path::CommandCollisionResult mode) {
  switch (mode) {
  case apm::daemon::path::CommandCollisionResult::Canonical:
    return "canonical";
  case apm::daemon::path::CommandCollisionResult::Namespaced:
    return "namespaced";
  case apm::daemon::path::CommandCollisionResult::Skipped:
  default:
    return "skipped";
  }
}

std::string currentTimestampIso() {
  std::time_t now = std::time(nullptr);
  std::tm *local = std::localtime(&now);
  if (!local) {
    return "";
  }

  char buf[64] = {0};
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", local) == 0) {
    return "";
  }
  return buf;
}

bool writeCommandIndexManifest(const std::vector<ShimDecision> &decisions,
                               const std::string &triggerReason,
                               std::string &errorOut) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"version\": 1,\n";
  out << "  \"generated_at\": " << static_cast<long long>(std::time(nullptr))
      << ",\n";
  out << "  \"generated_at_iso\": \"" << jsonEscape(currentTimestampIso())
      << "\",\n";
  out << "  \"trigger_reason\": \"" << jsonEscape(triggerReason)
      << "\",\n";
  out << "  \"entries\": [\n";

  for (std::size_t i = 0; i < decisions.size(); ++i) {
    const auto &d = decisions[i];
    out << "    {\n";
    out << "      \"package\": \"" << jsonEscape(d.packageName) << "\",\n";
    out << "      \"command\": \"" << jsonEscape(d.commandName) << "\",\n";
    out << "      \"shim\": \"" << jsonEscape(d.shimName) << "\",\n";
    out << "      \"binary\": \"" << jsonEscape(d.binaryPath) << "\",\n";
    out << "      \"mode\": \"" << modeToString(d.mode) << "\",\n";
    out << "      \"warning\": \"" << jsonEscape(d.warning) << "\"\n";
    out << "    }";
    if (i + 1 < decisions.size()) {
      out << ",";
    }
    out << "\n";
  }

  out << "  ]\n";
  out << "}\n";

  const std::string manifestPath = apm::config::getCommandIndexFile();
  if (!writeAtomicFile(manifestPath, out.str(), 0644, true)) {
    errorOut = "Failed to write command index manifest: " + manifestPath;
    return false;
  }

  return true;
}

enum class AppendLineResult { Added, AlreadyPresent, Failed };

std::string getParentDir(const std::string &path) {
  auto pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return {};
  }
  return path.substr(0, pos);
}

AppendLineResult appendLineIfMissing(const std::string &path,
                                     const std::string &line,
                                     bool executable = false,
                                     bool ensureParents = true) {
  std::string content;
  if (apm::fs::readFile(path, content)) {
    if (content.find(line) != std::string::npos) {
      return AppendLineResult::AlreadyPresent;
    }
  }

  if (ensureParents) {
    if (!ensureParentDir(path)) {
      return AppendLineResult::Failed;
    }
  } else {
    const std::string parent = getParentDir(path);
    if (!parent.empty() && !apm::fs::isDirectory(parent)) {
      return AppendLineResult::Failed;
    }
  }

  if (!content.empty() && content.back() != '\n') {
    content.push_back('\n');
  }
  content += line;
  content.push_back('\n');

  if (!writeAtomicFile(path, content, executable ? 0755 : 0644, true)) {
    return AppendLineResult::Failed;
  }
  if (!executable) {
    ::chmod(path.c_str(), 0644);
  }
  return AppendLineResult::Added;
}

bool writePathSourceFiles(std::vector<std::string> &warnings) {
  bool ok = true;
  const std::string pathDir = apm::config::getPathDir();
  const std::string shPath = apm::config::getShPathFile();
  const std::string bashPath = apm::config::getBashPathFile();

  if (!apm::fs::createDirs(pathDir)) {
    warnings.push_back("Failed to create PATH source directory: " + pathDir);
    return false;
  }
  if (!applyFileModeAndOwnership(pathDir, 0775, warnings)) {
    ok = false;
  }

  std::ostringstream sh;
  sh << "#!/system/bin/sh\n";
  sh << "# Auto-generated by apmd (event-driven hotload)\n";
  sh << "APM_SHIM_DIR=" << shellQuote(apm::config::getApmBinDir()) << "\n";
  sh << "APM_SH_PATH_FILE=" << shellQuote(shPath) << "\n";
  sh << "APM_BASH_PATH_FILE=" << shellQuote(bashPath) << "\n";
  sh << "case \":${PATH:-}:\" in\n";
  sh << "  *:\"$APM_SHIM_DIR\":*) ;;\n";
  sh << "  *)\n";
  sh << "    if [ -z \"${PATH:-}\" ]; then\n";
  sh << "      PATH=\"$APM_SHIM_DIR\"\n";
  sh << "    else\n";
  sh << "      PATH=\"$APM_SHIM_DIR:$PATH\"\n";
  sh << "    fi\n";
  sh << "    ;;\n";
  sh << "esac\n";
  sh << "export PATH\n";
  sh << "export ENV=\"$APM_SH_PATH_FILE\"\n";
  sh << "export BASH_ENV=\"$APM_BASH_PATH_FILE\"\n";
  sh << "unset APM_SHIM_DIR\n";
  sh << "unset APM_SH_PATH_FILE\n";
  sh << "unset APM_BASH_PATH_FILE\n";

  if (!writeAtomicFile(shPath, sh.str(), 0664, true)) {
    warnings.push_back("Failed to write sh PATH source file: " + shPath);
    ok = false;
  } else if (!applyFileModeAndOwnership(shPath, 0664, warnings)) {
    ok = false;
  }

  std::ostringstream bash;
  bash << "#!/system/bin/sh\n";
  bash << "# Auto-generated by apmd (event-driven hotload)\n";
  bash << "if [ -f " << shellQuote(shPath) << " ]; then\n";
  bash << "  . " << shellQuote(shPath) << "\n";
  bash << "fi\n";
  bash << "hash -r 2>/dev/null || true\n";

  if (!writeAtomicFile(bashPath, bash.str(), 0664, true)) {
    warnings.push_back("Failed to write bash PATH source file: " + bashPath);
    ok = false;
  } else if (!applyFileModeAndOwnership(bashPath, 0664, warnings)) {
    ok = false;
  }

  return ok;
}

bool installShellHooks(std::vector<std::string> &warnings) {
  if (apm::config::isEmulatorMode()) {
    return true;
  }

  const std::string shPath = apm::config::getShPathFile();
  const std::string bashPath = apm::config::getBashPathFile();
  const std::string shHookLine =
      "[ -f \"" + shPath + "\" ] && . \"" + shPath + "\"";
  const std::string bashHookLine =
      "[ -f \"" + bashPath + "\" ] && . \"" + bashPath + "\"";

  struct HookTarget {
    const char *path;
    bool required;
    bool ensureParents;
  };

  static const HookTarget kShTargets[] = {
      {"/data/local/userinit.sh", true, true},
      {"/data/local/tmp/.profile", true, true},
      {"/data/local/tmp/.mkshrc", true, true},
      {"/data/.profile", false, false},
      {"/data/.mkshrc", false, false},
      {"/root/.profile", false, false},
      {"/root/.mkshrc", false, false},
  };
  static const HookTarget kBashTargets[] = {
      {"/data/local/tmp/.bashrc", true, true},
      {"/data/local/tmp/.bash_profile", true, true},
      {"/data/.bashrc", false, false},
      {"/data/.bash_profile", false, false},
      {"/root/.bashrc", false, false},
      {"/root/.bash_profile", false, false},
  };

  std::size_t installed = 0;
  std::size_t alreadyPresent = 0;
  std::size_t skipped = 0;
  std::size_t failed = 0;

  auto installHookTargets = [&](const HookTarget *targets, std::size_t count,
                                const std::string &hookLine,
                                const std::string &label) {
    for (std::size_t i = 0; i < count; ++i) {
      const HookTarget &target = targets[i];
      const std::string targetPath = target.path;
      const std::string parent = getParentDir(targetPath);

      if (!target.ensureParents && !parent.empty() &&
          !apm::fs::isDirectory(parent)) {
        ++skipped;
        apm::logger::info("export_path: " + label +
                          " hook skipped (parent missing): " + targetPath);
        continue;
      }

      AppendLineResult result = appendLineIfMissing(targetPath, hookLine, false,
                                                    target.ensureParents);
      if (result == AppendLineResult::Added) {
        ++installed;
        apm::logger::info("export_path: " + label +
                          " hook installed: " + targetPath);
        continue;
      }

      if (result == AppendLineResult::AlreadyPresent) {
        ++alreadyPresent;
        apm::logger::debug("export_path: " + label +
                           " hook already present: " + targetPath);
        continue;
      }

      ++failed;
      std::string reason =
          "Failed to install " + label + " hook into " + targetPath;
      if (!target.required) {
        reason += " (optional target)";
      }
      warnings.push_back(reason);
    }
  };

  installHookTargets(kShTargets, sizeof(kShTargets) / sizeof(kShTargets[0]),
                     shHookLine, "sh/mksh");
  installHookTargets(kBashTargets,
                     sizeof(kBashTargets) / sizeof(kBashTargets[0]),
                     bashHookLine, "bash");

  const std::string serviceDir = "/data/adb/service.d";
  if (apm::fs::isDirectory(serviceDir)) {
    const std::string serviceHook = apm::fs::joinPath(serviceDir, "99apm-path.sh");
    std::ostringstream service;
    service << "#!/system/bin/sh\n";
    service << shHookLine << "\n";

    if (!writeAtomicFile(serviceHook, service.str(), 0755, true)) {
      warnings.push_back("Failed to install service.d hook: " + serviceHook);
    }
  }

  std::ostringstream summary;
  summary << "export_path: shell hook targets installed=" << installed
          << ", already-present=" << alreadyPresent << ", skipped=" << skipped
          << ", failed=" << failed;
  apm::logger::info(summary.str());

  return true;
}

bool pathSourceFilesPresent() {
  return apm::fs::pathExists(apm::config::getShPathFile()) &&
         apm::fs::pathExists(apm::config::getBashPathFile());
}

void collectHotloadSummary(const std::vector<ShimDecision> &decisions,
                           const std::vector<std::string> &warnings,
                           const std::string &triggerReason,
                           bool ok,
                           apm::daemon::path::CommandHotloadSummary *summary) {
  if (!summary) {
    return;
  }

  summary->ok = ok;
  summary->triggerReason = triggerReason;
  summary->activatedCommands.clear();
  summary->namespacedCommands.clear();
  summary->collisionWarnings = warnings;

  for (const auto &d : decisions) {
    if (d.mode == apm::daemon::path::CommandCollisionResult::Canonical ||
        d.mode == apm::daemon::path::CommandCollisionResult::Namespaced) {
      summary->activatedCommands.push_back(d.shimName);
    }
    if (d.mode == apm::daemon::path::CommandCollisionResult::Namespaced) {
      summary->namespacedCommands.push_back(d.shimName);
    }
    if (!d.warning.empty()) {
      summary->collisionWarnings.push_back(d.warning);
    }
  }

  std::sort(summary->activatedCommands.begin(), summary->activatedCommands.end());
  std::sort(summary->namespacedCommands.begin(), summary->namespacedCommands.end());
  std::sort(summary->collisionWarnings.begin(), summary->collisionWarnings.end());
  summary->collisionWarnings.erase(
      std::unique(summary->collisionWarnings.begin(),
                  summary->collisionWarnings.end()),
      summary->collisionWarnings.end());

  std::ostringstream msg;
  msg << "Hotload rebuild (" << triggerReason << "): "
      << summary->activatedCommands.size() << " shim(s) active";
  if (!summary->namespacedCommands.empty()) {
    msg << ", " << summary->namespacedCommands.size() << " namespaced";
  }
  if (!summary->collisionWarnings.empty()) {
    msg << ", " << summary->collisionWarnings.size() << " warning(s)";
  }
  summary->message = msg.str();
}

} // namespace

namespace apm::daemon::path {

CommandCollisionResult
resolve_command_collision(const std::string &packageName,
                          const std::string &commandName,
                          std::string &resolvedShimName) {
  resolvedShimName.clear();

  if (!isLikelyCommandName(packageName) || !isLikelyCommandName(commandName)) {
    return CommandCollisionResult::Skipped;
  }

  if (!commandExistsInSystemPaths(commandName)) {
    resolvedShimName = commandName;
    return CommandCollisionResult::Canonical;
  }

  std::string namespaced = makeNamespacedCommand(packageName, commandName);
  if (!isLikelyCommandName(namespaced)) {
    return CommandCollisionResult::Skipped;
  }

  if (commandExistsInSystemPaths(namespaced)) {
    return CommandCollisionResult::Skipped;
  }

  resolvedShimName = std::move(namespaced);
  return CommandCollisionResult::Namespaced;
}

bool rebuild_command_index_and_shims(const std::string &triggerReason,
                                     CommandHotloadSummary *summary) {
  std::vector<std::string> warnings;
  bool ok = true;

  const std::string resolvedTrigger =
      triggerReason.empty() ? "unspecified" : triggerReason;

  if (!apm::fs::createDirs(apm::config::getApmRoot()) ||
      !apm::fs::createDirs(apm::config::getApmBinDir()) ||
      !apm::fs::createDirs(apm::config::getPathDir()) ||
      !apm::fs::createDirs(apm::config::getSandboxRoot()) ||
      !apm::fs::createDirs(apm::config::getSandboxStateDir()) ||
      !apm::fs::createDirs(apm::config::getSandboxEnvDir()) ||
      !apm::fs::createDirs(apm::config::getSandboxMountsDir())) {
    warnings.push_back(
        "Failed to prepare runtime directories for hotload rebuild");
    ok = false;
  }

  std::vector<CommandCandidate> candidates;
  discoverStatusCommands(candidates, warnings);
  discoverManualCommands(candidates, warnings);

  std::map<std::string, std::vector<CommandCandidate>> grouped;
  for (const auto &candidate : candidates) {
    grouped[candidate.commandName].push_back(candidate);
  }

  std::vector<ShimDecision> decisions;
  decisions.reserve(candidates.size());

  std::unordered_set<std::string> assignedNames;

  for (auto &it : grouped) {
    auto &providers = it.second;
    std::sort(providers.begin(), providers.end(),
              [](const CommandCandidate &a, const CommandCandidate &b) {
                if (a.packageName != b.packageName) {
                  return a.packageName < b.packageName;
                }
                return a.binaryPath < b.binaryPath;
              });
    providers.erase(
        std::unique(providers.begin(), providers.end(),
                    [](const CommandCandidate &a, const CommandCandidate &b) {
                      return a.packageName == b.packageName &&
                             a.commandName == b.commandName &&
                             a.binaryPath == b.binaryPath;
                    }),
        providers.end());

    const std::string &commandName = it.first;
    const bool systemCollision = commandExistsInSystemPaths(commandName);
    if (systemCollision) {
      warnings.push_back("System command collision for '" + commandName +
                         "'; canonical shim disabled");
    }

    bool canonicalConsumed = systemCollision;

    for (const auto &provider : providers) {
      ShimDecision decision;
      decision.packageName = provider.packageName;
      decision.commandName = provider.commandName;
      decision.binaryPath = provider.binaryPath;
      decision.sourceTermuxEnv = provider.termuxPackage;

      if (!canonicalConsumed) {
        if (!assignedNames.count(commandName)) {
          decision.shimName = commandName;
          decision.mode = CommandCollisionResult::Canonical;
          assignedNames.insert(commandName);
        } else {
          decision.warning =
              "Canonical shim already reserved for command '" + commandName +
              "'";
        }
        canonicalConsumed = true;
      }

      if (decision.mode == CommandCollisionResult::Skipped) {
        std::string namespaced =
            makeNamespacedCommand(provider.packageName, commandName);
        if (!isLikelyCommandName(namespaced)) {
          decision.warning = "Unable to generate valid namespaced shim for '" +
                             provider.packageName + "/" + commandName + "'";
        } else if (commandExistsInSystemPaths(namespaced)) {
          decision.warning = "Namespaced shim collides with system command: '" +
                             namespaced + "'";
        } else if (assignedNames.count(namespaced)) {
          decision.warning =
              "Namespaced shim already assigned: '" + namespaced + "'";
        } else {
          decision.shimName = namespaced;
          decision.mode = CommandCollisionResult::Namespaced;
          assignedNames.insert(namespaced);
        }
      }

      decisions.push_back(std::move(decision));
    }
  }

  std::unordered_set<std::string> oldManaged =
      loadShimsFromManifest(apm::config::getCommandIndexFile());
  auto scannedManaged = loadManagedShimsFromBinDir();
  oldManaged.insert(scannedManaged.begin(), scannedManaged.end());

  std::unordered_set<std::string> newManaged;
  for (auto &decision : decisions) {
    if (decision.mode != CommandCollisionResult::Canonical &&
        decision.mode != CommandCollisionResult::Namespaced) {
      continue;
    }

    std::string shimErr;
    if (!writeShimFile(decision, shimErr)) {
      decision.warning = shimErr;
      decision.mode = CommandCollisionResult::Skipped;
      decision.shimName.clear();
      warnings.push_back(shimErr);
      ok = false;
      continue;
    }

    newManaged.insert(decision.shimName);
  }

  for (const auto &oldShim : oldManaged) {
    if (newManaged.count(oldShim)) {
      continue;
    }

    std::string stalePath = apm::fs::joinPath(apm::config::getApmBinDir(), oldShim);
    if (!apm::fs::removeFile(stalePath)) {
      warnings.push_back("Failed to remove stale shim: " + stalePath);
      ok = false;
    }
  }

  std::string manifestErr;
  if (!writeCommandIndexManifest(decisions, resolvedTrigger, manifestErr)) {
    warnings.push_back(manifestErr);
    ok = false;
  }

  if (!writePathSourceFiles(warnings)) {
    ok = false;
  }
  installShellHooks(warnings);

  if (apm::config::isEmulatorMode()) {
    generateEmulatorEnv();
  }

  collectHotloadSummary(decisions, warnings, resolvedTrigger, ok, summary);

  if (summary) {
    if (summary->ok) {
      apm::logger::info("export_path: " + summary->message);
    } else {
      apm::logger::warn("export_path: " + summary->message);
    }
    for (const auto &warning : summary->collisionWarnings) {
      apm::logger::warn("export_path: " + warning);
    }
  }

  return ok;
}

void refreshPathEnvironment() {
  CommandHotloadSummary summary;
  if (!rebuild_command_index_and_shims("refresh", &summary)) {
    apm::logger::warn("export_path: hotload refresh completed with warnings");
  }
}

void ensureProfileLoaded() {
  if (apm::config::isEmulatorMode()) {
    generateEmulatorEnv();
    return;
  }

  if (!pathSourceFilesPresent()) {
    refreshPathEnvironment();
  }
}

void generateEmulatorEnv() {
  if (!apm::config::isEmulatorMode()) {
    return;
  }

  const std::string envPath = apm::fs::joinPath(apm::config::getApmRoot(), "apm-env.sh");
  std::ostringstream out;
  out << "#!/bin/bash\n";
  out << "# APM Emulator Mode Environment\n";
  out << "# Auto-generated by apmd\n";
  out << "if [ -f " << shellQuote(apm::config::getShPathFile())
      << " ]; then\n";
  out << "  . " << shellQuote(apm::config::getShPathFile()) << "\n";
  out << "fi\n";

  if (!writeAtomicFile(envPath, out.str(), 0755, true)) {
    apm::logger::warn("export_path: failed to write emulator env file: " +
                      envPath);
  }
}

} // namespace apm::daemon::path
