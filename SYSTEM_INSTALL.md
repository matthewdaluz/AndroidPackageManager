# APM System-Wide Installation Guide

## Overview

APM can now be installed system-wide on LineageOS 22.0+ (Android 14+) without requiring Magisk or root access at runtime. The installation uses a LineageOS Recovery flashable ZIP following the MindTheGApps pattern.

## Quick Start

### Building the Flashable ZIP

```bash
cd apm-flashable
./build_recovery_zip.sh
```

This will:
1. Build ARM64 APM binaries (if not already built)
2. Copy binaries to the flashable structure
3. Create `apm-systemwide-arm64-YYYYMMDD.zip` in the project root

### Installation

1. **Transfer ZIP to device**:
   ```bash
   adb push apm-systemwide-arm64-*.zip /sdcard/
   ```

2. **Boot into LineageOS Recovery**:
   - Power off device
   - Boot to recovery (usually Power + Volume Down)

3. **Flash the ZIP**:
   - Select "Apply update" → "Apply from ADB" (or "Choose from /sdcard/")
   - Flash: `adb sideload apm-systemwide-arm64-*.zip`

4. **Reboot system**

5. **Verify installation**:
   ```bash
   adb shell
   apm ping
   # Should respond: "Daemon is reachable"
   ```

### Uninstallation

```bash
cd apm-flashable-uninstall
./build_uninstall_zip.sh
# Flash the generated apm-systemwide-uninstall-*.zip in recovery
```

## What Gets Installed

- `/system/bin/apm` - CLI client (0755 root:root)
- `/system/bin/apmd` - Daemon (0755 root:root)
- `/system/etc/init/init.apmd.rc` - Init service definition (0644 root:root)
- `/system/addon.d/30-apm.sh` - OTA survival script (0755 root:root)
- `/system/xbin/{xz,unxz,...}` - Optional compression tools (0755 root:root)

All files use SELinux context: `u:object_r:system_file:s0`

## How It Works

### Installation Process (update-binary)

1. Extract ZIP contents to `/tmp/apm-install/`
2. Validate device architecture (arm64) and Android version (SDK ≥34)
3. Mount `/mnt/system` read-write (handles dynamic partitions, A/B slots)
4. Generate `/system/addon.d/30-apm.sh` from templates + file list
5. Set permissions and SELinux contexts on all files
6. Copy `system/*` to `/mnt/system/`
7. Unmount and cleanup

### Boot Process (init.apmd.rc)

1. **post-fs-data**: Create `/data/apm/` directory structure
2. **late_start**: Launch `apmd-keeper` service
3. **apmd-keeper**: Monitor daemon, auto-restart if crashed
4. **apmd service**: Register Binder service `apm.apmd` on socket `/dev/socket/apmd`

### OTA Survival (addon.d)

LineageOS OTA updates preserve APM through `/system/addon.d/30-apm.sh`:
- **Backup**: Save files before update
- **Restore**: Copy files back after update
- **Post-restore**: Reapply permissions and ownership

## Requirements

- **Device**: ARM64 Android device with unlocked bootloader
- **OS**: LineageOS 22.0+ (Android 14+, SDK 34+)
- **Recovery**: LineageOS Recovery or AOSP-based recovery
- **Storage**: ~10 MB free on system partition

## ⚠️ SELinux Compatibility Notice

**Current approach:** APM runs as an init service with `u:r:init:s0` context and should work with SELinux in **enforcing mode**.

The daemon is started by init with proper capabilities and socket permissions, which should allow Binder service registration without requiring permissive mode.

**If you experience SELinux denials:**
- Check `dmesg | grep avc | grep apmd` for denials
- Verify SELinux mode: `getenforce` (should work in enforcing)
- If issues persist, you may need to set permissive mode manually or wait for proper SELinux policy implementation

**Known limitations:**
- Some SELinux policies may still block custom Binder services
- IPC socket fallback provides alternative if Binder fails
- Custom ROM modifications may be needed for full enforcing mode support

**For maximum security:**
- APM attempts to work with enforcing SELinux first
- Falls back to IPC socket transport if Binder is blocked
- Only use permissive mode if both transports fail

**If you need permissive mode:**
Add this line to `init.apmd.rc` in `on post-fs-data` section:
```
exec - root -- /system/bin/setenforce 0
```

## Limitations

- **ARM64 only**: No support for 32-bit devices (yet)
- **LineageOS/AOSP**: Stock ROMs with dm-verity won't work
- **Recovery access**: Requires bootloader unlock to flash
- **OTA**: Survives LineageOS updates, but not ROM changes

## Troubleshooting

### "apmd not running"

Check keeper logs:
```bash
adb shell cat /data/apm/logs/apmd-keeper.log
```

Verify binary:
```bash
adb shell ls -l /system/bin/apmd
adb shell /system/bin/apmd --help
```

### "Service apm.apmd not found"

Wait 30 seconds after boot, then:
```bash
adb shell service list | grep apm.apmd
```

Check SDK level:
```bash
adb shell getprop ro.build.version.sdk
# Must be ≥34
```

### "Installation aborted: wrong arch"

The ZIP is ARM64-only. Rebuild for your device arch:
```bash
./build_android.sh
# Select correct architecture
cd apm-flashable
./build_recovery_zip.sh
```

## Comparison: Magisk vs System-Wide

| Feature | Magisk Module | System-Wide |
|---------|---------------|-------------|
| Requires Magisk | Yes | No |
| Requires root | Yes | No (after install) |
| OTA survival | Via Magisk | Via addon.d |
| System modification | OverlayFS | Direct install |
| Recovery needed | No (if Magisk installed) | Yes (for initial install) |
| Bootloader unlock | Required | Required |

## Directory Structure

```
apm-flashable/
├── META-INF/com/google/android/
│   ├── update-binary          # Shell-based installer
│   └── updater-script         # Dummy file
├── system/
│   ├── bin/                   # Binaries (copied during build)
│   ├── etc/init/init.apmd.rc  # Init service
│   ├── addon.d/               # OTA survival templates
│   └── xbin/                  # Optional tools
├── build.prop                 # Package metadata
├── build_recovery_zip.sh      # Build script
└── README.md

apm-flashable-uninstall/
├── META-INF/com/google/android/
│   ├── update-binary          # Uninstaller script
│   └── updater-script
└── build_uninstall_zip.sh
```

## Development

To modify the installer:
1. Edit `META-INF/com/google/android/update-binary`
2. Test in recovery (no rebuild needed for script changes)
3. Rebuild ZIP to include changes

To modify init behavior:
1. Edit `system/etc/init/init.apmd.rc`
2. Rebuild ZIP
3. Flash and reboot to test

## Credits

Installation approach based on MindTheGApps project pattern.
