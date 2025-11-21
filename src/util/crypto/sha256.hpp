/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: sha256.hpp
 * Purpose: Declare the SHA-256 helper class and related file hashing helpers.
 * Last Modified: November 18th, 2025. - 3:00 PM Eastern Time.
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

#include <cstdint>
#include <string>

namespace apm::crypto {

class SHA256 {
public:
  SHA256();
  void update(const uint8_t *data, size_t len);
  void update(const std::string &data);
  std::string finalHex(); // returns lowercase hex string

private:
  void processBlock(const uint8_t *block);

  uint64_t m_bitLen;
  uint32_t m_state[8];
  uint8_t m_buffer[64];
  size_t m_bufferSize;
};

// Convenience helper to compute SHA256 of an entire file
bool sha256File(const std::string &path, std::string &outHex, std::string *err);

} // namespace apm::crypto
