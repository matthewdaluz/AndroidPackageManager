# Verbose Debug Logging Implementation Guide

This document contains all the changes needed to add extremely verbose debugging to the Binder system.

## 1. src/apmd/apmd.cpp

### Update runDaemon signature (line ~86):
```cpp
int runDaemon(const std::string &serviceName, bool debugMode) {
  waitForDataReady();

  apm::logger::setLogFile("/data/apm/logs/apmd.log");
  apm::logger::setMinLogLevel(debugMode ? apm::logger::Level::Debug
                                        : apm::logger::Level::Info);

  apm::logger::info("apmd starting, service: " + serviceName);
  if (debugMode) {
    apm::logger::info("apmd: DEBUG mode enabled");
  }
```

### Update main function (line ~141):
```cpp
int main(int argc, char **argv) {
  std::string serviceName = apm::daemon::DEFAULT_SERVICE_NAME;
  bool debugMode = false;

  // Simple arg parser: apmd [--service name] [--debug]
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if ((arg == "--service" || arg == "--svc") && i + 1 < argc) {
      serviceName = argv[++i];
    } else if (arg == "--debug" || arg == "-d") {
      debugMode = true;
    } else if (arg == "--help" || arg == "-h") {
      std::cout << "apmd - APM daemon\n"
                << "Usage: apmd [--service <name>] [--debug]\n"
                << "  --debug, -d    Enable verbose debug logging\n";
      return 0;
    }
  }

  return apm::daemon::runDaemon(serviceName, debugMode);
}
```

## 2. src/core/binder_support.cpp

### Replace tryLoadFrom function:
```cpp
bool tryLoadFrom(const char *libName, std::string *errorMsg) {
  apm::logger::debug(std::string("tryLoadFrom: attempting dlopen for ") + libName);
  
  auto &sym = symbols();
  sym.handle = dlopen(libName, RTLD_NOW);
  if (!sym.handle) {
    const char *err = dlerror();
    apm::logger::debug(std::string("tryLoadFrom: dlopen failed: ") + (err ? err : "unknown"));
    
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
  apm::logger::debug(std::string("tryLoadFrom: AServiceManager_waitForService ") + 
                     (sym.waitForService ? "found" : "NOT FOUND"));
  
  apm::logger::debug("tryLoadFrom: loading ABinderProcess_setThreadPoolMaxThreadCount");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.setThreadPoolMax),
                   "ABinderProcess_setThreadPoolMaxThreadCount");
  apm::logger::debug(std::string("tryLoadFrom: ABinderProcess_setThreadPoolMaxThreadCount ") + 
                     (sym.setThreadPoolMax ? "found" : "NOT FOUND"));
  
  apm::logger::debug("tryLoadFrom: loading ABinderProcess_startThreadPool");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.startThreadPool),
                   "ABinderProcess_startThreadPool");
  apm::logger::debug(std::string("tryLoadFrom: ABinderProcess_startThreadPool ") + 
                     (sym.startThreadPool ? "found" : "NOT FOUND"));
  
  apm::logger::debug("tryLoadFrom: loading ABinderProcess_joinThreadPool");
  ok &= loadSymbol(sym.handle, reinterpret_cast<void **>(&sym.joinThreadPool),
                   "ABinderProcess_joinThreadPool");
  apm::logger::debug(std::string("tryLoadFrom: ABinderProcess_joinThreadPool ") + 
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
```

### Replace ensureLoaded function:
```cpp
bool ensureLoaded(std::string *errorMsg) {
  apm::logger::debug("ensureLoaded: checking API level availability");
  
  if (__builtin_available(android 34, *)) {
    apm::logger::debug("ensureLoaded: API 34+ available, proceeding with symbol loading");
    
    static std::once_flag flag;
    static bool loaded = false;
    std::call_once(flag, [&]() {
      apm::logger::debug("ensureLoaded: first call, attempting dynamic library load");
      
      const std::vector<const char *> candidates = {"libbinder_ndk.so",
                                                    "libbinder.so"};
      for (const char *lib : candidates) {
        apm::logger::debug(std::string("ensureLoaded: trying to load ") + lib);
        
        if (tryLoadFrom(lib, errorMsg)) {
          apm::logger::debug(std::string("ensureLoaded: successfully loaded ") + lib);
          loaded = true;
          break;
        } else {
          apm::logger::debug(std::string("ensureLoaded: failed to load ") + lib + 
                            (errorMsg && !errorMsg->empty() ? ": " + *errorMsg : ""));
        }
      }
      if (!loaded && errorMsg && errorMsg->empty()) {
        *errorMsg = "Unable to load Binder runtime (libbinder_ndk.so)";
      }
    });
    
    apm::logger::debug(std::string("ensureLoaded: returning ") + (loaded ? "true" : "false"));
    return loaded;
  }

  apm::logger::debug("ensureLoaded: API < 34, Binder unavailable");
  if (errorMsg) {
    *errorMsg = "Binder runtime requires Android API level 34 or newer";
  }
  return false;
}
```

### Replace addService function:
```cpp
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
    
    apm::logger::debug("addService: AServiceManager_addService returned status=" + 
                       std::to_string(status) + " (" + 
                       (status == STATUS_OK ? "OK" : 
                        status == STATUS_PERMISSION_DENIED ? "PERMISSION_DENIED" :
                        status == STATUS_BAD_VALUE ? "BAD_VALUE" :
                        status == STATUS_FAILED_TRANSACTION ? "FAILED_TRANSACTION" :
                        "OTHER") + ")");
  } catch (...) {
    apm::logger::error("addService: EXCEPTION during AServiceManager_addService");
    if (errorMsg)
      *errorMsg = "Exception while registering service " + instance;
    return false;
  }

  if (status != STATUS_OK) {
    apm::logger::debug("addService: registration FAILED with status " + std::to_string(status));
    if (errorMsg)
      *errorMsg = "AServiceManager_addService failed with status " +
                  std::to_string(status);
    return false;
  }
  
  apm::logger::debug("addService: registration SUCCESS for '" + instance + "'");
  return true;
}
```

### Replace getService function:
```cpp
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
                     std::string(wait ? "AServiceManager_waitForService" : "AServiceManager_getService"));

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
```

### Replace configureThreadPool function:
```cpp
bool configureThreadPool(int maxThreads, bool callerJoins,
                         std::string *errorMsg) {
  apm::logger::debug("configureThreadPool: ENTER maxThreads=" + 
                     std::to_string(maxThreads) + " callerJoins=" + 
                     (callerJoins ? "true" : "false"));
  
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
    apm::logger::error("configureThreadPool: EXCEPTION during thread pool setup");
    if (errorMsg)
      *errorMsg = "Exception while configuring Binder thread pool";
    return false;
  }
  
  apm::logger::debug("configureThreadPool: EXIT success");
  return true;
}
```

### Replace joinThreadPool function:
```cpp
void joinThreadPool() {
  apm::logger::debug("joinThreadPool: ENTER");
  
  std::string err;
  if (!ensureLoaded(&err)) {
    apm::logger::warn("joinThreadPool: Binder runtime unavailable: " + err);
    return;
  }

  if (symbols().joinThreadPool) {
    apm::logger::debug("joinThreadPool: calling ABinderProcess_joinThreadPool (BLOCKING)");
    try {
      symbols().joinThreadPool();
      apm::logger::debug("joinThreadPool: ABinderProcess_joinThreadPool returned");
    } catch (...) {
      apm::logger::error("joinThreadPool: EXCEPTION during thread pool join");
    }
  } else {
    apm::logger::warn("joinThreadPool: ABinderProcess_joinThreadPool symbol missing");
  }
  
  apm::logger::debug("joinThreadPool: EXIT");
}
```

## 3. src/apm/binder_client.cpp

Add these debug statements at the beginning of sendRequestBinder (after the existing code):

```cpp
bool sendRequestBinder(const Request &req, Response &resp,
                       const std::string &serviceName, std::string *errorMsg,
                       ProgressHandler progressHandler) {
  apm::logger::debug("=== sendRequestBinder: ENTER ===");
  apm::logger::debug("sendRequestBinder: service='" + serviceName + 
                     "' req.id='" + req.id + "' req.type=" + std::to_string(static_cast<int>(req.type)));
  
  // Hard-fail for devices below Android 14 (API 34)
  if (!isApiAtLeast34()) {
    apm::logger::debug("sendRequestBinder: API level < 34, aborting");
    if (errorMsg) {
      *errorMsg = withSuggestion("Android 14 (API 34) required for APM Binder",
                                 "Upgrade device OS to Android 14+.");
    }
    return false;
  }
  
  apm::logger::debug("sendRequestBinder: API level check passed");

  std::string runtimeErr;
  apm::logger::debug("sendRequestBinder: checking Binder runtime availability");
  
  if (!apm::binder::isBinderRuntimeAvailable(&runtimeErr)) {
    apm::logger::debug("sendRequestBinder: runtime check FAILED: " + runtimeErr);
    // ... rest of existing code
  }
  
  apm::logger::debug("sendRequestBinder: runtime available");

  // Add debug logs throughout the service lookup section
  ScopedStrongBinder service;
  {
    using namespace std::chrono_literals;
    apm::logger::debug("sendRequestBinder: attempting service lookup (fast path)");
    
    AIBinder *svc = apm::binder::getService(serviceName, false, &runtimeErr);
    
    if (!svc) {
      apm::logger::debug("sendRequestBinder: fast path FAILED, starting retry loop");
      
      const int maxAttempts = 5;
      for (int attempt = 0; attempt < maxAttempts && !svc; ++attempt) {
        apm::logger::debug("sendRequestBinder: retry " + std::to_string(attempt + 1) + 
                           "/" + std::to_string(maxAttempts));
        std::this_thread::sleep_for(500ms);
        svc = apm::binder::getService(serviceName, false, &runtimeErr);
        
        if (svc) {
          apm::logger::debug("sendRequestBinder: retry SUCCESS on attempt " + 
                             std::to_string(attempt + 1));
        }
      }
      
      if (!svc) {
        apm::logger::debug("sendRequestBinder: all retries exhausted, service not found");
      }
    } else {
      apm::logger::debug("sendRequestBinder: fast path SUCCESS");
    }
    
    service = ScopedStrongBinder(svc);
    apm::logger::debug("sendRequestBinder: service moved into ScopedStrongBinder, handle=" + 
                       std::to_string(reinterpret_cast<uintptr_t>(svc)));
  }
  
  if (!service.get()) {
    apm::logger::debug("sendRequestBinder: no service handle, ABORTING");
    // ... existing error handling
  }

  apm::logger::debug("sendRequestBinder: service.get()=" + 
                     std::to_string(reinterpret_cast<uintptr_t>(service.get())));

  apm::logger::debug("sendRequestBinder: creating ProgressCallbackReceiver");
  ProgressCallbackReceiver progressReceiver(progressHandler);
  apm::logger::debug("sendRequestBinder: progressReceiver.binder()=" + 
                     std::to_string(reinterpret_cast<uintptr_t>(progressReceiver.binder())));

  // Create a new parcel for the request
  apm::logger::debug("sendRequestBinder: preparing to call AIBinder_prepareTransaction");
  apm::logger::debug("sendRequestBinder:   service handle=" + 
                     std::to_string(reinterpret_cast<uintptr_t>(service.get())));
  
  AParcel *parcel = nullptr;
  binder_status_t parcelStatus =
      AIBinder_prepareTransaction(service.get(), &parcel);
  
  apm::logger::debug("sendRequestBinder: parcel=" + 
                     std::to_string(reinterpret_cast<uintptr_t>(parcel)));
                     
  if (parcelStatus != STATUS_OK || !parcel) {
    apm::logger::error("binder_client: prepareTransaction FAILED");
    apm::logger::error("  handle=" + std::to_string(reinterpret_cast<uintptr_t>(service.get())));
    apm::logger::error("  status=" + std::to_string(parcelStatus));
    apm::logger::error("  parcel=" + std::to_string(reinterpret_cast<uintptr_t>(parcel)));
    // ... existing error handling
  }
  
  apm::logger::debug("sendRequestBinder: prepareTransaction SUCCESS, writing request");

  // Write the request to the parcel
  binder_status_t writeStatus =
      writeParcelString(parcel, serializeRequest(req));
  apm::logger::debug("sendRequestBinder: writeParcelString returned " + std::to_string(writeStatus));
  
  if (writeStatus != STATUS_OK) {
    apm::logger::debug("sendRequestBinder: write request FAILED, status=" + std::to_string(writeStatus));
    // ... existing error handling
  }
  
  apm::logger::debug("sendRequestBinder: request written, attaching progress binder");

  binder_status_t cbStatus =
      AParcel_writeStrongBinder(parcel, progressReceiver.binder());
  apm::logger::debug("sendRequestBinder: writeStrongBinder returned " + std::to_string(cbStatus));
  
  if (cbStatus != STATUS_OK) {
    apm::logger::debug("sendRequestBinder: attach progress binder FAILED");
    // ... existing error handling
  }
  
  apm::logger::debug("sendRequestBinder: calling AIBinder_transact");
  apm::logger::debug("  service=" + std::to_string(reinterpret_cast<uintptr_t>(service.get())));
  apm::logger::debug("  code=" + std::to_string(apm::binder::TX_SEND_REQUEST));

  AParcel *reply = nullptr;
  binder_status_t status =
      AIBinder_transact(service.get(), apm::binder::TX_SEND_REQUEST, &parcel, &reply,
                        static_cast<binder_flags_t>(0));
                        
  apm::logger::debug("sendRequestBinder: AIBinder_transact returned status=" + 
                     std::to_string(status) + " reply=" + 
                     std::to_string(reinterpret_cast<uintptr_t>(reply)));
                     
  if (status != STATUS_OK || !reply) {
    apm::logger::error("sendRequestBinder: transact FAILED, status=" + std::to_string(status));
    // ... existing error handling
  }
  
  apm::logger::debug("sendRequestBinder: transact SUCCESS, reading reply");

  std::string rawResp;
  binder_status_t readStatus = readParcelString(reply, rawResp);
  apm::logger::debug("sendRequestBinder: readParcelString returned " + 
                     std::to_string(readStatus) + " length=" + std::to_string(rawResp.size()));
  
  AParcel_delete(reply);
  
  if (readStatus != STATUS_OK) {
    apm::logger::error("sendRequestBinder: read reply FAILED, status=" + std::to_string(readStatus));
    // ... existing error handling
  }
  
  apm::logger::debug("sendRequestBinder: parsing response");

  if (!parseResponse(rawResp, resp, errorMsg)) {
    apm::logger::error("binder_client: parseResponse FAILED");
    return false;
  }
  
  apm::logger::debug("sendRequestBinder: SUCCESS, resp.success=" + 
                     std::string(resp.success ? "true" : "false"));
  apm::logger::debug("=== sendRequestBinder: EXIT ===");

  return true;
}
```

## 4. src/apmd/binder_service.cpp

Add verbose logging to handleTransact in BinderServiceState class:

```cpp
binder_status_t handleTransact(transaction_code_t code, const AParcel *in,
                               AParcel *out) {
  apm::logger::debug("=== BinderService::handleTransact: ENTER ===");
  apm::logger::debug("handleTransact: code=" + std::to_string(code));
  apm::logger::debug("handleTransact: in=" + std::to_string(reinterpret_cast<uintptr_t>(in)));
  apm::logger::debug("handleTransact: out=" + std::to_string(reinterpret_cast<uintptr_t>(out)));
  
  if (__builtin_available(android 34, *)) {
    if (code != apm::binder::TX_SEND_REQUEST) {
      apm::logger::debug("handleTransact: UNKNOWN transaction code, expected " + 
                         std::to_string(apm::binder::TX_SEND_REQUEST));
      return STATUS_UNKNOWN_TRANSACTION;
    }
    
    apm::logger::debug("handleTransact: reading request string from parcel");

    std::string rawRequest;
    binder_status_t status = readParcelString(in, rawRequest);
    apm::logger::debug("handleTransact: readParcelString returned status=" + 
                       std::to_string(status) + " length=" + std::to_string(rawRequest.size()));
    
    if (status != STATUS_OK) {
      apm::logger::error("handleTransact: read request FAILED");
      return status;
    }

    apm::logger::debug("handleTransact: reading progress binder from parcel");
    
    AIBinder *cbRaw = nullptr;
    status = AParcel_readStrongBinder(in, &cbRaw);
    apm::logger::debug("handleTransact: readStrongBinder returned status=" + 
                       std::to_string(status) + " binder=" + 
                       std::to_string(reinterpret_cast<uintptr_t>(cbRaw)));
    
    if (status != STATUS_OK) {
      apm::logger::error("handleTransact: read progress binder FAILED");
      return status;
    }
    
    if (!cbRaw) {
      apm::logger::info("BinderService: no progress callback provided by client");
    }
    
    ScopedStrongBinder callback(cbRaw);

    apm::logger::debug("handleTransact: parsing request");
    
    apm::ipc::Request req;
    apm::ipc::Response resp;
    std::string parseErr;
    
    if (!parseRequest(rawRequest, req, &parseErr)) {
      apm::logger::error("handleTransact: parseRequest FAILED: " + parseErr);
      resp.success = false;
      resp.message =
          parseErr.empty() ? "Bad request" : ("Bad request: " + parseErr);
      resp.status = apm::ipc::ResponseStatus::Error;
      std::string payload = serializeResponse(resp);
      return writeParcelString(out, payload);
    }

    apm::logger::info("BinderService: received request type=" + 
                      std::to_string(static_cast<int>(req.type)) + " id=" + req.id);
    apm::logger::debug("handleTransact: dispatching request to handler");
    
    auto progressCb = [&](const apm::ipc::Response &progress) {
      apm::logger::debug("handleTransact: sending progress update");
      sendProgressToClient(callback.get(), progress);
    };

    m_owner.dispatchRequest(req, resp, progressCb);
    
    apm::logger::debug("handleTransact: request dispatched, serializing response");
    
    std::string payload = serializeResponse(resp);
    apm::logger::debug("handleTransact: response serialized, length=" + 
                       std::to_string(payload.size()));
    
    binder_status_t writeStatus = writeParcelString(out, payload);
    apm::logger::debug("handleTransact: writeParcelString returned " + 
                       std::to_string(writeStatus));
    
    apm::logger::debug("=== BinderService::handleTransact: EXIT ===");
    return writeStatus;
  }
  
  return STATUS_FAILED_TRANSACTION;
}
```

Add debug logging to BinderService::start() method at key points where INFO logs exist.

## Testing

After implementing these changes, run:
```bash
# Build and deploy
./build_android.sh

# On device with adb shell + su:
apmd --debug

# In another terminal:
apm ping
```

Check `/data/apm/logs/apmd.log` for extremely detailed debug output showing every Binder operation.
