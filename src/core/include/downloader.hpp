/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: downloader.hpp
 * Purpose: Declare the libcurl-based downloader wrapper and progress callbacks.
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

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace apm::net {

struct TransferProgress {
  std::string url;
  std::string destination;

  std::uint64_t downloadedBytes = 0;
  std::uint64_t downloadTotal = 0;

  std::uint64_t uploadedBytes = 0;
  std::uint64_t uploadTotal = 0;

  double downloadSpeedBytesPerSec = 0.0;
  double uploadSpeedBytesPerSec = 0.0;
  bool finished = false;
};

using TransferProgressCallback = std::function<void(const TransferProgress &)>;

struct DownloadRequest {
  std::string url;
  std::string destination;
  TransferProgressCallback progressCb;
};

struct DownloadResult {
  std::string url;
  std::string destination;
  bool success = false;
  std::string errorMsg;
};

// Download a file from URL into dest using libcurl while reporting progress.
bool downloadFile(const std::string &url, const std::string &dest,
                  std::string *errorMsg = nullptr,
                  TransferProgressCallback progressCb = {});

// Download multiple files in parallel with a cap on concurrent transfers.
// Results are ordered to match the requests vector. Returns true only if every
// transfer succeeds.
bool downloadFiles(const std::vector<DownloadRequest> &requests,
                   std::vector<DownloadResult> &results,
                   std::size_t maxParallel = 3,
                   std::string *errorMsg = nullptr);

} // namespace apm::net
