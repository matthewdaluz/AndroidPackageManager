# Hybrid Transport Mode Implementation

## Changes Made

### 1. Header Update (src/apmd/include/apmd.hpp)
- Added `socketPath` parameter to `runDaemon()` signature
- Default value: `""` (empty string for backward compatibility)
- Updated documentation to reflect new parameter

### 2. Implementation Update (src/apmd/apmd.cpp)

#### runDaemon() Function
- Updated both function signatures (anonymous namespace and apm::daemon namespace) to accept `socketPath` parameter
- Added hybrid mode logic in Binder transport block:
  - When `socketPath` is not empty, IPC fallback server is started alongside Binder
  - IPC server runs independently, providing fallback when Binder transactions fail
  - Logs appropriate info/warning messages for IPC server start status

#### main() Function
- Added `socketPath` variable declaration
- Added `--socket` argument parsing: `else if (arg == "--socket" && i + 1 < argc)`
- Pass `socketPath` to `runDaemon()` call
- Updated comment to reflect new argument

## How It Works

1. **Boot Sequence**:
   - `init.apmd.rc` starts apmd with: `--socket /dev/socket/apmd`
   - `main()` parses the argument and stores path in `socketPath` variable
   - `runDaemon()` receives the socket path

2. **Transport Initialization**:
   - Daemon detects Binder mode (system install)
   - Starts Binder service normally
   - **NEW**: Also starts IPC server on `/dev/socket/apmd` for fallback
   - Both transports run simultaneously

3. **Client Behavior**:
   - apm CLI attempts Binder connection first
   - If Binder `AIBinder_prepareTransaction` fails (-38), CLI falls back to IPC socket
   - Socket connection should succeed, allowing APM commands to work

## Testing After Rebuild

```bash
# 1. Rebuild binaries
cd /home/matthew/Documents/Projects/AndroidPackageManager
cmake --build build -j$(nproc)

# 2. Rebuild recovery ZIP
bash apm-flashable/build_recovery_zip.sh

# 3. Flash in recovery
adb reboot recovery
# Install apm-flashable.zip
# Reboot

# 4. Verify both transports
adb shell logcat -d | grep apmd
# Should see: "apmd: Binder service started"
# Should see: "apmd: IPC fallback server started successfully"

# 5. Test APM commands
adb shell apm ping
adb shell apm list
```

## Expected Results

- **Before**: Both Binder and IPC failed, APM unusable
- **After**: 
  - Binder may still fail with -38, but that's okay
  - IPC fallback provides working communication
  - All APM commands functional via socket transport
  - Can investigate Binder -38 issue separately without blocking usage

## Next Steps (After Verification)

1. Clean old export-path.sh on device:
   ```bash
   adb shell rm -f /data/apm/installed/commands/{export-path.sh,apm-path.sh}
   adb shell killall apmd
   ```

2. Verify regeneration works without /system/etc/profile errors

3. Investigate Binder AIBinder_prepareTransaction -38 error (low priority now)
