#!/usr/bin/env bash
# Build LineageOS Recovery flashable ZIP for APM (slot-aware installer)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FLASHABLE_DIR="${PROJECT_ROOT}/apm-flashable-new"
OUTPUT_DIR="${PROJECT_ROOT}"
SELINUX_DST_DIR="${FLASHABLE_DIR}/system/etc/selinux"
SELINUX_SRC_DIR="${APM_SELINUX_SOURCE_DIR:-${PROJECT_ROOT}/selinux-contexting}"
ANDROID_ABIS=("arm64-v8a" "armeabi-v7a" "x86_64" "x86")
TARGET_ABI="${APM_ANDROID_ABI:-arm64-v8a}"
API_LEVEL="${APM_ANDROID_API:-34}"

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

normalize_android_abi() {
  case "$1" in
    arm64|aarch64|arm64-v8a) printf '%s\n' "arm64-v8a" ;;
    arm|armeabi|armeabi-v7a) printf '%s\n' "armeabi-v7a" ;;
    x86-64|amd64|x86_64) printf '%s\n' "x86_64" ;;
    x86|i386|i686) printf '%s\n' "x86" ;;
    all) printf '%s\n' "all" ;;
    *) printf '%s\n' "$1" ;;
  esac
}

is_supported_android_abi() {
  local abi="$1"
  local supported
  for supported in "${ANDROID_ABIS[@]}"; do
    if [[ "${supported}" == "${abi}" ]]; then
      return 0
    fi
  done
  return 1
}

build_dir_for_abi() {
  printf '%s\n' "${PROJECT_ROOT}/build-$1"
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --api)
        if [[ $# -lt 2 ]]; then
          log_error "Missing value for --api"
          exit 1
        fi
        API_LEVEL="$2"
        shift 2
        ;;
      --abi)
        if [[ $# -lt 2 ]]; then
          log_error "Missing value for --abi"
          exit 1
        fi
        TARGET_ABI="$(normalize_android_abi "$2")"
        shift 2
        ;;
      --all)
        TARGET_ABI="all"
        shift
        ;;
      *)
        log_error "Unknown argument: $1"
        echo "Usage: ./build_recovery_zip.sh [--api LEVEL] [--abi ABI|all] [--all]" >&2
        exit 1
        ;;
    esac
  done
}

collect_target_abis() {
  TARGET_ABIS=()
  if [[ "${TARGET_ABI}" == "all" ]]; then
    TARGET_ABIS=("${ANDROID_ABIS[@]}")
    return
  fi

  if ! is_supported_android_abi "${TARGET_ABI}"; then
    log_error "Unsupported Android ABI: ${TARGET_ABI}"
    exit 1
  fi

  TARGET_ABIS=("${TARGET_ABI}")
}

ensure_binaries_built() {
  local missing_abis=()
  local abi
  local build_dir

  if [ "${FORCE_REBUILD}" = "1" ]; then
    log_info "APM_FORCE_REBUILD=1 -> rebuilding binaries from current source"
    cd "${PROJECT_ROOT}"
    ./build_android.sh --api "${API_LEVEL}" --abi "${TARGET_ABI}"
    return
  fi

  for abi in "${TARGET_ABIS[@]}"; do
    build_dir="$(build_dir_for_abi "${abi}")"
    if [[ ! -f "${build_dir}/apm" || ! -f "${build_dir}/apmd" || ! -f "${build_dir}/amsd" ]]; then
      missing_abis+=("${abi}")
    fi
  done

  if [[ "${#missing_abis[@]}" -eq 0 ]]; then
    return
  fi

  log_warn "Missing APM binaries for: ${missing_abis[*]}"
  cd "${PROJECT_ROOT}"
  if [[ "${#missing_abis[@]}" -eq "${#ANDROID_ABIS[@]}" ]] && [[ "${TARGET_ABI}" == "all" ]]; then
    ./build_android.sh --api "${API_LEVEL}" --all
    return
  fi

  for abi in "${missing_abis[@]}"; do
    ./build_android.sh --api "${API_LEVEL}" --abi "${abi}"
  done
}

sync_static_payload() {
  require_file "${FLASHABLE_DIR}/system/bin/apm-sh-path"
  require_file "${FLASHABLE_DIR}/system/bin/apm-bash-path"
  require_file "${SELINUX_SRC_DIR}/apm.cil"
  require_file "${SELINUX_SRC_DIR}/apm_file_contexts"
  require_file "${SELINUX_SRC_DIR}/apm_property_contexts"
  require_file "${SELINUX_SRC_DIR}/apm_service_contexts"

  mkdir -p "${FLASHABLE_DIR}/system/bin"
  mkdir -p "${SELINUX_DST_DIR}"

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

  if [ -d "${PROJECT_ROOT}/prebuilt/xz" ]; then
    mkdir -p "${FLASHABLE_DIR}/system/xbin"
    cp -r "${PROJECT_ROOT}/prebuilt/xz"/* "${FLASHABLE_DIR}/system/xbin/"
    chmod 0755 "${FLASHABLE_DIR}/system/xbin"/* || true
  else
    log_warn "prebuilt/xz not found; continuing without xz helpers"
  fi

  chmod 0755 "${FLASHABLE_DIR}/META-INF/com/google/android/update-binary"
  chmod 0644 "${FLASHABLE_DIR}/META-INF/com/google/android/updater-script"
}

package_zip_for_abi() {
  local abi="$1"
  local build_dir
  local zip_name
  local zip_path

  build_dir="$(build_dir_for_abi "${abi}")"

  require_file "${build_dir}/apm"
  require_file "${build_dir}/apmd"
  require_file "${build_dir}/amsd"

  log_info "Copying ${abi} binaries into flashable template"
  cp "${build_dir}/apm" "${FLASHABLE_DIR}/system/bin/apm"
  cp "${build_dir}/apmd" "${FLASHABLE_DIR}/system/bin/apmd"
  cp "${build_dir}/amsd" "${FLASHABLE_DIR}/system/bin/amsd"
  chmod 0755 "${FLASHABLE_DIR}/system/bin/apm" "${FLASHABLE_DIR}/system/bin/apmd" "${FLASHABLE_DIR}/system/bin/amsd"

  zip_name="apm-lineage-recovery-$(date +%Y%m%d)-${abi}.zip"
  zip_path="${OUTPUT_DIR}/${zip_name}"

  log_info "Creating ZIP: ${zip_name}"
  rm -f "${zip_path}"
  cd "${FLASHABLE_DIR}"
  zip -r0 "${zip_path}" META-INF/ -x "*.git*"
  zip -r9 "${zip_path}" system/ build.prop -x "*.git*" -x "*~" -x "*.swp"

  if command -v zipalign >/dev/null 2>&1; then
    log_info "Running zipalign for ${abi}"
    zipalign -f 4 "${zip_path}" "${zip_path}.aligned"
    mv "${zip_path}.aligned" "${zip_path}"
  fi

  log_info "ZIP created: ${zip_path}"
  log_info "Size: $(du -h "${zip_path}" | cut -f1)"

  log_info "Verifying critical contents for ${abi}"
  unzip -l "${zip_path}" | grep -q "system/bin/apm" && log_info "  ✓ apm"
  unzip -l "${zip_path}" | grep -q "system/bin/apmd" && log_info "  ✓ apmd"
  unzip -l "${zip_path}" | grep -q "system/bin/amsd" && log_info "  ✓ amsd"
  unzip -l "${zip_path}" | grep -q "system/bin/apm-sh-path" && log_info "  ✓ apm-sh-path"
  unzip -l "${zip_path}" | grep -q "system/bin/apm-bash-path" && log_info "  ✓ apm-bash-path"
  unzip -l "${zip_path}" | grep -q "system/etc/init/init.apmd.rc" && log_info "  ✓ init.apmd.rc"
  unzip -l "${zip_path}" | grep -q "system/etc/init/init.amsd.rc" && log_info "  ✓ init.amsd.rc"
  unzip -l "${zip_path}" | grep -q "system/etc/selinux/apm.cil" && log_info "  ✓ apm.cil"
  unzip -l "${zip_path}" | grep -q "system/etc/selinux/apm_file_contexts" && log_info "  ✓ apm_file_contexts"
  unzip -l "${zip_path}" | grep -q "system/etc/selinux/apm_property_contexts" && log_info "  ✓ apm_property_contexts"
  unzip -l "${zip_path}" | grep -q "system/etc/selinux/apm_service_contexts" && log_info "  ✓ apm_service_contexts"
}

# Build policy:
# - default: rebuild so flashable always matches latest source changes
# - override: APM_FORCE_REBUILD=0 to reuse existing binaries
FORCE_REBUILD="${APM_FORCE_REBUILD:-1}"
TARGET_ABIS=()

parse_args "$@"
TARGET_ABI="$(normalize_android_abi "${TARGET_ABI}")"
collect_target_abis
ensure_binaries_built
sync_static_payload

for abi in "${TARGET_ABIS[@]}"; do
  package_zip_for_abi "${abi}"
done

log_info "Flash with LineageOS Recovery (adb sideload or install from UI)."
