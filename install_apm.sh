#!/usr/bin/env bash
set -euo pipefail

echo "== APM Installer (adb root, overlay into /system) =="

# Host payload locations
PAYLOAD_BIN_DIR="apm-flashable/system/bin"
PAYLOAD_POLICY_BIN="apm-flashable/magiskpolicy-arm64/magiskpolicy"
PAYLOAD_INIT="apm-flashable/system/etc/init/init.apmd.rc"
PAYLOAD_SEPOLICY_DIR="apm-flashable/sepolicy"

# Device destinations (all payloads live under /data, then bind-mounted at boot)
OVERLAY_BASE="/data/apm-system-overlay"
OVERLAY_BIN="$OVERLAY_BASE/system/bin"
OVERLAY_INIT="$OVERLAY_BASE/init"
OVERLAY_SEPOLICY="$OVERLAY_BASE/sepolicy"
LOG_DIR="/data/apm/logs"
DATA_INIT="/data/apm/init"
DATA_BIN="/data/apm/bin"

REQUIRED_BINS=(apm apmd xz lzmainfo)

abort() {
    echo "ERROR: $1"
    exit 1
}

have_files() {
    for bin in "${REQUIRED_BINS[@]}"; do
        [ -f "$PAYLOAD_BIN_DIR/$bin" ] || abort "Missing $PAYLOAD_BIN_DIR/$bin — build or drop the Android binaries first."
    done
    [ -f "$PAYLOAD_POLICY_BIN" ] || abort "Missing $PAYLOAD_POLICY_BIN — build or drop magiskpolicy (arm64) first."
    [ -f "$PAYLOAD_INIT" ] || abort "Missing $PAYLOAD_INIT"
}

check_adb() {
    adb get-state >/dev/null 2>&1 || abort "Device not connected."
}

ensure_root() {
    echo "[3] Restarting ADB as root..."
    adb root >/dev/null 2>&1 || abort "adb root failed — enable Rooted Debugging in Developer Options."
    adb wait-for-device
    if [ "$(adb shell 'id -u' 2>/dev/null | tr -d '\r')" != "0" ]; then
        abort "adbd is not running as root."
    fi
}

prepare_device_dirs() {
    echo "[4] Preparing /data layout..."
    adb shell "mkdir -p $OVERLAY_BIN $OVERLAY_INIT $OVERLAY_SEPOLICY $LOG_DIR /data/apm /data/ams $DATA_BIN $DATA_INIT"
    adb shell "chmod 0755 $OVERLAY_BIN $OVERLAY_INIT $OVERLAY_SEPOLICY"
    adb shell "chmod 0775 /data/apm /data/ams $LOG_DIR $DATA_BIN $DATA_INIT"
    adb shell "chown -R root:root $OVERLAY_BASE $LOG_DIR $DATA_BIN $DATA_INIT"
    adb shell "chown root:shell /data/apm /data/ams"
}

push_binaries() {
    echo "[5] Pushing binaries into /data/apm-system-overlay (bind-mounted at boot)..."
    for bin in "${REQUIRED_BINS[@]}"; do
        adb push "$PAYLOAD_BIN_DIR/$bin" "$OVERLAY_BIN/" >/dev/null
    done
    adb push "$PAYLOAD_POLICY_BIN" "$OVERLAY_BIN/magiskpolicy" >/dev/null
    adb push "$PAYLOAD_POLICY_BIN" "$DATA_BIN/magiskpolicy" >/dev/null

    adb shell "chmod 0755 $OVERLAY_BIN/* $DATA_BIN/magiskpolicy"
    adb shell "chown root:root $OVERLAY_BIN/* $DATA_BIN/magiskpolicy"
    adb shell "if command -v chcon >/dev/null 2>&1; then chcon u:object_r:system_file:s0 $OVERLAY_BIN/* $DATA_BIN/magiskpolicy; fi"
}

push_init() {
    echo "[6] Staging init.apmd.rc under /data ..."
    adb push "$PAYLOAD_INIT" "$OVERLAY_INIT/" >/dev/null
    adb push "$PAYLOAD_INIT" "$DATA_INIT/" >/dev/null
    adb shell "chmod 0644 $OVERLAY_INIT/init.apmd.rc $DATA_INIT/init.apmd.rc"
    adb shell "chown root:root $OVERLAY_INIT/init.apmd.rc $DATA_INIT/init.apmd.rc"
    adb shell "if command -v chcon >/dev/null 2>&1; then chcon u:object_r:system_file:s0 $OVERLAY_INIT/init.apmd.rc $DATA_INIT/init.apmd.rc; fi"
}

stage_sepolicy() {
    if ls "$PAYLOAD_SEPOLICY_DIR"/*.te >/dev/null 2>&1; then
        echo "[7] Staging SELinux policy sources (applied on boot)..."
        adb push "$PAYLOAD_SEPOLICY_DIR/"*.te "$OVERLAY_SEPOLICY/" >/dev/null
        adb shell "chmod 0644 $OVERLAY_SEPOLICY/"*.te
        adb shell "chown root:root $OVERLAY_SEPOLICY/"*.te
        adb shell "if command -v chcon >/dev/null 2>&1; then chcon u:object_r:system_file:s0 $OVERLAY_SEPOLICY/*; fi"
    else
        echo "[7] No .te files found; skipping SELinux staging."
    fi
}

set_path_env() {
    echo "[8] Prepending APM bins to PATH for this session..."
    adb shell "cat > $DATA_INIT/apm-path.sh <<'EOF'
export PATH=/data/apm/bin:/data/apm-system-overlay/system/bin:\$PATH
EOF
chmod 0644 $DATA_INIT/apm-path.sh
chown root:root $DATA_INIT/apm-path.sh
if command -v chcon >/dev/null 2>&1; then chcon u:object_r:system_file:s0 $DATA_INIT/apm-path.sh; fi
"
    adb shell "PATH=/data/apm/bin:/data/apm-system-overlay/system/bin:\$PATH; command -v apm >/dev/null 2>&1 && echo 'PATH active in current adbd session' || echo 'PATH exported; apm not yet runnable (will work after boot)'"
}

finalize() {
    echo "[9] Installation finished. Rebooting..."
    adb reboot
    echo ""
    echo "== Done! After reboot, run: =="
    echo "adb root && adb shell apm ping"
    echo ""
}

echo "[1] Checking host payloads..."
have_files
echo "[2] Checking ADB..."
check_adb
ensure_root
prepare_device_dirs
push_binaries
push_init
stage_sepolicy
set_path_env
finalize
