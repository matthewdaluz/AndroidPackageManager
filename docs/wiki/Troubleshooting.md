# Troubleshooting

## `apm ping` fails

Check:

- `apmd` is running
- Android runtime uses abstract socket `@apmd`, so there is no filesystem socket to inspect
- emulator mode uses `$HOME/APMEmulator/data/apm/apmd.socket`
- `/data/apm/logs/apmd.log` for bind/listen errors

## `module-install` fails

Check:

- zip contains `module-info.json`
- zip contains `overlay/`
- module name contains only allowed chars (`[A-Za-z0-9._-]`)
- if `module-info.json` sets `install-sh: true`, zip includes `install.sh`
- verify `install.sh` exits with code `0`
- note: if `install.sh` fails, AMS rolls back and uninstalls the module
- `unzip` is available on device

Logs:

- `/data/ams/logs/<module>.log`
- `/data/ams/amsd.log`

## Installed command not found in shell

If a package installs successfully but `command -v <name>` fails in a new shell:

- confirm shim exists under `/data/apm/bin`:
  - `ls -l /data/apm/bin/<name>`
- open a fresh shell session (or `su` session) after install/remove actions
- verify canonical PATH source files exist:
  - `/data/apm/path/sh-path.sh`
  - `/data/apm/path/bash-path.sh`
- verify boot fallback hook scripts exist and are executable:
  - `/system/bin/apm-sh-path`
  - `/system/bin/apm-bash-path`
- in a fresh shell, verify fallback env vars:
  - `echo "$ENV"` should be `/system/bin/apm-sh-path`
  - `echo "$BASH_ENV"` should be `/system/bin/apm-bash-path`
- verify hook line exists exactly once in managed startup files that are present:
  - each managed file should include the APM block that sets `APM_SHIM_DIR`
    to `/data/apm/bin`, updates `PATH`, and sources the APM path script
  - required targets:
    - `/data/local/userinit.sh`
    - `/data/local/tmp/.profile`
    - `/data/local/tmp/.mkshrc`
    - `/data/local/tmp/.bashrc`
    - `/data/local/tmp/.bash_profile`
  - best-effort targets (may be absent on some devices):
    - `/data/.profile`
    - `/data/.mkshrc`
    - `/data/.bashrc`
    - `/data/.bash_profile`
- check `/data/apm/logs/apmd.log` for `export_path` hook install warnings

You can force a hotload rebuild by installing/removing any package, then opening a fresh shell.

## Overlays not applied at boot

Check:

- safe mode files under `/data/ams/.amsd_*`
- `amsd` log for overlay backend failures
- module `state.json` (`enabled` true)

If safe mode is active, AMS intentionally skips overlay apply until boot success/flag clear logic completes.

## Repository update/download failures

Check:

- source entries in `/data/apm/sources/sources.list*`
- trusted keys in `/data/apm/keys`
- trust policy options (`trusted=`, `deb-signatures=`)

Also check `apmd` logs for Release and Packages verification messages.

## Signature verification failures

Release signatures:

- ensure correct key imported via `apm key-add`
- inspect `trusted=` source option

`.deb` signatures:

- inspect `deb-signatures=` source option
- clear stale cache if needed:

```bash
apm sig-cache clear
```

## Forgot-password cooldown

On wrong security answers, reset attempts are locked for 5 minutes.

State file:

- `/data/apm/.security/reset-lockout.txt`

## Legacy module path block

`apmd` startup can fail if legacy module data exists under:

- `/data/apm/modules`

Current AMS expects modules under `/data/ams/modules`.

## Factory reset behavior questions

`apm factory-reset` is destructive to APM/AMS runtime data.

It removes installed payloads, security data, lists, module trees, and does best-effort cleanup for system app overlays.
