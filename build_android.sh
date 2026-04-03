#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_BUILD_DIR="${SCRIPT_DIR}/build"
BORINGSSL_PREBUILT_DIR="${SCRIPT_DIR}/prebuilt/boringssl"
ROOT_COMPILE_COMMANDS="${SCRIPT_DIR}/compile_commands.json"
ANDROID_ABIS=("arm64-v8a" "armeabi-v7a" "x86_64" "x86")

# Default Android platform
DEFAULT_PLATFORM="android-34"
REQUESTED_ABI="${APM_ANDROID_ABI:-}"
EMULATOR_MODE=0

publish_compile_commands() {
  local build_dir="$1"
  local build_compile_commands="${build_dir}/compile_commands.json"

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
  local build_dir="$1"
  local cache_file="${build_dir}/CMakeCache.txt"
  local strip_tool=""

  if [[ -f "${cache_file}" ]]; then
    strip_tool=$(grep '^CMAKE_STRIP:FILEPATH=' "${cache_file}" | head -n1 | cut -d= -f2- || true)
  fi

  if [[ -z "${strip_tool}" ]]; then
    strip_tool="$(command -v llvm-strip || command -v strip || true)"
  fi

  printf '%s\n' "${strip_tool}"
}

strip_built_binaries() {
  local build_dir="$1"
  local binaries=("${build_dir}/apm" "${build_dir}/apmd" "${build_dir}/amsd")
  local binary
  local stripped_any=0
  local strip_tool

  if [[ "${APM_STRIP_BINARIES:-1}" == "0" ]]; then
    echo "Skipping binary stripping because APM_STRIP_BINARIES=0."
    return
  fi

  strip_tool="$(resolve_strip_tool "${build_dir}")"
  if [[ -z "${strip_tool}" ]]; then
    echo "Warning: no strip tool found; leaving binaries unstripped."
    return
  fi

  echo "Stripping built binaries in ${build_dir} with ${strip_tool}..."
  for binary in "${binaries[@]}"; do
    if [[ ! -f "${binary}" ]]; then
      continue
    fi

    if ! "${strip_tool}" --strip-unneeded "${binary}" 2>/dev/null; then
      "${strip_tool}" "${binary}"
    fi
    stripped_any=1
    echo "  Stripped ${binary}"
  done

  if [[ "${stripped_any}" == "0" ]]; then
    echo "Warning: no built binaries were found to strip."
  fi
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
  printf '%s\n' "${SCRIPT_DIR}/build-$1"
}

require_boringssl_prebuilt() {
  local abi="$1"
  local toolchain_file="$2"
  local boringssl_build_dir="${BORINGSSL_PREBUILT_DIR}/build-${abi}"

  if [[ -f "${boringssl_build_dir}/libssl.a" \
     && -f "${boringssl_build_dir}/libcrypto.a" \
     && -d "${BORINGSSL_PREBUILT_DIR}/include" ]]; then
    return
  fi

  echo ""
  echo "BoringSSL prebuilts for Android ABI ${abi} are missing."
  echo "Expected files:"
  echo "  ${boringssl_build_dir}/libssl.a"
  echo "  ${boringssl_build_dir}/libcrypto.a"
  echo "  ${BORINGSSL_PREBUILT_DIR}/include/"
  echo ""
  echo "Example: build BoringSSL from source (run inside your cloned 'boringssl' repo):"
  echo "  cmake -DANDROID_ABI=${abi} -DANDROID_PLATFORM=${DEFAULT_PLATFORM} -DCMAKE_TOOLCHAIN_FILE=${toolchain_file} -GNinja -B build-${abi}"
  echo "  ninja -C build-${abi} -j\$(nproc)"
  echo ""
  echo "Then copy the outputs into this repository:"
  echo "  mkdir -p \"${boringssl_build_dir}\""
  echo "  cp -f build-${abi}/ssl/libssl.a \"${boringssl_build_dir}/\""
  echo "  cp -f build-${abi}/crypto/libcrypto.a \"${boringssl_build_dir}/\""
  echo "  cp -rf include \"${BORINGSSL_PREBUILT_DIR}/\""
  echo ""
  echo "After that, re-run ./build_android.sh."
  exit 1
}

prompt_for_target() {
  echo "Select target architecture:"
  local arch_choices=(
    "arm64-v8a"
    "armeabi-v7a"
    "x86_64"
    "x86"
    "All Android architectures"
    "x86_64 (Emulator Mode)"
  )

  select choice in "${arch_choices[@]}" "Custom input"; do
    if [[ -z "${choice:-}" ]]; then
      echo "Invalid selection. Please choose again."
      continue
    fi

    case "${choice}" in
      "All Android architectures")
        REQUESTED_ABI="all"
        EMULATOR_MODE=0
        ;;
      "x86_64 (Emulator Mode)")
        REQUESTED_ABI="x86_64"
        EMULATOR_MODE=1
        ;;
      "Custom input")
        read -r -p "Enter custom ABI (e.g., arm64-v8a or all): " custom_abi
        REQUESTED_ABI="$(normalize_android_abi "${custom_abi}")"
        EMULATOR_MODE=0
        ;;
      *)
        REQUESTED_ABI="$(normalize_android_abi "${choice}")"
        EMULATOR_MODE=0
        ;;
    esac
    break
  done
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --api)
        if [[ $# -lt 2 ]]; then
          echo "Missing value for --api" >&2
          exit 1
        fi
        DEFAULT_PLATFORM="android-$2"
        shift 2
        ;;
      --abi)
        if [[ $# -lt 2 ]]; then
          echo "Missing value for --abi" >&2
          exit 1
        fi
        REQUESTED_ABI="$(normalize_android_abi "$2")"
        shift 2
        ;;
      --all)
        REQUESTED_ABI="all"
        shift
        ;;
      --emulator)
        REQUESTED_ABI="x86_64"
        EMULATOR_MODE=1
        shift
        ;;
      *)
        if [[ "$1" =~ ^[0-9]+$ ]]; then
          DEFAULT_PLATFORM="android-$1"
          shift
        else
          echo "Unknown argument: $1" >&2
          echo "Usage: ./build_android.sh [API_LEVEL] [--api LEVEL] [--abi ABI|all] [--all] [--emulator]" >&2
          exit 1
        fi
        ;;
    esac
  done
}

build_emulator() {
  local build_dir="${DEFAULT_BUILD_DIR}"

  echo "Cleaning build directory ${build_dir}..."
  rm -rf "${build_dir}"
  mkdir -p "${build_dir}"

  echo ""
  echo "Configuring for Emulator Mode:"
  echo "  Target    = Native x86_64 Linux"
  echo "  Mode      = Emulator (no Android NDK)"
  echo "  Build Dir = ${build_dir}"
  echo ""

  cmake -S "${SCRIPT_DIR}" -B "${build_dir}" \
    -G "Ninja" \
    -DAPM_EMULATOR_MODE=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

  publish_compile_commands "${build_dir}"

  echo "Building with cmake --build --parallel $(nproc)..."
  cmake --build "${build_dir}" --parallel "$(nproc)"
  strip_built_binaries "${build_dir}"
}

build_android_abi() {
  local abi="$1"
  local build_dir
  build_dir="$(build_dir_for_abi "${abi}")"

  require_boringssl_prebuilt "${abi}" "${TOOLCHAIN_FILE}"

  echo "Cleaning build directory ${build_dir}..."
  rm -rf "${build_dir}"
  mkdir -p "${build_dir}"

  echo ""
  echo "Configuring:"
  echo "  ABI       = ${abi}"
  echo "  Platform  = ${DEFAULT_PLATFORM}"
  echo "  SDK       = ${ANDROID_SDK_ROOT_ENV}"
  echo "  NDK       = ${ANDROID_NDK_ROOT_ENV}"
  echo "  Build Dir = ${build_dir}"
  echo ""

  cmake -S "${SCRIPT_DIR}" -B "${build_dir}" \
    -G "Ninja" \
    -DANDROID_ABI="${abi}" \
    -DANDROID_PLATFORM="${DEFAULT_PLATFORM}" \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_FILE}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DANDROID_STL="c++_static" \
    -DANDROID_SDK_ROOT="${ANDROID_SDK_ROOT_ENV}" \
    -DANDROID_NDK_ROOT="${ANDROID_NDK_ROOT_ENV}" \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

  publish_compile_commands "${build_dir}"

  echo "Building ${abi} with cmake --build --parallel $(nproc)..."
  cmake --build "${build_dir}" --parallel "$(nproc)"
  strip_built_binaries "${build_dir}"
}

parse_args "$@"

if [[ -z "${REQUESTED_ABI}" ]]; then
  prompt_for_target
else
  REQUESTED_ABI="$(normalize_android_abi "${REQUESTED_ABI}")"
fi

API_LEVEL=${DEFAULT_PLATFORM#android-}
if (( API_LEVEL < 29 )); then
  echo ""
  echo "⚠ ERROR: Android API < 29 is not supported by APM builds."
  echo "  You selected: ${DEFAULT_PLATFORM}"
  echo "  APM requires ANDROID_PLATFORM >= android-29 (recommended: 34)"
  exit 1
fi

rm -f "${ROOT_COMPILE_COMMANDS}"

if [[ "${EMULATOR_MODE}" == "1" ]]; then
  build_emulator
else
  if [[ "${REQUESTED_ABI}" != "all" ]] && ! is_supported_android_abi "${REQUESTED_ABI}"; then
    echo "Unsupported Android ABI: ${REQUESTED_ABI}" >&2
    echo "Supported ABIs: ${ANDROID_ABIS[*]}, all" >&2
    exit 1
  fi

  ANDROID_SDK_ROOT_ENV="${ANDROID_SDK_ROOT:-/opt/android-sdk}"
  ANDROID_NDK_ROOT_ENV="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-}}"

  if [[ -z "${ANDROID_NDK_ROOT_ENV}" ]]; then
    if [[ -d "${ANDROID_SDK_ROOT_ENV}/ndk" ]]; then
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

  if [[ "${REQUESTED_ABI}" == "all" ]]; then
    for abi in "${ANDROID_ABIS[@]}"; do
      build_android_abi "${abi}"
    done
  else
    build_android_abi "${REQUESTED_ABI}"
  fi
fi

echo "Build completed successfully for API ${API_LEVEL}."
