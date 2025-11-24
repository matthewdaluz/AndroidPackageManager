/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: te_parser.cpp
 * Purpose: Implement a tiny .te parser for apm-policy.
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

#include "te_parser.hpp"

#include "fs.hpp"

#include <cctype>
#include <sstream>

namespace apm::selinux {

namespace {

std::string trim(const std::string &s) {
  std::size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])))
    ++start;
  std::size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])))
    --end;
  return s.substr(start, end - start);
}

std::string stripTrailingSemicolon(const std::string &s) {
  if (!s.empty() && s.back() == ';')
    return s.substr(0, s.size() - 1);
  return s;
}

void tokenize(const std::string &stmt, std::vector<std::string> &out) {
  std::istringstream ss(stmt);
  std::string tok;
  while (ss >> tok) {
    out.push_back(stripTrailingSemicolon(tok));
  }
}

void addWarning(const std::string &msg, TePolicy &out) {
  out.warnings.push_back(msg);
}

} // namespace

bool parseTeFile(const std::string &path, TePolicy &out, std::string &error) {
  std::string content;
  if (!apm::fs::readFile(path, content)) {
    error = "Failed to read " + path;
    return false;
  }

  std::string pending;
  std::istringstream lines(content);
  std::string line;
  while (std::getline(lines, line)) {
    auto commentPos = line.find('#');
    auto slashPos = line.find("//");
    if (slashPos != std::string::npos &&
        (commentPos == std::string::npos || slashPos < commentPos)) {
      commentPos = slashPos;
    }
    if (commentPos != std::string::npos) {
      line = line.substr(0, commentPos);
    }
    line = trim(line);
    if (line.empty())
      continue;

    if (!pending.empty())
      pending.push_back(' ');
    pending.append(line);

    std::size_t semicolonPos = std::string::npos;
    while ((semicolonPos = pending.find(';')) != std::string::npos) {
      std::string stmt = trim(pending.substr(0, semicolonPos));
      pending = pending.substr(semicolonPos + 1);
      if (stmt.empty())
        continue;

      std::vector<std::string> tokens;
      tokenize(stmt, tokens);
      if (tokens.empty())
        continue;

      const std::string &kind = tokens[0];
      if (kind == "type") {
        if (tokens.size() < 2) {
          addWarning("Ignoring malformed type statement: " + stmt, out);
          continue;
        }
        TeRule rule;
        rule.kind = TeRule::Kind::Type;
        rule.lhs = tokens[1];
        out.rules.push_back(std::move(rule));
      } else if (kind == "typeattribute") {
        if (tokens.size() < 3) {
          addWarning("Ignoring malformed typeattribute statement: " + stmt,
                     out);
          continue;
        }
        TeRule rule;
        rule.kind = TeRule::Kind::TypeAttribute;
        rule.lhs = tokens[1];
        rule.rhs = tokens[2];
        out.rules.push_back(std::move(rule));
      } else if (kind == "allow" || kind == "dontaudit") {
        if (tokens.size() < 3) {
          addWarning("Ignoring malformed rule: " + stmt, out);
          continue;
        }
        std::string objClass = tokens[2];
        auto colon = objClass.find(':');
        if (colon == std::string::npos) {
          addWarning("Missing class in rule: " + stmt, out);
          continue;
        }
        TeRule rule;
        rule.kind =
            kind == "allow" ? TeRule::Kind::Allow : TeRule::Kind::DontAudit;
        rule.lhs = tokens[1];
        rule.rhs = objClass.substr(0, colon);
        rule.tclass = objClass.substr(colon + 1);
        for (std::size_t i = 3; i < tokens.size(); ++i) {
          rule.perms.push_back(tokens[i]);
        }
        if (rule.perms.empty()) {
          addWarning("No permissions listed in rule: " + stmt, out);
          continue;
        }
        out.rules.push_back(std::move(rule));
      } else {
        addWarning("Unknown directive: " + stmt, out);
      }
    }
  }

  if (!pending.empty()) {
    addWarning("Dangling statement without semicolon: " + pending, out);
  }

  return true;
}

} // namespace apm::selinux
