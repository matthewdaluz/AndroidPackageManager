/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: protocol.cpp
 * Purpose: Implement IPC request/response parsing plus serialization utilities.
 * Last Modified: 2026-03-15 12:19:43.769027823 -0400.
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

#include "protocol.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace apm::ipc {

// -------------------------------------------------------------
// Helpers
// -------------------------------------------------------------

// Uppercase helper used for canonicalizing command types.
static inline std::string upper(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s)
    out.push_back(static_cast<char>(std::toupper(c)));
  return out;
}

// Trim whitespace from both ends of a string in-place.
static inline void trim(std::string &s) {
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
            return !std::isspace(ch);
          }));
  s.erase(std::find_if(s.rbegin(), s.rend(),
                       [](unsigned char ch) { return !std::isspace(ch); })
              .base(),
          s.end());
}

static bool parseBoolFieldValue(std::string value, bool &out) {
  trim(value);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });

  if (value == "1" || value == "true" || value == "yes" ||
      value == "on") {
    out = true;
    return true;
  }

  if (value == "0" || value == "false" || value == "no" ||
      value == "off") {
    out = false;
    return true;
  }

  return false;
}

// Escape line-delimiter-sensitive characters so values can be safely serialized
// into single-line key:value frames.
static std::string escapeFieldValue(const std::string &value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  return out;
}

// Reverse escaping performed by escapeFieldValue().
static std::string unescapeFieldValue(const std::string &value) {
  std::string out;
  out.reserve(value.size());

  for (std::size_t i = 0; i < value.size(); ++i) {
    char c = value[i];
    if (c != '\\' || i + 1 >= value.size()) {
      out.push_back(c);
      continue;
    }

    char next = value[++i];
    switch (next) {
    case 'n':
      out.push_back('\n');
      break;
    case 'r':
      out.push_back('\r');
      break;
    case '\\':
      out.push_back('\\');
      break;
    default:
      // Unknown escape: preserve both bytes for forward compatibility.
      out.push_back('\\');
      out.push_back(next);
      break;
    }
  }

  return out;
}

// Parse RFC822-style key:value lines until a blank line and return them.
static std::unordered_map<std::string, std::string>
parseKeyValueLines(const std::string &raw, std::string *errorMsg) {
  std::unordered_map<std::string, std::string> fields;

  std::istringstream in(raw);
  std::string line;

  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r')
      line.pop_back();

    if (line.empty())
      break;

    auto pos = line.find(':');
    if (pos == std::string::npos) {
      if (errorMsg)
        *errorMsg = "Malformed line: " + line;
      apm::logger::error("parseKeyValueLines: malformed line: " + line);
      fields.clear();
      return fields;
    }

    std::string key = line.substr(0, pos);
    std::string value = line.substr(pos + 1);

    trim(key);
    trim(value);
    value = unescapeFieldValue(value);

    if (key.empty()) {
      if (errorMsg)
        *errorMsg = "Empty key in line: " + line;
      fields.clear();
      return fields;
    }

    fields[key] = value;
  }

  return fields;
}

// -------------------------------------------------------------
// Type parsing
// -------------------------------------------------------------

// Map a textual request type (e.g. "INSTALL") to the enum variant.
RequestType parseType(const std::string &sRaw) {
  std::string s = upper(sRaw);

  if (s == "PING")
    return RequestType::Ping;
  if (s == "AUTHENTICATE")
    return RequestType::Authenticate;
  if (s == "FORGOT_PASSWORD")
    return RequestType::ForgotPassword;
  if (s == "LIST")
    return RequestType::List;
  if (s == "INFO")
    return RequestType::Info;
  if (s == "SEARCH")
    return RequestType::Search;
  if (s == "UPDATE")
    return RequestType::Update;
  if (s == "ADD_REPO")
    return RequestType::AddRepo;
  if (s == "LIST_REPOS")
    return RequestType::ListRepos;
  if (s == "REMOVE_REPO")
    return RequestType::RemoveRepo;
  if (s == "INSTALL")
    return RequestType::Install;
  if (s == "REMOVE")
    return RequestType::Remove;
  if (s == "AUTOREMOVE")
    return RequestType::Autoremove;
  if (s == "UPGRADE")
    return RequestType::Upgrade;
  if (s == "APK_INSTALL")
    return RequestType::ApkInstall;
  if (s == "APK_UNINSTALL")
    return RequestType::ApkUninstall;
  if (s == "MODULE_LIST")
    return RequestType::ModuleList;
  if (s == "MODULE_INSTALL")
    return RequestType::ModuleInstall;
  if (s == "MODULE_ENABLE")
    return RequestType::ModuleEnable;
  if (s == "MODULE_DISABLE")
    return RequestType::ModuleDisable;
  if (s == "MODULE_REMOVE")
    return RequestType::ModuleRemove;
  if (s == "FACTORY_RESET")
    return RequestType::FactoryReset;
  if (s == "WIPE_CACHE")
    return RequestType::WipeCache;
  if (s == "DEBUG_LOGGING")
    return RequestType::DebugLogging;
  if (s == "LOG_CLEAR")
    return RequestType::LogClear;

  return RequestType::Unknown;
}

// Convert a RequestType enum into the canonical uppercase token.
std::string typeToString(RequestType t) {
  switch (t) {
  case RequestType::Ping:
    return "PING";
  case RequestType::Authenticate:
    return "AUTHENTICATE";
  case RequestType::ForgotPassword:
    return "FORGOT_PASSWORD";
  case RequestType::List:
    return "LIST";
  case RequestType::Info:
    return "INFO";
  case RequestType::Search:
    return "SEARCH";
  case RequestType::Update:
    return "UPDATE";
  case RequestType::AddRepo:
    return "ADD_REPO";
  case RequestType::ListRepos:
    return "LIST_REPOS";
  case RequestType::RemoveRepo:
    return "REMOVE_REPO";
  case RequestType::Install:
    return "INSTALL";
  case RequestType::Remove:
    return "REMOVE";
  case RequestType::Autoremove:
    return "AUTOREMOVE";
  case RequestType::Upgrade:
    return "UPGRADE";
  case RequestType::ApkInstall:
    return "APK_INSTALL";
  case RequestType::ApkUninstall:
    return "APK_UNINSTALL";
  case RequestType::ModuleList:
    return "MODULE_LIST";
  case RequestType::ModuleInstall:
    return "MODULE_INSTALL";
  case RequestType::ModuleEnable:
    return "MODULE_ENABLE";
  case RequestType::ModuleDisable:
    return "MODULE_DISABLE";
  case RequestType::ModuleRemove:
    return "MODULE_REMOVE";
  case RequestType::FactoryReset:
    return "FACTORY_RESET";
  case RequestType::WipeCache:
    return "WIPE_CACHE";
  case RequestType::DebugLogging:
    return "DEBUG_LOGGING";
  case RequestType::LogClear:
    return "LOG_CLEAR";
  default:
    return "UNKNOWN";
  }
}

// -------------------------------------------------------------
// Request Parsing
// -------------------------------------------------------------

// Parse a raw IPC frame into a Request object, validating required fields for
// each command.
bool parseRequest(const std::string &raw, Request &out, std::string *errorMsg) {
  out = Request{};

  auto fields = parseKeyValueLines(raw, errorMsg);
  if (fields.empty())
    return false;

  out.rawFields = fields;

  auto get = [&](const std::string &k) -> std::string {
    auto it = fields.find(k);
    return (it != fields.end()) ? it->second : "";
  };

  std::string typeStr = get("type");
  if (typeStr.empty()) {
    if (errorMsg)
      *errorMsg = "Request missing type field";
    return false;
  }

  out.type = parseType(typeStr);
  if (out.type == RequestType::Unknown) {
    if (errorMsg)
      *errorMsg = "Unknown request type: " + typeStr;
    return false;
  }

  out.id = get("id");
  out.sessionToken = get("session");

  if (out.type == RequestType::AddRepo || out.type == RequestType::RemoveRepo) {
    out.repoPath = get("repo_path");
    if (out.repoPath.empty()) {
      if (errorMsg)
        *errorMsg = "Missing 'repo_path' field";
      return false;
    }
  }

  // Commands that require packageName
  // Install/Remove use packageName
  if (out.type == RequestType::Install || out.type == RequestType::Remove ||
      out.type == RequestType::ApkUninstall || out.type == RequestType::Info) {

    out.packageName = get("package");
    if (out.packageName.empty()) {
      if (errorMsg)
        *errorMsg = "Missing 'package' field";
      return false;
    }
  }

  // ApkInstall uses apkPath instead of packageName
  if (out.type == RequestType::ApkInstall) {
    out.apkPath = get("apkPath");
    std::string sysFlag = get("installAsSystem");

    out.installAsSystem =
        (!sysFlag.empty() &&
         (sysFlag == "1" || sysFlag == "true" || sysFlag == "yes"));

    if (out.apkPath.empty()) {
      if (errorMsg)
        *errorMsg = "Missing 'apkPath' field";
      return false;
    }
  }

  if (out.type == RequestType::ModuleInstall) {
    out.modulePath = get("module_path");
    if (out.modulePath.empty()) {
      if (errorMsg)
        *errorMsg = "Missing 'module_path' field";
      return false;
    }
  }

  if (out.type == RequestType::ModuleEnable ||
      out.type == RequestType::ModuleDisable ||
      out.type == RequestType::ModuleRemove) {
    out.moduleName = get("module");
    if (out.moduleName.empty())
      out.moduleName = get("package");
    if (out.moduleName.empty()) {
      if (errorMsg)
        *errorMsg = "Missing 'module' field";
      return false;
    }
  }

  if (out.type == RequestType::Authenticate) {
    out.authAction = get("auth_action");
    if (out.authAction.empty())
      out.authAction = "unlock";
    out.authSecret = get("auth_secret");
    if (out.authSecret.empty()) {
      if (errorMsg)
        *errorMsg = "Missing 'auth_secret' field";
      return false;
    }
  }

  if (out.type == RequestType::DebugLogging) {
    const std::string enabled = get("enabled");
    if (enabled.empty()) {
      if (errorMsg)
        *errorMsg = "Missing 'enabled' field";
      return false;
    }

    if (!parseBoolFieldValue(enabled, out.debugLoggingEnabled)) {
      if (errorMsg)
        *errorMsg = "Invalid 'enabled' field: " + enabled;
      return false;
    }
  }

  if (out.type == RequestType::LogClear) {
    out.moduleName = get("module");
  }

  return true;
}

// -------------------------------------------------------------
// Request Serialization
// -------------------------------------------------------------

// Convert a Request struct back into the newline-delimited format understood
// by apmd/apm.
std::string serializeRequest(const Request &req) {
  std::ostringstream out;

  out << "type:" << typeToString(req.type) << "\n";

  if (!req.id.empty())
    out << "id:" << escapeFieldValue(req.id) << "\n";

  if (!req.sessionToken.empty())
    out << "session:" << escapeFieldValue(req.sessionToken) << "\n";

  if (req.type == RequestType::Authenticate) {
    if (!req.authAction.empty())
      out << "auth_action:" << escapeFieldValue(req.authAction) << "\n";
    if (!req.authSecret.empty())
      out << "auth_secret:" << escapeFieldValue(req.authSecret) << "\n";
  }

  if (!req.packageName.empty())
    out << "package:" << escapeFieldValue(req.packageName) << "\n";

  if (!req.repoPath.empty())
    out << "repo_path:" << escapeFieldValue(req.repoPath) << "\n";

  if (!req.apkPath.empty())
    out << "apkPath:" << escapeFieldValue(req.apkPath) << "\n";

  if (req.installAsSystem)
    out << "installAsSystem:1\n";

  if (!req.modulePath.empty())
    out << "module_path:" << escapeFieldValue(req.modulePath) << "\n";
  if (!req.moduleName.empty())
    out << "module:" << escapeFieldValue(req.moduleName) << "\n";

  if (req.type == RequestType::DebugLogging) {
    out << "enabled:" << (req.debugLoggingEnabled ? "true" : "false") << "\n";
  }

  for (const auto &kv : req.rawFields) {
    if (kv.first == "type" || kv.first == "id" || kv.first == "package" ||
        kv.first == "session" || kv.first == "apkPath" ||
        kv.first == "installAsSystem" || kv.first == "module_path" ||
        kv.first == "module" || kv.first == "auth_action" ||
        kv.first == "auth_secret" || kv.first == "enabled" ||
        kv.first == "repo_path")
      continue;
    out << kv.first << ":" << escapeFieldValue(kv.second) << "\n";
  }

  out << "\n";
  return out.str();
}

// -------------------------------------------------------------
// Response Parsing
// -------------------------------------------------------------

// Parse a response frame (ok/error/progress) and retain any additional key/value
// metadata for higher-level handlers.
bool parseResponse(const std::string &raw, Response &out,
                   std::string *errorMsg) {
  out = Response{};

  auto fields = parseKeyValueLines(raw, errorMsg);
  if (fields.empty())
    return false;

  out.rawFields = fields;

  auto get = [&](const std::string &k) -> std::string {
    auto it = fields.find(k);
    return (it != fields.end()) ? it->second : "";
  };

  std::string status = get("status");
  if (status == "ok") {
    out.status = ResponseStatus::Ok;
    out.success = true;
  } else if (status == "error") {
    out.status = ResponseStatus::Error;
    out.success = false;
  } else if (status == "progress") {
    out.status = ResponseStatus::Progress;
    out.success = false;
  } else {
    if (errorMsg)
      *errorMsg = "Unknown status in response";
    return false;
  }

  out.id = get("id");
  out.message = get("message");
  out.rawFields.erase("status");
  out.rawFields.erase("id");
  out.rawFields.erase("message");

  return true;
}

// -------------------------------------------------------------
// Response Serialization
// -------------------------------------------------------------

// Convert a Response struct into the wire format understood by CLI clients.
std::string serializeResponse(const Response &resp) {
  std::ostringstream out;

  std::string statusStr;
  switch (resp.status) {
  case ResponseStatus::Ok:
    statusStr = "ok";
    break;
  case ResponseStatus::Error:
    statusStr = "error";
    break;
  case ResponseStatus::Progress:
    statusStr = "progress";
    break;
  default:
    statusStr = resp.success ? "ok" : "error";
    break;
  }

  out << "status:" << statusStr << "\n";
  if (!resp.id.empty())
    out << "id:" << escapeFieldValue(resp.id) << "\n";
  if (!resp.message.empty())
    out << "message:" << escapeFieldValue(resp.message) << "\n";
  for (const auto &kv : resp.rawFields) {
    out << kv.first << ":" << escapeFieldValue(kv.second) << "\n";
  }

  out << "\n";
  return out.str();
}

} // namespace apm::ipc
