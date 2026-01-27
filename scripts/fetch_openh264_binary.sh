#!/usr/bin/env bash
set -euo pipefail

# This script fetches Cisco's prebuilt OpenH264 binary for Linux x86_64 v2.6.0.
# Using the official binary ensures you receive Cisco's binary license (including patent grant).
# Building from source does NOT grant that patent license; use at your own risk.

VERSION="2.6.0"
ARCH="linux64"
BASE_URL="http://ciscobinary.openh264.org"
TARBALL="libopenh264-${VERSION}-${ARCH}.8.so.bz2"

DEST_DIR="${OPENH264_ROOT:-./third_party/openh264}"
mkdir -p "${DEST_DIR}/lib" "${DEST_DIR}/include"

echo "Downloading ${TARBALL} ..."
curl -fsSL "${BASE_URL}/${TARBALL}" -o /tmp/${TARBALL}
echo "Decompressing..."
bzip2 -df /tmp/${TARBALL}
mv /tmp/libopenh264-${VERSION}-${ARCH}.8.so "${DEST_DIR}/lib/libopenh264.so"

echo "NOTE: you still need headers. On Debian/Ubuntu, install: sudo apt-get install libopenh264-dev"
echo "Place headers under ${DEST_DIR}/include or point CMake OPENH264_ROOT to your system install."
echo "Done. Set -DOPENH264_ROOT=${DEST_DIR} -DENABLE_OPENH264=ON when running CMake."
