/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: ipc_client.cpp
 * Purpose: Implement the request/response client used by the CLI to contact apmd.
 * Last Modified: November 18th, 2025. - 3:00 PM Eastern Time.
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

#include "ipc_client.hpp"
#include "logger.hpp"

#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace apm::ipc {

// Write the entire buffer to the socket, retrying on EINTR and surfacing
// failures so the higher-level sendRequest can report them.
static bool writeAll(int fd, const char *data, std::size_t len) {
  std::size_t sent = 0;
  while (sent < len) {
    ssize_t n = ::write(fd, data + sent, len - sent);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      apm::logger::error("ipc_client: write() failed: " +
                         std::string(std::strerror(errno)));
      return false;
    }
    if (n == 0) {
      // Should not happen on a connected socket
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }
  return true;
}

// Serialize the request, send it to apmd, and parse streamed responses (with
// support for progress frames in between). Returns true only if we received a
// final response message.
bool sendRequest(const Request &req, Response &resp,
                 const std::string &socketPath, std::string *errorMsg,
                 ProgressHandler progressHandler) {
  if (socketPath.empty()) {
    if (errorMsg)
      *errorMsg = "Socket path is empty";
    apm::logger::error("ipc_client: empty socket path");
    return false;
  }

  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    if (errorMsg)
      *errorMsg = "socket() failed: " + std::string(std::strerror(errno));
    apm::logger::error("ipc_client: socket() failed: " +
                       std::string(std::strerror(errno)));
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socketPath.size() >= sizeof(addr.sun_path)) {
    if (errorMsg)
      *errorMsg = "Socket path too long: " + socketPath;
    apm::logger::error("ipc_client: socket path too long");
    ::close(fd);
    return false;
  }
  std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    if (errorMsg)
      *errorMsg = "connect() failed: " + std::string(std::strerror(errno));
    apm::logger::error("ipc_client: connect() failed: " +
                       std::string(std::strerror(errno)));
    ::close(fd);
    return false;
  }

  std::string wireReq = serializeRequest(req);

  if (!writeAll(fd, wireReq.data(), wireReq.size())) {
    if (errorMsg)
      *errorMsg = "Failed to send request to daemon";
    ::close(fd);
    return false;
  }

  std::string buffer;
  char temp[512];
  bool gotFinalResponse = false;

  while (!gotFinalResponse) {
    ssize_t n = ::read(fd, temp, sizeof(temp));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      if (errorMsg)
        *errorMsg = "read() failed: " + std::string(std::strerror(errno));
      apm::logger::error("ipc_client: read() failed: " +
                         std::string(std::strerror(errno)));
      ::close(fd);
      return false;
    }
    if (n == 0 && buffer.empty()) {
      // Daemon closed connection without sending more data
      break;
    }

    if (n > 0) {
      buffer.append(temp, static_cast<std::size_t>(n));
    }

    while (true) {
      auto pos = buffer.find("\n\n");
      if (pos == std::string::npos)
        break;

      std::string frame = buffer.substr(0, pos + 2);
      buffer.erase(0, pos + 2);

      Response chunk;
      if (!parseResponse(frame, chunk, errorMsg)) {
        apm::logger::error("ipc_client: parseResponse failed");
        ::close(fd);
        return false;
      }

      if (chunk.status == ResponseStatus::Progress) {
        if (progressHandler)
          progressHandler(chunk);
      } else {
        resp = chunk;
        gotFinalResponse = true;
        break;
      }
    }

    if (n == 0)
      break;
  }

  ::close(fd);

  if (!gotFinalResponse) {
    if (errorMsg)
      *errorMsg = "Connection closed before receiving final response";
    apm::logger::error("ipc_client: no final response received");
    return false;
  }

  return true;
}

} // namespace apm::ipc
