# BoringSSL Android Multi-ABI Build Helper

This directory contains `build_all.sh`, a helper script used to compile BoringSSL for all Android ABIs required by APM.

## Purpose

Use this script to generate one Android build directory per ABI from the BoringSSL source tree so the compiled artifacts can be staged into the repository's prebuilt area.

Target ABIs:

- `arm64-v8a`
- `armeabi-v7a`
- `x86`
- `x86_64`

## What the script does

When run from the BoringSSL source root, `build_all.sh`:

1. Configures CMake with the Android NDK toolchain for each ABI.
2. Builds with Ninja.
3. Creates/refreshes `build-<abi>` directories in the BoringSSL root.
4. Copies each build directory's output into `out/<abi>` for convenience.

## Prerequisites

- Android NDK installed at the path configured in the script (`NDK_PATH`).
- `cmake` and `ninja` available in `PATH`.

If your NDK path differs, edit `NDK_PATH` in `build_all.sh`.

## Usage

Run the script from the root directory of the BoringSSL source checkout:

```bash
cd /path/to/boringssl
bash /path/to/AndroidPackageManager/boringssl-tools/compile-for-all-abi/build_all.sh
```

After completion, copy all generated build directories and the `include` directory into this repository's prebuilt tree:

```bash
cd /path/to/boringssl
mkdir -p /path/to/AndroidPackageManager/prebuilt/boringssl
cp -a build-arm64-v8a build-armeabi-v7a build-x86 build-x86_64 include \
	/path/to/AndroidPackageManager/prebuilt/boringssl/
```

That staged content is what APM consumes as prebuilt BoringSSL artifacts.
