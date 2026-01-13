# APM — Android Package Manager

APM is a GPLv3-licensed package manager for rooted Android. It mirrors the APT workflow (sources lists, Packages indices, dependency tracking) while adding Android-specific features: a root daemon that performs filesystem changes, CLI helpers that work offline when possible, Termux compatibility, APK install/uninstall helpers, and a Magisk-inspired module system (AMS) for OverlayFS-based system customization. The runtime is IPC-only over UNIX sockets (Binder code is retained for reference). `apmd` handles package lifecycle; `amsd` applies AMS overlays from `/data/ams`, exposes module IPC on `/dev/socket/amsd`, and uses safe-mode counters under `/ams` to avoid overlay boot loops. Emulator mode is available for host-side testing (`$HOME/APMEmulator`).

> Upgrade note: AMSD builds require factory-resetting previous `/data/apm` installs before flashing. Legacy modules under `/data/apm/modules` will block startup; remove them or perform a full reset first. The current CLI/daemon release is **APM 1.8.0b (open beta)**.


## Components

| Piece | Role |
| ----- | ---- |
| `apm` | User-facing CLI. Talks to `apmd` over `/data/apm/apmd.sock` (IPC-only), renders progress, and runs local-only queries (`list`, `info`, `search`, `package-install`). |
| `apmd` | Root daemon. Downloads repo metadata, installs/upgrades/removes packages, maintains PATH helper scripts, performs package/APK verification, and handles AMS module management plus APK/system-overlay work. |
| `amsd` | AMS daemon. Applies OverlayFS layers for enabled modules, monitors partitions, tracks safe-mode boot counters (under `/ams`), and exposes module-only IPC on `/dev/socket/amsd`. |
| AMS | Built-in module system similar to Magisk. Modules live under `/data/ams/modules`, carry metadata + overlay payloads, and are mounted via OverlayFS by `amsd` at startup or when partitions appear. |
| Core | Shared plumbing for repo parsing, status DB, dependency resolution, tar/deb extraction, download helpers (curl + zlib), and GPG verification. |

Repository layout:

```
src/
  apm/        # CLI + commands + IPC client
  apmd/       # Daemon, IPC server, installers, PATH helpers, security manager
  amsd/       # AMS daemon + IPC server
  ams/        # AMS module manager + metadata/state helpers
  core/       # Repo parsing, status DB, deb/tar extractors, downloader
  util/       # Filesystem helpers, crypto helpers (SHA256/MD5/GPG verification)
  thirdparty/ # Vendored OpenSSL/BoringSSL headers
```


## Filesystem layout (Android)

`src/core/include/config.hpp` defines the on-device directories:

| Path | Purpose |
| ---- | ------- |
| `/data/apm/installed` | Root for repo/manual payloads (contains `commands/`, `dependencies/`, `termux/`) |
| `/data/apm/installed/commands` | Root packages installed from repos + manual `.deb` installs |
| `/data/apm/installed/dependencies` | Dependency payloads installed alongside root packages |
| `/data/apm/installed/termux` | Termux compatibility tree (payloads under `usr/`, wrappers in `/data/apm/bin`) |
| `/data/apm/bin` | PATH shims, Termux wrappers, and bundled binaries (Magisk installs) |
| `/data/apm/cache`, `/data/apm/pkgs` | Temp downloads and cached `.deb` artifacts |
| `/data/apm/lists` | Cached `Release`/`Packages` indices |
| `/data/apm/status` | dpkg-style status database |
| `/data/apm/sources` | `sources.list` plus `sources.list.d/*.list` |
| `/data/apm/manual-packages` | Manual package manifests for `package-install` |
| `/data/apm/keys` | Trusted keyrings for Release/.deb verification (`.asc` or `.gpg`) |
| `/data/apm/.security` | Security material (`masterkey.bin`, `passpin.bin`, `session.bin`, `security-questions.bin`, `reset-lockout.txt`) |
| `/data/apm/apmd.sock` | Default apmd UNIX socket |
| `/data/apm/logs` | `apmd` logs |
| `/data/ams` | AMS root (modules, logs, runtime) |
| `/data/ams/modules` | Installed AMS modules |
| `/data/ams/logs` | Module logs (`<module>.log`) |
| `/data/ams/.runtime` | OverlayFS upper/work/base directories |
| `/dev/socket/amsd` | AMSD IPC socket |

> AMSD safe-mode files are currently written under `/ams` (`.amsd_boot_counter`, `.amsd_safe_mode`, `.amsd_safe_mode_threshold`); deployment scripts create `/ams` alongside `/data/ams`.

> Emulator mode rewrites APM paths under `$HOME/APMEmulator/data/apm` and AMS paths under `$HOME/APMEmulator/ams` with sockets `apmd.socket` and `amsd.socket`.

## Building

### Dependencies

APM is a CMake/C++17 project. If zlib or libcurl are missing, the build can fetch zlib 1.3.1 plus curl 8.7.1 with mbedTLS 3.6.0 (pass `-DAPM_USE_SYSTEM_CURL=OFF` to force the bundled stack).

Needed tools:
- CMake ≥ 3.14
- C++17 compiler + make/ninja
- git + patch (FetchContent + bundled patches)
- `zlib` headers; `libcurl` headers if you want to link against the system copy
- OpenSSL/`libssl` headers (TLS + crypto for host builds; Android builds link against BoringSSL)

Distro-friendly install hints:
- **Ubuntu/Debian:** `sudo apt update && sudo apt install build-essential cmake pkg-config git patch zlib1g-dev libcurl4-openssl-dev libssl-dev google-android-tools-installer sdkmanager`
- **Fedora/RHEL:** `sudo dnf groupinstall "Development Tools" && sudo dnf install cmake git patch zlib-devel libcurl-devel openssl-devel`
- **Arch/Manjaro:** `sudo pacman -S --needed base-devel cmake git patch zlib curl openssl`
- **openSUSE:** `sudo zypper install -t pattern devel_C_C++ && sudo zypper install cmake git patch zlib-devel libcurl-devel libopenssl-devel`
> The `Ubuntu/Debian` packages installation method has been tested, and it is not guaranteed that the other methods will work.

If you skip the curl/zlib dev packages, CMake will transparently build the bundled versions. `APM_USE_SYSTEM_CURL` defaults to ON for host builds and OFF for Android builds.

### Emulator Mode (Host / x86_64)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAPM_EMULATOR_MODE=ON
cmake --build build -j$(nproc)
```

Outputs under `build/`:

| Binary | Description |
| ------ | ----------- |
| `apm`  | CLI (auto-detects emulator socket). |
| `apmd` | Emulator daemon (run with `--emulator`). |
| `amsd` | Emulator AMS daemon (run with `--emulator`). |

Notes:
- Emulator roots live under `$HOME/APMEmulator/data/apm` (APM) and `$HOME/APMEmulator/ams` (AMS).
- `apmd` writes `apmd-emulator.log` under the emulator logs directory.
- `apm` auto-detects emulator mode by checking for `apmd.socket`.

### Build for Android

Use the helper script to target a specific ABI with the NDK toolchain:

```bash
./build_android.sh        # prompts for ABI and API level (default API 34)
```

Notes:
- API level must be ≥ 29 (script enforces this).
- Script looks for `ANDROID_NDK_ROOT` (or `ANDROID_NDK_HOME`), otherwise uses the latest NDK under `$ANDROID_SDK_ROOT/ndk/` (default `/opt/android-sdk`).
- The "x86_64 (Emulator Mode)" option builds a host emulator binary without the NDK (`-DAPM_EMULATOR_MODE=ON`).
- Outputs land in `build/` just like the host build.

### Build inside AOSP (Soong)

APM ships a Soong blueprint for platform builds.

1. Place this tree at `system/apm/` in your AOSP checkout.
2. Add `apm`, `apmd`, and `amsd` to your product's `PRODUCT_PACKAGES`, then build with `m apm apmd amsd`.
3. Use the init scripts in `apm-flashable/system/etc/init/` (or the Magisk variants) when wiring services. The system-wide `init.apmd.rc` waits for the `amsd.ready` property before starting `apmd`.


## Deploying on-device

### Magisk Module (Recommended)

1. Build the Magisk module:
   ```bash
   ./build_android.sh  # Compile apm, apmd, amsd binaries
   cd apm-magisk
   ```
2. Install on your device:
   - **Option A:** Flash the module via Magisk Manager (recommended).
   - **Option B:** Manual install: `adb push apm-magisk /data/adb/modules/apm && adb shell killall magiskd && adb shell sleep 2 && adb reboot`
3. After boot, daemons start automatically. Verify: `adb shell ps | grep -E 'apmd|amsd'`

**Why Magisk?** The module handles SELinux policy application, filesystem overlays, and daemon lifecycle without requiring custom ROMs or recovery flashing. It survives OTA updates and avoids boot-time SELinux policy conflicts.

### Recovery Flashable ZIP (Deprecated/Experimental)

> **Note:** Flashable ZIP deployments are currently deprioritized due to SELinux policy conflicts on boot. Use the Magisk module instead.

Legacy instructions (if recovery flashing is needed):

1. Push `apm`, `apmd`, and `amsd` (e.g., to `/data/local/tmp`).
2. Initialize the layout once: `su -c 'mkdir -p /data/apm/{installed,pkgs,lists,cache,logs,sources,sources.list.d,keys,manual-packages} /data/ams/{modules,logs,.runtime}'`.
3. Start the daemons as root (start `amsd` before `apmd`): `su -c "/data/local/tmp/amsd"` then `su -c "/data/local/tmp/apmd"`.
4. Run CLI commands; `apm` defaults to `/data/apm/apmd.sock`.


## Repositories

APM consumes standard `deb` lines. Place entries in `/data/apm/sources/sources.list` or `/data/apm/sources/sources.list.d/*.list`.

Example:

```
deb [arch=arm64] https://deb.debian.org/debian bookworm main contrib non-free
deb https://packages.termux.dev/apt/termux-main stable main
deb [trusted=yes] https://packages.termux.dev/apt/termux-main/ stable main
```

Notes:

- Termux repos are detected automatically; Debian arches are mapped to Termux equivalents.
- `apm update` downloads `InRelease` first (clear-signed); if missing, it falls back to `Release` + `Release.gpg`.
- `Packages.xz` indices are intentionally skipped on Android; use `Packages.gz` or `Packages`.
- Release metadata is verified against trusted keys in `/data/apm/keys` (`.asc` or `.gpg`, including inline `InRelease` signatures).
  - `[trusted=yes]` skips Release signature verification for that repo.
  - `[trusted=required]` enforces Release verification; the source is skipped if the signature or trusted key is missing.
  - No `trusted` option defaults to verification, but missing/invalid signatures only warn and continue unverified.
- Packages and `.deb` payloads are hash-verified (SHA256 preferred, MD5 fallback from repo metadata).
- Package-level signatures:
  - `[deb-signatures=required]` enforces GPG verification for each `.deb` using detached signatures (`.asc` preferred, `.gpg` fallback). Missing or invalid signatures abort installation.
  - `[deb-signatures=optional]` attempts verification when a signature is available; installation proceeds if verification fails or no signature exists.
  - `[deb-signatures=disabled]` skips package-level verification (default).
  - Verified signatures are cached in `PKGS_DIR/sig-cache.json`, keyed by the `.deb` file’s SHA256. Cache entries include signature type, source, verifier fingerprint (if available), and local path.
  - Trusted keys must be present in `apm::config::TRUSTED_KEYS_DIR` (ASCII-armored `.asc` or binary `.gpg`). Use `apm key-add <key.asc|.gpg>` to import.
- Set `APM_CAINFO=/path/to/cacert.pem` to point curl at a custom CA bundle; otherwise the downloader tries common Android/Linux locations or builds a bundle from `/system/etc/security/cacerts`.


## CLI overview

Most commands hit the daemon; `list`, `info`, and `search` operate offline.

| Command | Summary |
| ------- | ------- |
| `apm ping` | Ping AMSD on `/dev/socket/amsd` (module daemon). |
| `apm update` | Refresh repo metadata with per-stage progress events. |
| `apm install <pkg>` | Resolve direct dependencies (first alternative only), show an install plan, prompt for confirmation, then install. |
| `apm remove <pkg>` | Removes manual packages locally first; otherwise asks the daemon, which protects reverse dependencies unless forced (force flag is future work). |
| `apm upgrade [pkg ...]` | Upgrade all installed packages or a subset; uses the same resolver and installer as `install`. |
| `apm autoremove` | Remove packages marked `Auto-Installed` in the status DB. |
| `apm factory-reset` | Wipe installed commands/deps, credentials, repo lists, AMS modules, and system app overlays (prompts before running). |
| `apm forgot-password` | Recover access with stored security questions and issue a fresh session token. |
| `apm list` | Print installed packages from `/data/apm/status`. |
| `apm info <pkg>` | Show installed metadata plus the candidate version from cached indices. |
| `apm search <pattern ...>` | Case-insensitive search across cached repo descriptions. |
| `apm package-install <file>` | Local-only install of `.deb` or tarballs (`.tar`, `.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.gz`, `.xz`). |
| `apm key-add <key.asc|.gpg>` | Import a trusted GPG key into `/data/apm/keys` for Release/.deb verification. |
| `apm sig-cache show|clear` | Inspect or clear the cached `.deb` signature verification results. |
| `apm apk-install <apk> [--install-as-system]` | Ask the daemon to install an APK via `pm install -r` or stage it as a system app overlay under `/data/adb/modules/apm-system-apps`. |
| `apm apk-uninstall <pkg>` | Uninstall via `pm uninstall`, fall back to `--user 0`, then clean any system overlay. |
| `apm module-list` | List AMS modules and status. |
| `apm module-install <zip>` | Install an AMS module ZIP. |
| `apm module-enable/disable/remove <name>` | Toggle or remove modules and rebuild overlays. |
| `apm version` | Show APM version and build date. |


## Security

- The first privileged command (e.g., `apm update`, `apm install`, module/APK operations) prompts you to set an APM password/PIN plus three recovery questions.
- Secrets are protected with AES-256-GCM using a randomly generated master key stored at `/data/apm/.security/masterkey.bin`; the password/PIN is salted, stretched with PBKDF2 (200k iterations), and encrypted into `/data/apm/.security/passpin.bin`.
- Security questions are stored (salted + derived) under `/data/apm/.security/security-questions.bin`.
- Successful authentication starts a 3-minute session recorded at `/data/apm/.security/session.bin`; AMSD validates the same session token for module requests.
- `apm forgot-password` performs a security-question challenge before issuing a new session; reset attempts are rate-limited (5-minute cooldown recorded in `/data/apm/.security/reset-lockout.txt`).
- Session HMACs and ciphertexts are generated with BoringSSL; there is no hardware-keystore dependency.


## Manual packages

`apm package-install` supports two manual formats:

- **`.deb`**: Uses the embedded control file to read `Package`/`Version`, extracts the data tarball, and installs into `/data/apm/installed/commands/<pkg>`.
- **Tarballs** (`.tar`, `.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.gz`, `.xz`): Must contain a `package-info.json` at the archive root (or one nested directory). The `prefix` in `package-info.json` must live under `/data/apm/installed/<pkg>`.

Extraction shells out to the system `tar` binary; ensure it is available on-device.

Example `package-info.json`:

```json
{
  "package": "fastfetch-gz",
  "version": "1.0.0",
  "prefix": "/data/apm/installed/fastfetch-gz",
  "installed_files": ["bin/fastfetch"]
}
```

APM records manual manifests under `/data/apm/manual-packages/<pkg>.json`. `apm remove` replays that manifest before falling back to daemon removal. Manual names cannot collide with repo-managed packages.


## APK workflows

- User apps: the daemon runs `pm install -r <apk>`.
- System apps: `--install-as-system` stages the APK as `system/app/<name>/base.apk` inside the Magisk module `/data/adb/modules/apm-system-apps`, fixes ownership/permissions, and requires a reboot for Android to register the system app.
- Uninstall: try `pm uninstall`, then `pm uninstall --user 0`, then scrub any staged overlay directory.


## Termux compatibility

If a repo package is marked as Termux (detected via repo URI, payload layout, or dependencies), `apmd` rewrites its payload into `/data/apm/installed/termux/usr`, maintains a manifest of installed files, and drops shim wrappers into `/data/apm/bin` so commands are reachable on PATH. The daemon also writes `/data/apm/installed/termux/env.sh` with `PREFIX`, `LD_LIBRARY_PATH`, `HOME`, and `TMPDIR` defaults.


## PATH integration

`apmd` keeps helper scripts fresh whenever packages change:

- `/data/apm/installed/commands/apm-path.sh` updates PATH with package `bin/` and `usr/bin` directories plus dependency/Termux shims.
- `/data/apm/installed/commands/export-path.sh` writes `/data/local/tmp/.apm_profile`, installs shell hooks in `/data/local/userinit.sh`, `/data/local/tmp/.profile`, `/data/local/tmp/.mkshrc`, and `/data/adb/service.d/99apm-path.sh`, then rehashes the current shell.
- A keepalive loop refreshes PATH hooks every 5 seconds so shells pick up new installs.

In emulator mode, `apmd` generates `$HOME/APMEmulator/data/apm/apm-env.sh`. Source it to pick up new commands.


## AMS (APM Module System)

AMS is a Magisk-style overlay framework baked into `apmd` and executed by `amsd`. It targets rooted devices and uses OverlayFS (no boot image patching) to layer files over `/system`, `/vendor`, and `/product`.

- **Layout:** `/data/ams/modules/<name>/` contains `module-info.json`, `overlay/{system,vendor,product}`, optional `post-fs-data.sh` and `service.sh`, and `workdir/`. Runtime state lives under `/data/ams/.runtime/{upper,work,base}`; per-module logs land in `/data/ams/logs/<name>.log`.
- **Metadata:** `module-info.json` fields: `name`, `version`, `author`, `description`, `mount` (enable overlay), `post_fs_data` (run script after mount), `service` (background script).
- **Lifecycle:** `apm module-install <zip>` extracts the ZIP (single nested dir tolerated), validates metadata, creates workdirs, marks the module enabled, rebuilds overlays, and runs lifecycle scripts. `module-enable/disable/remove` toggle state, rebuild overlays, and persist `state.json`. `module-list` prints module info + state. Boot-counter-based safe mode under `/ams` skips overlays after repeated failures until reset.
- **Overlay rules:** AMS snapshots base mounts for `/system`, `/vendor`, `/product` into `.runtime/base` and layers enabled modules alphabetically (last wins). Overlays use shared upper/work dirs under `.runtime`. If no modules remain, AMS remounts the base mirrors.
- **Packaging:** ZIP contents should be flat (`module-info.json` at root) or inside a single top-level dir. `overlay/` is required even if empty. Create `overlay/{system,vendor,product}`, optional scripts, then `zip -r ../my-module.zip .` from inside the module dir.
- **Tooling:** Module installs shell out to `unzip -oq`. Ensure `unzip` is available on-device.


## Logging & troubleshooting

- CLI log: `apm-cli.log` in the current working directory (errors only).
- Daemon log: `/data/apm/logs/apmd.log` (emulator mode: `apmd-emulator.log` under the emulator logs dir).
- AMSD log: `/data/ams/amsd.log` (Magisk service also captures stdout to `/ams/logs/amsd.log`).
- Module logs: `/data/ams/logs/<module>.log`.
- `apm ping` targets AMSD; use `getprop amsd.ready` to confirm readiness.
- Socket override: `apmd --socket /tmp/custom.sock`, `amsd --socket /tmp/custom.sock` (CLI currently uses the default apmd socket).
- Binder transport: deprecated and unused by default; code remains for reference only.


## Known limitations / roadmap

- Binder transport is disabled at runtime; only the UNIX socket transport is exercised.
- Dependency resolution only follows the first alternative and does not recurse beyond direct dependencies.
- `Packages.xz` indices are ignored on Android builds.
- `apm install` is interactive; non-interactive flags (`--yes`, `--reinstall`, CLI `--simulate`) are not implemented yet.
- `apm remove` has no `--force`/purge flags yet.
- System APK install assumes Magisk-owned `/data/adb/modules`.
- AMS requires a clean base mount snapshot; if `/system` is already overlay-mounted, capture will fail until the device reboots cleanly.
- Secrets are protected in software with a locally stored master key; there is no hardware keystore binding or revocation for imported signing keys.


## Security review

Pre-open-beta security review notes live in `SECURITY_REVIEW.txt`. The review covers `src/` (excluding the vendored OpenSSL headers under `src/thirdparty/`) and summarizes fixed and outstanding items.


## Contributing

Patches are welcome. Follow the existing C++ style, keep comments concise/ASCII, and mention whether your change affects the daemon, CLI, or AMS along with any manual test steps. There are no automated tests yet; please include manual verification notes.


## License

APM is distributed under the GNU General Public License v3.0 (or later). See the source headers for the exact notice.
