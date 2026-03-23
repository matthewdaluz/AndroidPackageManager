/*
 * AMSD - APM Module System Daemon
 *
 * Validate shared session tokens issued by apmd using the same HMAC material.
 */

/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: security_manager.cpp
 * Purpose: Validate AMSD session tokens using the shared apmd session file and
 *          common HMAC derivation material.
 * Last Modified: 2026-03-15 11:56:16.535858305 -0400.
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

#include "security_manager.hpp"

#include "logger.hpp"
#include "security.hpp"

#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <array>
#include <iomanip>
#include <sstream>

namespace apm::amsd {

namespace {

std::string formatOpenSslError() {
  unsigned long err = ERR_get_error();
  if (err == 0)
    return "OpenSSL error";

  char buf[256] = {0};
  ERR_error_string_n(err, buf, sizeof(buf));
  return std::string(buf);
}

} // namespace

SecurityManager::SecurityManager() = default;

std::string SecurityManager::bytesToHex(const std::vector<uint8_t> &data) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (uint8_t b : data) {
    out << std::setw(2) << static_cast<int>(b);
  }
  return out.str();
}

bool SecurityManager::deriveHmac(const apm::security::SessionState &state,
                                 std::string &out, std::string *errorMsg) {
  std::vector<uint8_t> key;
  if (!crypto_.deriveKeyMaterial(key, errorMsg))
    return false;

  const std::string payload = apm::security::buildSessionPayload(state);
  std::array<uint8_t, SHA256_DIGEST_LENGTH> mac{};
  unsigned int macLen = 0;
  if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char *>(payload.data()),
            payload.size(), mac.data(), &macLen) ||
      macLen != mac.size()) {
    if (errorMsg)
      *errorMsg = "Session HMAC failed: " + formatOpenSslError();
    return false;
  }

  out = bytesToHex(std::vector<uint8_t>(mac.begin(), mac.begin() + macLen));
  return true;
}

bool SecurityManager::validateSessionToken(const std::string &token,
                                           std::string *errorMsg) {
  if (token.empty()) {
    if (errorMsg)
      *errorMsg = "Session token missing";
    return false;
  }

  apm::security::SessionState state;
  if (!apm::security::loadSession(state, errorMsg)) {
    if (errorMsg && errorMsg->empty())
      *errorMsg = "Authentication required";
    return false;
  }

  std::string expectedHmac;
  if (!deriveHmac(state, expectedHmac, errorMsg))
    return false;

  if (expectedHmac != state.hmac) {
    if (errorMsg)
      *errorMsg = "Session integrity check failed";
    return false;
  }

  if (apm::security::isSessionExpired(state,
                                      apm::security::currentUnixSeconds())) {
    if (errorMsg)
      *errorMsg = "Session expired";
    return false;
  }

  if (state.token != token) {
    if (errorMsg)
      *errorMsg = "Session token mismatch";
    return false;
  }

  return true;
}

} // namespace apm::amsd
