#!/usr/bin/env bash
set -e

echo "== APM Installer (Pixel 7a / Tensor / LineageOS userdebug) =="

# -----------------------------
# DIRECTORY CONFIG
# -----------------------------
BINARIES_DIR="build"
FLASHABLE_DIR="apm-flashable"

DATA_APM="/data/apm"
DATA_SYSTEM_BIN="$DATA_APM/system/bin"
DATA_INIT="$DATA_APM/init"
DATA_SCRIPTS="$DATA_APM/scripts"
DATA_SEPOLICY="$DATA_APM/sepolicy"
DATA_LOGS="$DATA_APM/logs"

SYSTEM_BIN="/system/bin"
SYSTEM_ETC_INIT="/system/etc/init"

# -----------------------------
# HELPER
# -----------------------------
abort() {
    echo "ERROR: $1"
    exit 1
}

check_adb() {
    adb get-state >/dev/null 2>&1 || abort "Device not connected."
}

echo "[1] Checking ADB..."
check_adb

echo "[2] Restarting ADB as root..."
adb root || abort "adb root failed — enable Rooted Debugging in Developer Options."

echo "[3] Creating APM directories in /data ..."
adb shell "mkdir -p $DATA_SYSTEM_BIN $DATA_INIT $DATA_SCRIPTS $DATA_SEPOLICY $DATA_LOGS"

adb shell "chmod -R 0755 $DATA_APM"
adb shell "chown -R root:root $DATA_APM"

# -----------------------------
# PUSH BINARIES INTO /data
# -----------------------------
echo "[4] Installing binaries into /data/apm/system/bin ..."
adb push "$BINARIES_DIR/apm"        "$DATA_SYSTEM_BIN/" >/dev/null
adb push "$BINARIES_DIR/apmd"       "$DATA_SYSTEM_BIN/" >/dev/null
adb push "$BINARIES_DIR/apm-policy" "$DATA_SYSTEM_BIN/" >/dev/null

adb shell "chmod 0755 $DATA_SYSTEM_BIN/*"

# SELinux: make them system_file so they can be bind-mounted into /system/bin
adb shell "chcon u:object_r:system_file:s0 $DATA_SYSTEM_BIN/*"

# -----------------------------
# INIT SCRIPT
# -----------------------------
echo "[5] Installing init.apmd.rc into /data/apm/init ..."
adb push "$FLASHABLE_DIR/system/etc/init/init.apmd.rc" "$DATA_INIT/" >/dev/null

adb shell "chmod 0644 $DATA_INIT/init.apmd.rc"
adb shell "chcon u:object_r:system_file:s0 $DATA_INIT/init.apmd.rc"

# -----------------------------
# SCRIPTS
# -----------------------------
echo "[6] Installing boot scripts ..."
adb push "$FLASHABLE_DIR/scripts/" "$DATA_SCRIPTS/" >/dev/null

adb shell "chmod 0755 $DATA_SCRIPTS/"*.sh
adb shell "chcon u:object_r:system_file:s0 $DATA_SCRIPTS/"*.sh

# -----------------------------
# SEPOLICY TE RULES
# -----------------------------
echo "[7] Installing TE rules ..."
adb push "$FLASHABLE_DIR/sepolicy/"*.te "$DATA_SEPOLICY/" || abort "Failed to push TE files"
adb shell "chmod 0644 $DATA_SEPOLICY/"*.te

# -----------------------------
# APPLY SEPOLICY NOW
# -----------------------------
echo "[8] Applying SELinux policies (live inject)..."

adb shell "for f in $DATA_SEPOLICY/*.te; do
    echo 'Injecting TE: ' \$f
    $DATA_SYSTEM_BIN/apm-policy \"\$f\"
done"

# -----------------------------
# FINAL INSTRUCTIONS
# -----------------------------
echo "[9] Installation finished!"
echo "APM will be enabled on next boot."
echo "The following will happen automatically at boot:"
echo "  • /data/apm/scripts/apm-overlay.sh → bind mounts binaries + init.rc"
echo "  • /data/apm/scripts/apm-sepolicy.sh → reapplies SELinux rules"
echo ""

echo "[10] Rebooting..."
adb reboot

echo ""
echo "== Done! After reboot, run: =="
echo "adb root && adb shell apm ping"
echo ""

