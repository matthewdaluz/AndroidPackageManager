#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

# Default safe platform for Binder NDK
DEFAULT_PLATFORM="android-34"

echo "Cleaning build directory..."
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

echo "Select target architecture:"
ARCH_CHOICES=("arm64-v8a" "armeabi-v7a" "x86_64" "x86" "x86_64 (Emulator Mode)")
select CHOICE in "${ARCH_CHOICES[@]}" "Custom input"; do
  if [[ -z "${CHOICE:-}" ]]; then
    echo "Invalid selection. Please choose again."
    continue
  fi

  if [[ "${CHOICE}" == "Custom input" ]]; then
    read -r -p "Enter custom ABI (e.g., arm64-v8a): " CUSTOM_ABI
    ANDROID_ABI="${CUSTOM_ABI}"
    EMULATOR_MODE=0
  elif [[ "${CHOICE}" == "x86_64 (Emulator Mode)" ]]; then
    ANDROID_ABI="x86_64"
    EMULATOR_MODE=1
  else
    ANDROID_ABI="${CHOICE}"
    EMULATOR_MODE=0
  fi

  case "${ANDROID_ABI}" in
    arm64|aarch64) ANDROID_ABI="arm64-v8a" ;;
    arm|armeabi|armeabi-v7a) ANDROID_ABI="armeabi-v7a" ;;
    x86-64|amd64) ANDROID_ABI="x86_64" ;;
    x86|i386|i686) ANDROID_ABI="x86" ;;
  esac
  break
done

# Allow platform override: ./build_android.sh 33
if [[ $# -ge 1 ]]; then
  USER_API=$1
  DEFAULT_PLATFORM="android-${USER_API}"
fi

# Guard against unsupported binder APIs
API_LEVEL=${DEFAULT_PLATFORM#android-}
if (( API_LEVEL < 29 )); then
  echo ""
  echo "⚠ ERROR: Android API < 29 does not support NDK Binder symbols like AIBinder_decStrong."
  echo "  You selected: ${DEFAULT_PLATFORM}"
  echo "  APM requires ANDROID_PLATFORM >= android-29 (recommended: 34)"
  exit 1
fi

if [[ "${EMULATOR_MODE}" == "1" ]]; then
  # Emulator mode: native x86_64 Linux build without NDK
  echo ""
  echo "Configuring for Emulator Mode:"
  echo "  Target    = Native x86_64 Linux"
  echo "  Mode      = Emulator (no Android NDK)"
  echo ""

  cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -G "Unix Makefiles" \
    -DAPM_EMULATOR_MODE=ON \
    -DCMAKE_BUILD_TYPE=Release

else
  # Normal Android build with NDK
  NDK_ROOT="${ANDROID_NDK:-${ANDROID_NDK_HOME:-$HOME/Android/NDK}}"
  TOOLCHAIN_FILE="${NDK_ROOT}/build/cmake/android.toolchain.cmake"

  if [[ ! -d "${NDK_ROOT}" ]]; then
    echo "Android NDK not found at ${NDK_ROOT}. Set ANDROID_NDK or ANDROID_NDK_HOME." >&2
    exit 1
  fi

  if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
    echo "Android toolchain file not found at ${TOOLCHAIN_FILE}." >&2
    exit 1
  fi

  echo ""
  echo "Configuring:"
  echo "  ABI       = ${ANDROID_ABI}"
  echo "  Platform  = ${DEFAULT_PLATFORM}"
  echo "  NDK       = ${NDK_ROOT}"
  echo ""

  cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -G "Unix Makefiles" \
    -DANDROID_ABI="${ANDROID_ABI}" \
    -DANDROID_PLATFORM="${DEFAULT_PLATFORM}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DANDROID_STL="c++_static"
fi

echo "Building with make -j$(nproc)..."
cmake --build "${BUILD_DIR}" -- -j"$(nproc)"

echo "Build completed successfully for API ${API_LEVEL}."

