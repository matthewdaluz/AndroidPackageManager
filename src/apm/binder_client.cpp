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

#include "binder_client.hpp"

#include "binder_defs.hpp"
#include "binder_support.hpp"
#include "config.hpp"
#include "logger.hpp"

#include <limits>
#include <string>
#include <utility>

#if defined(__ANDROID__)
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>

#include <mutex>
#endif

namespace apm::ipc {
namespace {

#if defined(__ANDROID__)

class ProgressCallbackReceiver;

AIBinder_Class *getProgressClass() {
  static AIBinder_Class *cls = nullptr;
  static std::once_flag flag;
  std::call_once(flag, [&]() {
    cls = AIBinder_Class_define(apm::binder::PROGRESS_INTERFACE,
                                [](void *args) { return args; },
                                [](void * /*userData*/) {},
                                [](AIBinder *binder, transaction_code_t code,
                                   const AParcel *in, AParcel * /*out*/) {
                                  auto *receiver = static_cast<ProgressCallbackReceiver *>(
                                      AIBinder_getUserData(binder));
                                  if (!receiver)
                                    return STATUS_FAILED_TRANSACTION;
                                  return receiver->handleTransact(code, in);
                                });
  });
  return cls;
}

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

class ProgressCallbackReceiver {
public:
  explicit ProgressCallbackReceiver(ProgressHandler handler)
      : m_handler(std::move(handler)), m_binder(nullptr) {
    if (!m_handler)
      return;

    AIBinder_Class *cls = getProgressClass();
    if (!cls) {
      apm::logger::warn("binder_client: failed to allocate progress binder class");
      return;
    }

    m_binder = ndk::SpAIBinder(AIBinder_new(cls, this));
  }

  ndk::SpAIBinder binder() const { return m_binder; }

  binder_status_t handleTransact(transaction_code_t code,
                                 const AParcel *in) {
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
  ndk::SpAIBinder m_binder;
};

bool sendRequestBinder(const Request &req, Response &resp,
                       const std::string &serviceName, std::string *errorMsg,
                       ProgressHandler progressHandler) {
  std::string runtimeErr;
  if (!apm::binder::isBinderRuntimeAvailable(&runtimeErr)) {
    if (errorMsg)
      *errorMsg = runtimeErr;
    return false;
  }

  ndk::SpAIBinder service =
      apm::binder::getService(serviceName, true, &runtimeErr);
  if (!service.get()) {
    if (errorMsg)
      *errorMsg = runtimeErr;
    return false;
  }

  ProgressCallbackReceiver progressReceiver(progressHandler);

  AParcel *parcel = nullptr;
  if (AIBinder_prepareTransaction(service.get(), &parcel) != STATUS_OK) {
    if (errorMsg)
      *errorMsg = "Failed to prepare Binder transaction";
    return false;
  }

  std::string payload = serializeRequest(req);
  if (writeParcelString(parcel, payload) != STATUS_OK) {
    if (errorMsg)
      *errorMsg = "Failed to write Binder payload";
    AParcel_delete(parcel);
    return false;
  }

  binder_status_t status =
      AParcel_writeStrongBinder(parcel, progressReceiver.binder().get());
  if (status != STATUS_OK) {
    if (errorMsg)
      *errorMsg = "Failed to attach progress callback";
    AParcel_delete(parcel);
    return false;
  }

  AParcel *reply = nullptr;
  status = AIBinder_transact(service.get(), apm::binder::TX_SEND_REQUEST,
                             parcel, &reply, 0);
  AParcel_delete(parcel);
  if (status != STATUS_OK || !reply) {
    if (errorMsg)
      *errorMsg = "Binder transact failed with status " +
                  std::to_string(status);
    return false;
  }

  std::string rawResp;
  binder_status_t readStatus = readParcelString(reply, rawResp);
  AParcel_delete(reply);
  if (readStatus != STATUS_OK) {
    if (errorMsg)
      *errorMsg = "Failed to read Binder reply";
    return false;
  }

  if (!parseResponse(rawResp, resp, errorMsg)) {
    apm::logger::error("binder_client: parseResponse failed for Binder reply");
    return false;
  }

  return true;
}

#endif // __ANDROID__

} // namespace

bool sendRequest(const Request &req, Response &resp,
                 const std::string &serviceName, std::string *errorMsg,
                 ProgressHandler progressHandler) {
#if defined(__ANDROID__)
  return sendRequestBinder(req, resp, serviceName, errorMsg, progressHandler);
#else
  if (errorMsg)
    *errorMsg = "Binder transport is only available on Android builds";
  return false;
#endif
}

} // namespace apm::ipc
