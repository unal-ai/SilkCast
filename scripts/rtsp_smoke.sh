#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

SILKCAST_BIN="${SILKCAST_BIN:-${ROOT_DIR}/build/silkcast}"
SILKCAST_PORT="${SILKCAST_PORT:-8090}"
RTSP_PORT="${RTSP_PORT:-8554}"
RTSP_PATH="${RTSP_PATH:-live.sdp}"
RTSP_URL="rtsp://127.0.0.1:${RTSP_PORT}/${RTSP_PATH}"
OUT_MP4="${OUT_MP4:-${TMPDIR:-/tmp}/silkcast_rtsp_relay.mp4}"
LOG_DIR="${LOG_DIR:-${TMPDIR:-/tmp}}"
export RTSP_URL

MEDIAMTX_BIN="${MEDIAMTX_BIN:-}"
if [[ -z "${MEDIAMTX_BIN}" ]]; then
  MEDIAMTX_BIN="$(command -v mediamtx || true)"
  if [[ -z "${MEDIAMTX_BIN}" && -x /opt/homebrew/opt/mediamtx/bin/mediamtx ]]; then
    MEDIAMTX_BIN="/opt/homebrew/opt/mediamtx/bin/mediamtx"
  fi
fi

FFMPEG_BIN="${FFMPEG_BIN:-}"
if [[ -z "${FFMPEG_BIN}" ]]; then
  FFMPEG_BIN="$(command -v ffmpeg || true)"
fi

if [[ -z "${MEDIAMTX_BIN}" ]]; then
  echo "mediamtx not found. Install it (for example: brew install mediamtx) or set MEDIAMTX_BIN."
  exit 1
fi
if [[ -z "${FFMPEG_BIN}" ]]; then
  echo "ffmpeg not found. Install ffmpeg or set FFMPEG_BIN."
  exit 1
fi

if [[ ! -x "${SILKCAST_BIN}" ]]; then
  echo "SilkCast binary missing; building first..."
  cmake -S . -B build
  cmake --build build
fi

MEDIAMTX_CONF="${MEDIAMTX_CONF:-}"
if [[ -z "${MEDIAMTX_CONF}" ]]; then
  if [[ -f /opt/homebrew/etc/mediamtx/mediamtx.yml ]]; then
    MEDIAMTX_CONF="/opt/homebrew/etc/mediamtx/mediamtx.yml"
  elif [[ -f /usr/local/etc/mediamtx/mediamtx.yml ]]; then
    MEDIAMTX_CONF="/usr/local/etc/mediamtx/mediamtx.yml"
  fi
fi

MEDIAMTX_LOG="${LOG_DIR}/mediamtx.log"
FFMPEG_LOG="${LOG_DIR}/rtsp_push.log"
SILKCAST_LOG="${LOG_DIR}/silkcast_rtsp.log"

cleanup() {
  for pid in "${SILKCAST_PID:-}" "${FFMPEG_PID:-}" "${MEDIAMTX_PID:-}"; do
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
      kill "${pid}" || true
    fi
  done
}
trap cleanup EXIT

echo "Starting mediamtx..."
if [[ -n "${MEDIAMTX_CONF}" ]]; then
  "${MEDIAMTX_BIN}" "${MEDIAMTX_CONF}" > "${MEDIAMTX_LOG}" 2>&1 &
else
  "${MEDIAMTX_BIN}" > "${MEDIAMTX_LOG}" 2>&1 &
fi
MEDIAMTX_PID=$!

echo "Publishing synthetic RTSP stream via ffmpeg..."
"${FFMPEG_BIN}" -re -f lavfi -i testsrc=size=640x360:rate=15 -an \
  -c:v libx264 -preset ultrafast -tune zerolatency -g 15 -keyint_min 15 \
  -f rtsp -rtsp_transport tcp "${RTSP_URL}" > "${FFMPEG_LOG}" 2>&1 &
FFMPEG_PID=$!

echo "Starting SilkCast..."
"${SILKCAST_BIN}" --addr 127.0.0.1 --port "${SILKCAST_PORT}" > "${SILKCAST_LOG}" 2>&1 &
SILKCAST_PID=$!

echo "Waiting for SilkCast readiness..."
ready=0
for _ in $(seq 1 30); do
  if curl -s --max-time 1 "http://127.0.0.1:${SILKCAST_PORT}/system/info" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.2
done

if [[ "${ready}" -ne 1 ]]; then
  echo "SilkCast did not become ready. See log: ${SILKCAST_LOG}"
  exit 1
fi

if command -v python3 >/dev/null 2>&1; then
  RTSP_ENC="$(python3 - <<'PY'
import os
import urllib.parse
print(urllib.parse.quote(os.environ["RTSP_URL"], safe=""))
PY
)"
elif command -v python >/dev/null 2>&1; then
  RTSP_ENC="$(python - <<'PY'
import os
import urllib.parse
print(urllib.parse.quote(os.environ["RTSP_URL"], safe=""))
PY
)"
else
  echo "python3/python not found for URL encoding."
  exit 1
fi

set +e
curl --max-time 3 -o "${OUT_MP4}" \
  "http://127.0.0.1:${SILKCAST_PORT}/stream/live/${RTSP_ENC}?codec=h264&container=mp4"
curl_rc=$?
set -e

if [[ "${curl_rc}" -ne 0 && "${curl_rc}" -ne 28 ]]; then
  echo "curl failed with code ${curl_rc}"
  exit 1
fi

if [[ ! -s "${OUT_MP4}" ]]; then
  echo "No relay data received from SilkCast."
  exit 1
fi

if ! head -c 64 "${OUT_MP4}" | grep -q "ftyp"; then
  echo "Output does not look like fMP4 (missing ftyp box)."
  exit 1
fi

bytes="$(wc -c < "${OUT_MP4}" | tr -d ' ')"
echo "OK: wrote ${bytes} bytes to ${OUT_MP4}"
echo "Logs:"
echo "  mediamtx: ${MEDIAMTX_LOG}"
echo "  ffmpeg:   ${FFMPEG_LOG}"
echo "  silkcast: ${SILKCAST_LOG}"
