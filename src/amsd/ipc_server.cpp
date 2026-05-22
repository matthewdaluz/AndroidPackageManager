/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: ipc_server.cpp
 * Purpose: Implement the AMSD UNIX domain socket server that accepts module
 *          IPC requests and forwards them to the dispatcher.
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

#include "ipc_server.hpp"

#include "config.hpp"
#include "logger.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <grp.h>
#include <limits>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace apm::amsd {

namespace {

constexpr std::size_t kMaxRequestBytes = 64 * 1024;
constexpr std::size_t kMaxClientWorkers = 32;
constexpr auto kRequestReadDeadline = std::chrono::seconds(5);
constexpr auto kResponseWriteDeadline = std::chrono::seconds(5);
constexpr const char *kLogFileTag = "ipc_server.cpp";

void setSocketAccess(const std::string &socketPath) {
  const bool emulatorMode = apm::config::isEmulatorMode();
  const mode_t socketMode = emulatorMode ? 0600 : 0660;

  if (!emulatorMode) {
    struct group *shellGroup = ::getgrnam("shell");
    if (!shellGroup) {
      apm::logger::warn("amsd: could not look up shell group for socket");
    } else if (::chown(socketPath.c_str(), 0, shellGroup->gr_gid) < 0 &&
               errno != EPERM) {
      apm::logger::warn("amsd: chown() failed on socket: " +
                        std::string(std::strerror(errno)));
    }
  }

  if (::chmod(socketPath.c_str(), socketMode) < 0) {
    apm::logger::warn("amsd: chmod() failed on socket: " +
                      std::string(std::strerror(errno)));
  }
}

bool waitForSocket(int fd, short events,
                   std::chrono::steady_clock::time_point deadline,
                   std::string *errorMsg) {
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      errno = ETIMEDOUT;
      if (errorMsg)
        *errorMsg = "socket I/O timed out";
      return false;
    }

    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    const auto timeoutMs = std::min<long long>(
        std::max<long long>(remaining.count(), 1),
        std::numeric_limits<int>::max());

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = events;
    const int rc = ::poll(&pfd, 1, static_cast<int>(timeoutMs));
    if (rc > 0) {
      if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
        errno = EIO;
        if (errorMsg)
          *errorMsg = "socket poll reported an error";
        return false;
      }
      if ((pfd.revents & (events | POLLHUP)) != 0)
        return true;
      continue;
    }
    if (rc == 0)
      continue;
    if (errno == EINTR)
      continue;
    if (errorMsg)
      *errorMsg = "poll() failed: " + std::string(std::strerror(errno));
    return false;
  }
}

bool readRequestFrame(int fd, std::string &buffer, bool &tooLarge,
                      std::string *errorMsg) {
  const auto deadline =
      std::chrono::steady_clock::now() + kRequestReadDeadline;
  tooLarge = false;
  buffer.clear();
  char temp[512];

  while (true) {
    if (!waitForSocket(fd, POLLIN, deadline, errorMsg))
      return false;

    const ssize_t n = ::recv(fd, temp, sizeof(temp), MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      if (errorMsg)
        *errorMsg = "read() failed: " + std::string(std::strerror(errno));
      return false;
    }
    if (n == 0)
      return true;

    const std::size_t incoming = static_cast<std::size_t>(n);
    if (buffer.size() + incoming > kMaxRequestBytes) {
      tooLarge = true;
      if (errorMsg)
        *errorMsg = "request too large";
      return false;
    }

    buffer.append(temp, incoming);
    if (buffer.find("\n\n") != std::string::npos)
      return true;
  }
}

bool writeAll(int fd, const char *data, std::size_t len,
              std::string *errorMsg) {
  const auto deadline =
      std::chrono::steady_clock::now() + kResponseWriteDeadline;
  std::size_t sent = 0;
  while (sent < len) {
    if (!waitForSocket(fd, POLLOUT, deadline, errorMsg))
      return false;

    ssize_t n =
        ::send(fd, data + sent, len - sent, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
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

void sendResponseMessage(int clientFd, apm::ipc::Response resp) {
  if (resp.status == apm::ipc::ResponseStatus::Unknown) {
    resp.status = resp.success ? apm::ipc::ResponseStatus::Ok
                               : apm::ipc::ResponseStatus::Error;
  }
  const bool debugEnabled = apm::logger::isDebugEnabled();
  if (debugEnabled) {
    apm::logger::debug(std::string(kLogFileTag) +
                       ": sendResponseMessage status=" +
                       (resp.success ? "ok" : "error") + " id='" + resp.id +
                       "' msg='" + resp.message + "'");
  }
  std::string wire = apm::ipc::serializeResponse(resp);
  std::string err;
  if (!writeAll(clientFd, wire.data(), wire.size(), &err)) {
    apm::logger::warn("amsd: sendResponseMessage failed: " + err);
    ::shutdown(clientFd, SHUT_RDWR);
  }
}

} // namespace

IpcServer::IpcServer(const std::string &socketPath,
                     RequestDispatcher &dispatcher)
    : listenFd_(-1), socketPath_(socketPath), running_(false),
      dispatcher_(dispatcher) {}

IpcServer::~IpcServer() {
  stop();
  waitForClients();
  if (!socketPath_.empty()) {
    ::unlink(socketPath_.c_str());
  }
}

bool IpcServer::start() {
  if (socketPath_.empty()) {
    apm::logger::error("amsd: socket path is empty");
    return false;
  }

  ::unlink(socketPath_.c_str());

  listenFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listenFd_ < 0) {
    apm::logger::error("amsd: socket() failed: " +
                       std::string(std::strerror(errno)));
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socketPath_.size() >= sizeof(addr.sun_path)) {
    apm::logger::error("amsd: socket path too long: " + socketPath_);
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }
  std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

  if (::bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) <
      0) {
    apm::logger::error("amsd: bind() failed: " +
                       std::string(std::strerror(errno)));
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }

  if (::listen(listenFd_, 8) < 0) {
    apm::logger::error("amsd: listen() failed: " +
                       std::string(std::strerror(errno)));
    ::close(listenFd_);
    listenFd_ = -1;
    return false;
  }

  setSocketAccess(socketPath_);

  running_.store(true);
  apm::logger::info("amsd: listening on " + socketPath_);
  return true;
}

void IpcServer::run() {
  if (listenFd_ < 0) {
    apm::logger::error("amsd: server not started");
    return;
  }

  while (running_.load()) {
    int clientFd = ::accept(listenFd_, nullptr, nullptr);
    if (clientFd < 0) {
      if (errno == EINTR)
        continue;
      if (!running_.load())
        break;
      apm::logger::error("amsd: accept() failed: " +
                         std::string(std::strerror(errno)));
      break;
    }

    if (!startClientWorker(clientFd))
      ::close(clientFd);
  }

  apm::logger::info("amsd: IPC loop stopped");
}

void IpcServer::stop() {
  running_.store(false);
  if (listenFd_ >= 0) {
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    listenFd_ = -1;
  }
}

bool IpcServer::startClientWorker(int clientFd) {
  {
    std::lock_guard<std::mutex> lock(clientMutex_);
    if (activeClients_ >= kMaxClientWorkers) {
      apm::logger::warn("amsd: rejecting client at worker limit");
      return false;
    }
    ++activeClients_;
  }

  try {
    std::thread([this, clientFd]() {
      handleClient(clientFd);
      ::shutdown(clientFd, SHUT_RDWR);
      ::close(clientFd);
      {
        std::lock_guard<std::mutex> lock(clientMutex_);
        --activeClients_;
      }
      clientCv_.notify_one();
    }).detach();
  } catch (...) {
    std::lock_guard<std::mutex> lock(clientMutex_);
    --activeClients_;
    apm::logger::error("amsd: failed to start client worker");
    clientCv_.notify_one();
    return false;
  }

  return true;
}

void IpcServer::waitForClients() {
  std::unique_lock<std::mutex> lock(clientMutex_);
  clientCv_.wait(lock, [this]() { return activeClients_ == 0; });
}

void IpcServer::handleClient(int clientFd) {
  if (apm::logger::isDebugEnabled()) {
    apm::logger::debug(std::string(kLogFileTag) +
                       ": IpcServer::handleClient new client fd=" +
                       std::to_string(clientFd));
  }

  std::string buffer;
  bool tooLarge = false;
  std::string readErr;
  if (!readRequestFrame(clientFd, buffer, tooLarge, &readErr)) {
    if (tooLarge) {
      apm::logger::warn("amsd: rejecting oversized request (> " +
                        std::to_string(kMaxRequestBytes) + " bytes)");
      apm::ipc::Response badResp;
      badResp.success = false;
      badResp.message = "Bad request: request too large";
      sendResponseMessage(clientFd, badResp);
    } else {
      apm::logger::warn("amsd: " + readErr);
    }
    return;
  }

  apm::ipc::Request req;
  std::string parseErr;
  if (!apm::ipc::parseRequest(buffer, req, &parseErr)) {
    apm::logger::error("amsd: parseRequest failed: " + parseErr);
    apm::ipc::Response badResp;
    badResp.success = false;
    badResp.message = "Bad request: " + parseErr;
    sendResponseMessage(clientFd, badResp);
    return;
  }

  if (apm::logger::isDebugEnabled()) {
    apm::logger::debug(std::string(kLogFileTag) + ": parsed request id='" +
                       req.id + "' type=" + apm::ipc::typeToString(req.type));
  }

  apm::ipc::Response resp;
  resp.id = req.id;
  {
    std::lock_guard<std::mutex> dispatchLock(dispatchMutex_);
    dispatcher_.dispatch(req, resp);
  }

  if (apm::logger::isDebugEnabled()) {
    apm::logger::debug(std::string(kLogFileTag) +
                       ": completed request id='" + req.id +
                       "' success=" + (resp.success ? "true" : "false") +
                       " message='" + resp.message + "'");
  }

  sendResponseMessage(clientFd, resp);
}

} // namespace apm::amsd
