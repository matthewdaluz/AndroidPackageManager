#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
DEFAULT_PLATFORM="android-21"

echo "Cleaning build directory..."
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"

echo "Select target architecture:"
ARCH_CHOICES=("arm64-v8a" "armeabi-v7a" "x86_64" "x86")
select CHOICE in "${ARCH_CHOICES[@]}" "Custom input"; do
  if [[ -z "${CHOICE:-}" ]]; then
    echo "Invalid selection. Please choose again."
    continue
  fi

  if [[ "${CHOICE}" == "Custom input" ]]; then
    read -r -p "Enter custom ABI (e.g., arm64-v8a): " CUSTOM_ABI
    ANDROID_ABI="${CUSTOM_ABI}"
  else
    ANDROID_ABI="${CHOICE}"
  fi

  case "${ANDROID_ABI}" in
    arm64|aarch64) ANDROID_ABI="arm64-v8a" ;;
    arm|armeabi|armeabi-v7a) ANDROID_ABI="armeabi-v7a" ;;
    x86-64|amd64) ANDROID_ABI="x86_64" ;;
    x86|i386|i686) ANDROID_ABI="x86" ;;
  esac
  break
done

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

echo "Configuring for ABI=${ANDROID_ABI}, platform=${DEFAULT_PLATFORM} using NDK at ${NDK_ROOT}"
cmake -S "${SCRIPT_DIR}" -B "${BUILD_DIR}" \
  -G "Unix Makefiles" \
  -DANDROID_ABI="${ANDROID_ABI}" \
  -DANDROID_PLATFORM="${DEFAULT_PLATFORM}" \
  -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}"

echo "Building with make -j$(nproc)..."
cmake --build "${BUILD_DIR}" -- -j"$(nproc)"

echo "Build completed."
