/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: binder_client.cpp
 * Purpose: Implement the request/response client used by the CLI to contact
 * apmd over Binder.
 * Last Modified: November 28th, 2025. - 8:59 AM Eastern Time.
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

/*
 * NOTICE: Binder transport deprecated
 * ----------------------------------
 * As of December 2025, the Binder transport is no longer used by default.
 * The project runs in IPC-only mode over a UNIX domain socket under
 * /data/apm/apmd.sock for reliability across ROMs and SELinux policies.
 *
 * This Binder client implementation is retained for reference only to aid
 * contributors who may wish to re-enable Binder in the future. It is not
 * invoked by the current code paths.
 */

#include "binder_client.hpp"

#include "binder_defs.hpp"
#include "binder_support.hpp"
#include "logger.hpp"

#include <limits>
#include <string>
#include <thread>
#include <utility>

#if defined(__ANDROID__)
#include <android/binder_status.h>

static const char *statusToString(binder_status_t status) {
  switch (status) {
  case STATUS_OK:
    return "OK";
  case STATUS_UNKNOWN_TRANSACTION:
    return "Unknown transaction";
  case STATUS_BAD_VALUE:
    return "Bad value";
  case STATUS_FAILED_TRANSACTION:
    return "Failed transaction";
  case STATUS_PERMISSION_DENIED:
    return "Permission denied";
  case STATUS_UNEXPECTED_NULL:
    return "Unexpected null";
  default:
    return "Error";
  }
}
#include <android/api-level.h>
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>

#include <mutex>
#endif

namespace apm::ipc {
#if defined(__ANDROID__)

inline bool isApiAtLeast34() {
  int api = android_get_device_api_level();
  return api >= 34;
}

static std::string withSuggestion(const std::string &base,
                                  const char *suggestion = nullptr) {
  if (suggestion && *suggestion) {
    return base + "\nSuggestion: " + suggestion;
  }
  return base;
}

struct ScopedStrongBinder {
  explicit ScopedStrongBinder(AIBinder *b = nullptr) : binder(b) {}
  ~ScopedStrongBinder() {
    if (binder && isApiAtLeast34()) {
      AIBinder_decStrong(binder);
    }
  }
  ScopedStrongBinder(const ScopedStrongBinder &) = delete;
  ScopedStrongBinder &operator=(const ScopedStrongBinder &) = delete;
  ScopedStrongBinder(ScopedStrongBinder &&other) noexcept
      : binder(other.binder) {
    other.binder = nullptr;
  }
  ScopedStrongBinder &operator=(ScopedStrongBinder &&other) noexcept {
    if (this != &other) {
      if (binder && isApiAtLeast34()) {
        AIBinder_decStrong(binder);
      }
      binder = other.binder;
      other.binder = nullptr;
    }
    return *this;
  }

  AIBinder *get() const { return binder; }

private:
  AIBinder *binder;
};

class ProgressCallbackReceiver;
AIBinder_Class *getProgressClass();

bool stringAllocator(void *stringData, int32_t length, char **buffer) {
  auto *out = static_cast<std::string *>(stringData);
  if (length < 0) {
    out->clear();
    *buffer = nullptr;
    return true;
  }
  out->assign(static_cast<std::size_t>(length), '\0');
  *buffer = out->data();
  return true;
}

binder_status_t readParcelString(const AParcel *parcel, std::string &out) {
  if (!isApiAtLeast34()) {
    return STATUS_FAILED_TRANSACTION;
  }

  binder_status_t status = AParcel_readString(parcel, &out, stringAllocator);
  if (status != STATUS_OK) {
    return status;
  }
  if (!out.empty() && out.back() == '\0') {
    out.pop_back();
  }
  return STATUS_OK;
}

binder_status_t writeParcelString(AParcel *parcel, const std::string &value) {
  if (!isApiAtLeast34()) {
    return STATUS_FAILED_TRANSACTION;
  }

  if (value.size() >
      static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) {
    return STATUS_BAD_VALUE;
  }
  return AParcel_writeString(parcel, value.c_str(),
                             static_cast<int32_t>(value.size()));
}

class ProgressCallbackReceiver {
public:
  explicit ProgressCallbackReceiver(ProgressHandler handler)
      : m_handler(std::move(handler)), m_binder(nullptr) {
    if (!m_handler || !isApiAtLeast34())
      return;

    AIBinder_Class *cls = getProgressClass();
    if (!cls) {
      apm::logger::warn(
          "binder_client: failed to allocate progress binder class");
      return;
    }

    m_binder = ScopedStrongBinder(AIBinder_new(cls, this));
  }

  AIBinder *binder() const { return m_binder.get(); }

  binder_status_t handleTransact(transaction_code_t code, const AParcel *in) {
    if (code != apm::binder::TX_PROGRESS_EVENT || !m_handler) {
      return STATUS_UNKNOWN_TRANSACTION;
    }

    std::string raw;
    if (readParcelString(in, raw) != STATUS_OK) {
      return STATUS_BAD_VALUE;
    }

    apm::ipc::Response progress;
    std::string parseErr;
    if (!parseResponse(raw, progress, &parseErr)) {
      apm::logger::warn("binder_client: failed to parse progress frame: " +
                        parseErr);
      return STATUS_BAD_VALUE;
    }

    m_handler(progress);
    return STATUS_OK;
  }

private:
  ProgressHandler m_handler;
  ScopedStrongBinder m_binder;
};

AIBinder_Class *getProgressClass() {
  static AIBinder_Class *cls = nullptr;
  static std::once_flag flag;
  std::call_once(flag, [&]() {
    if (!isApiAtLeast34()) {
      return;
    }
    cls = AIBinder_Class_define(
        apm::binder::PROGRESS_INTERFACE,
        /* onCreate */ [](void *args) -> void * { return args; },
        /* onDestroy */ [](void *) {},
        /* onTransact */
        [](AIBinder *binder, transaction_code_t code, const AParcel *in,
           AParcel *) -> binder_status_t {
          auto *receiver = static_cast<ProgressCallbackReceiver *>(
              AIBinder_getUserData(binder));
          if (!receiver)
            return STATUS_FAILED_TRANSACTION;
          return receiver->handleTransact(code, in);
        });
  });

  return cls;
}

/* Remove incomplete and duplicate function definition here */

bool sendRequestBinder(const Request &req, Response &resp,
                       const std::string &serviceName, std::string *errorMsg,
                       ProgressHandler progressHandler) {
  apm::logger::debug("=== sendRequestBinder: ENTER ===");
  apm::logger::debug(
      "sendRequestBinder: service='" + serviceName + "' req.id='" + req.id +
      "' req.type=" + std::to_string(static_cast<int>(req.type)));

  // Hard-fail for devices below Android 14 (API 34)
  if (!isApiAtLeast34()) {
    apm::logger::debug("sendRequestBinder: API level < 34, aborting");
    if (errorMsg) {
      *errorMsg = withSuggestion("Android 14 (API 34) required for APM Binder",
                                 "Upgrade device OS to Android 14+.");
    }
    return false;
  }

  apm::logger::debug("sendRequestBinder: API level check passed");

  std::string runtimeErr;
  apm::logger::debug("sendRequestBinder: checking Binder runtime availability");

  if (!apm::binder::isBinderRuntimeAvailable(&runtimeErr)) {
    apm::logger::debug("sendRequestBinder: runtime check FAILED: " +
                       runtimeErr);
    if (errorMsg) {
      *errorMsg = withSuggestion(runtimeErr,
                                 "Ensure libbinder_ndk is present on device.");
    }
    return false;
  }

  apm::logger::debug("sendRequestBinder: runtime available");

  // Resolve the service with a bounded wait to avoid hanging if apmd is not
  // running yet.
  ScopedStrongBinder service;
  {
    using namespace std::chrono_literals;
    apm::logger::debug(
        "sendRequestBinder: attempting service lookup (fast path)");

    AIBinder *svc = apm::binder::getService(serviceName, false, &runtimeErr);

    if (!svc) {
      apm::logger::debug(
          "sendRequestBinder: fast path FAILED, starting retry loop");

      const int maxAttempts = 5;
      for (int attempt = 0; attempt < maxAttempts && !svc; ++attempt) {
        apm::logger::debug("sendRequestBinder: retry " +
                           std::to_string(attempt + 1) + "/" +
                           std::to_string(maxAttempts));
        std::this_thread::sleep_for(500ms);
        svc = apm::binder::getService(serviceName, false, &runtimeErr);

        if (svc) {
          apm::logger::debug("sendRequestBinder: retry SUCCESS on attempt " +
                             std::to_string(attempt + 1));
        }
      }

      if (!svc) {
        apm::logger::debug(
            "sendRequestBinder: all retries exhausted, service not found");
      }
    } else {
      apm::logger::debug("sendRequestBinder: fast path SUCCESS");
    }

    service = ScopedStrongBinder(svc);
    apm::logger::debug(
        "sendRequestBinder: service moved into ScopedStrongBinder, handle=" +
        std::to_string(reinterpret_cast<uintptr_t>(svc)));
  }

  if (!service.get()) {
    apm::logger::debug("sendRequestBinder: no service handle, ABORTING");
    if (errorMsg) {
      std::string msg = runtimeErr.empty()
                            ? ("Service '" + serviceName + "' not found")
                            : runtimeErr;
      *errorMsg =
          withSuggestion(msg, "Verify apmd is running; try 'su -c "
                              "/system/bin/apmd' or wait for Magisk service.");
    }
    return false;
  }

  apm::logger::debug(
      "sendRequestBinder: service.get()=" +
      std::to_string(reinterpret_cast<uintptr_t>(service.get())));

  apm::logger::debug("sendRequestBinder: creating ProgressCallbackReceiver");
  ProgressCallbackReceiver progressReceiver(progressHandler);
  apm::logger::debug(
      "sendRequestBinder: progressReceiver.binder()=" +
      std::to_string(reinterpret_cast<uintptr_t>(progressReceiver.binder())));

  // Create a new parcel for the request
  apm::logger::debug(
      "sendRequestBinder: preparing to call AIBinder_prepareTransaction");
  apm::logger::debug(
      "sendRequestBinder:   service handle=" +
      std::to_string(reinterpret_cast<uintptr_t>(service.get())));

  AParcel *parcel = nullptr;
  binder_status_t parcelStatus =
      AIBinder_prepareTransaction(service.get(), &parcel);

  apm::logger::debug("sendRequestBinder: parcel=" +
                     std::to_string(reinterpret_cast<uintptr_t>(parcel)));

  if (parcelStatus != STATUS_OK || !parcel) {
    apm::logger::error("binder_client: prepareTransaction FAILED");
    apm::logger::error("  handle=" + std::to_string(reinterpret_cast<uintptr_t>(
                                         service.get())));
    apm::logger::error("  status=" + std::to_string(parcelStatus));
    apm::logger::error("  parcel=" +
                       std::to_string(reinterpret_cast<uintptr_t>(parcel)));
    if (errorMsg) {
      *errorMsg =
          withSuggestion(std::string("Failed to prepare Binder transaction: ") +
                             statusToString(parcelStatus) + " (" +
                             std::to_string(parcelStatus) + ")",
                         "Restart apmd and retry the command.");
    }
    return false;
  }

  apm::logger::debug(
      "sendRequestBinder: prepareTransaction SUCCESS, writing request");

  // Write the request to the parcel
  binder_status_t writeStatus =
      writeParcelString(parcel, serializeRequest(req));
  apm::logger::debug("sendRequestBinder: writeParcelString returned " +
                     std::to_string(writeStatus));

  if (writeStatus != STATUS_OK) {
    apm::logger::debug("sendRequestBinder: write request FAILED, status=" +
                       std::to_string(writeStatus));
    if (parcel) {
      AParcel_delete(parcel);
    }
    if (errorMsg)
      *errorMsg = "Failed to write request to Binder parcel";
    return false;
  }

  apm::logger::debug(
      "sendRequestBinder: request written, attaching progress binder");

  // Include the optional progress callback binder so the daemon can send
  // incremental updates.
  binder_status_t cbStatus =
      AParcel_writeStrongBinder(parcel, progressReceiver.binder());
  apm::logger::debug("sendRequestBinder: writeStrongBinder returned " +
                     std::to_string(cbStatus));

  if (cbStatus != STATUS_OK) {
    apm::logger::debug("sendRequestBinder: attach progress binder FAILED");
    if (parcel) {
      AParcel_delete(parcel);
    }
    if (errorMsg) {
      *errorMsg = withSuggestion(
          std::string("Failed to attach progress callback binder: ") +
              statusToString(cbStatus) + " (" + std::to_string(cbStatus) + ")",
          "Proceed without progress; re-run with fewer options if issue "
          "persists.");
    }
    return false;
  }

  apm::logger::debug("sendRequestBinder: calling AIBinder_transact");
  apm::logger::debug("  service=" + std::to_string(reinterpret_cast<uintptr_t>(
                                        service.get())));
  apm::logger::debug("  code=" + std::to_string(apm::binder::TX_SEND_REQUEST));

  AParcel *reply = nullptr;
  binder_status_t status =
      AIBinder_transact(service.get(), // service binder
                        apm::binder::TX_SEND_REQUEST, &parcel, &reply,
                        static_cast<binder_flags_t>(0));

  apm::logger::debug("sendRequestBinder: AIBinder_transact returned status=" +
                     std::to_string(status) + " reply=" +
                     std::to_string(reinterpret_cast<uintptr_t>(reply)));

  if (status != STATUS_OK || !reply) {
    apm::logger::error("sendRequestBinder: transact FAILED, status=" +
                       std::to_string(status));
    if (errorMsg) {
      *errorMsg = withSuggestion(std::string("Binder transact failed: ") +
                                     statusToString(status) + " (" +
                                     std::to_string(status) + ")",
                                 "Ensure apmd has registered and is "
                                 "responsive; try again in a moment.");
    }
    return false;
  }

  apm::logger::debug("sendRequestBinder: transact SUCCESS, reading reply");

  std::string rawResp;
  binder_status_t readStatus = readParcelString(reply, rawResp);
  apm::logger::debug("sendRequestBinder: readParcelString returned " +
                     std::to_string(readStatus) +
                     " length=" + std::to_string(rawResp.size()));

  AParcel_delete(reply);

  if (readStatus != STATUS_OK) {
    apm::logger::error("sendRequestBinder: read reply FAILED, status=" +
                       std::to_string(readStatus));
    if (errorMsg) {
      *errorMsg =
          withSuggestion(std::string("Failed to read Binder reply: ") +
                             statusToString(readStatus) + " (" +
                             std::to_string(readStatus) + ")",
                         "Re-run the command; if persistent, restart apmd.");
    }
    return false;
  }

  apm::logger::debug("sendRequestBinder: parsing response");

  if (!parseResponse(rawResp, resp, errorMsg)) {
    apm::logger::error("binder_client: parseResponse FAILED");
    return false;
  }

  apm::logger::debug("sendRequestBinder: SUCCESS, resp.success=" +
                     std::string(resp.success ? "true" : "false"));
  apm::logger::debug("=== sendRequestBinder: EXIT ===");

  return true;
}

#else // !__ANDROID__

bool sendRequestBinder(const Request & /*req*/, Response & /*resp*/,
                       const std::string & /*serviceName*/,
                       std::string *errorMsg,
                       ProgressHandler /*progressHandler*/) {
  if (errorMsg) {
    *errorMsg = "Binder transport is unavailable on this platform";
  }
  return false;
}

#endif // __ANDROID__

// Removed generic sendRequest wrapper; callers should use
// transport::sendRequestAuto() which delegates to this when appropriate.

} // namespace apm::ipc
