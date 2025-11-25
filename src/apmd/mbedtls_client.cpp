/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: mbedtls_client.cpp
 * Purpose: Local symmetric crypto engine built on BoringSSL (AES-256-GCM +
 * PBKDF2). Last Modified: November 25th, 2025. - 11:45 AM Eastern Time.
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

#include "include/mbedtls_client.hpp"

#include "config.hpp"
#include "fs.hpp"
#include "security.hpp"

#include <algorithm>
#include <fstream>
#include <sys/stat.h>

#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace apm::daemon {

namespace {

constexpr std::size_t kMasterKeyLen = 32;
constexpr std::size_t kGcmIvLen = 12;
constexpr std::size_t kGcmTagLen = 16;
constexpr std::size_t kSessionKeyLen = 32;
constexpr unsigned int kPbkdf2Iterations = 200000;

} // namespace

CryptoEngine::CryptoEngine() : m_masterKeyLoaded(false) {}

std::string CryptoEngine::formatError(int code) const {
  (void)code;
  char buf[256] = {0};
  unsigned long err = ERR_get_error();
  if (err == 0)
    return "OpenSSL error";
  ERR_error_string_n(err, buf, sizeof(buf));
  return std::string(buf);
}

bool CryptoEngine::randomBytes(std::size_t count, std::vector<uint8_t> &out,
                               std::string *errorMsg) {
  out.assign(count, 0);
  std::ifstream rng("/dev/urandom", std::ios::in | std::ios::binary);
  if (!rng) {
    if (errorMsg)
      *errorMsg = "Unable to open /dev/urandom";
    return false;
  }

  std::size_t total = 0;
  while (total < count && rng.good()) {
    rng.read(reinterpret_cast<char *>(out.data() + total),
             static_cast<std::streamsize>(count - total));
    const std::streamsize got = rng.gcount();
    if (got <= 0)
      break;
    total += static_cast<std::size_t>(got);
  }

  if (total != count) {
    if (errorMsg)
      *errorMsg = "Failed to read requested random bytes from /dev/urandom";
    return false;
  }
  return true;
}

bool CryptoEngine::loadMasterKey(std::string *errorMsg) {
  std::string raw;
  if (!apm::fs::readFile(apm::config::MASTER_KEY_FILE, raw)) {
    if (errorMsg)
      *errorMsg = "Unable to read master key file";
    return false;
  }

  if (raw.size() != kMasterKeyLen) {
    if (errorMsg)
      *errorMsg = "Master key file has invalid length";
    return false;
  }

  m_masterKey.assign(raw.begin(), raw.end());
  m_masterKeyLoaded = true;
  return true;
}

bool CryptoEngine::writeMasterKey(const std::vector<uint8_t> &key,
                                  std::string *errorMsg) {
  const std::string data(reinterpret_cast<const char *>(key.data()),
                         key.size());
  if (!apm::fs::writeFile(apm::config::MASTER_KEY_FILE, data, true)) {
    if (errorMsg)
      *errorMsg = "Failed to write master key to disk";
    return false;
  }

  ::chmod(apm::config::MASTER_KEY_FILE, 0600);
  return true;
}

bool CryptoEngine::ensureMasterKey(std::string *errorMsg) {
  if (m_masterKeyLoaded)
    return true;

  if (!apm::security::ensureSecurityDir(errorMsg))
    return false;

  if (apm::fs::isFile(apm::config::MASTER_KEY_FILE)) {
    return loadMasterKey(errorMsg);
  }

  std::vector<uint8_t> key;
  if (!randomBytes(kMasterKeyLen, key, errorMsg))
    return false;

  if (!writeMasterKey(key, errorMsg))
    return false;

  m_masterKey = std::move(key);
  m_masterKeyLoaded = true;
  return true;
}

bool CryptoEngine::encrypt(const std::vector<uint8_t> &plaintext,
                           std::vector<uint8_t> &ivOut,
                           std::vector<uint8_t> &ciphertextOut,
                           std::vector<uint8_t> &tagOut,
                           std::string *errorMsg) {
  if (!ensureMasterKey(errorMsg))
    return false;

  if (!randomBytes(kGcmIvLen, ivOut, errorMsg))
    return false;

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    if (errorMsg)
      *errorMsg = "Failed to allocate cipher context";
    return false;
  }

  if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) !=
      1) {
    if (errorMsg)
      *errorMsg = "AES-GCM init failed: " + formatError(0);
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(ivOut.size()), nullptr) != 1) {
    if (errorMsg)
      *errorMsg = "AES-GCM IV length set failed: " + formatError(0);
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, m_masterKey.data(),
                         ivOut.data()) != 1) {
    if (errorMsg)
      *errorMsg = "AES-GCM key/iv set failed: " + formatError(0);
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  ciphertextOut.resize(plaintext.size());
  int outLen = 0;
  int total = 0;
  if (EVP_EncryptUpdate(ctx, ciphertextOut.data(), &outLen, plaintext.data(),
                        static_cast<int>(plaintext.size())) != 1) {
    if (errorMsg)
      *errorMsg = "AES-GCM encrypt failed: " + formatError(0);
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  total = outLen;

  if (EVP_EncryptFinal_ex(ctx, ciphertextOut.data() + total, &outLen) != 1) {
    if (errorMsg)
      *errorMsg = "AES-GCM final failed: " + formatError(0);
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }
  total += outLen;
  ciphertextOut.resize(static_cast<std::size_t>(total));

  tagOut.resize(kGcmTagLen);
  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG,
                          static_cast<int>(tagOut.size()),
                          tagOut.data()) != 1) {
    if (errorMsg)
      *errorMsg = "AES-GCM tag retrieval failed: " + formatError(0);
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  EVP_CIPHER_CTX_free(ctx);

  return true;
}

bool CryptoEngine::decrypt(const std::vector<uint8_t> &iv,
                           const std::vector<uint8_t> &ciphertext,
                           const std::vector<uint8_t> &tag,
                           std::vector<uint8_t> &plaintextOut,
                           std::string *errorMsg) {
  if (!ensureMasterKey(errorMsg))
    return false;

  if (iv.size() != kGcmIvLen) {
    if (errorMsg)
      *errorMsg = "Invalid IV length for AES-GCM";
    return false;
  }

  if (tag.size() != kGcmTagLen) {
    if (errorMsg)
      *errorMsg = "Invalid GCM tag length";
    return false;
  }

  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  if (!ctx) {
    if (errorMsg)
      *errorMsg = "Failed to allocate cipher context";
    return false;
  }

  if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) !=
      1) {
    if (errorMsg)
      *errorMsg = "AES-GCM init failed: " + formatError(0);
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(iv.size()), nullptr) != 1) {
    if (errorMsg)
      *errorMsg = "AES-GCM IV length set failed: " + formatError(0);
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, m_masterKey.data(),
                         iv.data()) != 1) {
    if (errorMsg)
      *errorMsg = "AES-GCM key/iv set failed: " + formatError(0);
    EVP_CIPHER_CTX_free(ctx);
    return false;
  }

  plaintextOut.resize(ciphertext.size());
  int outLen = 0;
  int total = 0;
  if (EVP_DecryptUpdate(ctx, plaintextOut.data(), &outLen, ciphertext.data(),
                        static_cast<int>(ciphertext.size())) != 1) {
    if (errorMsg)
      *errorMsg = "AES-GCM decrypt failed: " + formatError(0);
    EVP_CIPHER_CTX_free(ctx);
    plaintextOut.clear();
    return false;
  }
  total = outLen;

  if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG,
                          static_cast<int>(tag.size()),
                          const_cast<uint8_t *>(tag.data())) != 1) {
    if (errorMsg)
      *errorMsg = "AES-GCM tag set failed: " + formatError(0);
    EVP_CIPHER_CTX_free(ctx);
    plaintextOut.clear();
    return false;
  }

  if (EVP_DecryptFinal_ex(ctx, plaintextOut.data() + total, &outLen) != 1) {
    if (errorMsg)
      *errorMsg = "AES-GCM authentication failed: " + formatError(0);
    EVP_CIPHER_CTX_free(ctx);
    plaintextOut.clear();
    return false;
  }
  total += outLen;
  plaintextOut.resize(static_cast<std::size_t>(total));

  EVP_CIPHER_CTX_free(ctx);

  return true;
}

bool CryptoEngine::derivePasswordKey(const std::string &secret,
                                     const std::vector<uint8_t> &salt,
                                     std::vector<uint8_t> &derivedOut,
                                     std::string *errorMsg) {
  derivedOut.assign(kMasterKeyLen, 0);
  if (PKCS5_PBKDF2_HMAC(secret.data(), secret.size(), salt.data(), salt.size(),
                        kPbkdf2Iterations, EVP_sha256(), derivedOut.size(),
                        derivedOut.data()) != 1) {
    if (errorMsg)
      *errorMsg = "PBKDF2 derivation failed: " + formatError(0);
    return false;
  }

  return true;
}

bool CryptoEngine::deriveKeyMaterial(std::vector<uint8_t> &materialOut,
                                     std::string *errorMsg) {
  if (!ensureMasterKey(errorMsg))
    return false;

  const std::string label = "apm-session-derive";
  materialOut.assign(kSessionKeyLen, 0);

  unsigned int macLen = 0;
  unsigned char mac[EVP_MAX_MD_SIZE] = {0};
  if (!HMAC(EVP_sha256(), m_masterKey.data(), static_cast<int>(m_masterKey.size()),
            reinterpret_cast<const unsigned char *>(label.data()),
            label.size(), mac, &macLen)) {
    if (errorMsg)
      *errorMsg = "HMAC derivation failed: " + formatError(0);
    materialOut.clear();
    return false;
  }

  materialOut.assign(mac, mac + std::min<std::size_t>(macLen, kSessionKeyLen));

  return true;
}

} // namespace apm::daemon
