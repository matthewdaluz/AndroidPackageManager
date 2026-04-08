# APM LineageOS Recovery Flashable

This directory contains the maintained recovery deployment template for APM.

It is built for LineageOS Recovery and uses a custom slot-aware installer under `META-INF/com/google/android/update-binary`.

## What the Flashable Contains

Core binaries:

- `system/bin/apm`
- `system/bin/apmd`
- `system/bin/amsd`

Boot PATH fallback hooks:

- `system/bin/apm-sh-path`
- `system/bin/apm-bash-path`

Init services:

- `system/etc/init/init.apmd.rc`
- `system/etc/init/init.amsd.rc`

SELinux payload:

- `system/etc/selinux/apm.cil`
- `system/etc/selinux/apm_file_contexts`
- `system/etc/selinux/apm_property_contexts`
- `system/etc/selinux/apm_service_contexts`

OTA survival:

- generated `system/addon.d/30-apm.sh`

Optional helper payload:

- `system/xbin/*` from `prebuilt/xz/` when that directory exists

## Install Strategy

The custom recovery installer is slot-aware and looks for mount targets under:

- `/mnt/system`
- `/mnt/system_a`
- `/mnt/system_b`

Behavior:

- if only one supported target exists, install there
- if multiple exist, choose the active slot via `ro.boot.slot_suffix` or `ro.boot.slot`
- fail if no supported `/mnt/system*` target is present

## Init Behavior

### `init.amsd.rc`

- creates `/data/ams` runtime directories in `post-fs-data`
- starts `amsd` early
- uses `u:r:su:s0` as the current compatibility service label

### `init.apmd.rc`

- exports:
  - `ENV=/system/bin/apm-sh-path`
  - `BASH_ENV=/system/bin/apm-bash-path`
- creates `/data/apm` and `/data/local/tmp/apm` runtime roots
- removes stale shim copies of `apm`, `apmd`, and `amsd` from `/data/local/tmp/apm/bin`
- removes stale `apmd` socket files
- starts `apmd` only after `amsd.ready=1`

## Runtime Paths Expected by the Flashable

APM:

- `/data/apm`
- `/data/local/tmp/apm`

AMS:

- `/data/ams`

The boot hook scripts pin `/data/local/tmp/apm/bin` onto `PATH` and source:

- `/data/local/tmp/apm/path/sh-path.sh`
- `/data/local/tmp/apm/path/bash-path.sh`

when those generated files exist.

## Build

From this directory:

```bash
./build_recovery_zip.sh
```

Useful variants:

```bash
./build_recovery_zip.sh --abi arm64-v8a
./build_recovery_zip.sh --all
./build_recovery_zip.sh --api 34
```

Important current defaults:

- `APM_FORCE_REBUILD=1`
- default ABI: `arm64-v8a`
- default API: `34`

Outputs are written to the project root as:

- `apm-lineage-recovery-YYYYMMDD-<abi>.zip`

## Build Script Behavior

`build_recovery_zip.sh` currently:

- rebuilds missing binaries, or always rebuilds when `APM_FORCE_REBUILD=1`
- copies `apm`, `apmd`, and `amsd` from `build-<abi>/`
- syncs SELinux payload from `../selinux-contexting/` unless overridden
- verifies critical ZIP contents after packaging

Useful environment overrides:

- `APM_FORCE_REBUILD=0`
- `APM_ANDROID_ABI=<abi>`
- `APM_ANDROID_API=<level>`
- `APM_SELINUX_SOURCE_DIR=/custom/selinux/payload`

## Flash

1. Build the ZIP.
2. Boot into LineageOS Recovery.
3. Sideload or install the generated ZIP.
4. Reboot Android.

## Notes

- Magisk packaging is deprecated and not the maintained path here.
- The service labels intentionally stay conservative for recovery/boot compatibility while custom policy merge behavior varies by device.
