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
