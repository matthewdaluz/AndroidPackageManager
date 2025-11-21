/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: gpg_verify.cpp
 * Purpose: Implement the (currently disabled) GPG signature verification stub.
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

#include "gpg_verify.hpp"

#include "logger.hpp"
#include <string>

namespace apm::crypto {

// NOTICE: GPG SIGNATURE VERIFICATION DISABLED
// REASON: Verification via GPG is currently broken. We need to rework GPG
// verification.
//         In the meantime, disable it to avoid breaking APM entirely.
//
// APM will accept all Release files even if:
//   - no keyring exists
//   - gpgv is missing
//   - the signature is invalid
//
// SHA256 verification (Release -> Packages -> .deb) still works normally.
// APM remains functional until gpgv can be bundled later.

bool verifyDetachedSignature(const std::string &dataPath,
                             const std::string &sigPath,
                             const std::string &trustedKeysDir,
                             std::string *errorMsg) {
  apm::logger::warn("GPG signature verification DISABLED: accepting "
                    "Release file without verification");

  if (errorMsg) {
    *errorMsg = "GPG verification disabled";
  }

  // Always succeed
  return true;
}

} // namespace apm::crypto
