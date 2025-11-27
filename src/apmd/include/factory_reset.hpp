/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: factory_reset.hpp
 * Purpose: Declare daemon-side factory reset helpers that wipe APM data,
 * installed content, and system app overlays.
 * Last Modified: November 27th, 2025. - 11:30 AM Eastern Time.
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

#include "ams/module_manager.hpp"

#include <string>

namespace apm::daemon {

struct FactoryResetResult {
  bool ok = false;
  std::string message;
};

bool performFactoryReset(apm::ams::ModuleManager &moduleManager,
                         FactoryResetResult &out);

} // namespace apm::daemon
