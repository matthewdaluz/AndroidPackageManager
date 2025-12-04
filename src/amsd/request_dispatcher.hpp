/*
 * AMSD - APM Module System Daemon
 *
 * Handle AMS IPC requests for module operations with shared-session auth.
 */

/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: request_dispatcher.hpp
 * Purpose: Declare the AMSD IPC dispatcher that validates sessions and routes
 *          module commands to ModuleManager.
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

#pragma once

#include "ams/module_manager.hpp"
#include "protocol.hpp"
#include "security_manager.hpp"

namespace apm::amsd {

class RequestDispatcher {
public:
  RequestDispatcher(apm::ams::ModuleManager &moduleManager,
                    SecurityManager &securityManager);

  void dispatch(const apm::ipc::Request &req, apm::ipc::Response &resp) const;

private:
  apm::ams::ModuleManager &moduleManager_;
  SecurityManager &securityManager_;
};

} // namespace apm::amsd
