#!/system/bin/sh

# Stop daemon if running
if [ -f "/data/apm/apmd.pid" ]; then
    kill "$(cat /data/apm/apmd.pid)" 2>/dev/null
    rm -f /data/apm/apmd.pid
fi

# Remove data directory (optional: keep if user wants)
rm -rf /data/apm
