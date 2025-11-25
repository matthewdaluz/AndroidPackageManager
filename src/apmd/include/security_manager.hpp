/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: security_manager.hpp
 * Purpose: Declare daemon-side helpers for password/PIN validation and session
 * issuance. Last Modified: November 23rd, 2025. - 12:06 PM Eastern Time.
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

#include "mbedtls_client.hpp"
#include "security.hpp"

#include <string>
#include <vector>

namespace apm::daemon {

class SecurityManager {
public:
  SecurityManager();

  bool isPasspinConfigured() const;

  bool authenticate(const std::string &action, const std::string &secret,
                    apm::security::SessionState &sessionOut,
                    std::string *errorMsg = nullptr);

  bool validateSessionToken(const std::string &token,
                            std::string *errorMsg = nullptr);

private:
  bool persistPasspin(const std::string &secret, std::string *errorMsg);
  bool decryptPasspin(std::vector<uint8_t> &saltOut,
                      std::vector<uint8_t> &derivedOut, std::string *errorMsg);
  bool verifySecret(const std::string &secret, std::string *errorMsg);
  bool issueSession(apm::security::SessionState &sessionOut,
                    std::string *errorMsg);
  bool deriveHmac(const apm::security::SessionState &state, std::string &out,
                  std::string *errorMsg);
  std::string randomHex(std::size_t bytes);
  std::string bytesToHex(const std::vector<uint8_t> &data);
  bool constantTimeEquals(const std::vector<uint8_t> &a,
                          const std::vector<uint8_t> &b) const;

  CryptoEngine m_crypto;
};

} // namespace apm::daemon
