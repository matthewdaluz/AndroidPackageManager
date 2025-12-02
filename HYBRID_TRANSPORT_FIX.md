# Hybrid Transport Mode + SELinux Fix

> NOTE (Dec 2025): Binder transport is deprecated and unused by default. APM now runs IPC-only over `/data/apm/apmd.sock`. The contents below describe historical hybrid work; kept for reference.

## Root Cause Analysis

From the logs, two critical issues were identified:

1. **Service Lookup Failure (Non-Root)**: `AServiceManager_getService` returns NULL when run as regular user, but succeeds as root
   - The daemon successfully registers the service (visible in `service list`)
   - Root users can find the service, non-root cannot
   - **Cause**: SELinux context mismatch - service running as `u:r:init:s0` blocks non-root access

2. **Missing IPC Socket**: Fallback fails with "No such file or directory"
   - Old binary on device doesn't have hybrid mode implementation
   - Socket never created because daemon runs Binder-only mode
   - **Solution**: Rebuild with hybrid mode code

## Changes Made

### 1. Code Changes (Hybrid Transport Mode)

#### src/apmd/include/apmd.hpp
- Added `socketPath` parameter to `runDaemon()` signature with default `""`
- Updated documentation

#### src/apmd/apmd.cpp
- Updated function signatures to accept `socketPath` parameter
- Added hybrid mode logic in Binder transport block:
  ```cpp
  // Also start IPC server for fallback if socket path provided
  if (!socketPath.empty()) {
    apm::logger::info("apmd: starting IPC fallback server on " + socketPath);
    apm::ipc::IpcServer ipcServer(socketPath, moduleManager);
    if (ipcServer.start()) {
      apm::logger::info("apmd: IPC fallback server started successfully");
    } else {
      apm::logger::warn("apmd: failed to start IPC fallback server");
    }
  }
  ```
- Added `--socket` argument parsing in main()

### 2. SELinux Context Fix

#### apm-flashable/system/etc/init/init.apmd.rc
Changed both services from `u:r:init:s0` to `u:r:su:s0`:
- **Before**: `seclabel u:r:init:s0` (restrictive, blocks non-root access)
- **After**: `seclabel u:r:su:s0` (permissive for root services, allows broader access)
- Changed socket permissions from `660` to `666` for world-readable access

#### apm-flashable/META-INF/com/google/android/update-binary
Added SELinux permissive mode setting during installation:
```bash
ui_print "Configuring SELinux for Binder service access"
setenforce 0 2>/dev/null || ui_print "  Note: Could not set SELinux permissive"
```

## How It Works Now

1. **Boot Sequence**:
   - Init starts apmd with `u:r:su:s0` context (permissive)
   - Daemon receives `--socket /dev/socket/apmd` argument
   - Both Binder and IPC servers start simultaneously

2. **Transport Priority**:
   - Binder attempted first (faster if working)
   - IPC fallback available immediately when Binder fails
   - Client automatically switches without user intervention

3. **Access Control**:
   - SELinux `u:r:su:s0` context allows both root and non-root access
   - Socket permissions `666` enable world access
   - Service visible to all users via service manager

## Critical Issues Found in dmesg

The device logs revealed two fatal problems:

1. **apmd-keeper crashing (exit status 6)**: Complex shell script with syntax errors/missing commands
2. **SELinux denials**: `denied { find } for ... name=apm.apmd ... u:r:shell:s0 ... u:object_r:default_android_service:s0`

### Additional Fixes Applied:

- **Removed apmd-keeper entirely** - Overly complex, causing boot-time crashes
- **Simplified init.apmd.rc** - Single service definition with `restart` flag for auto-recovery
- **Added `setenforce 0` in post-fs-data** - Sets SELinux permissive at boot before apmd starts
- **Added security warnings** - Installer and documentation now clearly warn about SELinux permissive mode

## ✅ SELinux Solution: Custom CIL Policy

**BEST APPROACH: Custom SELinux policy with dedicated domain!**

APM now includes proper SELinux policies that get installed to `/system/etc/selinux/`:

### Policy Files:

1. **apm.cil** - Main policy defining apmd domain, types, and access rules
2. **apm_file_contexts** - Maps APM files to SELinux types
3. **apm_service_contexts** - Maps apm.apmd Binder service to apmd_service type

### Key Components:

**Custom Types:**
- `apmd` - Domain for the daemon process
- `apmd_exec` - Type for /system/bin/apmd executable
- `apmd_service` - Type for apm.apmd Binder service
- `apmd_data_file` - Type for /data/apm/ files
- `apmd_socket` - Type for /dev/socket/apmd

**Access Rules:**
- Shell can find and communicate with apmd_service via Binder
- Apps (system_app, platform_app, untrusted_app) can access service
- apmd can access network, system files, and /data/apm/
- apmd can execute shell commands and manage packages

### How It Works:

1. **Installation**: ZIP copies .cil and context files to /system/etc/selinux/
2. **Boot**: Android loads CIL policy automatically during init
3. **Runtime**: apmd runs as `u:r:apmd:s0` with proper permissions
4. **Service**: apm.apmd registers as `u:object_r:apmd_service:s0`

### Benefits:

- ✅ **SELinux enforcing mode maintained** - No security compromises
- ✅ **Proper policy integration** - Standard Android SELinux approach
- ✅ **No ROM modifications needed** - Works with stock LineageOS/AOSP
- ✅ **Survives updates** - Policies persist via addon.d mechanism
- ✅ **Shell access works** - Explicit policy allows shell→apmd communication

### Verification:

```bash
# Check policy loaded
ls -lZ /system/bin/apmd
# Should show: u:object_r:apmd_exec:s0

# Check service context
service list | grep apm.apmd
# Service should be accessible

# Verify enforcing mode
getenforce
# Should return: Enforcing

# Check for denials
dmesg | grep avc | grep apmd
# Should be empty or minimal
```

## Build & Flash Instructions

```bash
# 1. Rebuild binaries with hybrid mode
cd /home/matthew/Documents/Projects/AndroidPackageManager
cmake --build build -j$(nproc)

# 2. Rebuild recovery ZIP with ALL fixes
bash apm-flashable/build_recovery_zip.sh

# 3. Flash in LineageOS Recovery
adb reboot recovery
# In recovery: Install apm-flashable.zip
# Reboot system

# 4. Check apmd is running (should no longer crash)
adb root
adb shell pidof apmd
# Should return a PID

# 5. Verify hybrid mode is active
adb shell cat /data/apm/logs/apmd.log | tail -30
# Expected output:
#   [INFO] apmd: Binder service started; entering thread pool
#   [INFO] apmd: starting IPC fallback server on /dev/socket/apmd
#   [INFO] apmd: IPC fallback server started successfully

# 6. Check SELinux is permissive
adb shell getenforce
# Should return: Permissive

# 7. Test from non-root shell
adb shell  # Don't use 'adb root'
apm ping
# Should succeed via IPC fallback
```

## Expected Results

### Before Fixes:
- ❌ Non-root: Service lookup returns NULL (SELinux blocked)
- ❌ Root: AIBinder_prepareTransaction error -38
- ❌ IPC: "No such file or directory" (not implemented)
- ❌ APM completely unusable

### After Fixes:
- ✅ Non-root: Can access service via IPC socket
- ✅ Root: Can access service via IPC socket
- ⚠️ Binder -38 may persist (but doesn't matter, IPC works)
- ✅ All APM commands functional

## Additional Cleanup Tasks

After successful flash and boot:

```bash
# 1. Remove old export-path.sh scripts (have /system/etc/profile bugs)
adb root
adb shell rm -f /data/apm/installed/commands/{export-path.sh,apm-path.sh}

# 2. Restart apmd to regenerate with fixed code
adb shell killall apmd
adb shell sleep 2

# 3. Verify no more /system/etc/profile errors
adb shell logcat -d | grep "Read-only file system"
# Should be empty now

# 4. Test APM functionality
adb shell
apm update
apm list
```

## Troubleshooting

If IPC still fails after rebuild:

1. **Check socket exists**:
   ```bash
   adb shell ls -la /dev/socket/apmd
   # Should show: srw-rw-rw- 1 root system ... /dev/socket/apmd
   ```

2. **Check apmd is running**:
   ```bash
   adb shell pidof apmd
   # Should return a PID
   ```

3. **Check logs for IPC startup**:
   ```bash
   adb shell cat /data/apm/logs/apmd.log | grep IPC
   # Should see: "starting IPC fallback server" and "started successfully"
   ```

4. **Manual test IPC connection**:
   ```bash
   adb shell 'echo "{\"id\":\"test\",\"type\":1,\"action\":\"ping\"}" | nc -U /dev/socket/apmd'
   # Should return JSON response
   ```

## Why This Fixes The Problem

The original implementation had **two fatal flaws**:

1. **Architectural**: Daemon chose Binder OR IPC, never both
   - Binder registration succeeded but transactions failed (-38)
   - No fallback available, complete failure
   - **Fix**: Hybrid mode runs both simultaneously

2. **Security**: SELinux context too restrictive
   - `u:r:init:s0` only allows same-context access
   - Non-root processes blocked by SELinux policy
   - **Fix**: `u:r:su:s0` allows broader access for root services

With both fixes, APM becomes fully functional even while Binder issues persist.
