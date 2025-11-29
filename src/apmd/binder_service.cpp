/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: binder_service.cpp
 * Purpose: Implement the Binder endpoint that exposes apmd operations to
 * Android clients, replacing the legacy UNIX socket transport.
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
#include <utility>

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
        callback, apm::binder::TX_PROGRESS_EVENT, &parcel, &reply,
        FLAG_ONEWAY);
    if (reply) {
      AParcel_delete(reply);
    }
    if (txStatus != STATUS_OK) {
      apm::logger::warn(
          "BinderService: progress transact failed with status " +
          std::to_string(txStatus));
    }
  }
}

class BinderServiceState {
public:
  explicit BinderServiceState(BinderService &owner) : m_owner(owner) {}

  binder_status_t handleTransact(transaction_code_t code, const AParcel *in,
                                 AParcel *out) {
    if (__builtin_available(android 34, *)) {
      if (code != apm::binder::TX_SEND_REQUEST) {
        return STATUS_UNKNOWN_TRANSACTION;
      }

      std::string rawRequest;
      binder_status_t status = readParcelString(in, rawRequest);
      if (status != STATUS_OK) {
        return status;
      }

      AIBinder *cbRaw = nullptr;
      status = AParcel_readStrongBinder(in, &cbRaw);
      if (status != STATUS_OK) {
        return status;
      }
      ScopedStrongBinder callback(cbRaw);

      apm::ipc::Request req;
      apm::ipc::Response resp;
      std::string parseErr;
      if (!parseRequest(rawRequest, req, &parseErr)) {
        resp.success = false;
        resp.message =
            parseErr.empty() ? "Bad request" : ("Bad request: " + parseErr);
        resp.status = apm::ipc::ResponseStatus::Error;
        std::string payload = serializeResponse(resp);
        return writeParcelString(out, payload);
      }

      apm::logger::info("BinderService: received request");
      auto progressCb = [&](const apm::ipc::Response &progress) {
        sendProgressToClient(callback.get(), progress);
      };

      m_owner.dispatchRequest(req, resp, progressCb);
      std::string payload = serializeResponse(resp);
      return writeParcelString(out, payload);
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
    if (!apm::binder::isBinderRuntimeAvailable(errorMsg)) {
      apm::logger::error("BinderService: Binder runtime unavailable");
      return false;
    }

    if (!gServiceClass) {
      gServiceClass = AIBinder_Class_define(apm::binder::INTERFACE, onCreate,
                                            onDestroy, onTransact);
    }

    if (!gServiceClass) {
      if (errorMsg)
        *errorMsg = "Failed to create Binder class";
      return false;
    }

    m_binder = AIBinder_new(gServiceClass, this);
    if (!m_binder) {
      if (errorMsg)
        *errorMsg = "Failed to allocate Binder object";
      return false;
    }

    std::string regErr;
    if (!apm::binder::addService(m_binder, m_instanceName, &regErr)) {
      if (errorMsg)
        *errorMsg = regErr;
      if (__builtin_available(android 34, *)) {
        AIBinder_decStrong(m_binder);
      }
      m_binder = nullptr;
      return false;
    }

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
