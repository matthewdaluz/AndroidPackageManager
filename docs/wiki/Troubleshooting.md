# Troubleshooting

## `apm ping` fails

Check:

- `apmd` is running
- Android uses abstract socket `@apmd`, so there is no filesystem socket to inspect
- emulator mode uses `$HOME/APMEmulator/data/apm/apmd.socket`
- `/data/apm/logs/apmd.log` for bind/listen/auth errors

## Privileged commands keep asking for authentication

Check:

- session lifetime is only `180` seconds
- `/data/apm/.security/session.bin` exists and is readable
- `/data/apm/.security/` still exists after resets or manual cleanup

If setup was never completed, the first privileged command will ask you to create a password/PIN and exactly 3 security questions.

## Forgot-password flow is locked out

Wrong security answers trigger a 5 minute cooldown.

State file:

- `/data/apm/.security/reset-lockout.txt`

## `apm update` fails

Check:

- source entries under `/data/apm/sources/sources.list` and `/data/apm/sources/sources.list.d/`
- trusted keys under `/data/apm/keys`
- `trusted=` and `deb-signatures=` options in the source definition
- `/data/apm/logs/apmd.log` for Release, InRelease, checksum, or Packages download errors

Remember:

- `Packages.gz` and plain `Packages` are supported
- `Packages.xz` is not used in the current Android path

## Signature verification fails

Release verification:

- import the correct public key with `apm key-add`
- inspect `trusted=` for the affected source

Detached `.deb` verification:

- inspect `deb-signatures=` for the source
- inspect `/data/apm/pkgs/sig-cache.json`
- clear stale cache entries if needed:

```bash
apm sig-cache clear
```

## `package-install` fails

For local `.deb` installs:

- confirm the file exists
- confirm the archive is actually a readable Debian package

For tar-style manual installs (`.tar`, `.tar.gz`, `.tgz`, `.tar.xz`, `.txz`, `.gz`, `.xz`):

- confirm the archive contains exactly one intended package root
- confirm `package-info.json` exists
- confirm `package-info.json` includes `package` and `prefix`

Manual package metadata is stored under:

- `/data/local/tmp/apm/runtime/manual-packages`

## Installed command is not found in a new shell

Check the current runtime paths, not the older `/data/apm/bin` layout:

- generated shims:
  - `/data/local/tmp/apm/bin/<name>`
- generated PATH files:
  - `/data/local/tmp/apm/path/sh-path.sh`
  - `/data/local/tmp/apm/path/bash-path.sh`
- boot fallback hooks:
  - `/system/bin/apm-sh-path`
  - `/system/bin/apm-bash-path`

Also check:

- `/data/apm/logs/apmd.log` for `export_path` warnings
- managed shell startup files for the APM hook block
- a fresh shell session after install/remove operations

## `module-install` fails

Check:

- the ZIP contains `module-info.json`
- the ZIP contains `overlay/`
- module name uses only `[A-Za-z0-9._-]`
- if `install-sh: true`, `install.sh` exists
- `install.sh` exits with status `0`
- `unzip` is available on the device

Logs:

- `/data/ams/logs/<module>.log`
- `/data/ams/logs/amsd.log`

Remember:

- if `install.sh` fails, AMS rolls the install back automatically

## Overlays are not applied

Check:

- `/data/ams/.amsd_boot_counter`
- `/data/ams/.amsd_safe_mode_threshold`
- `/data/ams/.amsd_safe_mode`
- the module `state.json` file
- `/data/ams/logs/amsd.log`

If a target partition was not mounted when `amsd` started, the partition monitor may retry later in the same boot. If safe mode is active, overlays are intentionally skipped.

## `apk-install --install-as-system` fails

Check:

- `apmd` is running as root
- the AMS module skeleton `apm-system-apps` can be created under `/data/ams/modules/`
- `/data/ams/modules/apm-system-apps/overlay/system/app/` is writable

Also remember:

- successful staging still requires a reboot before Android recognizes the APK as a system app

## `apk-uninstall` did not fully remove a staged system app

The uninstall flow:

- tries `pm uninstall <package>`
- falls back to `pm uninstall --user 0 <package>`
- then does best-effort overlay cleanup

Check:

- `/data/ams/modules/apm-system-apps/overlay/system/app/<package>`
- `/data/apm/logs/apmd.log`

## Legacy module path block on startup

`apmd` refuses to continue if legacy modules still exist under:

- `/data/apm/modules`

Current AMS uses:

- `/data/ams/modules`

The supported cleanup path is a factory reset.

## Factory reset left some data behind

Current factory reset targets:

- installed runtime content
- generated shim binaries
- manual package metadata
- security data
- package status DB
- AMS modules
- repository lists
- system apps staged through `--install-as-system`

It may leave behind:

- source definitions
- trusted keys
- package download cache
- general cache directories

Check the resulting daemon message and `/data/apm/logs/apmd.log` for partial-failure details.
