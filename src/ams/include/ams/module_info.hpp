/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: module_info.hpp
 * Purpose: Declare AMS module metadata/state structures and JSON helpers.
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

namespace apm::ams {

struct ModuleInfo {
  std::string name;
  std::string version;
  std::string author;
  std::string description;
  bool mount = true;
  bool runPostFsData = false;
  bool runService = false;
};

struct ModuleState {
  bool enabled = false;
  std::string installedAt;
  std::string updatedAt;
  std::string lastError;
};

bool parseModuleInfo(const std::string &json, ModuleInfo &info,
                     std::string *errorMsg = nullptr);
bool readModuleInfoFile(const std::string &path, ModuleInfo &info,
                        std::string *errorMsg = nullptr);

bool readModuleState(const std::string &path, ModuleState &state,
                     std::string *errorMsg = nullptr);
bool writeModuleState(const std::string &path, const ModuleState &state,
                      std::string *errorMsg = nullptr);

std::string makeIsoTimestamp();

} // namespace apm::ams

