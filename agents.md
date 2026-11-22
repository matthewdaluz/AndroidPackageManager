# Android Package Manager contributor guide

APM is a C++17 package manager for Android with a Linux emulator mode. It ships three binaries. `apm` is the CLI, `apmd` handles package operations, and `amsd` applies Android Module System (AMS) overlays and runs module hooks. The project is GPL-3.0-or-later.

## Before editing

- Read each affected area before editing, including callers, headers, build definitions, runtime scripts, and relevant docs. The existing maintainer profile is in [`.github/agents/apm-code-maintainer.agent.md`](.github/agents/apm-code-maintainer.agent.md).
- Follow the surrounding C++ and shell style. Reuse the config, filesystem, logging, and protocol helpers. Preserve existing CLI behavior, on-disk formats, and upgrade paths unless the change explicitly revises them.
- Update both `CMakeLists.txt` and `Android.bp` when source files, include paths, or link dependencies change. Several files under `src/apmd` also build into the CLI or `amsd`. Check each consumer of shared code.
- Use the implementation and tracked examples to resolve discrepancies in the documentation. Start with [README.md](README.md) and [the wiki index](docs/wiki/Home.md). Check [CLI and operations](docs/wiki/CLI-and-Operations.md) when changing a command and [app repositories](docs/wiki/App-Repositories.md) when changing bundle behavior.

## Source map

| Area | Start here |
| --- | --- |
| CLI | `src/apm/apm.cpp` contains dispatch, help, authentication prompts, progress rendering, and local commands. Also see `commands/search.cpp`, `transport.cpp`, and `ipc_client.cpp` in the same tree. |
| Package daemon | `src/apmd/apmd.cpp` initializes runtime state and handles Android startup migrations. `ipc_server.cpp` handles authorization, request dispatch, and repository cache coordination. `install_manager.cpp` manages package lifecycles, `export_path.cpp` generates shell shims, and `factory_reset.cpp` implements reset and scoped cache wipes. |
| APKs and app bundles | `src/apmd/apk_installer.cpp` handles direct APK operations. `app_installer.cpp` stages repository bundles and `apk_metadata.cpp` reads binary APK manifests. `src/core/app_repo.cpp` parses definitions and selects GitHub release assets. |
| AMS | `src/amsd/amsd.cpp` coordinates boot, safe mode, and service readiness. Its `ipc_server.cpp`, `request_dispatcher.cpp`, and `security_manager.cpp` handle AMS socket requests. `src/ams/module_manager.cpp` owns ZIP installation, overlay backends, lifecycle hooks, and module state. `module_info.cpp` parses module metadata. |
| Shared core | `src/core/` holds runtime configuration and logging, source and index handling, downloads, dependency resolution, status and manual package metadata, archive handling, and shared session state. Public interfaces are under `src/core/include/`. |
| Utilities | `src/util/` contains filesystem helpers, daemon process locking, SHA-256 and MD5 digest helpers, and OpenPGP verification and key import under `crypto/`. |
| Repository examples | `Repositories/PackageRepos/` contains Termux `.repo` examples. `Repositories/AppRepos/` contains the MicroG `.apprepo` example. |

## Runtime contracts

- Resolve paths through `src/core/config.cpp` and its header. Android keeps persistent APM state in `/data/apm`, shell-facing installs and shims in `/data/local/tmp/apm`, and AMS state in `/data/ams`. The Linux emulator uses `$HOME/APMEmulator/data/apm` and `$HOME/APMEmulator/ams`. For layout changes, check startup migration, shell permissions and labels, and the reset and cache-wipe scopes.
- IPC uses UNIX sockets. On Android, `apmd` listens at abstract socket `@apmd` and `amsd` listens at `/data/ams/amsd.sock`. The emulator uses `apmd.socket` and `amsd.socket` in the corresponding roots. The CLI defaults to `apmd`, including for its module commands. AMS also exposes module operations through its own dispatcher.
- For IPC changes, trace the CLI call, the shared wire format in `src/apmd/include/protocol.hpp` and `src/apmd/protocol.cpp`, and the relevant daemon handler. Preserve framing, escaping, response IDs, progress events, and final response handling. Check authorization at the daemon boundary. `apmd` issues sessions and both daemons validate them using shared state. Keep tokens, PINs, and recovery answers out of logs.
- `src/apmd/export_path.cpp` produces the command index, shims, and shell environment files. For package or runtime path changes, check shim rebuilds, collisions with system commands and other packages, and the Termux environment. Coordinate shell startup changes with the flashable hooks `apm-sh-path` and `apm-bash-path`.

## Packages, APKs, and modules

- Debian and Termux sources use `.repo` files, while APK bundles use `.apprepo` files in the same sources directory. Follow `repo_index.cpp` and `release_parser.cpp` for Release and Packages metadata. Keep Release signature policy (`Trusted`), index and payload checksum checks, and detached package signature policy (`Deb-Signatures`) intact. Android repository indexes use `Packages.gz` or plain `Packages`. Individual package payloads support additional compression formats.
- Repository installs use dependency resolution and the dpkg-style status database. Termux has its own runtime prefix, dependency handling, and wrappers. Local `package-install` goes through the CLI and manual package metadata. Tar-style manual packages use `package-info.json` and a validated install prefix. Trace install, remove, and upgrade paths when changing persisted state or ownership of installed files.
- Both local and daemon-backed `.deb` installation use the shared `deb_extractor.cpp`, `archive_decompressor.cpp`, and `tar_extractor.cpp`. The code parses ar containers and uses native gzip, XZ/LZMA, bzip2, and Zstandard decoders before invoking `tar`. Preserve input validation, archive path and link checks, decompression limits, and temporary-file cleanup.
- Direct APK installs use Android `pm` or stage into the AMS `apm-system-apps` module. App repository bundles stage into `apm-app-<id>` modules and use separate status keys. Preserve plan confirmation, downloaded size and SHA-256 verification, manifest-derived package IDs, permission validation, and rollback of module and status writes. System app changes take effect after reboot.
- AMS modules use `module-info.json`, `state.json`, and `overlay/{system,vendor,product}`. Check both daemons when changing module lifecycle behavior, and check safe mode, boot readiness, overlay application, and hook ordering when changing startup behavior. See [AMS module development](docs/wiki/AMS-Module-Development.md) and [the AMS architecture](docs/wiki/AMS-Architecture.md).

## Build and validation

Standalone builds use CMake 3.24 or newer and Ninja. CMake uses static BoringSSL archives and headers under `prebuilt/boringssl/`, builds bundled curl, and fetches archive codecs through `cmake/ArchiveCompression.cmake`. Android needs matching NDK archives under `build-<abi>/` inside the prebuilt tree. A Linux emulator build needs native Linux archives under `build-x86_64/` or `build-x86/` inside that tree. Verify the target platform when staging archives because Android x86 prebuilts use the same directory names.

For an Android NDK build, use the wrapper with an explicit ABI. It defaults to API 34 and accepts API 29 or newer. `--all` covers `arm64-v8a`, `armeabi-v7a`, `x86_64`, and `x86`. The wrapper recreates the selected build directory and publishes the compilation database at the repository root.

```sh
./build_android.sh --abi arm64-v8a
```

For Linux emulator checks, configure a separate build directory to preserve the existing build cache. Start `apmd` and `amsd` with `--emulator` and use an isolated home directory shared by the daemons and CLI. Once `apmd` has created its socket, the emulator-built CLI detects it automatically. Use `apm --help` and `apm ping` as smoke checks.

```sh
cmake -S . -B build-emulator -G Ninja -DAPM_EMULATOR_MODE=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-emulator --parallel
```

- The default build leaves `APM_BUILD_TESTS` off. The current checkout lacks the `tests/` sources and Python fixtures referenced by CMake's archive and app repository regression targets. With those files present, configure with `-DAPM_BUILD_TESTS=ON`, build, and run `ctest --test-dir <build-dir> --output-on-failure`.
- For shell changes, run `bash -n` on host build scripts and `sh -n` on Android/recovery scripts. Check the two `addon.d` fragments together because the installer generates one script from them.
- Validate Android-only behavior on a suitable test device or ROM when available, especially init/SELinux, boot overlays, `pm` integration, and recovery deployment. Report which checks ran and which require a device or missing prerequisites.

## Recovery packaging

- `apm-flashable-new/` is the LineageOS Recovery template. `build_recovery_zip.sh` rebuilds by default, stages all three binaries, and emits an ABI-specific ZIP at the repository root. The slot-aware `META-INF/com/google/android/update-binary` chooses the target and generates the OTA survival hook from `system/addon.d/`. Keep its file permissions and payload paths aligned with the ZIP builder.
- Edit SELinux policy in `selinux-contexting/` and sync the tracked mirror at `apm-flashable-new/system/etc/selinux/`. Init service definitions live under `apm-flashable-new/system/etc/init/` and are also referenced by Soong. Check init service names, file and property contexts, and boot ordering together when those interfaces change.
- Update root `THIRD_PARTY_NOTICES` and its flashable copy if bundled codec licensing or versions change. Keep support for archive decoding in the native codec path. The flashable `system/xbin/` tree is helper payload, and the ZIP builder can copy additional helpers from `prebuilt/xz/`.
- Keep build outputs, staged BoringSSL prebuilts, generated compilation databases, and recovery ZIPs out of commits. See [build and deployment](docs/wiki/Build-and-Deployment.md), [the recovery template guide](apm-flashable-new/README.md), and [the BoringSSL helper guide](boringssl-tools/compile-for-all-abi/README.md).
