# Build and Deployment

## Build Modes

### Host emulator mode

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAPM_EMULATOR_MODE=ON
cmake --build build -j$(nproc)
```

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

### AOSP/Soong mode

Use `Android.bp` targets:

- `apm`
- `apmd`
- `amsd`

## Deployment Options

### 1) Magisk module

Use assets under `apm-magisk/`.

Highlights:

- post-fs-data script creates runtime dirs and populates `/data/apm/bin`
- service script starts `amsd` first, then `apmd`
- init rc files are included in module payload

### 2) Lineage recovery flashable

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
- `/data/apm/apmd.sock`
- `/data/ams/amsd.sock`

2. Check daemon logs:
- `/data/apm/logs/apmd.log`
- `/data/ams/amsd.log`

3. Confirm CLI connectivity:

```bash
apm ping
```

4. Confirm module manager operations:

```bash
apm module-list
```
