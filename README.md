# APM - Android Package Manager

APM is a GPL-3.0-or-later package manager stack for Android. It combines an APT-like package workflow, a privileged daemon, an Android module system, APK staging helpers, and shell PATH hotload support.

Current CLI version in source: `2.0.3b - Open Beta`

## Components

- `apm`: CLI client. It handles local read-only commands, manual package installs, log viewing/export, trusted-key import, and IPC requests to the daemon.
- `apmd`: privileged package daemon. It handles repository updates, package install/remove/upgrade/autoremove, APK install/uninstall, security sessions, factory reset, and command shim generation.
- `amsd`: AMS daemon. It applies overlays, runs enabled module scripts at boot, manages AMS safe mode, and exposes module IPC.
- AMS: APM Module System. Modules live under `/data/ams/modules` and can overlay `system`, `vendor`, and `product`.

## Runtime Architecture

- Active transport is UNIX sockets, not Binder.
- `apm -> apmd`
  - Android: abstract socket `@apmd`
  - Emulator mode: `$HOME/APMEmulator/data/apm/apmd.socket`
- `client -> amsd`
  - Android: `/data/ams/amsd.sock`
  - Emulator mode: `$HOME/APMEmulator/ams/amsd.socket`

## Runtime Layout

### Android

Persistent APM data under `/data/apm`:

- `cache`
- `keys`
- `lists`
- `logs`
- `pkgs`
- `sandbox/{state,env,mounts}`
- `sources/*.repo`
- `status`
- `.security/`
- `debug.txt`

Shell-accessible runtime content under `/data/local/tmp/apm`:

- `runtime/installed/`
- `runtime/installed/commands/`
- `runtime/installed/dependencies/`
- `runtime/installed/termux/`
- `runtime/manual-packages/`
- `bin/`
- `path/`
- `logs/`

AMS data under `/data/ams`:

- `modules/`
- `logs/`
- `.runtime/{upper,work,base}`
- `amsd.sock`
- `.amsd_boot_counter`
- `.amsd_safe_mode_threshold`
- `.amsd_safe_mode`

### Emulator Mode

- APM root: `$HOME/APMEmulator/data/apm`
- AMS root: `$HOME/APMEmulator/ams`

The CLI auto-detects emulator mode by looking for the emulator `apmd.socket`.

## Package and Repository Behavior

- Sources are read from `/data/apm/sources/*.repo`.
- Add a source with `apm add-repo <file.repo>`, then run `apm update`.
- Supported source options:
  - `Type=deb`
  - `URL=...`
  - `Suites=<dist>,<component>[,<component>...]`
  - `Architectures=...`
  - `Trusted=...`
  - `Deb-Signatures=...`
- Release metadata flow:
  - Prefer `InRelease`
  - Fall back to `Release` + `Release.gpg`
- `Packages.gz` and plain `Packages` are supported.
- `Packages.xz` is intentionally disabled in the current Android path.
- Termux-style repositories are auto-detected and architecture-mapped when needed.

Trust behavior:

- `Trusted=yes|true|1`: skip Release signature verification
- `Trusted=required`: require Release signature verification
- default: try verification and continue unverified on failure

Detached `.deb` signature behavior:

- `Deb-Signatures=required`: package install fails if detached verification fails or no signature is available
- `Deb-Signatures=optional`: verification is attempted when possible, but install can continue
- `Deb-Signatures=disabled`: skip detached package signature checks

Detached package signature results are cached in `/data/apm/pkgs/sig-cache.json`.

## CLI Surface

Daemon-backed commands:

- `apm ping`
- `apm update`
- `apm add-repo <file.repo>`
- `apm list-repos`
- `apm remove-repo <name|name.repo>`
- `apm install <pkg>`
- `apm remove <pkg>`
- `apm upgrade [pkgs...]`
- `apm autoremove`
- `apm debuglogging <true|false>`
- `apm factory-reset`
- `apm wipe-cache [all|apm|repo-lists|package-downloads|sig-cache|ams-runtime]`
- `apm forgot-password`
- `apm module-list`
- `apm module-install <zip>`
- `apm module-enable <name>`
- `apm module-disable <name>`
- `apm module-remove <name>`
- `apm apk-install <apk> [--install-as-system]`
- `apm apk-uninstall <package>`

Session-free daemon/local commands:

- `apm list`
- `apm list-repos`
- `apm info <pkg>`
- `apm search <pattern>`
- `apm package-install <file>`
- `apm log [--apm|--ams|--module <name>|<module>] [--export|--clear]`
- `apm log --clear-all`
- `apm version`
- `apm key-add <file.asc|file.gpg>`
- `apm sig-cache show`
- `apm sig-cache clear`
- `apm help`

Notes:

- `package-install` supports local `.deb` files plus tar-style archives such as `.tar`, `.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.gz`, and `.xz`.
- Tarball/manual packages must contain `package-info.json`.
- `remove` first checks whether the target is a manual package and removes it locally before falling back to daemon-backed package removal.
- `log --export` writes a timestamped copy to `/storage/emulated/0`.
- `log --export` and live log following are local read operations.
- `log --clear` and `log --clear-all` are daemon-backed operations and require an authenticated session because they delete privileged log files.
- `apk-install --install-as-system` stages the APK into the AMS-backed `apm-system-apps` overlay module and requires a reboot for Android to see it as a system app.

## Security Model

- Privileged operations require an authenticated session.
- Session-free requests:
  - `Ping`
  - `Authenticate`
  - `ForgotPassword`
  - `List`
  - `Info`
  - `Search`
  - `ListRepos`
- Session-required requests:
  - update/add-repo/remove-repo/install/remove/upgrade/autoremove
  - APK operations
  - module lifecycle operations
  - factory reset
  - debug logging toggle
- First privileged use triggers password/PIN setup if none exists.
- Initial setup requires exactly 3 security questions.
- Password/PIN and security-question data are encrypted with AES-256-GCM.
- User-secret derivation uses PBKDF2-HMAC-SHA256 with `200000` iterations.
- Session tokens are HMAC-protected and expire after `180` seconds.
- Failed forgot-password answer verification triggers a `5` minute cooldown.

## Build Prerequisites

### Debian/Ubuntu

```bash
sudo apt update
sudo apt install -y \
  build-essential cmake pkg-config git \
  ninja-build clang clangd
```

### Fedora

```bash
sudo dnf install -y \
  @development-tools cmake pkgconf-pkg-config git \
  ninja-build clang clangd clang-tools-extra
```

### Arch Linux

```bash
sudo pacman -Sy --needed \
  base-devel cmake pkgconf git ninja clang clangd
```

### Android SDK / NDK

`build_android.sh` expects an Android SDK plus an NDK reachable via `ANDROID_NDK_ROOT`, `ANDROID_NDK_HOME`, or `$ANDROID_SDK_ROOT/ndk`.

Recommended SDK components:

```bash
sdkmanager --install \
  "build-tools;36.1.0" \
  "cmake;4.1.2" \
  "ndk;r29"
```

### BoringSSL Prebuilts

All CMake builds use repository-staged BoringSSL prebuilts:

- Android: `prebuilt/boringssl/build-<abi>/libssl.a`
- Android: `prebuilt/boringssl/build-<abi>/libcrypto.a`
- Host/emulator: `prebuilt/boringssl/build-x86_64/` or `build-x86/`
- Headers: `prebuilt/boringssl/include/openssl/base.h`

See `boringssl-tools/compile-for-all-abi/README.md` for the Android helper flow.

## Build

### Android / NDK

```bash
./build_android.sh
```

Helpful flags:

- `./build_android.sh --abi arm64-v8a`
- `./build_android.sh --all`
- `./build_android.sh --emulator`
- `./build_android.sh --api 34`

Behavior:

- API level must be `29` or newer
- Android builds go to `build-<abi>/`
- Emulator builds go to `build/`
- `compile_commands.json` is published at the workspace root

### Host Emulator Build

```bash
cmake -S . -B build -G Ninja -DAPM_EMULATOR_MODE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel "$(nproc)"
```

Run emulator binaries with `--emulator`:

```bash
./build/apmd --emulator
./build/amsd --emulator
```

### AOSP / Soong

`Android.bp` defines `apm`, `apmd`, and `amsd` targets.

## Deployment

### Recovery Flashable

`apm-flashable-new/` contains the maintained recovery deployment path.

Build it with:

```bash
cd apm-flashable-new
./build_recovery_zip.sh
```

The flashable builder:

- rebuilds binaries by default with `APM_FORCE_REBUILD=1`
- supports `--abi`, `--all`, and `--api`
- syncs SELinux payload from `selinux-contexting/`
- emits `apm-lineage-recovery-YYYYMMDD-<abi>.zip`

Magisk packaging is deprecated and no longer maintained here.

## Documentation

- `docs/wiki/Home.md`
- `docs/wiki/APM-Architecture.md`
- `docs/wiki/AMS-Architecture.md`
- `docs/wiki/CLI-and-Operations.md`
- `docs/wiki/Build-and-Deployment.md`
- `docs/wiki/AMS-Module-Development.md`
- `docs/wiki/Troubleshooting.md`
- `.deb-signature-verification-flow.md`
