# APM LineageOS Recovery Flashable Template

This folder is a fresh flashable-ZIP template for installing APM directly from **LineageOS Recovery**.

## What It Installs

- `apm`, `apmd`, `amsd` binaries
- init services:
  - `system/etc/init/init.amsd.rc`
  - `system/etc/init/init.apmd.rc`
- SELinux payload:
  - `system/etc/selinux/apm.cil`
  - `system/etc/selinux/apm_file_contexts`
  - `system/etc/selinux/apm_property_contexts`
  - `system/etc/selinux/apm_service_contexts`
- addon.d OTA survival script (`system/addon.d/30-apm.sh`)

## Mount/Slot Selection Behavior

Installer target selection is strictly based on mount points present under `/mnt` in recovery:

1. If only one of `/mnt/system`, `/mnt/system_a`, `/mnt/system_b` exists: install there.
2. If multiple exist: choose active slot via `ro.boot.slot_suffix` / `ro.boot.slot` and install to matching `system_a` or `system_b`.
3. If none exist: installation fails.
4. Install path is the selected partition **root** (for example `/mnt/system/`).
5. If recovery exposes top-level `bin`/`etc` as symlinks (loop-prone layout), installer auto-switches to the real writable root (`/mnt/system/system/` style) to avoid copy failures.

## Daemon Startup Design

- `amsd` starts in `post-fs-data` so overlays are prepared early.
- `apmd` starts after `amsd.ready=1`.
- Both are `class core` services.
- Current template uses `u:r:su:s0` service labels as a boot-safety fallback.
- AMSD socket path is `/data/ams/amsd.sock` (not `/dev/socket/amsd`).

## Access Model

Template is configured so APM runtime assets are reachable by `root`, `init`, and `shell` workflows:

- data paths under `/data/apm` and `/data/ams` are created with `root:shell` where appropriate
- `amsd` socket is created `0666 root:shell`
- `apmd` socket is chmod'd by daemon runtime (`0666` on Android mode)

## Build ZIP

From project root:

```bash
cd apm-flashable-new
./build_recovery_zip.sh
```

Output ZIP is created at project root as `apm-lineage-recovery-YYYYMMDD.zip`.

## Flash

1. Boot to LineageOS Recovery
2. Flash/sideload generated ZIP
3. Reboot system
