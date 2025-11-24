/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: mbedtls_client.cpp
 * Purpose: Local symmetric crypto engine built on mbedTLS (AES-256-GCM +
 * PBKDF2). Last Modified: November 23rd, 2025. - 12:06 PM Eastern Time.
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

#include <fstream>
#include <sys/stat.h>

#include <mbedtls/error.h>
#include <mbedtls/gcm.h>
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>

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
  char buf[128] = {0};
  mbedtls_strerror(code, buf, sizeof(buf));
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

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);

  ciphertextOut.resize(plaintext.size());
  tagOut.resize(kGcmTagLen);

  int ret =
      mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, m_masterKey.data(),
                         static_cast<unsigned int>(m_masterKey.size() * 8));
  if (ret != 0) {
    if (errorMsg)
      *errorMsg = "Failed to set AES-GCM key: " + formatError(ret);
    mbedtls_gcm_free(&ctx);
    return false;
  }

  ret = mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, plaintext.size(),
                                  ivOut.data(), ivOut.size(), nullptr, 0,
                                  plaintext.data(), ciphertextOut.data(),
                                  tagOut.size(), tagOut.data());

  mbedtls_gcm_free(&ctx);

  if (ret != 0) {
    if (errorMsg)
      *errorMsg = "AES-GCM encryption failed: " + formatError(ret);
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

  mbedtls_gcm_context ctx;
  mbedtls_gcm_init(&ctx);

  plaintextOut.resize(ciphertext.size());

  int ret =
      mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, m_masterKey.data(),
                         static_cast<unsigned int>(m_masterKey.size() * 8));
  if (ret != 0) {
    if (errorMsg)
      *errorMsg = "Failed to set AES-GCM key: " + formatError(ret);
    mbedtls_gcm_free(&ctx);
    return false;
  }

  ret = mbedtls_gcm_auth_decrypt(&ctx, ciphertext.size(), iv.data(), iv.size(),
                                 nullptr, 0, tag.data(), tag.size(),
                                 ciphertext.data(), plaintextOut.data());

  mbedtls_gcm_free(&ctx);

  if (ret != 0) {
    if (errorMsg)
      *errorMsg = "AES-GCM decryption failed: " + formatError(ret);
    plaintextOut.clear();
    return false;
  }

  return true;
}

bool CryptoEngine::derivePasswordKey(const std::string &secret,
                                     const std::vector<uint8_t> &salt,
                                     std::vector<uint8_t> &derivedOut,
                                     std::string *errorMsg) {
  derivedOut.assign(kMasterKeyLen, 0);
  const int ret = mbedtls_pkcs5_pbkdf2_hmac_ext(
      MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char *>(secret.data()),
      secret.size(), salt.data(), salt.size(), kPbkdf2Iterations,
      static_cast<uint32_t>(derivedOut.size()), derivedOut.data());

  if (ret != 0) {
    if (errorMsg)
      *errorMsg = "PBKDF2 derivation failed: " + formatError(ret);
    return false;
  }

  return true;
}

bool CryptoEngine::deriveKeyMaterial(std::vector<uint8_t> &materialOut,
                                     std::string *errorMsg) {
  if (!ensureMasterKey(errorMsg))
    return false;

  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) {
    if (errorMsg)
      *errorMsg = "Unable to get SHA-256 md info";
    return false;
  }

  const std::string label = "apm-session-derive";
  materialOut.assign(kSessionKeyLen, 0);

  const int ret =
      mbedtls_md_hmac(info, m_masterKey.data(), m_masterKey.size(),
                      reinterpret_cast<const unsigned char *>(label.data()),
                      label.size(), materialOut.data());

  if (ret != 0) {
    if (errorMsg)
      *errorMsg = "HMAC derivation failed: " + formatError(ret);
    materialOut.clear();
    return false;
  }

  return true;
}

} // namespace apm::daemon
