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
  auto &sym = symbols();
  sym.handle = dlopen(libName, RTLD_NOW);
  if (!sym.handle) {
    if (errorMsg)
      *errorMsg = std::string("dlopen failed for ") + libName;
    return false;
  }

  bool ok = true;
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.addService),
                   "AServiceManager_addService");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.getService),
                   "AServiceManager_getService");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.waitForService),
                   "AServiceManager_waitForService");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.setThreadPoolMax),
                   "ABinderProcess_setThreadPoolMaxThreadCount");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.startThreadPool),
                   "ABinderProcess_startThreadPool");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.joinThreadPool),
                   "ABinderProcess_joinThreadPool");

  if (!ok) {
    if (errorMsg)
      *errorMsg = std::string("Missing Binder runtime symbols in ") + libName;
    dlclose(sym.handle);
    sym = BinderSymbols{};
  }
  return ok;
}

bool ensureLoaded(std::string *errorMsg) {
  if (__builtin_available(android 34, *)) {
    static std::once_flag flag;
    static bool loaded = false;
    std::call_once(flag, [&]() {
      const std::vector<const char *> candidates = {"libbinder_ndk.so",
                                                    "libbinder.so"};
      for (const char *lib : candidates) {
        if (tryLoadFrom(lib, errorMsg)) {
          loaded = true;
          break;
        }
      }
      if (!loaded && errorMsg && errorMsg->empty()) {
        *errorMsg = "Unable to load Binder runtime (libbinder_ndk.so)";
      }
    });
    return loaded;
  }

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
  if (!ensureLoaded(errorMsg))
    return false;
  if (!binder) {
    if (errorMsg)
      *errorMsg = "Binder service handle is null";
    return false;
  }
  auto status = symbols().addService(binder, instance.c_str());
  if (status != STATUS_OK) {
    if (errorMsg)
      *errorMsg = "AServiceManager_addService failed with status " +
                  std::to_string(status);
    return false;
  }
  return true;
}

AIBinder *getService(const std::string &instance, bool wait,
                     std::string *errorMsg) {
  AIBinder *handle = nullptr;
  if (!ensureLoaded(errorMsg))
    return handle;

  auto fn = wait ? symbols().waitForService : symbols().getService;
  if (!fn) {
    if (errorMsg)
      *errorMsg = "Binder runtime missing service lookup entry points";
    return handle;
  }

  AIBinder *raw = fn(instance.c_str());
  if (!raw && errorMsg) {
    *errorMsg = "Service " + instance + " not found";
  }
  return raw;
}

bool configureThreadPool(int maxThreads, bool callerJoins,
                         std::string *errorMsg) {
  (void)callerJoins;
  if (!ensureLoaded(errorMsg))
    return false;

  if (!symbols().setThreadPoolMax || !symbols().startThreadPool) {
    if (errorMsg)
      *errorMsg = "Binder runtime missing thread pool symbols";
    return false;
  }

  symbols().setThreadPoolMax(maxThreads);
  symbols().startThreadPool();
  return true;
}

void joinThreadPool() {
  std::string err;
  if (!ensureLoaded(&err)) {
    apm::logger::warn("Binder runtime unavailable: " + err);
    return;
  }

  if (symbols().joinThreadPool) {
    symbols().joinThreadPool();
  } else {
    apm::logger::warn(
        "Binder runtime missing ABinderProcess_joinThreadPool symbol");
  }
}

} // namespace apm::binder

#endif // defined(__ANDROID__)
