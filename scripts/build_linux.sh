#!/usr/bin/env bash
set -euo pipefail

# Build helper for Linux (amd64/arm64). Optional OpenH264.
# Usage: ARCH=arm64 ENABLE_H264=1 ./scripts/build_linux.sh

ARCH=${ARCH:-$(uname -m)}
ENABLE_H264=${ENABLE_H264:-0}
OPENH264_ROOT=${OPENH264_ROOT:-}

BUILD_DIR="build-${ARCH}"
cmake -S . -B "${BUILD_DIR}" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_OPENH264=$([[ "${ENABLE_H264}" == "1" ]] && echo ON || echo OFF) \
  ${OPENH264_ROOT:+-DOPENH264_ROOT=${OPENH264_ROOT}}

cmake --build "${BUILD_DIR}" -- -j"$(nproc)"
echo "Binary at ${BUILD_DIR}/silkcast"
