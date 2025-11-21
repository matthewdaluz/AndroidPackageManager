#!/system/bin/sh

MODDIR=${0%/*}

# Create runtime directories
mkdir -p /data/apm/bin
mkdir -p /data/apm/installed
mkdir -p /data/apm/installed/dependencies
mkdir -p /data/apm/installed/commands
mkdir -p /data/apm/cache
mkdir -p /data/apm/lists
mkdir -p /data/apm/pkgs
mkdir -p /data/apm/logs
mkdir -p /data/apm/sources
mkdir -p /data/apm/sources.list.d
mkdir -p /data/apm/keys

# Copy bundled binaries/scripts to runtime area
for FILE in "$MODDIR"/files/*; do
    TARGET="/data/apm/bin/$(basename "$FILE")"
    if [ -L "$FILE" ]; then
        ln -sf "$(readlink "$FILE")" "$TARGET"
    else
        cp -f "$FILE" "$TARGET"
        chmod 755 "$TARGET"
    fi
done

# Nothing else needed here!
