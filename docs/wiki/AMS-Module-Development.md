# AMS Module Development

This page documents the current AMS module format accepted by `ModuleManager`.

## Required Structure

Minimum module zip content:

```text
module-info.json
overlay/
```

Optional subtrees and scripts:

```text
overlay/system/
overlay/vendor/
overlay/product/
install.sh
post-fs-data.sh
service.sh
```

`overlay/` must exist, even if only one target subtree is used.

## `module-info.json` Schema

Recognized fields:

- `name` (required)
- `version`
- `author`
- `description`
- `mount` (bool, default `true`)
- `post_fs_data` (bool, default `false`)
- `service` (bool, default `false`)
- `install-sh` (bool, default `false`)

Name validation:

- allowed chars: `a-z`, `A-Z`, `0-9`, `-`, `_`, `.`

## Example Module

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

From inside module root:

```bash
zip -r ../example-fonts.zip .
```

Accepted zip layouts:

- flat root (`module-info.json` at archive root)
- single nested top-level folder containing module root

## Installation

```bash
apm module-install /path/to/example-fonts.zip
```

Other lifecycle commands:

```bash
apm module-list
apm module-enable example-fonts
apm module-disable example-fonts
apm module-remove example-fonts
```

## Script Behavior

- `install.sh` runs once during `module-install` when `install-sh` is true.
- If `install-sh` is true, `install.sh` must exist and exit with status `0`.
- On `install.sh` failure, install fails and AMS keeps the module disabled with
  `last_error` set in `state.json`.
- `post-fs-data.sh` runs synchronously when `post_fs_data` is true.
- `service.sh` runs in background when `service` is true.
- Script output is appended to `/data/ams/logs/<module>.log`.

## State File

AMS writes `state.json` in module root with fields:

- `enabled`
- `installed_at`
- `updated_at`
- `last_error`

## Practical Tips

- Keep top-level overlay entries additive and predictable.
- Validate module name early before packaging.
- Check `/data/ams/logs/<module>.log` after each install/enable cycle.
- Reboot once after major overlay changes if target partitions were not yet mounted when module was enabled.
