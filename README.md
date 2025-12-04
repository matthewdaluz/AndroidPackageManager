# APM — Android Package Manager

APM is a GPLv3-licensed package manager for rooted Android. It mirrors the APT workflow (sources lists, Packages indices, dependency tracking) while adding Android-specific features: a root daemon that performs filesystem changes, CLI helpers that work offline when possible, Termux compatibility, APK install/uninstall helpers, and a Magisk-inspired module system (AMS) for OverlayFS-based system customization. The runtime is IPC-only over UNIX sockets (Binder is deprecated but left in the tree) and now ships a dedicated `amsd` daemon owning `/ams` plus a safe-mode boot counter to avoid overlay boot loops. An emulator mode is available for host-side testing.

> Upgrade note: AMSD builds require factory-resetting previous `/data/apm` installs before flashing. Legacy modules under `/data/apm/modules` will block startup; remove them or perform a full reset first. The current CLI/daemon release is **APM 1.8.0b (closed beta)**.


## Components

| Piece | Role |
| ----- | ---- |
| `apm` | User-facing CLI. Talks to the daemon over `/data/apm/apmd.sock` (IPC-only), renders progress, and runs local-only queries (`list`, `info`, `search`, manual installs). |
| `apmd` | Root daemon. Downloads repo metadata, installs/upgrades/removes packages, maintains PATH helper scripts, performs package/APK verification, and handles APK/system-overlay work. |
| `amsd` | AMS daemon. Owns the `/ams` hierarchy, applies OverlayFS layers for enabled modules, tracks safe-mode boot counters (threshold file-backed under `/ams`), and exposes module-only IPC on `/dev/socket/amsd`. |
| AMS | Built-in module system similar to Magisk. Modules live under `/ams/modules`, carry metadata + overlay payloads, and are mounted via OverlayFS by `amsd` at startup or when partitions appear. |
| Core | Shared plumbing for repo parsing, status DB, dependency resolution, tar/deb extraction, and download helpers (curl + zlib). |

Repository layout:

```
src/
  apm/        # CLI + commands + IPC client
  apmd/       # Daemon, IPC server, installers, PATH helpers, security manager
  ams/        # AMS module manager + metadata/state helpers
  core/       # Repo parsing, status DB, deb/tar extractors, downloader
  util/       # Filesystem helpers, crypto helpers (SHA256/MD5/GPG verification)
```


## Filesystem layout (Android)

`src/core/include/config.hpp` defines the on-device directories:

| Path | Purpose |
| ---- | ------- |
| `/data/apm/installed` | Package payloads unpacked from `.deb` and manual archives |
| `/data/apm/installed/commands` | Root for CLI-exposed commands + PATH helper scripts |
| `/data/apm/installed/dependencies` | Dependency payloads installed alongside root packages |
| `/data/apm/installed/termux` | Termux compatibility tree (wrappers land in `/data/apm/bin`) |
| `/data/apm/cache`, `/data/apm/pkgs` | Temp downloads and cached `.deb` artifacts |
| `/data/apm/lists` | Cached `Release`/`Packages` indices |
| `/data/apm/status` | dpkg-style status database |
| `/data/apm/sources` | `sources.list` plus `sources.list.d/*.list` |
| `/ams` | AMS runtime root owned by `amsd` (modules + overlays + logs) |
| `/ams/modules` | Installed AMS modules and their per-module logs |
| `/ams/.runtime` | OverlayFS work/base/upper areas owned by `amsd` |
| `/ams/.amsd_boot_counter`, `/ams/.amsd_safe_mode`, `/ams/.amsd_safe_mode_threshold` | Safe-mode counter/flag/threshold files consulted before mounting overlays |
| `/data/apm/keys` | Trusted keyrings for Release/.deb verification (`.asc` or `.gpg`) |
| `/data/apm/apmd.sock` | Default UNIX socket |
| `/data/apm/logs` | `apm`/`apmd` logs; module logs live under `/data/apm/logs/modules` |
| `/data/apm/modules` | AMS modules; `.runtime` holds OverlayFS upper/work/base dirs |
> `/ams/` is to be changed to `/data/apm/ams/` in v1.9.0b and later due to filesystem issues.

## Building

### Dependencies

APM is a CMake/C++17 project. If zlib or libcurl are missing, the build will fetch zlib 1.3.1 plus curl 8.7.1 with mbedTLS 3.6.0 (pass `-DAPM_USE_SYSTEM_CURL=OFF` to force the bundled stack).

Needed tools:
- CMake ≥ 3.14
- C++17 compiler + make/ninja
- git + patch (FetchContent + bundled patches)
- `zlib` headers; `libcurl` headers if you want to link against the system copy
- OpenSSL/`libssl` headers (TLS for host builds; the Android build can also use NDK BoringSSL)

Distro-friendly install hints:
- **Ubuntu/Debian:** `sudo apt update && sudo apt install build-essential cmake pkg-config git patch zlib1g-dev libcurl4-openssl-dev libssl-dev google-android-tools-installer sdkmanager`
- **Fedora/RHEL:** `sudo dnf groupinstall "Development Tools" && sudo dnf install cmake git patch zlib-devel libcurl-devel openssl-devel`
- **Arch/Manjaro:** `sudo pacman -S --needed base-devel cmake git patch zlib curl openssl`
- **openSUSE:** `sudo zypper install -t pattern devel_C_C++ && sudo zypper install cmake git patch zlib-devel libcurl-devel libopenssl-devel`
> The `Ubuntu/Debian` packages installation method has been tested, and it is not guaranteed that the other methods will work.

If you skip the curl/zlib dev packages, CMake will transparently build the bundled versions.

### Build on Linux (host) (Not recommended)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Outputs under `build/`:

| Binary | Description |
| ------ | ----------- |
| `apm`  | CLI (can run unprivileged if it can reach the daemon socket). |
| `apmd` | Root daemon that writes under `/data/apm`. |
| `amsd` | Daemon for AMS. |

### Build for Android

Use the helper script to target a specific ABI with the NDK toolchain:

```bash
./build_android.sh        # prompts for ABI, uses $ANDROID_NDK, ~/Android/NDK, or /opt/android-sdk
```

Notes:
- Script expects NDK r27d at `$ANDROID_NDK`, `$ANDROID_NDK_HOME`, or `~/Android/NDK`. When configuring manually with `-DANDROID=ON`, CMake will attempt to download r27d if it cannot find one.
- Outputs land in `build/` just like the host build.

### Build inside AOSP (Soong)

APM ships a Soong blueprint for Android 15 platform builds.

1. Place this tree at `system/apm/` in your AOSP checkout.
2. Add `apm` and `apmd` to your product's `PRODUCT_PACKAGES` (for example in `device.mk`), then build with `m apm apmd` or as part of a full platform build.
3. Soong installs both binaries into `/system/bin/` and drops `init.apmd.rc` into `/system/etc/init/`.
4. The bundled init script waits for `/data`, provisions `/data/apm` subdirectories, and starts `apmd` as root. Transport is IPC-only.


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

1. Push `apm` and `apmd` (e.g., to `/data/local/tmp`).
2. Initialize the layout once: `su -c 'mkdir -p /data/apm/{installed,pkgs,lists,cache,logs,sources,sources.list.d,keys}'`.
3. Start the daemon as root: `su -c "/data/local/tmp/apmd"` (the daemon binds `/data/apm/apmd.sock` by default).
4. Run CLI commands; override the socket with `apm --socket <path> ...` if needed.


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
- `apm update` downloads Release + Packages(.gz) into `/data/apm/lists` and parses them locally; `.xz` indices are intentionally skipped on Android.
- Release metadata is verified against trusted keys in `/data/apm/keys` (`.asc` or `.gpg`, including inline `InRelease` signatures). Missing signatures or keys cause the source to be skipped unless `[trusted=yes]` is set.
- Packages and `.deb` payloads are hash-verified (SHA256 preferred, MD5 fallback from repo metadata). Detached package signatures can be enforced per-source and successful results are cached in `PKGS_DIR/sig-cache.json`.
- Per-source trust overrides:
  - `[trusted=yes]` skips Release signature verification for that repo.
  - `[trusted=required]` enforces Release verification; the source is skipped
    if the signature or trusted key is missing.
  - No `trusted` option defaults to verifying Release signatures.
- Package-level signatures:
  - `[deb-signatures=required]` enforces GPG verification for each `.deb` using detached signatures (`.asc` preferred, `.gpg` fallback). Missing or invalid signatures abort installation.
  - `[deb-signatures=optional]` attempts verification when a signature is available; installation proceeds if verification fails.
  - `[deb-signatures=disabled]` skips package-level verification (default).
  - Verified signatures are cached in `apm`’s package cache directory at `PKGS_DIR/sig-cache.json`, keyed by the `.deb` file’s SHA256. Cache entries include signature type, source, verifier fingerprint (if available), and local path.
  - Trusted keys must be present in `apm::config::TRUSTED_KEYS_DIR` (ASCII-armored `.asc` or binary `.gpg`). Use `apm key-add <key.asc|.gpg>` to import.
- Set `APM_CAINFO=/path/to/cacert.pem` to point curl at a custom CA bundle; otherwise the downloader tries common Android/Linux locations or builds a bundle from `/system/etc/security/cacerts`.


## CLI overview

Most commands hit the daemon; `list`, `info`, and `search` operate offline.

| Command | Summary |
| ------- | ------- |
| `apm ping` | Confirm daemon reachability. |
| `apm update` | Refresh repo metadata with per-stage progress events. |
| `apm install <pkg> [--simulate] [--reinstall]` | Resolve dependencies (first alternative only), download `.deb` files, and install them under `/data/apm/installed`/`dependencies`. Termux packages are detected and rewritten into `/data/apm/installed/termux/usr` with PATH wrappers in `/data/apm/bin`. |
| `apm remove <pkg>` | Removes manual packages locally first; otherwise asks the daemon, which protects reverse dependencies unless forced (force flag is future work). |
| `apm upgrade [pkg ...]` | Upgrade all installed packages or a subset; uses the same resolver and installer as `install`. |
| `apm autoremove` | Remove packages marked `Auto-Installed` in the status DB. |
| `apm factory-reset` | Wipe installed commands/deps, credentials, repo lists, AMS modules, and system app overlays (prompts before running). |
| `apm forgot-password` | Recover access with stored security questions and issue a fresh session token. |
| `apm list` | Print installed packages from `/data/apm/status`. |
| `apm info <pkg>` | Show installed metadata plus the candidate version from cached indices. |
| `apm search <pattern ...>` | Case-insensitive search across cached repo descriptions. |
| `apm package-install <file>` | Local-only install of `.deb` or `.tar.*` archives that contain `package-info.json`; records manifests under `/data/apm/manual-packages`. |
| `apm key-add <key.asc|.gpg>` | Import a trusted GPG key into `/data/apm/keys` for Release/.deb verification. |
| `apm sig-cache show|clear` | Inspect or clear the cached `.deb` signature verification results. |
| `apm apk-install <apk> [--install-as-system]` | Ask the daemon to install an APK via `pm install -r` or stage it as a system app overlay under `/data/adb/modules/apm-system-apps`. |
| `apm apk-uninstall <pkg>` | Uninstall via `pm uninstall`, fall back to `--user 0`, then clean any system overlay. |
| `apm module-list` | List AMS modules and status. |
| `apm module-install <zip>` | Install an AMS module ZIP. |
| `apm module-enable/disable/remove <name>` | Toggle or remove modules and rebuild overlays. |


## Security

- The first privileged command (e.g., `apm update`, `apm install`, module/APK operations) prompts you to set an APM password/PIN plus three recovery questions. Losing it requires successful recovery or a factory reset to clear `passpin.bin`.
- Secrets are protected with AES-256-GCM using a randomly generated master key stored locally at `/data/apm/.security/masterkey.bin`; the password/PIN is salted, stretched with PBKDF2, and encrypted into `/data/apm/.security/passpin.bin`. Session HMACs and ciphertexts are generated with BoringSSL; there is no hardware-keystore dependency.
- Successful authentication starts a 3-minute session recorded at `/data/apm/.security/session.bin`; during that window, privileged commands run without re-entering the password/PIN. `apm forgot-password` performs a security-question challenge before issuing a new session. Reset attempts are rate-limited (cooldown recorded in `/data/apm/.security/reset-lockout.txt`).


## Manual packages

`apm package-install` sideloads archives that ship a `package-info.json`:

```json
{
  "package": "fastfetch-gz",
  "version": "1.0.0",
  "prefix": "/data/apm/installed/fastfetch-gz",
  "installed_files": ["bin/fastfetch"]
}
```

The CLI validates the prefix, unpacks the archive, snapshots every file, and stores metadata under `/data/apm/manual-packages/<pkg>.json`. `apm remove` replays that manifest before falling back to daemon removal. Manual names cannot collide with repo-managed packages.


## APK workflows

- User apps: the daemon runs `pm install -r <apk>`.
- System apps: `--install-as-system` stages the APK as `system/app/<name>/base.apk` inside the Magisk module `/data/adb/modules/apm-system-apps`, fixes ownership/permissions, and requires a reboot for Android to register the system app.
- Uninstall: try `pm uninstall`, then `pm uninstall --user 0`, then scrub any staged overlay directory.


## Termux compatibility

If a repo package is marked as Termux (detected via repo URI, payload layout, or dependencies), `apmd` rewrites its payload into `/data/apm/installed/termux/usr`, maintains a manifest of installed files, and drops shim wrappers into `/data/apm/bin` so commands are reachable on PATH. The daemon also writes `/data/apm/installed/termux/env.sh` and basic HOME/TMP scaffolding.


## PATH integration

`apmd` keeps helper scripts fresh whenever packages change:

- `/data/apm/installed/commands/apm-path.sh` updates PATH with package `bin/` and `usr/bin` directories plus dependency/Termux shims.
- `/data/apm/installed/commands/export-path.sh` writes and sources `/data/local/tmp/.apm_profile`, touches a sourced marker, and rehashes the current shell.

Source `/data/local/tmp/.apm_profile` (or `export-path.sh`) in your shell startup to pick up new commands.


## AMS (APM Module System)

AMS is a Magisk-style overlay framework baked into `apmd`. It targets rooted devices and uses OverlayFS (no boot image patching) to layer files over `/system`, `/vendor`, and `/product`.

- **Layout:** `/data/apm/modules/<name>/` contains `module-info.json`, `overlay/{system,vendor,product}`, optional `post-fs-data.sh` and `service.sh`, `workdir/` (upper/work scratch dirs), and daemon-maintained `state.json`. Runtime state lives under `/data/apm/modules/.runtime/{upper,work,base}`; per-module logs land in `/data/apm/logs/modules/<name>.log`.
- **Metadata:** `module-info.json` fields: `name`, `version`, `author`, `description`, `mount` (enable overlay), `post_fs_data` (run script after mount), `service` (background script). `state.json` tracks `enabled`, ISO8601 timestamps, and `last_error`.
- **Lifecycle:** `apm module-install <zip>` extracts the ZIP (single nested dir tolerated), validates metadata, creates workdirs, marks the module enabled, rebuilds overlays, and runs lifecycle scripts. `module-enable/disable/remove` toggle state, rebuild overlays, and persist `state.json`. `module-list` prints module info + state. Boot-counter-based safe mode under `/ams` skips overlays after repeated failures until reset.
- **Overlay rules:** AMS snapshots base mounts for `/system`, `/vendor`, `/product` into `.runtime/base` and layers enabled modules alphabetically (last wins). Overlays use shared upper/work dirs under `.runtime`. If no modules remain, AMS remounts the base mirrors.
- **Packaging:** ZIP contents should be flat (module-info.json at root). Packaging steps: create `overlay/{system,vendor,product}`, optional scripts, then `zip -r ../my-module.zip .` from inside the module dir.
- **Troubleshooting:** Check `/data/apm/logs/modules/<name>.log`; `module-list` surfaces `Last-Error`. Ensure overlay paths exist and scripts exit cleanly. Alphabetical module order controls override priority.


## Logging & troubleshooting

- CLI log: `apm-cli.log` in the current working directory, mirrored to stderr.
- Daemon log: `/data/apm/logs/apmd.log`. AMSD logs to `/data/ams/logs/amsd.log`; module logs live under `/data/apm/logs/modules/<module>.log`.
- Socket override: `apm --socket /tmp/custom.sock ...` / `apmd --socket /tmp/custom.sock`.
- Binder transport: deprecated and unused by default; code remains for reference only.


## Known limitations / roadmap

- Binder transport is disabled at runtime; only the UNIX socket transport is exercised.
- Dependency resolution only follows the first alternative and goes one level deep.
- `Packages.xz` indices are ignored on Android builds.
- System APK install assumes Magisk-owned `/data/adb/modules`.
- AMS requires a clean base mount snapshot; if `/system` is already overlay-mounted, capture will fail until the device reboots cleanly.
- Secrets are protected in software with a locally stored master key; there is no hardware keystore binding or revocation for imported signing keys.


## Contributing

Patches are welcome. Follow the existing C++ style, keep comments concise/ASCII, and mention whether your change affects the daemon, CLI, or AMS along with any manual test steps.


## License

APM is distributed under the GNU General Public License v3.0 (or later). See the source headers for the exact notice.
