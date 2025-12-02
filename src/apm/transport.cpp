#include "transport.hpp"
#include "binder_client.hpp"
#include "config.hpp"
#include "ipc_client.hpp"
#include "logger.hpp"

#include <algorithm>
#include <cctype>
#include <unistd.h>

namespace apm::ipc {

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return s;
}

static std::string readExePath() {
  char buf[512];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n < 0) {
    return ""; // best-effort
  }
  buf[n] = '\0';
  return std::string(buf);
}

TransportMode detectTransportMode() {
  // IPC-only mode: Binder is deprecated and unused by default.
  // See binder_* files for reference if enabling Binder in the future.
  (void)toLower;      // silence unused helpers after simplification
  (void)readExePath;  // silence unused helpers after simplification
  if (apm::config::isEmulatorMode()) {
    apm::logger::debug("transport: emulator mode -> ipc");
  } else {
    apm::logger::debug("transport: IPC-only mode active");
  }
  return TransportMode::IPC;
}

// Binder fallback logic removed in IPC-only mode.

bool sendRequestAuto(const Request &req, Response &resp,
                     const std::string & /*binderServiceName*/,
                     std::string *errorMsg, ProgressHandler progressHandler) {
  // IPC-only path
  bool ok = apm::ipc::sendRequest(req, resp, apm::config::getIpcSocketPath(),
                                  errorMsg, progressHandler);
  if (ok) resp.rawFields["transport"] = "ipc";
  return ok;
}

} // namespace apm::ipc
