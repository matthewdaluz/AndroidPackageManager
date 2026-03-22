/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: ipc_server.cpp
 * Purpose: Implement the apmd UNIX socket server, request dispatch, and
 * response helpers. Last Modified: March 15th, 2026. - 10:51 PM EDT.
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

#include "include/ipc_server.hpp"
#include "config.hpp"
#include "include/apk_installer.hpp"
#include "export_path.hpp"
#include "factory_reset.hpp"
#include "install_manager.hpp"
#include "logger.hpp"
#include "protocol.hpp"
#include "repo_index.hpp"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace apm::ipc {

namespace {

constexpr std::size_t kMaxRequestBytes = 64 * 1024;

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
static std::string joinPackages(const std::vector<std::string> &pkgs) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < pkgs.size(); ++i) {
    if (i > 0)
      oss << ' ';
    oss << pkgs[i];
  }
  return oss.str();
}

// Join arbitrary strings with a delimiter for structured response fields.
static std::string joinStrings(const std::vector<std::string> &items,
                               const std::string &delimiter) {
  std::ostringstream oss;
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (i > 0) {
      oss << delimiter;
    }
    oss << items[i];
  }
  return oss.str();
}

// Return just the basename component of a path (used for UI labels).
static std::string fileNameFromPath(const std::string &path) {
  if (path.empty())
    return "";
  auto pos = path.find_last_of('/');
  if (pos == std::string::npos || pos + 1 >= path.size())
    return path;
  return path.substr(pos + 1);
}

// Serialize and send a response frame to the client. Progress frames are sent
// as-is; other responses are coerced into Ok/Error status automatically.
static bool writeAll(int fd, const char *data, std::size_t len,
                     std::string *errorMsg) {
  std::size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::write(fd, data + sent, len - sent);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errorMsg)
        *errorMsg = "write() failed: " + std::string(std::strerror(errno));
      return false;
    }
    if (n == 0) {
      if (errorMsg)
        *errorMsg = "write() returned 0";
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

void sendResponseMessage(int clientFd, Response resp) {
  if (resp.status == ResponseStatus::Unknown) {
    resp.status = resp.success ? ResponseStatus::Ok : ResponseStatus::Error;
  }
  std::string wire = serializeResponse(resp);
  std::string err;
  if (!writeAll(clientFd, wire.data(), wire.size(), &err)) {
    apm::logger::warn("IpcServer::sendResponseMessage: " + err);
  }
}

} // namespace

IpcServer::IpcServer(const std::string &socketPath,
                     apm::ams::ModuleManager &moduleManager)
    : m_listenFd(-1), m_socketPath(socketPath), m_running(false),
      m_moduleManager(moduleManager), m_securityManager() {}

IpcServer::~IpcServer() {
  if (m_listenFd >= 0) {
    ::close(m_listenFd);
  }
  if (!m_socketPath.empty()) {
    ::unlink(m_socketPath.c_str());
  }
}

// Create/bind/listen on the configured UNIX domain socket. Removes stale
// sockets before binding and configures the server for blocking accept().
bool IpcServer::start() {
  if (m_socketPath.empty()) {
    apm::logger::error("IpcServer::start: socket path is empty");
    return false;
  }

  // Remove stale socket if present
  ::unlink(m_socketPath.c_str());

  m_listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (m_listenFd < 0) {
    apm::logger::error("IpcServer::start: socket() failed: " +
                       std::string(std::strerror(errno)));
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;

  if (m_socketPath.size() >= sizeof(addr.sun_path)) {
    apm::logger::error("IpcServer::start: socket path too long: " +
                       m_socketPath);
    ::close(m_listenFd);
    m_listenFd = -1;
    return false;
  }

  std::strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);

  if (::bind(m_listenFd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
      0) {
    apm::logger::error("IpcServer::start: bind() failed: " +
                       std::string(std::strerror(errno)));
    ::close(m_listenFd);
    m_listenFd = -1;
    return false;
  }

  if (::listen(m_listenFd, 8) < 0) {
    apm::logger::error("IpcServer::start: listen() failed: " +
                       std::string(std::strerror(errno)));
    ::close(m_listenFd);
    m_listenFd = -1;
    return false;
  }

  // Set socket permissions (0600 for emulator mode, 0666 for Android)
  mode_t socketMode = apm::config::isEmulatorMode() ? 0600 : 0666;
  if (::chmod(m_socketPath.c_str(), socketMode) < 0) {
    apm::logger::warn("IpcServer::start: chmod() failed: " +
                      std::string(std::strerror(errno)));
    // Continue anyway - not a fatal error
  }

  m_running = true;
  apm::logger::info("apmd: listening on " + m_socketPath);
  return true;
}

// Accept client connections in a loop until stop() flips the running flag.
void IpcServer::run() {
  if (m_listenFd < 0) {
    apm::logger::error("IpcServer::run: server not started");
    return;
  }

  while (m_running) {
    int clientFd = ::accept(m_listenFd, nullptr, nullptr);
    if (clientFd < 0) {
      if (errno == EINTR)
        continue;
      apm::logger::error("IpcServer::run: accept() failed: " +
                         std::string(std::strerror(errno)));
      break;
    }

    handleClient(clientFd);
    ::close(clientFd);
  }

  apm::logger::info("IpcServer::run: stopping event loop");
}

// Signal the accept loop to exit after the current iteration.
void IpcServer::stop() { m_running = false; }

// Read, parse, and dispatch a single client request. Each request is served
// synchronously; expensive operations report progress over the socket.
void IpcServer::handleClient(int clientFd) {
  apm::logger::debug("IpcServer::handleClient: new client");

  // Read until blank line
  std::string buffer;
  char temp[512];

  while (true) {
    ssize_t n = ::read(clientFd, temp, sizeof(temp));
    if (n < 0) {
      if (errno == EINTR)
        continue;

      apm::logger::error("IpcServer::handleClient: read() failed: " +
                         std::string(std::strerror(errno)));
      return;
    }
    if (n == 0)
      break;

    const std::size_t incoming = static_cast<std::size_t>(n);
    if (buffer.size() + incoming > kMaxRequestBytes) {
      apm::logger::warn(
          "IpcServer::handleClient: rejecting oversized request (> " +
          std::to_string(kMaxRequestBytes) + " bytes)");

      Response badResp;
      badResp.success = false;
      badResp.message = "Bad request: request too large";
      sendResponseMessage(clientFd, badResp);
      return;
    }

    buffer.append(temp, incoming);

    if (buffer.find("\n\n") != std::string::npos)
      break;
  }

  Request req;
  std::string parseErr;

  if (!parseRequest(buffer, req, &parseErr)) {
    apm::logger::error("IpcServer::handleClient: parseRequest failed: " +
                       parseErr);

    Response badResp;
    badResp.success = false;
    badResp.message = "Bad request: " + parseErr;
    sendResponseMessage(clientFd, badResp);
    return;
  }

  apm::logger::info("IpcServer::handleClient: received request");

  Response resp;
  resp.id = req.id;

  auto requiresAuth = [](RequestType t) {
    return !(t == RequestType::Ping || t == RequestType::Authenticate ||
             t == RequestType::ForgotPassword);
  };

  if (requiresAuth(req.type)) {
    std::string authErr;
    if (!m_securityManager.validateSessionToken(req.sessionToken, &authErr)) {
      resp.success = false;
      resp.message = authErr.empty() ? "Authentication required" : authErr;
      resp.rawFields["auth"] = "required";
      sendResponseMessage(clientFd, resp);
      return;
    }
  }

  switch (req.type) {

  case RequestType::Authenticate: {
    apm::logger::info("IpcServer: Authenticate request received");
    apm::security::SessionState session;
    std::string authErr;
    // Collect security questions/answers if setting a new secret
    std::vector<std::pair<std::string, std::string>> securityQa;
    if (req.authAction == "set") {
      bool complete = true;
      for (std::size_t idx = 0; idx < apm::security::SECURITY_QUESTION_COUNT;
           ++idx) {
        auto qIt = req.rawFields.find("security_q" + std::to_string(idx + 1));
        auto aIt = req.rawFields.find("security_a" + std::to_string(idx + 1));
        if (qIt == req.rawFields.end() || aIt == req.rawFields.end() ||
            qIt->second.empty() || aIt->second.empty()) {
          complete = false;
          break;
        }
        securityQa.emplace_back(qIt->second, aIt->second);
      }
      if (!complete) {
        resp.success = false;
        resp.message =
            "Security questions must be provided when setting a password/PIN.";
        break;
      }
    }
    if (!m_securityManager.authenticate(req.authAction, req.authSecret,
                                        securityQa, session, &authErr)) {
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

  case RequestType::ForgotPassword: {
    auto collectAnswers = [&](std::vector<std::string> &answersOut) -> bool {
      answersOut.clear();
      for (std::size_t idx = 0; idx < apm::security::SECURITY_QUESTION_COUNT;
           ++idx) {
        const std::string key = "security_a" + std::to_string(idx + 1);
        auto it = req.rawFields.find(key);
        if (it == req.rawFields.end() || it->second.empty())
          return false;
        answersOut.push_back(it->second);
      }
      return true;
    };

    std::string stage = "questions";
    auto itStage = req.rawFields.find("reset_stage");
    if (itStage != req.rawFields.end() && !itStage->second.empty())
      stage = itStage->second;

    if (stage == "questions") {
      std::vector<std::string> questions;
      std::string err;
      if (!m_securityManager.fetchSecurityQuestions(questions, &err)) {
        resp.success = false;
        resp.message = err.empty() ? "Unable to load security questions" : err;
        break;
      }

      resp.success = true;
      resp.message =
          "Answer the security questions to reset your password/PIN.";
      for (std::size_t idx = 0; idx < questions.size(); ++idx) {
        resp.rawFields["security_q" + std::to_string(idx + 1)] = questions[idx];
      }
      break;
    }

    if (stage == "verify") {
      std::vector<std::string> answers;
      if (!collectAnswers(answers)) {
        resp.success = false;
        resp.message = "All security questions must be answered.";
        break;
      }

      std::string err;
      if (!m_securityManager.validateSecurityAnswers(answers, &err)) {
        resp.success = false;
        resp.message = err.empty() ? "Security answers did not match" : err;
        break;
      }

      resp.success = true;
      resp.message = "Security answers verified. Set a new password/PIN.";
      break;
    }

    if (stage == "reset") {
      std::vector<std::string> answers;
      if (!collectAnswers(answers)) {
        resp.success = false;
        resp.message = "All security questions must be answered.";
        break;
      }

      std::string newSecret;
      auto itSecret = req.rawFields.find("new_secret");
      if (itSecret != req.rawFields.end())
        newSecret = itSecret->second;

      if (newSecret.empty()) {
        resp.success = false;
        resp.message = "New password/PIN is required.";
        break;
      }

      apm::security::SessionState session;
      std::string err;
      if (!m_securityManager.resetForgottenSecret(newSecret, answers, session,
                                                  &err)) {
        resp.success = false;
        resp.message =
            err.empty() ? "Failed to reset password/PIN." : std::move(err);
        break;
      }

      resp.success = true;
      resp.message = "Password/PIN reset. Session active.";
      resp.rawFields["session_token"] = session.token;
      resp.rawFields["session_expires"] = std::to_string(session.expiresAt);
      break;
    }

    resp.success = false;
    resp.message = "Unknown forgot-password stage";
    break;
  }

  case RequestType::Upgrade: {
    apm::logger::info("IpcServer: Upgrade request received");

    // Build repo indices (like for install)
    apm::repo::RepoIndexList indices;
    std::string err;
    if (!apm::repo::buildRepoIndices(
            apm::config::getSourcesList(), apm::config::getListsDir(),
            apm::config::getDefaultArch(), indices, &err)) {
      resp.success = false;
      resp.message =
          err.empty() ? "Failed to load repository metadata (run 'apm update')"
                      : err;
      break;
    }

    // Parse optional target packages passed via raw field "targets"
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
    // Later we can read simulate/distUpgrade flags from req.rawFields

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

  case RequestType::Ping: {
    resp.success = true;
    resp.message = "pong";
    break;
  }

  case RequestType::Install: {
    apm::logger::info("IpcServer: Install request for package: " +
                      req.packageName);

    // 1) Build repo indices (sources.list + Packages files)
    apm::repo::RepoIndexList indices;
    std::string err;
    if (!apm::repo::buildRepoIndices(
            apm::config::getSourcesList(), apm::config::getListsDir(),
            apm::config::getDefaultArch(), indices, &err)) {
      resp.success = false;
      resp.message =
          err.empty() ? "Failed to load repository metadata (run 'apm update')"
                      : err;
      break;
    }

    // 2) Extract install options (currently simulate / reinstall)
    apm::install::InstallOptions opts;
    {
      auto it = req.rawFields.find("simulate");
      if (it != req.rawFields.end()) {
        const std::string &v = it->second;
        if (v == "1" || v == "true" || v == "yes")
          opts.simulate = true;
      }
      it = req.rawFields.find("reinstall");
      if (it != req.rawFields.end()) {
        const std::string &v = it->second;
        if (v == "1" || v == "true" || v == "yes")
          opts.reinstall = true;
      }
    }

    apm::install::InstallResult res;
    auto installProgressCb = [&](const apm::install::InstallProgress &prog) {
      Response evt;
      evt.status = ResponseStatus::Progress;
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
      evt.rawFields["dl_speed"] = std::to_string(prog.downloadSpeedBytesPerSec);
      evt.rawFields["ul_speed"] = std::to_string(prog.uploadSpeedBytesPerSec);
      evt.rawFields["finished"] = prog.finished ? "1" : "0";
      sendResponseMessage(clientFd, evt);
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
    if (!res.activatedCommands.empty())
      resp.rawFields["activated_commands"] =
          joinPackages(res.activatedCommands);
    if (!res.namespacedCommands.empty())
      resp.rawFields["namespaced_commands"] =
          joinPackages(res.namespacedCommands);
    if (!res.collisionWarnings.empty())
      resp.rawFields["collision_warnings"] =
          joinStrings(res.collisionWarnings, " || ");
    break;
  }

  case RequestType::Remove: {
    apm::logger::info("IpcServer: Remove request for package: " +
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
    if (!res.activatedCommands.empty())
      resp.rawFields["activated_commands"] =
          joinPackages(res.activatedCommands);
    if (!res.namespacedCommands.empty())
      resp.rawFields["namespaced_commands"] =
          joinPackages(res.namespacedCommands);
    if (!res.collisionWarnings.empty())
      resp.rawFields["collision_warnings"] =
          joinStrings(res.collisionWarnings, " || ");
    break;
  }

  case RequestType::Autoremove: {
    apm::logger::info("IpcServer: Autoremove request received");

    apm::install::RemoveOptions opts;
    apm::install::AutoremoveResult ares;
    if (!apm::install::autoremove(opts, ares)) {
      resp.success = false;
      resp.message = ares.message.empty() ? "Autoremove failed" : ares.message;
      break;
    }

    resp.success = ares.ok;
    resp.message = ares.message;
    if (!ares.removedPackages.empty()) {
      resp.rawFields["removed"] = joinPackages(ares.removedPackages);
    }
    break;
  }

  // ------------------------------------------------------------
  // APK INSTALL
  // ------------------------------------------------------------
  case RequestType::ApkInstall: {
    apm::logger::info("IpcServer: ApkInstall request");

    // apkPath must be sent in rawFields
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

  // ------------------------------------------------------------
  // APK UNINSTALL
  // ------------------------------------------------------------
  case RequestType::ApkUninstall: {
    apm::logger::info("IpcServer: ApkUninstall request for " + req.packageName);

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

  case RequestType::Update: {
    apm::logger::info("IpcServer: Update request received");

    std::string summary;
    std::string err;
    auto progressCb = [&](const apm::repo::RepoUpdateProgress &prog) {
      Response evt;
      evt.status = ResponseStatus::Progress;
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
      sendResponseMessage(clientFd, evt);
    };

    bool ok = apm::repo::updateFromSourcesList(
        apm::config::getSourcesList(), apm::config::getListsDir(),
        apm::config::getDefaultArch(), &summary, &err, progressCb);

    if (ok) {
      resp.success = true;
      resp.message = summary.empty() ? "Repositories updated" : summary;
    } else {
      resp.success = false;
      resp.message = err.empty() ? "Repository update failed" : err;
    }
    break;
  }

  case RequestType::ModuleInstall: {
    apm::logger::info("IpcServer: ModuleInstall request");
    if (req.modulePath.empty()) {
      resp.success = false;
      resp.message = "module_path is missing";
      break;
    }

    apm::ams::ModuleOperationResult result;
    if (!m_moduleManager.installFromZip(req.modulePath, result)) {
      resp.success = false;
      resp.message =
          result.message.empty() ? "Module install failed" : result.message;
    } else {
      resp.success = result.ok;
      resp.message = result.message;
    }
    break;
  }

  case RequestType::ModuleEnable:
  case RequestType::ModuleDisable:
  case RequestType::ModuleRemove: {
    const std::string moduleName =
        !req.moduleName.empty() ? req.moduleName : req.packageName;
    if (moduleName.empty()) {
      resp.success = false;
      resp.message = "module name missing";
      break;
    }

    apm::ams::ModuleOperationResult result;
    bool ok = false;
    if (req.type == RequestType::ModuleEnable)
      ok = m_moduleManager.enableModule(moduleName, result);
    else if (req.type == RequestType::ModuleDisable)
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

  case RequestType::ModuleList: {
    apm::logger::info("IpcServer: ModuleList request");
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

  case RequestType::FactoryReset: {
    apm::logger::info("IpcServer: FactoryReset request received");

    apm::daemon::FactoryResetResult result;
    if (!apm::daemon::performFactoryReset(m_moduleManager, result)) {
      resp.success = false;
      resp.message =
          result.message.empty() ? "Factory reset failed" : result.message;
    } else {
      apm::daemon::path::CommandHotloadSummary hotloadSummary;
      if (!apm::daemon::path::rebuild_command_index_and_shims(
              "factory-reset", &hotloadSummary)) {
        apm::logger::warn("IpcServer: hotload rebuild reported warnings after "
                          "factory reset");
      }
      resp.success = true;
      resp.message =
          result.message.empty() ? "Factory reset completed." : result.message;
    }
    break;
  }

  default: {
    resp.success = false;
    resp.message = "Unsupported request type";
    break;
  }
  }

  sendResponseMessage(clientFd, resp);
  apm::logger::debug("IpcServer::handleClient: response sent");
}

} // namespace apm::ipc
