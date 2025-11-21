#!/system/bin/sh
MODDIR=${0%/*}

# Ensure apmd hasn't started already
if [ -f /data/apm/apmd.pid ]; then
    PID=$(cat /data/apm/apmd.pid)
    if [ -d "/proc/$PID" ]; then
        exit 0
    fi
fi

# Start apmd using the wrapper (system/bin/apmd)
# This guarantees it runs with correct PATH and exec mountpoints
/system/bin/apmd > /data/apm/logs/apmd.log 2>&1 &

# Save PID
echo $! > /data/apm/apmd.pid

