#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

SILKCAST_BIN="${SILKCAST_BIN:-${ROOT_DIR}/build/silkcast}"
ADDR="${ADDR:-127.0.0.1}"
PORT="${PORT:-18080}"
WS_PORT="${WS_PORT:-$((PORT + 1))}"
TMP_DIR="${TMP_DIR:-${TMPDIR:-/tmp}}"

if [[ ! -x "${SILKCAST_BIN}" ]]; then
  echo "SilkCast binary not found; building..."
  cmake -S . -B build
  cmake --build build -j4
fi

BODY_FILE="$(mktemp "${TMP_DIR}/silkcast_smoke_body.XXXXXX")"
LOG_FILE="$(mktemp "${TMP_DIR}/silkcast_smoke_log.XXXXXX")"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
  rm -f "${BODY_FILE}" "${LOG_FILE}"
}
trap cleanup EXIT

fail() {
  echo "FAIL: $1"
  echo "--- server log ---"
  sed -n '1,220p' "${LOG_FILE}" || true
  exit 1
}

expect_code() {
  local name="$1"
  local expected="$2"
  local url="$3"
  local code
  code="$(curl -s -o "${BODY_FILE}" -w "%{http_code}" --max-time 3 "${url}" || true)"
  if [[ "${code}" != "${expected}" ]]; then
    fail "${name}: expected HTTP ${expected}, got ${code}, url=${url}, body=$(cat "${BODY_FILE}")"
  fi
}

expect_body_contains() {
  local name="$1"
  local pattern="$2"
  if ! grep -q "${pattern}" "${BODY_FILE}"; then
    fail "${name}: response body missing pattern '${pattern}', body=$(cat "${BODY_FILE}")"
  fi
}

echo "Starting SilkCast on ${ADDR}:${PORT}..."
"${SILKCAST_BIN}" --addr "${ADDR}" --port "${PORT}" --ws-port "${WS_PORT}" > "${LOG_FILE}" 2>&1 &
SERVER_PID=$!

ready=0
for _ in $(seq 1 40); do
  if curl -s --max-time 1 "http://${ADDR}:${PORT}/system/info" >/dev/null 2>&1; then
    ready=1
    break
  fi
  sleep 0.1
done
if [[ "${ready}" -ne 1 ]]; then
  fail "server did not become ready"
fi

expect_code "system/info" "200" "http://${ADDR}:${PORT}/system/info"
expect_body_contains "system/info" "\"version\""
expect_body_contains "system/info" "\"ws_port\":${WS_PORT}"
expect_body_contains "system/info" "\"ws_enabled\":true"

expect_code "capabilities" "200" "http://${ADDR}:${PORT}/capabilities"
expect_body_contains "capabilities" "\"codecs\""

expect_code "bad param" "400" "http://${ADDR}:${PORT}/stream/live/video0?w=abc"
expect_body_contains "bad param" "\"field\":\"w\""

expect_code "ws query" "426" "http://${ADDR}:${PORT}/stream/ws?id=video0"
expect_body_contains "ws query" "\"upgrade_required\""
expect_body_contains "ws query" "\"ws_url\":\"ws://${ADDR}:${WS_PORT}/stream/ws/video0\""

expect_code "ws missing id" "400" "http://${ADDR}:${PORT}/stream/ws"
expect_body_contains "ws missing id" "id query parameter is required"

expect_code "ws sidecar upgrade required" "426" "http://${ADDR}:${WS_PORT}/stream/ws/video0"
expect_body_contains "ws sidecar upgrade required" "\"upgrade_required\""

# RTSP encoded route smoke:
# trigger one attempt, then stats should expose lifecycle fields.
RTSP_ENC="rtsp%3A%2F%2F127.0.0.1%3A8554%2Flive.sdp"
set +e
curl -s --max-time 2 -o "${BODY_FILE}" \
  "http://${ADDR}:${PORT}/stream/live/${RTSP_ENC}?codec=h264&container=mp4" >/dev/null
set -e
expect_code "rtsp stats" "200" "http://${ADDR}:${PORT}/stream/${RTSP_ENC}/stats"
expect_body_contains "rtsp stats" "\"state\":\""
expect_body_contains "rtsp stats" "\"teardown_reason\":\""
expect_body_contains "rtsp stats" "\"startup_ms\":"
expect_body_contains "rtsp stats" "\"first_frame_ms\":"
expect_body_contains "rtsp stats" "\"first_iframe_ms\":"

echo "PASS: smoke_api checks completed."
