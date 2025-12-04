#
# /*
#  * APM - Android Package Manager
#  *
#  * RedHead Industries - Technologies Branch
#  * Copyright (C) 2025 RedHead Industries
#  *
#  * File: service.sh
#  * Purpose: Magisk service hook to keep apmd running when Android init
#  *          cannot load system-level init scripts (Magisk mode).
#  * Last Modified: December 4th, 2025. - 09:07 AM Eastern Time
#  * Author: Matthew DaLuz - RedHead Founder
#  *
#  * APM is free software: you can redistribute it and/or modify
#  * it under the terms of the GNU General Public License as published by
#  * the Free Software Foundation, either version 3 of the License, or
#  * (at your option) any later version.
#  *
#  * APM is distributed in the hope that it will be useful,
#  * but WITHOUT ANY WARRANTY; without even the implied warranty of
#  * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  * GNU General Public License for more details.
#  *
#  * You should have received a copy of the GNU General Public License
#  * along with APM. If not, see <https://www.gnu.org/licenses/>.
#  *
#  */
#

#!/system/bin/sh
MODDIR=${0%/*}

PID_FILE=/data/apm/apmd.pid
LOG_FILE=/data/apm/logs/apmd.log
AMSD_BIN=/data/apm/bin/amsd
AMSD_PID_FILE=/data/apm/amsd.pid
AMSD_LOG=/ams/logs/amsd.log
AMSD_SOCKET=/dev/socket/amsd

mkdir -p /data/apm/logs
mkdir -p /ams/logs

# Return 0 if a running apmd process is detected
is_apmd_running() {
    # Prefer PID file if present
    if [ -f "$PID_FILE" ]; then
        pid="$(cat "$PID_FILE" 2>/dev/null)"
        if [ -n "$pid" ] && [ -d "/proc/$pid" ]; then
            return 0
        fi
    fi

    # Fallback to pidof in case PID file is missing/stale
    pidof apmd >/dev/null 2>&1
}

is_amsd_running() {
    if [ -f "$AMSD_PID_FILE" ]; then
        pid="$(cat "$AMSD_PID_FILE" 2>/dev/null)"
        if [ -n "$pid" ] && [ -d "/proc/$pid" ]; then
            return 0
        fi
    fi
    pidof amsd >/dev/null 2>&1
}

start_amsd() {
    "$AMSD_BIN" >> "$AMSD_LOG" 2>&1 &
    echo $! > "$AMSD_PID_FILE"
}

start_apmd() {
    /data/apm/bin/apmd >> "$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"
}

wait_for_amsd_ready() {
    readiness_timeout_sec=30
    elapsed=0
    while [ $elapsed -lt $readiness_timeout_sec ]; do
        if getprop amsd.ready 2>/dev/null | grep -q "^1$"; then
            return 0
        fi
        if [ -S "$AMSD_SOCKET" ]; then
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    return 1
}

# Start amsd first so overlays are applied before apmd comes up
if [ -x "$AMSD_BIN" ]; then
    if ! is_amsd_running; then
        start_amsd
    fi

    if ! wait_for_amsd_ready; then
        echo "[apm-magisk] Warning: AMSD readiness check timed out" >> "$AMSD_LOG"
    fi
else
    echo "[apm-magisk] Warning: AMSD binary missing at $AMSD_BIN" >> "$LOG_FILE"
fi

# Keep trying to start apmd until it is confirmed running
while :; do
    if is_apmd_running; then
        break
    fi

    start_apmd
    sleep 5
done

# Wait for Binder readiness by pinging the daemon via CLI.
# This avoids races where clients call apm before the Binder service registers.
readiness_timeout_sec=30
elapsed=0
while [ $elapsed -lt $readiness_timeout_sec ]; do
    # The CLI returns 0 on success for ping; suppress output.
    if /system/bin/apm ping >/dev/null 2>&1; then
        break
    fi
    sleep 1
    elapsed=$((elapsed + 1))
done

if [ $elapsed -ge $readiness_timeout_sec ]; then
    echo "[apm-magisk] Warning: apmd readiness check timed out after ${readiness_timeout_sec}s" >> "$LOG_FILE"
else
    echo "[apm-magisk] apmd Binder service is ready (in ${elapsed}s)" >> "$LOG_FILE"
fi
