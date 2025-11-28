/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: apk_install.cpp
 * Purpose: Implement the CLI wrapper for forwarding apk-install requests to
 * the daemon.
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

#include "apk_install.hpp"
#include "config.hpp"
#include "binder_client.hpp"

#include <iostream>

namespace apm::cli {

// Parse command-line arguments for apk-install and forward the request to the
// daemon using the IPC client. Returns shell-style status code.
int apkInstall(const std::vector<std::string> &args) {
  if (args.empty()) {
    std::cerr << "apm: 'apk-install' requires an APK path\n";
    return 1;
  }

  std::string apkPath = args[0];
  bool installAsSystem = false;

  // Parse optional flags
  for (size_t i = 1; i < args.size(); i++) {
    if (args[i] == "--install-as-system") {
      installAsSystem = true;
    } else {
      std::cerr << "Unknown argument: " << args[i] << "\n";
      return 1;
    }
  }

  // Build IPC request for apmd
  apm::ipc::Request req;
  req.type = apm::ipc::RequestType::ApkInstall;

  // Store fields as rawFields (matches daemon request parsing)
  req.rawFields["apkPath"] = apkPath;
  req.rawFields["installAsSystem"] = installAsSystem ? "1" : "0";

  apm::ipc::Response resp;
  std::string err;

  if (!apm::ipc::sendRequest(req, resp, apm::config::BINDER_SERVICE, &err)) {
    std::cerr << "APK install failed: " << err << "\n";
    return 1;
  }

  if (!resp.success) {
    std::cerr << "APK install failed: " << resp.message << "\n";
    return 1;
  }

  std::cout << "APK installed successfully!\n";
  return 0;
}

} // namespace apm::cli
