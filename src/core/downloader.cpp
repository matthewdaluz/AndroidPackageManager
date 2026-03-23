/*
 * APM - Android Package Manager
 *
 * RedHead Industries - Technologies Branch
 * Copyright (C) 2025 RedHead Industries
 *
 * File: downloader.cpp
 * Purpose: Implement libcurl downloads with progress reporting and CA bundle
 * management. Last Modified: 2026-03-15 11:56:16.537647032 -0400.
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

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

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
      apm::logger::warn("psa_crypto_init() failed: " + std::to_string(status));
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

bool prepareOutputFile(const std::string &dest, FILE *&outFile,
                       std::string *errorMsg, const char *logPrefix) {
  auto pos = dest.find_last_of('/');
  if (pos != std::string::npos) {
    std::string parent = dest.substr(0, pos);
    if (!apm::fs::createDirs(parent)) {
      if (errorMsg)
        *errorMsg = "Failed to create directory: " + parent;
      apm::logger::error(std::string(logPrefix) +
                         ": cannot create directory: " + parent);
      return false;
    }
  }

  outFile = std::fopen(dest.c_str(), "wb");
  if (!outFile) {
    if (errorMsg) {
      *errorMsg =
          "Failed to open destination '" + dest + "': " + std::strerror(errno);
    }
    apm::logger::error(std::string(logPrefix) +
                       ": cannot open destination: " + dest);
    return false;
  }

  return true;
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

  ensureCurlInitialized();
  ensurePsaInitialized();

  FILE *outFile = nullptr;
  if (!prepareOutputFile(dest, outFile, errorMsg, "downloadFile")) {
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
      *errorMsg =
          "curl_easy_perform() failed: " + std::string(curl_easy_strerror(res));
    apm::logger::error("downloadFile: curl failed: " +
                       std::string(curl_easy_strerror(res)));
    std::remove(dest.c_str());
    return false;
  }

  return true;
}

bool downloadFiles(const std::vector<DownloadRequest> &requests,
                   std::vector<DownloadResult> &results,
                   std::size_t maxParallel, std::string *errorMsg) {
  results.clear();
  results.resize(requests.size());

  if (requests.empty())
    return true;

  const std::size_t parallel =
      std::max<std::size_t>(1, std::min<std::size_t>(3, maxParallel));

  ensureCurlInitialized();
  ensurePsaInitialized();

  CURLM *multi = curl_multi_init();
  if (!multi) {
    if (errorMsg)
      *errorMsg = "curl_multi_init() failed";
    apm::logger::error("downloadFiles: curl_multi_init() failed");
    return false;
  }

  struct ActiveTransfer {
    std::size_t requestIndex = 0;
    const DownloadRequest *request = nullptr;
    DownloadResult *result = nullptr;
    std::unique_ptr<FILE, decltype(&std::fclose)> file{nullptr, &std::fclose};
    CURL *easy = nullptr;
    CurlProgressContext progress;
  };

  std::vector<std::unique_ptr<ActiveTransfer>> active;
  active.reserve(parallel);
  bool allOk = true;
  std::size_t nextIndex = 0;

  auto startTransfer = [&](std::size_t idx) {
    const auto &req = requests[idx];
    DownloadResult &res = results[idx];
    res.url = req.url;
    res.destination = req.destination;
    res.success = false;
    res.errorMsg.clear();

    if (req.url.empty() || req.destination.empty()) {
      res.errorMsg = "URL or dest is empty";
      allOk = false;
      return;
    }

    auto transfer = std::make_unique<ActiveTransfer>();
    transfer->requestIndex = idx;
    transfer->request = &req;
    transfer->result = &res;

    FILE *file = nullptr;
    if (!prepareOutputFile(req.destination, file, &res.errorMsg,
                           "downloadFiles")) {
      allOk = false;
      return;
    }
    transfer->file.reset(file);

    transfer->progress.cb = req.progressCb;
    transfer->progress.url = req.url;
    transfer->progress.dest = req.destination;

    transfer->easy = curl_easy_init();
    if (!transfer->easy) {
      res.errorMsg = "curl_easy_init() failed";
      std::remove(req.destination.c_str());
      transfer->file.reset();
      allOk = false;
      return;
    }

    curl_easy_setopt(transfer->easy, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(transfer->easy, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(transfer->easy, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(transfer->easy, CURLOPT_SSLVERSION,
                     CURL_SSLVERSION_TLSv1_2);
    curl_easy_setopt(transfer->easy, CURLOPT_WRITEFUNCTION, &writeCallback);
    curl_easy_setopt(transfer->easy, CURLOPT_WRITEDATA,
                     static_cast<void *>(transfer->file.get()));
    ensureCurlCaBundle(transfer->easy);

    if (transfer->progress.cb) {
      curl_easy_setopt(transfer->easy, CURLOPT_NOPROGRESS, 0L);
      curl_easy_setopt(transfer->easy, CURLOPT_XFERINFOFUNCTION,
                       &curlProgressCallback);
      curl_easy_setopt(transfer->easy, CURLOPT_XFERINFODATA,
                       static_cast<void *>(&transfer->progress));
    } else {
      curl_easy_setopt(transfer->easy, CURLOPT_NOPROGRESS, 1L);
    }

    curl_easy_setopt(transfer->easy, CURLOPT_PRIVATE,
                     static_cast<void *>(transfer.get()));

    CURLMcode addRes = curl_multi_add_handle(multi, transfer->easy);
    if (addRes != CURLM_OK) {
      res.errorMsg = "curl_multi_add_handle() failed: " +
                     std::string(curl_multi_strerror(addRes));
      curl_easy_cleanup(transfer->easy);
      transfer->easy = nullptr;
      std::remove(req.destination.c_str());
      transfer->file.reset();
      allOk = false;
      return;
    }

    active.push_back(std::move(transfer));
  };

  auto launchAvailable = [&]() {
    while (nextIndex < requests.size() && active.size() < parallel) {
      startTransfer(nextIndex);
      ++nextIndex;
    }
  };

  launchAvailable();

  int stillRunning = 0;
  curl_multi_perform(multi, &stillRunning);

  while (!active.empty()) {
    int numfds = 0;
    curl_multi_wait(multi, nullptr, 0, 1000, &numfds);
    curl_multi_perform(multi, &stillRunning);

    CURLMsg *msg;
    int msgsLeft = 0;
    while ((msg = curl_multi_info_read(multi, &msgsLeft))) {
      if (!msg || msg->msg != CURLMSG_DONE) {
        continue;
      }

      ActiveTransfer *transfer = nullptr;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE,
                        reinterpret_cast<void **>(&transfer));
      if (!transfer) {
        continue;
      }

      const bool success = (msg->data.result == CURLE_OK);
      if (!success) {
        transfer->result->errorMsg =
            "curl_easy_perform() failed: " +
            std::string(curl_easy_strerror(msg->data.result));
        allOk = false;
      } else {
        transfer->result->success = true;
        transfer->result->errorMsg.clear();
      }

      curl_off_t downloaded = 0;
      curl_off_t uploaded = 0;
      curl_easy_getinfo(msg->easy_handle, CURLINFO_SIZE_DOWNLOAD_T,
                        &downloaded);
      curl_easy_getinfo(msg->easy_handle, CURLINFO_SIZE_UPLOAD_T, &uploaded);
      transfer->progress.lastDownloaded =
          static_cast<std::uint64_t>(downloaded);
      transfer->progress.lastUploaded = static_cast<std::uint64_t>(uploaded);
      emitFinalProgress(transfer->progress, success);

      curl_multi_remove_handle(multi, msg->easy_handle);
      curl_easy_cleanup(msg->easy_handle);

      if (transfer->file) {
        transfer->file.reset();
      }
      if (!success && transfer->request) {
        std::remove(transfer->request->destination.c_str());
      }

      auto it =
          std::find_if(active.begin(), active.end(),
                       [transfer](const std::unique_ptr<ActiveTransfer> &ptr) {
                         return ptr.get() == transfer;
                       });
      if (it != active.end()) {
        active.erase(it);
      }
    }

    launchAvailable();
  }

  curl_multi_cleanup(multi);

  if (!allOk && errorMsg) {
    for (const auto &res : results) {
      if (!res.success && !res.errorMsg.empty()) {
        *errorMsg = res.errorMsg;
        break;
      }
    }
  }

  return allOk;
}

} // namespace apm::net
