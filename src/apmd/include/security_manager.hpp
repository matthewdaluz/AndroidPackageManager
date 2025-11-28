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

#include "boringssl_client.hpp"
#include "security.hpp"

#include <string>
#include <utility>
#include <vector>

namespace apm::daemon {

class SecurityManager {
public:
  SecurityManager();

  bool isPasspinConfigured() const;

  bool authenticate(const std::string &action, const std::string &secret,
                    const std::vector<std::pair<std::string, std::string>>
                        &securityQuestions,
                    apm::security::SessionState &sessionOut,
                    std::string *errorMsg = nullptr);

  bool fetchSecurityQuestions(std::vector<std::string> &questionsOut,
                              std::string *errorMsg = nullptr);

  bool validateSecurityAnswers(const std::vector<std::string> &answers,
                               std::string *errorMsg = nullptr);

  bool resetForgottenSecret(const std::string &newSecret,
                            const std::vector<std::string> &answers,
                            apm::security::SessionState &sessionOut,
                            std::string *errorMsg = nullptr);

  bool validateSessionToken(const std::string &token,
                            std::string *errorMsg = nullptr);

private:
  struct StoredQuestion {
    std::string prompt;
    std::vector<uint8_t> salt;
    std::vector<uint8_t> answerHash;
  };

  bool persistPasspin(const std::string &secret, bool allowOverwrite,
                      std::string *errorMsg);
  bool persistSecurityQuestions(
      const std::vector<std::pair<std::string, std::string>> &questions,
      std::string *errorMsg);
  bool loadStoredQuestions(std::vector<StoredQuestion> &out,
                           std::string *errorMsg) const;
  bool decryptPasspin(std::vector<uint8_t> &saltOut,
                      std::vector<uint8_t> &derivedOut, std::string *errorMsg);
  bool verifySecret(const std::string &secret, std::string *errorMsg);
  bool issueSession(apm::security::SessionState &sessionOut,
                    std::string *errorMsg);
  bool deriveHmac(const apm::security::SessionState &state, std::string &out,
                  std::string *errorMsg);
  std::string randomHex(std::size_t bytes);
  std::string bytesToHex(const std::vector<uint8_t> &data);
  bool hexToBytes(const std::string &hex, std::vector<uint8_t> &out) const;
  bool constantTimeEquals(const std::vector<uint8_t> &a,
                          const std::vector<uint8_t> &b) const;
  bool isLockedOut(std::uint64_t nowSeconds, std::uint64_t &unlockAt) const;
  bool writeLockoutUntil(std::uint64_t unlockAt,
                         std::string *errorMsg) const;

  CryptoEngine m_crypto;
};

} // namespace apm::daemon
