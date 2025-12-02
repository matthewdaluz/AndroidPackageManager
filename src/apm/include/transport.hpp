#pragma once

#include "protocol.hpp"
#include <functional>
#include <string>

namespace apm::ipc {

enum class TransportMode { IPC };

// Mirror ProgressHandler definition from client headers to avoid including
// transport-specific headers here.
using ProgressHandler = std::function<void(const Response &)>;

// Detect desired transport mode. Binder is fully disabled; IPC is always used.
TransportMode detectTransportMode();

// Dispatch request over the IPC socket transport.
bool sendRequestAuto(const Request &req, Response &resp,
                     std::string *errorMsg = nullptr,
                     ProgressHandler progressHandler = {});

} // namespace apm::ipc
