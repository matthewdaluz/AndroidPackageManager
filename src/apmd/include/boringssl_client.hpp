/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: boringssl_client.hpp
 * Purpose: Local symmetric crypto engine built on BoringSSL (AES-256-GCM +
 * PBKDF2). Last Modified: 2026-03-15 11:56:16.537055051 -0400.
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
#include <vector>

namespace apm::daemon {

class CryptoEngine {
public:
  CryptoEngine();

  // Ensure the master key exists on disk and is loaded into memory.
  bool ensureMasterKey(std::string *errorMsg = nullptr);

  // Generate cryptographically secure random bytes.
  bool randomBytes(std::size_t count, std::vector<uint8_t> &out,
                   std::string *errorMsg = nullptr);

  // Encrypt plaintext with AES-256-GCM. Outputs a 12-byte IV, ciphertext, and
  // 16-byte tag.
  bool encrypt(const std::vector<uint8_t> &plaintext,
               std::vector<uint8_t> &ivOut, std::vector<uint8_t> &ciphertextOut,
               std::vector<uint8_t> &tagOut, std::string *errorMsg = nullptr);

  // Decrypt ciphertext with AES-256-GCM.
  bool decrypt(const std::vector<uint8_t> &iv,
               const std::vector<uint8_t> &ciphertext,
               const std::vector<uint8_t> &tag,
               std::vector<uint8_t> &plaintextOut,
               std::string *errorMsg = nullptr);

  // Derive a PBKDF2-HMAC-SHA256 key from a user secret and salt.
  bool derivePasswordKey(const std::string &secret,
                         const std::vector<uint8_t> &salt,
                         std::vector<uint8_t> &derivedOut,
                         std::string *errorMsg = nullptr);

  // Deterministic material derived from the master key for session binding.
  bool deriveKeyMaterial(std::vector<uint8_t> &materialOut,
                         std::string *errorMsg = nullptr);

private:
  bool loadMasterKey(std::string *errorMsg);
  bool writeMasterKey(const std::vector<uint8_t> &key, std::string *errorMsg);
  std::string formatCryptoError() const;

  std::vector<uint8_t> m_masterKey;
  bool m_masterKeyLoaded;
};

} // namespace apm::daemon
