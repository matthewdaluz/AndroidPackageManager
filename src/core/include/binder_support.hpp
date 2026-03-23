/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: binder_support.hpp
 * Purpose: Provide lightweight helpers for loading Binder runtime symbols
 * safely at runtime so builds can fall back when Binder is unavailable.
 * Last Modified: 2026-03-15 11:56:16.537911560 -0400.
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

#pragma once

/*
 * NOTICE: Binder transport deprecated
 * ----------------------------------
 * Binder is not used at runtime; IPC socket is the only transport.
 * This header remains to preserve optional Binder support for future work.
 */
#include <string>

struct AIBinder;

namespace apm::binder {

#if defined(__ANDROID__)

// Returns true when Binder runtime symbols can be resolved from libbinder_ndk
// or libbinder at runtime.
bool isBinderRuntimeAvailable(std::string *errorMsg = nullptr);

// Register the given Binder service instance with the platform service
// manager. Returns false if the required symbols are missing or registration
// fails.
bool addService(AIBinder *binder, const std::string &instance,
                std::string *errorMsg = nullptr);

// Fetch a service by name. When |wait| is true this will block until the
// service becomes available. Returns an empty SpAIBinder on failure.
AIBinder *getService(const std::string &instance, bool wait,
                     std::string *errorMsg = nullptr);

// Configure and start the Binder thread pool. |callerJoins| indicates whether
// the caller will later invoke joinThreadPool.
bool configureThreadPool(int maxThreads, bool callerJoins,
                         std::string *errorMsg = nullptr);

// Enter the Binder thread pool loop if the runtime is available.
void joinThreadPool();

#else

inline bool isBinderRuntimeAvailable(std::string *errorMsg = nullptr) {
  if (errorMsg) {
    *errorMsg = "NDK Binder is unavailable on this platform";
  }
  return false;
}

inline bool addService(AIBinder * /*binder*/, const std::string & /*instance*/,
                       std::string *errorMsg = nullptr) {
  if (errorMsg) {
    *errorMsg = "NDK Binder runtime not available";
  }
  return false;
}

inline AIBinder *getService(const std::string & /*instance*/, bool /*wait*/,
                            std::string *errorMsg = nullptr) {
  if (errorMsg) {
    *errorMsg = "NDK Binder runtime not available";
  }
  return nullptr;
}

inline bool configureThreadPool(int /*maxThreads*/, bool /*callerJoins*/,
                                std::string *errorMsg = nullptr) {
  if (errorMsg) {
    *errorMsg = "NDK Binder runtime not available";
  }
  return false;
}

inline void joinThreadPool() {}

#endif

} // namespace apm::binder
