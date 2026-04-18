/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2026 RedHead Industries
 *
 * File: apm.cpp
 * Purpose: Implement the apm CLI, including local commands and IPC-backed
 * operations.
 * Last Modified: 2026-03-23 11:33:54.167465170 -0400.
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
#include "logger.hpp"
#include "manual_package.hpp"
#include "repo_index.hpp"
#include "search.hpp"
#include "security.hpp"
#include "status_db.hpp"
#include "tar_extractor.hpp"
#include "transport.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

// Editable CLI metadata.
static constexpr const char *kApmVersion = "2.0.3b - Open Beta";
static constexpr const char *kApmBuildDate =
    "April 18th, 2026. - 5:00 PM Eastern Time.";
static constexpr const char *kApmCopyright =
    "Copyright (C) 2026 RedHead Industries";
static constexpr const char *kApmLicense =
    "License: GNU GPL v3 or later (GPL-3.0-or-later)";
static constexpr const char *kLogExportDir = "/storage/emulated/0";

enum class LogTarget { Apm, Ams };

enum class LogSelectionKind { Daemon, Module };

struct LogSelection {
  LogSelectionKind kind = LogSelectionKind::Daemon;
  LogTarget daemon = LogTarget::Apm;
  std::string moduleName;
};

struct LogCommandOptions {
  LogSelection selection;
  bool exportFile = false;
  bool clearFile = false;
  bool clearAll = false;
  bool showHelp = false;
};

struct WipeCacheSelection {
  bool apmGeneral = false;
  bool repoLists = false;
  bool packageDownloads = false;
  bool signatureCache = false;
  bool amsRuntime = false;
  bool showHelp = false;
};

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

static bool supportsAnsiTty() {
  if (::isatty(STDOUT_FILENO) == 0)
    return false;

  const char *term = std::getenv("TERM");
  if (!term || !*term)
    return false;

  return std::strcmp(term, "dumb") != 0;
}

// Maintain and render multiple in-place progress lines (one per download).
struct MultiProgressUi {
  std::unordered_map<std::string, std::size_t> index;
  std::vector<std::string> lines;
  std::size_t renderedLines = 0;
  bool supportsAnsi = supportsAnsiTty();
};

static void renderProgressLines(MultiProgressUi &ui) {
  if (ui.lines.empty())
    return;

  if (!ui.supportsAnsi) {
    std::cout << "\r" << ui.lines.back() << std::flush;
    ui.renderedLines = ui.lines.size();
    return;
  }

  if (ui.renderedLines > 0) {
    std::cout << "\r";
    if (ui.renderedLines > 1)
      std::cout << "\033[" << (ui.renderedLines - 1) << "A";
  }

  for (std::size_t i = 0; i < ui.lines.size(); ++i) {
    std::cout << "\r\033[K" << ui.lines[i];
    if (i + 1 < ui.lines.size())
      std::cout << "\n";
  }

  ui.renderedLines = ui.lines.size();
  std::cout << std::flush;
}

static void finalizeProgressLines(MultiProgressUi &ui) {
  if (ui.lines.empty() || ui.renderedLines == 0)
    return;

  std::cout << std::endl;
  ui.renderedLines = 0;
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

static bool parseBooleanArg(const std::string &value, bool &out) {
  const std::string lower = toLower(value);
  if (lower == "1" || lower == "true" || lower == "yes" || lower == "on" ||
      lower == "enable" || lower == "enabled") {
    out = true;
    return true;
  }
  if (lower == "0" || lower == "false" || lower == "no" || lower == "off" ||
      lower == "disable" || lower == "disabled") {
    out = false;
    return true;
  }
  return false;
}

static bool hasSelectedCaches(const WipeCacheSelection &selection) {
  return selection.apmGeneral || selection.repoLists ||
         selection.packageDownloads || selection.signatureCache ||
         selection.amsRuntime;
}

static void selectAllCaches(WipeCacheSelection &selection) {
  selection.apmGeneral = true;
  selection.repoLists = true;
  selection.packageDownloads = true;
  selection.signatureCache = true;
  selection.amsRuntime = true;
}

static std::vector<std::string> splitTokens(const std::string &input) {
  std::vector<std::string> tokens;
  std::string current;

  for (char ch : input) {
    if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',') {
      if (!current.empty()) {
        tokens.push_back(current);
        current.clear();
      }
      continue;
    }
    current.push_back(ch);
  }

  if (!current.empty())
    tokens.push_back(current);

  return tokens;
}

static bool applyWipeCacheToken(const std::string &tokenRaw,
                                WipeCacheSelection &selection,
                                std::string *errorMsg) {
  const std::string token = toLower(tokenRaw);

  if (token.empty())
    return true;
  if (token == "--help" || token == "-h" || token == "help") {
    selection.showHelp = true;
    return true;
  }
  if (token == "all") {
    selectAllCaches(selection);
    return true;
  }
  if (token == "1" || token == "apm" || token == "general" ||
      token == "apm-cache" || token == "cache") {
    selection.apmGeneral = true;
    return true;
  }
  if (token == "2" || token == "repo" || token == "repos" ||
      token == "lists" || token == "repo-lists") {
    selection.repoLists = true;
    return true;
  }
  if (token == "3" || token == "pkg" || token == "pkgs" ||
      token == "package" || token == "packages" || token == "downloads" ||
      token == "package-downloads") {
    selection.packageDownloads = true;
    return true;
  }
  if (token == "4" || token == "sig" || token == "signature" ||
      token == "sig-cache" || token == "signature-cache") {
    selection.signatureCache = true;
    return true;
  }
  if (token == "5" || token == "ams" || token == "runtime" ||
      token == "ams-runtime") {
    selection.amsRuntime = true;
    return true;
  }

  if (errorMsg)
    *errorMsg = "unknown cache target '" + tokenRaw + "'";
  return false;
}

static void printWipeCacheUsage() {
  std::cout
      << "Usage:\n"
      << "  apm wipe-cache [target...]\n"
      << "\nTargets:\n"
      << "  all               Wipe every cache bucket listed below\n"
      << "  apm               Clear the APM general cache under "
      << apm::config::getCacheDir() << "\n"
      << "  repo-lists        Clear cached repository lists under "
      << apm::config::getListsDir() << "\n"
      << "  package-downloads Clear cached package downloads under "
      << apm::config::getPkgsDir() << "\n"
      << "  sig-cache         Clear "
      << apm::fs::joinPath(apm::config::getPkgsDir(), "sig-cache.json") << "\n"
      << "  ams-runtime       Rebuild AMS runtime cache under "
      << apm::config::getModuleRuntimeDir() << "\n"
      << "\nIf no targets are passed, apm will prompt you to choose.\n";
}

static bool promptForWipeCacheSelection(WipeCacheSelection &selection,
                                        std::string *errorMsg) {
  std::cout << "Select cache targets to wipe:\n";
  std::cout << "  1. APM general cache (" << apm::config::getCacheDir()
            << ")\n";
  std::cout << "  2. Repository lists (" << apm::config::getListsDir()
            << ")\n";
  std::cout << "  3. Package downloads (" << apm::config::getPkgsDir()
            << ")\n";
  std::cout << "  4. Signature cache ("
            << apm::fs::joinPath(apm::config::getPkgsDir(), "sig-cache.json")
            << ")\n";
  std::cout << "  5. AMS runtime cache (" << apm::config::getModuleRuntimeDir()
            << ")\n";
  std::cout << "Type numbers, names, or 'all' separated by spaces/commas: "
            << std::flush;

  std::string line;
  if (!std::getline(std::cin, line)) {
    if (errorMsg)
      *errorMsg = "selection aborted";
    return false;
  }

  auto tokens = splitTokens(line);
  if (tokens.empty()) {
    if (errorMsg)
      *errorMsg = "no cache targets selected";
    return false;
  }

  for (const auto &token : tokens) {
    if (!applyWipeCacheToken(token, selection, errorMsg))
      return false;
  }

  return true;
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

static std::string makeAbsolutePath(const std::string &path) {
  if (path.empty() || path.front() == '/')
    return path;

  char cwd[4096];
  if (::getcwd(cwd, sizeof(cwd)) == nullptr)
    return path;
  return apm::fs::joinPath(cwd, path);
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
  std::string tempPath = apm::fs::joinPath(apm::config::getCacheDir(), name.str());
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

static const char *logTargetLabel(LogTarget target) {
  return target == LogTarget::Apm ? "APM daemon" : "AMS daemon";
}

static std::string logSelectionLabel(const LogSelection &selection) {
  if (selection.kind == LogSelectionKind::Module) {
    return "module '" + selection.moduleName + "'";
  }
  return logTargetLabel(selection.daemon);
}

static std::string daemonLogFileName(LogTarget target) {
  return target == LogTarget::Apm ? "apmd.log" : "amsd.log";
}

static std::string moduleLogFileName(const std::string &moduleName) {
  return moduleName + ".log";
}

static void appendUniqueLogPath(const std::string &path,
                                std::vector<std::string> &paths) {
  if (path.empty())
    return;
  if (std::find(paths.begin(), paths.end(), path) == paths.end()) {
    paths.push_back(path);
  }
}

static std::vector<std::string> candidateDaemonLogPaths(LogTarget target) {
  std::vector<std::string> paths;
  const std::string fileName = daemonLogFileName(target);

  const std::string shellPath =
      apm::fs::joinPath(apm::config::getShellLogsDir(), fileName);
  appendUniqueLogPath(shellPath, paths);

  std::string primaryPath;
  if (target == LogTarget::Apm) {
    primaryPath = apm::fs::joinPath(apm::config::getLogsDir(), fileName);
  } else {
    primaryPath = apm::fs::joinPath(apm::config::getModuleLogsDir(), fileName);
  }
  appendUniqueLogPath(primaryPath, paths);

  return paths;
}

static bool isValidModuleLogName(const std::string &moduleName,
                                 std::string *errorMsg) {
  if (moduleName.empty()) {
    if (errorMsg)
      *errorMsg = "module name cannot be empty";
    return false;
  }
  if (moduleName == "." || moduleName == ".." ||
      moduleName.find('/') != std::string::npos ||
      moduleName.find('\\') != std::string::npos) {
    if (errorMsg)
      *errorMsg = "invalid module name for 'apm log': " + moduleName;
    return false;
  }
  return true;
}

static std::vector<std::string> candidateLogPaths(
    const LogSelection &selection) {
  if (selection.kind == LogSelectionKind::Module) {
    std::vector<std::string> paths;
    appendUniqueLogPath(apm::fs::joinPath(apm::config::getModuleLogsDir(),
                                          moduleLogFileName(selection.moduleName)),
                        paths);
    return paths;
  }
  return candidateDaemonLogPaths(selection.daemon);
}

static std::string resolveLogPath(const LogSelection &selection) {
  const auto candidates = candidateLogPaths(selection);
  for (const auto &path : candidates) {
    if (apm::fs::isFile(path)) {
      return path;
    }
  }

  if (!candidates.empty()) {
    return candidates.front();
  }

  return {};
}

static bool parseLogTargetName(const std::string &value, LogTarget &targetOut) {
  if (value == "apm" || value == "apmd") {
    targetOut = LogTarget::Apm;
    return true;
  }
  if (value == "ams" || value == "amsd") {
    targetOut = LogTarget::Ams;
    return true;
  }
  return false;
}

static bool selectDaemonLogTarget(LogTarget requested, bool &targetSet,
                                  LogSelection &selection,
                                  std::string *errorMsg) {
  if (targetSet &&
      (selection.kind != LogSelectionKind::Daemon ||
       selection.daemon != requested)) {
    if (errorMsg) {
      *errorMsg =
          "choose only one log target (--apm, --ams, or a module name) for "
          "'apm log'";
    }
    return false;
  }
  targetSet = true;
  selection.kind = LogSelectionKind::Daemon;
  selection.daemon = requested;
  selection.moduleName.clear();
  return true;
}

static bool selectModuleLogTarget(const std::string &requested,
                                  bool &targetSet, LogSelection &selection,
                                  std::string *errorMsg) {
  if (!isValidModuleLogName(requested, errorMsg))
    return false;

  if (targetSet &&
      (selection.kind != LogSelectionKind::Module ||
       selection.moduleName != requested)) {
    if (errorMsg) {
      *errorMsg =
          "choose only one log target (--apm, --ams, or a module name) for "
          "'apm log'";
    }
    return false;
  }

  targetSet = true;
  selection.kind = LogSelectionKind::Module;
  selection.moduleName = requested;
  return true;
}

static void printLogUsage() {
  std::cout << "Usage:\n"
            << "  apm log [--apm|--ams|--module <name>|<module>] "
               "[--export|--clear]\n"
            << "  apm log --clear-all\n"
            << "\n"
            << "Defaults to the APM daemon log.\n"
            << "Use --export to copy the selected log to "
            << kLogExportDir << ".\n"
            << "Use --clear to delete the selected log, or --clear-all to "
               "delete all daemon and module logs.\n"
            << "Clearing logs requires an authenticated daemon session.\n";
}

static bool parseLogCommandArgs(int argc, char **argv, LogCommandOptions &out,
                                std::string *errorMsg) {
  bool targetSet = false;

  for (int idx = 0; idx < argc; ++idx) {
    std::string arg = argv[idx];
    if (arg == "--help" || arg == "-h") {
      out.showHelp = true;
      continue;
    }
    if (arg == "--export") {
      out.exportFile = true;
      continue;
    }
    if (arg == "--clear") {
      out.clearFile = true;
      continue;
    }
    if (arg == "--clear-all") {
      out.clearAll = true;
      continue;
    }
    if (arg == "--apm") {
      if (!selectDaemonLogTarget(LogTarget::Apm, targetSet, out.selection,
                                 errorMsg))
        return false;
      continue;
    }
    if (arg == "--ams") {
      if (!selectDaemonLogTarget(LogTarget::Ams, targetSet, out.selection,
                                 errorMsg))
        return false;
      continue;
    }
    if (arg == "--module") {
      if (idx + 1 >= argc) {
        if (errorMsg)
          *errorMsg = "missing value after '--module'";
        return false;
      }
      if (!selectModuleLogTarget(argv[++idx], targetSet, out.selection,
                                 errorMsg))
        return false;
      continue;
    }
    if (arg.rfind("--module=", 0) == 0) {
      if (!selectModuleLogTarget(arg.substr(std::strlen("--module=")), targetSet,
                                 out.selection, errorMsg))
        return false;
      continue;
    }
    if (arg == "--daemon") {
      if (idx + 1 >= argc) {
        if (errorMsg)
          *errorMsg = "missing value after '--daemon'";
        return false;
      }
      LogTarget parsed = LogTarget::Apm;
      const std::string value = argv[++idx];
      if (!parseLogTargetName(value, parsed)) {
        if (errorMsg)
          *errorMsg = "unknown daemon for 'apm log': " + value;
        return false;
      }
      if (!selectDaemonLogTarget(parsed, targetSet, out.selection, errorMsg))
        return false;
      continue;
    }
    if (arg.rfind("--daemon=", 0) == 0) {
      LogTarget parsed = LogTarget::Apm;
      const std::string value = arg.substr(std::strlen("--daemon="));
      if (!parseLogTargetName(value, parsed)) {
        if (errorMsg)
          *errorMsg = "unknown daemon for 'apm log': " + value;
        return false;
      }
      if (!selectDaemonLogTarget(parsed, targetSet, out.selection, errorMsg))
        return false;
      continue;
    }

    LogTarget parsed = LogTarget::Apm;
    if (parseLogTargetName(arg, parsed)) {
      if (!selectDaemonLogTarget(parsed, targetSet, out.selection, errorMsg))
        return false;
      continue;
    }

    if (!arg.empty() && arg[0] != '-') {
      if (!selectModuleLogTarget(arg, targetSet, out.selection, errorMsg))
        return false;
      continue;
    }

    if (errorMsg)
      *errorMsg = "unknown option for 'apm log': " + arg;
    return false;
  }

  const int actionCount = (out.exportFile ? 1 : 0) + (out.clearFile ? 1 : 0) +
                          (out.clearAll ? 1 : 0);
  if (actionCount > 1) {
    if (errorMsg) {
      *errorMsg =
          "choose only one action (--export, --clear, or --clear-all) for "
          "'apm log'";
    }
    return false;
  }

  if (out.clearAll && targetSet) {
    if (errorMsg) {
      *errorMsg = "'--clear-all' cannot be combined with --apm, --ams, or a "
                  "module name";
    }
    return false;
  }

  return true;
}

static std::string buildLogExportPath(const LogSelection &selection) {
  std::time_t now = std::time(nullptr);
  std::tm tm {};
#if defined(_POSIX_VERSION)
  ::localtime_r(&now, &tm);
#else
  std::tm *tmp = std::localtime(&now);
  if (tmp)
    tm = *tmp;
#endif

  std::ostringstream name;
  if (selection.kind == LogSelectionKind::Module) {
    name << selection.moduleName;
  } else {
    name << (selection.daemon == LogTarget::Apm ? "apmd" : "amsd");
  }
  name << "-" << std::put_time(&tm, "%Y%m%d-%H%M%S") << ".log";
  return apm::fs::joinPath(kLogExportDir, name.str());
}

static std::string logSelectionWireTarget(const LogSelection &selection) {
  if (selection.kind == LogSelectionKind::Module)
    return "module";
  return selection.daemon == LogTarget::Apm ? "apm" : "ams";
}

static bool copyFileToPath(const std::string &src, const std::string &dst,
                           std::string *errorMsg) {
  std::ifstream in(src, std::ios::binary);
  if (!in.is_open()) {
    if (errorMsg)
      *errorMsg = "failed to open source log: " + src;
    return false;
  }

  auto slash = dst.find_last_of('/');
  if (slash != std::string::npos) {
    std::string parent = dst.substr(0, slash);
    if (!parent.empty() && !apm::fs::createDirs(parent)) {
      if (errorMsg)
        *errorMsg = "failed to create export directory: " + parent;
      return false;
    }
  }

  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out.is_open()) {
    if (errorMsg)
      *errorMsg = "failed to open export path: " + dst;
    return false;
  }

  out << in.rdbuf();
  if (!out.good()) {
    if (errorMsg)
      *errorMsg = "failed while writing export file: " + dst;
    return false;
  }

  return true;
}

static bool writeLogRange(const std::string &path, off_t offset, off_t size,
                          std::string *errorMsg) {
  if (size <= offset)
    return true;

  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    if (errorMsg)
      *errorMsg = "failed to open log file: " + path;
    return false;
  }

  in.seekg(offset, std::ios::beg);
  if (!in.good()) {
    if (errorMsg)
      *errorMsg = "failed to seek log file: " + path;
    return false;
  }

  char buf[4096];
  off_t remaining = size - offset;
  while (remaining > 0) {
    std::streamsize chunk = remaining > static_cast<off_t>(sizeof(buf))
                                ? static_cast<std::streamsize>(sizeof(buf))
                                : static_cast<std::streamsize>(remaining);
    in.read(buf, chunk);
    std::streamsize got = in.gcount();
    if (got <= 0)
      break;
    std::cout.write(buf, got);
    remaining -= static_cast<off_t>(got);
  }

  if (!in.good() && !in.eof()) {
    if (errorMsg)
      *errorMsg = "failed while reading log file: " + path;
    return false;
  }

  std::cout.flush();
  return true;
}

// -----------------------------------------------------------------------------
// Session + authentication helpers
// -----------------------------------------------------------------------------

using SecurityQaList = std::vector<std::pair<std::string, std::string>>;

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

static bool promptForSecurityQuestions(SecurityQaList &qaOut) {
  qaOut.clear();
  std::cout << "Set security questions (used for password/PIN recovery):\n";
  for (std::size_t idx = 0; idx < apm::security::SECURITY_QUESTION_COUNT;
       ++idx) {
    std::string question;
    std::cout << "  Question " << (idx + 1) << ": " << std::flush;
    if (!std::getline(std::cin, question) || question.empty()) {
      std::cerr << "Security question cannot be empty.\n";
      return false;
    }

    std::string answer;
    std::cout << "  Answer " << (idx + 1) << ": " << std::flush;
    if (!std::getline(std::cin, answer) || answer.empty()) {
      std::cerr << "Security answers cannot be empty.\n";
      return false;
    }

    qaOut.emplace_back(std::move(question), std::move(answer));
  }

  return true;
}

static bool requestSessionUnlock(const std::string &action,
                                 const std::string &secret,
                                 const SecurityQaList &securityQa,
                                 std::string &sessionTokenOut,
                                 std::string *failureMessageOut = nullptr,
                                 bool printFailure = true) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Authenticate;
  req.id = "authenticate-1";
  req.authAction = action;
  req.authSecret = secret;

  for (std::size_t idx = 0; idx < securityQa.size(); ++idx) {
    req.rawFields["security_q" + std::to_string(idx + 1)] =
        securityQa[idx].first;
    req.rawFields["security_a" + std::to_string(idx + 1)] =
        securityQa[idx].second;
  }

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    const std::string message = "Error: " + err;
    if (failureMessageOut)
      *failureMessageOut = message;
    if (printFailure)
      std::cerr << message << "\n";
    return false;
  }

  if (!resp.success) {
    const std::string message =
        resp.message.empty() ? "Authentication failed" : resp.message;
    if (failureMessageOut)
      *failureMessageOut = message;
    if (printFailure)
      std::cerr << message << "\n";
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

static bool ensureAuthenticatedSession(std::string &sessionTokenOut) {
  bool hadSession = false;
  if (loadActiveSessionToken(sessionTokenOut, hadSession))
    return true;

  if (hadSession)
    std::cout << "APM security session expired. Please re-authenticate.\n";

  std::string secret;
  if (!promptForSecret("Enter APM password/PIN: ", secret)) {
    std::cerr << "Failed to read password/PIN input.\n";
    return false;
  }

  std::string authErr;
  if (requestSessionUnlock("unlock", secret, SecurityQaList{}, sessionTokenOut,
                           &authErr, false)) {
    return true;
  }

  if (authErr != "Password/PIN not configured") {
    if (!authErr.empty())
      std::cerr << authErr << "\n";
    return false;
  }

  std::string newSecret;
  if (!promptForNewSecret(newSecret)) {
    std::cerr << "Password/PIN setup aborted.\n";
    return false;
  }

  SecurityQaList securityQa;
  if (!promptForSecurityQuestions(securityQa)) {
    std::cerr << "Security question setup aborted.\n";
    return false;
  }

  return requestSessionUnlock("set", newSecret, securityQa, sessionTokenOut);
}

static void attachSession(apm::ipc::Request &req,
                          const std::string &sessionToken) {
  if (!sessionToken.empty())
    req.sessionToken = sessionToken;
}

static bool requiresAuthSession(const std::string &cmd) {
  return cmd == "update" || cmd == "add-repo" || cmd == "install" ||
         cmd == "remove" || cmd == "upgrade" || cmd == "autoremove" ||
         cmd == "module-install" || cmd == "module-list" ||
         cmd == "module-enable" || cmd == "module-disable" ||
         cmd == "module-remove" || cmd == "apk-install" ||
         cmd == "apk-uninstall" || cmd == "factory-reset" ||
         cmd == "debuglogging";
}

static bool ensureManualSlotAvailable(const std::string &pkgName,
                                      std::string *errorMsg) {
  if (pkgName.empty()) {
    if (errorMsg)
      *errorMsg = "Package name is empty";
    return false;
  }
  std::string nameErr;
  if (!apm::security::validatePackageName(pkgName, &nameErr)) {
    if (errorMsg)
      *errorMsg = nameErr;
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
// apm::config::getInstalledDir() before touching the filesystem.
static bool prefixWithinInstalledRoot(const std::string &prefix) {
  const std::string root = apm::config::getInstalledDir();
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
      apm::fs::joinPath(apm::config::getCommandsDir(), pkgName);
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
          "prefix must be under " + std::string(apm::config::getInstalledDir());
    return false;
  }

  if (info.prefix == apm::config::getInstalledDir()) {
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
      << "  apm <command> [args...]\n"
      << "\nCommands:\n"
      << "  ping                        Check connection to apmd\n"
      << "  update                      Update repository metadata\n"
      << "  add-repo <file.repo>        Add a repository source file\n"
      << "  install <pkg>               Install a package from repo\n"
      << "  package-install <file>      Install a local .deb/.gz/.xz\n"
      << "  remove <pkg>                Remove an installed package\n"
      << "  upgrade [pkgs...]           Upgrade all or selected packages\n"
      << "  autoremove                  Remove unused auto-installed deps\n"
      << "  debuglogging <true|false>   Enable or disable daemon debug logging\n"
      << "  factory-reset               Reset APM data and installed content\n"
      << "  wipe-cache [targets...]     Wipe selected APM/AMS cache buckets\n"
      << "  forgot-password             Recover access with security "
         "questions\n"
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
      << "  log [...]                   View, export, or clear logs\n"
      << "  version                     Show APM version, build, and license info\n"
      << "  key-add <file.asc|.gpg>     Import a trusted public key\n"
      << "  sig-cache show              Show cached .deb signature verifications\n"
      << "  sig-cache clear             Clear the signature cache\n"
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
int cmdPing() {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Ping;
  req.id = "ping-1";

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "OK: " : "ERROR: ")
            << (resp.message.empty() ? "(no message)" : resp.message) << "\n";
  return resp.success ? 0 : 1;
}

int cmdForgotPassword() {
  apm::ipc::Request qReq;
  qReq.type = apm::ipc::RequestType::ForgotPassword;
  qReq.id = "forgot-questions-1";
  qReq.rawFields["reset_stage"] = "questions";

  apm::ipc::Response qResp;
  std::string err;

  if (!apm::ipc::sendRequestAuto(qReq, qResp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  if (!qResp.success) {
    std::cerr << (qResp.message.empty() ? "Unable to start recovery"
                                        : qResp.message)
              << "\n";
    return 1;
  }

  std::vector<std::string> questions;
  for (std::size_t idx = 0; idx < apm::security::SECURITY_QUESTION_COUNT;
       ++idx) {
    auto it = qResp.rawFields.find("security_q" + std::to_string(idx + 1));
    if (it == qResp.rawFields.end() || it->second.empty()) {
      std::cerr << "Security question data missing. Cannot continue.\n";
      return 1;
    }
    questions.push_back(it->second);
  }

  std::vector<std::string> answers;
  answers.reserve(questions.size());
  for (std::size_t idx = 0; idx < questions.size(); ++idx) {
    std::cout << "Security question " << (idx + 1) << ": " << questions[idx]
              << "\n";
    std::cout << "Answer: " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer) || answer.empty()) {
      std::cerr << "Answer cannot be empty. Aborting.\n";
      return 1;
    }
    answers.push_back(std::move(answer));
  }

  apm::ipc::Request verifyReq;
  verifyReq.type = apm::ipc::RequestType::ForgotPassword;
  verifyReq.id = "forgot-verify-1";
  verifyReq.rawFields["reset_stage"] = "verify";
  for (std::size_t idx = 0; idx < answers.size(); ++idx) {
    verifyReq.rawFields["security_a" + std::to_string(idx + 1)] = answers[idx];
  }

  apm::ipc::Response verifyResp;
  if (!apm::ipc::sendRequestAuto(verifyReq, verifyResp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }
  if (!verifyResp.success) {
    std::cerr << (verifyResp.message.empty() ? "Security answers incorrect"
                                             : verifyResp.message)
              << "\n";
    return 1;
  }

  if (!verifyResp.message.empty())
    std::cout << verifyResp.message << "\n";

  std::string newSecret;
  if (!promptForNewSecret(newSecret)) {
    std::cerr << "Password/PIN reset aborted.\n";
    return 1;
  }

  apm::ipc::Request resetReq;
  resetReq.type = apm::ipc::RequestType::ForgotPassword;
  resetReq.id = "forgot-reset-1";
  resetReq.rawFields["reset_stage"] = "reset";
  resetReq.rawFields["new_secret"] = newSecret;
  for (std::size_t idx = 0; idx < answers.size(); ++idx) {
    resetReq.rawFields["security_a" + std::to_string(idx + 1)] = answers[idx];
  }

  apm::ipc::Response resetResp;
  if (!apm::ipc::sendRequestAuto(resetReq, resetResp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  if (!resetResp.success) {
    std::cerr << (resetResp.message.empty() ? "Failed to reset password/PIN"
                                            : resetResp.message)
              << "\n";
    return 1;
  }

  if (!resetResp.message.empty())
    std::cout << resetResp.message << "\n";

  return 0;
}

// Trigger repository metadata refresh via the daemon so Packages lists stay
// current.
int cmdUpdate(const std::string &sessionToken) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Update;
  req.id = "update-1";
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;
  struct UpdateProgressUi {
    bool supportsAnsi = supportsAnsiTty();
    bool rendered = false;
    std::string activeKey;
    std::chrono::steady_clock::time_point lastRender{};
  } ui;

  auto finalizeUpdateProgress = [&ui]() {
    if (!ui.rendered)
      return;
    std::cout << "\n";
    ui.rendered = false;
  };

  auto onProgress = [&](const apm::ipc::Response &chunk) {
    auto eventIt = chunk.rawFields.find("event");
    if (eventIt == chunk.rawFields.end() || eventIt->second != "repo-update")
      return;
    if (!ui.supportsAnsi)
      return;

    auto getField = [&](const std::string &key) -> std::string {
      auto it = chunk.rawFields.find(key);
      return it != chunk.rawFields.end() ? it->second : "";
    };

    const std::string stage = getField("stage");
    const std::string remote = getField("remote");
    const std::string component = getField("component");
    const std::string key = stage + ":" + remote + ":" + component;

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
    line << label << " " << buildProgressBar(ratio) << " "
         << formatBytes(current) << "/";
    if (total > 0)
      line << formatBytes(total);
    else
      line << "??";
    line << "  " << formatSpeed(dlSpeed) << "↓ " << formatSpeed(ulSpeed) << "↑";
    if (finished)
      line << "  done";

    const auto now = std::chrono::steady_clock::now();
    const bool keyChanged = (key != ui.activeKey);
    constexpr auto kMinRenderInterval = std::chrono::milliseconds(120);
    if (!finished && ui.rendered && !keyChanged &&
        (now - ui.lastRender) < kMinRenderInterval) {
      return;
    }

    ui.activeKey = key;
    ui.lastRender = now;
    std::cout << "\r\033[K" << line.str() << std::flush;
    ui.rendered = true;
  };

  if (!apm::ipc::sendRequestAuto(req, resp, &err, onProgress)) {
    finalizeUpdateProgress();
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  finalizeUpdateProgress();

  std::cout << (resp.success ? "Update succeeded" : "Update failed");
  if (!resp.message.empty())
    std::cout << ": " << resp.message;
  std::cout << "\n";
  return resp.success ? 0 : 1;
}

// Ask the daemon to validate and install a .repo source file.
int cmdAddRepo(const std::string &sessionToken, const std::string &path) {
  if (path.size() < 5 || path.compare(path.size() - 5, 5, ".repo") != 0) {
    std::cerr << "apm: add-repo requires a .repo file\n";
    return 1;
  }

  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::AddRepo;
  req.id = "add-repo-1";
  req.repoPath = makeAbsolutePath(path);
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;
  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  if (!resp.success) {
    std::cerr << "Add repo failed: "
              << (resp.message.empty() ? "unknown error" : resp.message)
              << "\n";
    return 1;
  }

  std::cout << (resp.message.empty() ? "Repository source added"
                                     : resp.message)
            << "\n";
  return 0;
}

// Ask the daemon to install a package along with its dependencies.
int cmdInstall(const std::string &sessionToken, const std::string &pkg) {
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

  if (!apm::ipc::sendRequestAuto(planReq, planResp, &err)) {
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

  MultiProgressUi ui;

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
    auto idxIt = ui.index.find(key);
    std::size_t idx;
    if (idxIt == ui.index.end()) {
      idx = ui.lines.size();
      ui.index.emplace(key, idx);
      ui.lines.emplace_back();
    } else {
      idx = idxIt->second;
    }

    std::string label =
        pkgName.empty() ? fileLabel : pkgName + " - " + fileLabel;

    std::ostringstream line;
    line << label << " " << buildProgressBar(ratio) << " "
         << formatBytes(current) << "/";
    if (total > 0)
      line << formatBytes(total);
    else
      line << "??";
    line << "  " << formatSpeed(dlSpeed) << "↓ " << formatSpeed(ulSpeed) << "↑";
    if (finished)
      line << "  done";

    ui.lines[idx] = line.str();
    renderProgressLines(ui);
  };

  if (!apm::ipc::sendRequestAuto(req, resp, &err, onProgress)) {
    finalizeProgressLines(ui);
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  finalizeProgressLines(ui);

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
int cmdRemove(const std::string &sessionToken, const std::string &pkg) {
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

  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "Remove succeeded: " : "Remove failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

// Forward an APK install request to the daemon, optionally forcing a system
// install overlay.
int cmdApkInstall(const std::string &sessionToken, const std::string &apk,
                  bool installAsSystem) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::ApkInstall;
  req.id = "apk-install-1";
  attachSession(req, sessionToken);

  req.apkPath = apk;
  req.installAsSystem = installAsSystem;

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "APK install succeeded: "
                             : "APK install failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

// Forward an APK uninstall request to the daemon for a given package name.
int cmdApkUninstall(const std::string &sessionToken,
                    const std::string &pkgName) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::ApkUninstall;
  req.id = "apk-uninstall-1";
  req.packageName = pkgName;
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "APK uninstall succeeded: "
                             : "APK uninstall failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

// Request upgrades for either all installed packages or a provided subset.
int cmdUpgrade(const std::string &sessionToken, int argc, char **argv) {
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

  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "Upgrade succeeded: " : "Upgrade failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

// Ask the daemon to remove auto-installed dependencies no longer needed.
int cmdAutoremove(const std::string &sessionToken) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Autoremove;
  req.id = "autoremove-1";
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "Autoremove succeeded: " : "Autoremove failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

int cmdDebugLogging(const std::string &sessionToken, const std::string &value) {
  bool enabled = false;
  if (!parseBooleanArg(value, enabled)) {
    std::cerr << "apm: invalid value for 'debuglogging': " << value << "\n"
              << "Use one of: true, false, on, off, enable, disable\n";
    return 1;
  }

  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::DebugLogging;
  req.id = "debuglogging-1";
  req.debugLoggingEnabled = enabled;
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "" : "Failed: ")
            << (resp.message.empty()
                    ? (enabled ? "Debug logging enabled."
                               : "Debug logging disabled.")
                    : resp.message)
            << "\n";
  return resp.success ? 0 : 1;
}

int cmdFactoryReset(const std::string &sessionToken) {
  std::cout << "APM factory reset will:\n";
  std::cout << "  - Remove installed commands and dependencies under "
            << apm::config::getInstalledDir() << "\n";
  std::cout << "  - Clear APM shim binaries under " << apm::config::getApmBinDir()
            << "\n";
  std::cout << "  - Remove manual package metadata under "
            << apm::config::getManualPackagesDir() << "\n";
  std::cout << "  - Clear password/PIN and session data under "
            << apm::config::getSecurityDir() << "\n";
  std::cout << "  - Reset package status database at "
            << apm::config::getStatusFile() << "\n";
  std::cout << "  - Remove AMS modules under " << apm::config::getModulesDir()
            << "\n";
  std::cout << "  - Delete repository lists under " << apm::config::getListsDir()
            << "\n";
  std::cout << "  - Uninstall system apps staged with --install-as-system\n";
  std::cout << "Proceed with factory reset? [y/N]: " << std::flush;

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
    std::cout << "Factory reset aborted.\n";
    return 0;
  }

  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::FactoryReset;
  req.id = "factory-reset-1";
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  if (!resp.success) {
    std::cerr << "Factory reset failed: "
              << (resp.message.empty() ? "unknown error" : resp.message)
              << "\n";
    return 1;
  }

  std::cout << (resp.message.empty() ? "Factory reset completed."
                                     : resp.message)
            << "\n";
  std::cout << "Please reboot your device to finish the reset.\n";
  return 0;
}

int cmdWipeCache(int argc, char **argv) {
  WipeCacheSelection selection;
  std::string parseErr;

  for (int idx = 0; idx < argc; ++idx) {
    for (const auto &token : splitTokens(argv[idx])) {
      if (!applyWipeCacheToken(token, selection, &parseErr)) {
        std::cerr << "apm wipe-cache: " << parseErr << "\n";
        printWipeCacheUsage();
        return 1;
      }
    }
  }

  if (selection.showHelp) {
    printWipeCacheUsage();
    return 0;
  }

  if (!hasSelectedCaches(selection)) {
    if (!promptForWipeCacheSelection(selection, &parseErr)) {
      std::cerr << "apm wipe-cache: " << parseErr << "\n";
      return 1;
    }
  }

  if (!hasSelectedCaches(selection)) {
    std::cerr << "apm wipe-cache: no cache targets selected\n";
    return 1;
  }

  std::cout << "APM cache wipe will:\n";
  if (selection.apmGeneral) {
    std::cout << "  - Clear APM general cache under "
              << apm::config::getCacheDir() << "\n";
  }
  if (selection.repoLists) {
    std::cout << "  - Delete repository lists under "
              << apm::config::getListsDir() << "\n";
  }
  if (selection.packageDownloads) {
    std::cout << "  - Remove cached package downloads under "
              << apm::config::getPkgsDir();
    if (!selection.signatureCache)
      std::cout << " (keeping sig-cache.json)";
    std::cout << "\n";
  }
  if (selection.signatureCache) {
    std::cout << "  - Clear signature cache at "
              << apm::fs::joinPath(apm::config::getPkgsDir(), "sig-cache.json")
              << "\n";
  }
  if (selection.amsRuntime) {
    std::cout << "  - Rebuild AMS runtime cache under "
              << apm::config::getModuleRuntimeDir() << "\n";
  }
  std::cout << "Proceed with cache wipe? [y/N]: " << std::flush;

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
    std::cout << "Cache wipe aborted.\n";
    return 0;
  }

  std::string sessionToken;
  if (!ensureAuthenticatedSession(sessionToken))
    return 1;

  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::WipeCache;
  req.id = "wipe-cache-1";
  attachSession(req, sessionToken);
  req.rawFields["wipe_apm_cache"] = selection.apmGeneral ? "true" : "false";
  req.rawFields["wipe_repo_lists"] = selection.repoLists ? "true" : "false";
  req.rawFields["wipe_package_downloads"] =
      selection.packageDownloads ? "true" : "false";
  req.rawFields["wipe_signature_cache"] =
      selection.signatureCache ? "true" : "false";
  req.rawFields["wipe_ams_runtime"] = selection.amsRuntime ? "true" : "false";

  apm::ipc::Response resp;
  std::string err;
  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.message.empty()
                    ? (resp.success ? "Selected caches cleared."
                                    : "Cache wipe failed.")
                    : resp.message)
            << "\n";
  return resp.success ? 0 : 1;
}

int cmdModuleInstall(const std::string &sessionToken,
                     const std::string &zipPath) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::ModuleInstall;
  req.id = "module-install-1";
  req.modulePath = zipPath;
  req.rawFields["module_path"] = zipPath;
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? "Module install succeeded: "
                             : "Module install failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

static int runModuleToggle(const std::string &sessionToken,
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

  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.success ? verb + " succeeded: " : verb + " failed: ")
            << resp.message << "\n";
  return resp.success ? 0 : 1;
}

int cmdModuleEnable(const std::string &sessionToken, const std::string &name) {
  return runModuleToggle(sessionToken,
                         apm::ipc::RequestType::ModuleEnable, name,
                         "Module enable");
}

int cmdModuleDisable(const std::string &sessionToken, const std::string &name) {
  return runModuleToggle(sessionToken,
                         apm::ipc::RequestType::ModuleDisable, name,
                         "Module disable");
}

int cmdModuleRemove(const std::string &sessionToken, const std::string &name) {
  return runModuleToggle(sessionToken,
                         apm::ipc::RequestType::ModuleRemove, name,
                         "Module remove");
}

int cmdModuleList(const std::string &sessionToken) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::ModuleList;
  req.id = "module-list-1";
  attachSession(req, sessionToken);

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
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
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::List;

  apm::ipc::Response resp;
  std::string err;
  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  if (!resp.message.empty())
    std::cout << resp.message << "\n";
  return resp.success ? 0 : 1;
}

// Display local installation info plus candidate repo metadata for a package.
int cmdInfoLocal(const std::string &name) {
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::Info;
  req.packageName = name;

  apm::ipc::Response resp;
  std::string err;
  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  if (!resp.message.empty())
    std::cout << resp.message << "\n";
  return resp.success ? 0 : 1;
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
#ifdef APM_EMULATOR_MODE
  if (apm::config::isEmulatorMode())
    std::cout << " (Emulator Mode)";
#endif
  std::cout << "\n"
            << kApmCopyright << "\n"
            << kApmLicense << "\n"
            << "Free software: you may redistribute and modify it.\n"
            << "Distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;\n"
            << "without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n"
            << "See LICENSE for details.\n";
  return 0;
}

// Import an ASCII-armored public key into the trusted key directory.
int cmdKeyAdd(const std::string &path) {
  std::string fingerprint;
  std::string storedPath;
  std::string error;

  if (!apm::crypto::importTrustedPublicKey(path, apm::config::getTrustedKeysDir(),
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

// Show the contents of the .deb signature cache.
int cmdSigCacheShow() {
  std::string cachePath = apm::fs::joinPath(apm::config::getPkgsDir(), "sig-cache.json");
  if (!apm::fs::pathExists(cachePath)) {
    std::cout << "Signature cache is empty (no cache file found)\n";
    return 0;
  }

  std::string content;
  if (!apm::fs::readFile(cachePath, content)) {
    std::cerr << "Failed to read cache file: " << cachePath << "\n";
    return 1;
  }

  if (content.empty() || content == "{\n}\n" || content == "{}\n") {
    std::cout << "Signature cache is empty\n";
    return 0;
  }

  std::cout << "Signature Cache:\n";
  std::cout << "Location: " << cachePath << "\n\n";
  
  // Simple JSON parser to extract entries
  std::istringstream in(content);
  std::string line;
  std::string currentSha;
  std::string sigType, sigSource, sigPath, verifiedBy;
  
  while (std::getline(in, line)) {
    // Trim whitespace
    size_t start = line.find_first_not_of(" \t");
    if (start == std::string::npos) continue;
    line = line.substr(start);
    
    // Look for SHA256 key
    if (line[0] == '"' && line.find("\":") != std::string::npos) {
      size_t end = line.find('"', 1);
      if (end != std::string::npos) {
        currentSha = line.substr(1, end - 1);
        sigType.clear(); sigSource.clear(); sigPath.clear(); verifiedBy.clear();
      }
    }
    // Parse fields
    else if (line.find("\"sigType\":") != std::string::npos) {
      size_t valStart = line.find('"', line.find(':'));
      if (valStart != std::string::npos) {
        size_t valEnd = line.find('"', valStart + 1);
        if (valEnd != std::string::npos)
          sigType = line.substr(valStart + 1, valEnd - valStart - 1);
      }
    }
    else if (line.find("\"sigSource\":") != std::string::npos) {
      size_t valStart = line.find('"', line.find(':'));
      if (valStart != std::string::npos) {
        size_t valEnd = line.find('"', valStart + 1);
        if (valEnd != std::string::npos)
          sigSource = line.substr(valStart + 1, valEnd - valStart - 1);
      }
    }
    else if (line.find("\"sigPath\":") != std::string::npos) {
      size_t valStart = line.find('"', line.find(':'));
      if (valStart != std::string::npos) {
        size_t valEnd = line.find('"', valStart + 1);
        if (valEnd != std::string::npos)
          sigPath = line.substr(valStart + 1, valEnd - valStart - 1);
      }
    }
    else if (line.find("\"verifiedBy\":") != std::string::npos) {
      size_t valStart = line.find('"', line.find(':'));
      if (valStart != std::string::npos) {
        size_t valEnd = line.find('"', valStart + 1);
        if (valEnd != std::string::npos)
          verifiedBy = line.substr(valStart + 1, valEnd - valStart - 1);
      }
    }
    // End of entry
    else if (line[0] == '}' && !currentSha.empty()) {
      std::cout << "Package SHA256: " << currentSha << "\n";
      std::cout << "  Signature Type: " << (sigType.empty() ? "(unknown)" : sigType) << "\n";
      std::cout << "  Source:         " << (sigSource.empty() ? "(unknown)" : sigSource) << "\n";
      std::cout << "  Path:           " << (sigPath.empty() ? "(none)" : sigPath) << "\n";
      std::cout << "  Verified By:    " << (verifiedBy.empty() ? "(not recorded)" : verifiedBy) << "\n";
      std::cout << "\n";
      currentSha.clear();
    }
  }
  
  return 0;
}

// Clear the .deb signature cache.
int cmdSigCacheClear() {
  std::string cachePath = apm::fs::joinPath(apm::config::getPkgsDir(), "sig-cache.json");
  if (!apm::fs::pathExists(cachePath)) {
    std::cout << "Signature cache is already empty\n";
    return 0;
  }

  if (!apm::fs::removeFile(cachePath)) {
    std::cerr << "Failed to remove cache file: " << cachePath << "\n";
    return 1;
  }

  std::cout << "Signature cache cleared\n";
  return 0;
}

static int cmdLogClear(const LogCommandOptions &options) {
  std::string sessionToken;
  if (!ensureAuthenticatedSession(sessionToken))
    return 1;

  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::LogClear;
  req.id = options.clearAll ? "log-clear-all-1" : "log-clear-1";
  attachSession(req, sessionToken);

  if (options.clearAll) {
    req.rawFields["clear_all"] = "true";
  } else {
    req.rawFields["log_target"] = logSelectionWireTarget(options.selection);
    if (options.selection.kind == LogSelectionKind::Module)
      req.moduleName = options.selection.moduleName;
  }

  apm::ipc::Response resp;
  std::string err;
  if (!apm::ipc::sendRequestAuto(req, resp, &err)) {
    std::cerr << "Error: " << err << "\n";
    return 1;
  }

  std::cout << (resp.message.empty()
                    ? (resp.success ? "Log clear completed." :
                                      "Log clear failed.")
                    : resp.message)
            << "\n";
  return resp.success ? 0 : 1;
}

int cmdLog(int argc, char **argv) {
  LogCommandOptions options;
  std::string parseErr;
  if (!parseLogCommandArgs(argc, argv, options, &parseErr)) {
    std::cerr << "apm log: " << parseErr << "\n";
    printLogUsage();
    return 1;
  }

  if (options.showHelp) {
    printLogUsage();
    return 0;
  }

  if (options.clearFile) {
    return cmdLogClear(options);
  }
  if (options.clearAll)
    return cmdLogClear(options);

  const std::string logPath = resolveLogPath(options.selection);

  if (options.exportFile) {
    if (!apm::fs::isFile(logPath)) {
      std::cerr << "apm log: log file not found: " << logPath << "\n";
      return 1;
    }

    const std::string exportPath = buildLogExportPath(options.selection);
    std::string copyErr;
    if (!copyFileToPath(logPath, exportPath, &copyErr)) {
      std::cerr << "apm log: " << copyErr << "\n";
      return 1;
    }

    std::cout << "Exported " << logSelectionLabel(options.selection)
              << " log to " << exportPath << "\n";
    return 0;
  }

  std::cout << "Following " << logSelectionLabel(options.selection)
            << " log at " << logPath << "\n";
  std::cout << "Press Ctrl-C to stop.\n";

  bool fileSeen = false;
  bool waitingShown = false;
  ino_t currentInode = 0;
  off_t currentOffset = 0;

  while (true) {
    struct stat st {};
    if (::stat(logPath.c_str(), &st) != 0) {
      if (!waitingShown) {
        std::cerr << "apm log: waiting for "
                  << logSelectionLabel(options.selection)
                  << " log to appear...\n";
        waitingShown = true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }

    if (!fileSeen) {
      fileSeen = true;
      currentInode = st.st_ino;
      currentOffset = 0;
    } else if (st.st_ino != currentInode || st.st_size < currentOffset) {
      std::cout << "\n[log restarted]\n";
      currentInode = st.st_ino;
      currentOffset = 0;
    }

    waitingShown = false;

    std::string readErr;
    if (!writeLogRange(logPath, currentOffset, st.st_size, &readErr)) {
      std::cerr << "\napm log: " << readErr << "\n";
      return 1;
    }
    currentOffset = st.st_size;

    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

} // namespace

//
// ============================================================
// MAIN
// ============================================================
//

// Entry point that parses global options and dispatches to subcommands.
int main(int argc, char **argv) {
  int i = 1;
  while (i < argc) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      printUsage();
      return 0;
    } else
      break;
  }

  // Auto-detect emulator mode by checking if emulator socket exists
  bool emulatorMode = false;
#ifdef APM_EMULATOR_MODE
  const char *home = ::getenv("HOME");
  if (home && *home) {
    std::string emulatorSocket =
        std::string(home) + "/APMEmulator/data/apm/apmd.socket";
    emulatorMode = apm::fs::pathExists(emulatorSocket);
  }
#endif

  // Set emulator mode so path getters work correctly
  apm::config::setEmulatorMode(emulatorMode);

  // Quiet CLI: write only errors by default, no stderr mirroring.
  apm::logger::setLogFile(
      apm::fs::joinPath(apm::config::getLogsDir(), "apm.log"));
  apm::logger::setDebugControlFile(apm::config::getDebugFlagFile());
  apm::logger::setMinLogLevel(apm::logger::Level::Error);
  apm::logger::enableStderr(false);
  apm::logger::info("apm: debug control file = " +
                    apm::config::getDebugFlagFile());

  // Log chosen transport mode (IPC-only).
  apm::ipc::detectTransportMode();
  apm::logger::info("apm: transport mode = ipc");

  if (i >= argc) {
    printUsage();
    return 1;
  }

  std::string cmd = argv[i++];

  std::string sessionToken;
  if (requiresAuthSession(cmd)) {
    if (!ensureAuthenticatedSession(sessionToken))
      return 1;
  }

  //
  // ============================
  //   IPC COMMAND DISPATCH
  // ============================
  //

  if (cmd == "ping")
    return cmdPing();
  if (cmd == "forgot-password")
    return cmdForgotPassword();
  if (cmd == "update")
    return cmdUpdate(sessionToken);

  if (cmd == "add-repo") {
    if (i >= argc) {
      std::cerr << "apm: 'add-repo' requires a .repo file path\n";
      return 1;
    }
    if (i + 1 != argc) {
      std::cerr << "apm: 'add-repo' accepts exactly one file path\n";
      return 1;
    }
    return cmdAddRepo(sessionToken, argv[i]);
  }

  if (cmd == "install") {
    if (i >= argc) {
      std::cerr << "apm: 'install' requires a package\n";
      return 1;
    }
    return cmdInstall(sessionToken, argv[i]);
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
    return cmdModuleInstall(sessionToken, argv[i]);
  }

  if (cmd == "module-list") {
    return cmdModuleList(sessionToken);
  }

  if (cmd == "module-enable") {
    if (i >= argc) {
      std::cerr << "apm: 'module-enable' requires a module name\n";
      return 1;
    }
    return cmdModuleEnable(sessionToken, argv[i]);
  }

  if (cmd == "module-disable") {
    if (i >= argc) {
      std::cerr << "apm: 'module-disable' requires a module name\n";
      return 1;
    }
    return cmdModuleDisable(sessionToken, argv[i]);
  }

  if (cmd == "module-remove") {
    if (i >= argc) {
      std::cerr << "apm: 'module-remove' requires a module name\n";
      return 1;
    }
    return cmdModuleRemove(sessionToken, argv[i]);
  }

  if (cmd == "remove") {
    if (i >= argc) {
      std::cerr << "apm: 'remove' requires a package\n";
      return 1;
    }
    return cmdRemove(sessionToken, argv[i]);
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
    return cmdApkInstall(sessionToken, apk, sys);
  }

  if (cmd == "apk-uninstall") {
    if (i >= argc) {
      std::cerr << "apm: 'apk-uninstall' requires a package name\n";
      return 1;
    }
    return cmdApkUninstall(sessionToken, argv[i]);
  }

  if (cmd == "upgrade") {
    int remaining = argc - i;
    return cmdUpgrade(sessionToken, remaining, argv + i);
  }

  if (cmd == "autoremove")
    return cmdAutoremove(sessionToken);

  if (cmd == "debuglogging") {
    if (i >= argc) {
      std::cerr << "apm: 'debuglogging' requires true or false\n";
      return 1;
    }
    if (i + 1 != argc) {
      std::cerr << "apm: 'debuglogging' accepts exactly one value\n";
      return 1;
    }
    return cmdDebugLogging(sessionToken, argv[i]);
  }

  if (cmd == "factory-reset")
    return cmdFactoryReset(sessionToken);
  if (cmd == "wipe-cache") {
    int remaining = argc - i;
    return cmdWipeCache(remaining, argv + i);
  }

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
  if (cmd == "log") {
    int remaining = argc - i;
    return cmdLog(remaining, argv + i);
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
  if (cmd == "sig-cache") {
    if (i >= argc) {
      std::cerr << "apm: 'sig-cache' requires an operation (show|clear)\n";
      return 1;
    }
    std::string op = argv[i];
    if (op == "show")
      return cmdSigCacheShow();
    else if (op == "clear")
      return cmdSigCacheClear();
    else {
      std::cerr << "apm: unknown sig-cache operation '" << op << "'\n";
      return 1;
    }
  }
  if (cmd == "help")
    return printUsage(), 0;

  std::cerr << "apm: unknown command '" << cmd << "'\n";
  printUsage();
  return 1;
}
