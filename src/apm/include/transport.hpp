#pragma once

#include "protocol.hpp"
#include <functional>
#include <string>

namespace apm::ipc {

enum class TransportMode { Binder, IPC };

// Mirror ProgressHandler definition from client headers to avoid including
// both transport variants here.
using ProgressHandler = std::function<void(const Response &)>;

// Detect desired transport mode.
// Order of precedence:
// 1. Environment variable APM_TRANSPORT = "binder" | "ipc" (case-insensitive)
// 2. Executable path prefix: /system/bin -> Binder, /data/apm/bin -> IPC
// 3. Binder runtime availability check -> Binder if available else IPC
TransportMode detectTransportMode();

// Dispatch request automatically via chosen transport. If Binder fails
// (e.g., prepare transaction / unknown error) it will attempt a single
// fallback over IPC before returning failure.
bool sendRequestAuto(const Request &req, Response &resp,
                     const std::string &binderServiceName,
                     std::string *errorMsg = nullptr,
                     ProgressHandler progressHandler = {});

} // namespace apm::ipc
