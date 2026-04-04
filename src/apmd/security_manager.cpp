/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: security_manager.cpp
 * Purpose: Implement daemon-side password/PIN handling plus session issuance
 * and validation using BoringSSL primitives.
 * Last Modified: 2026-03-15 13:02:02.294334606 -0400.
 * Author: Matthew DaLuz - RedHead Founder
 *
 * APM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * APM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See thei
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with APM. If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "include/security_manager.hpp"

#include "config.hpp"
#include "fs.hpp"
#include "security.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

namespace apm::daemon {

namespace {

constexpr std::size_t kSaltLen = 32;
constexpr std::size_t kDerivedLen = 32;
constexpr std::size_t kIvLen = 12;
constexpr std::size_t kTagLen = 16;
constexpr std::uint64_t kResetCooldownSeconds = 300;

std::string formatCryptoError() {
  unsigned long err = ERR_get_error();
  if (err == 0)
    return "BoringSSL error";

  char buf[256] = {0};
  ERR_error_string_n(err, buf, sizeof(buf));
  return std::string(buf);
}

} // namespace

SecurityManager::SecurityManager() = default;

bool SecurityManager::isPasspinConfigured() const {
  return apm::fs::isFile(apm::config::getPassPinFile());
}

bool SecurityManager::randomHex(std::size_t bytes, std::string &out,
                                std::string *errorMsg) {
  out.clear();
  std::vector<uint8_t> random;
  if (!m_crypto.randomBytes(bytes, random, errorMsg))
    return false;
  out = bytesToHex(random);
  return true;
}

std::string SecurityManager::bytesToHex(const std::vector<uint8_t> &data) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (uint8_t b : data) {
    out << std::setw(2) << static_cast<int>(b);
  }
  return out.str();
}

bool SecurityManager::hexToBytes(const std::string &hex,
                                 std::vector<uint8_t> &out) const {
  out.clear();
  if (hex.size() % 2 != 0)
    return false;

  auto decodeNibble = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
      return 10 + (c - 'A');
    return -1;
  };

  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    int hi = decodeNibble(hex[i]);
    int lo = decodeNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      out.clear();
      return false;
    }
    out.push_back(static_cast<uint8_t>((hi << 4) | lo));
  }

  return true;
}

bool SecurityManager::constantTimeEquals(const std::vector<uint8_t> &a,
                                         const std::vector<uint8_t> &b) const {
  if (a.size() != b.size())
    return false;
  uint8_t diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return diff == 0;
}

bool SecurityManager::persistPasspin(const std::string &secret,
                                     bool allowOverwrite,
                                     std::string *errorMsg) {
  if (secret.empty()) {
    if (errorMsg)
      *errorMsg = "Password/PIN cannot be empty";
    return false;
  }

  if (isPasspinConfigured() && !allowOverwrite) {
    if (errorMsg)
      *errorMsg =
          "Password/PIN already set. Factory reset is required to clear it.";
    return false;
  }

  if (!apm::security::ensureSecurityDir(errorMsg))
    return false;

  if (!m_crypto.ensureMasterKey(errorMsg))
    return false;

  std::vector<uint8_t> salt;
  if (!m_crypto.randomBytes(kSaltLen, salt, errorMsg))
    return false;

  std::vector<uint8_t> derived;
  if (!m_crypto.derivePasswordKey(secret, salt, derived, errorMsg))
    return false;

  std::vector<uint8_t> plain;
  plain.reserve(salt.size() + derived.size());
  plain.insert(plain.end(), salt.begin(), salt.end());
  plain.insert(plain.end(), derived.begin(), derived.end());

  std::vector<uint8_t> iv;
  std::vector<uint8_t> ciphertext;
  std::vector<uint8_t> tag;
  if (!m_crypto.encrypt(plain, iv, ciphertext, tag, errorMsg))
    return false;

  std::string stored;
  stored.reserve(iv.size() + ciphertext.size() + tag.size());
  stored.append(reinterpret_cast<const char *>(iv.data()), iv.size());
  stored.append(reinterpret_cast<const char *>(ciphertext.data()),
                ciphertext.size());
  stored.append(reinterpret_cast<const char *>(tag.data()), tag.size());

  const std::string passPinFile = apm::config::getPassPinFile();
  if (!apm::fs::writeFile(passPinFile, stored, true)) {
    if (errorMsg)
      *errorMsg = "Failed to write encrypted password/PIN";
    return false;
  }

  ::chmod(passPinFile.c_str(), 0600);
  return true;
}

bool SecurityManager::persistSecurityQuestions(
    const std::vector<std::pair<std::string, std::string>> &questions,
    std::string *errorMsg) {
  if (questions.size() != apm::security::SECURITY_QUESTION_COUNT) {
    if (errorMsg)
      *errorMsg = "Exactly " +
                  std::to_string(apm::security::SECURITY_QUESTION_COUNT) +
                  " security questions are required.";
    return false;
  }

  if (!apm::security::ensureSecurityDir(errorMsg))
    return false;

  if (!m_crypto.ensureMasterKey(errorMsg))
    return false;

  std::ostringstream plain;
  for (const auto &qa : questions) {
    const std::string &prompt = qa.first;
    const std::string &answer = qa.second;
    if (prompt.empty() || answer.empty()) {
      if (errorMsg)
        *errorMsg = "Security questions and answers cannot be empty";
      return false;
    }
    if (prompt.find('\n') != std::string::npos ||
        prompt.find('\r') != std::string::npos) {
      if (errorMsg)
        *errorMsg = "Security questions cannot contain newlines";
      return false;
    }

    std::vector<uint8_t> salt;
    if (!m_crypto.randomBytes(kSaltLen, salt, errorMsg))
      return false;

    std::vector<uint8_t> derived;
    if (!m_crypto.derivePasswordKey(answer, salt, derived, errorMsg))
      return false;

    plain << "question:" << prompt << "\n";
    plain << "salt:" << bytesToHex(salt) << "\n";
    plain << "answer:" << bytesToHex(derived) << "\n\n";
  }

  std::string plainStr = plain.str();
  std::vector<uint8_t> iv;
  std::vector<uint8_t> ciphertext;
  std::vector<uint8_t> tag;
  std::vector<uint8_t> plainBytes(plainStr.begin(), plainStr.end());

  if (!m_crypto.encrypt(plainBytes, iv, ciphertext, tag, errorMsg))
    return false;

  std::string stored;
  stored.reserve(iv.size() + ciphertext.size() + tag.size());
  stored.append(reinterpret_cast<const char *>(iv.data()), iv.size());
  stored.append(reinterpret_cast<const char *>(ciphertext.data()),
                ciphertext.size());
  stored.append(reinterpret_cast<const char *>(tag.data()), tag.size());

  const std::string qaFile = apm::config::getSecurityQaFile();
  if (!apm::fs::writeFile(qaFile, stored, true)) {
    if (errorMsg)
      *errorMsg = "Failed to write security questions file";
    return false;
  }

  ::chmod(qaFile.c_str(), 0600);
  return true;
}

bool SecurityManager::loadStoredQuestions(std::vector<StoredQuestion> &out,
                                          std::string *errorMsg) const {
  out.clear();

  std::string raw;
  if (!apm::fs::readFile(apm::config::getSecurityQaFile(), raw)) {
    if (errorMsg)
      *errorMsg = "Security questions are not configured";
    return false;
  }

  if (raw.size() <= kIvLen + kTagLen) {
    if (errorMsg)
      *errorMsg = "Stored security questions are corrupted";
    return false;
  }

  const std::vector<uint8_t> iv(raw.begin(), raw.begin() + kIvLen);
  const std::vector<uint8_t> tag(raw.end() - kTagLen, raw.end());
  const std::vector<uint8_t> ciphertext(raw.begin() + kIvLen,
                                        raw.end() - kTagLen);

  std::vector<uint8_t> plainBytes;
  if (!const_cast<decltype(m_crypto) &>(m_crypto).decrypt(iv, ciphertext, tag,
                                                          plainBytes, errorMsg))
    return false;

  std::string plain(plainBytes.begin(), plainBytes.end());
  std::istringstream in(plain);
  std::string line;
  StoredQuestion current;

  auto finalizeCurrent = [&]() -> bool {
    if (current.prompt.empty() && current.salt.empty() &&
        current.answerHash.empty())
      return true;
    if (current.prompt.empty() || current.salt.size() != kSaltLen ||
        current.answerHash.size() != kDerivedLen)
      return false;
    out.push_back(current);
    current = StoredQuestion{};
    return true;
  };

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line.empty()) {
      if (!finalizeCurrent()) {
        if (errorMsg)
          *errorMsg = "Malformed security question entry";
        return false;
      }
      continue;
    }

    auto pos = line.find(':');
    if (pos == std::string::npos) {
      if (errorMsg)
        *errorMsg = "Malformed security question line";
      return false;
    }

    const std::string key = line.substr(0, pos);
    const std::string value = line.substr(pos + 1);

    if (key == "question") {
      current.prompt = value;
    } else if (key == "salt") {
      if (!hexToBytes(value, current.salt)) {
        if (errorMsg)
          *errorMsg = "Malformed salt in security question file";
        return false;
      }
    } else if (key == "answer") {
      if (!hexToBytes(value, current.answerHash)) {
        if (errorMsg)
          *errorMsg = "Malformed answer in security question file";
        return false;
      }
    } else {
      if (errorMsg)
        *errorMsg = "Unknown field in security question file";
      return false;
    }
  }

  if (!finalizeCurrent()) {
    if (errorMsg)
      *errorMsg = "Incomplete security question entry";
    return false;
  }

  if (out.empty() || out.size() != apm::security::SECURITY_QUESTION_COUNT) {
    if (errorMsg)
      *errorMsg = "Security question data is incomplete";
    out.clear();
    return false;
  }

  return true;
}

bool SecurityManager::isLockedOut(std::uint64_t nowSeconds,
                                  std::uint64_t &unlockAt) const {
  unlockAt = 0;

  std::string raw;
  if (!apm::fs::readFile(apm::config::getResetLockoutFile(), raw))
    return false;

  errno = 0;
  char *end = nullptr;
  unsigned long long parsed = std::strtoull(raw.c_str(), &end, 10);
  if (errno != 0 || end == raw.c_str()) {
    apm::fs::removeFile(apm::config::getResetLockoutFile());
    return false;
  }

  unlockAt = static_cast<std::uint64_t>(parsed);
  if (nowSeconds >= unlockAt) {
    apm::fs::removeFile(apm::config::getResetLockoutFile());
    return false;
  }

  return true;
}

bool SecurityManager::writeLockoutUntil(std::uint64_t unlockAt,
                                        std::string *errorMsg) const {
  if (!apm::security::ensureSecurityDir(errorMsg))
    return false;

  const std::string lockoutFile = apm::config::getResetLockoutFile();
  if (!apm::fs::writeFile(lockoutFile, std::to_string(unlockAt), true)) {
    if (errorMsg)
      *errorMsg = "Failed to write lockout file";
    return false;
  }

  ::chmod(lockoutFile.c_str(), 0600);
  return true;
}

bool SecurityManager::decryptPasspin(std::vector<uint8_t> &saltOut,
                                     std::vector<uint8_t> &derivedOut,
                                     std::string *errorMsg) {
  std::string raw;
  if (!apm::fs::readFile(apm::config::getPassPinFile(), raw)) {
    if (errorMsg)
      *errorMsg = "Password/PIN not configured";
    return false;
  }

  if (raw.size() <= kIvLen + kTagLen) {
    if (errorMsg)
      *errorMsg = "Stored password/PIN is corrupted";
    return false;
  }

  const std::vector<uint8_t> iv(raw.begin(), raw.begin() + kIvLen);
  const std::vector<uint8_t> tag(raw.end() - kTagLen, raw.end());
  const std::vector<uint8_t> ciphertext(raw.begin() + kIvLen,
                                        raw.end() - kTagLen);

  std::vector<uint8_t> plain;
  if (!m_crypto.decrypt(iv, ciphertext, tag, plain, errorMsg))
    return false;

  if (plain.size() != kSaltLen + kDerivedLen) {
    if (errorMsg)
      *errorMsg = "Stored password/PIN is corrupted";
    return false;
  }

  saltOut.assign(plain.begin(), plain.begin() + kSaltLen);
  derivedOut.assign(plain.begin() + kSaltLen, plain.end());
  return true;
}

bool SecurityManager::verifySecret(const std::string &secret,
                                   std::string *errorMsg) {
  if (secret.empty()) {
    if (errorMsg)
      *errorMsg = "Password/PIN cannot be empty";
    return false;
  }

  std::vector<uint8_t> salt;
  std::vector<uint8_t> storedDerived;
  if (!decryptPasspin(salt, storedDerived, errorMsg))
    return false;

  std::vector<uint8_t> attempt;
  if (!m_crypto.derivePasswordKey(secret, salt, attempt, errorMsg))
    return false;

  if (!constantTimeEquals(storedDerived, attempt)) {
    if (errorMsg)
      *errorMsg = "Password/PIN verification failed";
    return false;
  }

  return true;
}

bool SecurityManager::fetchSecurityQuestions(
    std::vector<std::string> &questionsOut, std::string *errorMsg) {
  questionsOut.clear();

  const std::uint64_t now = apm::security::currentUnixSeconds();
  std::uint64_t unlockAt = 0;
  if (isLockedOut(now, unlockAt)) {
    if (errorMsg) {
      const std::uint64_t remaining = (unlockAt > now) ? (unlockAt - now) : 0;
      std::ostringstream oss;
      oss << "Password/PIN reset is locked. Try again in " << (remaining / 60)
          << "m " << (remaining % 60) << "s.";
      *errorMsg = oss.str();
    }
    return false;
  }

  std::vector<StoredQuestion> stored;
  if (!loadStoredQuestions(stored, errorMsg))
    return false;

  questionsOut.reserve(stored.size());
  for (const auto &q : stored)
    questionsOut.push_back(q.prompt);

  return true;
}

bool SecurityManager::validateSecurityAnswers(
    const std::vector<std::string> &answers, std::string *errorMsg) {
  const std::uint64_t now = apm::security::currentUnixSeconds();
  std::uint64_t unlockAt = 0;
  if (isLockedOut(now, unlockAt)) {
    if (errorMsg) {
      const std::uint64_t remaining = (unlockAt > now) ? (unlockAt - now) : 0;
      std::ostringstream oss;
      oss << "Password/PIN reset is locked. Try again in " << (remaining / 60)
          << "m " << (remaining % 60) << "s.";
      *errorMsg = oss.str();
    }
    return false;
  }

  std::vector<StoredQuestion> stored;
  if (!loadStoredQuestions(stored, errorMsg))
    return false;

  if (answers.size() != stored.size()) {
    if (errorMsg)
      *errorMsg = "All security questions must be answered.";
    return false;
  }

  for (std::size_t i = 0; i < stored.size(); ++i) {
    std::vector<uint8_t> attempt;
    if (!m_crypto.derivePasswordKey(answers[i], stored[i].salt, attempt,
                                    errorMsg))
      return false;

    if (!constantTimeEquals(attempt, stored[i].answerHash)) {
      const std::uint64_t until = now + kResetCooldownSeconds;
      writeLockoutUntil(until, nullptr);
      if (errorMsg)
        *errorMsg =
            "One or more security answers are incorrect. Try again in 5 "
            "minutes.";
      return false;
    }
  }

  apm::fs::removeFile(apm::config::getResetLockoutFile());
  return true;
}

bool SecurityManager::resetForgottenSecret(
    const std::string &newSecret, const std::vector<std::string> &answers,
    apm::security::SessionState &sessionOut, std::string *errorMsg) {
  if (!validateSecurityAnswers(answers, errorMsg))
    return false;

  if (!persistPasspin(newSecret, true, errorMsg))
    return false;

  return issueSession(sessionOut, errorMsg);
}

bool SecurityManager::deriveHmac(const apm::security::SessionState &state,
                                 std::string &out, std::string *errorMsg) {
  std::vector<uint8_t> key;
  if (!m_crypto.deriveKeyMaterial(key, errorMsg))
    return false;

  const std::string payload = apm::security::buildSessionPayload(state);
  std::array<uint8_t, SHA256_DIGEST_LENGTH> mac{};
  unsigned int macLen = 0;
  if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char *>(payload.data()),
            payload.size(), mac.data(), &macLen) ||
      macLen != mac.size()) {
    if (errorMsg)
      *errorMsg = "Session HMAC failed: " + formatCryptoError();
    return false;
  }

  out = bytesToHex(std::vector<uint8_t>(mac.begin(), mac.begin() + macLen));
  return true;
}

bool SecurityManager::issueSession(apm::security::SessionState &sessionOut,
                                   std::string *errorMsg) {
  sessionOut = apm::security::SessionState{};
  if (!randomHex(32, sessionOut.token, errorMsg)) {
    if (errorMsg && errorMsg->empty())
      *errorMsg = "Failed to generate session token";
    return false;
  }
  sessionOut.expiresAt = apm::security::currentUnixSeconds() + 180;

  if (!deriveHmac(sessionOut, sessionOut.hmac, errorMsg))
    return false;

  if (!apm::security::writeSession(sessionOut, errorMsg))
    return false;

  return true;
}

bool SecurityManager::authenticate(
    const std::string &actionRaw, const std::string &secret,
    const std::vector<std::pair<std::string, std::string>> &securityQuestions,
    apm::security::SessionState &sessionOut, std::string *errorMsg) {
  std::string action = actionRaw;
  std::transform(action.begin(), action.end(), action.begin(), ::tolower);
  if (action.empty())
    action = "unlock";

  if (action == "set") {
    if (isPasspinConfigured()) {
      if (errorMsg)
        *errorMsg =
            "Password/PIN already set. Use forgot-password to reset it.";
      return false;
    }

    if (securityQuestions.size() != apm::security::SECURITY_QUESTION_COUNT) {
      if (errorMsg)
        *errorMsg = "Security questions are required to set a password/PIN.";
      return false;
    }

    if (!persistSecurityQuestions(securityQuestions, errorMsg))
      return false;

    if (!persistPasspin(secret, false, errorMsg)) {
      apm::fs::removeFile(apm::config::getSecurityQaFile());
      return false;
    }
  } else {
    if (!verifySecret(secret, errorMsg))
      return false;
  }

  return issueSession(sessionOut, errorMsg);
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
    if (errorMsg)
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

  // Persist the session to keep the shared token file in sync for AMSD.
  apm::security::writeSession(state, nullptr);

  return true;
}

} // namespace apm::daemon
