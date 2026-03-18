# Troubleshooting

## `apm ping` fails

Check:

- `apmd` is running
- `/data/apm/apmd.sock` exists and is accessible
- `/data/apm/logs/apmd.log` for bind/listen errors

## `module-install` fails

Check:

- zip contains `module-info.json`
- zip contains `overlay/`
- module name contains only allowed chars (`[A-Za-z0-9._-]`)
- `unzip` is available on device

Logs:

- `/data/ams/logs/<module>.log`
- `/data/ams/amsd.log`

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
