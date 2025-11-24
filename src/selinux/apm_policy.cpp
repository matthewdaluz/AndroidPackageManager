/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: apm_policy.cpp
 * Purpose: Entry point for apm-policy, a minimal SELinux policy injector.
 *
 * Last Modified: November 24th, 2025. - 10:20 AM Eastern Time
 *
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

#include "logger.hpp"
#include "policy_parser.hpp"
#include "te_parser.hpp"

#include <iostream>
#include <string>
#include <vector>

using apm::logger::error;
using apm::logger::info;
using apm::logger::warn;

namespace {

const char *kDefaultPolicyPath = "/sys/fs/selinux/policy";
const char *kLogPath = "/data/apm/logs/policy.log";

void printUsage(const char *argv0) {
  std::cerr << "Usage: " << argv0 << " <rules.te> [policy_path]\n";
}

} // namespace

int main(int argc, char **argv) {
  apm::logger::setLogFile(kLogPath);
  apm::logger::enableStderr(true);

  if (argc < 2 || argc > 3) {
    printUsage(argv[0]);
    return 1;
  }

  std::string tePath = argv[1];
  std::string policyPath = (argc == 3) ? argv[2] : kDefaultPolicyPath;

  info("apm-policy starting");
  info("Loading rules from " + tePath);

  apm::selinux::TePolicy tePolicy;
  std::string errorMsg;
  if (!apm::selinux::parseTeFile(tePath, tePolicy, errorMsg)) {
    error("Failed to parse .te file: " + errorMsg);
    return 1;
  }

  for (const auto &warnMsg : tePolicy.warnings) {
    warn("Parser warning: " + warnMsg);
  }

  apm::selinux::PolicyParser parser;
  if (!parser.load(policyPath, errorMsg)) {
    error("Failed to load policy: " + errorMsg);
    return 1;
  }

  info("Applying " + std::to_string(tePolicy.rules.size()) + " rule(s)");

  for (const auto &rule : tePolicy.rules) {
    switch (rule.kind) {
    case apm::selinux::TeRule::Kind::Type:
      if (!parser.ensureType(rule.lhs, false, errorMsg)) {
        error("Failed to add type " + rule.lhs + ": " + errorMsg);
        return 1;
      }
      info("Ensured type " + rule.lhs);
      break;
    case apm::selinux::TeRule::Kind::TypeAttribute:
      if (!parser.assignAttribute(rule.lhs, rule.rhs, errorMsg)) {
        error("Failed to assign attribute " + rule.rhs + " to " + rule.lhs +
              ": " + errorMsg);
        return 1;
      }
      info("Assigned attribute " + rule.rhs + " to " + rule.lhs);
      break;
    case apm::selinux::TeRule::Kind::Allow:
      if (!parser.addAvRule(rule.lhs, rule.rhs, rule.tclass, rule.perms, false,
                            errorMsg)) {
        error("Failed to add allow rule: " + errorMsg);
        return 1;
      }
      info("Added allow " + rule.lhs + " " + rule.rhs + ":" + rule.tclass);
      break;
    case apm::selinux::TeRule::Kind::DontAudit:
      if (!parser.addAvRule(rule.lhs, rule.rhs, rule.tclass, rule.perms, true,
                            errorMsg)) {
        error("Failed to add dontaudit rule: " + errorMsg);
        return 1;
      }
      info("Added dontaudit " + rule.lhs + " " + rule.rhs + ":" + rule.tclass);
      break;
    }
  }

  info("Writing updated policy to " + policyPath);
  if (!parser.save(policyPath, errorMsg)) {
    error("Failed to write policy: " + errorMsg);
    return 1;
  }

  info("Policy update complete");
  return 0;
}
