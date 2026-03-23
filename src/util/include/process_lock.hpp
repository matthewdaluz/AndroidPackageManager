/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: process_lock.hpp
 * Purpose: Process locking with stale lock detection for emulator mode.
 * Last Modified: 2026-03-15 11:56:16.542962490 -0400.
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

#pragma once

#include <string>

namespace apm::lock {

// RAII process lock manager
class ProcessLock {
public:
  explicit ProcessLock(const std::string &lockFilePath);
  ~ProcessLock();

  // Try to acquire the lock, removing stale locks if needed
  // Returns true if lock acquired, false otherwise
  // If errorMsg is provided, it will contain details on failure
  bool acquire(std::string *errorMsg = nullptr);

  // Release the lock
  void release();

  // Check if lock is currently held by this instance
  bool isHeld() const;

private:
  std::string lockPath_;
  bool held_;

  bool isProcessRunning(pid_t pid);
  bool readLockFile(pid_t &pid, std::string &timestamp, std::string &cmdline);
  bool writeLockFile();
  bool removeLockFile();
};

} // namespace apm::lock
