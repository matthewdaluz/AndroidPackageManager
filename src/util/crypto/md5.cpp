/*
 * APM - Android Package Manager
 *
 * File: md5.cpp
 * Purpose: Implement a simple MD5 file hashing helper using BoringSSL.
 * Last Modified: 2026-03-15 11:56:16.542846254 -0400.
 */

#include "md5.hpp"

#include <openssl/md5.h>

#include <array>
#include <cstdio>
#include <vector>

namespace apm::crypto {

static std::string toHex(const uint8_t *data, size_t len) {
  static const char *digits = "0123456789abcdef";
  std::string out;
  out.resize(len * 2);
  for (size_t i = 0; i < len; ++i) {
    out[2 * i] = digits[(data[i] >> 4) & 0xF];
    out[2 * i + 1] = digits[data[i] & 0xF];
  }
  return out;
}

bool md5File(const std::string &path, std::string &outHex,
             std::string *errorMsg) {
  FILE *fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    if (errorMsg)
      *errorMsg = "Could not open file for MD5: " + path;
    return false;
  }

  MD5_CTX ctx;
  MD5_Init(&ctx);

  std::vector<uint8_t> buf(16 * 1024);
  while (true) {
    size_t n = std::fread(buf.data(), 1, buf.size(), fp);
    if (n > 0) {
      MD5_Update(&ctx, buf.data(), n);
    }
    if (n < buf.size()) {
      if (std::ferror(fp)) {
        if (errorMsg)
          *errorMsg = "Read error during MD5: " + path;
        std::fclose(fp);
        return false;
      }
      break;
    }
  }

  std::array<uint8_t, MD5_DIGEST_LENGTH> digest{};
  MD5_Final(digest.data(), &ctx);
  std::fclose(fp);

  outHex = toHex(digest.data(), digest.size());
  return true;
}

} // namespace apm::crypto
