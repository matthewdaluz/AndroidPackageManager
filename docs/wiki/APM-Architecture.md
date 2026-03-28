# APM Architecture

## Components

- `apm`: user CLI. Sends IPC requests and handles local offline commands.
- `apmd`: privileged package daemon. Handles repository updates, package install/remove/upgrade/autoremove, APK workflows, module operations, security sessions, and PATH hotload.
- `amsd`: AMS daemon. Applies enabled overlays, manages module startup script execution, and provides module IPC endpoint.

## Active IPC Topology

- `apm -> apmd`: `/data/apm/apmd.sock`
- `client -> amsd`: `/data/ams/amsd.sock` (module daemon API)
- Emulator mode paths:
  - apmd: `$HOME/APMEmulator/data/apm/apmd.socket`
  - amsd: `$HOME/APMEmulator/ams/amsd.socket`

Binder code exists as legacy/reference in the tree, but current runtime is socket-first.

## Data Layout

### APM data tree

- `/data/apm/installed`
- `/data/apm/cache`
- `/data/apm/pkgs`
- `/data/apm/lists`
- `/data/apm/sources`
- `/data/apm/status`
- `/data/apm/manual-packages`
- `/data/apm/keys`
- `/data/apm/.security`
- `/data/apm/logs`

### AMS data tree

- `/data/ams/modules`
- `/data/ams/logs`
- `/data/ams/.runtime/{upper,work,base}`
- `/data/ams/amsd.sock`
- `/data/ams/.amsd_boot_counter`
- `/data/ams/.amsd_safe_mode_threshold`
- `/data/ams/.amsd_safe_mode`

## Request Security Model

`apmd` request auth policy:

- No session required: `Ping`, `Authenticate`, `ForgotPassword`
- Session required: most package/module/APK/reset actions

`amsd` request auth policy:

- No session required: `Ping`
- Session required: module lifecycle/list operations

Session behavior:

- Session token + expiry + HMAC persisted in `/data/apm/.security/session.bin`
- Expiry is 180 seconds
- HMAC integrity uses key material derived from master key

## Package Workflow (High-level)

1. `apm update` downloads `InRelease` first, then falls back to `Release` + `Release.gpg`.
2. Source options `trusted=` and `deb-signatures=` influence Release and package signature enforcement.
3. `apm install` resolves direct deps (not full recursive SAT solving).
4. `.deb` payloads are checksum verified (SHA256 preferred, MD5 fallback).
5. Optional/required detached `.deb` signatures (`.asc` or `.gpg`) can be verified and cached in `sig-cache.json`.

## PATH Hotload / Command Shims

`apmd` rebuilds command index and shims through `src/apmd/export_path.cpp`:

- Generates shims in `/data/apm/bin`
- Generates canonical PATH source files under `/data/apm/path`:
  - `/data/apm/path/sh-path.sh` (sh/mksh source file)
  - `/data/apm/path/bash-path.sh` (bash source file)
- Flashable boot fallback exports from init:
  - `ENV=/system/bin/apm-sh-path`
  - `BASH_ENV=/system/bin/apm-bash-path`
  - these scripts keep `/data/apm/bin` on PATH and source `/data/apm/path/*` when present
- Installs shell hooks into:
  - `/data/local/userinit.sh`
  - `/data/local/tmp/.profile`
  - `/data/local/tmp/.mkshrc`
  - `/data/local/tmp/.bashrc`
  - `/data/local/tmp/.bash_profile`
  - `/data/.profile` (best effort)
  - `/data/.mkshrc` (best effort)
  - `/data/.bashrc` (best effort)
  - `/data/.bash_profile` (best effort)
- May install a legacy `service.d` hook if `/data/adb/service.d` exists (compatibility path only; official Magisk APM package is deprecated and no longer distributed)

## Versioning Note

Current hardcoded CLI version string lives in `src/apm/apm.cpp` and is `2.0.0b - Open Beta`.
