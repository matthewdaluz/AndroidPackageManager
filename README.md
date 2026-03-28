# APM - Android Package Manager

APM is a GPLv3 package manager for rooted Android with an APT-like workflow and Android-specific daemon tooling.

Current CLI version string in source: `2.0.0b - Open Beta`.

## What APM Includes

- `apm`: CLI client.
- `apmd`: main package daemon (install/update/remove/upgrade, APK workflows, AMS module operations).
- `amsd`: AMS daemon (overlay application + module daemon IPC).
- AMS (APM Module System): module format and runtime for system/vendor/product overlays.

## Architecture At A Glance

- Transport is UNIX socket based.
- CLI requests go to `apmd` over `/data/apm/apmd.sock` (emulator: `$HOME/APMEmulator/data/apm/apmd.socket`).
- AMS daemon serves module IPC over `/data/ams/amsd.sock` (emulator: `$HOME/APMEmulator/ams/amsd.socket`).
- Binder code exists in-tree as legacy/reference, but active runtime flow is socket-first.

## Repository Layout

```text
src/
  apm/        CLI + IPC client
  apmd/       Main daemon, package install logic, APK logic, auth/session, PATH hotload
  amsd/       AMS daemon, module dispatcher, overlay startup/safe-mode handling
  ams/        Module metadata + module manager
  core/       Repo parsing, download/index handling, status DB, extraction, shared security utils
  util/       Filesystem helpers + crypto helpers (SHA256/MD5/OpenPGP verify)
  thirdparty/ Vendored headers
```

## Runtime Paths (Android)

APM root:

- `/data/apm/installed`
- `/data/apm/installed/commands`
- `/data/apm/installed/dependencies`
- `/data/apm/installed/termux`
- `/data/apm/bin`
- `/data/apm/cache`
- `/data/apm/pkgs`
- `/data/apm/lists`
- `/data/apm/status`
- `/data/apm/sources` and `/data/apm/sources/sources.list.d`
- `/data/apm/manual-packages`
- `/data/apm/keys`
- `/data/apm/path`
- `/data/apm/.security`
- `/data/apm/logs`
- `/data/apm/apmd.sock`

AMS root:

- `/data/ams/modules`
- `/data/ams/logs`
- `/data/ams/.runtime`
- `/data/ams/.runtime/upper`
- `/data/ams/.runtime/work`
- `/data/ams/.runtime/base`
- `/data/ams/amsd.sock`
- Safe-mode files: `/data/ams/.amsd_boot_counter`, `/data/ams/.amsd_safe_mode_threshold`, `/data/ams/.amsd_safe_mode`

Emulator mode roots:

- `$HOME/APMEmulator/data/apm`
- `$HOME/APMEmulator/ams`

## Build Prerequisites

### Debian/Ubuntu packages (required)

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config git patch \
  zlib1g-dev libcurl4-openssl-dev libssl-dev \
  ninja-build soong clang clangd sdkmanager
```

### Other distro equivalents (recommended)

Fedora:

```bash
sudo dnf install -y \
  @development-tools cmake pkgconf-pkg-config git patch \
  zlib-devel libcurl-devel openssl-devel \
  ninja-build clang clang-tools-extra
```

Arch Linux:

```bash
sudo pacman -S --needed \
  base-devel cmake pkgconf git patch zlib curl openssl ninja clang
```

Note: package names for `soong` and `sdkmanager` vary by distro/repo.

### Android SDK/NDK components (required)

```bash
sdkmanager --install \
  "build-tools;36.1.0" \
  "cmake;4.1.2" \
  "ndk;r29" \
  "tools;26.1.1"
```

### Recommended editor setup

Visual Studio Code is strongly recommended for modifying APM source. Install:

- `C/C++`
- `C/C++ DevTools`
- `C/C++ Extension Pack`
- `clangd`
- `CMake Tools`

## Build

### 1) Host Emulator Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAPM_EMULATOR_MODE=ON
cmake --build build -j$(nproc)
```

Run daemons with `--emulator` when built in emulator mode.

### 2) Android Build (NDK)

```bash
./build_android.sh
```

Notes:

- Script enforces API level >= 29.
- Detects NDK from `ANDROID_NDK_ROOT`/`ANDROID_NDK_HOME` or `$ANDROID_SDK_ROOT/ndk`.
- Includes an `x86_64 (Emulator Mode)` option.

### 3) AOSP/Soong

Use `Android.bp` (`apm`, `apmd`, `amsd` targets).

## Deploy

### Magisk Module Status

The Magisk version of APM is deprecated and no longer available.

### Recovery Flashable

`apm-flashable-new/` contains a slot-aware Lineage Recovery installer and SELinux payload.

## Repositories And Trust Policies

APM reads `deb` entries from:

- `/data/apm/sources/sources.list`
- `/data/apm/sources/sources.list.d/*.list`

Supported options in bracket blocks include:

- `arch=` / `architectures=`
- `trusted=`
- `deb-signatures=`

Trust behavior:

- `trusted=yes` skips Release signature verification.
- `trusted=required` requires valid Release signature.
- Default (`trusted` not set) attempts verification and can continue unverified on failure.

Package signature behavior:

- `deb-signatures=required`: `.deb` detached signature required and must verify.
- `deb-signatures=optional`: attempts verify if signature exists; install may continue when missing/invalid.
- `deb-signatures=disabled`: no package-level detached signature enforcement.

`Packages.gz` and plain `Packages` are used. `Packages.xz` is intentionally disabled in current Android path.

## CLI Commands

Daemon-backed:

- `apm ping`
- `apm update`
- `apm install <pkg>`
- `apm remove <pkg>`
- `apm upgrade [pkgs...]`
- `apm autoremove`
- `apm factory-reset`
- `apm forgot-password`
- `apm module-list`
- `apm module-install <zip>`
- `apm module-enable <name>`
- `apm module-disable <name>`
- `apm module-remove <name>`
- `apm apk-install <apk> [--install-as-system]`
- `apm apk-uninstall <package>`

Local/offline helpers:

- `apm list`
- `apm info <pkg>`
- `apm search <pattern>`
- `apm package-install <file>`
- `apm key-add <file.asc|file.gpg>`
- `apm sig-cache show`
- `apm sig-cache clear`
- `apm version`
- `apm help`

## Security Model

- First privileged flow sets or unlocks a password/PIN.
- Exactly 3 security questions are required on initial setup.
- Master key at `/data/apm/.security/masterkey.bin`.
- Password/PIN and security answers are stored encrypted.
- PBKDF2-HMAC-SHA256 uses 200,000 iterations.
- Session token is persisted with HMAC integrity and 180-second expiry.
- Forgot-password wrong answers trigger a 5-minute cooldown lockout.
- `amsd` validates session tokens against the same session material.

## Package And APK Notes

Package install behavior:

- Dependency resolver is currently direct dependency oriented (not full recursive SAT solver).
- SHA256 verification is preferred, with MD5 fallback when metadata provides it.
- Signature verification cache stored at `/data/apm/pkgs/sig-cache.json`.

Manual package install:

- Supports `.deb` and archive suffixes `.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.tar`, `.gz`, `.xz`.
- Tar-based manual packages require `package-info.json`.

APK workflows:

- User app install uses staged `pm install --user 0 -r` flow.
- `--install-as-system` stages APK into AMS module `apm-system-apps` under `overlay/system/app/.../base.apk`.

## AMS Overview

AMS modules live in `/data/ams/modules/<module_name>`.

Expected module files:

- `module-info.json` (required)
- `overlay/` (required)
- `overlay/system`, `overlay/vendor`, `overlay/product` (optional subtrees)
- `install.sh` (optional, required when `install-sh` is `true`)
- `post-fs-data.sh` (optional)
- `service.sh` (optional)

`module-info.json` fields in current parser:

- `name` (required)
- `version`
- `author`
- `description`
- `mount` (default `true`)
- `post_fs_data` (default `false`)
- `service` (default `false`)
- `install-sh` (default `false`)

`state.json` fields:

- `enabled`
- `installed_at`
- `updated_at`
- `last_error`

Install expectations:

- ZIP extraction uses `unzip -oq`.
- Either flat module root or single nested top directory is accepted.
- `overlay/` directory must exist.
- If `install-sh` is `true`, `install.sh` is required and runs once during
  `module-install`.
- If `install.sh` fails, `module-install` fails and AMS automatically rolls
  back by uninstalling the module.

Overlay targets:

- `system`
- `vendor`
- `product`

Current implementation detail: the bind-backend path is the primary overlay path reached during `applyOverlayForTarget()`.

## Logging

- `apmd`: `/data/apm/logs/apmd.log`
- `amsd`: `/data/ams/amsd.log`
- Module logs: `/data/ams/logs/<module>.log`

## Known Limitations

- Dependency resolution is direct-dependency oriented (not a full recursive SAT resolver).
- `Packages.xz` indexes are intentionally disabled in the current Android path.
- Binder paths remain in source as legacy/reference; active runtime uses UNIX sockets.
- `apmd` startup aborts if legacy modules are detected under `/data/apm/modules`.

## Documentation (Wiki Drafts In Repo)

Wiki-ready markdown pages are in `docs/wiki/`:

- `docs/wiki/Home.md`
- `docs/wiki/APM-Architecture.md`
- `docs/wiki/AMS-Architecture.md`
- `docs/wiki/AMS-Module-Development.md`
- `docs/wiki/CLI-and-Operations.md`
- `docs/wiki/Build-and-Deployment.md`
- `docs/wiki/Troubleshooting.md`

These are intended to be copied into GitHub Wiki pages.

## License

GNU GPL v3.0 or later.
