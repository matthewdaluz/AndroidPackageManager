/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: module_info.cpp
 * Purpose: Implement AMS module metadata/state parsing and serialization helpers.
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

#include "ams/module_info.hpp"

#include "config.hpp"
#include "fs.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <ctime>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace {

class JsonReader {
public:
  explicit JsonReader(const std::string &input) : data(input) {}

  struct Value {
    enum class Type { String, Bool, Null };
    Type type = Type::String;
    std::string stringValue;
    bool boolValue = false;
  };

  bool parseObject(std::unordered_map<std::string, Value> &out,
                   std::string *errorMsg) {
    skipWhitespace();
    if (!consume('{', errorMsg))
      return false;

    skipWhitespace();
    if (peek() == '}') {
      advance();
      return true;
    }

    while (pos < data.size()) {
      std::string key;
      if (!parseString(key, errorMsg))
        return false;

      skipWhitespace();
      if (!consume(':', errorMsg))
        return false;

      skipWhitespace();
      Value value;
      if (!parseValue(value, errorMsg))
        return false;
      out[key] = value;

      skipWhitespace();
      char c = peek();
      if (c == ',') {
        advance();
        skipWhitespace();
        continue;
      }
      if (c == '}') {
        advance();
        return true;
      }
      if (errorMsg)
        *errorMsg = "Malformed JSON object";
      return false;
    }

    if (errorMsg)
      *errorMsg = "Unexpected end of JSON object";
    return false;
  }

private:
  const std::string &data;
  std::size_t pos = 0;

  char peek() const {
    if (pos >= data.size())
      return '\0';
    return data[pos];
  }

  void advance() {
    if (pos < data.size())
      ++pos;
  }

  void skipWhitespace() {
    while (pos < data.size()) {
      char c = data[pos];
      if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
        ++pos;
        continue;
      }
      break;
    }
  }

  bool consume(char expected, std::string *errorMsg) {
    skipWhitespace();
    if (peek() != expected) {
      if (errorMsg)
        *errorMsg = std::string("Expected '") + expected + "' in JSON";
      return false;
    }
    advance();
    return true;
  }

  bool parseValue(Value &out, std::string *errorMsg) {
    char c = peek();
    if (c == '"') {
      out.type = Value::Type::String;
      return parseString(out.stringValue, errorMsg);
    }

    if (matchLiteral("true")) {
      out.type = Value::Type::Bool;
      out.boolValue = true;
      return true;
    }
    if (matchLiteral("false")) {
      out.type = Value::Type::Bool;
      out.boolValue = false;
      return true;
    }
    if (matchLiteral("null")) {
      out.type = Value::Type::Null;
      out.stringValue.clear();
      out.boolValue = false;
      return true;
    }

    if (errorMsg)
      *errorMsg = "Unsupported value in JSON";
    return false;
  }

  bool matchLiteral(const char *literal) {
    std::size_t len = std::strlen(literal);
    if (data.compare(pos, len, literal) == 0) {
      pos += len;
      return true;
    }
    return false;
  }

  bool parseString(std::string &out, std::string *errorMsg) {
    if (peek() != '"') {
      if (errorMsg)
        *errorMsg = "Expected string in JSON";
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
            *errorMsg = "Invalid escape in JSON string";
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
            *errorMsg = "Unsupported escape in JSON string";
          return false;
        }
      } else {
        result.push_back(c);
      }
    }

    if (errorMsg)
      *errorMsg = "Unterminated JSON string";
    return false;
  }
};

} // namespace

namespace apm::ams {

static bool loadFile(const std::string &path, std::string &out,
                     std::string *errorMsg) {
  if (!apm::fs::readFile(path, out)) {
    if (errorMsg)
      *errorMsg = "Failed to read " + path;
    return false;
  }
  return true;
}

std::string makeIsoTimestamp() {
  using namespace std::chrono;
  auto now = system_clock::now();
  std::time_t t = system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &t);
#else
  std::tm *ptm = std::gmtime(&t);
  if (ptm)
    tm = *ptm;
#endif

  std::ostringstream ss;
  ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
  return ss.str();
}

bool parseModuleInfo(const std::string &json, ModuleInfo &info,
                     std::string *errorMsg) {
  info = ModuleInfo{};
  std::unordered_map<std::string, JsonReader::Value> fields;
  JsonReader reader(json);
  if (!reader.parseObject(fields, errorMsg))
    return false;

  auto getString = [&](const char *key) -> std::string {
    auto it = fields.find(key);
    if (it == fields.end())
      return {};
    if (it->second.type != JsonReader::Value::Type::String)
      return {};
    return it->second.stringValue;
  };

  auto getBool = [&](const char *key, bool defaultValue) -> bool {
    auto it = fields.find(key);
    if (it == fields.end())
      return defaultValue;
    if (it->second.type == JsonReader::Value::Type::Bool)
      return it->second.boolValue;
    if (it->second.type == JsonReader::Value::Type::String) {
      std::string lower = it->second.stringValue;
      std::transform(lower.begin(), lower.end(), lower.begin(),
                     [](unsigned char c) { return std::tolower(c); });
      if (lower == "true")
        return true;
      if (lower == "false")
        return false;
    }
    return defaultValue;
  };

  info.name = getString("name");
  info.version = getString("version");
  info.author = getString("author");
  info.description = getString("description");
  info.mount = getBool("mount", true);
  info.runPostFsData = getBool("post_fs_data", false);
  info.runService = getBool("service", false);

  if (info.name.empty()) {
    if (errorMsg)
      *errorMsg = "module-info.json missing required 'name'";
    return false;
  }

  auto isValidChar = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
           c == '_' || c == '.';
  };
  if (!std::all_of(info.name.begin(), info.name.end(), isValidChar)) {
    if (errorMsg)
      *errorMsg = "Module name contains invalid characters";
    return false;
  }

  return true;
}

bool readModuleInfoFile(const std::string &path, ModuleInfo &info,
                        std::string *errorMsg) {
  std::string content;
  if (!loadFile(path, content, errorMsg))
    return false;
  return parseModuleInfo(content, info, errorMsg);
}

bool readModuleState(const std::string &path, ModuleState &state,
                     std::string *errorMsg) {
  state = ModuleState{};
  if (!apm::fs::pathExists(path))
    return true;

  std::string data;
  if (!loadFile(path, data, errorMsg))
    return false;

  std::unordered_map<std::string, JsonReader::Value> fields;
  JsonReader reader(data);
  if (!reader.parseObject(fields, errorMsg))
    return false;

  auto itEnabled = fields.find("enabled");
  if (itEnabled != fields.end() &&
      itEnabled->second.type == JsonReader::Value::Type::Bool) {
    state.enabled = itEnabled->second.boolValue;
  }

  auto itInstalled = fields.find("installed_at");
  if (itInstalled != fields.end() &&
      itInstalled->second.type == JsonReader::Value::Type::String) {
    state.installedAt = itInstalled->second.stringValue;
  }

  auto itUpdated = fields.find("updated_at");
  if (itUpdated != fields.end() &&
      itUpdated->second.type == JsonReader::Value::Type::String) {
    state.updatedAt = itUpdated->second.stringValue;
  }

  auto itError = fields.find("last_error");
  if (itError != fields.end()) {
    if (itError->second.type == JsonReader::Value::Type::String)
      state.lastError = itError->second.stringValue;
    else
      state.lastError.clear();
  }

  return true;
}

bool writeModuleState(const std::string &path, const ModuleState &state,
                      std::string *errorMsg) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"enabled\": " << (state.enabled ? "true" : "false") << ",\n";
  out << "  \"installed_at\": \"" << state.installedAt << "\",\n";
  out << "  \"updated_at\": \"" << state.updatedAt << "\",\n";
  out << "  \"last_error\": ";
  if (state.lastError.empty())
    out << "null\n";
  else
    out << "\"" << state.lastError << "\"\n";
  out << "}\n";

  if (!apm::fs::writeFile(path, out.str())) {
    if (errorMsg)
      *errorMsg = "Failed to write " + path;
    return false;
  }
  return true;
}

} // namespace apm::ams
