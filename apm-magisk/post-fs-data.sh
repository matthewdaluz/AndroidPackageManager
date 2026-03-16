#!/system/bin/sh
#
# APM - Android Package Manager
#
# RedHead Industries - Technologies Branch
# Copyright (C) 2026 RedHead Industries
#
# File: post-fs-data.sh
# Purpose: Prepare APM/AMS directory structure for Magisk installs and deploy bundled binaries.
# Last Modified: March 15th, 2026. - 11:15 PM Eastern Time.
# Author: Matthew DaLuz - RedHead Founder
#
# APM is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# APM is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with APM. If not, see <https://www.gnu.org/licenses/>.
#

MODDIR=${0%/*}

# Create runtime directories
mkdir -p /data/apm/bin
mkdir -p /data/apm/installed
mkdir -p /data/apm/installed/dependencies
mkdir -p /data/apm/installed/commands
mkdir -p /data/apm/cache
mkdir -p /data/apm/sandbox
mkdir -p /data/apm/lists
mkdir -p /data/apm/pkgs
mkdir -p /data/apm/logs
mkdir -p /data/apm/sources
mkdir -p /data/apm/sources.list.d
mkdir -p /data/apm/keys
# AMS layout
mkdir -p /ams
mkdir -p /ams/modules
mkdir -p /ams/logs
mkdir -p /ams/.runtime
mkdir -p /ams/.runtime/upper
mkdir -p /ams/.runtime/work
mkdir -p /ams/.runtime/base

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
