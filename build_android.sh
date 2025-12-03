#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

# Default Android platform
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

API_LEVEL=${DEFAULT_PLATFORM#android-}
if (( API_LEVEL < 29 )); then
  echo ""
  echo "⚠ ERROR: Android API < 29 is not supported by APM builds."
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
  # Normal Android build with NDK resolved under /opt/android-sdk
  ANDROID_SDK_ROOT_ENV="${ANDROID_SDK_ROOT:-/opt/android-sdk}"
  ANDROID_NDK_ROOT_ENV="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"

  if [[ -z "${ANDROID_NDK_ROOT_ENV}" ]]; then
    if [[ -d "${ANDROID_SDK_ROOT_ENV}/ndk" ]]; then
      # Auto-detect latest NDK directory by lexicographic sort (versioned folders)
      ANDROID_NDK_ROOT_ENV=$(ls -1 "${ANDROID_SDK_ROOT_ENV}/ndk" | sort -r | head -n1)
      ANDROID_NDK_ROOT_ENV="${ANDROID_SDK_ROOT_ENV}/ndk/${ANDROID_NDK_ROOT_ENV}"
    fi
  fi

  if [[ -z "${ANDROID_NDK_ROOT_ENV}" || ! -d "${ANDROID_NDK_ROOT_ENV}" ]]; then
    echo "Android NDK not found. Set ANDROID_NDK_ROOT or install under ${ANDROID_SDK_ROOT_ENV}/ndk/." >&2
    exit 1
  fi

  TOOLCHAIN_FILE="${ANDROID_NDK_ROOT_ENV}/build/cmake/android.toolchain.cmake"
  if [[ ! -f "${TOOLCHAIN_FILE}" ]]; then
    echo "Android toolchain file not found at ${TOOLCHAIN_FILE}." >&2
    exit 1
  fi

  echo ""
  echo "Configuring:"
  echo "  ABI       = ${ANDROID_ABI}"
  echo "  Platform  = ${DEFAULT_PLATFORM}"
  echo "  SDK       = ${ANDROID_SDK_ROOT_ENV}"
  echo "  NDK       = ${ANDROID_NDK_ROOT_ENV}"
  echo ""

  cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
    -G "Unix Makefiles" \
    -DANDROID_ABI="${ANDROID_ABI}" \
    -DANDROID_PLATFORM="${DEFAULT_PLATFORM}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DANDROID_STL="c++_static" \
    -DANDROID_SDK_ROOT="${ANDROID_SDK_ROOT_ENV}" \
    -DANDROID_NDK_ROOT="${ANDROID_NDK_ROOT_ENV}"
fi

echo "Building with make -j$(nproc)..."
cmake --build "${BUILD_DIR}" -- -j"$(nproc)"

echo "Build completed successfully for API ${API_LEVEL}."
