/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: security.cpp
 * Purpose: Implement shared security helpers for session persistence and time
 * handling.
 * Last Modified: 2026-03-15 11:56:16.537911560 -0400.
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

#include "security.hpp"

#include "config.hpp"
#include "fs.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <grp.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

namespace apm::security {

namespace {

static inline void trim(std::string &s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
            return !std::isspace(ch);
          }));
  s.erase(std::find_if(s.rbegin(), s.rend(),
                       [](unsigned char ch) { return !std::isspace(ch); })
              .base(),
          s.end());
}

static bool lockFdWithFallback(int fd, int operation, std::string *errorMsg) {
  if (::flock(fd, operation) == 0)
    return true;

  if (operation == LOCK_SH && errno == EINVAL) {
    if (::flock(fd, LOCK_EX) == 0)
      return true;
  }

  if (errorMsg)
    *errorMsg = "flock failed: " + std::string(std::strerror(errno));
  return false;
}

static bool readAllFromFd(int fd, std::string &out, std::string *errorMsg) {
  out.clear();
  char buf[512];
  while (true) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errorMsg)
        *errorMsg = "read() failed: " + std::string(std::strerror(errno));
      return false;
    }
    if (n == 0)
      break;
    out.append(buf, static_cast<std::size_t>(n));
  }
  return true;
}

static bool writeAllToFd(int fd, const std::string &data,
                         std::string *errorMsg) {
  const char *ptr = data.data();
  std::size_t remaining = data.size();
  while (remaining > 0) {
    ssize_t n = ::write(fd, ptr, remaining);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errorMsg)
        *errorMsg = "write() failed: " + std::string(std::strerror(errno));
      return false;
    }
    remaining -= static_cast<std::size_t>(n);
    ptr += n;
  }
  return true;
}

static bool lookupShellGroup(gid_t &gidOut) {
  struct group *shellGroup = ::getgrnam("shell");
  if (!shellGroup)
    return false;
  gidOut = shellGroup->gr_gid;
  return true;
}

} // namespace

bool ensureSecurityDir(std::string *errorMsg) {
  const std::string secDir = apm::config::getSecurityDir();
  if (!apm::fs::createDirs(secDir, 0750)) {
    if (errorMsg)
      *errorMsg = "Failed to create security directory at " + secDir;
    return false;
  }

  gid_t shellGid = 0;
  if (lookupShellGroup(shellGid)) {
    ::chown(secDir.c_str(), 0, shellGid);
  }
  ::chmod(secDir.c_str(), 0750);
  return true;
}

bool validatePackageName(const std::string &name, std::string *errorMsg) {
  if (name.empty()) {
    if (errorMsg)
      *errorMsg = "Package name is empty";
    return false;
  }

  if (name.find('/') != std::string::npos ||
      name.find('\\') != std::string::npos) {
    if (errorMsg)
      *errorMsg =
          "Invalid package name '" + name + "': contains path separators";
    return false;
  }

  if (name.find("..") != std::string::npos) {
    if (errorMsg)
      *errorMsg = "Invalid package name '" + name + "': contains '..'";
    return false;
  }

  const unsigned char first = static_cast<unsigned char>(name.front());
  if (!std::islower(first) && !std::isdigit(first)) {
    if (errorMsg)
      *errorMsg =
          "Invalid package name '" + name + "': must start with [a-z0-9]";
    return false;
  }

  for (char c : name) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::islower(uc) || std::isdigit(uc) || c == '+' || c == '-' ||
        c == '.') {
      continue;
    }
    if (errorMsg)
      *errorMsg = "Invalid package name '" + name +
                  "': contains unsupported characters";
    return false;
  }

  return true;
}

std::uint64_t currentUnixSeconds() {
  using namespace std::chrono;
  return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string serializeSession(const SessionState &state) {
  std::ostringstream out;
  out << "token:" << state.token << "\n";
  out << "expires:" << state.expiresAt << "\n";
  out << "hmac:" << state.hmac << "\n";
  return out.str();
}

std::string buildSessionPayload(const SessionState &state) {
  return state.token + "|" + std::to_string(state.expiresAt);
}

bool parseSession(const std::string &raw, SessionState &out,
                  std::string *errorMsg) {
  out = SessionState{};

  std::istringstream in(raw);
  std::string line;

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    if (line.empty())
      continue;
    auto pos = line.find(':');
    if (pos == std::string::npos) {
      if (errorMsg)
        *errorMsg = "Malformed session line: " + line;
      return false;
    }
    std::string key = line.substr(0, pos);
    std::string value = line.substr(pos + 1);
    trim(key);
    trim(value);
    if (key == "token")
      out.token = value;
    else if (key == "expires") {
      errno = 0;
      char *end = nullptr;
      unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
      if (errno != 0 || end == value.c_str()) {
        if (errorMsg)
          *errorMsg = "Invalid session expiry value";
        return false;
      }
      out.expiresAt = static_cast<std::uint64_t>(parsed);
    } else if (key == "hmac") {
      out.hmac = value;
    }
  }

  if (out.token.empty() || out.expiresAt == 0 || out.hmac.empty()) {
    if (errorMsg)
      *errorMsg = "Session file missing required fields";
    return false;
  }
  return true;
}

bool loadSession(SessionState &out, std::string *errorMsg) {
  const std::string sessionFile = apm::config::getSessionFile();
  int fd = ::open(sessionFile.c_str(), O_RDONLY);
  if (fd < 0) {
    if (errorMsg)
      *errorMsg = "Unable to read session file";
    return false;
  }

  std::string raw;
  bool ok = false;

  do {
    if (!lockFdWithFallback(fd, LOCK_SH, errorMsg))
      break;
    if (!readAllFromFd(fd, raw, errorMsg))
      break;
    ok = true;
  } while (false);

  ::close(fd);
  if (!ok)
    return false;
  return parseSession(raw, out, errorMsg);
}

bool writeSession(const SessionState &state, std::string *errorMsg) {
  if (!ensureSecurityDir(errorMsg))
    return false;

  const std::string sessionFile = apm::config::getSessionFile();
  int fd = ::open(sessionFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0660);
  if (fd < 0) {
    if (errorMsg)
      *errorMsg = "Failed to open session file for writing";
    return false;
  }

  bool ok = false;
  const std::string serialized = serializeSession(state);

  do {
    if (!lockFdWithFallback(fd, LOCK_EX, errorMsg))
      break;
    if (!writeAllToFd(fd, serialized, errorMsg))
      break;
    ok = true;
  } while (false);

  ::close(fd);
  if (ok) {
    gid_t shellGid = 0;
    if (lookupShellGroup(shellGid)) {
      ::chown(sessionFile.c_str(), 0, shellGid);
    }
    ::chmod(sessionFile.c_str(), 0660);
  }
  return ok;
}

bool isSessionExpired(const SessionState &state, std::uint64_t nowSeconds) {
  return nowSeconds >= state.expiresAt;
}

} // namespace apm::security
