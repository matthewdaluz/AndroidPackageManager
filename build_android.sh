#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"
BORINGSSL_PREBUILT_DIR="${SCRIPT_DIR}/prebuilt/boringssl"
ROOT_COMPILE_COMMANDS="${SCRIPT_DIR}/compile_commands.json"

# Default Android platform
DEFAULT_PLATFORM="android-34"

publish_compile_commands() {
  local build_compile_commands="${BUILD_DIR}/compile_commands.json"

  if [[ ! -f "${build_compile_commands}" ]]; then
    echo "Warning: ${build_compile_commands} was not generated; skipping workspace compile_commands.json."
    return
  fi

  rm -f "${ROOT_COMPILE_COMMANDS}"
  if ln -s "${build_compile_commands}" "${ROOT_COMPILE_COMMANDS}" 2>/dev/null; then
    echo "Linked ${ROOT_COMPILE_COMMANDS} -> ${build_compile_commands}"
  else
    cp -f "${build_compile_commands}" "${ROOT_COMPILE_COMMANDS}"
    echo "Symlink unavailable; copied compile_commands.json to workspace root."
  fi
}

resolve_strip_tool() {
  local cache_file="${BUILD_DIR}/CMakeCache.txt"

  if [[ -f "${cache_file}" ]]; then
    STRIP_TOOL=$(grep '^CMAKE_STRIP:FILEPATH=' "${cache_file}" | head -n1 | cut -d= -f2- || true)
  fi

  if [[ -z "${STRIP_TOOL:-}" ]]; then
    STRIP_TOOL="$(command -v llvm-strip || command -v strip || true)"
  fi
}

strip_built_binaries() {
  local binaries=("${BUILD_DIR}/apm" "${BUILD_DIR}/apmd" "${BUILD_DIR}/amsd")
  local binary
  local stripped_any=0

  if [[ "${APM_STRIP_BINARIES:-1}" == "0" ]]; then
    echo "Skipping binary stripping because APM_STRIP_BINARIES=0."
    return
  fi

  resolve_strip_tool
  if [[ -z "${STRIP_TOOL:-}" ]]; then
    echo "Warning: no strip tool found; leaving binaries unstripped."
    return
  fi

  echo "Stripping built binaries with ${STRIP_TOOL}..."
  for binary in "${binaries[@]}"; do
    if [[ ! -f "${binary}" ]]; then
      continue
    fi

    if ! "${STRIP_TOOL}" --strip-unneeded "${binary}" 2>/dev/null; then
      "${STRIP_TOOL}" "${binary}"
    fi
    stripped_any=1
    echo "  Stripped ${binary}"
  done

  if [[ "${stripped_any}" == "0" ]]; then
    echo "Warning: no built binaries were found to strip."
  fi
}

echo "Cleaning build directory..."
rm -rf "${BUILD_DIR}"
mkdir -p "${BUILD_DIR}"
rm -f "${ROOT_COMPILE_COMMANDS}"

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
    -G "Ninja" \
    -DAPM_EMULATOR_MODE=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

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

  if [[ ! -f "${BORINGSSL_PREBUILT_DIR}/libssl.a" \
     || ! -f "${BORINGSSL_PREBUILT_DIR}/libcrypto.a" \
     || ! -d "${BORINGSSL_PREBUILT_DIR}/include" ]]; then
    echo ""
    echo "BoringSSL prebuilts for Android are missing."
    echo "Expected files:"
    echo "  ${BORINGSSL_PREBUILT_DIR}/libssl.a"
    echo "  ${BORINGSSL_PREBUILT_DIR}/libcrypto.a"
    echo "  ${BORINGSSL_PREBUILT_DIR}/include/"
    echo ""
    echo "Example: build BoringSSL from source (run inside your cloned 'boringssl' repo):"
    echo "  cmake -DANDROID_ABI=${ANDROID_ABI} -DANDROID_PLATFORM=${DEFAULT_PLATFORM} -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN_FILE} -GNinja -B build"
    echo "  ninja -C build -j\$(nproc)"
    echo ""
    echo "Then copy the outputs into this repository:"
    echo "  mkdir -p \"${BORINGSSL_PREBUILT_DIR}\""
    echo "  cp -f build/ssl/libssl.a \"${BORINGSSL_PREBUILT_DIR}/\""
    echo "  cp -f build/crypto/libcrypto.a \"${BORINGSSL_PREBUILT_DIR}/\""
    echo "  cp -rf include \"${BORINGSSL_PREBUILT_DIR}/\""
    echo ""
    echo "After that, re-run ./build_android.sh."
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
    -G "Ninja" \
    -DANDROID_ABI="${ANDROID_ABI}" \
    -DANDROID_PLATFORM="${DEFAULT_PLATFORM}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DANDROID_STL="c++_static" \
    -DANDROID_SDK_ROOT="${ANDROID_SDK_ROOT_ENV}" \
    -DANDROID_NDK_ROOT="${ANDROID_NDK_ROOT_ENV}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
fi

publish_compile_commands

echo "Building with cmake --build --parallel $(nproc)..."
cmake --build "${BUILD_DIR}" --parallel "$(nproc)"
strip_built_binaries

echo "Build completed successfully for API ${API_LEVEL}."
