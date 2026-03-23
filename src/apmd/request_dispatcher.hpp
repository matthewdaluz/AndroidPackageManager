/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: request_dispatcher.hpp
 * Purpose: Declare a reusable request dispatcher that executes apmd commands
 * using a supplied progress callback. Binder shims (kept in binder_* files)
 * can reuse this if that transport is ever re-enabled.
 * Last Modified: 2026-03-15 11:56:16.537055051 -0400.
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

#include "ams/module_manager.hpp"
#include "protocol.hpp"
#include "security_manager.hpp"

#include <functional>

namespace apm::ipc {

using ProgressCallback = std::function<void(const apm::ipc::Response &)>;

class RequestDispatcher {
public:
  RequestDispatcher(apm::ams::ModuleManager &moduleManager,
                    apm::daemon::SecurityManager &securityManager);

  // Execute the provided request, writing the final response to |resp| and
  // emitting any progress frames via |progressCb| if supplied.
  void dispatch(const apm::ipc::Request &req, apm::ipc::Response &resp,
                const ProgressCallback &progressCb) const;

private:
  apm::ams::ModuleManager &m_moduleManager;
  apm::daemon::SecurityManager &m_securityManager;
};

} // namespace apm::ipc
