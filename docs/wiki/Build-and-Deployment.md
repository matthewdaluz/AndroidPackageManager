# Build and Deployment

## Build System Overview

Primary maintained build paths in the repo:

- CMake for host/emulator and Android NDK builds
- `build_android.sh` as the Android/emulator convenience wrapper
- `Android.bp` for Soong/AOSP integration
- `apm-flashable-new/build_recovery_zip.sh` for LineageOS Recovery flashables

## Prerequisites

### Host tools

Debian/Ubuntu:

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config git \
  ninja-build clang clangd
```

Fedora:

```bash
sudo dnf install -y \
  @development-tools cmake pkgconf-pkg-config git \
  ninja-build clang clang-tools-extra
```

Arch Linux:

```bash
sudo pacman -S --needed \
  base-devel cmake pkgconf git ninja clang
```

### Android SDK / NDK

Recommended SDK packages:

```bash
sdkmanager --install \
  "build-tools;36.1.0" \
  "cmake;4.1.2" \
  "ndk;r29"
```

`build_android.sh` resolves the NDK from:

- `ANDROID_NDK_ROOT`
- `ANDROID_NDK_HOME`
- `$ANDROID_SDK_ROOT/ndk/<latest>`

The build requires Android API `29` or newer.

### BoringSSL Prebuilts

All builds expect staged BoringSSL prebuilts in `prebuilt/boringssl/`.

Required layout:

- `prebuilt/boringssl/include/openssl/base.h`
- Android:
  - `prebuilt/boringssl/build-arm64-v8a/libssl.a`
  - `prebuilt/boringssl/build-arm64-v8a/libcrypto.a`
  - same pattern for `armeabi-v7a`, `x86`, and `x86_64`
- Host/emulator:
  - `prebuilt/boringssl/build-x86_64/` or `build-x86/`

`CMakeLists.txt` also fetches/builds zlib when a system copy is not available.

## Android and Emulator Builds

### Wrapper Script

```bash
./build_android.sh
```

Supported flags:

- `--api <level>`
- `--abi <abi>`
- `--all`
- `--emulator`

Default behavior:

- default API: `34`
- default Android ABI comes from prompt or `APM_ANDROID_ABI`
- Android outputs go to `build-<abi>/`
- emulator output goes to `build/`
- built binaries are stripped unless `APM_STRIP_BINARIES=0`
- workspace `compile_commands.json` is linked or copied from the active build directory

### Build a Specific Android ABI

```bash
./build_android.sh --api 34 --abi arm64-v8a
```

### Build All Android ABIs

```bash
./build_android.sh --all
```

### Build Emulator Mode

```bash
./build_android.sh --emulator
```

This produces native Linux `x86_64` binaries compiled with `APM_EMULATOR_MODE=ON`.

Run emulator daemons with:

```bash
./build/apmd --emulator
./build/amsd --emulator
```

## Direct CMake Emulator Build

```bash
cmake -S . -B build -G Ninja -DAPM_EMULATOR_MODE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"
```

## Soong / AOSP

`Android.bp` defines these binaries:

- `apm`
- `apmd`
- `amsd`

The binary definitions reference `init_rc` entries for daemon startup in Soong/AOSP builds. The maintained recovery flashable carries the concrete init scripts under `apm-flashable-new/system/etc/init/`.

## Recovery Flashable Build

The maintained deployment path is the LineageOS Recovery flashable under `apm-flashable-new/`.

Build a ZIP:

```bash
cd apm-flashable-new
./build_recovery_zip.sh
```

Useful options:

- `./build_recovery_zip.sh --abi arm64-v8a`
- `./build_recovery_zip.sh --all`
- `./build_recovery_zip.sh --api 34`

Environment knobs:

- `APM_FORCE_REBUILD=1` by default
- `APM_FORCE_REBUILD=0` to reuse existing built binaries
- `APM_ANDROID_ABI` to preselect an ABI
- `APM_ANDROID_API` to set the default API
- `APM_SELINUX_SOURCE_DIR` to override the SELinux payload source directory

Current flashable builder behavior:

- rebuilds binaries from current source by default
- copies `apm`, `apmd`, and `amsd` into the template
- syncs SELinux payload from `selinux-contexting/`
- preserves shell PATH boot hooks in `system/bin/`
- emits `apm-lineage-recovery-YYYYMMDD-<abi>.zip`
- verifies that critical payload entries are present in the final ZIP

## Flashable Payload

The flashable ships:

- `/system/bin/apm`
- `/system/bin/apmd`
- `/system/bin/amsd`
- `/system/bin/apm-sh-path`
- `/system/bin/apm-bash-path`
- `/system/etc/init/init.apmd.rc`
- `/system/etc/init/init.amsd.rc`
- `/system/etc/selinux/apm.cil`
- `/system/etc/selinux/apm_file_contexts`
- `/system/etc/selinux/apm_property_contexts`
- `/system/etc/selinux/apm_service_contexts`
- addon.d template pieces used to create `/system/addon.d/30-apm.sh`

The custom recovery installer is slot-aware and chooses its target from `/mnt/system*`.

During install, `update-binary` combines `system/addon.d/addond_head`, the final payload file list, and `system/addon.d/addond_tail` into `system/addon.d/30-apm.sh`.

It also detects recovery layouts where top-level `bin` or `etc` are symlinks and copies into the nested `system/` root when needed.

## Boot/Init Behavior in the Flashable

`init.amsd.rc`:

- prepares `/data/ams`
- starts `amsd` in `post-fs-data`
- restarts `amsd` again on `sys.boot_completed=1`

`init.apmd.rc`:

- exports boot-level fallback values for `ENV` and `BASH_ENV`
- prepares `/data/apm` plus `/data/local/tmp/apm`
- removes stale sockets and stale shim copies of `apm`, `apmd`, and `amsd`
- starts `apmd` only after `amsd.ready=1`

## SELinux Payload

Canonical SELinux payload sources:

- `selinux-contexting/apm.cil`
- `selinux-contexting/apm_file_contexts`
- `selinux-contexting/apm_property_contexts`
- `selinux-contexting/apm_service_contexts`

The policy reflects the current socket-based runtime and keeps the service-context file only for packaging compatibility.

## Verification Checklist After Deployment

1. Confirm `amsd` socket exists:
   - `/data/ams/amsd.sock`
2. Confirm `apmd` is reachable:
   - Android uses abstract socket `@apmd`
   - emulator uses `$HOME/APMEmulator/data/apm/apmd.socket`
3. Confirm logs exist:
   - `/data/apm/logs/apmd.log`
   - `/data/ams/logs/amsd.log`
4. Confirm CLI connectivity:

```bash
apm ping
```

5. Confirm module operations:

```bash
apm module-list
```
