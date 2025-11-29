/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: security.cpp
 * Purpose: Implement shared security helpers for session persistence and time
 * handling. Last Modified: November 25th, 2025. - 11:35 AM Eastern Time.
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
#include <sstream>
#include <string>
#include <sys/stat.h>

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

} // namespace

bool ensureSecurityDir(std::string *errorMsg) {
  const std::string secDir = apm::config::getSecurityDir();
  if (apm::fs::createDirs(secDir, 0700))
    return true;
  if (errorMsg)
    *errorMsg = "Failed to create security directory at " + secDir;
  return false;
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
  std::string raw;
  if (!apm::fs::readFile(apm::config::getSessionFile(), raw)) {
    if (errorMsg)
      *errorMsg = "Unable to read session file";
    return false;
  }
  return parseSession(raw, out, errorMsg);
}

bool writeSession(const SessionState &state, std::string *errorMsg) {
  if (!ensureSecurityDir(errorMsg))
    return false;

  const std::string sessionFile = apm::config::getSessionFile();
  const std::string serialized = serializeSession(state);
  if (!apm::fs::writeFile(sessionFile, serialized, true)) {
    if (errorMsg)
      *errorMsg = "Failed to write session file";
    return false;
  }

  ::chmod(sessionFile.c_str(), 0600);
  return true;
}

bool isSessionExpired(const SessionState &state, std::uint64_t nowSeconds) {
  return nowSeconds >= state.expiresAt;
}

} // namespace apm::security
