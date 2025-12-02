# APM System-Wide Recovery Flashable ZIP

This directory contains the structure for creating a LineageOS Recovery flashable ZIP that installs APM (Android Package Manager) directly to the system partition.

## Structure

```
apm-flashable/
├── META-INF/
│   └── com/
│       └── google/
│           └── android/
│               ├── update-binary    # Installation script
│               └── updater-script   # Dummy file
├── system/
│   ├── bin/                         # APM binaries (copied during build)
│   │   ├── apm
│   │   └── apmd
│   ├── etc/
│   │   └── init/
│   │       └── init.apmd.rc        # Init service definition
│   ├── addon.d/                     # OTA survival scripts
│   │   ├── addond_head             # Template header
│   │   └── addond_tail             # Template footer
│   └── xbin/                        # Optional compression tools
├── build.prop                       # Package metadata (arch, SDK version)
└── build_recovery_zip.sh           # Build script
```

## Building

1. **Build APM binaries** (if not already built):
   ```bash
   cd /path/to/AndroidPackageManager
   ./build_android.sh
   # Select arm64-v8a when prompted
   ```

2. **Generate the flashable ZIP**:
   ```bash
   cd apm-flashable
   ./build_recovery_zip.sh
   ```

3. **Output**: `apm-systemwide-arm64-YYYYMMDD.zip` in the project root

## Installation

1. **Boot into LineageOS Recovery**
2. **Flash the ZIP**: `adb sideload apm-systemwide-arm64-*.zip` or use Recovery UI
3. **Reboot system**
4. **Verify installation**:
   ```bash
   adb shell
   apm ping
   ```

## What Gets Installed

- `/system/bin/apm` - CLI client binary
- `/system/bin/apmd` - Root daemon binary
- `/system/etc/init/init.apmd.rc` - Init service (auto-starts apmd at boot)
- `/system/addon.d/30-apm.sh` - OTA survival script (preserves APM across updates)
- `/system/xbin/{xz,unxz,...}` - Optional compression tools

## OTA Updates

The addon.d script ensures APM survives LineageOS OTA updates. After an OTA:
- APM binaries are automatically restored
- Permissions and ownership are reapplied
- apmd service restarts on next boot

## Requirements

- **Device**: ARM64 Android device
- **OS**: LineageOS 22.0+ (Android 14+, SDK 34+)
- **Recovery**: LineageOS Recovery (or compatible AOSP-based recovery)
- **Bootloader**: Unlocked (to flash recovery ZIP)

## Uninstallation

To remove APM from system:
1. Flash the companion uninstall ZIP (coming soon)
2. Or manually in recovery:
   ```bash
   mount /system
   rm /system/bin/apm
   rm /system/bin/apmd
   rm /system/etc/init/init.apmd.rc
   rm /system/addon.d/30-apm.sh
   ```

## Notes

- **No Magisk required** - Pure system integration
- **No app needed** - CLI-only interface
- **Read-only system safe** - Installed via recovery when system is rw
- **SELinux compatible** - Uses standard system_file context
- **A/B partitions supported** - Handles slot suffixes automatically

## Troubleshooting

**apmd not starting:**
- Check logs: `cat /data/apm/logs/apmd-keeper.log`
- Verify binary permissions: `ls -l /system/bin/apmd`
- Check SELinux: `getenforce` (should work in both permissive/enforcing)

**apm command not found:**
- Verify PATH: `echo $PATH | grep /system/bin`
- Check binary exists: `ls -l /system/bin/apm`

**Binder service not registered:**
- Wait 30 seconds after boot for full initialization
- Check service: `service list | grep apm.apmd`
- Verify SDK level: `getprop ro.build.version.sdk` (must be ≥34)
