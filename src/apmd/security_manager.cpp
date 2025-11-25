/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: security_manager.cpp
 * Purpose: Implement daemon-side password/PIN handling plus session issuance
 * and validation. Last Modified: November 23rd, 2025. - 12:06 PM Eastern Time.
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
#include <iomanip>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <mbedtls/error.h>
#include <mbedtls/md.h>

namespace apm::daemon {

namespace {

constexpr std::size_t kSaltLen = 32;
constexpr std::size_t kDerivedLen = 32;
constexpr std::size_t kIvLen = 12;
constexpr std::size_t kTagLen = 16;

std::string formatMbedError(int code) {
  char buf[128] = {0};
  mbedtls_strerror(code, buf, sizeof(buf));
  return std::string(buf);
}

} // namespace

SecurityManager::SecurityManager() = default;

bool SecurityManager::isPasspinConfigured() const {
  return apm::fs::isFile(apm::config::PASS_PIN_FILE);
}

std::string SecurityManager::randomHex(std::size_t bytes) {
  std::random_device rd;
  std::mt19937 rng(rd());
  std::uniform_int_distribution<int> dist(0, 255);
  std::ostringstream out;
  for (std::size_t i = 0; i < bytes; ++i) {
    int v = dist(rng);
    out << std::hex << std::setw(2) << std::setfill('0') << (v & 0xff);
  }
  return out.str();
}

std::string SecurityManager::bytesToHex(const std::vector<uint8_t> &data) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (uint8_t b : data) {
    out << std::setw(2) << static_cast<int>(b);
  }
  return out.str();
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
                                     std::string *errorMsg) {
  if (secret.empty()) {
    if (errorMsg)
      *errorMsg = "Password/PIN cannot be empty";
    return false;
  }

  if (isPasspinConfigured()) {
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

  if (!apm::fs::writeFile(apm::config::PASS_PIN_FILE, stored, true)) {
    if (errorMsg)
      *errorMsg = "Failed to write encrypted password/PIN";
    return false;
  }

  ::chmod(apm::config::PASS_PIN_FILE, 0600);
  return true;
}

bool SecurityManager::decryptPasspin(std::vector<uint8_t> &saltOut,
                                     std::vector<uint8_t> &derivedOut,
                                     std::string *errorMsg) {
  std::string raw;
  if (!apm::fs::readFile(apm::config::PASS_PIN_FILE, raw)) {
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

bool SecurityManager::deriveHmac(const apm::security::SessionState &state,
                                 std::string &out, std::string *errorMsg) {
  std::vector<uint8_t> key;
  if (!m_crypto.deriveKeyMaterial(key, errorMsg))
    return false;

  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) {
    if (errorMsg)
      *errorMsg = "Unable to get SHA-256 md info";
    return false;
  }

  const std::string payload = apm::security::buildSessionPayload(state);
  std::vector<uint8_t> mac(mbedtls_md_get_size(info));

  const int ret =
      mbedtls_md_hmac(info, key.data(), key.size(),
                      reinterpret_cast<const unsigned char *>(payload.data()),
                      payload.size(), mac.data());

  if (ret != 0) {
    if (errorMsg)
      *errorMsg = "Session HMAC failed: " + formatMbedError(ret);
    return false;
  }

  out = bytesToHex(mac);
  return true;
}

bool SecurityManager::issueSession(apm::security::SessionState &sessionOut,
                                   std::string *errorMsg) {
  sessionOut = apm::security::SessionState{};
  sessionOut.token = randomHex(32);
  sessionOut.expiresAt = apm::security::currentUnixSeconds() + 180;

  if (!deriveHmac(sessionOut, sessionOut.hmac, errorMsg))
    return false;

  if (!apm::security::writeSession(sessionOut, errorMsg))
    return false;

  return true;
}

bool SecurityManager::authenticate(const std::string &actionRaw,
                                   const std::string &secret,
                                   apm::security::SessionState &sessionOut,
                                   std::string *errorMsg) {
  std::string action = actionRaw;
  std::transform(action.begin(), action.end(), action.begin(), ::tolower);
  if (action.empty())
    action = "unlock";

  if (action == "set") {
    if (!persistPasspin(secret, errorMsg))
      return false;
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

  return true;
}

} // namespace apm::daemon
