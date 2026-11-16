/*
 * APM - Android Package Manager
 *
 * Skidworks Open-Engineering - Software Branch
 * Copyright (C) 2027 Skidworks Open-Engineering
 *
 * File: tar_extractor.cpp
 * Purpose: Implement tar extraction by shelling out to the system tar binary.
 * Last Modified: November 15th, 2026. - 09:14 AM EST.
 * Author: Matthew DaLuz - Skidworks Founder
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
#include "archive_decompressor.hpp"
#include "config.hpp"
#include "fs.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include <poll.h>
#include <signal.h>
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
                           std::string *errorMsg, const std::string &context) {
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

static bool pathHasPrefixBoundary(const std::string &path,
                                  const std::string &prefix) {
  if (prefix.empty() || path.size() < prefix.size()) {
    return false;
  }
  if (path.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  return path.size() == prefix.size() || prefix.back() == '/' ||
         path[prefix.size()] == '/';
}

static bool validateAllowedAbsoluteSymlinkTarget(
    const std::string &entryName, const std::string &rawTarget,
    const apm::tar::ExtractOptions &options, std::string *errorMsg) {
  for (const auto &prefix : options.allowedAbsoluteSymlinkTargetPrefixes) {
    if (!pathHasPrefixBoundary(rawTarget, prefix)) {
      continue;
    }

    std::string suffix = rawTarget.substr(prefix.size());
    while (!suffix.empty() && suffix.front() == '/') {
      suffix.erase(0, 1);
    }

    std::vector<std::string> parts;
    if (!appendSegments(suffix, false, parts, errorMsg, entryName)) {
      return false;
    }
    return true;
  }

  if (errorMsg) {
    *errorMsg = "Unsafe tar link (absolute target): " + entryName;
  }
  return false;
}

static bool validateLinkTarget(const std::string &entryName,
                               const std::string &rawTarget, char linkType,
                               const apm::tar::ExtractOptions &options,
                               std::string *errorMsg) {
  if (rawTarget.empty()) {
    if (errorMsg) {
      *errorMsg = "Unsafe tar link (empty target): " + entryName;
    }
    return false;
  }

  if (rawTarget.find('\\') != std::string::npos) {
    if (errorMsg) {
      *errorMsg = "Unsafe tar link (backslash in target): " + entryName;
    }
    return false;
  }

  if (rawTarget.front() == '/') {
    if (linkType == 'h') {
      if (errorMsg) {
        *errorMsg = "Unsafe tar hardlink (absolute target): " + entryName;
      }
      return false;
    }
    return validateAllowedAbsoluteSymlinkTarget(entryName, rawTarget, options,
                                                errorMsg);
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
                            std::string &output, std::string *errorMsg,
                            bool captureOutput = true) {
  int stdoutPipe[2], stderrPipe[2];
  if (::pipe(stdoutPipe) != 0) {
    if (errorMsg)
      *errorMsg = "pipe() failed: " + std::string(std::strerror(errno));
    return false;
  }
  if (::pipe(stderrPipe) != 0) {
    const auto saved = errno;
    ::close(stdoutPipe[0]);
    ::close(stdoutPipe[1]);
    if (errorMsg)
      *errorMsg = "pipe() failed: " + std::string(std::strerror(saved));
    return false;
  }
  std::vector<char *> argv;
  for (const auto &arg : args)
    argv.push_back(const_cast<char *>(arg.c_str()));
  argv.push_back(nullptr);
  const pid_t pid = ::fork();
  if (pid < 0) {
    const auto saved = errno;
    for (int fd : {stdoutPipe[0], stdoutPipe[1], stderrPipe[0], stderrPipe[1]})
      ::close(fd);
    if (errorMsg)
      *errorMsg = "fork() failed: " + std::string(std::strerror(saved));
    return false;
  }
  if (pid == 0) {
    ::close(stdoutPipe[0]);
    ::close(stderrPipe[0]);
    if (::dup2(stdoutPipe[1], STDOUT_FILENO) < 0 ||
        ::dup2(stderrPipe[1], STDERR_FILENO) < 0)
      _exit(127);
    ::close(stdoutPipe[1]);
    ::close(stderrPipe[1]);
    ::execvp(argv[0], argv.data());
    const char message[] = "cannot execute tar; check that tar is installed "
                           "and accessible in PATH\n";
    const auto ignored = ::write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)ignored;
    _exit(127);
  }
  ::close(stdoutPipe[1]);
  ::close(stderrPipe[1]);
  output.clear();
  std::string diagnostic;
  pollfd pipes[2] = {{stdoutPipe[0], POLLIN, 0}, {stderrPipe[0], POLLIN, 0}};
  bool readError = false;
  while (pipes[0].fd >= 0 || pipes[1].fd >= 0) {
    if (::poll(pipes, 2, -1) < 0) {
      if (errno == EINTR)
        continue;
      readError = true;
      break;
    }
    for (int i = 0; i < 2; ++i) {
      if (pipes[i].fd < 0 || !pipes[i].revents)
        continue;
      char buffer[4096];
      const auto size = ::read(pipes[i].fd, buffer, sizeof(buffer));
      if (size > 0) {
        if (i == 0 && captureOutput)
          output.append(buffer, static_cast<std::size_t>(size));
        else if (i == 1 && diagnostic.size() < 16384)
          diagnostic.append(buffer, std::min(static_cast<std::size_t>(size),
                                             16384 - diagnostic.size()));
      } else if (size == 0) {
        ::close(pipes[i].fd);
        pipes[i].fd = -1;
      } else if (errno != EINTR) {
        readError = true;
        break;
      }
    }
    if (readError)
      break;
  }
  for (auto &pipe : pipes)
    if (pipe.fd >= 0)
      ::close(pipe.fd);
  if (readError)
    ::kill(pid, SIGTERM);
  int status = 0;
  pid_t waited;
  do {
    waited = ::waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited < 0 || readError) {
    if (errorMsg)
      *errorMsg = "Failed to read or wait for tar process";
    return false;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    if (errorMsg) {
      while (!diagnostic.empty() &&
             (diagnostic.back() == '\n' || diagnostic.back() == '\r'))
        diagnostic.pop_back();
      *errorMsg = "tar " + args[1] +
                  (WIFEXITED(status) ? " exited with status " +
                                           std::to_string(WEXITSTATUS(status))
                                     : " terminated abnormally");
      if (!diagnostic.empty())
        *errorMsg += ": " + diagnostic;
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
  std::string ignored;
  return runTarForOutput(args, ignored, errorMsg, false);
}

} // namespace

namespace apm::tar {

// Decode compression internally, then use system tar for archive validation
// and extraction on Android and Linux hosts.
bool extractTar(const std::string &tarPath, const std::string &destDir,
                std::string *errorMsg) {
  return extractTar(tarPath, destDir, ExtractOptions{}, errorMsg);
}

bool extractTar(const std::string &tarPath, const std::string &destDir,
                const ExtractOptions &options, std::string *errorMsg) {
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

  apm::archive::PreparedTar prepared;
  if (!apm::archive::prepareTar(tarPath, apm::config::getCacheDir(),
                                options.maxDecompressedBytes, prepared,
                                errorMsg)) {
    return false;
  }

  std::vector<std::string> entries;
  if (!listTarEntries(prepared.path, entries, errorMsg)) {
    if (errorMsg && errorMsg->empty()) {
      *errorMsg = "Failed to list tar entries";
    }
    apm::logger::error("extractTar: failed to list tar entries");
    return false;
  }

  std::vector<TarEntryInfo> entryInfo;
  if (!listTarEntryInfo(prepared.path, entryInfo, errorMsg)) {
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
    const TarEntryInfo &info = entryInfo[i];
    std::string entry = entries[i];

    // Some tar implementations include symbolic-link suffixes in -tf output:
    //   path/to/link -> ../target
    // Strip these metadata suffixes so path validation only sees the entry
    // path.
    if (info.type == 'l' || info.type == 'h') {
      size_t arrow = entry.find(" -> ");
      if (arrow != std::string::npos) {
        entry = entry.substr(0, arrow);
      } else {
        size_t linkTo = entry.find(" link to ");
        if (linkTo != std::string::npos) {
          entry = entry.substr(0, linkTo);
        }
      }
    }

    if (!validateEntryPath(entry, errorMsg)) {
      apm::logger::error("extractTar: unsafe tar entry path: " + entry);
      return false;
    }

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
      if (!validateLinkTarget(entry, info.linkTarget, info.type, options,
                              errorMsg)) {
        apm::logger::error("extractTar: unsafe tar link target: " + entry);
        return false;
      }
    }
  }

  apm::logger::debug("extractTar: extracting " + tarPath + " -> " + destDir);

  std::vector<std::string> args = {"tar", "-xf", prepared.path, "-C", destDir};
  return execTar(args, errorMsg);
}

} // namespace apm::tar
