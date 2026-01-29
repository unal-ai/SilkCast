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
DEMO_MODE=${DEMO_MODE:-stream} # stream | list
STREAM_DEVICE=${STREAM_DEVICE:-}
STREAM_CODEC=${STREAM_CODEC:-mjpeg}
STREAM_PARAMS=${STREAM_PARAMS:-"codec=${STREAM_CODEC}"}

echo "Starting SilkCast on ${ADDR}:${PORT} (idle-timeout ${IDLE_TIMEOUT}s)..."
"${BIN}" --addr "${ADDR}" --port "${PORT}" --idle-timeout "${IDLE_TIMEOUT}" &
PID=$!

sleep 1
URL="http://localhost:${PORT}/device/list"

if [[ "${DEMO_MODE}" == "stream" ]]; then
  if [[ -z "${STREAM_DEVICE}" ]] && command -v curl >/dev/null 2>&1; then
    DEV_JSON="$(curl -sf "http://localhost:${PORT}/device/list" || true)"
    if [[ -n "${DEV_JSON}" ]]; then
      if command -v python3 >/dev/null 2>&1; then
        STREAM_DEVICE="$(python3 - <<'PY' <<<"${DEV_JSON}"
import json, sys
try:
    data = json.load(sys.stdin)
    if isinstance(data, list) and data:
        print(data[0])
except Exception:
    pass
PY
)"
      else
        STREAM_DEVICE="$(echo "${DEV_JSON}" | sed -E 's/^\\[\\"?([^",\\]]*).*/\\1/')"
      fi
    fi
  fi

  if [[ -n "${STREAM_DEVICE}" ]]; then
    URL="http://localhost:${PORT}/stream/live/${STREAM_DEVICE}"
    if [[ -n "${STREAM_PARAMS}" ]]; then
      URL="${URL}?${STREAM_PARAMS}"
    fi
  fi
fi
if command -v xdg-open >/dev/null 2>&1; then
  xdg-open "${URL}" >/dev/null 2>&1 || true
elif command -v open >/dev/null 2>&1; then
  open "${URL}" >/dev/null 2>&1 || true
fi

echo "SilkCast running (pid ${PID}). Press Ctrl+C to stop if launched from a terminal."

wait ${PID}
