#!/usr/bin/env bash
set -euo pipefail

# Simple launcher intended to be double-clickable on desktop environments.
# It builds (if needed) then starts SilkCast and opens the device list in a browser.

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_DIR}/build"
BIN="${BUILD_DIR}/silkcast"

if [[ ! -x "${BIN}" ]]; then
  mkdir -p "${BUILD_DIR}"
  cmake -S "${REPO_DIR}" -B "${BUILD_DIR}"
  cmake --build "${BUILD_DIR}"
fi

PORT=${PORT:-8080}
ADDR=${ADDR:-0.0.0.0}
IDLE_TIMEOUT=${IDLE_TIMEOUT:-10}

echo "Starting SilkCast on ${ADDR}:${PORT} (idle-timeout ${IDLE_TIMEOUT}s)..."
"${BIN}" --addr "${ADDR}" --port "${PORT}" --idle-timeout "${IDLE_TIMEOUT}" &
PID=$!

sleep 1
URL="http://localhost:${PORT}/device/list"
if command -v xdg-open >/dev/null 2>&1; then
  xdg-open "${URL}" >/dev/null 2>&1 || true
elif command -v open >/dev/null 2>&1; then
  open "${URL}" >/dev/null 2>&1 || true
fi

echo "SilkCast running (pid ${PID}). Press Ctrl+C to stop if launched from a terminal."

wait ${PID}
