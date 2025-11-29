/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: gpg_verify.cpp
 * Purpose: Implement OpenPGP RSA/SHA256 detached signature verification and key
 * import helpers using BoringSSL primitives.
 * Last Modified: November 28th, 2025. - 10:37 AM Eastern Time.
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

#include "gpg_verify.hpp"

#include "fs.hpp"
#include "logger.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <vector>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

// Compatibility shim: some systems (OpenSSL builds) do not provide BoringSSL's
// <openssl/base.h> and therefore do not define bssl::UniquePtr; implement a
// minimal replacement here so the code can compile against OpenSSL as well.
#ifndef OPENSSL_HEADER_BASE_H
namespace bssl {

template <typename T> struct Deleter;

// Specialize deleters for the OpenSSL types used in this file.
template <> struct Deleter<BIO> {
  static void Free(BIO *p) { BIO_free(p); }
};
template <> struct Deleter<EVP_PKEY> {
  static void Free(EVP_PKEY *p) { EVP_PKEY_free(p); }
};
template <> struct Deleter<RSA> {
  static void Free(RSA *p) { RSA_free(p); }
};
template <> struct Deleter<BIGNUM> {
  static void Free(BIGNUM *p) { BN_free(p); }
};
template <> struct Deleter<EVP_MD_CTX> {
  static void Free(EVP_MD_CTX *p) { EVP_MD_CTX_free(p); }
};

// Minimal UniquePtr implementation with move semantics and the operations
// used by this translation unit (get, release, reset, operator->, bool).
template <typename T> class UniquePtr {
public:
  UniquePtr() noexcept : ptr_(nullptr) {}
  explicit UniquePtr(T *p) noexcept : ptr_(p) {}
  UniquePtr(UniquePtr &&o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
  UniquePtr &operator=(UniquePtr &&o) noexcept {
    if (this != &o) {
      reset(o.ptr_);
      o.ptr_ = nullptr;
    }
    return *this;
  }

  UniquePtr(const UniquePtr &) = delete;
  UniquePtr &operator=(const UniquePtr &) = delete;

  ~UniquePtr() { reset(); }

  T *get() const noexcept { return ptr_; }
  T *release() noexcept {
    T *p = ptr_;
    ptr_ = nullptr;
    return p;
  }
  void reset(T *p = nullptr) noexcept {
    if (ptr_)
      Deleter<T>::Free(ptr_);
    ptr_ = p;
  }

  T *operator->() const noexcept { return ptr_; }
  explicit operator bool() const noexcept { return ptr_ != nullptr; }

private:
  T *ptr_;
};
} // namespace bssl
#endif // OPENSSL_HEADER_BASE_H

namespace apm::crypto {

namespace {

// RAII wrapper for RSA contexts to avoid leaking allocated buffers.
struct LoadedKey {
  LoadedKey() : keyBytes(0) {}
  LoadedKey(const LoadedKey &) = delete;
  LoadedKey &operator=(const LoadedKey &) = delete;

  bssl::UniquePtr<EVP_PKEY> pkey;
  std::size_t keyBytes;
};

struct ParsedSignature {
  uint8_t sigType = 0;
  uint8_t pubKeyAlgo = 0;
  uint8_t hashAlgo = 0;
  uint16_t hashLeft16 = 0;
  std::vector<uint8_t> hashedSubpackets;
  std::vector<uint8_t> unhashedSubpackets;
  std::vector<uint8_t> signatureMpi;
};

std::string formatOpenSslError() {
  unsigned long err = ERR_get_error();
  if (err == 0)
    return "OpenSSL error";

  char buf[256] = {0};
  ERR_error_string_n(err, buf, sizeof(buf));
  return std::string(buf);
}

bool isTrustedKeyFile(const std::string &path) {
  auto dotPos = path.find_last_of('.');
  if (dotPos == std::string::npos)
    return false;
  std::string ext = path.substr(dotPos);
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return ext == ".asc" || ext == ".gpg";
}

// Decode base64 into out. Returns true on success.
bool decodeBase64(const std::string &b64, std::vector<uint8_t> &out) {
  out.clear();
  if (b64.empty())
    return false;

  std::size_t maxOut = (b64.size() + 3) / 4 * 3;
  out.assign(maxOut, 0);
  int ret = EVP_DecodeBlock(out.data(),
                            reinterpret_cast<const unsigned char *>(b64.data()),
                            static_cast<int>(b64.size()));
  if (ret < 0)
    return false;

  std::size_t decoded = static_cast<std::size_t>(ret);
  std::size_t padding = 0;
  for (auto it = b64.rbegin(); it != b64.rend() && *it == '='; ++it)
    ++padding;
  if (padding > decoded)
    padding = 0;

  out.resize(decoded - padding);
  return true;
}

// Extract ASCII armor payload if present; otherwise try to base64-decode the
// full blob. If base64 fails, treat the content as already-binary.
bool decodeArmoredBlock(const std::string &text, std::vector<uint8_t> &out,
                        bool &armorFound, std::string *errorMsg) {
  armorFound = false;
  out.clear();

  const std::string beginMarker = "-----BEGIN";
  const std::string endMarker = "-----END";

  std::istringstream ss(text);
  std::string line;
  bool sawBegin = false;
  bool inPayload = false;
  std::string payload;

  while (std::getline(ss, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (!sawBegin) {
      if (line.find(beginMarker) == 0) {
        sawBegin = true;
      }
      continue;
    }

    if (!inPayload) {
      if (line.empty()) {
        inPayload = true;
      }
      continue;
    }

    if (line.find(endMarker) == 0)
      break;

    for (char c : line) {
      if (!std::isspace(static_cast<unsigned char>(c)))
        payload.push_back(c);
    }
  }

  if (sawBegin) {
    armorFound = true;
    if (!decodeBase64(payload, out)) {
      if (errorMsg)
        *errorMsg = "Failed to decode ASCII-armored data";
      return false;
    }
    return true;
  }

  std::string compact;
  compact.reserve(text.size());
  for (char c : text) {
    if (!std::isspace(static_cast<unsigned char>(c)))
      compact.push_back(c);
  }

  if (decodeBase64(compact, out))
    return true;

  out.assign(text.begin(), text.end());
  return true;
}

// Extract only the PGP SIGNATURE armored block from a blob of text. Returns
// false if no signature block is found so callers can fall back to the full
// content (e.g. for binary signatures).
bool extractSignatureArmor(const std::string &text, std::string &out) {
  static const std::string beginMarker = "-----BEGIN PGP SIGNATURE-----";
  static const std::string endMarker = "-----END PGP SIGNATURE-----";

  auto beginPos = text.find(beginMarker);
  if (beginPos == std::string::npos)
    return false;

  auto endPos = text.find(endMarker, beginPos);
  if (endPos == std::string::npos)
    return false;

  endPos += endMarker.size();

  out = text.substr(beginPos, endPos - beginPos);

  while (endPos < text.size() &&
         (text[endPos] == '\n' || text[endPos] == '\r')) {
    out.push_back(text[endPos]);
    ++endPos;
  }

  return true;
}

bool parsePacketHeader(const std::vector<uint8_t> &buf, std::size_t &offset,
                       uint8_t &tag, std::size_t &bodyLen,
                       std::string *errorMsg) {
  if (offset >= buf.size()) {
    if (errorMsg)
      *errorMsg = "Unexpected end of packet data";
    return false;
  }

  uint8_t hdr = buf[offset++];
  if ((hdr & 0x80u) == 0) {
    if (errorMsg)
      *errorMsg = "Invalid OpenPGP packet header";
    return false;
  }

  const bool newFormat = (hdr & 0x40u) != 0;
  if (newFormat) {
    tag = hdr & 0x3fu;
    if (offset >= buf.size()) {
      if (errorMsg)
        *errorMsg = "Truncated new-format packet";
      return false;
    }

    uint8_t lenByte = buf[offset++];
    if (lenByte < 192) {
      bodyLen = lenByte;
    } else if (lenByte < 224) {
      if (offset >= buf.size()) {
        if (errorMsg)
          *errorMsg = "Truncated 2-octet length";
        return false;
      }
      bodyLen = ((static_cast<std::size_t>(lenByte) - 192) << 8) +
                static_cast<std::size_t>(buf[offset++]) + 192;
    } else if (lenByte == 255) {
      if (offset + 4 > buf.size()) {
        if (errorMsg)
          *errorMsg = "Truncated 5-octet length";
        return false;
      }
      bodyLen = (static_cast<std::size_t>(buf[offset]) << 24) |
                (static_cast<std::size_t>(buf[offset + 1]) << 16) |
                (static_cast<std::size_t>(buf[offset + 2]) << 8) |
                static_cast<std::size_t>(buf[offset + 3]);
      offset += 4;
    } else {
      if (errorMsg)
        *errorMsg = "Partial body lengths are unsupported";
      return false;
    }
  } else {
    tag = (hdr >> 2) & 0x0fu;
    uint8_t lenType = hdr & 0x03u;

    if (lenType == 0) {
      if (offset >= buf.size()) {
        if (errorMsg)
          *errorMsg = "Truncated 1-octet length";
        return false;
      }
      bodyLen = buf[offset++];
    } else if (lenType == 1) {
      if (offset + 2 > buf.size()) {
        if (errorMsg)
          *errorMsg = "Truncated 2-octet length";
        return false;
      }
      bodyLen = (static_cast<std::size_t>(buf[offset]) << 8) |
                static_cast<std::size_t>(buf[offset + 1]);
      offset += 2;
    } else if (lenType == 2) {
      if (offset + 4 > buf.size()) {
        if (errorMsg)
          *errorMsg = "Truncated 4-octet length";
        return false;
      }
      bodyLen = (static_cast<std::size_t>(buf[offset]) << 24) |
                (static_cast<std::size_t>(buf[offset + 1]) << 16) |
                (static_cast<std::size_t>(buf[offset + 2]) << 8) |
                static_cast<std::size_t>(buf[offset + 3]);
      offset += 4;
    } else {
      if (errorMsg)
        *errorMsg = "Indeterminate lengths are unsupported";
      return false;
    }
  }

  if (offset + bodyLen > buf.size()) {
    if (errorMsg)
      *errorMsg = "Packet length exceeds available data";
    return false;
  }

  return true;
}

bool parseMpi(const std::vector<uint8_t> &buf, std::size_t &offset,
              std::vector<uint8_t> &out, std::size_t &bitLen,
              std::string *errorMsg) {
  if (offset + 2 > buf.size()) {
    if (errorMsg)
      *errorMsg = "MPI truncated";
    return false;
  }

  bitLen = (static_cast<std::size_t>(buf[offset]) << 8) |
           static_cast<std::size_t>(buf[offset + 1]);
  offset += 2;

  std::size_t byteLen = (bitLen + 7) / 8;
  if (offset + byteLen > buf.size()) {
    if (errorMsg)
      *errorMsg = "MPI length exceeds buffer";
    return false;
  }

  out.assign(buf.begin() + static_cast<std::ptrdiff_t>(offset),
             buf.begin() + static_cast<std::ptrdiff_t>(offset + byteLen));
  offset += byteLen;
  return true;
}

bool parseRsaPublicKeyPacket(const std::vector<uint8_t> &body,
                             std::vector<uint8_t> &modulus,
                             std::size_t &modulusBits,
                             std::vector<uint8_t> &exponent,
                             std::string *errorMsg) {
  std::size_t off = 0;
  if (body.size() < 6) {
    if (errorMsg)
      *errorMsg = "Public key packet too small";
    return false;
  }

  uint8_t version = body[off++];
  if (version != 4) {
    if (errorMsg)
      *errorMsg = "Only OpenPGP public key V4 is supported";
    return false;
  }

  off += 4; // creation time
  if (off >= body.size()) {
    if (errorMsg)
      *errorMsg = "Public key packet truncated after timestamp";
    return false;
  }

  uint8_t algo = body[off++];
  if (algo != 1 && algo != 2 && algo != 3) {
    if (errorMsg)
      *errorMsg = "Unsupported public key algorithm (RSA only)";
    return false;
  }

  std::size_t nBits = 0;
  if (!parseMpi(body, off, modulus, nBits, errorMsg))
    return false;

  std::size_t eBits = 0;
  if (!parseMpi(body, off, exponent, eBits, errorMsg))
    return false;

  (void)eBits;
  modulusBits = nBits;
  if (exponent.empty()) {
    if (errorMsg)
      *errorMsg = "Exponent missing from public key";
    return false;
  }

  return true;
}

bool findRsaPublicKey(const std::vector<uint8_t> &blob,
                      std::vector<uint8_t> &modulus, std::size_t &modulusBits,
                      std::vector<uint8_t> &exponent, std::string *errorMsg) {
  std::size_t off = 0;
  while (off < blob.size()) {
    uint8_t tag = 0;
    std::size_t bodyLen = 0;

    if (!parsePacketHeader(blob, off, tag, bodyLen, errorMsg))
      return false;

    std::vector<uint8_t> body(blob.begin() + static_cast<std::ptrdiff_t>(off),
                              blob.begin() +
                                  static_cast<std::ptrdiff_t>(off + bodyLen));
    off += bodyLen;

    if (tag == 6) {
      return parseRsaPublicKeyPacket(body, modulus, modulusBits, exponent,
                                     errorMsg);
    }
  }

  if (errorMsg)
    *errorMsg = "No RSA public key packet found";
  return false;
}

bool parseSignaturePacket(const std::vector<uint8_t> &blob,
                          ParsedSignature &sig, std::string *errorMsg) {
  std::size_t off = 0;
  uint8_t tag = 0;
  std::size_t bodyLen = 0;

  if (!parsePacketHeader(blob, off, tag, bodyLen, errorMsg))
    return false;

  if (tag != 2) {
    if (errorMsg)
      *errorMsg = "Detached signature does not contain a signature packet";
    return false;
  }

  if (off + bodyLen > blob.size()) {
    if (errorMsg)
      *errorMsg = "Signature packet truncated";
    return false;
  }

  std::vector<uint8_t> body(blob.begin() + static_cast<std::ptrdiff_t>(off),
                            blob.begin() +
                                static_cast<std::ptrdiff_t>(off + bodyLen));
  off += bodyLen;

  std::size_t bOff = 0;
  if (body.size() < 6) {
    if (errorMsg)
      *errorMsg = "Signature packet too small";
    return false;
  }

  uint8_t version = body[bOff++];
  if (version != 4) {
    if (errorMsg)
      *errorMsg = "Only OpenPGP signature V4 is supported";
    return false;
  }

  sig.sigType = body[bOff++];
  sig.pubKeyAlgo = body[bOff++];
  sig.hashAlgo = body[bOff++];

  uint16_t hashedLen = (static_cast<uint16_t>(body[bOff]) << 8) |
                       static_cast<uint16_t>(body[bOff + 1]);
  bOff += 2;
  if (bOff + hashedLen > body.size()) {
    if (errorMsg)
      *errorMsg = "Hashed subpacket length exceeds signature size";
    return false;
  }

  sig.hashedSubpackets.assign(
      body.begin() + static_cast<std::ptrdiff_t>(bOff),
      body.begin() + static_cast<std::ptrdiff_t>(
                         bOff + static_cast<std::size_t>(hashedLen)));
  bOff += hashedLen;

  if (bOff + 2 > body.size()) {
    if (errorMsg)
      *errorMsg = "Signature missing unhashed subpacket length";
    return false;
  }

  uint16_t unhashedLen = (static_cast<uint16_t>(body[bOff]) << 8) |
                         static_cast<uint16_t>(body[bOff + 1]);
  bOff += 2;
  if (bOff + unhashedLen > body.size()) {
    if (errorMsg)
      *errorMsg = "Unhashed subpacket length exceeds signature size";
    return false;
  }

  sig.unhashedSubpackets.assign(
      body.begin() + static_cast<std::ptrdiff_t>(bOff),
      body.begin() + static_cast<std::ptrdiff_t>(
                         bOff + static_cast<std::size_t>(unhashedLen)));
  bOff += unhashedLen;

  if (bOff + 2 > body.size()) {
    if (errorMsg)
      *errorMsg = "Signature missing hash trailer";
    return false;
  }

  sig.hashLeft16 = (static_cast<uint16_t>(body[bOff]) << 8) |
                   static_cast<uint16_t>(body[bOff + 1]);
  bOff += 2;

  std::size_t mpiBits = 0;
  if (!parseMpi(body, bOff, sig.signatureMpi, mpiBits, errorMsg))
    return false;

  (void)mpiBits;
  if (bOff != body.size()) {
    if (errorMsg)
      *errorMsg = "Unexpected data after signature MPI";
    return false;
  }

  return true;
}

std::string sha256Hex(const std::vector<uint8_t> &data) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  const unsigned char *input =
      data.empty() ? nullptr
                   : reinterpret_cast<const unsigned char *>(data.data());

  if (!SHA256(input, data.size(), digest.data()))
    return {};

  static const char *hex = "0123456789abcdef";
  std::string out;
  out.reserve(digest.size() * 2);
  for (uint8_t b : digest) {
    out.push_back(hex[b >> 4]);
    out.push_back(hex[b & 0x0f]);
  }
  return out;
}

bool modulusLengthValid(std::size_t bits) {
  return bits >= 2048 && bits <= 4096;
}

bool buildKeyFromBlob(const std::vector<uint8_t> &blob, LoadedKey &keyOut,
                      std::string &fingerprint, std::string *errorMsg) {
  fingerprint = sha256Hex(blob);
  if (fingerprint.empty()) {
    if (errorMsg)
      *errorMsg = "Failed to compute key fingerprint";
    return false;
  }
  if (blob.empty()) {
    if (errorMsg)
      *errorMsg = "Public key data is empty";
    return false;
  }

  // First try parsing as PEM/DER public key.
  {
    bssl::UniquePtr<BIO> bio(
        BIO_new_mem_buf(blob.data(), static_cast<int>(blob.size())));
    if (bio) {
      bssl::UniquePtr<EVP_PKEY> pkey(
          PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
      if (!pkey) {
        const unsigned char *ptr =
            reinterpret_cast<const unsigned char *>(blob.data());
        bssl::UniquePtr<EVP_PKEY> der(
            d2i_PUBKEY(nullptr, &ptr, static_cast<long>(blob.size())));
        if (der)
          pkey = std::move(der);
      }

      if (pkey) {
        if (EVP_PKEY_base_id(pkey.get()) != EVP_PKEY_RSA) {
          if (errorMsg)
            *errorMsg = "Public key is not RSA";
          return false;
        }

        const std::size_t bits =
            static_cast<std::size_t>(EVP_PKEY_bits(pkey.get()));
        if (!modulusLengthValid(bits)) {
          if (errorMsg)
            *errorMsg = "RSA key size outside 2048-4096 bit range";
          return false;
        }

        keyOut.pkey = std::move(pkey);
        keyOut.keyBytes = (bits + 7) / 8;
        return true;
      }
    }
  }

  std::vector<uint8_t> n;
  std::vector<uint8_t> e;
  std::size_t nBits = 0;
  if (!findRsaPublicKey(blob, n, nBits, e, errorMsg))
    return false;

  if (!modulusLengthValid(nBits)) {
    if (errorMsg)
      *errorMsg = "RSA key size outside 2048-4096 bit range";
    return false;
  }

  bssl::UniquePtr<RSA> rsa(RSA_new());
  bssl::UniquePtr<BIGNUM> N(
      BN_bin2bn(n.data(), static_cast<int>(n.size()), nullptr));
  bssl::UniquePtr<BIGNUM> E(
      BN_bin2bn(e.data(), static_cast<int>(e.size()), nullptr));
  if (!rsa || !N || !E) {
    if (errorMsg)
      *errorMsg = "Failed to allocate RSA components";
    return false;
  }

  BIGNUM *nRaw = N.release();
  BIGNUM *eRaw = E.release();

  if (RSA_set0_key(rsa.get(), nRaw, eRaw, nullptr) != 1) {
    BN_free(nRaw);
    BN_free(eRaw);
    if (errorMsg)
      *errorMsg = "Failed to import RSA key: " + formatOpenSslError();
    return false;
  }

  const BIGNUM *mod = nullptr;
  const BIGNUM *exp = nullptr;
  RSA_get0_key(rsa.get(), &mod, &exp, nullptr);
  if (!mod || !exp || BN_is_zero(mod) || BN_is_negative(mod) ||
      BN_is_zero(exp) || BN_is_negative(exp) || !BN_is_odd(exp)) {
    if (errorMsg)
      *errorMsg = "RSA key is invalid";
    return false;
  }
  if (static_cast<std::size_t>(BN_num_bits(mod)) != nBits) {
    if (errorMsg)
      *errorMsg = "RSA modulus length mismatch";
    return false;
  }

  bssl::UniquePtr<EVP_PKEY> pkey(EVP_PKEY_new());
  if (!pkey || EVP_PKEY_assign_RSA(pkey.get(), rsa.release()) != 1) {
    if (errorMsg)
      *errorMsg = "Failed to set up RSA key: " + formatOpenSslError();
    return false;
  }

  keyOut.pkey = std::move(pkey);
  keyOut.keyBytes = (nBits + 7) / 8;
  return true;
}

bool loadKeyFile(const std::string &path, LoadedKey &keyOut,
                 std::string &fingerprint, std::string *errorMsg) {
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) {
    if (errorMsg)
      *errorMsg = "Failed to open key: " + path;
    return false;
  }

  std::ostringstream ss;
  ss << in.rdbuf();
  if (!in.good() && !in.eof()) {
    if (errorMsg)
      *errorMsg = "Error reading key: " + path;
    return false;
  }

  const std::string content = ss.str();
  std::vector<uint8_t> decoded;
  bool armored = false;
  if (!decodeArmoredBlock(content, decoded, armored, errorMsg))
    return false;
  (void)armored;

  return buildKeyFromBlob(decoded, keyOut, fingerprint, errorMsg);
}

bool normalizeSignature(const std::vector<uint8_t> &mpi, std::size_t keyBytes,
                        std::vector<uint8_t> &out, std::string *errorMsg) {
  out.clear();
  if (keyBytes == 0) {
    if (errorMsg)
      *errorMsg = "Invalid RSA modulus length";
    return false;
  }

  if (mpi.size() > keyBytes) {
    if (errorMsg)
      *errorMsg = "Signature length exceeds RSA modulus length";
    return false;
  }

  out.assign(keyBytes - mpi.size(), 0);
  out.insert(out.end(), mpi.begin(), mpi.end());
  return true;
}

bool hashSignedData(const ParsedSignature &sig, const std::string &dataPath,
                    std::vector<uint8_t> &digest, std::string *errorMsg) {
  bssl::UniquePtr<EVP_MD_CTX> ctx(EVP_MD_CTX_new());
  if (!ctx) {
    if (errorMsg)
      *errorMsg = "SHA256 init failed: unable to allocate context";
    return false;
  }

  digest.assign(SHA256_DIGEST_LENGTH, 0);

  if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) {
    if (errorMsg)
      *errorMsg = "SHA256 init failed: " + formatOpenSslError();
    return false;
  }

  std::ifstream in(dataPath, std::ios::binary);
  if (!in.is_open()) {
    if (errorMsg)
      *errorMsg = "Failed to open signed data: " + dataPath;
    return false;
  }

  std::array<char, 4096> buf{};
  while (in.good()) {
    in.read(buf.data(), buf.size());
    std::streamsize got = in.gcount();
    if (got > 0) {
      if (EVP_DigestUpdate(ctx.get(),
                           reinterpret_cast<const unsigned char *>(buf.data()),
                           static_cast<std::size_t>(got)) != 1) {
        if (errorMsg)
          *errorMsg = "SHA256 update failed: " + formatOpenSslError();
        return false;
      }
    }
  }

  if (!in.eof()) {
    if (errorMsg)
      *errorMsg = "Failed reading signed data: " + dataPath;
    return false;
  }

  std::vector<uint8_t> header;
  header.reserve(6 + sig.hashedSubpackets.size());
  header.push_back(0x04);
  header.push_back(sig.sigType);
  header.push_back(sig.pubKeyAlgo);
  header.push_back(sig.hashAlgo);
  uint16_t hashedLen = static_cast<uint16_t>(sig.hashedSubpackets.size());
  header.push_back(static_cast<uint8_t>(hashedLen >> 8));
  header.push_back(static_cast<uint8_t>(hashedLen & 0xff));
  header.insert(header.end(), sig.hashedSubpackets.begin(),
                sig.hashedSubpackets.end());

  if (EVP_DigestUpdate(ctx.get(), header.data(), header.size()) != 1) {
    if (errorMsg)
      *errorMsg = "SHA256 header update failed: " + formatOpenSslError();
    return false;
  }

  uint32_t trailerLen = static_cast<uint32_t>(header.size());
  uint8_t trailer[6] = {0x04,
                        0xff,
                        static_cast<uint8_t>((trailerLen >> 24) & 0xff),
                        static_cast<uint8_t>((trailerLen >> 16) & 0xff),
                        static_cast<uint8_t>((trailerLen >> 8) & 0xff),
                        static_cast<uint8_t>(trailerLen & 0xff)};

  if (EVP_DigestUpdate(ctx.get(), trailer, sizeof(trailer)) != 1) {
    if (errorMsg)
      *errorMsg = "SHA256 trailer update failed: " + formatOpenSslError();
    return false;
  }

  unsigned int outLen = 0;
  if (EVP_DigestFinal_ex(ctx.get(), digest.data(), &outLen) != 1) {
    if (errorMsg)
      *errorMsg = "SHA256 finalize failed: " + formatOpenSslError();
    return false;
  }

  digest.resize(outLen);
  return true;
}

bool verifyWithKey(LoadedKey &key, const std::vector<uint8_t> &digest,
                   const std::vector<uint8_t> &sigBytes,
                   std::string *errorMsg) {
  if (!key.pkey) {
    if (errorMsg)
      *errorMsg = "RSA key not loaded";
    return false;
  }

  const RSA *rsa = EVP_PKEY_get0_RSA(key.pkey.get());
  if (!rsa) {
    if (errorMsg)
      *errorMsg = "Loaded key is not RSA";
    return false;
  }

  const size_t rsaSize = static_cast<std::size_t>(RSA_size(rsa));
  if (sigBytes.size() != rsaSize) {
    if (errorMsg)
      *errorMsg = "Signature length does not match RSA modulus length";
    return false;
  }

  // Decrypt the signature with the public key using PKCS#1 v1.5 padding to
  // recover the DigestInfo and then verify it matches the SHA-256 digest.
  std::vector<uint8_t> recovered(rsaSize, 0);
  int recLen = RSA_public_decrypt(
      static_cast<int>(sigBytes.size()),
      reinterpret_cast<const unsigned char *>(sigBytes.data()),
      recovered.data(), const_cast<RSA *>(rsa), RSA_PKCS1_PADDING);
  if (recLen <= 0) {
    if (errorMsg)
      *errorMsg = "RSA_public_decrypt failed: " + formatOpenSslError();
    return false;
  }

  // ASN.1 DigestInfo prefix for SHA-256 (DER):
  // 30 31 30 0d 06 09 60 86 48 01 65 03 04 02 01 05 00 04 20
  static const uint8_t sha256_prefix[] = {
      0x30, 0x31, 0x30, 0x0d, 0x06, 0x09, 0x60, 0x86, 0x48, 0x01,
      0x65, 0x03, 0x04, 0x02, 0x01, 0x05, 0x00, 0x04, 0x20};
  const size_t prefix_len = sizeof(sha256_prefix);
  const size_t expected_len = prefix_len + SHA256_DIGEST_LENGTH;

  if (static_cast<std::size_t>(recLen) != expected_len) {
    if (errorMsg)
      *errorMsg = "Decrypted signature has unexpected DigestInfo length";
    return false;
  }

  if (!std::equal(sha256_prefix, sha256_prefix + prefix_len,
                  recovered.data())) {
    if (errorMsg)
      *errorMsg = "Decrypted signature DigestInfo prefix mismatch";
    return false;
  }

  if (!std::equal(digest.begin(), digest.end(),
                  recovered.data() + static_cast<std::ptrdiff_t>(prefix_len))) {
    if (errorMsg)
      *errorMsg = "Decrypted signature digest mismatch";
    return false;
  }

  return true;
}

} // namespace

bool verifyDetachedSignature(const std::string &dataPath,
                             const std::string &sigPath,
                             const std::string &trustedKeysDir,
                             std::string *errorMsg) {
  std::ifstream sigFile(sigPath, std::ios::binary);
  if (!sigFile.is_open()) {
    if (errorMsg)
      *errorMsg = "Failed to open signature: " + sigPath;
    return false;
  }

  std::ostringstream sigStream;
  sigStream << sigFile.rdbuf();
  if (!sigFile.good() && !sigFile.eof()) {
    if (errorMsg)
      *errorMsg = "Error reading signature: " + sigPath;
    return false;
  }

  std::string sigText = sigStream.str();

  auto extPos = sigPath.find_last_of('.');
  if (extPos != std::string::npos) {
    std::string ext = sigPath.substr(extPos);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });

    if (ext == ".gpg") {
      std::string sliced;
      if (extractSignatureArmor(sigText, sliced)) {
        sigText.swap(sliced);
      }
    }
  }

  std::vector<uint8_t> sigBlob;
  bool sigArmored = false;
  if (!decodeArmoredBlock(sigText, sigBlob, sigArmored, errorMsg))
    return false;
  (void)sigArmored;

  ParsedSignature sig;
  if (!parseSignaturePacket(sigBlob, sig, errorMsg))
    return false;

  if (sig.hashAlgo != 8) {
    if (errorMsg)
      *errorMsg = "Unsupported hash algorithm (SHA256 required)";
    return false;
  }

  if (sig.pubKeyAlgo != 1 && sig.pubKeyAlgo != 3) {
    if (errorMsg)
      *errorMsg = "Unsupported signature algorithm (RSA only)";
    return false;
  }

  std::vector<uint8_t> digest;
  if (!hashSignedData(sig, dataPath, digest, errorMsg))
    return false;

  uint16_t digestLeft = (static_cast<uint16_t>(digest[0]) << 8) |
                        static_cast<uint16_t>(digest[1]);
  if (digestLeft != sig.hashLeft16) {
    if (errorMsg)
      *errorMsg = "Signature hash prefix mismatch";
    return false;
  }

  if (!apm::fs::isDirectory(trustedKeysDir)) {
    if (errorMsg)
      *errorMsg = "Trusted keys directory missing: " + trustedKeysDir;
    return false;
  }

  bool anyKey = false;
  std::string lastErr;

  auto entries = apm::fs::listDir(trustedKeysDir, false);
  for (const auto &entryName : entries) {
    if (entryName.empty() || entryName == "." || entryName == "..")
      continue;

    std::string path = apm::fs::joinPath(trustedKeysDir, entryName);
    if (!apm::fs::isRegularFile(path))
      continue;

    if (!isTrustedKeyFile(path))
      continue;

    anyKey = true;
    LoadedKey key;
    std::string fingerprint;
    std::string keyErr;

    if (!loadKeyFile(path, key, fingerprint, &keyErr)) {
      apm::logger::warn("Skipping key " + path + ": " + keyErr);
      lastErr = keyErr;
      continue;
    }

    std::vector<uint8_t> sigPadded;
    if (!normalizeSignature(sig.signatureMpi, key.keyBytes, sigPadded,
                            &keyErr)) {
      apm::logger::warn("Skipping key " + path + ": " + keyErr);
      lastErr = keyErr;
      continue;
    }

    if (verifyWithKey(key, digest, sigPadded, &keyErr)) {
      apm::logger::info("Signature verified with key " + fingerprint);
      return true;
    }

    apm::logger::warn("Signature verification failed with key " + fingerprint +
                      ": " + keyErr);
    lastErr = keyErr;
  }

  if (!anyKey) {
    if (errorMsg)
      *errorMsg = "No trusted keys found in " + trustedKeysDir;
    return false;
  }

  if (errorMsg) {
    if (!lastErr.empty())
      *errorMsg = "Signature did not match any trusted key: " + lastErr;
    else
      *errorMsg = "Signature did not match any trusted key";
  }

  return false;
}

bool importTrustedPublicKey(const std::string &ascPath,
                            const std::string &trustedKeysDir,
                            std::string *fingerprintOut,
                            std::string *storedPathOut, std::string *errorMsg) {
  std::ifstream in(ascPath, std::ios::binary);
  if (!in.is_open()) {
    if (errorMsg)
      *errorMsg = "Failed to open key file: " + ascPath;
    return false;
  }

  std::ostringstream ss;
  ss << in.rdbuf();
  if (!in.good() && !in.eof()) {
    if (errorMsg)
      *errorMsg = "Failed to read key file: " + ascPath;
    return false;
  }

  const std::string content = ss.str();

  std::vector<uint8_t> decoded;
  bool armored = false;
  if (!decodeArmoredBlock(content, decoded, armored, errorMsg))
    return false;
  (void)armored;

  LoadedKey key;
  std::string fingerprint;
  if (!buildKeyFromBlob(decoded, key, fingerprint, errorMsg))
    return false;

  std::string dest = apm::fs::joinPath(
      trustedKeysDir, armored ? fingerprint + ".asc" : fingerprint + ".gpg");

  if (!apm::fs::createDirs(trustedKeysDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create trusted keys directory: " + trustedKeysDir;
    return false;
  }

  if (!apm::fs::writeFile(dest, content, false)) {
    if (errorMsg)
      *errorMsg = "Failed to store trusted key at " + dest;
    return false;
  }

  if (fingerprintOut)
    *fingerprintOut = fingerprint;
  if (storedPathOut)
    *storedPathOut = dest;

  return true;
}

} // namespace apm::crypto
