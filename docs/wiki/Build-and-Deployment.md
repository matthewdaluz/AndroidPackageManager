# Build and Deployment

## Prerequisites

### Debian/Ubuntu packages (required)

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config git \
  ninja-build soong clang clangd sdkmanager
```

### Other distro equivalents (recommended)

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

Note: package names for `soong` and `sdkmanager` vary by distro/repo.

### BoringSSL prebuilts (required for all builds)

APM uses bundled `libcurl` with repository-staged BoringSSL prebuilts only.

- Android builds require `prebuilt/boringssl/build-<abi>/libssl.a`
- Android builds require `prebuilt/boringssl/build-<abi>/libcrypto.a`
- Host/emulator builds require `prebuilt/boringssl/build-x86_64/` or `prebuilt/boringssl/build-x86/`
- All builds require `prebuilt/boringssl/include/openssl/base.h`

### Required Android SDK components

```bash
sdkmanager --install \
  "build-tools;36.1.0" \
  "cmake;4.1.2" \
  "ndk;r29" \
  "tools;26.1.1"
```

### Recommended editor setup

Visual Studio Code is strongly recommended when modifying APM code. Install:

- `C/C++`
- `C/C++ DevTools`
- `C/C++ Extension Pack`
- `clangd`
- `CMake Tools`

## Build Modes

### Host emulator mode

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAPM_EMULATOR_MODE=ON
cmake --build build -j$(nproc)
```

This mode links against bundled `libcurl` and the host BoringSSL prebuilts in `prebuilt/boringssl/`.

Run with emulator flag:

```bash
./build/apmd --emulator
./build/amsd --emulator
```

### Android NDK mode

```bash
./build_android.sh
```

Notes:

- API level minimum is 29.
- Script supports ABI selection and detects NDK paths.
- Script requires staged Android BoringSSL prebuilts under `prebuilt/boringssl/build-<abi>/`.

### AOSP/Soong mode

Use `Android.bp` targets:

- `apm`
- `apmd`
- `amsd`

## Deployment Options

### Magisk module status

The Magisk version of APM is deprecated and no longer available.

### Lineage recovery flashable

Use `apm-flashable-new/`:

```bash
cd apm-flashable-new
./build_recovery_zip.sh
```

Installer behavior:

- slot-aware target selection under `/mnt/system*`
- payload copy into selected system root
- installs binaries, init scripts, SELinux fragments, addon.d assets

## SELinux payload location

Canonical source used by flashable build script:

- `selinux-contexting/apm.cil`
- `selinux-contexting/apm_file_contexts`
- `selinux-contexting/apm_property_contexts`
- `selinux-contexting/apm_service_contexts`

## Verification Checklist After Deployment

1. Confirm sockets exist:
- `amsd`: `/data/ams/amsd.sock`
1. Confirm `apmd` is listening:
- Android: abstract socket `@apmd` (not visible in the filesystem)
- Emulator: `$HOME/APMEmulator/data/apm/apmd.socket`

2. Check daemon logs:
- `/data/apm/logs/apmd.log`
- `/data/ams/logs/amsd.log`

3. Confirm CLI connectivity:

```bash
apm ping
```

4. Confirm module manager operations:

```bash
apm module-list
```
