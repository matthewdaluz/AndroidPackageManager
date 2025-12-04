/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: ipc_server.cpp
 * Purpose: Implement the AMSD UNIX domain socket server that accepts module
 *          IPC requests and forwards them to the dispatcher.
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

#include "ipc_server.hpp"

#include "logger.hpp"
#include "protocol.hpp"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace apm::amsd {

namespace {

void sendResponseMessage(int clientFd, apm::ipc::Response resp) {
  if (resp.status == apm::ipc::ResponseStatus::Unknown) {
    resp.status = resp.success ? apm::ipc::ResponseStatus::Ok
                               : apm::ipc::ResponseStatus::Error;
  }
  std::string wire = apm::ipc::serializeResponse(resp);
  ::write(clientFd, wire.data(), wire.size());
}

} // namespace

IpcServer::IpcServer(const std::string &socketPath,
                     RequestDispatcher &dispatcher)
    : listenFd_(-1), socketPath_(socketPath), running_(false),
      dispatcher_(dispatcher) {}

IpcServer::~IpcServer() {
  stop();
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

  if (::chmod(socketPath_.c_str(), 0666) < 0) {
    apm::logger::warn("amsd: chmod() failed on socket: " +
                      std::string(std::strerror(errno)));
  }

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

    handleClient(clientFd);
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

void IpcServer::handleClient(int clientFd) {
  std::string buffer;
  char temp[512];

  while (true) {
    ssize_t n = ::read(clientFd, temp, sizeof(temp));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      apm::logger::error("amsd: read() failed: " +
                         std::string(std::strerror(errno)));
      return;
    }
    if (n == 0)
      break;

    buffer.append(temp, static_cast<std::size_t>(n));
    if (buffer.find("\n\n") != std::string::npos)
      break;
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

  apm::ipc::Response resp;
  resp.id = req.id;
  dispatcher_.dispatch(req, resp);
  sendResponseMessage(clientFd, resp);
}

} // namespace apm::amsd
