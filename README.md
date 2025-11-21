# APM — Android Package Manager

APM is a GPLv3-licensed package manager tailored for rooted Android environments. It brings an APT-like workflow to `/data/apm`, making it easy to pull Debian/Termux repositories, install `.deb` payloads with dependency tracking, manage standalone tarballs, and even stage APKs as user or system apps through a long-running daemon.


## Highlights

- **Client/Daemon split:** The `apm` CLI focuses on UX (interactive planning, progress bars, quick local queries) while `apmd` handles privileged filesystem work over a UNIX socket (`/data/apm/apmd.sock`).
- **Repository support:** Parses traditional `deb ...` `sources.list` entries plus Termux style repos, downloads `Release`/`Packages.gz` pairs, and caches them under `/data/apm/lists`.
- **Package orchestration:** Installs `.deb` archives into `/data/apm/installed/<pkg>`, records dpkg-style status metadata, enforces simple reverse-dependency rules, and can upgrade or autoremove packages.
- **Manual payloads:** `apm package-install` understands standalone `.deb` or `.tar.*` drops that include a `package-info.json`, records every installed file, and lets `apm remove` clean them up without daemon help.
- **APK workflows:** `apk-install` forwards to the daemon which either runs `pm install -r` (user app) or stages overlays under `/data/adb/modules/apm-system-apps` for Magisk-based system installs, plus matching uninstall support.
- **Environment helpers:** `apmd` keeps `/data/apm/installed/commands` in sync, writes `apm-path.sh`/`export-path.sh`, and maintains `/data/local/tmp/.apm_profile` so shells can `source` a single file to pick up newly installed binaries.
- **Portable build:** Standard CMake ≥3.14 project, C++17, libcurl, zlib. When system libcurl+TLS aren’t available, the build pulls curl 8.7.1 + mbedTLS 3.6.0 automatically.


## Repository Layout

```
src/
  apm/        # CLI, IPC client, human-facing commands
  apmd/       # Daemon, IPC server, install/upgrade/remove, APK helper
  core/       # Shared plumbing (repo parsing, status DB, dependency logic…)
  util/       # Filesystem helpers and crypto stubs (SHA-256, GPG hook)
cmake/        # FetchContent + patch logic for bundled curl/mbedtls
```

The Android-facing filesystem layout is described in `src/core/include/config.hpp`:

| Path | Purpose |
| ---- | ------- |
| `/data/apm/installed` | Package payloads unpacked from `.deb` and manual archives |
| `/data/apm/installed/commands` | Symlinks/scripts exposed on `$PATH` |
| `/data/apm/cache`, `/data/apm/pkgs` | Temporary downloads and cached `.deb` artifacts |
| `/data/apm/lists` | Cached `Release`/`Packages` indices |
| `/data/apm/status` | dpkg-style status database |
| `/data/apm/sources` | `sources.list` plus `sources.list.d/*.list` |
| `/data/apm/keys` | Placeholder for trusted keyrings (GPG check is stubbed today) |
| `/data/apm/apmd.sock` | Default UNIX socket used by the CLI |
| `/data/apm/logs` | `apm`/`apmd` log files |


## Building

Requirements:

- CMake ≥ 3.14
- A C++17 toolchain
- zlib development headers
- libcurl development headers (optional: set `-DAPM_USE_SYSTEM_CURL=OFF` to always build the bundled one)

Build steps:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

The build produces two binaries under `build/`:

| Binary | Description |
| ------ | ----------- |
| `apm`  | User-facing CLI (can run unprivileged as long as it can reach the daemon socket). |
| `apmd` | Daemon that must run as root/rooted shell so it can write under `/data/apm`. |

> **Tip:** Set `-DAPM_USE_SYSTEM_CURL=ON` if your build environment already ships libcurl + TLS. Otherwise, CMake uses `FetchContent` to pull curl + mbedTLS and applies the patches in `cmake/patches/`.


## Deploying on-device

1. Copy `apm` and `apmd` to your device (e.g., `/data/local/tmp`).
2. Create the runtime directories once: `su -c 'mkdir -p /data/apm/{installed,pkgs,lists,cache,logs,sources,sources.list.d,keys}'`.
3. Start the daemon as root: `su -c "/data/local/tmp/apmd --socket /data/apm/apmd.sock"`. Keep it running (e.g., via `nohup` or an init script).
4. From any shell, call the CLI. By default it talks to `/data/apm/apmd.sock`; override with `apm --socket <path> ...` if needed.


## Configuring repositories

APM reuses APT-style `sources.list` files. Place entries under `/data/apm/sources/sources.list` or drop individual `.list` files under `/data/apm/sources/sources.list.d/`.

Example (`/data/apm/sources/sources.list`):

```
deb [arch=arm64] https://deb.debian.org/debian bookworm main contrib non-free
deb https://packages.termux.dev/apt/termux-main stable main
```

Notes:

- `arch=` overrides the default (`arm64`); Termux repos are detected automatically and the code maps Debian architectures to Termux equivalents.
- After editing your sources, run `apm update` to fetch Release/Packages metadata. Only `Packages` and `Packages.gz` are supported on-device; `.xz` indices are skipped on Android to avoid bundling liblzma.
- Trusted keyrings should live in `/data/apm/keys`, but **Release GPG verification is currently stubbed out** (`src/util/crypto/gpg_verify.cpp`). SHA256 verification of Release/Packages contents is structured for future enablement.
- Set `APM_CAINFO=/path/to/cacert.pem` if you need a custom CA bundle for libcurl.


## Using the CLI

```
apm [--socket /custom/apmd.sock] <command> [args]
```

Most commands talk to the daemon; `list`, `info`, and `search` are local/offline operations. Key workflows:

| Command | Summary |
| ------- | ------- |
| `apm ping` | Round-trip ping to confirm the daemon socket responds. |
| `apm update` | Download Release/Packages metadata, showing per-stage progress bars. |
| `apm install <pkg>` | Two-phase install: 1) simulated plan (lists packages to install) + confirmation, 2) actual download/install with live progress per file. |
| `apm remove <pkg>` | Removes manual packages locally before falling back to daemon removal (with reverse dependency checks). |
| `apm upgrade [pkg ...]` | Upgrade everything in the status DB or a provided subset. |
| `apm autoremove` | Removes packages that were marked `autoInstalled`. |
| `apm list` | List installed packages straight from `/data/apm/status`. |
| `apm info <pkg>` | Show installed metadata plus the candidate version from cached repo indices. |
| `apm search <pattern ...>` | Case-insensitive search across cached repo descriptions. |
| `apm package-install <file>` | Install a standalone `.deb` or `.tar.*` archive that ships a `package-info.json`. |
| `apm apk-install <apk> [--install-as-system]` | Forward APK installs to the daemon (system installs require root/Magisk). |
| `apm apk-uninstall <package>` | Remove an APK via `pm uninstall` (with `--user 0` fallback) plus Magisk overlay cleanup. |


## Manual packages

`apm package-install` lets you sideload content that is not present in any configured repo. The archive must contain a `package-info.json` descriptor similar to:

```json
{
  "package": "fastfetch-gz",
  "version": "1.0.0",
  "prefix": "/data/apm/installed/fastfetch-gz",
  "installed_files": ["bin/fastfetch"]
}
```

Workflow:

1. Run `apm package-install <path/to/file.[deb|tar.gz|tar.xz|tar]>`.
2. The CLI extracts the archive locally, validates that the declared `prefix` lives under `/data/apm/installed`, moves the files into place, snapshots every installed file, and records the JSON under `/data/apm/manual-packages/<package>.json`.
3. Later, `apm remove <package>` first checks the manual package registry. If present, it replays the recorded manifest to delete the files and metadata without involving the daemon.

Manual installs cannot overlap with a repo-managed package (the CLI refuses to install if the name already exists in the status DB).


## APK management

The daemon module in `src/apmd/apk_installer.cpp` supports both user and system installs:

- Default: calls `pm install -r <apk>`.
- `--install-as-system`: requires root. The daemon prepares `/data/adb/modules/apm-system-apps`, copies the APK as `system/app/<name>/base.apk`, sets ownership/permissions, and logs a reminder that a reboot is required for Android to register the system app.
- `apm apk-uninstall <pkg>` first tries `pm uninstall <pkg>`, then `pm uninstall --user 0 <pkg>`, and finally scrubs any Magisk overlay directory.


## PATH integration

`apmd` maintains helper scripts so that shells can automatically discover commands installed under `/data/apm/installed/commands`:

- `/data/apm/installed/commands/apm-path.sh`: Adds the global commands directory plus each package’s `bin/` and `usr/bin/` to `$PATH`.
- `/data/apm/installed/commands/export-path.sh`: Writes `/data/local/tmp/.apm_profile`, marks when it was sourced, and refreshes the current shell (runs `hash -r` for busybox/ash compatibility).

Usage:

```sh
# Source the profile once per shell
. /data/apm/installed/commands/export-path.sh

# Or, if the profile file exists
. /data/local/tmp/.apm_profile
```

Whenever packages are installed/removed, the daemon refreshes these scripts so they remain current.


## Logging & troubleshooting

- CLI logs: `apm` writes `apm-cli.log` (current working directory) and mirrors logs to stderr when run interactively.
- Daemon logs: `/data/apm/logs/apmd.log`.
- curl CA bundle: set `APM_CAINFO=/path/to/cacert.pem` or drop PEM files under `/system/etc/security/cacerts` and the downloader will build a bundle automatically.
- Socket overrides: use `apm --socket /tmp/custom.sock ...` and `apmd --socket /tmp/custom.sock` if `/data/apm/apmd.sock` is not suitable.


## Known limitations / roadmap

- **GPG verification is disabled** (`src/util/crypto/gpg_verify.cpp`). Release signatures are currently accepted without verification; SHA256 plumbing exists but is not enforced. Only use trusted mirrors until verification is restored.
- Dependency resolution considers *only the first alternative* in each `Depends:` stanza and resolves one level deep. Complex dependency graphs may require manual installs for missing pieces.
- `Packages.xz` indices are rejected on Android builds to avoid shipping extra decompressors; ensure your repos provide `Packages` or `Packages.gz`.
- System APK installs assume Magisk is managing `/data/adb/modules`; other overlay mechanisms are not yet supported.
- There is no SELinux policy installer; devices in enforcing mode must already allow `/data/apm` modifications.


## Contributing

Issues and pull requests are welcome. When sending patches:

1. Follow the project’s existing C++ style (see headers in `src/` for guidance).
2. Keep comments concise and prefer ASCII text.
3. Mention whether your change affects daemon/CLI, and include any manual test steps.


## License

APM is distributed under the GNU General Public License v3.0 (or later). See the headers throughout `src/` for the precise notice.

