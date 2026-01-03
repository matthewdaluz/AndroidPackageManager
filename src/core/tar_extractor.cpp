/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: tar_extractor.cpp
 * Purpose: Implement tar extraction by shelling out to the system tar binary.
 * Last Modified: January 3rd, 2026. - 8:35 AM Eastern Time.
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

#include <cerrno>
#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

struct TarEntryInfo {
  char type = '\0';
  std::string linkTarget;
};

static std::string stripLeadingDotSlash(const std::string &path) {
  size_t start = 0;
  while (start + 1 < path.size() && path.compare(start, 2, "./") == 0) {
    start += 2;
  }
  return path.substr(start);
}

static bool appendSegments(const std::string &path, bool allowParent,
                           std::vector<std::string> &parts,
                           std::string *errorMsg,
                           const std::string &context) {
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) {
      end = path.size();
    }
    const bool isLast = end == path.size();
    std::string segment = path.substr(start, end - start);

    if (segment.empty()) {
      if (!isLast) {
        if (errorMsg) {
          *errorMsg = "Unsafe tar path (empty segment): " + context;
        }
        return false;
      }
      break;
    }

    if (segment == ".") {
      // Skip current directory segments.
    } else if (segment == "..") {
      if (!allowParent || parts.empty()) {
        if (errorMsg) {
          *errorMsg = "Unsafe tar path (parent traversal): " + context;
        }
        return false;
      }
      parts.pop_back();
    } else {
      parts.push_back(segment);
    }

    if (isLast) {
      break;
    }
    start = end + 1;
  }

  return true;
}

static bool validateEntryPath(const std::string &rawPath,
                              std::string *errorMsg) {
  std::string path = stripLeadingDotSlash(rawPath);
  if (path.empty()) {
    return true;
  }

  if (path.front() == '/') {
    if (errorMsg) {
      *errorMsg = "Unsafe tar path (absolute): " + rawPath;
    }
    return false;
  }

  if (path.find('\\') != std::string::npos) {
    if (errorMsg) {
      *errorMsg = "Unsafe tar path (backslash): " + rawPath;
    }
    return false;
  }

  std::vector<std::string> parts;
  if (!appendSegments(path, false, parts, errorMsg, rawPath)) {
    return false;
  }

  return true;
}

static bool validateLinkTarget(const std::string &entryName,
                               const std::string &rawTarget,
                               std::string *errorMsg) {
  if (rawTarget.empty()) {
    if (errorMsg) {
      *errorMsg = "Unsafe tar link (empty target): " + entryName;
    }
    return false;
  }

  if (rawTarget.front() == '/') {
    if (errorMsg) {
      *errorMsg = "Unsafe tar link (absolute target): " + entryName;
    }
    return false;
  }

  if (rawTarget.find('\\') != std::string::npos) {
    if (errorMsg) {
      *errorMsg = "Unsafe tar link (backslash in target): " + entryName;
    }
    return false;
  }

  std::string name = stripLeadingDotSlash(entryName);
  while (!name.empty() && name.back() == '/') {
    name.pop_back();
  }

  std::string baseDir;
  size_t slash = name.find_last_of('/');
  if (slash != std::string::npos) {
    baseDir = name.substr(0, slash);
  }

  std::vector<std::string> parts;
  if (!appendSegments(baseDir, false, parts, errorMsg, entryName)) {
    return false;
  }

  if (!appendSegments(rawTarget, true, parts, errorMsg, entryName)) {
    return false;
  }

  return true;
}

static bool runTarForOutput(const std::vector<std::string> &args,
                            std::string &output, std::string *errorMsg) {
  int pipefd[2];
  if (::pipe(pipefd) != 0) {
    if (errorMsg) {
      *errorMsg = "pipe() failed: " + std::string(std::strerror(errno));
    }
    return false;
  }

  pid_t pid = ::fork();
  if (pid < 0) {
    if (errorMsg) {
      *errorMsg = "fork() failed: " + std::string(std::strerror(errno));
    }
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    return false;
  }

  if (pid == 0) {
    ::close(pipefd[0]);
    if (::dup2(pipefd[1], STDOUT_FILENO) < 0) {
      _exit(127);
    }
    ::close(pipefd[1]);

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &arg : args) {
      argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    _exit(127);
  }

  ::close(pipefd[1]);
  output.clear();
  char buffer[4096];
  ssize_t bytes = 0;
  while ((bytes = ::read(pipefd[0], buffer, sizeof(buffer))) > 0) {
    output.append(buffer, static_cast<size_t>(bytes));
  }
  ::close(pipefd[0]);

  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    if (errorMsg) {
      *errorMsg = "waitpid() failed: " + std::string(std::strerror(errno));
    }
    return false;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (errorMsg) {
      if (WIFEXITED(status)) {
        *errorMsg =
            "tar exited with status " + std::to_string(WEXITSTATUS(status));
      } else {
        *errorMsg = "tar terminated abnormally";
      }
    }
    return false;
  }

  return true;
}

static bool listTarEntries(const std::string &tarPath,
                           std::vector<std::string> &entries,
                           std::string *errorMsg) {
  std::string output;
  if (!runTarForOutput({"tar", "-tf", tarPath}, output, errorMsg)) {
    return false;
  }

  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    entries.push_back(line);
  }

  return true;
}

static bool listTarEntryInfo(const std::string &tarPath,
                             std::vector<TarEntryInfo> &entries,
                             std::string *errorMsg) {
  std::string output;
  if (!runTarForOutput({"tar", "-tvf", tarPath}, output, errorMsg)) {
    return false;
  }

  std::istringstream lines(output);
  std::string line;
  while (std::getline(lines, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }

    char typeChar = '\0';
    for (char ch : line) {
      if (!std::isspace(static_cast<unsigned char>(ch))) {
        typeChar = ch;
        break;
      }
    }

    if (typeChar == '\0') {
      continue;
    }

    TarEntryInfo info;
    info.type = typeChar;

    if (typeChar == 'l' || typeChar == 'h') {
      size_t arrow = line.find(" -> ");
      if (arrow != std::string::npos) {
        info.linkTarget = line.substr(arrow + 4);
      } else {
        size_t linkTo = line.find(" link to ");
        if (linkTo != std::string::npos) {
          info.linkTarget = line.substr(linkTo + 9);
        }
      }
    }

    entries.push_back(info);
  }

  return true;
}

static bool execTar(const std::vector<std::string> &args,
                    std::string *errorMsg) {
  pid_t pid = ::fork();
  if (pid < 0) {
    if (errorMsg) {
      *errorMsg = "fork() failed in extractTar";
    }
    apm::logger::error("extractTar: fork() failed");
    return false;
  }

  if (pid == 0) {
    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &arg : args) {
      argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execvp(argv[0], argv.data());
    _exit(127);
  }

  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    if (errorMsg) {
      *errorMsg = "waitpid() failed in extractTar";
    }
    apm::logger::error("extractTar: waitpid() failed");
    return false;
  }

  if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);
    if (code == 0) {
      apm::logger::debug("extractTar: tar exited with code 0");
      return true;
    }
    if (errorMsg) {
      *errorMsg = "tar exited with non-zero status: " + std::to_string(code);
    }
    apm::logger::error("extractTar: tar exited with status " +
                       std::to_string(code));
    return false;
  }

  if (errorMsg) {
    *errorMsg = "tar process terminated abnormally";
  }
  apm::logger::error("extractTar: tar process terminated abnormally");
  return false;
}

} // namespace

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

  std::vector<std::string> entries;
  if (!listTarEntries(tarPath, entries, errorMsg)) {
    if (errorMsg && errorMsg->empty()) {
      *errorMsg = "Failed to list tar entries";
    }
    apm::logger::error("extractTar: failed to list tar entries");
    return false;
  }

  std::vector<TarEntryInfo> entryInfo;
  if (!listTarEntryInfo(tarPath, entryInfo, errorMsg)) {
    if (errorMsg && errorMsg->empty()) {
      *errorMsg = "Failed to read tar entry metadata";
    }
    apm::logger::error("extractTar: failed to read tar entry metadata");
    return false;
  }

  if (entries.size() != entryInfo.size()) {
    if (errorMsg) {
      *errorMsg = "Tar entry list mismatch";
    }
    apm::logger::error("extractTar: tar entry list mismatch");
    return false;
  }

  for (size_t i = 0; i < entries.size(); ++i) {
    const std::string &entry = entries[i];
    if (!validateEntryPath(entry, errorMsg)) {
      apm::logger::error("extractTar: unsafe tar entry path: " + entry);
      return false;
    }

    const TarEntryInfo &info = entryInfo[i];
    if (info.type != '-' && info.type != 'd' && info.type != 'l' &&
        info.type != 'h') {
      if (errorMsg) {
        *errorMsg = "Unsafe tar entry type: " + entry;
      }
      apm::logger::error("extractTar: unsafe tar entry type detected");
      return false;
    }

    if (info.type == 'l' || info.type == 'h') {
      if (info.linkTarget.empty()) {
        if (errorMsg) {
          *errorMsg = "Tar link entry missing target: " + entry;
        }
        apm::logger::error("extractTar: link entry missing target");
        return false;
      }
      if (!validateLinkTarget(entry, info.linkTarget, errorMsg)) {
        apm::logger::error("extractTar: unsafe tar link target: " + entry);
        return false;
      }
    }
  }

  apm::logger::debug("extractTar: extracting " + tarPath + " -> " + destDir);

  std::vector<std::string> args = {"tar", "-xf", tarPath, "-C", destDir};
  return execTar(args, errorMsg);
}

} // namespace apm::tar
