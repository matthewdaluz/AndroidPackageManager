/*
 * AMSD - APM Module System Daemon
 *
 * Dispatch module IPC requests to ModuleManager after session validation.
 */

/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: request_dispatcher.cpp
 * Purpose: Handle AMSD IPC requests for module lifecycle operations while
 *          enforcing session validation.
 * Last Modified: December 4th, 2025. - 09:07 AM Eastern Time
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

#include "request_dispatcher.hpp"

#include "logger.hpp"

#include <sstream>
#include <utility>

namespace apm::amsd {

RequestDispatcher::RequestDispatcher(apm::ams::ModuleManager &moduleManager,
                                     SecurityManager &securityManager)
    : moduleManager_(moduleManager), securityManager_(securityManager) {}

void RequestDispatcher::dispatch(const apm::ipc::Request &req,
                                 apm::ipc::Response &resp) const {
  resp = apm::ipc::Response{};
  resp.id = req.id;

  auto finalizeStatus = [](apm::ipc::Response &r) {
    if (r.status == apm::ipc::ResponseStatus::Unknown)
      r.status = r.success ? apm::ipc::ResponseStatus::Ok
                           : apm::ipc::ResponseStatus::Error;
  };

  auto requiresAuth = [](apm::ipc::RequestType t) {
    return !(t == apm::ipc::RequestType::Ping);
  };

  if (requiresAuth(req.type)) {
    std::string authErr;
    if (!securityManager_.validateSessionToken(req.sessionToken, &authErr)) {
      resp.success = false;
      resp.message = authErr.empty() ? "Authentication required" : authErr;
      resp.rawFields["auth"] = "required";
      finalizeStatus(resp);
      return;
    }
  }

  switch (req.type) {
  case apm::ipc::RequestType::Ping: {
    resp.success = true;
    resp.message = "pong";
    break;
  }

  case apm::ipc::RequestType::ModuleInstall: {
    apm::logger::info("AMSD: ModuleInstall request");
    if (req.modulePath.empty()) {
      resp.success = false;
      resp.message = "module_path is missing";
      break;
    }

    apm::ams::ModuleOperationResult result;
    if (!moduleManager_.installFromZip(req.modulePath, result)) {
      resp.success = false;
      resp.message =
          result.message.empty() ? "Module install failed" : result.message;
    } else {
      resp.success = result.ok;
      resp.message = result.message;
    }
    break;
  }

  case apm::ipc::RequestType::ModuleEnable:
  case apm::ipc::RequestType::ModuleDisable:
  case apm::ipc::RequestType::ModuleRemove: {
    const std::string moduleName =
        !req.moduleName.empty() ? req.moduleName : req.packageName;
    if (moduleName.empty()) {
      resp.success = false;
      resp.message = "module name missing";
      break;
    }

    apm::ams::ModuleOperationResult result;
    bool ok = false;
    if (req.type == apm::ipc::RequestType::ModuleEnable)
      ok = moduleManager_.enableModule(moduleName, result);
    else if (req.type == apm::ipc::RequestType::ModuleDisable)
      ok = moduleManager_.disableModule(moduleName, result);
    else
      ok = moduleManager_.removeModule(moduleName, result);

    resp.success = ok && result.ok;
    resp.message = result.message.empty()
                       ? (resp.success ? "Module operation succeeded"
                                       : "Module operation failed")
                       : result.message;
    break;
  }

  case apm::ipc::RequestType::ModuleList: {
    apm::logger::info("AMSD: ModuleList request");
    std::vector<apm::ams::ModuleStatusEntry> modules;
    std::string listErr;
    if (!moduleManager_.listModules(modules, &listErr)) {
      resp.success = false;
      resp.message = listErr.empty() ? "Failed to enumerate modules" : listErr;
      break;
    }

    std::ostringstream body;
    auto yesNo = [](bool v) { return v ? "yes" : "no"; };

    if (modules.empty()) {
      body << "No modules installed.";
    } else {
      for (std::size_t idx = 0; idx < modules.size(); ++idx) {
        const auto &entry = modules[idx];
        body << "Module: " << entry.info.name << "\n";
        if (!entry.info.version.empty())
          body << "  Version: " << entry.info.version << "\n";
        if (!entry.info.author.empty())
          body << "  Author: " << entry.info.author << "\n";
        if (!entry.info.description.empty())
          body << "  Description: " << entry.info.description << "\n";
        body << "  Enabled: " << yesNo(entry.state.enabled) << "\n";
        body << "  Mount: " << yesNo(entry.info.mount) << "\n";
        body << "  post-fs-data: " << yesNo(entry.info.runPostFsData) << "\n";
        body << "  service: " << yesNo(entry.info.runService) << "\n";
        body << "  install-sh: " << yesNo(entry.info.runInstallSh) << "\n";
        if (!entry.state.installedAt.empty())
          body << "  Installed: " << entry.state.installedAt << "\n";
        if (!entry.state.updatedAt.empty())
          body << "  Updated: " << entry.state.updatedAt << "\n";
        if (!entry.state.lastError.empty())
          body << "  Last-Error: " << entry.state.lastError << "\n";
        if (!entry.path.empty())
          body << "  Path: " << entry.path << "\n";
        if (idx + 1 < modules.size())
          body << "\n";
      }
    }

    resp.success = true;
    resp.message = body.str();
    break;
  }

  default: {
    resp.success = false;
    resp.message = "Unsupported request type";
    break;
  }
  }

  finalizeStatus(resp);
}

} // namespace apm::amsd
