# BoringSSL Android Multi-ABI Build Helper

This directory contains `build_all.sh`, a helper for producing Android BoringSSL build directories that APM can stage into `prebuilt/boringssl/`.

## What the Script Builds

Current ABI list in `build_all.sh`:

- `arm64-v8a`
- `armeabi-v7a`
- `x86`
- `x86_64`

Current script defaults:

- `NDK_PATH=/opt/android-sdk/ndk/29.0.14206865`
- `API_LEVEL=android-35`
- `BUILD_TYPE=Release`

## What It Does

When run from the root of a BoringSSL checkout, the script:

1. creates `build-<abi>` directories
2. configures CMake with the Android toolchain for each ABI
3. builds with Ninja
4. copies each build directory into `out/<abi>/`

It does not copy artifacts directly into the APM repository for you.

## Prerequisites

- a local BoringSSL checkout
- Android NDK at the path configured in the script, or a manually edited replacement
- `cmake`
- `ninja`

If your NDK path differs, edit `NDK_PATH` in `build_all.sh` before running it.

## Usage

From the BoringSSL source root:

```bash
bash /path/to/AndroidPackageManager/boringssl-tools/compile-for-all-abi/build_all.sh
```

After it finishes, stage the results into APM's prebuilt tree:

```bash
mkdir -p /path/to/AndroidPackageManager/prebuilt/boringssl
cp -a \
  build-arm64-v8a \
  build-armeabi-v7a \
  build-x86 \
  build-x86_64 \
  include \
  /path/to/AndroidPackageManager/prebuilt/boringssl/
```

APM expects those staged directories to contain:

- `libssl.a`
- `libcrypto.a`

and it expects headers under:

- `prebuilt/boringssl/include/`

## Important Limitation

This helper only covers Android ABI builds.

APM emulator/host builds also require native BoringSSL prebuilts in one of:

- `prebuilt/boringssl/build-x86_64/`
- `prebuilt/boringssl/build-x86/`

Those host prebuilts must be built separately.
