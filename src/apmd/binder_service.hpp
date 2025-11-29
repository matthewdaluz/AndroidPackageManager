/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: binder_service.hpp
 * Purpose: Declare the Binder-based service endpoint for apmd, allowing the
 * daemon to run as a native Android system service.
 * Last Modified: November 28th, 2025. - 8:59 AM Eastern Time.
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
#include "binder_support.hpp"
#include "request_dispatcher.hpp"
#include "security_manager.hpp"

#include <string>

namespace apm::ipc {

class BinderService {
public:
  BinderService(const std::string &instanceName,
                apm::ams::ModuleManager &moduleManager,
                apm::daemon::SecurityManager &securityManager);
  ~BinderService();

  // Register the service with the platform Service Manager. Returns false on
  // failure (Binder not available, registration failed, etc.).
  bool start(std::string *errorMsg = nullptr);

  // Enter the Binder thread pool loop. This call blocks.
  void joinThreadPool();

  // Whether start() succeeded.
  bool isStarted() const;

  // Internal helper used by the Binder service state to run requests.
  void dispatchRequest(apm::ipc::Request &req, apm::ipc::Response &resp,
                       const apm::ipc::ProgressCallback &progressCb);

private:
#if defined(__ANDROID__)
  std::string m_instanceName;
  apm::ipc::RequestDispatcher m_dispatcher;
  AIBinder *m_binder;
  bool m_started;
#endif
};

} // namespace apm::ipc
