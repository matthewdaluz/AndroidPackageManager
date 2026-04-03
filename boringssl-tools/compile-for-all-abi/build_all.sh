#!/usr/bin/env bash

set -e

# === CONFIG ===
NDK_PATH="/opt/android-sdk/ndk/29.0.14206865"
API_LEVEL="android-35"
BUILD_TYPE="Release"

ABIS=(
  "arm64-v8a"
  "armeabi-v7a"
  "x86"
  "x86_64"
)

# === PATHS ===
TOOLCHAIN_FILE="$NDK_PATH/build/cmake/android.toolchain.cmake"
ROOT_DIR="$(pwd)"
OUT_DIR="$ROOT_DIR/out"

mkdir -p "$OUT_DIR"

# === BUILD LOOP ===
for ABI in "${ABIS[@]}"; do
  echo "=============================="
  echo "Building for ABI: $ABI"
  echo "=============================="

  BUILD_DIR="$ROOT_DIR/build-$ABI"
  INSTALL_DIR="$OUT_DIR/$ABI"

  rm -rf "$BUILD_DIR"
  mkdir -p "$BUILD_DIR"
  mkdir -p "$INSTALL_DIR"

  cmake \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="$API_LEVEL" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -GNinja \
    -B "$BUILD_DIR"

  ninja -C "$BUILD_DIR" -j"$(nproc)"

  # Copy outputs (adjust if needed)
  echo "Copying output for $ABI..."
  cp -r "$BUILD_DIR"/* "$INSTALL_DIR"/

done

echo "All builds completed!"
