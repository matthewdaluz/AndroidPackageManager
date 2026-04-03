#!/usr/bin/env bash
# Build LineageOS Recovery flashable ZIP for APM (slot-aware installer)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
FLASHABLE_DIR="${PROJECT_ROOT}/apm-flashable-new"
OUTPUT_DIR="${PROJECT_ROOT}"
SELINUX_DST_DIR="${FLASHABLE_DIR}/system/etc/selinux"
SELINUX_SRC_DIR="${APM_SELINUX_SOURCE_DIR:-${PROJECT_ROOT}/selinux-contexting}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

require_file() {
  local f="$1"
  [ -f "$f" ] || { log_error "Missing required file: $f"; exit 1; }
}

ensure_line_present() {
  local file="$1"
  local line="$2"
  grep -Fqx "$line" "$file" || echo "$line" >> "$file"
}

# Build policy:
# - default: rebuild so flashable always matches latest source changes
# - override: APM_FORCE_REBUILD=0 to reuse existing binaries
FORCE_REBUILD="${APM_FORCE_REBUILD:-1}"

if [ "${FORCE_REBUILD}" = "1" ]; then
  log_info "APM_FORCE_REBUILD=1 -> rebuilding binaries from current source"
  cd "${PROJECT_ROOT}"
  echo "1" | ./build_android.sh
elif [ ! -f "${BUILD_DIR}/apm" ] || [ ! -f "${BUILD_DIR}/apmd" ] || [ ! -f "${BUILD_DIR}/amsd" ]; then
  log_warn "APM binaries not found in ${BUILD_DIR}; invoking build_android.sh"
  cd "${PROJECT_ROOT}"
  echo "1" | ./build_android.sh
fi

require_file "${BUILD_DIR}/apm"
require_file "${BUILD_DIR}/apmd"
require_file "${BUILD_DIR}/amsd"
require_file "${FLASHABLE_DIR}/system/bin/apm-sh-path"
require_file "${FLASHABLE_DIR}/system/bin/apm-bash-path"
require_file "${SELINUX_SRC_DIR}/apm.cil"
require_file "${SELINUX_SRC_DIR}/apm_file_contexts"
require_file "${SELINUX_SRC_DIR}/apm_property_contexts"
require_file "${SELINUX_SRC_DIR}/apm_service_contexts"

mkdir -p "${FLASHABLE_DIR}/system/bin"
mkdir -p "${SELINUX_DST_DIR}"

log_info "Copying binaries into flashable template"
cp "${BUILD_DIR}/apm" "${FLASHABLE_DIR}/system/bin/apm"
cp "${BUILD_DIR}/apmd" "${FLASHABLE_DIR}/system/bin/apmd"
cp "${BUILD_DIR}/amsd" "${FLASHABLE_DIR}/system/bin/amsd"
chmod 0755 "${FLASHABLE_DIR}/system/bin/apm" "${FLASHABLE_DIR}/system/bin/apmd" "${FLASHABLE_DIR}/system/bin/amsd"
chmod 0755 "${FLASHABLE_DIR}/system/bin/apm-sh-path" "${FLASHABLE_DIR}/system/bin/apm-bash-path"

if [ "${SELINUX_SRC_DIR}" != "${SELINUX_DST_DIR}" ]; then
  log_info "Syncing SELinux payload from ${SELINUX_SRC_DIR}/"
  cp "${SELINUX_SRC_DIR}/apm.cil" "${SELINUX_DST_DIR}/apm.cil"
  cp "${SELINUX_SRC_DIR}/apm_file_contexts" "${SELINUX_DST_DIR}/apm_file_contexts"
  cp "${SELINUX_SRC_DIR}/apm_property_contexts" "${SELINUX_DST_DIR}/apm_property_contexts"
  cp "${SELINUX_SRC_DIR}/apm_service_contexts" "${SELINUX_DST_DIR}/apm_service_contexts"
else
  log_info "Using SELinux payload from flashable tree: ${SELINUX_DST_DIR}/"
fi
ensure_line_present "${SELINUX_DST_DIR}/apm_file_contexts" "/system/bin/apm-sh-path                         u:object_r:system_file:s0"
ensure_line_present "${SELINUX_DST_DIR}/apm_file_contexts" "/system/bin/apm-bash-path                       u:object_r:system_file:s0"

# Copy xz tools if present
if [ -d "${PROJECT_ROOT}/prebuilt/xz" ]; then
  mkdir -p "${FLASHABLE_DIR}/system/xbin"
  cp -r "${PROJECT_ROOT}/prebuilt/xz"/* "${FLASHABLE_DIR}/system/xbin/"
  chmod 0755 "${FLASHABLE_DIR}/system/xbin"/* || true
else
  log_warn "prebuilt/xz not found; continuing without xz helpers"
fi

chmod 0755 "${FLASHABLE_DIR}/META-INF/com/google/android/update-binary"
chmod 0644 "${FLASHABLE_DIR}/META-INF/com/google/android/updater-script"

ZIP_NAME="apm-lineage-recovery-$(date +%Y%m%d).zip"
ZIP_PATH="${OUTPUT_DIR}/${ZIP_NAME}"

log_info "Creating ZIP: ${ZIP_NAME}"
rm -f "${ZIP_PATH}"
cd "${FLASHABLE_DIR}"
zip -r0 "${ZIP_PATH}" META-INF/ -x "*.git*"
zip -r9 "${ZIP_PATH}" system/ build.prop -x "*.git*" -x "*~" -x "*.swp"

if command -v zipalign >/dev/null 2>&1; then
  log_info "Running zipalign"
  zipalign -f 4 "${ZIP_PATH}" "${ZIP_PATH}.aligned"
  mv "${ZIP_PATH}.aligned" "${ZIP_PATH}"
fi

log_info "ZIP created: ${ZIP_PATH}"
log_info "Size: $(du -h "${ZIP_PATH}" | cut -f1)"

log_info "Verifying critical contents"
unzip -l "${ZIP_PATH}" | grep -q "system/bin/apm" && log_info "  ✓ apm"
unzip -l "${ZIP_PATH}" | grep -q "system/bin/apmd" && log_info "  ✓ apmd"
unzip -l "${ZIP_PATH}" | grep -q "system/bin/amsd" && log_info "  ✓ amsd"
unzip -l "${ZIP_PATH}" | grep -q "system/bin/apm-sh-path" && log_info "  ✓ apm-sh-path"
unzip -l "${ZIP_PATH}" | grep -q "system/bin/apm-bash-path" && log_info "  ✓ apm-bash-path"
unzip -l "${ZIP_PATH}" | grep -q "system/etc/init/init.apmd.rc" && log_info "  ✓ init.apmd.rc"
unzip -l "${ZIP_PATH}" | grep -q "system/etc/init/init.amsd.rc" && log_info "  ✓ init.amsd.rc"
unzip -l "${ZIP_PATH}" | grep -q "system/etc/selinux/apm.cil" && log_info "  ✓ apm.cil"
unzip -l "${ZIP_PATH}" | grep -q "system/etc/selinux/apm_file_contexts" && log_info "  ✓ apm_file_contexts"
unzip -l "${ZIP_PATH}" | grep -q "system/etc/selinux/apm_property_contexts" && log_info "  ✓ apm_property_contexts"
unzip -l "${ZIP_PATH}" | grep -q "system/etc/selinux/apm_service_contexts" && log_info "  ✓ apm_service_contexts"

log_info "Flash with LineageOS Recovery (adb sideload or install from UI)."
