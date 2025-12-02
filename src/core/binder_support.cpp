/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: binder_support.cpp
 * Purpose: Resolve Binder runtime symbols dynamically and expose helpers that
 * let APM switch to Binder on Android while keeping non-Android builds
 * functional.
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
 * The helper functions here remain for reference, but the runtime
 * operates in IPC-only mode and does not depend on Binder.
 */

#include "binder_support.hpp"

#if defined(__ANDROID__)

#include "logger.hpp"

#include <android/binder_ibinder.h>
#include <dlfcn.h>

#include <mutex>
#include <vector>

namespace apm::binder {

namespace {

struct BinderSymbols {
  void *handle = nullptr;
  binder_status_t (*addService)(AIBinder *, const char *) = nullptr;
  AIBinder *(*getService)(const char *) = nullptr;
  AIBinder *(*waitForService)(const char *) = nullptr;
  void (*setThreadPoolMax)(int) = nullptr;
  void (*startThreadPool)() = nullptr;
  void (*joinThreadPool)() = nullptr;
};

BinderSymbols &symbols() {
  static BinderSymbols sym{};
  return sym;
}

bool loadSymbol(void *handle, void **fnOut, const char *name) {
  *fnOut = dlsym(handle, name);
  return *fnOut != nullptr;
}

bool tryLoadFrom(const char *libName, std::string *errorMsg) {
  apm::logger::debug(std::string("tryLoadFrom: attempting dlopen for ") +
                     libName);

  auto &sym = symbols();
  sym.handle = dlopen(libName, RTLD_NOW);
  if (!sym.handle) {
    const char *err = dlerror();
    apm::logger::debug(std::string("tryLoadFrom: dlopen failed: ") +
                       (err ? err : "unknown"));

    if (errorMsg)
      *errorMsg = std::string("dlopen failed for ") + libName;
    return false;
  }

  apm::logger::debug(std::string("tryLoadFrom: dlopen succeeded, handle=") +
                     std::to_string(reinterpret_cast<uintptr_t>(sym.handle)));

  bool ok = true;

  apm::logger::debug("tryLoadFrom: loading AServiceManager_addService");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.addService),
                   "AServiceManager_addService");
  apm::logger::debug(std::string("tryLoadFrom: AServiceManager_addService ") +
                     (sym.addService ? "found" : "NOT FOUND"));

  apm::logger::debug("tryLoadFrom: loading AServiceManager_getService");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.getService),
                   "AServiceManager_getService");
  apm::logger::debug(std::string("tryLoadFrom: AServiceManager_getService ") +
                     (sym.getService ? "found" : "NOT FOUND"));

  apm::logger::debug("tryLoadFrom: loading AServiceManager_waitForService");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.waitForService),
                   "AServiceManager_waitForService");
  apm::logger::debug(
      std::string("tryLoadFrom: AServiceManager_waitForService ") +
      (sym.waitForService ? "found" : "NOT FOUND"));

  apm::logger::debug(
      "tryLoadFrom: loading ABinderProcess_setThreadPoolMaxThreadCount");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.setThreadPoolMax),
                   "ABinderProcess_setThreadPoolMaxThreadCount");
  apm::logger::debug(
      std::string("tryLoadFrom: ABinderProcess_setThreadPoolMaxThreadCount ") +
      (sym.setThreadPoolMax ? "found" : "NOT FOUND"));

  apm::logger::debug("tryLoadFrom: loading ABinderProcess_startThreadPool");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.startThreadPool),
                   "ABinderProcess_startThreadPool");
  apm::logger::debug(
      std::string("tryLoadFrom: ABinderProcess_startThreadPool ") +
      (sym.startThreadPool ? "found" : "NOT FOUND"));

  apm::logger::debug("tryLoadFrom: loading ABinderProcess_joinThreadPool");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.joinThreadPool),
                   "ABinderProcess_joinThreadPool");
  apm::logger::debug(
      std::string("tryLoadFrom: ABinderProcess_joinThreadPool ") +
      (sym.joinThreadPool ? "found" : "NOT FOUND"));

  if (!ok) {
    apm::logger::debug("tryLoadFrom: symbol loading incomplete, cleaning up");

    if (errorMsg)
      *errorMsg = std::string("Missing Binder runtime symbols in ") + libName;
    dlclose(sym.handle);
    sym = BinderSymbols{};
  } else {
    apm::logger::debug("tryLoadFrom: all symbols loaded successfully");
  }

  return ok;
}

bool ensureLoaded(std::string *errorMsg) {
  apm::logger::debug("ensureLoaded: checking API level availability");

  if (__builtin_available(android 34, *)) {
    apm::logger::debug(
        "ensureLoaded: API 34+ available, proceeding with symbol loading");

    static std::once_flag flag;
    static bool loaded = false;
    std::call_once(flag, [&]() {
      apm::logger::debug(
          "ensureLoaded: first call, attempting dynamic library load");

      const std::vector<const char *> candidates = {"libbinder_ndk.so",
                                                    "libbinder.so"};
      for (const char *lib : candidates) {
        apm::logger::debug(std::string("ensureLoaded: trying to load ") + lib);

        if (tryLoadFrom(lib, errorMsg)) {
          apm::logger::debug(std::string("ensureLoaded: successfully loaded ") +
                             lib);
          loaded = true;
          break;
        } else {
          apm::logger::debug(
              std::string("ensureLoaded: failed to load ") + lib +
              (errorMsg && !errorMsg->empty() ? ": " + *errorMsg : ""));
        }
      }
      if (!loaded && errorMsg && errorMsg->empty()) {
        *errorMsg = "Unable to load Binder runtime (libbinder_ndk.so)";
      }
    });

    apm::logger::debug(std::string("ensureLoaded: returning ") +
                       (loaded ? "true" : "false"));
    return loaded;
  }

  apm::logger::debug("ensureLoaded: API < 34, Binder unavailable");
  if (errorMsg) {
    *errorMsg = "Binder runtime requires Android API level 34 or newer";
  }
  return false;
}

} // namespace

bool isBinderRuntimeAvailable(std::string *errorMsg) {
  return ensureLoaded(errorMsg);
}

bool addService(AIBinder *binder, const std::string &instance,
                std::string *errorMsg) {
  apm::logger::debug("addService: ENTER for instance '" + instance + "'");

  if (!ensureLoaded(errorMsg)) {
    apm::logger::debug("addService: ensureLoaded failed, aborting");
    return false;
  }

  if (!binder) {
    apm::logger::debug("addService: NULL binder pointer provided");
    if (errorMsg)
      *errorMsg = "Binder service handle is null";
    return false;
  }

  apm::logger::debug("addService: binder handle=" +
                     std::to_string(reinterpret_cast<uintptr_t>(binder)));
  apm::logger::debug("addService: calling AServiceManager_addService");

  binder_status_t status = STATUS_UNKNOWN_ERROR;
  try {
    status = symbols().addService(binder, instance.c_str());

    apm::logger::debug(
        "addService: AServiceManager_addService returned status=" +
        std::to_string(status) + " (" +
        (status == STATUS_OK                   ? "OK"
         : status == STATUS_PERMISSION_DENIED  ? "PERMISSION_DENIED"
         : status == STATUS_BAD_VALUE          ? "BAD_VALUE"
         : status == STATUS_FAILED_TRANSACTION ? "FAILED_TRANSACTION"
                                               : "OTHER") +
        ")");
  } catch (...) {
    apm::logger::error(
        "addService: EXCEPTION during AServiceManager_addService");
    if (errorMsg)
      *errorMsg = "Exception while registering service " + instance;
    return false;
  }

  if (status != STATUS_OK) {
    apm::logger::debug("addService: registration FAILED with status " +
                       std::to_string(status));
    if (errorMsg)
      *errorMsg = "AServiceManager_addService failed with status " +
                  std::to_string(status);
    return false;
  }

  apm::logger::debug("addService: registration SUCCESS for '" + instance + "'");
  return true;
}

AIBinder *getService(const std::string &instance, bool wait,
                     std::string *errorMsg) {
  apm::logger::debug("getService: ENTER for instance '" + instance +
                     "' wait=" + (wait ? "true" : "false"));

  AIBinder *handle = nullptr;
  if (!ensureLoaded(errorMsg)) {
    apm::logger::debug("getService: ensureLoaded failed, returning NULL");
    return handle;
  }

  auto fn = wait ? symbols().waitForService : symbols().getService;
  if (!fn) {
    apm::logger::debug("getService: function pointer is NULL");
    if (errorMsg)
      *errorMsg = "Binder runtime missing service lookup entry points";
    return handle;
  }

  apm::logger::debug("getService: calling " +
                     std::string(wait ? "AServiceManager_waitForService"
                                      : "AServiceManager_getService"));

  AIBinder *raw = fn(instance.c_str());

  if (!raw) {
    apm::logger::debug("getService: lookup returned NULL (service not found)");
    if (errorMsg)
      *errorMsg = "Service " + instance + " not found";
    return nullptr;
  }

  apm::logger::debug("getService: obtained raw handle=" +
                     std::to_string(reinterpret_cast<uintptr_t>(raw)));

  apm::logger::debug("getService: calling AIBinder_incStrong");
  try {
    AIBinder_incStrong(raw);
    apm::logger::debug("getService: AIBinder_incStrong SUCCESS");
  } catch (...) {
    apm::logger::error("getService: EXCEPTION in AIBinder_incStrong");
    if (errorMsg)
      *errorMsg = "Failed to acquire strong reference to service " + instance;
    return nullptr;
  }

  apm::logger::debug("getService: returning handle=" +
                     std::to_string(reinterpret_cast<uintptr_t>(raw)));
  return raw;
}

bool configureThreadPool(int maxThreads, bool callerJoins,
                         std::string *errorMsg) {
  apm::logger::debug(
      "configureThreadPool: ENTER maxThreads=" + std::to_string(maxThreads) +
      " callerJoins=" + (callerJoins ? "true" : "false"));

  (void)callerJoins;
  if (!ensureLoaded(errorMsg)) {
    apm::logger::debug("configureThreadPool: ensureLoaded failed");
    return false;
  }

  if (!symbols().setThreadPoolMax || !symbols().startThreadPool) {
    apm::logger::debug("configureThreadPool: missing thread pool symbols");
    if (errorMsg)
      *errorMsg = "Binder runtime missing thread pool symbols";
    return false;
  }

  apm::logger::debug("configureThreadPool: calling setThreadPoolMax(" +
                     std::to_string(maxThreads) + ")");

  try {
    symbols().setThreadPoolMax(maxThreads);
    apm::logger::debug("configureThreadPool: setThreadPoolMax SUCCESS");

    apm::logger::debug("configureThreadPool: calling startThreadPool");
    symbols().startThreadPool();
    apm::logger::debug("configureThreadPool: startThreadPool SUCCESS");
  } catch (...) {
    apm::logger::error(
        "configureThreadPool: EXCEPTION during thread pool setup");
    if (errorMsg)
      *errorMsg = "Exception while configuring Binder thread pool";
    return false;
  }

  apm::logger::debug("configureThreadPool: EXIT success");
  return true;
}

void joinThreadPool() {
  apm::logger::debug("joinThreadPool: ENTER");

  std::string err;
  if (!ensureLoaded(&err)) {
    apm::logger::warn("joinThreadPool: Binder runtime unavailable: " + err);
    return;
  }

  if (symbols().joinThreadPool) {
    apm::logger::debug(
        "joinThreadPool: calling ABinderProcess_joinThreadPool (BLOCKING)");
    try {
      symbols().joinThreadPool();
      apm::logger::debug(
          "joinThreadPool: ABinderProcess_joinThreadPool returned");
    } catch (...) {
      apm::logger::error("joinThreadPool: EXCEPTION during thread pool join");
    }
  } else {
    apm::logger::warn(
        "joinThreadPool: ABinderProcess_joinThreadPool symbol missing");
  }

  apm::logger::debug("joinThreadPool: EXIT");
}

} // namespace apm::binder

#endif // defined(__ANDROID__)
