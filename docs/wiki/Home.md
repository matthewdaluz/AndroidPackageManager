# APM Wiki Home

This wiki documents APM (Android Package Manager), AMS (APM Module System), runtime architecture, operations, and module authoring.

## Suggested Page Order

1. [APM Architecture](APM-Architecture)
2. [AMS Architecture](AMS-Architecture)
3. [CLI and Operations](CLI-and-Operations)
4. [Build and Deployment](Build-and-Deployment)
5. [AMS Module Development](AMS-Module-Development)
6. [Troubleshooting](Troubleshooting)

## Quick Facts

- Runtime transport is UNIX socket based.
- Main daemon socket: `/data/apm/apmd.sock`.
- AMS daemon socket: `/data/ams/amsd.sock`.
- CLI version string in source: `2.0.0b - Open Beta`.
- Core runtime directories: `/data/apm` and `/data/ams`.
- Magisk distribution is deprecated and no longer available.

## Source Map

- `src/apm`: CLI
- `src/apmd`: main daemon
- `src/amsd`: AMS daemon
- `src/ams`: module manager and metadata parser
- `src/core`: repo/index/extractor/status/security helpers
- `src/util`: filesystem and crypto helpers
