# AMSD Implementation Plan

## Overview

AMSD (AMS Daemon) will become a standalone daemon that manages the APM Module System independently from APMD. AMSD starts during Android `init`, owns the `/ams/` storage hierarchy, maintains OverlayFS layers per partition, and exposes its own authenticated IPC endpoint at `/dev/socket/amsd`. APMD will focus on package management and refuse to start if legacy module paths exist. Users must factory-reset APM before installing the AMSD-enabled release.

---

## Step-by-Step Plan

### 1. Relocate AMS Paths to `/ams/`
- Update `src/core/config.cpp` getters (`getModulesDir`, `getModuleLogsDir`, `getModuleRuntime*Dir`) to return `/ams/...` paths (or emulator equivalents).
- Update `src/core/include/config.hpp` constants (`MODULES_DIR`, `MODULE_LOGS_DIR`, `MODULE_RUNTIME_*_DIR`).
- Replace hardcoded `/data/apm/modules` references with `apm::config` getters throughout the codebase.

### 2. AMSD Daemon (`src/amsd/amsd.cpp`)
- Wait for `/data` readiness (reuse `waitForDataReady()` logic).
- Read `/ams/.amsd_safe_mode_threshold` (default 3) and `/ams/.amsd_boot_counter`.
- Enter safe mode if `counter >= threshold` (create `/ams/.amsd_safe_mode`, skip module loading).
- Instantiate `apm::ams::ModuleManager` with new paths.
- Apply overlays for partitions currently mounted.
- Increment boot counter before attempting overlay work.
- Start background partition monitor thread (see Step 3).
- Run module lifecycle scripts (`post-fs-data.sh`, `service.sh`).
- Start AMSD IPC server on `/dev/socket/amsd` (Step 5).
- On success: reset boot counter to 0, clear safe-mode flag, set `setprop amsd.ready 1`.

### 3. Partition Monitoring & Safe Mode
- In `ModuleManager`:
  - Add `isPartitionMounted(mountPoint)` using `/proc/mounts`.
  - Implement `applyOverlayForPartition(const OverlayTarget&)`. Failures should only disable that partition’s overlay, log error, and continue.
  - Refactor `applyEnabledModules()` to iterate over overlay targets, calling the partition-specific helper.
  - Add synchronized background monitor: poll every 5s up to 30 iterations (150s). When a new partition appears, apply overlays immediately. Exit thread after all targets verified or timeout reached.
  - Add safe mode helpers: `incrementBootCounter(path)`, `getBootCounter(path)`, `resetBootCounter(path)`, `getBootThreshold(path)`, `enterSafeMode(path)`, `isSafeModeActive(path)`, `clearSafeMode(path)`.
  - Guard ModuleManager operations with mutexes so IPC requests and background thread do not race.
  - All logs go to `/ams/logs/amsd.log`.

### 4. Shared Security Context
- Extend `src/core/security.cpp/.hpp` with helpers to read/write session token blobs from `/data/apm/.security/session.bin` using `flock()`.
- Modify APMD’s `SecurityManager` to persist session tokens to the shared file whenever sessions are issued/validated.
- Implement `src/amsd/security_manager.{hpp,cpp}` that validates tokens using the shared file (read-only, no token issuance).

### 5. AMSD IPC Stack
- Create `src/amsd/protocol.{hpp,cpp}` (reuse definitions from APMD or move to shared location).
- Create `src/amsd/request_dispatcher.{hpp,cpp}` handling only ModuleList/Install/Enable/Disable/Remove operations and invoking ModuleManager.
- Create `src/amsd/ipc_server.{hpp,cpp}`: bind `/dev/socket/amsd`, require valid session token (0666 socket but authenticated), translate incoming requests via dispatcher, send responses.
- Handle graceful shutdown and socket cleanup on SIGTERM/SIGINT.

### 6. Init Integration (`init.amsd.rc`)
- Trigger: `on init`.
- Provision directories:
  - `/ams`, `/ams/modules`, `/ams/logs` (0755 root:root).
  - `/ams/.runtime`, `/ams/.runtime/{upper,work,base}` (0700 root:root).
- Service definition:
  - `service amsd /system/bin/amsd`
  - `class core`
  - `user root`, `group root system`
  - `seclabel u:r:init:s0`
  - `socket amsd stream 0666 root root`
  - `restart_period 90`
- On success, AMSD sets property `amsd.ready=1` for APMD to consume.

### 7. APM Client Dual-Routing
- In `src/apm/apm.cpp`, detect module commands (`module-*`, `module-safe-mode-*`) and route them to `/dev/socket/amsd`; other commands stay on `/data/apm/apmd.sock`.
- Update `src/apm/ipc_client.cpp` and `src/apm/transport.cpp` to accept a socket path parameter.
- Provide clear errors when AMSD socket connection fails: suggest checking `getprop amsd.ready` or AMSD logs.

### 8. Legacy Path Detection in APMD
- In `src/apmd/apmd.cpp`, before daemon init, detect non-empty `/data/apm/modules/` (e.g., look for subdirectories with `module-info.json`).
- If found, log fatal message instructing user to factory reset and exit with error.
- Optionally warn if `/ams/modules/` missing after factory reset (fresh install guidance).

### 9. Build System Updates
- `CMakeLists.txt`: add `amsd` executable with AMS, AMSD, and required core sources (fs/logger/config/security). Link pthread/zlib as needed.
- `Android.bp`: add `cc_binary { name: "amsd", ... }` plus `prebuilt_etc` entry for `init.amsd.rc`.
- Ensure `build_android.sh` builds `amsd` by default.

### 10. Deployment & Packaging
- `init.apmd.rc`: remove `/data/apm/modules` provisioning; update service trigger to `on property:amsd.ready=1`.
- `apm-flashable/`:
  - Copy `amsd` binary into `system/bin/`.
  - Include `init.amsd.rc` under `system/etc/init/`.
  - Update `addon.d` templates to preserve `amsd` binary and new init script.
- `apm-magisk/`:
  - Add `amsd` binary to `files/`.
  - Update `post-fs-data.sh` to create `/ams/...` directories.
  - Update `service.sh` to start AMSD first, poll readiness (socket/property), then start APMD.
- Documentation (README, release notes) must call out factory-reset requirement and new architecture.

---

## Additional Considerations

1. **Session File Locking**
   - Use `flock()` with `LOCK_EX` for writes and `LOCK_SH` for reads on `/data/apm/.security/session.bin`. Verify behavior on target filesystems; add fallback to exclusive locking if shared locks fail.

2. **Boot Counter Semantics**
   - Increment counter before overlay attempts. If AMSD crash loops, safe mode triggers once counter >= threshold (default 3). Counter resets to 0 only after successful full init.

3. **Partition Monitor Coordination**
   - Guard ModuleManager operations with mutex. Background overlay reapply may briefly block IPC module commands; document potential short delay during first 2.5 minutes post-boot.

4. **Safe Mode Recovery Command**
   - Add `apm module-safe-mode-exit` (future work) to clear `/ams/.amsd_safe_mode` and reset counter when user confirms issues resolved.

5. **Factory Reset Enforcement**
   - Document required commands (e.g., `rm -rf /data/apm`) before flashing AMSD build. APMD refuses to start until legacy modules are removed.

6. **Future SELinux Documentation**
   - Plan to document additional `sepolicy` rules if stricter ROMs block AMSD operations (e.g., mount permissions). This doc will be prepared later per request.
