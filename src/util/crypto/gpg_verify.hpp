/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: gpg_verify.hpp
 * Purpose: Declare helpers for verifying detached OpenPGP signatures.
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

#include <string>

namespace apm::crypto {

// Verify a detached OpenPGP signature:
//
//   sigPath is a detached signature (Release.gpg)
//   dataPath is the signed file (Release)
//   trustedKeysDir contains one or more *.gpg keyring files.
//
// Returns true if signature verifies against at least one keyring;
// false otherwise. errorMsg is optional.
bool verifyDetachedSignature(const std::string &dataPath,
                             const std::string &sigPath,
                             const std::string &trustedKeysDir,
                             std::string *errorMsg = nullptr);

} // namespace apm::crypto
