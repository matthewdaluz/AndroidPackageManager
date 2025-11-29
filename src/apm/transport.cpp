#include "transport.hpp"
#include "binder_client.hpp"
#include "binder_support.hpp"
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
  // Env override first
  const char *env = ::getenv("APM_TRANSPORT");
  if (env && *env) {
    std::string v = toLower(env);
    if (v == "binder") {
      apm::logger::debug("transport: env override -> binder");
      return TransportMode::Binder;
    } else if (v == "ipc" || v == "unix" || v == "socket") {
      apm::logger::debug("transport: env override -> ipc");
      return TransportMode::IPC;
    }
    apm::logger::warn("transport: unknown APM_TRANSPORT value: " +
                      std::string(env));
  }

  std::string exe = readExePath();
  if (!exe.empty()) {
    if (exe.rfind("/system/bin/", 0) == 0) {
      apm::logger::debug(
          "transport: exe path indicates system install -> binder");
      return TransportMode::Binder;
    }
    if (exe.rfind("/data/apm/bin/", 0) == 0) {
      apm::logger::debug("transport: exe path indicates magisk install -> ipc");
      return TransportMode::IPC;
    }
  }

  // Fallback: if Binder runtime is available choose Binder, else IPC
  std::string err;
  if (apm::binder::isBinderRuntimeAvailable(&err)) {
    apm::logger::debug("transport: binder runtime available -> binder");
    return TransportMode::Binder;
  }
  apm::logger::debug("transport: binder runtime unavailable ('" + err +
                     "') -> ipc");
  return TransportMode::IPC;
}

static bool shouldFallbackToIpc(const std::string &binderErr) {
  if (binderErr.empty())
    return true; // unknown failure, try IPC
  // Heuristics for binder failure cases observed in earlier diagnostics.
  const char *tokens[] = {"Failed to prepare Binder transaction",
                          "Binder transact failed",
                          "Service '",
                          "not found",
                          "unknown error",
                          "STATUS_UNKNOWN_ERROR"};
  for (const char *t : tokens) {
    if (binderErr.find(t) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool sendRequestAuto(const Request &req, Response &resp,
                     const std::string &binderServiceName,
                     std::string *errorMsg, ProgressHandler progressHandler) {
  TransportMode mode = detectTransportMode();
  if (mode == TransportMode::Binder) {
    std::string binderErr;
    bool ok = sendRequestBinder(req, resp, binderServiceName, &binderErr,
                                progressHandler);
    if (ok) {
      resp.rawFields["transport"] = "binder";
      if (errorMsg)
        *errorMsg = "binder"; // success, marker only
      return true;
    }
    apm::logger::warn("transport: binder request failed: " + binderErr);
    if (shouldFallbackToIpc(binderErr)) {
      apm::logger::info("transport: attempting IPC fallback");
      std::string ipcErr;
      bool ipcOk = apm::ipc::sendRequest(
          req, resp, apm::config::IPC_SOCKET_PATH, &ipcErr, progressHandler);
      if (ipcOk) {
        resp.rawFields["binder_failure"] = binderErr;
        resp.rawFields["transport"] = "ipc_fallback";
        if (errorMsg)
          *errorMsg =
              "Binder failed ('" + binderErr + "'); IPC fallback succeeded.";
        return true;
      }
      if (errorMsg) {
        *errorMsg = "Binder failed ('" + binderErr +
                    "'); IPC fallback failed ('" + ipcErr + "')";
      }
      return false;
    }
    // Do not fallback; propagate binder error
    if (errorMsg)
      *errorMsg = binderErr;
    return false;
  }

  // IPC primary path
  bool ok = apm::ipc::sendRequest(req, resp, apm::config::IPC_SOCKET_PATH,
                                  errorMsg, progressHandler);
  if (ok) {
    resp.rawFields["transport"] = "ipc";
  }
  return ok;
}

} // namespace apm::ipc
