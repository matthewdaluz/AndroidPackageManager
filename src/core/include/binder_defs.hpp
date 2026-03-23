/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: binder_defs.hpp
 * Purpose: Define shared Binder identifiers used by both the daemon and CLI,
 * including service names and transaction codes.
 * Last Modified: 2026-03-15 11:56:16.537647032 -0400.
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

#include <cstdint>

namespace apm::binder {

// Well-known service and interface descriptors for the APM daemon.
inline constexpr const char *SERVICE_NAME = "apm.apmd";
inline constexpr const char *INTERFACE = "com.redhead.apm.IApmService";
inline constexpr const char *PROGRESS_INTERFACE =
    "com.redhead.apm.IApmServiceProgress";

// Transaction codes shared between the service and its progress callback.
inline constexpr std::uint32_t TX_SEND_REQUEST = 1;
inline constexpr std::uint32_t TX_PROGRESS_EVENT = 2;

} // namespace apm::binder
