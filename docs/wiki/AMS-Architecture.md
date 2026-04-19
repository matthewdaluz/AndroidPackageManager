# AMS Architecture

## Scope

AMS is the overlay/module runtime used by APM for Android system customization. It is centered on `ModuleManager`, with `amsd` owning the boot lifecycle and `apmd` reusing the same module APIs for explicit module commands.

## Storage Layout

Modules live under:

- `/data/ams/modules/<module_name>`

Per-module logs live under:

- `/data/ams/logs/<module_name>.log`

Shared daemon/runtime paths:

- `/data/ams/logs/amsd.log`
- `/data/ams/.runtime/upper`
- `/data/ams/.runtime/work`
- `/data/ams/.runtime/base`
- `/data/ams/.runtime/bind-mounts-<target>.txt`
- `/data/ams/amsd.sock`

## Overlay Targets

AMS currently targets three trees:

- `system`
- `vendor`
- `product`

For `system`, the manager probes candidates in this order:

1. `/system_root/system`
2. `/system`
3. `/`

The first mounted/usable candidate becomes the overlay target for that boot attempt.

## Overlay Backend

The active backend is the bind-mount-based overlay path implemented through `applyBindMountBackendForTarget(...)`.

High-level behavior:

- ensure runtime scratch/base directories exist
- build overlay layer stacks from enabled modules
- refresh a base mirror for the selected target under `/data/ams/.runtime/base`
- stage merged module content in tmpfs under `/mnt/ams-overlay-<target>/<target>/bind-layer`
- align staged SELinux labels with the target tree where possible
- unmount previously tracked bind mounts before applying the new view
- bind-mount matching regular files read-only
- use small read-only overlay mounts for directories or special files that need overlay semantics
- save mounted paths in `/data/ams/.runtime/bind-mounts-<target>.txt`
- retry later if a partition is not mounted yet

If boot-time overlay application cannot complete because target partitions are not ready, AMS starts a partition monitor and retries for a bounded period.

Practical constraints:

- top-level overlay entries must line up with existing target filesystem objects
- top-level directories can be merged recursively when matching directories already exist
- top-level regular files can be bind-mounted only when matching regular files already exist
- symlinks and special files trigger a read-only overlay fallback for the affected directory

## Module Lifecycle

### Install

`module-install` does the following:

1. unzip the archive into a temporary cache directory
2. accept either a flat archive root or one nested top-level folder
3. require `module-info.json`
4. require `overlay/`
5. move the module to `/data/ams/modules/<name>`
6. create `workdir/` plus target work subdirectories
7. write `state.json` with `enabled=true`
8. run `install.sh` if `install-sh: true`
9. rebuild overlays immediately
10. run `post-fs-data.sh` and `service.sh` if declared in metadata

If `install.sh` is required and fails, installation is rolled back:

- the module directory is removed
- the module log is removed
- overlays are rebuilt again best-effort

### Enable / Disable / Remove

- enable:
  - update `state.json`
  - rebuild overlays
  - run `post-fs-data.sh` and `service.sh` if declared in metadata
- disable:
  - update `state.json`
  - rebuild overlays
- remove:
  - disable first if needed
  - remove module directory and module log
  - rebuild overlays

## Module Metadata

`module-info.json` currently recognizes:

- `name`
- `version`
- `author`
- `description`
- `mount`
- `post_fs_data`
- `service`
- `install-sh`

`name` is required and must use only:

- `A-Z`
- `a-z`
- `0-9`
- `.`
- `_`
- `-`

## Lifecycle Scripts

Supported script hooks:

- `install.sh`
  - runs during install when `install-sh: true`
  - required when enabled
  - failure aborts and rolls back install
- `post-fs-data.sh`
  - runs synchronously when `post_fs_data: true`
  - runs after a successful overlay rebuild during install, enable, or daemon startup
- `service.sh`
  - runs in the background when `service: true`
  - runs after `post-fs-data.sh` during install, enable, or daemon startup

Scripts run through `/system/bin/sh` and append output to the module log.

## Safe Mode and Boot Tracking

`amsd` tracks boot health with:

- counter: `/data/ams/.amsd_boot_counter`
- threshold: `/data/ams/.amsd_safe_mode_threshold`
- flag: `/data/ams/.amsd_safe_mode`

Current behavior:

- increment the boot counter when `amsd` starts
- if counter >= threshold, or the flag already exists, enter safe mode
- in safe mode, overlays and startup scripts are skipped
- when `sys.boot_completed=1`, reset the counter
- if safe mode was active, clear the safe mode flag so modules can retry next boot

Default safe-mode threshold is `3`.

## `amsd` Boot Sequence

At startup, `amsd`:

1. waits for `/data`; emulator builds require `--emulator` and use emulator paths instead
2. configures logging
3. evaluates safe mode
4. applies overlays if safe mode is not active
5. starts the partition monitor if overlay application was incomplete
6. runs enabled module startup scripts when overlays succeeded
7. starts the AMS IPC server
8. publishes `amsd.ready=1`
9. watches `sys.boot_completed` to mark boot success

## AMS IPC

Socket:

- Android: `/data/ams/amsd.sock`
- Emulator: `$HOME/APMEmulator/ams/amsd.socket`

Supported dispatcher operations:

- `Ping`
- `ModuleInstall`
- `ModuleEnable`
- `ModuleDisable`
- `ModuleRemove`
- `ModuleList`

Only `Ping` is available without a valid session token.
