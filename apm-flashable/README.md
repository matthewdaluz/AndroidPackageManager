# APM System-Wide Recovery Flashable ZIP

**⚠️ DEPRECATED/EXPERIMENTAL:** This deployment method is currently deprioritized due to SELinux policy conflicts on boot. **Use the Magisk module (`apm-magisk/`) instead**, which handles policy application and daemon lifecycle correctly.

This directory contains the structure for creating a LineageOS Recovery flashable ZIP that installs APM (Android Package Manager) directly to the system partition. It is retained for reference and future development but should not be used for production deployments.

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

## SELinux Policy Integration

**APM includes proper SELinux policies for enforcing mode compatibility.**

The installation includes CIL (Common Intermediate Language) policy files that define a custom `apmd` security domain with appropriate permissions for package management operations.

### Included Policy Files:

- **`/system/etc/selinux/apm.cil`** - Main policy defining types, domains, and access rules
- **`/system/etc/selinux/apm_file_contexts`** - File→type mappings for APM binaries and data
- **`/system/etc/selinux/apm_service_contexts`** - Binder service→type mapping

### SELinux Types:

| Type | Purpose |
|------|---------|
| `apmd` | Security domain for daemon process |
| `apmd_exec` | Executable type for /system/bin/apmd |
| `apmd_service` | Binder service type for apm.apmd |
| `apmd_data_file` | Data files in /data/apm/ |
| `apmd_socket` | Unix socket at /dev/socket/apmd |

### Key Permissions:

- ✅ Shell processes can find and communicate with apmd service
- ✅ Android apps (system, platform, untrusted) can access service
- ✅ Network access for repository downloads
- ✅ System file access for package installation
- ✅ Module loading capabilities for AMS

### How It Works:

1. **Installation**: Recovery ZIP copies policy files to /system/etc/selinux/
2. **Boot**: Android init loads .cil policy automatically during early boot
3. **Service Start**: apmd starts with `u:r:apmd:s0` context (defined in init.apmd.rc)
4. **Runtime**: All access controlled by explicit policy rules

### Verification:

```bash
# Check apmd runs with correct context
ps -AZ | grep apmd
# Should show: u:r:apmd:s0

# Verify service context
service check apm.apmd
# Should be accessible

# Confirm enforcing mode
getenforce
# Returns: Enforcing

# Check for policy violations
dmesg | grep avc | grep apmd
# Should be empty (no denials)
```

### Compatibility:

- **LineageOS 22+**: Full support, policies load automatically
- **AOSP-based ROMs**: Should work with standard init policy loading
- **Stock vendor ROMs**: May require additional vendor policy integration

**Note:** If your ROM doesn't support CIL policy loading from /system/etc/selinux/, the service will fall back to IPC socket transport which works without special policies.

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
- Check logs: `cat /data/apm/logs/apmd.log`
- Verify binary permissions: `ls -l /system/bin/apmd`
- Check SELinux: `getenforce` (should return "Permissive")
- Verify service: `pidof apmd` (should return a PID)

**apm command not found:**
- Verify PATH: `echo $PATH | grep /system/bin`
- Check binary exists: `ls -l /system/bin/apm`

**Binder service not registered:**
- Wait 30 seconds after boot for full initialization
- Check service: `service list | grep apm.apmd`
- Verify SDK level: `getprop ro.build.version.sdk` (must be ≥34)
