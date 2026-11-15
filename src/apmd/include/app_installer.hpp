// SPDX-License-Identifier: GPL-3.0-or-later
// APM - Android Package Manager

#pragma once
#include "install_manager.hpp"
#include "repo_index.hpp"
#include <string>
#include <vector>

namespace apm::apps {
struct InstallOptions {
  bool simulate = false;
  std::string destination = "app"; // app or priv-app beneath /system
  std::vector<std::string> permissions;
  std::string expectedPlan;
};
struct InstallResult {
  bool ok = false;
  std::string message;
  std::string plan;
  bool changed = false;
};
std::string statusKey(const std::string &id);
bool install(const repo::PackageEntry &bundle, const InstallOptions &options,
             InstallResult &out,
             install::InstallProgressCallback progress = {});
bool remove(const std::string &id, std::string &message);
} // namespace apm::apps
