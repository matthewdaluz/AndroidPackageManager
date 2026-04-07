# CLI and Operations

## Command Groups

### Connectivity and auth

- `apm ping`
- `apm forgot-password`

### Repository/package management

- `apm update`
- `apm install <pkg>`
- `apm remove <pkg>`
- `apm upgrade [pkgs...]`
- `apm autoremove`
- `apm package-install <file>`

### Module operations

- `apm module-list`
- `apm module-install <zip>`
- `apm module-enable <name>`
- `apm module-disable <name>`
- `apm module-remove <name>`

### APK operations

- `apm apk-install <apk>`
- `apm apk-install <apk> --install-as-system`
- `apm apk-uninstall <package>`

### Security keys and cache

- `apm key-add <file.asc|file.gpg>`
- `apm sig-cache show`
- `apm sig-cache clear`

### Local info helpers

- `apm list`
- `apm info <pkg>`
- `apm search <pattern>`
- `apm version`
- `apm help`

## Common Operational Flows

### Initial setup and update

```bash
apm ping
apm update
```

If no passpin is configured yet, privileged operations trigger setup flow.

### Install package

```bash
apm install <package>
```

Current dependency resolver behavior is direct dependency focused (not full recursive SAT).

### Install local archive

```bash
apm package-install /path/to/file.deb
```

Tar-style archives require `package-info.json`.

### Manage modules

```bash
apm module-install /path/to/module.zip
apm module-list
```

### Install APK

User app:

```bash
apm apk-install /path/to/app.apk
```

System overlay app staging:

```bash
apm apk-install /path/to/app.apk --install-as-system
```

## Factory Reset

```bash
apm factory-reset
```

Daemon-side reset removes installed payloads, security data, lists, modules, and performs best-effort cleanup of system app overlays.

## Logs to Check

- `/data/apm/logs/apmd.log`
- `/data/ams/logs/amsd.log`
- `/data/ams/logs/<module>.log`
