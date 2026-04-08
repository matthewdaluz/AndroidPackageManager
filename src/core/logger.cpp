/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: logger.cpp
 * Purpose: Implement the file-backed logger and severity helpers.
 * Last Modified: 2026-03-18 10:55:01.568628991 -0400.
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

#include "logger.hpp"
#include "config.hpp"
#include "fs.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <grp.h>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace apm::logger {

// ---------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------

static std::string g_logFile = "/data/apm/logs/apm.log";
static Level g_minLevel = Level::Info;
static std::string g_debugControlFile;
static bool g_debugControlFileResolved = false;
static bool g_debugEnabled = false;
static bool g_debugFileKnown = false;
static std::time_t g_debugMTime = 0;
static off_t g_debugFileSize = -1;
static bool g_stderrEnabled = true;
static std::mutex g_mutex;

// ---------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------

static const char *levelToString(Level level) {
  switch (level) {
  case Level::Debug:
    return "DEBUG";
  case Level::Info:
    return "INFO";
  case Level::Warn:
    return "WARN";
  case Level::Error:
    return "ERROR";
  default:
    return "UNKNOWN";
  }
}

static std::string currentTimestamp() {
  std::time_t now = std::time(nullptr);
  std::tm tm{};
  // localtime is not thread-safe, so guard with mutex or use localtime_r
  // We already hold g_mutex when calling this.
  std::tm *ptm = std::localtime(&now);
  if (ptm) {
    tm = *ptm;
  }

  std::ostringstream ss;
  ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
  return ss.str();
}

static std::string trimAsciiWhitespace(std::string value) {
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' ||
          value.back() == '\t')) {
    value.pop_back();
  }

  std::size_t start = 0;
  while (start < value.size() &&
         (value[start] == ' ' || value[start] == '\t' || value[start] == '\n' ||
          value[start] == '\r')) {
    ++start;
  }
  if (start > 0)
    value.erase(0, start);

  return value;
}

static void ensureDebugControlPathResolvedLocked() {
  if (g_debugControlFileResolved && !g_debugControlFile.empty())
    return;

  g_debugControlFile = apm::config::getDebugFlagFile();
  g_debugControlFileResolved = true;
}

static void ensureDebugControlFileExistsLocked() {
  ensureDebugControlPathResolvedLocked();
  if (g_debugControlFile.empty())
    return;

  if (apm::fs::pathExists(g_debugControlFile))
    return;

  const auto slash = g_debugControlFile.find_last_of('/');
  if (slash != std::string::npos) {
    const std::string parent = g_debugControlFile.substr(0, slash);
    if (!parent.empty()) {
      apm::fs::createDirs(parent);
    }
  }

  std::ofstream out(g_debugControlFile, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!out.is_open())
    return;

  out << "false\n";
  out.flush();
}

static bool parseDebugEnabledString(std::string value) {
  value = trimAsciiWhitespace(std::move(value));
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value == "true";
}

static void refreshDebugModeFromFileLocked() {
  ensureDebugControlPathResolvedLocked();
  ensureDebugControlFileExistsLocked();

  if (g_debugControlFile.empty()) {
    g_debugEnabled = false;
    g_debugFileKnown = true;
    return;
  }

  struct stat st {};
  if (::stat(g_debugControlFile.c_str(), &st) != 0) {
    g_debugEnabled = false;
    g_debugFileKnown = true;
    g_debugMTime = 0;
    g_debugFileSize = -1;
    return;
  }

  if (g_debugFileKnown && st.st_mtime == g_debugMTime && st.st_size == g_debugFileSize) {
    return;
  }

  std::ifstream in(g_debugControlFile, std::ios::in | std::ios::binary);
  std::string content;
  if (in.is_open()) {
    std::getline(in, content);
  }

  g_debugEnabled = parseDebugEnabledString(content);
  g_debugFileKnown = true;
  g_debugMTime = st.st_mtime;
  g_debugFileSize = st.st_size;
}

static Level effectiveMinLevelLocked() {
  if (g_debugEnabled) {
    return Level::Debug;
  }

  if (g_minLevel == Level::Debug) {
    return Level::Info;
  }

  return g_minLevel;
}

static void ensureLogDirExists(const std::string &path) {
  auto pos = path.find_last_of('/');
  if (pos == std::string::npos) {
    return;
  }

  std::string parent = path.substr(0, pos);
  if (!parent.empty()) {
    apm::fs::createDirs(parent);
  }
}

static bool hasPrefix(const std::string &value, const std::string &prefix) {
  return value.size() >= prefix.size() &&
         value.compare(0, prefix.size(), prefix) == 0;
}

static std::string baseName(const std::string &path) {
  if (path.empty())
    return {};

  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return path;
  if (slash + 1 >= path.size())
    return {};
  return path.substr(slash + 1);
}

static std::string mirrorLogFilePath(const std::string &path) {
  const std::string name = baseName(path);
  if (name.empty())
    return {};

  const std::string mirrorDir = apm::config::getShellLogsDir();
  if (mirrorDir.empty())
    return {};

  return apm::fs::joinPath(mirrorDir, name);
}

static void ensureShellReadableLogFile(const std::string &path) {
  if (apm::config::isEmulatorMode()) {
    return;
  }

  if (!hasPrefix(path, "/data/local/tmp/apm/")) {
    return;
  }

  struct group *shellGroup = ::getgrnam("shell");
  if (shellGroup) {
    ::chown(path.c_str(), 0, shellGroup->gr_gid);
  }
  ::chmod(path.c_str(), 0664);
}

static void updateDebugStateFromCurrentFileLocked(bool enabled) {
  g_debugEnabled = enabled;
  g_debugFileKnown = true;

  struct stat st {};
  if (!g_debugControlFile.empty() && ::stat(g_debugControlFile.c_str(), &st) == 0) {
    g_debugMTime = st.st_mtime;
    g_debugFileSize = st.st_size;
  } else {
    g_debugMTime = 0;
    g_debugFileSize = enabled ? 5 : 6;
  }
}

// ---------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------

// Override the default log file path (useful for CLI builds/tests).
void setLogFile(const std::string &path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!path.empty()) {
    g_logFile = path;
  }
}

void setDebugControlFile(const std::string &path) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!path.empty()) {
    g_debugControlFile = path;
    g_debugControlFileResolved = true;
  } else {
    g_debugControlFile.clear();
    g_debugControlFileResolved = false;
  }

  g_debugFileKnown = false;
  g_debugMTime = 0;
  g_debugFileSize = -1;
  refreshDebugModeFromFileLocked();
}

bool setDebugEnabled(bool enabled, std::string *errorMsg) {
  std::lock_guard<std::mutex> lock(g_mutex);
  ensureDebugControlPathResolvedLocked();

  if (g_debugControlFile.empty()) {
    if (errorMsg)
      *errorMsg = "Debug control file is not configured";
    return false;
  }

  const auto slash = g_debugControlFile.find_last_of('/');
  if (slash != std::string::npos) {
    const std::string parent = g_debugControlFile.substr(0, slash);
    if (!parent.empty() && !apm::fs::createDirs(parent)) {
      if (errorMsg)
        *errorMsg = "Failed to create debug control directory: " + parent;
      return false;
    }
  }

  const std::string content = enabled ? "true\n" : "false\n";
  std::ofstream out(g_debugControlFile,
                    std::ios::out | std::ios::trunc | std::ios::binary);
  if (!out.is_open()) {
    if (errorMsg)
      *errorMsg = "Failed to open debug control file: " + g_debugControlFile;
    return false;
  }

  out.write(content.data(), static_cast<std::streamsize>(content.size()));
  out.flush();
  if (!out.good()) {
    if (errorMsg)
      *errorMsg = "Failed to write debug control file: " + g_debugControlFile;
    return false;
  }

  updateDebugStateFromCurrentFileLocked(enabled);
  return true;
}

// Control which severity and above should be persisted.
void setMinLogLevel(Level level) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_minLevel = level;
}

bool isDebugEnabled() {
  std::lock_guard<std::mutex> lock(g_mutex);
  refreshDebugModeFromFileLocked();
  return g_debugEnabled;
}

// Mirror logs to stderr in addition to the log file when desired.
void enableStderr(bool enable) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_stderrEnabled = enable;
}

// Core sink that formats a timestamped line, writes it to disk, and optionally
// mirrors it to stderr. All helpers funnel through here.
void log(Level level, const std::string &message) {
  std::lock_guard<std::mutex> lock(g_mutex);

  refreshDebugModeFromFileLocked();
  const Level effectiveMin = effectiveMinLevelLocked();

  if (static_cast<int>(level) < static_cast<int>(effectiveMin)) {
    return;
  }

  const std::string ts = currentTimestamp();
  const char *lvlStr = levelToString(level);

  std::ostringstream line;
  line << "[" << ts << "] "
       << "[" << lvlStr << "] " << message << "\n";

  const std::string out = line.str();

  ensureLogDirExists(g_logFile);
  {
    std::ofstream file(g_logFile,
                       std::ios::out | std::ios::app | std::ios::binary);
    if (file.is_open()) {
      file.write(out.data(), static_cast<std::streamsize>(out.size()));
      file.flush();
      ensureShellReadableLogFile(g_logFile);
    }
  }

  const std::string mirrorFile = mirrorLogFilePath(g_logFile);
  if (!mirrorFile.empty() && mirrorFile != g_logFile) {
    ensureLogDirExists(mirrorFile);
    std::ofstream file(mirrorFile,
                       std::ios::out | std::ios::app | std::ios::binary);
    if (file.is_open()) {
      file.write(out.data(), static_cast<std::streamsize>(out.size()));
      file.flush();
      ensureShellReadableLogFile(mirrorFile);
    }
  }

  // Optionally mirror to stderr
  if (g_stderrEnabled) {
    std::cerr << out;
  }
}

void debug(const std::string &message) { log(Level::Debug, message); }

void info(const std::string &message) { log(Level::Info, message); }

void warn(const std::string &message) { log(Level::Warn, message); }

void error(const std::string &message) { log(Level::Error, message); }

} // namespace apm::logger
