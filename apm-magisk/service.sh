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
#  * Last Modified: 2025-11-23 11:01:42 EST
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

mkdir -p /data/apm/logs

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

start_apmd() {
    /system/bin/apmd >> "$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"
}

# Keep trying to start apmd until it is confirmed running
while :; do
    if is_apmd_running; then
        break
    fi

    start_apmd
    sleep 5
done
