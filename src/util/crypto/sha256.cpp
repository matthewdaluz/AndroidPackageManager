/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: sha256.cpp
 * Purpose: Implement the SHA-256 digest context plus a convenience file hashing helper.
 * Last Modified: 2026-03-15 11:56:16.542846254 -0400.
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

#include "sha256.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>

namespace apm::crypto {

static constexpr uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

// Helpers
static inline uint32_t rotr(uint32_t x, uint32_t n) {
  return (x >> n) | (x << (32 - n));
}

SHA256::SHA256() : m_bitLen(0), m_bufferSize(0) {
  m_state[0] = 0x6a09e667u;
  m_state[1] = 0xbb67ae85u;
  m_state[2] = 0x3c6ef372u;
  m_state[3] = 0xa54ff53au;
  m_state[4] = 0x510e527fu;
  m_state[5] = 0x9b05688cu;
  m_state[6] = 0x1f83d9abu;
  m_state[7] = 0x5be0cd19u;
}

// Absorb bytes into the SHA-256 state, processing 64-byte blocks as soon as
// they are available.
void SHA256::update(const uint8_t *data, size_t len) {
  m_bitLen += static_cast<uint64_t>(len) * 8;

  while (len > 0) {
    size_t toCopy = std::min(len, 64 - m_bufferSize);
    std::memcpy(m_buffer + m_bufferSize, data, toCopy);
    m_bufferSize += toCopy;
    data += toCopy;
    len -= toCopy;

    if (m_bufferSize == 64) {
      processBlock(m_buffer);
      m_bufferSize = 0;
    }
  }
}

void SHA256::update(const std::string &data) {
  update(reinterpret_cast<const uint8_t *>(data.data()), data.size());
}

// Compress a single 64-byte message block into the running hash state.
void SHA256::processBlock(const uint8_t *block) {
  uint32_t w[64];

  // Load block into message schedule
  for (int i = 0; i < 16; i++) {
    w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
           (uint32_t(block[i * 4 + 2]) << 8) | (uint32_t(block[i * 4 + 3]));
  }

  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  uint32_t a = m_state[0];
  uint32_t b = m_state[1];
  uint32_t c = m_state[2];
  uint32_t d = m_state[3];
  uint32_t e = m_state[4];
  uint32_t f = m_state[5];
  uint32_t g = m_state[6];
  uint32_t h = m_state[7];

  for (int i = 0; i < 64; i++) {
    uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    uint32_t ch = (e & f) ^ ((~e) & g);
    uint32_t temp1 = h + S1 + ch + K[i] + w[i];
    uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temp2 = S0 + maj;

    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  m_state[0] += a;
  m_state[1] += b;
  m_state[2] += c;
  m_state[3] += d;
  m_state[4] += e;
  m_state[5] += f;
  m_state[6] += g;
  m_state[7] += h;
}

// Finalize the hash and return the lowercase hexadecimal digest.
std::string SHA256::finalHex() {
  uint8_t pad[64] = {0x80};
  size_t padLen;

  size_t mod = m_bufferSize % 64;
  if (mod < 56)
    padLen = 56 - mod;
  else
    padLen = 64 + 56 - mod;

  update(pad, padLen);

  uint8_t lenBytes[8];
  for (int i = 0; i < 8; i++) {
    lenBytes[7 - i] = (m_bitLen >> (i * 8)) & 0xff;
  }
  update(lenBytes, 8);

  // Convert state to hex string
  char hex[65];
  for (int i = 0; i < 8; i++) {
    std::sprintf(hex + i * 8, "%08x", m_state[i]);
  }
  hex[64] = '\0';

  return std::string(hex, 64);
}

// Convenience helper that hashes a file in chunks and returns the digest.
bool sha256File(const std::string &path, std::string &outHex,
                std::string *err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err)
      *err = "Could not open file for SHA256: " + path;
    return false;
  }

  SHA256 ctx;
  char buf[4096];

  while (in.good()) {
    in.read(buf, sizeof(buf));
    std::streamsize n = in.gcount();
    if (n > 0)
      ctx.update(reinterpret_cast<uint8_t *>(buf), size_t(n));
  }

  if (!in.eof()) {
    if (err)
      *err = "Read error during SHA256: " + path;
    return false;
  }

  outHex = ctx.finalHex();
  return true;
}

} // namespace apm::crypto
