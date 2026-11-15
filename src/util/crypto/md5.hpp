/*
 * APM - Android Package Manager
 *
 * File: md5.hpp
 * Purpose: Provide a simple MD5 file hashing helper using BoringSSL.
 * Last Modified: November 15th, 2026. - 09:14 AM EST.
 */

#pragma once

#include <string>

namespace apm::crypto {

// Compute MD5 of an entire file at |path| and return hex (lowercase) in
// |outHex|. Returns true on success; false on failure with an optional error
// message.
bool md5File(const std::string &path, std::string &outHex,
             std::string *errorMsg = nullptr);

} // namespace apm::crypto
