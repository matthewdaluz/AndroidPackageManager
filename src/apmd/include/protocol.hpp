/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: protocol.hpp
 * Purpose: Define request/response data structures and serialization helpers shared by the CLI and daemon.
 * Last Modified: 2026-03-18 10:55:01.572244347 -0400.
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
#include <unordered_map>

namespace apm::ipc {

enum class RequestType {
  Unknown = 0,
  Ping,
  Authenticate,
  ForgotPassword,
  List,
  Info,
  Search,
  Update,
  Install,
  Remove,
  Autoremove,
  Upgrade,
  ApkInstall,
  ApkUninstall,
  ModuleList,
  ModuleInstall,
  ModuleEnable,
  ModuleDisable,
  ModuleRemove,
  FactoryReset,
  DebugLogging,
  LogClear
};

struct Request {
  RequestType type = RequestType::Unknown;
  std::string id;

  // Shared field for Install/Remove/ApkUninstall
  std::string packageName;

  std::string sessionToken;

  // Authentication specific fields
  std::string authAction;
  std::string authSecret;

  // ----------- NEW FIELDS FOR APK INSTALL -----------
  std::string apkPath;
  bool installAsSystem = false;
  // ---------------------------------------------------

  std::string modulePath;
  std::string moduleName;
  bool debugLoggingEnabled = false;

  std::unordered_map<std::string, std::string> rawFields;
};

enum class ResponseStatus {
  Unknown = 0,
  Ok,
  Error,
  Progress
};

struct Response {
  ResponseStatus status = ResponseStatus::Unknown;
  bool success = false;
  std::string message;
  std::string id;
  std::unordered_map<std::string, std::string> rawFields;
};

bool parseRequest(const std::string &raw, Request &out,
                  std::string *errorMsg = nullptr);
std::string serializeRequest(const Request &req);
std::string typeToString(RequestType t);

bool parseResponse(const std::string &raw, Response &out,
                   std::string *errorMsg = nullptr);
std::string serializeResponse(const Response &resp);

} // namespace apm::ipc
