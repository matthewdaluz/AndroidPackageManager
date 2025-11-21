/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: downloader.cpp
 * Purpose: Implement libcurl downloads with progress reporting and CA bundle management.
 * Last Modified: November 18th, 2025. - 3:00 PM Eastern Time.
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

#include "downloader.hpp"
#include "fs.hpp"
#include "logger.hpp"

#include <curl/curl.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>

namespace {

using Clock = std::chrono::steady_clock;

std::once_flag gCurlInitFlag;

void ensureCurlInitialized() {
  std::call_once(gCurlInitFlag,
                 []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

#if defined(__has_attribute)
#if __has_attribute(weak)
#define APM_WEAK __attribute__((weak))
#endif
#endif
#ifndef APM_WEAK
#define APM_WEAK
#endif

extern "C" int psa_crypto_init(void) APM_WEAK;

std::once_flag gPsaInitFlag;

void ensurePsaInitialized() {
  std::call_once(gPsaInitFlag, []() {
    if (!psa_crypto_init)
      return;
    int status = psa_crypto_init();
    if (status != 0) {
      apm::logger::warn("psa_crypto_init() failed: " +
                        std::to_string(status));
    } else {
      apm::logger::debug("psa_crypto_init() completed");
    }
  });
}

struct CurlCaBundleState {
  enum class Mode {
    None,
    File,
    Blob,
  };

  Mode mode = Mode::None;
  std::string source;
  std::string blobData;
  curl_blob blob{};
};

CurlCaBundleState gCaBundleState;
std::once_flag gCaBundleInit;

bool fileExists(const std::string &path) {
  return apm::fs::isRegularFile(path);
}

bool loadCaBundleFromDir(const std::string &dir, std::string &out) {
  if (!apm::fs::isDirectory(dir)) {
    return false;
  }

  auto entries = apm::fs::listDir(dir, false);
  std::string bundle;
  bundle.reserve(entries.size() * 1200);

  for (const auto &entry : entries) {
    std::string fullPath = apm::fs::joinPath(dir, entry);
    if (!apm::fs::isRegularFile(fullPath)) {
      continue;
    }

    std::string pem;
    if (!apm::fs::readFile(fullPath, pem)) {
      continue;
    }

    if (pem.find("BEGIN CERTIFICATE") == std::string::npos) {
      continue;
    }

    bundle.append(pem);
    if (!bundle.empty() && bundle.back() != '\n') {
      bundle.push_back('\n');
    }
  }

  if (bundle.empty()) {
    return false;
  }

  out = std::move(bundle);
  return true;
}

void initCurlCaBundle() {
  CurlCaBundleState state;

  const char *envFile = std::getenv("APM_CAINFO");
  if (envFile && *envFile && fileExists(envFile)) {
    state.mode = CurlCaBundleState::Mode::File;
    state.source = envFile;
    apm::logger::info("Using CA bundle from APM_CAINFO: " + state.source);
    gCaBundleState = std::move(state);
    return;
  }

  static const char *kCandidateFiles[] = {
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/ssl/cert.pem",
      "/system/etc/security/cacert.pem",
      "/system/etc/security/cacerts.pem",
      "/system/etc/security/ca-certificates.crt",
  };

  for (const char *path : kCandidateFiles) {
    if (!path || !*path)
      continue;
    if (!fileExists(path))
      continue;
    state.mode = CurlCaBundleState::Mode::File;
    state.source = path;
    apm::logger::info("Using CA bundle file: " + state.source);
    gCaBundleState = std::move(state);
    return;
  }

  static const char *kCandidateDirs[] = {
      "/system/etc/security/cacerts",
      "/etc/security/cacerts",
  };

  for (const char *dir : kCandidateDirs) {
    if (!dir || !*dir)
      continue;
    std::string data;
    if (!loadCaBundleFromDir(dir, data))
      continue;
    state.mode = CurlCaBundleState::Mode::Blob;
    state.source = dir;
    state.blobData = std::move(data);
    state.blob.flags = CURL_BLOB_NOCOPY;
    apm::logger::info("Built CA bundle from directory: " + state.source);
    gCaBundleState = std::move(state);
    return;
  }

  apm::logger::warn(
      "No CA bundle detected; HTTPS downloads may fail (install certificates "
      "or set APM_CAINFO)");
  gCaBundleState = std::move(state);
}

void ensureCurlCaBundle(CURL *curl) {
  std::call_once(gCaBundleInit, initCurlCaBundle);

  switch (gCaBundleState.mode) {
  case CurlCaBundleState::Mode::File:
    curl_easy_setopt(curl, CURLOPT_CAINFO, gCaBundleState.source.c_str());
    break;
  case CurlCaBundleState::Mode::Blob:
    gCaBundleState.blob.data = gCaBundleState.blobData.data();
    gCaBundleState.blob.len = gCaBundleState.blobData.size();
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &gCaBundleState.blob);
    break;
  case CurlCaBundleState::Mode::None:
  default:
    break;
  }
}

size_t writeCallback(void *ptr, size_t size, size_t nmemb, void *userdata) {
  FILE *file = static_cast<FILE *>(userdata);
  return std::fwrite(ptr, size, nmemb, file);
}

struct CurlProgressContext {
  apm::net::TransferProgressCallback cb;
  std::string url;
  std::string dest;
  std::uint64_t lastDownloaded = 0;
  std::uint64_t lastUploaded = 0;
  double lastDownloadSpeed = 0.0;
  double lastUploadSpeed = 0.0;
  Clock::time_point lastTime = Clock::now();
};

int curlProgressCallback(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                         curl_off_t ultotal, curl_off_t ulnow) {
  auto *ctx = static_cast<CurlProgressContext *>(clientp);
  if (!ctx || !ctx->cb)
    return 0;

  auto now = Clock::now();
  double dt = std::chrono::duration<double>(now - ctx->lastTime).count();
  if (dt <= 0.0)
    dt = 1e-6; // avoid div-by-zero

  ctx->lastDownloadSpeed =
      (static_cast<double>(dlnow) - static_cast<double>(ctx->lastDownloaded)) /
      dt;
  ctx->lastUploadSpeed =
      (static_cast<double>(ulnow) - static_cast<double>(ctx->lastUploaded)) /
      dt;
  ctx->lastDownloaded = static_cast<std::uint64_t>(dlnow);
  ctx->lastUploaded = static_cast<std::uint64_t>(ulnow);
  ctx->lastTime = now;

  apm::net::TransferProgress progress;
  progress.url = ctx->url;
  progress.destination = ctx->dest;
  progress.downloadedBytes = static_cast<std::uint64_t>(dlnow);
  progress.downloadTotal = static_cast<std::uint64_t>(dltotal);
  progress.uploadedBytes = static_cast<std::uint64_t>(ulnow);
  progress.uploadTotal = static_cast<std::uint64_t>(ultotal);
  progress.downloadSpeedBytesPerSec = ctx->lastDownloadSpeed;
  progress.uploadSpeedBytesPerSec = ctx->lastUploadSpeed;
  progress.finished = false;

  ctx->cb(progress);
  return 0;
}

void emitFinalProgress(const CurlProgressContext &ctx, bool success) {
  if (!ctx.cb)
    return;

  apm::net::TransferProgress finalProgress;
  finalProgress.url = ctx.url;
  finalProgress.destination = ctx.dest;
  finalProgress.downloadedBytes = ctx.lastDownloaded;
  finalProgress.downloadTotal = ctx.lastDownloaded;
  finalProgress.uploadedBytes = ctx.lastUploaded;
  finalProgress.uploadTotal = ctx.lastUploaded;
  finalProgress.downloadSpeedBytesPerSec =
      success ? ctx.lastDownloadSpeed : 0.0;
  finalProgress.uploadSpeedBytesPerSec = ctx.lastUploadSpeed;
  finalProgress.finished = true;

  ctx.cb(finalProgress);
}

} // namespace

namespace apm::net {

// Download a URL to disk using libcurl with optional TLS bundle overrides and
// streaming progress callbacks.
bool downloadFile(const std::string &url, const std::string &dest,
                  std::string *errorMsg, TransferProgressCallback progressCb) {
  if (url.empty() || dest.empty()) {
    if (errorMsg)
      *errorMsg = "URL or dest is empty";
    apm::logger::error("downloadFile: URL or dest is empty");
    return false;
  }

  // Ensure parent directory exists
  auto pos = dest.find_last_of('/');
  if (pos != std::string::npos) {
    std::string parent = dest.substr(0, pos);
    if (!apm::fs::createDirs(parent)) {
      if (errorMsg)
        *errorMsg = "Failed to create directory: " + parent;
      apm::logger::error("downloadFile: cannot create directory: " + parent);
      return false;
    }
  }

  ensureCurlInitialized();
  ensurePsaInitialized();

  FILE *outFile = std::fopen(dest.c_str(), "wb");
  if (!outFile) {
    if (errorMsg)
      *errorMsg =
          "Failed to open destination '" + dest + "': " + std::strerror(errno);
    apm::logger::error("downloadFile: cannot open destination: " + dest);
    return false;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    if (errorMsg)
      *errorMsg = "curl_easy_init() failed";
    std::fclose(outFile);
    return false;
  }

  CurlProgressContext ctx;
  ctx.cb = std::move(progressCb);
  ctx.url = url;
  ctx.dest = dest;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &writeCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, outFile);
  ensureCurlCaBundle(curl);

  if (ctx.cb) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &curlProgressCallback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
  } else {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  }

  CURLcode res = curl_easy_perform(curl);

  bool success = (res == CURLE_OK);
  emitFinalProgress(ctx, success);

  curl_easy_cleanup(curl);
  std::fclose(outFile);

  if (res != CURLE_OK) {
    if (errorMsg)
      *errorMsg = "curl_easy_perform() failed: " +
                  std::string(curl_easy_strerror(res));
    apm::logger::error("downloadFile: curl failed: " +
                       std::string(curl_easy_strerror(res)));
    std::remove(dest.c_str());
    return false;
  }

  return true;
}

} // namespace apm::net
