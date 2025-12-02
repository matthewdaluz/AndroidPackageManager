#!/usr/bin/env bash
#
# APM - Android Package Manager
# Recovery ZIP Build Script
#
# Copyright (C) 2025 RedHead Industries
# Licensed under GPLv3
#

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
FLASHABLE_DIR="${PROJECT_ROOT}/apm-flashable"
OUTPUT_DIR="${PROJECT_ROOT}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Check if binaries exist
if [ ! -f "${BUILD_DIR}/apm" ] || [ ! -f "${BUILD_DIR}/apmd" ]; then
    log_error "APM binaries not found in ${BUILD_DIR}/"
    log_info "Running build_android.sh to build ARM64 binaries..."
    cd "${PROJECT_ROOT}"
    # Build for arm64 non-interactively
    echo "1" | ./build_android.sh
    if [ ! -f "${BUILD_DIR}/apm" ] || [ ! -f "${BUILD_DIR}/apmd" ]; then
        log_error "Build failed! Cannot create recovery ZIP."
        exit 1
    fi
fi

log_info "Found APM binaries in ${BUILD_DIR}/"

# Copy binaries to flashable structure
log_info "Copying binaries to ${FLASHABLE_DIR}/system/bin/"
cp "${BUILD_DIR}/apm" "${FLASHABLE_DIR}/system/bin/apm"
cp "${BUILD_DIR}/apmd" "${FLASHABLE_DIR}/system/bin/apmd"
chmod 755 "${FLASHABLE_DIR}/system/bin/apm"
chmod 755 "${FLASHABLE_DIR}/system/bin/apmd"

# Copy xz tools if available
if [ -d "${PROJECT_ROOT}/prebuilt/xz" ]; then
    log_info "Copying xz compression tools to ${FLASHABLE_DIR}/system/xbin/"
    cp -r "${PROJECT_ROOT}/prebuilt/xz"/* "${FLASHABLE_DIR}/system/xbin/"
    chmod 755 "${FLASHABLE_DIR}/system/xbin"/*
else
    log_warn "xz tools not found in prebuilt/, skipping..."
fi

# Create the ZIP
ZIP_NAME="apm-systemwide-arm64-$(date +%Y%m%d).zip"
ZIP_PATH="${OUTPUT_DIR}/${ZIP_NAME}"

log_info "Creating recovery ZIP: ${ZIP_NAME}"
cd "${FLASHABLE_DIR}"
rm -f "${ZIP_PATH}"

# Create ZIP with proper alignment for recovery (no compression for specific files)
# Exclude build scripts and documentation to prevent footer errors
zip -r0 "${ZIP_PATH}" META-INF/ -x "*.git*"
zip -r9 "${ZIP_PATH}" system/ build.prop -x "*.git*" -x "*~" -x "*.swp"

# Align ZIP for recovery verification
if command -v zipalign >/dev/null 2>&1; then
    log_info "Running zipalign on recovery ZIP..."
    zipalign -f 4 "${ZIP_PATH}" "${ZIP_PATH}.aligned"
    mv "${ZIP_PATH}.aligned" "${ZIP_PATH}"
fi

if [ -f "${ZIP_PATH}" ]; then
    log_info "Recovery ZIP created successfully!"
    log_info "Location: ${ZIP_PATH}"
    log_info "Size: $(du -h "${ZIP_PATH}" | cut -f1)"
    echo ""
    
    # Verify critical components
    log_info "Verifying ZIP contents..."
    if unzip -l "${ZIP_PATH}" | grep -q "system/bin/apmd"; then
        log_info "  ✓ APM binaries included"
    fi
    if unzip -l "${ZIP_PATH}" | grep -q "system/etc/init/init.apmd.rc"; then
        log_info "  ✓ Init service included"
    fi
    if unzip -l "${ZIP_PATH}" | grep -q "system/etc/selinux/apm.cil"; then
        log_info "  ✓ SELinux policy (apm.cil) included"
    fi
    if unzip -l "${ZIP_PATH}" | grep -q "system/etc/selinux/apm_file_contexts"; then
        log_info "  ✓ SELinux file contexts included"
    fi
    if unzip -l "${ZIP_PATH}" | grep -q "system/etc/selinux/apm_service_contexts"; then
        log_info "  ✓ SELinux service contexts included"
    fi
    
    echo ""
    log_info "Flash this ZIP in LineageOS Recovery to install APM system-wide."
    log_info "APM includes custom SELinux policies for enforcing mode support."
    log_info "After flashing, reboot and run 'apm ping' to verify installation."
else
    log_error "Failed to create recovery ZIP!"
    exit 1
fi
