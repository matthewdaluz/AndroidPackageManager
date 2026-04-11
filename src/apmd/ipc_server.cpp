/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: ipc_server.cpp
 * Purpose: Implement the apmd UNIX socket server, request dispatch, and
 * response helpers. Last Modified: 2026-03-22 12:40:07.402606623 -0400.
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
#include "fs.hpp"
#include "install_manager.hpp"
#include "logger.hpp"
#include "protocol.hpp"
#include "repo_index.hpp"
#include "status_db.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <grp.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <vector>

namespace apm::ipc {

namespace {

constexpr std::size_t kMaxRequestBytes = 64 * 1024;
constexpr const char *kLogFileTag = "ipc_server.cpp";

enum class LogClearTarget { Apm, Ams, Module, All };

bool isAbstractSocketName(const std::string &socketPath) {
  return !socketPath.empty() && socketPath.front() == '@';
}

std::string parentDir(const std::string &path) {
  auto slash = path.find_last_of('/');
  if (slash == std::string::npos)
    return ".";
  if (slash == 0)
    return "/";
  return path.substr(0, slash);
}

bool lookupShellGroup(gid_t &gidOut) {
  struct group *shellGroup = ::getgrnam("shell");
  if (!shellGroup)
    return false;
  gidOut = shellGroup->gr_gid;
  return true;
}

void ensurePathAccess(const std::string &path, mode_t mode) {
  if (path.empty())
    return;

  gid_t shellGid = 0;
  const bool haveShellGid = lookupShellGroup(shellGid);

  if (haveShellGid && ::chown(path.c_str(), 0, shellGid) < 0 &&
      errno != EPERM) {
    apm::logger::warn(std::string(kLogFileTag) + ": chown(" + path +
                      ") failed: " + std::strerror(errno));
  }

  if (::chmod(path.c_str(), mode) < 0) {
    apm::logger::warn(std::string(kLogFileTag) + ": chmod(" + path +
                      ") failed: " + std::strerror(errno));
  }
}

bool parseLogClearTarget(const Request &req, LogClearTarget &targetOut,
                         std::string &moduleNameOut, std::string *errorMsg) {
  moduleNameOut.clear();

  auto clearAllIt = req.rawFields.find("clear_all");
  if (clearAllIt != req.rawFields.end()) {
    const std::string &value = clearAllIt->second;
    if (value == "1" || value == "true" || value == "yes" || value == "on") {
      targetOut = LogClearTarget::All;
      return true;
    }
  }

  auto targetIt = req.rawFields.find("log_target");
  const std::string target =
      targetIt == req.rawFields.end() ? std::string() : targetIt->second;
  if (target == "apm" || target == "apmd") {
    targetOut = LogClearTarget::Apm;
    return true;
  }
  if (target == "ams" || target == "amsd") {
    targetOut = LogClearTarget::Ams;
    return true;
  }
  if (target == "module") {
    const std::string moduleName = req.moduleName;
    if (moduleName.empty()) {
      if (errorMsg)
        *errorMsg = "module name missing";
      return false;
    }
    if (moduleName == "." || moduleName == ".." ||
        moduleName.find('/') != std::string::npos ||
        moduleName.find('\\') != std::string::npos) {
      if (errorMsg)
        *errorMsg = "invalid module name";
      return false;
    }
    moduleNameOut = moduleName;
    targetOut = LogClearTarget::Module;
    return true;
  }

  if (errorMsg)
    *errorMsg = "invalid log clear target";
  return false;
}

bool parseBoolValue(std::string value, bool &out) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });

  if (value == "1" || value == "true" || value == "yes" || value == "on") {
    out = true;
    return true;
  }
  if (value == "0" || value == "false" || value == "no" || value == "off") {
    out = false;
    return true;
  }
  return false;
}

bool parseOptionalBoolField(const Request &req, const char *key, bool &out,
                            std::string *errorMsg) {
  auto it = req.rawFields.find(key);
  if (it == req.rawFields.end())
    return true;

  if (!parseBoolValue(it->second, out)) {
    if (errorMsg)
      *errorMsg = std::string("Invalid boolean value for '") + key + "'";
    return false;
  }
  return true;
}

bool parseWipeCacheSelection(const Request &req,
                             apm::daemon::WipeCacheSelection &selectionOut,
                             std::string *errorMsg) {
  selectionOut = apm::daemon::WipeCacheSelection{};

  bool wipeAll = false;
  if (!parseOptionalBoolField(req, "wipe_all", wipeAll, errorMsg) ||
      !parseOptionalBoolField(req, "wipe_apm_cache", selectionOut.apmGeneral,
                              errorMsg) ||
      !parseOptionalBoolField(req, "wipe_repo_lists", selectionOut.repoLists,
                              errorMsg) ||
      !parseOptionalBoolField(req, "wipe_package_downloads",
                              selectionOut.packageDownloads, errorMsg) ||
      !parseOptionalBoolField(req, "wipe_signature_cache",
                              selectionOut.signatureCache, errorMsg) ||
      !parseOptionalBoolField(req, "wipe_ams_runtime",
                              selectionOut.amsRuntime, errorMsg)) {
    return false;
  }

  if (wipeAll) {
    selectionOut.apmGeneral = true;
    selectionOut.repoLists = true;
    selectionOut.packageDownloads = true;
    selectionOut.signatureCache = true;
    selectionOut.amsRuntime = true;
  }

  if (!selectionOut.apmGeneral && !selectionOut.repoLists &&
      !selectionOut.packageDownloads && !selectionOut.signatureCache &&
      !selectionOut.amsRuntime) {
    if (errorMsg)
      *errorMsg = "No cache targets selected";
    return false;
  }

  return true;
}

void appendUniquePath(const std::string &path, std::vector<std::string> &paths) {
  if (path.empty())
    return;
  if (std::find(paths.begin(), paths.end(), path) == paths.end())
    paths.push_back(path);
}

bool endsWith(const std::string &value, const std::string &suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

void collectLogFilesInDir(const std::string &dir, std::vector<std::string> &paths) {
  if (dir.empty() || !apm::fs::isDirectory(dir))
    return;

  for (const auto &entry : apm::fs::listDir(dir, false)) {
    if (!endsWith(entry, ".log"))
      continue;
    const std::string path = apm::fs::joinPath(dir, entry);
    if (!apm::fs::isFile(path))
      continue;
    appendUniquePath(path, paths);
  }
}

std::vector<std::string> logPathsForTarget(LogClearTarget target,
                                           const std::string &moduleName) {
  std::vector<std::string> paths;
  switch (target) {
  case LogClearTarget::Apm:
    appendUniquePath(
        apm::fs::joinPath(apm::config::getLogsDir(), "apmd.log"), paths);
    appendUniquePath(
        apm::fs::joinPath(apm::config::getShellLogsDir(), "apmd.log"), paths);
    break;
  case LogClearTarget::Ams:
    appendUniquePath(
        apm::fs::joinPath(apm::config::getModuleLogsDir(), "amsd.log"), paths);
    appendUniquePath(
        apm::fs::joinPath(apm::config::getShellLogsDir(), "amsd.log"), paths);
    break;
  case LogClearTarget::Module:
    appendUniquePath(apm::fs::joinPath(apm::config::getModuleLogsDir(),
                                       moduleName + ".log"),
                     paths);
    break;
  case LogClearTarget::All:
    collectLogFilesInDir(apm::config::getLogsDir(), paths);
    collectLogFilesInDir(apm::config::getShellLogsDir(), paths);
    collectLogFilesInDir(apm::config::getModuleLogsDir(), paths);
    break;
  }
  return paths;
}

bool clearLogFiles(const std::vector<std::string> &paths, std::size_t &removedCount,
                   std::string *errorMsg) {
  removedCount = 0;
  for (const auto &path : paths) {
    if (path.empty() || !apm::fs::pathExists(path))
      continue;
    if (!apm::fs::removeFile(path)) {
      if (errorMsg)
        *errorMsg = "failed to remove log file: " + path;
      return false;
    }
    ++removedCount;
  }
  return true;
}

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

static std::string formatInstalledFrom(const apm::status::InstalledPackage &pkg) {
  std::ostringstream out;
  out << pkg.repoUri;
  if (!pkg.repoDist.empty()) {
    out << " " << pkg.repoDist;
  }
  if (!pkg.repoComponent.empty()) {
    out << " " << pkg.repoComponent;
  }
  return out.str();
}

static std::string formatRepoLocation(const apm::repo::PackageEntry &pkg) {
  std::ostringstream out;
  out << pkg.repoUri;
  if (!pkg.repoDist.empty()) {
    out << " " << pkg.repoDist;
  }
  if (!pkg.repoComponent.empty()) {
    out << " " << pkg.repoComponent;
  }
  return out.str();
}

static std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

static std::vector<std::string> splitLines(const std::string &value) {
  std::vector<std::string> out;
  std::istringstream in(value);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty()) {
      out.push_back(line);
    }
  }
  return out;
}

static bool buildListResponseMessage(std::string &messageOut) {
  apm::status::InstalledDb db;
  std::string err;
  if (!apm::status::loadStatus(db, &err)) {
    messageOut = "apm list: failed to load status DB: " + err;
    return false;
  }

  if (db.empty()) {
    messageOut = "No packages installed.";
    return true;
  }

  std::vector<std::string> names;
  names.reserve(db.size());
  for (const auto &kv : db) {
    names.push_back(kv.first);
  }
  std::sort(names.begin(), names.end());

  std::ostringstream out;
  out << "Installed packages:\n";
  for (const auto &name : names) {
    const auto &pkg = db.at(name);
    out << "  " << pkg.name;
    if (!pkg.version.empty()) {
      out << " " << pkg.version;
    }
    if (!pkg.architecture.empty()) {
      out << " [" << pkg.architecture << "]";
    }
    if (pkg.autoInstalled) {
      out << " (auto)";
    }
    out << "\n";
  }

  messageOut = out.str();
  if (!messageOut.empty() && messageOut.back() == '\n') {
    messageOut.pop_back();
  }
  return true;
}

static std::string buildInfoResponseMessage(const std::string &name) {
  std::ostringstream out;
  apm::status::InstalledPackage inst;
  std::string statusErr;
  const bool isInstalled = apm::status::isInstalled(name, &inst, &statusErr);

  out << "Package: " << name << "\n";
  if (!statusErr.empty()) {
    out << "Installed info unavailable: " << statusErr << "\n\n";
  } else {
    out << "Installed: " << (isInstalled ? "yes" : "no") << "\n";
    if (isInstalled) {
      if (!inst.version.empty())
        out << "Installed-Version: " << inst.version << "\n";
      if (!inst.architecture.empty())
        out << "Architecture: " << inst.architecture << "\n";
      if (!inst.installRoot.empty())
        out << "Installed-Root: " << inst.installRoot << "\n";
      if (!inst.status.empty())
        out << "Status: " << inst.status << "\n";
      if (inst.autoInstalled)
        out << "Auto-Installed: yes\n";
      if (!inst.repoUri.empty())
        out << "Installed-From: " << formatInstalledFrom(inst) << "\n";
    }
    out << "\n";
  }

  apm::repo::RepoIndexList indices;
  std::string repoErr;
  if (!apm::repo::buildRepoIndices(apm::config::getSourcesList(),
                                   apm::config::getListsDir(),
                                   apm::config::getDefaultArch(), indices,
                                   &repoErr)) {
    out << "Repository info unavailable";
    if (!repoErr.empty())
      out << ": " << repoErr;
    return out.str();
  }

  const apm::repo::PackageEntry *cand = nullptr;
  for (const auto &idx : indices) {
    const auto *found = apm::repo::findPackage(idx.packages, name, "");
    if (found) {
      cand = found;
      break;
    }
  }

  if (!cand) {
    out << "Not found in repositories.";
    return out.str();
  }

  out << "Candidate-Version: " << cand->version << "\n";
  if (!cand->architecture.empty())
    out << "Candidate-Architecture: " << cand->architecture << "\n";
  if (!cand->repoUri.empty())
    out << "Repository: " << formatRepoLocation(*cand) << "\n";
  if (!cand->filename.empty())
    out << "Filename: " << cand->filename << "\n";

  if (isInstalled && statusErr.empty() && !inst.version.empty() &&
      !cand->version.empty() && inst.version != cand->version) {
    out << "Upgrade-Available: yes\n";
    out << "  Installed: " << inst.version << "\n";
    out << "  Candidate: " << cand->version << "\n";
  }

  out << "\n=== Metadata ===\n";
  for (const auto &kv : cand->rawFields) {
    if (kv.first == "Description") {
      out << "Description: " << kv.second << "\n";
      auto itLong = cand->rawFields.find("Description-long");
      if (itLong != cand->rawFields.end()) {
        out << "\n" << itLong->second << "\n";
      }
      continue;
    }
    out << kv.first << ": " << kv.second << "\n";
  }

  return out.str();
}

static bool buildSearchResponseMessage(const std::vector<std::string> &patternsIn,
                                       std::string &messageOut) {
  if (patternsIn.empty()) {
    messageOut = "apm: 'search' requires a pattern";
    return false;
  }

  std::vector<std::string> patterns;
  patterns.reserve(patternsIn.size());
  for (const auto &pattern : patternsIn) {
    patterns.push_back(toLower(pattern));
  }

  apm::repo::RepoIndexList indices;
  std::string err;
  if (!apm::repo::buildRepoIndices(apm::config::getSourcesList(),
                                   apm::config::getListsDir(),
                                   apm::config::getDefaultArch(), indices,
                                   &err)) {
    messageOut = "apm search: failed to load repo indices";
    if (!err.empty()) {
      messageOut += ": " + err;
    }
    return false;
  }

  std::unordered_set<std::string> seen;
  std::size_t matchCount = 0;
  std::ostringstream out;

  for (const auto &idx : indices) {
    for (const auto &pkg : idx.packages) {
      std::string desc;
      auto it = pkg.rawFields.find("Description");
      if (it != pkg.rawFields.end()) {
        desc = it->second;
      }

      std::string hay = toLower(pkg.packageName);
      if (!desc.empty()) {
        hay.push_back(' ');
        hay += toLower(desc);
      }

      bool hit = false;
      for (const auto &pattern : patterns) {
        if (hay.find(pattern) != std::string::npos) {
          hit = true;
          break;
        }
      }

      if (!hit || !seen.insert(pkg.packageName).second) {
        continue;
      }

      ++matchCount;
      out << pkg.packageName;
      if (!pkg.version.empty()) {
        out << " " << pkg.version;
      }
      if (!pkg.architecture.empty()) {
        out << " [" << pkg.architecture << "]";
      }
      if (!desc.empty()) {
        out << " - " << desc;
      }
      out << "\n";
    }
  }

  if (matchCount == 0) {
    messageOut = "No packages found matching the given pattern(s).";
    return true;
  }

  messageOut = out.str();
  if (!messageOut.empty() && messageOut.back() == '\n') {
    messageOut.pop_back();
  }
  return true;
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

void sendResponseMessage(int clientFd, Response resp,
                         bool suppressLogging = false) {
  if (resp.status == ResponseStatus::Unknown) {
    resp.status = resp.success ? ResponseStatus::Ok : ResponseStatus::Error;
  }
  const bool debugEnabled = apm::logger::isDebugEnabled();
  if (debugEnabled && !suppressLogging) {
    apm::logger::debug(std::string(kLogFileTag) +
                       ": sendResponseMessage status=" +
                       (resp.success ? "ok" : "error") + " id='" + resp.id +
                       "' msg='" + resp.message + "'");
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

  const bool abstractSocket = isAbstractSocketName(m_socketPath);

  if (!apm::config::isEmulatorMode() && !abstractSocket) {
    ensurePathAccess(parentDir(m_socketPath), 0775);
  }

  // Remove stale socket if present
  if (!abstractSocket) {
    ::unlink(m_socketPath.c_str());
  }

  m_listenFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (m_listenFd < 0) {
    apm::logger::error("IpcServer::start: socket() failed: " +
                       std::string(std::strerror(errno)));
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  socklen_t addrLen = sizeof(sa_family_t);

  if (abstractSocket) {
    const std::string socketName = m_socketPath.substr(1);
    if (socketName.empty()) {
      apm::logger::error("IpcServer::start: abstract socket name is empty");
      ::close(m_listenFd);
      m_listenFd = -1;
      return false;
    }
    if (socketName.size() + 1 > sizeof(addr.sun_path)) {
      apm::logger::error("IpcServer::start: abstract socket name too long: " +
                         m_socketPath);
      ::close(m_listenFd);
      m_listenFd = -1;
      return false;
    }
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, socketName.data(), socketName.size());
    addrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 +
                                     socketName.size());
  } else {
    if (m_socketPath.size() >= sizeof(addr.sun_path)) {
      apm::logger::error("IpcServer::start: socket path too long: " +
                         m_socketPath);
      ::close(m_listenFd);
      m_listenFd = -1;
      return false;
    }

    std::strncpy(addr.sun_path, m_socketPath.c_str(),
                 sizeof(addr.sun_path) - 1);
    addrLen = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                     m_socketPath.size() + 1);
  }

  if (::bind(m_listenFd, reinterpret_cast<sockaddr *>(&addr), addrLen) <
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
  if (!abstractSocket) {
    if (::chmod(m_socketPath.c_str(), socketMode) < 0) {
      apm::logger::warn("IpcServer::start: chmod() failed: " +
                        std::string(std::strerror(errno)));
      // Continue anyway - not a fatal error
    }
    if (!apm::config::isEmulatorMode()) {
      ensurePathAccess(m_socketPath, socketMode);
    }
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
  apm::logger::debug(std::string(kLogFileTag) +
                     ": IpcServer::handleClient new client fd=" +
                     std::to_string(clientFd));

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
  if (apm::logger::isDebugEnabled()) {
    apm::logger::debug(std::string(kLogFileTag) + ": parsed request id='" +
                       req.id + "' type=" + typeToString(req.type));
  }

  Response resp;
  resp.id = req.id;

  auto requiresAuth = [](RequestType t) {
    return !(t == RequestType::Ping || t == RequestType::Authenticate ||
             t == RequestType::ForgotPassword || t == RequestType::List ||
             t == RequestType::Info || t == RequestType::Search);
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

  case RequestType::List: {
    apm::logger::info("IpcServer: List request received");
    resp.success = buildListResponseMessage(resp.message);
    break;
  }

  case RequestType::Info: {
    apm::logger::info("IpcServer: Info request for package: " +
                      req.packageName);
    if (req.packageName.empty()) {
      resp.success = false;
      resp.message = "Missing package name";
      break;
    }
    resp.success = true;
    resp.message = buildInfoResponseMessage(req.packageName);
    break;
  }

  case RequestType::Search: {
    apm::logger::info("IpcServer: Search request received");
    std::vector<std::string> patterns;
    auto it = req.rawFields.find("patterns");
    if (it != req.rawFields.end()) {
      patterns = splitLines(it->second);
    }
    resp.success = buildSearchResponseMessage(patterns, resp.message);
    break;
  }

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

  case RequestType::WipeCache: {
    apm::daemon::WipeCacheSelection selection;
    std::string parseErr;
    if (!parseWipeCacheSelection(req, selection, &parseErr)) {
      resp.success = false;
      resp.message = parseErr.empty() ? "Invalid cache wipe request"
                                      : parseErr;
      break;
    }

    apm::logger::info("IpcServer: WipeCache request received");

    apm::daemon::WipeCacheResult result;
    if (!apm::daemon::performWipeCache(m_moduleManager, selection, result)) {
      resp.success = false;
      resp.message =
          result.message.empty() ? "Cache wipe failed" : result.message;
    } else {
      resp.success = true;
      resp.message =
          result.message.empty() ? "Selected caches cleared." : result.message;
    }
    break;
  }

  case RequestType::DebugLogging: {
    apm::logger::info(std::string("IpcServer: DebugLogging request -> ") +
                      (req.debugLoggingEnabled ? "enable" : "disable"));

    std::string err;
    if (!apm::logger::setDebugEnabled(req.debugLoggingEnabled, &err)) {
      resp.success = false;
      resp.message = err.empty() ? "Failed to update debug logging" : err;
      break;
    }

    resp.success = true;
    resp.message = std::string("Debug logging ") +
                   (req.debugLoggingEnabled ? "enabled." : "disabled.");
    resp.rawFields["enabled"] = req.debugLoggingEnabled ? "true" : "false";
    break;
  }

  case RequestType::LogClear: {
    LogClearTarget target = LogClearTarget::Apm;
    std::string moduleName;
    std::string parseErr;
    if (!parseLogClearTarget(req, target, moduleName, &parseErr)) {
      resp.success = false;
      resp.message = parseErr.empty() ? "Invalid log clear request" : parseErr;
      break;
    }

    std::size_t removedCount = 0;
    std::string clearErr;
    if (!clearLogFiles(logPathsForTarget(target, moduleName), removedCount,
                       &clearErr)) {
      resp.success = false;
      resp.message = clearErr.empty() ? "Failed to clear logs" : clearErr;
      break;
    }

    resp.success = true;
    resp.rawFields["removed_count"] = std::to_string(removedCount);
    switch (target) {
    case LogClearTarget::All:
      resp.message = removedCount == 0 ? "No logs found to clear."
                                       : "Cleared " +
                                             std::to_string(removedCount) +
                                             " log file(s).";
      break;
    case LogClearTarget::Apm:
      resp.message = removedCount == 0 ? "No APM daemon log found to clear."
                                       : "Cleared APM daemon log.";
      break;
    case LogClearTarget::Ams:
      resp.message = removedCount == 0 ? "No AMS daemon log found to clear."
                                       : "Cleared AMS daemon log.";
      break;
    case LogClearTarget::Module:
      resp.message = removedCount == 0
                         ? "No module '" + moduleName + "' log found to clear."
                         : "Cleared module '" + moduleName + "' log.";
      break;
    }
    break;
  }

  default: {
    resp.success = false;
    resp.message = "Unsupported request type";
    break;
  }
  }

  const bool suppressPostResponseLogging = req.type == RequestType::LogClear;
  if (apm::logger::isDebugEnabled() && !suppressPostResponseLogging) {
    apm::logger::debug(std::string(kLogFileTag) +
                       ": completed request id='" + req.id +
                       "' success=" + (resp.success ? "true" : "false") +
                       " message='" + resp.message + "'");
  }
  sendResponseMessage(clientFd, resp, suppressPostResponseLogging);
  if (!suppressPostResponseLogging)
    apm::logger::debug("IpcServer::handleClient: response sent");
}

} // namespace apm::ipc
