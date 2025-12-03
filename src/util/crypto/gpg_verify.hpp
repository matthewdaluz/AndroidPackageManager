/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: gpg_verify.hpp
 * Purpose: Declare helpers for verifying detached OpenPGP signatures and
 * importing trusted keys. Last Modified: November 23rd, 2025. - 2:52 PM Eastern
 * Time. Author: Matthew DaLuz - RedHead Founder
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

// Verify a detached OpenPGP signature using RSA/SHA256:
//
//   sigPath is a detached signature (Release.gpg)
//   dataPath is the signed file (Release)
//   trustedKeysDir contains trusted public keys (*.asc or *.gpg)
//
// Returns true if the signature verifies against at least one trusted key;
// false otherwise. errorMsg is optional. If fingerprintOut is provided and
// verification succeeds, it is filled with the fingerprint of the key used.
bool verifyDetachedSignature(const std::string &dataPath,
                             const std::string &sigPath,
                             const std::string &trustedKeysDir,
                             std::string *errorMsg = nullptr,
                             std::string *fingerprintOut = nullptr);

// Verify a clearsigned OpenPGP Release (InRelease) file. On success, returns
// true and fills outCleartext with the decoded, dash-unescaped cleartext
// body that was signed. The returned text uses '\n' newlines and is suitable
// for feeding into parseReleaseText().
bool verifyClearsignedRelease(const std::string &inReleasePath,
                              const std::string &trustedKeysDir,
                              std::string &outCleartext,
                              std::string *errorMsg = nullptr);

// Import a trusted ASCII-armored or binary public key into trustedKeysDir. The
// key is validated to be an RSA V4 OpenPGP public key (2048–4096 bits). The
// stored filename is the lowercase SHA256 fingerprint of the decoded key data
// with an extension that reflects the original format (".asc" for armored,
// ".gpg" for binary).
bool importTrustedPublicKey(const std::string &ascPath,
                            const std::string &trustedKeysDir,
                            std::string *fingerprintOut = nullptr,
                            std::string *storedPathOut = nullptr,
                            std::string *errorMsg = nullptr);

} // namespace apm::crypto
