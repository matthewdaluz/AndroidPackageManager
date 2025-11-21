/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: tar_extractor.cpp
 * Purpose: Implement tar extraction by shelling out to the system tar binary.
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

#include "tar_extractor.hpp"
#include "fs.hpp"
#include "logger.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace apm::tar {

// Extract a tarball by forking the system `tar` binary. This keeps the build
// lightweight across Android and Linux hosts.
bool extractTar(const std::string &tarPath, const std::string &destDir,
                std::string *errorMsg) {
  if (tarPath.empty()) {
    if (errorMsg)
      *errorMsg = "Tar path is empty";
    apm::logger::error("extractTar: tar path is empty");
    return false;
  }

  // Tar file must exist
  if (!apm::fs::pathExists(tarPath)) {
    if (errorMsg)
      *errorMsg = "Tar file does not exist: " + tarPath;
    apm::logger::error("extractTar: tar file not found: " + tarPath);
    return false;
  }

  // Ensure destination directory exists
  if (!apm::fs::createDirs(destDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create destination directory: " + destDir;
    apm::logger::error("extractTar: cannot create dest dir: " + destDir);
    return false;
  }

  apm::logger::debug("extractTar: extracting " + tarPath + " -> " + destDir);

  pid_t pid = ::fork();
  if (pid < 0) {
    if (errorMsg)
      *errorMsg = "fork() failed in extractTar";
    apm::logger::error("extractTar: fork() failed");
    return false;
  }

  if (pid == 0) {
    // Child process: exec tar
    //
    // Using execlp("tar") will search PATH (on Android this typically
    // resolves to toybox tar or busybox tar). On Linux Mint it'll be /bin/tar.
    //
    // Command: tar -xf <tarPath> -C <destDir>
    ::execlp("tar", "tar", "-xf", tarPath.c_str(), "-C", destDir.c_str(),
             (char *)nullptr);

    // If we reach here, execlp failed.
    _exit(127);
  }

  // Parent: wait for child
  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    if (errorMsg)
      *errorMsg = "waitpid() failed in extractTar";
    apm::logger::error("extractTar: waitpid() failed");
    return false;
  }

  if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
    if (code == 0) {
      apm::logger::debug("extractTar: tar exited with code 0");
      return true;
    } else {
      if (errorMsg) {
        *errorMsg = "tar exited with non-zero status: " + std::to_string(code);
      }
      apm::logger::error("extractTar: tar exited with status " +
                         std::to_string(code));
      return false;
    }
  }

  if (errorMsg) {
    *errorMsg = "tar process terminated abnormally";
  }
  apm::logger::error("extractTar: tar process terminated abnormally");
  return false;
}

} // namespace apm::tar
