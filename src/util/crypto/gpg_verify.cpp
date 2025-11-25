/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: gpg_verify.cpp
 * Purpose: Implement OpenPGP RSA/SHA256 detached signature verification and key import helpers.
 * Last Modified: November 25th, 2025. - 11:35 AM Eastern Time.
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
#include "sha256.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <utility>
#include <vector>

#include <mbedtls/base64.h>
#include <mbedtls/error.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>

namespace apm::crypto {

namespace {

// RAII wrapper for mbedTLS PK contexts to avoid leaking allocated buffers.
struct LoadedKey {
  LoadedKey() { mbedtls_pk_init(&ctx); }
  ~LoadedKey() { mbedtls_pk_free(&ctx); }
  LoadedKey(const LoadedKey &) = delete;
  LoadedKey &operator=(const LoadedKey &) = delete;

  mbedtls_pk_context ctx;
  std::size_t keyBytes = 0;
  std::string fingerprint;
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

std::string formatMbedError(int code) {
  char buf[128] = {0};
  mbedtls_strerror(code, buf, sizeof(buf));
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

  size_t needed = 0;
  int rc = mbedtls_base64_decode(nullptr, 0, &needed,
                                 reinterpret_cast<const unsigned char *>(
                                     b64.data()),
                                 b64.size());
  if (rc != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL || needed == 0)
    return false;

  out.resize(needed);
  rc = mbedtls_base64_decode(out.data(), out.size(), &needed,
                             reinterpret_cast<const unsigned char *>(
                                 b64.data()),
                             b64.size());
  if (rc != 0)
    return false;

  out.resize(needed);
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
                      std::vector<uint8_t> &modulus,
                      std::size_t &modulusBits, std::vector<uint8_t> &exponent,
                      std::string *errorMsg) {
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
      body.begin() +
          static_cast<std::ptrdiff_t>(bOff + static_cast<std::size_t>(hashedLen)));
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
      body.begin() +
          static_cast<std::ptrdiff_t>(bOff + static_cast<std::size_t>(unhashedLen)));
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
  SHA256 ctx;
  if (!data.empty())
    ctx.update(data.data(), data.size());
  return ctx.finalHex();
}

bool modulusLengthValid(std::size_t bits) {
  return bits >= 2048 && bits <= 4096;
}

bool buildKeyFromBlob(const std::vector<uint8_t> &blob, LoadedKey &keyOut,
                      std::string &fingerprint, std::string *errorMsg) {
  fingerprint = sha256Hex(blob);

  mbedtls_pk_context parsed;
  mbedtls_pk_init(&parsed);
  int rc = mbedtls_pk_parse_public_key(&parsed, blob.data(), blob.size());
  if (rc == 0) {
    if (mbedtls_pk_get_type(&parsed) != MBEDTLS_PK_RSA) {
      if (errorMsg)
        *errorMsg = "Public key is not RSA";
      mbedtls_pk_free(&parsed);
      return false;
    }

    std::size_t bits = mbedtls_pk_get_bitlen(&parsed);
    if (!modulusLengthValid(bits)) {
      if (errorMsg)
        *errorMsg = "RSA key size outside 2048-4096 bit range";
      mbedtls_pk_free(&parsed);
      return false;
    }

    mbedtls_pk_free(&keyOut.ctx);
    std::swap(keyOut.ctx, parsed);
    keyOut.keyBytes = (bits + 7) / 8;
    mbedtls_pk_free(&parsed);
    return true;
  }

  mbedtls_pk_free(&parsed);

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

  if (mbedtls_pk_setup(&keyOut.ctx,
                       mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) {
    if (errorMsg)
      *errorMsg = "Failed to initialize RSA context";
    return false;
  }

  mbedtls_rsa_context *rsa = mbedtls_pk_rsa(keyOut.ctx);
  rc = mbedtls_rsa_import_raw(rsa, n.data(), n.size(), nullptr, 0, nullptr, 0,
                              e.data(), e.size(), nullptr, 0);
  if (rc != 0) {
    if (errorMsg)
      *errorMsg = "Failed to import RSA key: " + formatMbedError(rc);
    return false;
  }

  rc = mbedtls_rsa_complete(rsa);
  if (rc != 0) {
    if (errorMsg)
      *errorMsg = "Incomplete RSA key: " + formatMbedError(rc);
    return false;
  }

  mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_SHA256);
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
  digest.assign(32, 0);

  const mbedtls_md_info_t *info =
      mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) {
    if (errorMsg)
      *errorMsg = "SHA256 not available in mbedTLS";
    return false;
  }

  mbedtls_md_context_t md;
  mbedtls_md_init(&md);

  int rc = mbedtls_md_setup(&md, info, 0);
  if (rc != 0) {
    if (errorMsg)
      *errorMsg = "SHA256 setup failed: " + formatMbedError(rc);
    mbedtls_md_free(&md);
    return false;
  }

  rc = mbedtls_md_starts(&md);
  if (rc != 0) {
    if (errorMsg)
      *errorMsg = "SHA256 init failed: " + formatMbedError(rc);
    mbedtls_md_free(&md);
    return false;
  }

  std::ifstream in(dataPath, std::ios::binary);
  if (!in.is_open()) {
    if (errorMsg)
      *errorMsg = "Failed to open signed data: " + dataPath;
    mbedtls_md_free(&md);
    return false;
  }

  std::array<char, 4096> buf{};
  while (in.good()) {
    in.read(buf.data(), buf.size());
    std::streamsize got = in.gcount();
    if (got > 0) {
      rc = mbedtls_md_update(
          &md, reinterpret_cast<const unsigned char *>(buf.data()),
          static_cast<std::size_t>(got));
      if (rc != 0) {
        if (errorMsg)
          *errorMsg = "SHA256 update failed: " + formatMbedError(rc);
        mbedtls_md_free(&md);
        return false;
      }
    }
  }

  if (!in.eof()) {
    if (errorMsg)
      *errorMsg = "Failed reading signed data: " + dataPath;
    mbedtls_md_free(&md);
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

  rc = mbedtls_md_update(&md, header.data(), header.size());
  if (rc != 0) {
    if (errorMsg)
      *errorMsg = "SHA256 header update failed: " + formatMbedError(rc);
    mbedtls_md_free(&md);
    return false;
  }

  uint32_t trailerLen = static_cast<uint32_t>(header.size());
  uint8_t trailer[6] = {0x04, 0xff,
                        static_cast<uint8_t>((trailerLen >> 24) & 0xff),
                        static_cast<uint8_t>((trailerLen >> 16) & 0xff),
                        static_cast<uint8_t>((trailerLen >> 8) & 0xff),
                        static_cast<uint8_t>(trailerLen & 0xff)};

  rc = mbedtls_md_update(&md, trailer, sizeof(trailer));
  if (rc != 0) {
    if (errorMsg)
      *errorMsg = "SHA256 trailer update failed: " + formatMbedError(rc);
    mbedtls_md_free(&md);
    return false;
  }

  rc = mbedtls_md_finish(&md, digest.data());
  mbedtls_md_free(&md);

  if (rc != 0) {
    if (errorMsg)
      *errorMsg = "SHA256 finalize failed: " + formatMbedError(rc);
    return false;
  }

  return true;
}

bool verifyWithKey(LoadedKey &key, const std::vector<uint8_t> &digest,
                   const std::vector<uint8_t> &sigBytes,
                   std::string *errorMsg) {
  int rc = mbedtls_pk_verify(&key.ctx, MBEDTLS_MD_SHA256, digest.data(),
                             digest.size(), sigBytes.data(), sigBytes.size());
  if (rc != 0) {
    if (errorMsg)
      *errorMsg = formatMbedError(rc);
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

  std::vector<uint8_t> sigBlob;
  bool sigArmored = false;
  if (!decodeArmoredBlock(sigStream.str(), sigBlob, sigArmored, errorMsg))
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

  std::string dest =
      apm::fs::joinPath(trustedKeysDir, armored ? fingerprint + ".asc"
                                                : fingerprint + ".gpg");

  if (!apm::fs::createDirs(trustedKeysDir)) {
    if (errorMsg)
      *errorMsg = "Failed to create trusted keys directory: " +
                  trustedKeysDir;
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
