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

#if defined(__ANDROID__)

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>

#include <limits>
#include <utility>

namespace apm::ipc {

namespace {

AIBinder_Class *gServiceClass = nullptr;

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
  if (value.size() >
      static_cast<std::size_t>(std::numeric_limits<int32_t>::max())) {
    return STATUS_BAD_VALUE;
  }
  return AParcel_writeString(parcel, value.c_str(),
                             static_cast<int32_t>(value.size()));
}

// Send a serialized response to a remote binder (progress callback).
void sendProgressToClient(const ndk::SpAIBinder &callback,
                          const apm::ipc::Response &progress) {
  if (!callback.get())
    return;

  AParcel *parcel = nullptr;
  if (AIBinder_prepareTransaction(callback.get(), &parcel) != STATUS_OK) {
    apm::logger::warn("BinderService: failed to prepare progress parcel");
    return;
  }

  std::string payload = serializeResponse(progress);
  if (writeParcelString(parcel, payload) != STATUS_OK) {
    apm::logger::warn("BinderService: failed to encode progress frame");
    AParcel_delete(parcel);
    return;
  }

  binder_status_t txStatus =
      AIBinder_transact(callback.get(), apm::binder::TX_PROGRESS_EVENT, parcel,
                        nullptr, FLAG_ONEWAY);
  AParcel_delete(parcel);
  if (txStatus != STATUS_OK) {
    apm::logger::warn("BinderService: progress transact failed with status " +
                      std::to_string(txStatus));
  }
}

class BinderServiceState {
public:
  explicit BinderServiceState(BinderService &owner) : m_owner(owner) {}

  binder_status_t handleTransact(transaction_code_t code, const AParcel *in,
                                 AParcel *out) {
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
    ndk::SpAIBinder callback(cbRaw);

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
      sendProgressToClient(callback, progress);
    };

    m_owner.m_dispatcher.dispatch(req, resp, progressCb);
    std::string payload = serializeResponse(resp);
    return writeParcelString(out, payload);
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
  auto *state = static_cast<BinderServiceState *>(AIBinder_getUserData(binder));
  if (!state)
    return STATUS_FAILED_TRANSACTION;
  return state->handleTransact(code, in, out);
}

} // namespace

BinderService::BinderService(const std::string &instanceName,
                             apm::ams::ModuleManager &moduleManager,
                             apm::daemon::SecurityManager &securityManager)
    : m_instanceName(instanceName),
      m_dispatcher(moduleManager, securityManager), m_binder(nullptr),
      m_started(false) {}

bool BinderService::start(std::string *errorMsg) {
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

  m_binder = ndk::SpAIBinder(AIBinder_new(gServiceClass, this));
  if (!m_binder.get()) {
    if (errorMsg)
      *errorMsg = "Failed to allocate Binder object";
    return false;
  }

  std::string regErr;
  if (!apm::binder::addService(m_binder, m_instanceName, &regErr)) {
    if (errorMsg)
      *errorMsg = regErr;
    return false;
  }

  if (!apm::binder::configureThreadPool(1, true, errorMsg)) {
    return false;
  }

  m_started = true;
  apm::logger::info("BinderService: registered as " + m_instanceName);
  return true;
}

void BinderService::joinThreadPool() {
  if (!m_started) {
    apm::logger::warn("BinderService: joinThreadPool called before start()");
    return;
  }
  apm::binder::joinThreadPool();
}

bool BinderService::isStarted() const { return m_started; }

} // namespace apm::ipc

#else

namespace apm::ipc {

BinderService::BinderService(
    const std::string & /*instanceName*/,
    apm::ams::ModuleManager & /*moduleManager*/,
    apm::daemon::SecurityManager & /*securityManager*/) {}

bool BinderService::start(std::string * /*errorMsg*/) { return false; }

void BinderService::joinThreadPool() {}

bool BinderService::isStarted() const { return false; }

} // namespace apm::ipc

#endif
