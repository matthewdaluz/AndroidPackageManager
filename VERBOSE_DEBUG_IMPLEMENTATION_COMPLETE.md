# Verbose Debug Logging Implementation - COMPLETE

## Summary

All verbose debugging has been successfully implemented throughout the Binder system. The implementation adds **dmesg-style** comprehensive logging at every critical point in the Binder transaction flow.

## Changes Made

### 1. src/apmd/apmd.cpp
- ✅ Added `--debug` / `-d` flag support
- ✅ Updated `runDaemon()` signature to accept `debugMode` parameter
- ✅ Sets log level to `Debug` when `--debug` is passed
- ✅ Updated help text

**Usage**: `apmd --debug` or `apmd -d`

### 2. src/core/binder_support.cpp
Comprehensive debug logging added to all Binder runtime functions:

#### tryLoadFrom()
- dlopen attempt and result with handle pointer
- Each symbol load (6 symbols) with success/failure status
- Final cleanup or success confirmation

#### ensureLoaded()
- API level check (34+)
- Library candidate attempts (libbinder_ndk.so, libbinder.so)
- Load success/failure with error details
- Return value logging

#### getService()
- Entry with instance name and wait flag
- Function pointer validation
- Service lookup call (getService vs waitForService)
- Raw handle pointer
- AIBinder_incStrong call with exception handling
- Return handle pointer

#### addService()
- Entry with instance name
- Binder handle pointer
- AServiceManager_addService call with status code mapping:
  - STATUS_OK
  - STATUS_PERMISSION_DENIED
  - STATUS_BAD_VALUE
  - STATUS_FAILED_TRANSACTION
  - OTHER
- Success/failure status

#### configureThreadPool()
- Entry with maxThreads and callerJoins parameters
- setThreadPoolMax() call result
- startThreadPool() call result
- Exit status

#### joinThreadPool()
- Entry notification
- Blocking call warning
- ABinderProcess_joinThreadPool() invocation
- Return notification (if ever returns)

### 3. src/apm/binder_client.cpp
Comprehensive debug logging added to `sendRequestBinder()`:

- **Entry**: Service name, request ID, request type
- **API check**: API level 34+ verification
- **Runtime check**: Binder availability verification
- **Service lookup**:
  - Fast path attempt
  - Retry loop (5 attempts with iteration tracking)
  - Success on specific retry attempt
  - Final handle pointer
  - ScopedStrongBinder assignment
- **Progress callback**: ProgressReceiver creation and binder pointer
- **Prepare transaction**:
  - Service handle pointer
  - AIBinder_prepareTransaction call
  - Parcel pointer
  - Detailed error logging if failure
- **Write operations**:
  - Request serialization and write status
  - Progress binder attachment status
- **Transact**:
  - Service handle and transaction code
  - AIBinder_transact call with status and reply pointer
  - Detailed error on failure
- **Reply**:
  - Read status and data length
  - Parse result
  - Final success status
- **Exit**: Summary of operation result

### 4. src/apmd/binder_service.cpp
Comprehensive debug logging added to `handleTransact()`:

- **Entry**: Transaction code, input parcel pointer, output parcel pointer
- **Code validation**: Expected vs actual transaction code
- **Request read**: Status and data length
- **Progress binder read**: Status and binder pointer (null check logging)
- **Request parsing**: Success/failure with error details
- **Dispatch**: Request type and ID, handler invocation
- **Progress updates**: Individual progress callback invocations
- **Response**: Serialization length, write status
- **Exit**: Operation complete

## Debug Output Format

All debug messages follow this pattern:
```
[DEBUG] <function>: <operation> <details>
```

Examples:
```
[DEBUG] getService: ENTER for instance 'apm.apmd' wait=false
[DEBUG] getService: calling AServiceManager_getService
[DEBUG] getService: obtained raw handle=0x7b400070d0
[DEBUG] getService: calling AIBinder_incStrong
[DEBUG] getService: AIBinder_incStrong SUCCESS
[DEBUG] getService: returning handle=0x7b400070d0
```

```
[DEBUG] sendRequestBinder: preparing to call AIBinder_prepareTransaction
[DEBUG] sendRequestBinder:   service handle=0x7b400070d0
[DEBUG] sendRequestBinder: parcel=0x7b40007150
```

## Testing Instructions

### 1. Build with Debug Support
```bash
cd /home/matthew/Documents/Projects/AndroidPackageManager-GitHub/AndroidPackageManager
./build_android.sh
```

### 2. Deploy to Device
```bash
adb push build-android/arm64-v8a/apm /data/local/tmp/
adb push build-android/arm64-v8a/apmd /data/local/tmp/

adb shell su -c "mount -o remount,rw /"
adb shell su -c "cp /data/local/tmp/apm /system/bin/"
adb shell su -c "cp /data/local/tmp/apmd /system/bin/"
adb shell su -c "chmod 755 /system/bin/apm /system/bin/apmd"
```

### 3. Start Daemon with Debug Mode
```bash
# Stop existing daemon if running
adb shell su -c "killall apmd"

# Start with debug logging
adb shell su -c "apmd --debug &"
```

### 4. Trigger a Transaction
```bash
adb shell su -c "apm ping"
```

### 5. Examine Debug Output
```bash
adb shell su -c "cat /data/apm/logs/apmd.log | grep -E '(DEBUG|ERROR)'"
```

Or pull the full log:
```bash
adb pull /data/apm/logs/apmd.log ./apmd_debug.log
```

## Expected Debug Flow for Successful Transaction

When running `apm ping`, you should see (abbreviated):

```
[DEBUG] ensureLoaded: checking API level availability
[DEBUG] ensureLoaded: API 34+ available, proceeding with symbol loading
[DEBUG] ensureLoaded: first call, attempting dynamic library load
[DEBUG] tryLoadFrom: attempting dlopen for libbinder_ndk.so
[DEBUG] tryLoadFrom: dlopen succeeded, handle=0x...
[DEBUG] tryLoadFrom: loading AServiceManager_addService
[DEBUG] tryLoadFrom: AServiceManager_addService found
[DEBUG] tryLoadFrom: loading AServiceManager_getService
[DEBUG] tryLoadFrom: AServiceManager_getService found
[DEBUG] tryLoadFrom: all symbols loaded successfully
[DEBUG] ensureLoaded: successfully loaded libbinder_ndk.so

[DEBUG] addService: ENTER for instance 'apm.apmd'
[DEBUG] addService: binder handle=0x...
[DEBUG] addService: calling AServiceManager_addService
[DEBUG] addService: AServiceManager_addService returned status=0 (OK)
[DEBUG] addService: registration SUCCESS for 'apm.apmd'

[DEBUG] configureThreadPool: ENTER maxThreads=4 callerJoins=true
[DEBUG] configureThreadPool: calling setThreadPoolMax(4)
[DEBUG] configureThreadPool: setThreadPoolMax SUCCESS
[DEBUG] configureThreadPool: calling startThreadPool
[DEBUG] configureThreadPool: startThreadPool SUCCESS

[DEBUG] joinThreadPool: ENTER
[DEBUG] joinThreadPool: calling ABinderProcess_joinThreadPool (BLOCKING)

--- Client side (apm) ---

[DEBUG] sendRequestBinder: === ENTER ===
[DEBUG] sendRequestBinder: service='apm.apmd' req.id='ping' req.type=9
[DEBUG] sendRequestBinder: API level check passed
[DEBUG] sendRequestBinder: runtime available
[DEBUG] sendRequestBinder: attempting service lookup (fast path)

[DEBUG] getService: ENTER for instance 'apm.apmd' wait=false
[DEBUG] getService: calling AServiceManager_getService
[DEBUG] getService: obtained raw handle=0x...
[DEBUG] getService: calling AIBinder_incStrong
[DEBUG] getService: AIBinder_incStrong SUCCESS
[DEBUG] getService: returning handle=0x...

[DEBUG] sendRequestBinder: fast path SUCCESS
[DEBUG] sendRequestBinder: service moved into ScopedStrongBinder, handle=0x...
[DEBUG] sendRequestBinder: service.get()=0x...
[DEBUG] sendRequestBinder: creating ProgressCallbackReceiver
[DEBUG] sendRequestBinder: progressReceiver.binder()=0x...
[DEBUG] sendRequestBinder: preparing to call AIBinder_prepareTransaction
[DEBUG] sendRequestBinder:   service handle=0x...
[DEBUG] sendRequestBinder: parcel=0x...
[DEBUG] sendRequestBinder: prepareTransaction SUCCESS, writing request
[DEBUG] sendRequestBinder: writeParcelString returned 0
[DEBUG] sendRequestBinder: request written, attaching progress binder
[DEBUG] sendRequestBinder: writeStrongBinder returned 0
[DEBUG] sendRequestBinder: calling AIBinder_transact
[DEBUG] sendRequestBinder:   service=0x...
[DEBUG] sendRequestBinder:   code=1
[DEBUG] sendRequestBinder: AIBinder_transact returned status=0 reply=0x...
[DEBUG] sendRequestBinder: transact SUCCESS, reading reply
[DEBUG] sendRequestBinder: readParcelString returned 0 length=...
[DEBUG] sendRequestBinder: parsing response
[DEBUG] sendRequestBinder: SUCCESS, resp.success=true
[DEBUG] sendRequestBinder: === EXIT ===

--- Server side (apmd) ---

[DEBUG] handleTransact: === ENTER ===
[DEBUG] handleTransact: code=1
[DEBUG] handleTransact: in=0x...
[DEBUG] handleTransact: out=0x...
[DEBUG] handleTransact: reading request string from parcel
[DEBUG] handleTransact: readParcelString returned status=0 length=...
[DEBUG] handleTransact: reading progress binder from parcel
[DEBUG] handleTransact: readStrongBinder returned status=0 binder=0x...
[DEBUG] handleTransact: parsing request
[INFO] BinderService: received request type=9 id=ping
[DEBUG] handleTransact: dispatching request to handler
[DEBUG] handleTransact: request dispatched, serializing response
[DEBUG] handleTransact: response serialized, length=...
[DEBUG] handleTransact: writeParcelString returned 0
[DEBUG] handleTransact: === EXIT ===
```

## Diagnosing the -38 Error

If the `-38` error persists, the debug logs will show **exactly** where it occurs:

1. **If error at prepareTransaction**:
   ```
   [DEBUG] sendRequestBinder: service.get()=0x... (valid pointer)
   [DEBUG] sendRequestBinder:   service handle=0x... (same pointer)
   [ERROR] binder_client: prepareTransaction FAILED
   [ERROR]   handle=0x...
   [ERROR]   status=-38
   [ERROR]   parcel=0x0 (or NULL)
   ```
   → Indicates service handle is stale/invalid despite incStrong

2. **If error at getService**:
   ```
   [DEBUG] getService: obtained raw handle=0x...
   [ERROR] getService: EXCEPTION in AIBinder_incStrong
   ```
   → Indicates raw handle from servicemanager is corrupt

3. **If error at transact**:
   ```
   [DEBUG] sendRequestBinder: prepareTransaction SUCCESS, writing request
   [DEBUG] sendRequestBinder: calling AIBinder_transact
   [ERROR] sendRequestBinder: transact FAILED, status=-38
   ```
   → Indicates parcel preparation succeeded but transaction failed

## Next Steps

1. **Build and deploy** with verbose debugging enabled
2. **Run `apmd --debug`** to start daemon in debug mode
3. **Execute `apm ping`** to trigger a transaction
4. **Examine logs** at `/data/apm/logs/apmd.log`
5. **Share the logs** showing the full debug flow around the error

The verbose logging will capture:
- Every function entry/exit
- All pointer addresses
- All status codes with symbolic names
- Retry attempts and timing
- Exception occurrences
- Full transaction flow from both client and server perspectives

This level of detail will definitively identify where the -38 error originates and what state the binder handles are in at that point.
