#!/bin/bash
# Build and run the transport matrix on ESP-IDF's Linux target.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPS_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
OUT="$SCRIPT_DIR/run_result.txt"
SERVER_LOG="${TMPDIR:-/tmp}/otool_llm_local_sse_server.log"
SERVER_PID=""

cleanup() {
  if [[ -n "$SERVER_PID" ]]; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

exec >"$OUT" 2>&1

echo "=== server ==="
python3 "$APPS_DIR/local_sse_server.py" 18080 >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!
sleep 2
curl --fail --max-time 5 --silent --output /dev/null http://127.0.0.1:18080/ok

echo "=== build ==="
if [[ -n "${IDF_PATH:-}" && -f "$IDF_PATH/export.sh" ]]; then
  # shellcheck disable=SC1091
  source "$IDF_PATH/export.sh" >/dev/null
elif [[ -f "$HOME/esp/esp-idf/export.sh" ]]; then
  # shellcheck disable=SC1091
  source "$HOME/esp/esp-idf/export.sh" >/dev/null
else
  echo "ESP-IDF export.sh not found; set IDF_PATH" >&2
  exit 1
fi

idf.py -C "$SCRIPT_DIR" -B "$SCRIPT_DIR/build-linux" -D IDF_TARGET=linux build

echo "=== run ==="
ELF="$SCRIPT_DIR/build-linux/otool_llm_sdk_transport_test.elf"
if [[ ! -x "$ELF" ]]; then
  echo "ELF not built: $ELF" >&2
  exit 1
fi
"$ELF"
