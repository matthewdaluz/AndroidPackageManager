/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: boringssl_client.cpp
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

#include "include/boringssl_client.hpp"

#include "config.hpp"
#include "fs.hpp"
#include "security.hpp"

#include <openssl/base.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <sys/stat.h>
#include <utility>

namespace apm::daemon {

namespace {

constexpr std::size_t kMasterKeyLen = 32;
constexpr std::size_t kGcmIvLen = 12;
constexpr std::size_t kGcmTagLen = 16;
constexpr std::size_t kSessionKeyLen = 32;
constexpr unsigned int kPbkdf2Iterations = 200000;

} // namespace

CryptoEngine::CryptoEngine() : m_masterKeyLoaded(false) {}

std::string CryptoEngine::formatOpenSslError() const {
  unsigned long err = ERR_get_error();
  if (err == 0)
    return "OpenSSL error";

  char buf[256] = {0};
  ERR_error_string_n(err, buf, sizeof(buf));
  return std::string(buf);
}

bool CryptoEngine::randomBytes(std::size_t count, std::vector<uint8_t> &out,
                               std::string *errorMsg) {
  out.assign(count, 0);
  if (count == 0)
    return true;

  if (RAND_bytes(out.data(), static_cast<int>(out.size())) != 1) {
    if (errorMsg)
      *errorMsg = "Unable to generate random bytes: " + formatOpenSslError();
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

  bssl::UniquePtr<EVP_CIPHER_CTX> ctx(EVP_CIPHER_CTX_new());
  if (!ctx) {
    if (errorMsg)
      *errorMsg = "Failed to allocate cipher context";
    return false;
  }

  if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr,
                         nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(ivOut.size()), nullptr) != 1 ||
      EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, m_masterKey.data(),
                         ivOut.data()) != 1) {
    if (errorMsg)
      *errorMsg = "Failed to initialize AES-GCM: " + formatOpenSslError();
    return false;
  }

  ciphertextOut.resize(plaintext.empty() ? 1 : plaintext.size());
  int outLen = 0;
  int totalLen = 0;
  if (!plaintext.empty()) {
    if (EVP_EncryptUpdate(ctx.get(), ciphertextOut.data(), &outLen,
                          plaintext.data(),
                          static_cast<int>(plaintext.size())) != 1) {
      if (errorMsg)
        *errorMsg = "AES-GCM encryption failed: " + formatOpenSslError();
      return false;
    }
    totalLen = outLen;
  }

  if (EVP_EncryptFinal_ex(
          ctx.get(), ciphertextOut.data() + static_cast<std::size_t>(totalLen),
          &outLen) != 1) {
    if (errorMsg)
      *errorMsg = "AES-GCM finalize failed: " + formatOpenSslError();
    return false;
  }

  ciphertextOut.resize(static_cast<std::size_t>(totalLen + outLen));

  tagOut.resize(kGcmTagLen);
  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG,
                          static_cast<int>(tagOut.size()),
                          tagOut.data()) != 1) {
    if (errorMsg)
      *errorMsg = "Failed to retrieve AES-GCM tag: " + formatOpenSslError();
    return false;
  }

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

  bssl::UniquePtr<EVP_CIPHER_CTX> ctx(EVP_CIPHER_CTX_new());
  if (!ctx) {
    if (errorMsg)
      *errorMsg = "Failed to allocate cipher context";
    return false;
  }

  if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr,
                         nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN,
                          static_cast<int>(iv.size()), nullptr) != 1 ||
      EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, m_masterKey.data(),
                         iv.data()) != 1) {
    if (errorMsg)
      *errorMsg = "Failed to initialize AES-GCM: " + formatOpenSslError();
    return false;
  }

  plaintextOut.resize(ciphertext.empty() ? 1 : ciphertext.size());
  int outLen = 0;
  int totalLen = 0;
  if (!ciphertext.empty()) {
    if (EVP_DecryptUpdate(ctx.get(), plaintextOut.data(), &outLen,
                          ciphertext.data(),
                          static_cast<int>(ciphertext.size())) != 1) {
      if (errorMsg)
        *errorMsg = "AES-GCM decryption failed: " + formatOpenSslError();
      plaintextOut.clear();
      return false;
    }
    totalLen = outLen;
  }

  if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG,
                          static_cast<int>(tag.size()),
                          const_cast<uint8_t *>(tag.data())) != 1) {
    if (errorMsg)
      *errorMsg = "Failed to set AES-GCM tag: " + formatOpenSslError();
    plaintextOut.clear();
    return false;
  }

  if (EVP_DecryptFinal_ex(
          ctx.get(), plaintextOut.data() + static_cast<std::size_t>(totalLen),
          &outLen) != 1) {
    if (errorMsg)
      *errorMsg = "AES-GCM authentication failed: " + formatOpenSslError();
    plaintextOut.clear();
    return false;
  }

  plaintextOut.resize(static_cast<std::size_t>(totalLen + outLen));
  return true;
}

bool CryptoEngine::derivePasswordKey(const std::string &secret,
                                     const std::vector<uint8_t> &salt,
                                     std::vector<uint8_t> &derivedOut,
                                     std::string *errorMsg) {
  derivedOut.assign(kMasterKeyLen, 0);
  if (PKCS5_PBKDF2_HMAC(secret.data(), static_cast<int>(secret.size()),
                        salt.data(), static_cast<int>(salt.size()),
                        static_cast<int>(kPbkdf2Iterations), EVP_sha256(),
                        static_cast<int>(derivedOut.size()),
                        derivedOut.data()) != 1) {
    if (errorMsg)
      *errorMsg = "PBKDF2 derivation failed: " + formatOpenSslError();
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
  unsigned int outLen = 0;
  if (!HMAC(EVP_sha256(), m_masterKey.data(),
            static_cast<int>(m_masterKey.size()),
            reinterpret_cast<const unsigned char *>(label.data()), label.size(),
            materialOut.data(), &outLen) ||
      outLen != materialOut.size()) {
    if (errorMsg)
      *errorMsg = "HMAC derivation failed: " + formatOpenSslError();
    materialOut.clear();
    return false;
  }

  return true;
}

} // namespace apm::daemon
