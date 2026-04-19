# AMS Module Development

This page describes the module format currently accepted by `ModuleManager`.

## Minimum Archive Structure

Required contents:

```text
module-info.json
overlay/
```

Optional contents:

```text
overlay/system/
overlay/vendor/
overlay/product/
install.sh
post-fs-data.sh
service.sh
```

`overlay/` must exist even if only one target subtree is populated.

## Accepted ZIP Layouts

The installer accepts either:

- a flat archive root where `module-info.json` is at archive root
- one nested top-level folder that contains the real module root

Anything more complex is not auto-detected.

## `module-info.json`

Recognized fields:

- `name` required
- `version`
- `author`
- `description`
- `mount` boolean, default `true`
- `post_fs_data` boolean, default `false`
- `service` boolean, default `false`
- `install-sh` boolean, default `false`

Notes:

- booleans can be JSON booleans or `"true"` / `"false"` strings
- `null` is accepted and behaves like an unset/default value for recognized fields
- numbers, arrays, and objects are not accepted by the lightweight parser

### Name Rules

Allowed characters:

- `a-z`
- `A-Z`
- `0-9`
- `.`
- `_`
- `-`

If `name` is missing or contains unsupported characters, install fails.

## Example

`module-info.json`:

```json
{
  "name": "example-fonts",
  "version": "1.0.0",
  "author": "you",
  "description": "Example AMS module",
  "mount": true,
  "post_fs_data": false,
  "service": false,
  "install-sh": false
}
```

Directory tree:

```text
example-fonts/
  module-info.json
  overlay/
    system/
      fonts/
        MyFont.ttf
```

## Packaging

From inside the module root:

```bash
zip -r ../example-fonts.zip .
```

## Installation and Lifecycle

Install:

```bash
apm module-install /path/to/example-fonts.zip
```

Other lifecycle commands:

```bash
apm module-list
apm module-disable example-fonts
apm module-enable example-fonts
apm module-remove example-fonts
```

Important current behavior:

- install writes `state.json` with `enabled=true`
- overlays are rebuilt as part of install
- successful install runs declared `post-fs-data.sh` and `service.sh` hooks immediately after the overlay rebuild
- enabling a disabled module also rebuilds overlays and runs declared hooks

## Script Hooks

### `install.sh`

- only runs when `install-sh: true`
- must exist when enabled
- runs during install
- failure aborts install and triggers rollback

### `post-fs-data.sh`

- runs synchronously when `post_fs_data: true`
- runs after overlays are rebuilt during install, enable, and daemon startup

### `service.sh`

- runs in the background when `service: true`
- starts after `post-fs-data.sh` during install, enable, and daemon startup

All scripts run through `/system/bin/sh`.

Script output is appended to:

- `/data/ams/logs/<module>.log`

## Generated State

AMS writes `state.json` in the module root with:

- `enabled`
- `installed_at`
- `updated_at`
- `last_error`

It also creates:

- `workdir/system/`
- `workdir/vendor/`
- `workdir/product/`

## Practical Tips

- keep overlays additive and easy to reason about
- test each target subtree independently
- match the target filesystem shape at the top level; the bind backend expects top-level overlay entries to correspond to existing target files or directories
- prefer placing new files under existing directories instead of trying to create brand-new top-level target entries
- if using `install-sh`, make it idempotent and fail loudly
- check `/data/ams/logs/<module>.log` after install or enable
- if boot-time partitions were not ready when you enabled a module, `amsd` may need a reboot or partition-monitor retry window before the overlay fully appears
