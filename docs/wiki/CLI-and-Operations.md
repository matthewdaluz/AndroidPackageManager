# CLI and Operations

## Command Groups

### Connectivity and auth

- `apm ping`
- `apm forgot-password`

### Package and repository operations

- `apm update`
- `apm add-repo <file.repo>`
- `apm remove-repo <name|name.repo>`
- `apm install <pkg>`
- `apm remove <pkg>`
- `apm upgrade [pkgs...]`
- `apm autoremove`
- `apm package-install <file>`
- `apm debuglogging <true|false>`
- `apm factory-reset`
- `apm wipe-cache [all|apm|repo-lists|package-downloads|sig-cache|ams-runtime]`

### AMS module operations

- `apm module-list`
- `apm module-install <zip>`
- `apm module-enable <name>`
- `apm module-disable <name>`
- `apm module-remove <name>`

### APK operations

- `apm apk-install <apk>`
- `apm apk-install <apk> --install-as-system`
- `apm apk-uninstall <package>`

### Read-only daemon requests

- `apm list`
- `apm list-repos`
- `apm info <pkg>`
- `apm search <pattern>`

These go through `apmd`, but they do not require a security session.

### Local inspection and trust helpers

- `apm log [--apm|--ams|--module <name>|<module>] [--export|--clear]`
- `apm log [--daemon apm|ams] [--export|--clear]`
- `apm log --clear-all`
- `apm version`
- `apm key-add <file.asc|file.gpg>`
- `apm sig-cache show`
- `apm sig-cache clear`
- `apm help`

## Authentication Boundary

These commands require a valid session:

- `update`
- `add-repo`
- `remove-repo`
- `install`
- `remove`
- `upgrade`
- `autoremove`
- `debuglogging`
- `factory-reset`
- `wipe-cache`
- `module-*`
- `apk-*`
- `log --clear`
- `log --clear-all`

These do not:

- `ping`
- `forgot-password`
- `list`
- `list-repos`
- `info`
- `search`
- `log` follow/export
- `version`
- `key-add`
- `sig-cache`
- `help`

If no password/PIN exists yet, the first privileged command triggers setup and requires:

- a password or PIN
- exactly 3 security questions and answers

Sessions expire after `180` seconds.

## Common Operational Flows

### First-time setup and update

```bash
apm ping
apm update
```

If security is not configured yet, `update` will trigger the initial password/PIN setup flow.

### Install a repository package

```bash
apm install <package>
```

Notes:

- `apm install` first asks the daemon for a simulated install plan and prompts before running it
- repository metadata is built from configured sources before candidate resolution; running `apm update` first is still the clearest way to validate repository setup
- dependency resolution is direct, not a full SAT solver
- dependencies installed only to satisfy another package are tracked as auto-installed
- install downloads can report progress for multiple packages

### Remove a package

```bash
apm remove <package>
```

Behavior:

- manual/local package installs are removed locally first
- repo-backed installs are forwarded to `apmd`
- repo-backed removal refuses to remove a package required by another installed package unless internal force options are used by daemon code

### Autoremove

```bash
apm autoremove
```

This removes packages marked as auto-installed that are no longer needed by installed packages of the same package family.

### Install a local package payload

```bash
apm package-install /path/to/file.deb
```

Supported manual payloads:

- `.deb`
- tar-style archives including `.tar`, `.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.gz`, and `.xz`

Tarball/manual packages must contain `package-info.json`. Required fields are:

- `package`
- `prefix`

The prefix must live under `/data/local/tmp/apm/runtime/installed` and must include a package-specific subdirectory.

Local `.deb` manual installs use the Debian control `Package` and `Version` fields and install under `/data/local/tmp/apm/runtime/installed/commands/<package>`.

### Manage AMS modules

```bash
apm module-install /path/to/module.zip
apm module-list
apm module-disable example-module
apm module-enable example-module
apm module-remove example-module
```

`module-install` enables the module immediately and rebuilds overlays as part of the install.

### Install APKs

User app install:

```bash
apm apk-install /path/to/app.apk
```

System app staging:

```bash
apm apk-install /path/to/app.apk --install-as-system
```

Notes:

- normal APK installs use `pm install --user 0 -r`
- system app staging requires `apmd` to be running as root
- system app staging places `base.apk` into the `apm-system-apps` AMS overlay module
- reboot is required before Android recognizes the staged system app

### Inspect logs

Follow the default `apmd` log:

```bash
apm log
```

Follow AMS:

```bash
apm log --ams
```

Export the selected log:

```bash
apm log --ams --export
```

Exports are written to `/storage/emulated/0`.
Log clearing is daemon-backed and requires an authenticated session.

Clear the selected daemon log:

```bash
apm log --ams --clear
```

Clear a module log:

```bash
apm log example-module --clear
```

Clear all daemon and module logs:

```bash
apm log --clear-all
```

### Manage trusted keys and signature cache

```bash
apm key-add /path/to/repo-key.asc
apm sig-cache show
apm sig-cache clear
```

## Factory Reset

```bash
apm factory-reset
```

The CLI prompts for confirmation first.

The daemon then attempts to:

- remove installed runtime content under the current install root
- clear generated shim binaries
- remove manual package metadata
- wipe password/PIN, session, and security-question data
- delete the package status database
- remove AMS modules
- delete cached repository lists
- uninstall system apps staged with `--install-as-system`

It does not currently advertise a full wipe of:

- trusted keys
- repo source definitions
- package download cache under `pkgs/`
- general cache under `cache/`

## Cache Wipe

```bash
apm wipe-cache [all|apm|repo-lists|package-downloads|sig-cache|ams-runtime]
```

If no target is passed, the CLI prompts for one or more targets. Wiping package downloads preserves `sig-cache.json` unless `sig-cache` is selected too. Wiping `ams-runtime` clears `/data/ams/.runtime` and then asks the module manager to rebuild enabled overlays.

## Primary Logs

- `/data/apm/logs/apmd.log`
- `/data/ams/logs/amsd.log`
- `/data/ams/logs/<module>.log`

Useful supporting paths:

- `/data/apm/debug.txt`
- `/data/apm/pkgs/sig-cache.json`
- `/data/apm/.security/session.bin`
