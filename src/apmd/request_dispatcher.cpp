/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: request_dispatcher.cpp
 * Purpose: Implement the reusable request dispatcher shared by Binder and the
 * legacy IPC server. It converts parsed requests into responses and emits
 * progress callbacks when available.
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

#include "request_dispatcher.hpp"

#include "config.hpp"
#include "include/apk_installer.hpp"
#include "factory_reset.hpp"
#include "install_manager.hpp"
#include "logger.hpp"
#include "repo_index.hpp"

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace apm::ipc {

namespace {

// Convert a repo update stage enum into a compact logging-friendly string.
std::string stageToString(apm::repo::RepoUpdateStage stage) {
  switch (stage) {
  case apm::repo::RepoUpdateStage::DownloadRelease:
    return "download-release";
  case apm::repo::RepoUpdateStage::DownloadReleaseSignature:
    return "download-release-signature";
  case apm::repo::RepoUpdateStage::DownloadPackages:
    return "download-packages";
  case apm::repo::RepoUpdateStage::ParsePackages:
    return "parse-packages";
  default:
    return "unknown";
  }
}

// Join a vector of package names into a CLI-ready string for responses.
std::string joinPackages(const std::vector<std::string> &pkgs) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < pkgs.size(); ++i) {
    if (i > 0)
      oss << ' ';
    oss << pkgs[i];
  }
  return oss.str();
}

// Return just the basename component of a path (used for UI labels).
std::string fileNameFromPath(const std::string &path) {
  if (path.empty())
    return "";
  auto pos = path.find_last_of('/');
  if (pos == std::string::npos || pos + 1 >= path.size())
    return path;
  return path.substr(pos + 1);
}

// Normalize status to Ok/Error for final responses when the dispatcher does
// not explicitly set a status value.
void finalizeStatus(Response &resp) {
  if (resp.status == ResponseStatus::Unknown) {
    resp.status = resp.success ? ResponseStatus::Ok : ResponseStatus::Error;
  }
}

// Helper to parse common truthy string toggles.
bool isTruthy(const std::string &value) {
  return value == "1" || value == "true" || value == "yes";
}

} // namespace

RequestDispatcher::RequestDispatcher(apm::ams::ModuleManager &moduleManager,
                                     apm::daemon::SecurityManager &securityManager)
    : m_moduleManager(moduleManager), m_securityManager(securityManager) {}

void RequestDispatcher::dispatch(const apm::ipc::Request &req,
                                 apm::ipc::Response &resp,
                                 const ProgressCallback &progressCb) const {
  resp = apm::ipc::Response{};
  resp.id = req.id;

  auto sendProgress = [&](apm::ipc::Response progress) {
    if (!progressCb)
      return;
    if (progress.status == apm::ipc::ResponseStatus::Unknown) {
      progress.status = apm::ipc::ResponseStatus::Progress;
    }
    progressCb(progress);
  };

  auto requiresAuth = [](apm::ipc::RequestType t) {
    return !(t == apm::ipc::RequestType::Ping ||
             t == apm::ipc::RequestType::Authenticate);
  };

  if (requiresAuth(req.type)) {
    std::string authErr;
    if (!m_securityManager.validateSessionToken(req.sessionToken, &authErr)) {
      resp.success = false;
      resp.message = authErr.empty() ? "Authentication required" : authErr;
      resp.rawFields["auth"] = "required";
      finalizeStatus(resp);
      return;
    }
  }

  switch (req.type) {

  case apm::ipc::RequestType::Authenticate: {
    apm::logger::info("RequestDispatcher: Authenticate request received");
    apm::security::SessionState session;
    std::string authErr;
    if (!m_securityManager.authenticate(req.authAction, req.authSecret,
                                        session, &authErr)) {
      resp.success = false;
      resp.message =
          authErr.empty() ? "Authentication failed" : std::move(authErr);
      break;
    }

    resp.success = true;
    resp.message = (req.authAction == "set")
                       ? "Password/PIN saved. Session active."
                       : "Session unlocked.";
    resp.rawFields["session_token"] = session.token;
    resp.rawFields["session_expires"] = std::to_string(session.expiresAt);
    break;
  }

  case apm::ipc::RequestType::Upgrade: {
    apm::logger::info("RequestDispatcher: Upgrade request received");

    apm::repo::RepoIndexList indices;
    std::string err;
    if (!apm::repo::buildRepoIndices(
            apm::config::SOURCES_LIST, apm::config::LISTS_DIR,
            apm::config::DEFAULT_ARCH, indices, &err)) {
      resp.success = false;
      resp.message =
          err.empty() ? "Failed to load repository metadata (run 'apm update')"
                      : err;
      break;
    }

    std::vector<std::string> targets;
    auto it = req.rawFields.find("targets");
    if (it != req.rawFields.end()) {
      std::istringstream ss(it->second);
      std::string name;
      while (ss >> name) {
        targets.push_back(name);
      }
    }

    apm::install::UpgradeOptions opts;
    apm::install::UpgradeResult ures;
    if (!apm::install::upgradePackages(indices, targets, opts, ures)) {
      resp.success = false;
      resp.message = ures.message;
      break;
    }

    resp.success = ures.ok;
    resp.message = ures.message;
    break;
  }

  case apm::ipc::RequestType::Ping: {
    resp.success = true;
    resp.message = "pong";
    break;
  }

  case apm::ipc::RequestType::Install: {
    apm::logger::info("RequestDispatcher: Install request for package: " +
                      req.packageName);

    apm::repo::RepoIndexList indices;
    std::string err;
    if (!apm::repo::buildRepoIndices(
            apm::config::SOURCES_LIST, apm::config::LISTS_DIR,
            apm::config::DEFAULT_ARCH, indices, &err)) {
      resp.success = false;
      resp.message =
          err.empty() ? "Failed to load repository metadata (run 'apm update')"
                      : err;
      break;
    }

    apm::install::InstallOptions opts;
    {
      auto it = req.rawFields.find("simulate");
      if (it != req.rawFields.end() && isTruthy(it->second)) {
        opts.simulate = true;
      }
      it = req.rawFields.find("reinstall");
      if (it != req.rawFields.end() && isTruthy(it->second)) {
        opts.reinstall = true;
      }
    }

    apm::install::InstallResult res;
    auto installProgressCb = [&](const apm::install::InstallProgress &prog) {
      apm::ipc::Response evt;
      evt.status = apm::ipc::ResponseStatus::Progress;
      evt.id = req.id;
      if (prog.event == apm::install::InstallProgressEvent::Download) {
        evt.rawFields["event"] = "install-download";
      } else {
        evt.rawFields["event"] = "install-progress";
      }
      evt.rawFields["package"] = prog.packageName;
      evt.rawFields["url"] = prog.url;
      evt.rawFields["destination"] = prog.destination;
      evt.rawFields["file"] = fileNameFromPath(prog.destination);
      evt.rawFields["bytes"] = std::to_string(prog.downloadedBytes);
      evt.rawFields["total"] = std::to_string(prog.totalBytes);
      evt.rawFields["uploaded"] = std::to_string(prog.uploadedBytes);
      evt.rawFields["upload_total"] = std::to_string(prog.uploadTotal);
      evt.rawFields["dl_speed"] =
          std::to_string(prog.downloadSpeedBytesPerSec);
      evt.rawFields["ul_speed"] =
          std::to_string(prog.uploadSpeedBytesPerSec);
      evt.rawFields["finished"] = prog.finished ? "1" : "0";
      sendProgress(evt);
    };

    if (!apm::install::installWithDeps(indices, req.packageName, opts, res,
                                       installProgressCb)) {
      resp.success = false;
      resp.message = res.message.empty() ? "Install failed" : res.message;
      break;
    }

    resp.success = true;
    resp.message = res.message;
    if (!res.installedPackages.empty())
      resp.rawFields["packages"] = joinPackages(res.installedPackages);
    if (!res.skippedPackages.empty())
      resp.rawFields["skipped"] = joinPackages(res.skippedPackages);
    break;
  }

  case apm::ipc::RequestType::Remove: {
    apm::logger::info("RequestDispatcher: Remove request for package: " +
                      req.packageName);

    if (req.packageName.empty()) {
      resp.success = false;
      resp.message = "No package name provided";
      break;
    }

    apm::install::RemoveOptions opts;
    apm::install::RemoveResult res;

    if (!apm::install::removePackage(req.packageName, opts, res)) {
      resp.success = false;
      resp.message = res.message;
      break;
    }

    resp.success = true;
    resp.message = res.message;
    break;
  }

  case apm::ipc::RequestType::ApkInstall: {
    apm::logger::info("RequestDispatcher: ApkInstall request");

    auto it = req.rawFields.find("apkPath");
    if (it == req.rawFields.end() || it->second.empty()) {
      resp.success = false;
      resp.message = "Missing 'apkPath' field";
      break;
    }

    std::string apkPath = it->second;

    apm::apk::ApkInstallOptions opts;
    auto itSys = req.rawFields.find("installAsSystem");
    if (itSys != req.rawFields.end() && itSys->second == "1") {
      opts.installAsSystem = true;
    }

    apm::apk::ApkInstallResult r;
    if (!apm::apk::installApk(apkPath, opts, r)) {
      resp.success = false;
      resp.message = r.message;
    } else {
      resp.success = true;
      resp.message = r.message;
    }
    break;
  }

  case apm::ipc::RequestType::ApkUninstall: {
    apm::logger::info("RequestDispatcher: ApkUninstall request for " +
                      req.packageName);

    if (req.packageName.empty()) {
      resp.success = false;
      resp.message = "Missing package name";
      break;
    }

    apm::apk::ApkUninstallResult r;
    if (!apm::apk::uninstallApk(req.packageName, r)) {
      resp.success = false;
      resp.message = r.message;
    } else {
      resp.success = true;
      resp.message = r.message;
    }
    break;
  }

  case apm::ipc::RequestType::Update: {
    apm::logger::info("RequestDispatcher: Update request received");

    std::string summary;
    std::string err;
    auto progressCbWrapper = [&](const apm::repo::RepoUpdateProgress &prog) {
      apm::ipc::Response evt;
      evt.status = apm::ipc::ResponseStatus::Progress;
      evt.id = req.id;
      evt.rawFields["event"] = "repo-update";
      evt.rawFields["stage"] = stageToString(prog.stage);
      evt.rawFields["repo"] = prog.repoUri;
      evt.rawFields["dist"] = prog.dist;
      evt.rawFields["component"] = prog.component;
      evt.rawFields["remote"] = prog.remotePath;
      evt.rawFields["local"] = prog.localPath;
      evt.rawFields["description"] = prog.description;
      evt.rawFields["bytes"] = std::to_string(prog.currentBytes);
      evt.rawFields["total"] = std::to_string(prog.totalBytes);
      evt.rawFields["dl_speed"] = std::to_string(prog.downloadSpeed);
      evt.rawFields["ul_speed"] = std::to_string(prog.uploadSpeed);
      evt.rawFields["finished"] = prog.finished ? "1" : "0";
      sendProgress(evt);
    };

    bool ok = apm::repo::updateFromSourcesList(
        apm::config::SOURCES_LIST, apm::config::LISTS_DIR,
        apm::config::DEFAULT_ARCH, &summary, &err, progressCbWrapper);

    if (ok) {
      resp.success = true;
      resp.message = summary.empty() ? "Repositories updated" : summary;
    } else {
      resp.success = false;
      resp.message = err.empty() ? "Repository update failed" : err;
    }
    break;
  }

  case apm::ipc::RequestType::ModuleInstall: {
    apm::logger::info("RequestDispatcher: ModuleInstall request");
    if (req.modulePath.empty()) {
      resp.success = false;
      resp.message = "module_path is missing";
      break;
    }

    apm::ams::ModuleOperationResult result;
    if (!m_moduleManager.installFromZip(req.modulePath, result)) {
      resp.success = false;
      resp.message = result.message.empty() ? "Module install failed"
                                            : result.message;
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
      ok = m_moduleManager.enableModule(moduleName, result);
    else if (req.type == apm::ipc::RequestType::ModuleDisable)
      ok = m_moduleManager.disableModule(moduleName, result);
    else
      ok = m_moduleManager.removeModule(moduleName, result);

    resp.success = ok && result.ok;
    resp.message = result.message.empty()
                       ? (resp.success ? "Module operation succeeded"
                                       : "Module operation failed")
                       : result.message;
    break;
  }

  case apm::ipc::RequestType::ModuleList: {
    apm::logger::info("RequestDispatcher: ModuleList request");
    std::vector<apm::ams::ModuleStatusEntry> modules;
    std::string listErr;
    if (!m_moduleManager.listModules(modules, &listErr)) {
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

  case apm::ipc::RequestType::FactoryReset: {
    apm::logger::info("RequestDispatcher: FactoryReset request received");

    apm::daemon::FactoryResetResult result;
    if (!apm::daemon::performFactoryReset(m_moduleManager, result)) {
      resp.success = false;
      resp.message =
          result.message.empty() ? "Factory reset failed" : result.message;
    } else {
      resp.success = true;
      resp.message = result.message.empty() ? "Factory reset completed."
                                            : result.message;
    }
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

} // namespace apm::ipc
