#!/usr/bin/env bash
#
# APM - Android Package Manager
# Uninstall Recovery ZIP Build Script
#
# Copyright (C) 2025 RedHead Industries
# Licensed under GPLv3
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
OUTPUT_DIR="${PROJECT_ROOT}"

# Colors for output
GREEN='\033[0;32m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

ZIP_NAME="apm-systemwide-uninstall-$(date +%Y%m%d).zip"
ZIP_PATH="${OUTPUT_DIR}/${ZIP_NAME}"

log_info "Creating uninstall recovery ZIP: ${ZIP_NAME}"
cd "${SCRIPT_DIR}"
rm -f "${ZIP_PATH}"

# Create ZIP with proper alignment for recovery
zip -r0 "${ZIP_PATH}" META-INF/ -x "*.git*"

# Align ZIP for recovery verification
if command -v zipalign >/dev/null 2>&1; then
    log_info "Running zipalign on uninstall ZIP..."
    zipalign -f 4 "${ZIP_PATH}" "${ZIP_PATH}.aligned"
    mv "${ZIP_PATH}.aligned" "${ZIP_PATH}"
fi

if [ -f "${ZIP_PATH}" ]; then
    log_info "Uninstall ZIP created successfully!"
    log_info "Location: ${ZIP_PATH}"
    log_info "Size: $(du -h "${ZIP_PATH}" | cut -f1)"
    echo ""
    log_info "Flash this ZIP in LineageOS Recovery to remove APM from system."
else
    echo "Failed to create uninstall ZIP!"
    exit 1
fi
