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

#include "fs.hpp"
#include "logger.hpp"
#include "te_parser.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using apm::logger::error;
using apm::logger::info;
using apm::logger::warn;

extern char **environ;

namespace {

const char *kDefaultRulePath = "/data/apm-system-overlay/sepolicy/apm-sepolicy.rules";
const char *kLogPath = "/data/apm/logs/policy.log";
const char *kTempRuleTemplate = "/data/local/tmp/apm-policy.XXXXXX";

void printUsage(const char *argv0) {
  std::cerr << "Usage: " << argv0 << " <rules.te> [rule_output]\n";
}

std::string join(const std::vector<std::string> &items,
                 const std::string &sep) {
  std::string out;
  for (std::size_t i = 0; i < items.size(); ++i) {
    out.append(items[i]);
    if (i + 1 < items.size())
      out.append(sep);
  }
  return out;
}

bool isExecutable(const std::string &path) {
  return ::access(path.c_str(), X_OK) == 0;
}

bool findMagiskPolicy(std::string &outPath) {
  const std::vector<std::string> candidates = {
      "/sbin/magiskpolicy",
      "/sbin/.magisk/magiskpolicy",
      "/data/adb/magisk/magiskpolicy",
      "/system/bin/magiskpolicy",
  };
  for (const auto &candidate : candidates) {
    if (isExecutable(candidate)) {
      outPath = candidate;
      return true;
    }
  }

  const char *pathEnv = std::getenv("PATH");
  if (pathEnv) {
    std::string pathVar(pathEnv);
    std::size_t pos = 0;
    while (pos <= pathVar.size()) {
      std::size_t next = pathVar.find(':', pos);
      std::string dir =
          (next == std::string::npos) ? pathVar.substr(pos)
                                      : pathVar.substr(pos, next - pos);
      if (!dir.empty()) {
        std::string candidate = dir + "/magiskpolicy";
        if (isExecutable(candidate)) {
          outPath = candidate;
          return true;
        }
      }
      if (next == std::string::npos)
        break;
      pos = next + 1;
    }
  }
  return false;
}

bool writeRuleFile(const std::string &path,
                   const std::vector<std::string> &rules, bool append,
                   std::string &error) {
  std::string payload;
  for (const auto &line : rules) {
    payload.append(line);
    payload.push_back('\n');
  }

  bool ok = append ? apm::fs::appendFile(path, payload, true)
                   : apm::fs::writeFile(path, payload, true);
  if (!ok) {
    error = "Failed to write rule file at " + path;
    return false;
  }
  return true;
}

bool writeTempRuleFile(const std::vector<std::string> &rules,
                       std::string &outPath, std::string &error) {
  std::array<char, 128> tmpl{};
  std::snprintf(tmpl.data(), tmpl.size(), "%s", kTempRuleTemplate);
  int fd = ::mkstemp(tmpl.data());
  if (fd < 0) {
    error = "Unable to create temp rule file";
    return false;
  }

  std::string payload;
  for (const auto &line : rules) {
    payload.append(line);
    payload.push_back('\n');
  }

  const char *data = payload.data();
  std::size_t remaining = payload.size();
  while (remaining > 0) {
    ssize_t written = ::write(fd, data, remaining);
    if (written < 0) {
      error = "Failed writing temp rule file";
      ::close(fd);
      ::unlink(tmpl.data());
      return false;
    }
    data += static_cast<std::size_t>(written);
    remaining -= static_cast<std::size_t>(written);
  }

  ::close(fd);
  outPath.assign(tmpl.data());
  return true;
}

bool runCommand(const std::vector<std::string> &args, std::string &error) {
  if (args.empty()) {
    error = "No command specified";
    return false;
  }

  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (const auto &arg : args) {
    argv.push_back(const_cast<char *>(arg.c_str()));
  }
  argv.push_back(nullptr);

  pid_t pid = 0;
  int spawnRc = ::posix_spawn(&pid, args[0].c_str(), nullptr, nullptr,
                              argv.data(), environ);
  if (spawnRc != 0) {
    error = "Unable to spawn " + args[0] + ": " + std::strerror(spawnRc);
    return false;
  }

  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) {
    error = "Failed waiting for " + args[0] + ": " + std::strerror(errno);
    return false;
  }

  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    error = args[0] + " exited with code " +
            std::to_string(WEXITSTATUS(status));
    return false;
  }

  return true;
}

bool applyWithMagiskPolicy(const std::string &binaryPath,
                           const std::vector<std::string> &rules,
                           std::string &error) {
  std::string tempRulePath;
  if (!writeTempRuleFile(rules, tempRulePath, error)) {
    return false;
  }

  std::vector<std::string> args = {binaryPath, "--live", "--apply",
                                   tempRulePath};
  bool ok = runCommand(args, error);
  ::unlink(tempRulePath.c_str());
  return ok;
}

std::vector<std::string> buildRuleLines(const apm::selinux::TePolicy &policy) {
  std::vector<std::string> lines;
  for (const auto &rule : policy.rules) {
    switch (rule.kind) {
    case apm::selinux::TeRule::Kind::Type:
      lines.push_back("type " + rule.lhs);
      break;
    case apm::selinux::TeRule::Kind::TypeAttribute:
      lines.push_back("typeattribute " + rule.lhs + " " + rule.rhs);
      break;
    case apm::selinux::TeRule::Kind::Allow:
      lines.push_back("allow " + rule.lhs + " " + rule.rhs + ":" +
                      rule.tclass + " " + join(rule.perms, " "));
      break;
    case apm::selinux::TeRule::Kind::DontAudit:
      lines.push_back("dontaudit " + rule.lhs + " " + rule.rhs + ":" +
                      rule.tclass + " " + join(rule.perms, " "));
      break;
    }
  }
  return lines;
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
  std::string ruleOutPath = (argc == 3) ? argv[2] : kDefaultRulePath;

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

  auto ruleLines = buildRuleLines(tePolicy);
  if (ruleLines.empty()) {
    warn("No rules to apply from " + tePath);
    return 0;
  }

  std::string injectorPath;
  bool injected = false;
  if (findMagiskPolicy(injectorPath)) {
    info("Applying " + std::to_string(ruleLines.size()) +
         " rule(s) via magiskpolicy");
    if (!applyWithMagiskPolicy(injectorPath, ruleLines, errorMsg)) {
      error("Failed to inject policy: " + errorMsg);
      return 1;
    }
    info("Live policy injection complete");
    injected = true;
  } else {
    warn("magiskpolicy not found; rules will be written for external "
         "processing");
  }

  if (!writeRuleFile(ruleOutPath, ruleLines, true, errorMsg)) {
    error("Failed to persist rules to " + ruleOutPath + ": " + errorMsg);
    return 1;
  }

  info("Appended rules to " + ruleOutPath);
  if (!injected) {
    warn("Rules were not injected live; please apply them with magiskpolicy");
    return 1;
  }
  info("apm-policy finished");
  return 0;
}
