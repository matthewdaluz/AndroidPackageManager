/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: process_lock.cpp
 * Purpose: Process locking with stale lock detection for emulator mode.
 * Last Modified: November 29th, 2025.
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

#include "process_lock.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <signal.h>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>

namespace apm::lock {

ProcessLock::ProcessLock(const std::string &lockFilePath)
    : lockPath_(lockFilePath), held_(false) {}

ProcessLock::~ProcessLock() { release(); }

bool ProcessLock::isProcessRunning(pid_t pid) {
  if (pid <= 0)
    return false;

  // Try to send signal 0 (does not actually send a signal, just checks if
  // process exists)
  if (::kill(pid, 0) == 0) {
    return true; // Process exists
  }

  // Check errno to distinguish between "process doesn't exist" and other errors
  if (errno == ESRCH) {
    return false; // Process doesn't exist
  }

  // If we get EPERM, process exists but we don't have permission
  if (errno == EPERM) {
    return true;
  }

  // For other errors, assume process might exist
  return true;
}

bool ProcessLock::readLockFile(pid_t &pid, std::string &timestamp,
                               std::string &cmdline) {
  std::ifstream f(lockPath_);
  if (!f.is_open())
    return false;

  std::string line;
  if (!std::getline(f, line))
    return false;

  std::istringstream ss(line);
  if (!(ss >> pid))
    return false;

  if (!std::getline(f, timestamp))
    return false;

  // Rest of file is command line (may be multiple lines)
  std::ostringstream cmdlineStream;
  while (std::getline(f, line)) {
    if (!cmdlineStream.str().empty())
      cmdlineStream << "\n";
    cmdlineStream << line;
  }
  cmdline = cmdlineStream.str();

  return true;
}

bool ProcessLock::writeLockFile() {
  std::ofstream f(lockPath_, std::ios::trunc);
  if (!f.is_open())
    return false;

  pid_t pid = ::getpid();
  auto now = std::chrono::system_clock::now();
  auto timestamp = std::chrono::system_clock::to_time_t(now);

  // Write PID on first line
  f << pid << "\n";

  // Write timestamp on second line
  f << timestamp << "\n";

  // Write command line on remaining lines
  // Try to read /proc/self/cmdline
  std::ifstream cmdlineFile("/proc/self/cmdline");
  if (cmdlineFile.is_open()) {
    std::string cmdline;
    std::getline(cmdlineFile, cmdline, '\0'); // Read until null terminator
    // Replace null separators with spaces
    for (char &c : cmdline) {
      if (c == '\0')
        c = ' ';
    }
    f << cmdline << "\n";
  } else {
    f << "apmd --emulator\n";
  }

  f.close();
  return true;
}

bool ProcessLock::removeLockFile() {
  return ::unlink(lockPath_.c_str()) == 0 || errno == ENOENT;
}

bool ProcessLock::acquire(std::string *errorMsg) {
  if (held_)
    return true; // Already held

  // Check if lock file exists
  pid_t existingPid = 0;
  std::string existingTimestamp, existingCmdline;

  if (readLockFile(existingPid, existingTimestamp, existingCmdline)) {
    // Lock file exists, check if process is running
    if (isProcessRunning(existingPid)) {
      if (errorMsg) {
        *errorMsg = "Another apmd instance is already running (PID " +
                    std::to_string(existingPid) + ")";
      }
      return false;
    }

    // Stale lock - remove it
    if (!removeLockFile()) {
      if (errorMsg) {
        *errorMsg = "Failed to remove stale lock file: " +
                    std::string(std::strerror(errno));
      }
      return false;
    }
  }

  // Create new lock file
  if (!writeLockFile()) {
    if (errorMsg) {
      *errorMsg =
          "Failed to create lock file: " + std::string(std::strerror(errno));
    }
    return false;
  }

  held_ = true;
  return true;
}

void ProcessLock::release() {
  if (!held_)
    return;

  removeLockFile();
  held_ = false;
}

bool ProcessLock::isHeld() const { return held_; }

} // namespace apm::lock
