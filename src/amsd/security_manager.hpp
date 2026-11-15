/*
 * AMSD - APM Module System Daemon
 *
 * Read-only security helper that validates shared session tokens issued by
 * apmd. Token issuance/reset flows remain owned by apmd.
 */

/*
 * APM - Android Package Manager
 *
 * Skidworks Open-Engineering - Software Branch
 * Copyright (C) 2027 Skidworks Open-Engineering
 *
 * File: security_manager.hpp
 * Purpose: Declare the AMSD security helper that validates shared session
 *          tokens issued by apmd.
 * Last Modified: November 15th, 2026. - 09:14 AM EST.
 * Author: Matthew DaLuz - Skidworks Founder
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

#include "boringssl_client.hpp"
#include "security.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace apm::amsd {

class SecurityManager {
public:
  SecurityManager();

  bool validateSessionToken(const std::string &token,
                            std::string *errorMsg = nullptr);

private:
  bool deriveHmac(const apm::security::SessionState &state, std::string &out,
                  std::string *errorMsg);
  static std::string bytesToHex(const std::vector<uint8_t> &data);

  apm::daemon::CryptoEngine crypto_;
};

} // namespace apm::amsd
