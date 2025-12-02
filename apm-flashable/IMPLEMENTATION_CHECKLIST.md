# APM System-Wide Installation - Implementation Checklist

## ✅ Completed Tasks

### 1. Recovery ZIP Structure
- [x] Created `apm-flashable/` directory with proper structure
- [x] Set up `META-INF/com/google/android/` for recovery installer
- [x] Created `system/{bin,etc/init,addon.d,xbin}` directories
- [x] Added `build.prop` with arch and SDK metadata

### 2. Installation Scripts
- [x] Wrote `update-binary` shell script following MindTheGApps pattern
  - [x] Dynamic partition detection
  - [x] A/B slot support
  - [x] Architecture validation (arm64)
  - [x] SDK version checking (≥34)
  - [x] Mount `/mnt/system` read-write
  - [x] Set permissions (0755 for binaries, 0644 for configs)
  - [x] Apply SELinux contexts (`system_file`)
  - [x] Generate addon.d script dynamically
  - [x] Copy files to system partition
- [x] Created dummy `updater-script` file

### 3. OTA Survival (addon.d)
- [x] Created `addond_head` template with header and `list_files()` opener
- [x] Created `addond_tail` with backup/restore case logic
- [x] Implemented permission restoration in post-restore
- [x] Dynamic file list generation in `update-binary`

### 4. Init System Integration
- [x] Created `init.apmd.rc` for system-wide installation
  - [x] `post-fs-data` directory provisioning
  - [x] Removed Magisk-specific bind-mount logic
  - [x] `apmd-keeper` service for auto-restart
  - [x] `apmd` service with Binder registration
  - [x] Proper socket creation and permissions
  - [x] SELinux context `u:r:init:s0`

### 5. Build Automation
- [x] Created `build_recovery_zip.sh`
  - [x] Auto-builds ARM64 binaries if missing
  - [x] Copies binaries to flashable structure
  - [x] Includes xz compression tools
  - [x] Creates dated ZIP file
  - [x] Colorized output with status messages
- [x] Made script executable

### 6. Uninstallation
- [x] Created `apm-flashable-uninstall/` directory
- [x] Wrote uninstaller `update-binary`
  - [x] Removes `/system/bin/{apm,apmd}`
  - [x] Removes `/system/etc/init/init.apmd.rc`
  - [x] Removes `/system/addon.d/30-apm.sh`
  - [x] Removes optional xz tools
  - [x] Preserves `/data/apm/` user data
- [x] Created `build_uninstall_zip.sh`

### 7. Documentation
- [x] Created `apm-flashable/README.md`
  - [x] Structure overview
  - [x] Building instructions
  - [x] Installation steps
  - [x] What gets installed
  - [x] OTA survival explanation
  - [x] Requirements
  - [x] Troubleshooting
- [x] Created `SYSTEM_INSTALL.md` in project root
  - [x] Quick start guide
  - [x] Detailed installation process
  - [x] How it works (install/boot/OTA)
  - [x] Troubleshooting guide
  - [x] Comparison with Magisk approach

## 🔄 Testing Checklist (User)

### Pre-Installation
- [ ] Build APM ARM64 binaries: `./build_android.sh`
- [ ] Generate flashable ZIP: `cd apm-flashable && ./build_recovery_zip.sh`
- [ ] Transfer ZIP to device: `adb push apm-systemwide-*.zip /sdcard/`

### Installation
- [ ] Boot into LineageOS Recovery
- [ ] Flash ZIP via sideload or UI
- [ ] Reboot system
- [ ] Wait 30 seconds for daemon initialization

### Verification
- [ ] Test CLI: `adb shell apm ping`
- [ ] Check binaries: `adb shell ls -l /system/bin/apm*`
- [ ] Verify service: `adb shell service list | grep apm.apmd`
- [ ] Check logs: `adb shell cat /data/apm/logs/apmd-keeper.log`
- [ ] Test daemon: `adb shell cat /data/apm/logs/apmd.log`

### OTA Survival Test (Optional)
- [ ] Install a LineageOS update via OTA
- [ ] Reboot after update
- [ ] Verify APM still works: `adb shell apm ping`
- [ ] Check addon.d executed: `adb shell ls -l /system/addon.d/30-apm.sh`

### Uninstallation Test
- [ ] Build uninstall ZIP: `cd apm-flashable-uninstall && ./build_uninstall_zip.sh`
- [ ] Flash uninstall ZIP in recovery
- [ ] Verify removal: `adb shell ls /system/bin/apm*` (should fail)
- [ ] Check data preserved: `adb shell ls /data/apm/` (should exist)

## 📋 Key Features Implemented

1. **MindTheGApps Pattern**: Shell-based installer, no compiled binary needed
2. **Direct System Install**: Copies to `/system/bin/` while partition is rw
3. **OTA Survival**: addon.d script preserves across LineageOS updates
4. **No Magisk Required**: Pure system integration via recovery
5. **No App Needed**: CLI-only, Binder service auto-starts
6. **No OverlayFS**: Direct file installation, no runtime mounting
7. **ARM64 Target**: Android 14+ (SDK 34+) requirement
8. **A/B Slot Support**: Handles modern partition schemes
9. **SELinux Compatible**: Standard `system_file` context
10. **Clean Uninstall**: Separate uninstaller ZIP

## 🎯 Next Steps (Future Enhancements)

- [ ] Multi-arch support (armeabi-v7a detection and bundling)
- [ ] Automatic arch detection in build script
- [ ] SELinux policy fragment (if denials occur on enforcing devices)
- [ ] Signature verification in update-binary
- [ ] Interactive data wipe option in uninstaller
- [ ] Backup existing binaries before overwriting
- [ ] Version check to prevent downgrades
- [ ] SHA256 integrity checking

## 📝 Notes

- Installation follows MindTheGApps approach exactly
- No privileged app needed (original plan changed)
- System partition must be writable (recovery context)
- Compatible with LineageOS Recovery and AOSP-based recoveries
- Tested structure matches MindTheGApps-Android16 pattern
