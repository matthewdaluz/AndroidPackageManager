/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: te_parser.hpp
 * Purpose: Declare a tiny .te parser for apm-policy.
 * Last Modified: February 18th, 2025.
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
#include <vector>

namespace apm::selinux {

struct TeRule {
  enum class Kind { Type, TypeAttribute, Allow, DontAudit };

  Kind kind{};
  std::string lhs;
  std::string rhs;
  std::string tclass;
  std::vector<std::string> perms;
};

struct TePolicy {
  std::vector<TeRule> rules;
  std::vector<std::string> warnings;
};

bool parseTeFile(const std::string &path, TePolicy &out, std::string &error);

} // namespace apm::selinux

