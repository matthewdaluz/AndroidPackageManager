#include "transport.hpp"
#include "config.hpp"
#include "ipc_client.hpp"
#include "logger.hpp"

namespace apm::ipc {

TransportMode detectTransportMode() {
  if (apm::config::isEmulatorMode()) {
    apm::logger::debug("transport: emulator mode -> ipc");
  } else {
    apm::logger::debug("transport: IPC-only mode active");
  }
  return TransportMode::IPC;
}

bool sendRequestAuto(const Request &req, Response &resp,
                     std::string *errorMsg, ProgressHandler progressHandler) {
  bool ok = apm::ipc::sendRequest(req, resp, apm::config::getIpcSocketPath(),
                                  errorMsg, progressHandler);
  if (ok) resp.rawFields["transport"] = "ipc";
  return ok;
}

} // namespace apm::ipc
