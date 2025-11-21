/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: logger.cpp
 * Purpose: Implement the file-backed logger and severity helpers.
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

#include "logger.hpp"
#include "fs.hpp"

#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>

namespace apm::logger {

// ---------------------------------------------------------------------
// Internal state
// ---------------------------------------------------------------------

static std::string g_logFile = "/data/apm/logs/apm.log";
static Level g_minLevel = Level::Info;
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

static void ensureLogDirExists() {
  // Extract parent directory
  auto pos = g_logFile.find_last_of('/');
  if (pos == std::string::npos) {
    return;
  }

  std::string parent = g_logFile.substr(0, pos);
  if (!parent.empty()) {
    apm::fs::createDirs(parent);
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

// Control which severity and above should be persisted.
void setMinLogLevel(Level level) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_minLevel = level;
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

  if (static_cast<int>(level) < static_cast<int>(g_minLevel)) {
    return;
  }

  const std::string ts = currentTimestamp();
  const char *lvlStr = levelToString(level);

  std::ostringstream line;
  line << "[" << ts << "] "
       << "[" << lvlStr << "] " << message << "\n";

  const std::string out = line.str();

  // Write to log file
  ensureLogDirExists();
  {
    std::ofstream file(g_logFile,
                       std::ios::out | std::ios::app | std::ios::binary);
    if (file.is_open()) {
      file.write(out.data(), static_cast<std::streamsize>(out.size()));
      file.flush();
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
