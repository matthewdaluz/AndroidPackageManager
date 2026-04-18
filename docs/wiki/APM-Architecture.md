# APM Architecture

## Core Components

- `apm`: CLI entry point. It mixes local commands with daemon-backed requests.
- `apmd`: privileged package daemon. It owns repository updates, package lifecycle operations, APK operations, security sessions, factory reset, and command hotload.
- `amsd`: AMS daemon. It owns boot-time overlay application, safe mode, and module lifecycle script startup.
- `ModuleManager`: shared AMS lifecycle engine used by both daemons.

## Active IPC Topology

- `apm -> apmd`
  - Android: abstract UNIX socket `@apmd`
  - Emulator: `$HOME/APMEmulator/data/apm/apmd.socket`
- `client -> amsd`
  - Android: `/data/ams/amsd.sock`
  - Emulator: `$HOME/APMEmulator/ams/amsd.socket`

The runtime transport is socket-first. Binder-facing files in the tree are legacy or compatibility material, not the active request path.

## Filesystem Layout

### Persistent APM data

Under `/data/apm`:

- `cache/`
- `keys/`
- `lists/`
- `logs/`
- `pkgs/`
- `sandbox/state/`
- `sandbox/env/`
- `sandbox/mounts/`
- `sources/*.repo`
- `status`
- `.security/`
- `debug.txt`

### Shell-accessible runtime payloads

Under `/data/local/tmp/apm`:

- `runtime/installed/`
- `runtime/installed/commands/`
- `runtime/installed/dependencies/`
- `runtime/installed/termux/`
- `runtime/manual-packages/`
- `bin/`
- `path/`
- `logs/`

### AMS data

Under `/data/ams`:

- `modules/`
- `logs/`
- `.runtime/{upper,work,base}`
- `amsd.sock`
- `.amsd_boot_counter`
- `.amsd_safe_mode_threshold`
- `.amsd_safe_mode`

## Startup and Migration Behavior

`apmd` performs runtime migration on Android startup:

- migrates older installed/runtime trees into current shell-accessible locations
- rewrites stored install roots in the status DB
- rewrites manual package metadata prefixes
- normalizes permissions so shell-facing runtime paths remain readable/usable

If legacy modules are still present under `/data/apm/modules`, `apmd` refuses to continue and requires a factory reset before the AMSD-enabled layout can run.

## Request Security Model

### `apmd`

Session-free requests:

- `Ping`
- `Authenticate`
- `ForgotPassword`

Session-required requests:

- `Update`
- `Install`
- `Remove`
- `Autoremove`
- `Upgrade`
- `ApkInstall`
- `ApkUninstall`
- `ModuleList`
- `ModuleInstall`
- `ModuleEnable`
- `ModuleDisable`
- `ModuleRemove`
- `FactoryReset`
- `DebugLogging`

### `amsd`

Session-free requests:

- `Ping`

Session-required requests:

- `ModuleList`
- `ModuleInstall`
- `ModuleEnable`
- `ModuleDisable`
- `ModuleRemove`

### Session Details

- session file: `/data/apm/.security/session.bin`
- expiry: `180` seconds
- integrity: HMAC over `token|expiry`
- HMAC material is derived from the master key

## Repository Update Flow

`apm update` and daemon-side metadata loading use this shape:

1. Parse `/data/apm/sources/*.repo`
2. Detect repo format and architecture
3. Download `InRelease` when available
4. Fall back to `Release` + `Release.gpg`
5. Apply `Trusted=` policy
6. Parse Release checksums
7. Download `Packages.gz` first
8. Fall back to plain `Packages` if needed
9. Reject `Packages.xz` in the current Android path

`.repo` fields currently recognized:

- `Type=deb`
- `URL=...`
- `Suites=<dist>,<component>[,<component>...]`
- `Architectures=...`
- `Trusted=...`
- `Deb-Signatures=...`

APM also auto-detects Termux-style repos and maps architectures accordingly.

## Package Install Flow

Daemon-backed package installation is handled by `install_manager.cpp`.

Current behavior:

- dependency resolution is direct and practical, not a full SAT solver
- package payloads are checksum-verified
- SHA256 is preferred, with MD5 fallback if metadata provides it
- detached `.deb` signatures can be enforced per source
- detached verification results are cached in `/data/apm/pkgs/sig-cache.json`
- installed package metadata is persisted in a dpkg-style status file
- `autoInstalled` dependencies can later be removed by `autoremove`

Manual/local packages are separate from the repo-backed status DB:

- `apm package-install` handles local `.deb` files plus tar-style archives such as `.tar`, `.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.gz`, and `.xz`
- tarball/manual installs require `package-info.json`
- manual package metadata is stored under `/data/local/tmp/apm/runtime/manual-packages`

## PATH Hotload and Command Shims

APM rebuilds command shims after daemon startup and package changes.

Generated output:

- shim directory: `/data/local/tmp/apm/bin`
- sh hook file: `/data/local/tmp/apm/path/sh-path.sh`
- bash hook file: `/data/local/tmp/apm/path/bash-path.sh`
- command index: `/data/apm/sandbox/state/command-index.json`
- path env snapshot: `/data/apm/sandbox/env/apm-path.env`

Boot fallback hooks shipped by the flashable:

- `/system/bin/apm-sh-path`
- `/system/bin/apm-bash-path`

`apmd` also attempts to inject or refresh APM hook blocks in common shell startup files so new shell sessions see installed commands without manual PATH editing.

## APK Handling

`apmd` exposes two APK flows:

- user app install:
  - stage APK under `/data/local/tmp/apm-apk-staging`
  - run `pm install --user 0 -r`
- system app staging:
  - require root
  - stage `base.apk` into the AMS module `apm-system-apps`
  - target path: `/data/ams/modules/apm-system-apps/overlay/system/app/<name>/base.apk`
  - reboot required for Android to recognize the staged system app

## Versioning Note

The current hardcoded CLI version string lives in `src/apm/apm.cpp` and is `2.0.1b - Open Beta`.
