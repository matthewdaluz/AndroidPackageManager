/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: manual_package.cpp
 * Purpose: Implement manual package metadata parsing, persistence, and lifecycle helpers.
 * Last Modified: 2026-03-15 11:56:16.537911560 -0400.
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

#include "manual_package.hpp"

#include "config.hpp"
#include "fs.hpp"
#include "logger.hpp"
#include "security.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

class JsonParser {
public:
  explicit JsonParser(const std::string &input) : data(input) {}

  bool parsePackageInfo(apm::manual::PackageInfo &info,
                        std::string *errorMsg) {
    skipWhitespace();
    if (!consume('{', errorMsg))
      return false;

    bool firstField = true;
    while (true) {
      skipWhitespace();
      if (peek() == '}') {
        advance();
        break;
      }

      if (!firstField) {
        if (!consume(',', errorMsg))
          return false;
        skipWhitespace();
      }
      firstField = false;

      std::string key;
      if (!parseString(key, errorMsg))
        return false;

      skipWhitespace();
      if (!consume(':', errorMsg))
        return false;
      skipWhitespace();

      if (key == "package") {
        if (!parseString(info.name, errorMsg))
          return false;
      } else if (key == "version") {
        if (!parseString(info.version, errorMsg))
          return false;
      } else if (key == "prefix") {
        if (!parseString(info.prefix, errorMsg))
          return false;
      } else if (key == "install_time") {
        if (!parseUint64(info.installTime, errorMsg))
          return false;
      } else if (key == "installed_files") {
        if (!parseStringArray(info.installedFiles, errorMsg))
          return false;
      } else {
        if (!skipValue(errorMsg))
          return false;
      }
    }

    return true;
  }

private:
  const std::string &data;
  std::size_t pos = 0;

  void skipWhitespace() {
    while (pos < data.size()) {
      char c = data[pos];
      if (c == ' ' || c == '\n' || c == '\t' || c == '\r')
        ++pos;
      else
        break;
    }
  }

  char peek() const {
    if (pos >= data.size())
      return '\0';
    return data[pos];
  }

  void advance() {
    if (pos < data.size())
      ++pos;
  }

  bool consume(char expected, std::string *errorMsg) {
    skipWhitespace();
    if (peek() != expected) {
      if (errorMsg) {
        *errorMsg =
            std::string("Expected '") + expected + "' in package-info JSON";
      }
      return false;
    }
    advance();
    return true;
  }

  bool parseString(std::string &out, std::string *errorMsg) {
    skipWhitespace();
    if (peek() != '"') {
      if (errorMsg)
        *errorMsg = "Expected string in package-info JSON";
      return false;
    }
    advance();
    std::string result;
    while (pos < data.size()) {
      char c = data[pos++];
      if (c == '"') {
        out = result;
        return true;
      }
      if (c == '\\') {
        if (pos >= data.size()) {
          if (errorMsg)
            *errorMsg = "Invalid escape in package-info JSON";
          return false;
        }
        char esc = data[pos++];
        switch (esc) {
        case '"':
        case '\\':
        case '/':
          result.push_back(esc);
          break;
        case 'b':
          result.push_back('\b');
          break;
        case 'f':
          result.push_back('\f');
          break;
        case 'n':
          result.push_back('\n');
          break;
        case 'r':
          result.push_back('\r');
          break;
        case 't':
          result.push_back('\t');
          break;
        default:
          if (errorMsg)
            *errorMsg = "Unsupported escape in package-info JSON";
          return false;
        }
      } else {
        result.push_back(c);
      }
    }
    if (errorMsg)
      *errorMsg = "Unterminated string in package-info JSON";
    return false;
  }

  bool parseUint64(std::uint64_t &out, std::string *errorMsg) {
    skipWhitespace();
    if (pos >= data.size() || !std::isdigit(static_cast<unsigned char>(data[pos]))) {
      if (errorMsg)
        *errorMsg = "Expected numeric value in package-info JSON";
      return false;
    }
    std::uint64_t value = 0;
    while (pos < data.size() &&
           std::isdigit(static_cast<unsigned char>(data[pos]))) {
      std::uint64_t digit = static_cast<std::uint64_t>(data[pos] - '0');
      if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
        if (errorMsg)
          *errorMsg = "Numeric overflow in package-info JSON";
        return false;
      }
      value = value * 10 + digit;
      ++pos;
    }
    out = value;
    return true;
  }

  bool parseStringArray(std::vector<std::string> &out, std::string *errorMsg) {
    skipWhitespace();
    if (!consume('[', errorMsg))
      return false;

    skipWhitespace();
    if (peek() == ']') {
      advance();
      return true;
    }

    while (true) {
      std::string entry;
      if (!parseString(entry, errorMsg))
        return false;
      out.push_back(entry);
      skipWhitespace();
      char c = peek();
      if (c == ',') {
        advance();
        continue;
      }
      if (c == ']') {
        advance();
        break;
      }
      if (errorMsg)
        *errorMsg = "Malformed array in package-info JSON";
      return false;
    }
    return true;
  }

  bool skipLiteral(const char *literal) {
    std::size_t len = std::strlen(literal);
    if (data.compare(pos, len, literal) == 0) {
      pos += len;
      return true;
    }
    return false;
  }

  bool skipNumber(std::string *errorMsg) {
    skipWhitespace();
    bool hasDigits = false;
    if (peek() == '-' || peek() == '+')
      advance();
    while (pos < data.size()) {
      char c = data[pos];
      if (std::isdigit(static_cast<unsigned char>(c))) {
        hasDigits = true;
        ++pos;
        continue;
      }
      if (c == '.' || c == 'e' || c == 'E' || c == '+'
          || c == '-') {
        hasDigits = true;
        ++pos;
        continue;
      }
      break;
    }
    if (!hasDigits) {
      if (errorMsg)
        *errorMsg = "Malformed number in package-info JSON";
      return false;
    }
    return true;
  }

  bool skipValue(std::string *errorMsg) {
    skipWhitespace();
    char c = peek();
    if (c == '"') {
      std::string tmp;
      return parseString(tmp, errorMsg);
    }
    if (c == '{') {
      advance();
      skipWhitespace();
      if (peek() == '}') {
        advance();
        return true;
      }
      while (true) {
        std::string tmpKey;
        if (!parseString(tmpKey, errorMsg))
          return false;
        skipWhitespace();
        if (!consume(':', errorMsg))
          return false;
        if (!skipValue(errorMsg))
          return false;
        skipWhitespace();
        if (peek() == '}') {
          advance();
          break;
        }
        if (!consume(',', errorMsg))
          return false;
      }
      return true;
    }
    if (c == '[') {
      advance();
      skipWhitespace();
      if (peek() == ']') {
        advance();
        return true;
      }
      while (true) {
        if (!skipValue(errorMsg))
          return false;
        skipWhitespace();
        if (peek() == ']') {
          advance();
          break;
        }
        if (!consume(',', errorMsg))
          return false;
      }
      return true;
    }
    if (c == 't')
      return skipLiteral("true");
    if (c == 'f')
      return skipLiteral("false");
    if (c == 'n')
      return skipLiteral("null");
    if (c == '-' || c == '+' || std::isdigit(static_cast<unsigned char>(c)))
      return skipNumber(errorMsg);

    if (errorMsg)
      *errorMsg = "Unsupported token in package-info JSON";
    return false;
  }
};

std::string escapeJsonString(const std::string &input) {
  std::string out;
  out.reserve(input.size() + 8);
  for (char c : input) {
    switch (c) {
    case '\\':
      out.append("\\\\");
      break;
    case '"':
      out.append("\\\"");
      break;
    case '\b':
      out.append("\\b");
      break;
    case '\f':
      out.append("\\f");
      break;
    case '\n':
      out.append("\\n");
      break;
    case '\r':
      out.append("\\r");
      break;
    case '\t':
      out.append("\\t");
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  return out;
}

static bool validateManualPackageName(const std::string &name,
                                      std::string *errorMsg) {
  std::string err;
  if (!apm::security::validatePackageName(name, &err)) {
    if (errorMsg)
      *errorMsg = err;
    apm::logger::error("manual_package: " + err);
    return false;
  }
  return true;
}

} // namespace

namespace apm::manual {

// Parse a package-info JSON string into a strongly typed PackageInfo struct.
bool parsePackageInfo(const std::string &json, PackageInfo &info,
                      std::string *errorMsg) {
  info = PackageInfo{};
  JsonParser parser(json);
  if (!parser.parsePackageInfo(info, errorMsg))
    return false;
  if (info.name.empty()) {
    if (errorMsg)
      *errorMsg = "package-info is missing 'package'";
    return false;
  }
  if (info.prefix.empty()) {
    if (errorMsg)
      *errorMsg = "package-info is missing 'prefix'";
    return false;
  }
  return true;
}

// Serialize a PackageInfo struct back into the canonical JSON representation.
std::string serializePackageInfo(const PackageInfo &info) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"package\": \"" << escapeJsonString(info.name) << "\",\n";
  out << "  \"version\": \"" << escapeJsonString(info.version) << "\",\n";
  out << "  \"install_time\": " << info.installTime << ",\n";
  out << "  \"prefix\": \"" << escapeJsonString(info.prefix) << "\",\n";
  out << "  \"installed_files\": [\n";
  for (std::size_t i = 0; i < info.installedFiles.size(); ++i) {
    out << "    \"" << escapeJsonString(info.installedFiles[i]) << "\"";
    if (i + 1 < info.installedFiles.size())
      out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

// Read package-info JSON from disk and parse it in one step.
bool readPackageInfoFile(const std::string &path, PackageInfo &info,
                         std::string *errorMsg) {
  std::string content;
  if (!apm::fs::readFile(path, content)) {
    if (errorMsg)
      *errorMsg = "Failed to read " + path;
    apm::logger::error("manual_package: cannot read " + path);
    return false;
  }
  return parsePackageInfo(content, info, errorMsg);
}

// Write a PackageInfo struct to disk, creating parent directories as needed.
bool writePackageInfoFile(const std::string &path, const PackageInfo &info,
                          std::string *errorMsg) {
  std::string data = serializePackageInfo(info);
  if (!apm::fs::writeFile(path, data, true)) {
    if (errorMsg)
      *errorMsg = "Failed to write " + path;
    apm::logger::error("manual_package: cannot write " + path);
    return false;
  }
  return true;
}

// Compute the on-disk metadata path for a manual package name.
std::string installedInfoPath(const std::string &name) {
  return apm::fs::joinPath(apm::config::MANUAL_PACKAGES_DIR, name + ".json");
}

// Load a manual package manifest if it exists on disk.
bool loadInstalledPackage(const std::string &name, PackageInfo &info,
                          std::string *errorMsg) {
  if (!validateManualPackageName(name, errorMsg))
    return false;
  std::string path = installedInfoPath(name);
  if (!apm::fs::pathExists(path)) {
    if (errorMsg)
      *errorMsg = "Manual package '" + name + "' is not installed";
    return false;
  }
  return readPackageInfoFile(path, info, errorMsg);
}

// Persist PackageInfo metadata after installing or updating a manual package.
bool saveInstalledPackage(const PackageInfo &info, std::string *errorMsg) {
  if (info.name.empty()) {
    if (errorMsg)
      *errorMsg = "Manual package name is empty";
    return false;
  }
  if (!validateManualPackageName(info.name, errorMsg))
    return false;
  if (!apm::fs::createDirs(apm::config::MANUAL_PACKAGES_DIR)) {
    if (errorMsg)
      *errorMsg = std::string("Failed to create directory: ") +
                  apm::config::MANUAL_PACKAGES_DIR;
    return false;
  }
  return writePackageInfoFile(installedInfoPath(info.name), info, errorMsg);
}

// Delete the manifest for a manual package (no-op if not present).
bool removeInstalledPackage(const std::string &name, std::string *errorMsg) {
  if (!validateManualPackageName(name, errorMsg))
    return false;
  std::string path = installedInfoPath(name);
  if (!apm::fs::pathExists(path))
    return true;
  if (!apm::fs::removeFile(path)) {
    if (errorMsg)
      *errorMsg = "Failed to delete " + path;
    return false;
  }
  return true;
}

// Enumerate every package-info JSON file in MANUAL_PACKAGES_DIR.
bool listInstalledPackages(std::vector<PackageInfo> &out,
                           std::string *errorMsg) {
  out.clear();
  if (!apm::fs::pathExists(apm::config::MANUAL_PACKAGES_DIR))
    return true;
  auto entries = apm::fs::listDir(apm::config::MANUAL_PACKAGES_DIR, false);
  for (const auto &entry : entries) {
    if (entry.empty() || entry[0] == '.')
      continue;
    std::string path =
        apm::fs::joinPath(apm::config::MANUAL_PACKAGES_DIR, entry);
    if (!apm::fs::isFile(path))
      continue;
    PackageInfo info;
    if (!readPackageInfoFile(path, info, errorMsg))
      return false;
    out.push_back(std::move(info));
  }
  std::sort(out.begin(), out.end(),
            [](const PackageInfo &a, const PackageInfo &b) {
              return a.name < b.name;
            });
  return true;
}

// Quick existence check for a manual package.
bool isInstalled(const std::string &name) {
  if (!validateManualPackageName(name, nullptr))
    return false;
  return apm::fs::pathExists(installedInfoPath(name));
}

} // namespace apm::manual
