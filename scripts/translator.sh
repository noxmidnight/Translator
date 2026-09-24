#!/usr/bin/env bash
# Start llama-server + GTK translator GUI together.
# When the GUI exits (or this script is interrupted), the server we started is stopped.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
LOG="${TRANSLATOR_SERVER_LOG:-/tmp/translator-llama-server.log}"
SERVER_PID=""
STARTED_SERVER=0

ensure_config "$ROOT" || exit 1

HOST="$(get_ini "$CONFIG" server host 127.0.0.1)"
PORT="$(get_ini "$CONFIG" server port 8080)"
MODEL="$(get_ini "$CONFIG" model path /home/nox/models/allam-7b-instruct-preview-q4_k_m.gguf)"
CTX="$(get_ini "$CONFIG" model ctx_size 2048)"
NGL="$(get_ini "$CONFIG" model gpu_layers 99)"

if ! LLAMA_SERVER="$(find_llama_server)"; then
  echo "llama-server not found" >&2
  exit 1
fi

GUI="${TRANSLATOR_BIN:-$ROOT/build/translator}"
if [[ ! -x "$GUI" ]]; then
  echo "GUI binary not found: $GUI" >&2
  echo "Build first: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)" >&2
  exit 1
fi

if [[ ! -f "$MODEL" ]]; then
  echo "Model not found: $MODEL" >&2
  echo "Run: ./scripts/download-model.sh" >&2
  exit 1
fi

server_healthy() {
  curl -sf --connect-timeout 1 --max-time 2 "http://${HOST}:${PORT}/health" >/dev/null 2>&1
}

cleanup() {
  local ec=$?
  trap - EXIT INT TERM
  if [[ "$STARTED_SERVER" -eq 1 && -n "$SERVER_PID" ]]; then
    echo "Stopping llama-server (pid $SERVER_PID)…"
    kill "$SERVER_PID" 2>/dev/null || true
    for _ in 1 2 3 4 5; do
      kill -0 "$SERVER_PID" 2>/dev/null || break
      sleep 0.4
    done
    if kill -0 "$SERVER_PID" 2>/dev/null; then
      kill -9 "$SERVER_PID" 2>/dev/null || true
    fi
    pkill -P "$SERVER_PID" 2>/dev/null || true
  fi
  exit "$ec"
}

trap cleanup EXIT INT TERM

if server_healthy; then
  echo "Reusing existing llama-server at http://${HOST}:${PORT}"
  STARTED_SERVER=0
else
  echo "Starting llama-server…"
  echo "  model: $MODEL"
  echo "  http://${HOST}:${PORT}  ctx=$CTX  ngl=$NGL"
  echo "  log: $LOG"
  echo "Tip: close heavy apps — ALLaM-7B Q4 needs ~6 GB on 8GB Jetson."

  "$LLAMA_SERVER" \
    -m "$MODEL" \
    --host "$HOST" \
    --port "$PORT" \
    -c "$CTX" \
    -ngl "$NGL" \
    -fa on \
    --jinja \
    >"$LOG" 2>&1 &
  SERVER_PID=$!
  STARTED_SERVER=1

  echo -n "Waiting for model to load"
  for i in $(seq 1 180); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
      echo
      echo "llama-server exited early. Last log lines:" >&2
      tail -n 40 "$LOG" >&2 || true
      exit 1
    fi
    if server_healthy; then
      echo " ready."
      break
    fi
    echo -n "."
    sleep 1
    if [[ "$i" -eq 180 ]]; then
      echo
      echo "Timed out waiting for llama-server. See $LOG" >&2
      exit 1
    fi
  done
fi

echo "Launching GUI…"
cd "$ROOT"
export TRANSLATOR_CONFIG="$CONFIG"
export TRANSLATOR_ROOT="$ROOT"
"$GUI"
