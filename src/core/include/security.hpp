/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: security.hpp
 * Purpose: Declare shared security helpers for session storage and metadata handling.
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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace apm::security {

inline constexpr std::size_t SECURITY_QUESTION_COUNT = 3;

struct SessionState {
  std::string token;
  std::uint64_t expiresAt = 0;
  std::string hmac;
};

bool ensureSecurityDir(std::string *errorMsg = nullptr);
bool validatePackageName(const std::string &name,
                         std::string *errorMsg = nullptr);
std::string serializeSession(const SessionState &state);
bool parseSession(const std::string &raw, SessionState &out,
                  std::string *errorMsg = nullptr);
bool loadSession(SessionState &out, std::string *errorMsg = nullptr);
bool writeSession(const SessionState &state, std::string *errorMsg = nullptr);
bool isSessionExpired(const SessionState &state, std::uint64_t nowSeconds);
std::uint64_t currentUnixSeconds();
std::string buildSessionPayload(const SessionState &state);

} // namespace apm::security
