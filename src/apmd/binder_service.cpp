/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: binder_service.cpp
 * Purpose: Implement the Binder endpoint that exposes apmd operations to
 * Android clients, replacing the legacy UNIX socket transport.
 * Last Modified: 2026-03-15 11:56:16.536679330 -0400.
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
 * The daemon no longer registers a Binder service by default. IPC over
 * UNIX domain socket is the sole runtime transport. This file is kept
 * for reference and potential future Binder enablement.
 */

#include "binder_service.hpp"

#include "binder_defs.hpp"
#include "binder_support.hpp"
#include "logger.hpp"
#include "protocol.hpp"
#include "request_dispatcher.hpp"

#if defined(__ANDROID__)

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>

#include <limits>

namespace apm::ipc {

namespace {

AIBinder_Class *gServiceClass = nullptr;

struct ScopedStrongBinder {
  explicit ScopedStrongBinder(AIBinder *b = nullptr) : binder(b) {}
  ~ScopedStrongBinder() {
    if (binder) {
      if (__builtin_available(android 34, *)) {
        AIBinder_decStrong(binder);
      }
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
      if (binder) {
        if (__builtin_available(android 34, *)) {
          AIBinder_decStrong(binder);
        }
      }
      binder = other.binder;
      other.binder = nullptr;
    }
    return *this;
  }

  AIBinder *get() const { return binder; }
  AIBinder *release() {
    AIBinder *tmp = binder;
    binder = nullptr;
    return tmp;
  }

private:
  AIBinder *binder;
};

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
  if (__builtin_available(android 34, *)) {
    binder_status_t status = AParcel_readString(parcel, &out, stringAllocator);
    if (status != STATUS_OK) {
      return status;
    }
    if (!out.empty() && out.back() == '\0') {
      out.pop_back();
    }
    return STATUS_OK;
  }
  return STATUS_FAILED_TRANSACTION;
}

binder_status_t writeParcelString(AParcel *parcel, const std::string &value) {
  if (__builtin_available(android 34, *)) {
    if (value.size() >
        static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) {
      return STATUS_BAD_VALUE;
    }
    return AParcel_writeString(parcel, value.c_str(),
                               static_cast<int32_t>(value.size()));
  }
  return STATUS_FAILED_TRANSACTION;
}

// Send a serialized response to a remote binder (progress callback).
void sendProgressToClient(AIBinder *callback,
                          const apm::ipc::Response &progress) {
  if (__builtin_available(android 34, *)) {
    if (!callback)
      return;

    AParcel *parcel = nullptr;
    if (AIBinder_prepareTransaction(callback, &parcel) != STATUS_OK) {
      apm::logger::warn("BinderService: failed to prepare progress parcel");
      return;
    }

    std::string payload = serializeResponse(progress);
    if (writeParcelString(parcel, payload) != STATUS_OK) {
      apm::logger::warn("BinderService: failed to encode progress frame");
      AParcel_delete(parcel);
      return;
    }

    AParcel *reply = nullptr;
    binder_status_t txStatus = AIBinder_transact(
        callback, apm::binder::TX_PROGRESS_EVENT, &parcel, &reply, FLAG_ONEWAY);
    if (reply) {
      AParcel_delete(reply);
    }
    if (txStatus != STATUS_OK) {
      apm::logger::warn("BinderService: progress transact failed with status " +
                        std::to_string(txStatus));
    }
  }
}

class BinderServiceState {
public:
  explicit BinderServiceState(BinderService &owner) : m_owner(owner) {}

  binder_status_t handleTransact(transaction_code_t code, const AParcel *in,
                                 AParcel *out) {
    apm::logger::debug("=== BinderService::handleTransact: ENTER ===");
    apm::logger::debug("handleTransact: code=" + std::to_string(code));
    apm::logger::debug("handleTransact: in=" +
                       std::to_string(reinterpret_cast<uintptr_t>(in)));
    apm::logger::debug("handleTransact: out=" +
                       std::to_string(reinterpret_cast<uintptr_t>(out)));

    if (__builtin_available(android 34, *)) {
      if (code != apm::binder::TX_SEND_REQUEST) {
        apm::logger::debug(
            "handleTransact: UNKNOWN transaction code, expected " +
            std::to_string(apm::binder::TX_SEND_REQUEST));
        return STATUS_UNKNOWN_TRANSACTION;
      }

      apm::logger::debug("handleTransact: reading request string from parcel");

      std::string rawRequest;
      binder_status_t status = readParcelString(in, rawRequest);
      apm::logger::debug("handleTransact: readParcelString returned status=" +
                         std::to_string(status) +
                         " length=" + std::to_string(rawRequest.size()));

      if (status != STATUS_OK) {
        apm::logger::error("handleTransact: read request FAILED");
        return status;
      }

      apm::logger::debug("handleTransact: reading progress binder from parcel");

      AIBinder *cbRaw = nullptr;
      status = AParcel_readStrongBinder(in, &cbRaw);
      apm::logger::debug("handleTransact: readStrongBinder returned status=" +
                         std::to_string(status) + " binder=" +
                         std::to_string(reinterpret_cast<uintptr_t>(cbRaw)));

      if (status != STATUS_OK) {
        apm::logger::error("handleTransact: read progress binder FAILED");
        return status;
      }

      if (!cbRaw) {
        apm::logger::info(
            "BinderService: no progress callback provided by client");
      }

      ScopedStrongBinder callback(cbRaw);

      apm::logger::debug("handleTransact: parsing request");

      apm::ipc::Request req;
      apm::ipc::Response resp;
      std::string parseErr;

      if (!parseRequest(rawRequest, req, &parseErr)) {
        apm::logger::error("handleTransact: parseRequest FAILED: " + parseErr);
        resp.success = false;
        resp.message =
            parseErr.empty() ? "Bad request" : ("Bad request: " + parseErr);
        resp.status = apm::ipc::ResponseStatus::Error;
        std::string payload = serializeResponse(resp);
        return writeParcelString(out, payload);
      }

      apm::logger::info("BinderService: received request type=" +
                        std::to_string(static_cast<int>(req.type)) +
                        " id=" + req.id);
      apm::logger::debug("handleTransact: dispatching request to handler");

      auto progressCb = [&](const apm::ipc::Response &progress) {
        apm::logger::debug("handleTransact: sending progress update");
        sendProgressToClient(callback.get(), progress);
      };

      m_owner.dispatchRequest(req, resp, progressCb);

      apm::logger::debug(
          "handleTransact: request dispatched, serializing response");

      std::string payload = serializeResponse(resp);
      apm::logger::debug("handleTransact: response serialized, length=" +
                         std::to_string(payload.size()));

      binder_status_t writeStatus = writeParcelString(out, payload);
      apm::logger::debug("handleTransact: writeParcelString returned " +
                         std::to_string(writeStatus));

      apm::logger::debug("=== BinderService::handleTransact: EXIT ===");
      return writeStatus;
    }

    return STATUS_FAILED_TRANSACTION;
  }

private:
  BinderService &m_owner;
};

void *onCreate(void *args) {
  return new BinderServiceState(*static_cast<BinderService *>(args));
}

void onDestroy(void *userData) {
  auto *state = static_cast<BinderServiceState *>(userData);
  delete state;
}

binder_status_t onTransact(AIBinder *binder, transaction_code_t code,
                           const AParcel *in, AParcel *out) {
  if (__builtin_available(android 34, *)) {
    auto *state =
        static_cast<BinderServiceState *>(AIBinder_getUserData(binder));
    if (!state)
      return STATUS_FAILED_TRANSACTION;
    return state->handleTransact(code, in, out);
  }
  return STATUS_FAILED_TRANSACTION;
}

} // namespace

BinderService::BinderService(const std::string &instanceName,
                             apm::ams::ModuleManager &moduleManager,
                             apm::daemon::SecurityManager &securityManager)
    : m_instanceName(instanceName),
      m_dispatcher(moduleManager, securityManager), m_binder(nullptr),
      m_started(false) {}

BinderService::~BinderService() {
  if (__builtin_available(android 34, *)) {
    if (m_binder) {
      AIBinder_decStrong(m_binder);
    }
  }
}

void BinderService::dispatchRequest(
    apm::ipc::Request &req, apm::ipc::Response &resp,
    const apm::ipc::ProgressCallback &progressCb) {
  m_dispatcher.dispatch(req, resp, progressCb);
}

bool BinderService::start(std::string *errorMsg) {
  if (__builtin_available(android 34, *)) {
    apm::logger::info("BinderService: checking Binder runtime availability");
    if (!apm::binder::isBinderRuntimeAvailable(errorMsg)) {
      apm::logger::error("BinderService: Binder runtime unavailable");
      return false;
    }

    if (!gServiceClass) {
      apm::logger::info("BinderService: defining Binder service class");
      gServiceClass = AIBinder_Class_define(apm::binder::INTERFACE, onCreate,
                                            onDestroy, onTransact);
    }

    if (!gServiceClass) {
      if (errorMsg)
        *errorMsg = "Failed to create Binder class";
      return false;
    }

    apm::logger::info("BinderService: creating Binder instance object");
    m_binder = AIBinder_new(gServiceClass, this);
    if (!m_binder) {
      if (errorMsg)
        *errorMsg = "Failed to allocate Binder object";
      return false;
    }

    std::string regErr;
    apm::logger::info("BinderService: registering service '" + m_instanceName +
                      "'");
    if (!apm::binder::addService(m_binder, m_instanceName, &regErr)) {
      if (errorMsg)
        *errorMsg = regErr;
      if (__builtin_available(android 34, *)) {
        AIBinder_decStrong(m_binder);
      }
      m_binder = nullptr;
      return false;
    }

    apm::logger::info(
        "BinderService: configuring thread pool (max=1, callerJoins=true)");
    if (!apm::binder::configureThreadPool(1, true, errorMsg)) {
      if (__builtin_available(android 34, *)) {
        AIBinder_decStrong(m_binder);
      }
      m_binder = nullptr;
      return false;
    }

    m_started = true;
    apm::logger::info("BinderService: registered as " + m_instanceName);
    return true;
  }

  if (errorMsg) {
    *errorMsg = "NDK Binder requires Android API level 34 or newer";
  }
  return false;
}

void BinderService::joinThreadPool() {
  if (!m_started) {
    apm::logger::warn("BinderService: joinThreadPool called before start()");
    return;
  }
  if (__builtin_available(android 34, *)) {
    apm::logger::info("BinderService: joining Binder thread pool");
    apm::binder::joinThreadPool();
  } else {
    apm::logger::warn("BinderService: joinThreadPool unavailable below API 34");
  }
}

bool BinderService::isStarted() const { return m_started; }

} // namespace apm::ipc

#else

namespace apm::ipc {

BinderService::BinderService(
    const std::string & /*instanceName*/,
    apm::ams::ModuleManager & /*moduleManager*/,
    apm::daemon::SecurityManager & /*securityManager*/) {}

BinderService::~BinderService() = default;

bool BinderService::start(std::string *errorMsg) {
#if defined(__ANDROID__)
  if (errorMsg) {
    *errorMsg = "NDK Binder support requires Android API level 34 or newer";
  }
#else
  if (errorMsg) {
    *errorMsg = "Binder transport is only available on Android builds";
  }
#endif
  return false;
}

void BinderService::joinThreadPool() {}

bool BinderService::isStarted() const { return false; }

} // namespace apm::ipc

#endif
