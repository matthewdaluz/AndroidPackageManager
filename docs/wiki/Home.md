# APM Wiki Home

This wiki tracks the current codebase for APM, `apmd`, `amsd`, and AMS.

## Suggested Reading Order

1. [APM Architecture](APM-Architecture)
2. [AMS Architecture](AMS-Architecture)
3. [CLI and Operations](CLI-and-Operations)
4. [Build and Deployment](Build-and-Deployment)
5. [AMS Module Development](AMS-Module-Development)
6. [Troubleshooting](Troubleshooting)

## Quick Facts

- Current CLI version in source: `2.0.3b - Open Beta`
- Current CLI build date string: `April 18th, 2026. - 5:00 PM Eastern Time.`
- Main package transport is UNIX socket IPC
- Android `apmd` endpoint: abstract socket `@apmd`
- Android `amsd` endpoint: `/data/ams/amsd.sock`
- Persistent APM data lives under `/data/apm`
- Shell-accessible runtime payloads and shims live under `/data/local/tmp/apm`
- AMS modules live under `/data/ams/modules`
- Recovery flashable packaging is maintained
- Magisk packaging is deprecated

## Source Map

- `src/apm`: CLI, auth/session prompts, log helpers, manual package handling, IPC client/transport
- `src/apmd`: package daemon, install manager, APK installer, factory reset, PATH hotload
- `src/amsd`: AMS daemon, safe mode, module IPC dispatcher
- `src/ams`: module metadata parser and lifecycle manager
- `src/core`: config, repo/release parsing, downloader, status DB, manual package metadata
- `src/util`: filesystem/process helpers and crypto primitives

## Related Documents

- `README.md`
- `.deb-signature-verification-flow.md`
- `apm-flashable-new/README.md`
- `boringssl-tools/compile-for-all-abi/README.md`
