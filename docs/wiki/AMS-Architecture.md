# AMS Architecture

## Scope

AMS (APM Module System) is the module runtime used by APM for overlay-based system customization.

## Module Storage

Modules are stored in:

- `/data/ams/modules/<module_name>`

Module logs are stored in:

- `/data/ams/logs/<module_name>.log`

Runtime staging:

- `/data/ams/.runtime/upper`
- `/data/ams/.runtime/work`
- `/data/ams/.runtime/base`

## Overlay Targets

AMS targets three trees:

- `system`
- `vendor`
- `product`

The manager discovers candidate mountpoints for `system` in this order:

1. `/system_root/system`
2. `/system`
3. `/`

## Overlay Backend Behavior

Current `ModuleManager::applyOverlayForTarget()` attempts `applyBindMountBackendForTarget(...)` first and returns success immediately on that path.

What this means in practice right now:

- The bind backend is the active overlay path in normal flow.
- Additional compatibility strategy chain exists in code after that return point, but is currently not reached on successful bind-backend path.

## Module Lifecycle

Install flow (`module-install`):

1. unzip module zip (`unzip -oq`) into temporary directory
2. accept flat root or one nested top-level directory
3. require `module-info.json`
4. require `overlay/`
5. move module to `/data/ams/modules/<name>`
6. create/update `state.json` and workdirs
7. rebuild overlays
8. run lifecycle scripts when configured

Enable/disable/remove:

- mutate `state.json`
- rebuild overlays
- log results to module log

## Script Hooks

Based on `module-info.json` booleans:

- `post_fs_data: true` -> execute `post-fs-data.sh` in foreground
- `service: true` -> execute `service.sh` in background

Scripts run via `/system/bin/sh` and append to module log.

## Safe Mode

`amsd` tracks boot risk with files under `/data/ams`:

- counter: `.amsd_boot_counter`
- threshold: `.amsd_safe_mode_threshold` (default 3)
- flag: `.amsd_safe_mode`

Behavior:

- boot counter is incremented on daemon startup
- if threshold is reached or safe-mode flag exists, overlays are skipped
- when boot is considered successful (`sys.boot_completed=1`), counter is reset and safe-mode flag is cleared

## AMS IPC

`amsd` binds and listens on `/data/ams/amsd.sock` and sets mode `0666`.

Supported dispatcher operations include:

- `Ping`
- `ModuleInstall`
- `ModuleEnable`
- `ModuleDisable`
- `ModuleRemove`
- `ModuleList`

All except `Ping` require a valid session token.
