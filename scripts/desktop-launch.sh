#!/usr/bin/env bash
# Desktop launcher: start llama-server + GTK GUI; always free RAM on exit.
# Closing the GUI stops llama-server on the configured port (and any server
# this script started), so Jetson unified memory is released.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${TRANSLATOR_CONFIG:-$ROOT/config.ini}"
LOG="${TRANSLATOR_SERVER_LOG:-/tmp/translator-llama-server.log}"
PIDFILE="${TRANSLATOR_PIDFILE:-/tmp/translator-desktop.pids}"
SERVER_PID=""
STARTED_SERVER=0

notify() {
  local summary="$1" body="${2:-}"
  if command -v notify-send >/dev/null 2>&1; then
    notify-send -a "Translator" "$summary" "$body" || true
  fi
  echo "$summary ${body}" >&2
}

if [[ ! -f "$CONFIG" ]]; then
  if [[ -f "$ROOT/config.ini.example" ]]; then
    cp "$ROOT/config.ini.example" "$ROOT/config.ini"
    CONFIG="$ROOT/config.ini"
  else
    notify "Translator" "Missing config.ini"
    exit 1
  fi
fi

get_ini() {
  local section="$1" key="$2" default="${3:-}"
  awk -F'=' -v s="[$section]" -v k="$key" -v d="$default" '
    $0 ~ /^\[/ { insec = ($0 == s) }
    insec && $1 ~ "^[[:space:]]*"k"[[:space:]]*$" {
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; found=1; exit
    }
    END { if (!found) print d }
  ' "$CONFIG"
}

HOST="$(get_ini server host 127.0.0.1)"
PORT="$(get_ini server port 8080)"
MODEL="$(get_ini model path /home/nox/models/allam-7b-instruct-preview-q4_k_m.gguf)"
CTX="$(get_ini model ctx_size 2048)"
NGL="$(get_ini model gpu_layers 99)"

LLAMA_SERVER="${LLAMA_SERVER:-llama-server}"
if ! command -v "$LLAMA_SERVER" >/dev/null 2>&1; then
  if [[ -x /usr/local/bin/llama-server ]]; then
    LLAMA_SERVER=/usr/local/bin/llama-server
  else
    notify "Translator" "llama-server not found"
    exit 1
  fi
fi

GUI="${TRANSLATOR_BIN:-$ROOT/build/translator}"
if [[ ! -x "$GUI" ]]; then
  if command -v cmake >/dev/null 2>&1; then
    notify "Translator" "Building GUI…"
    cmake -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$ROOT/build" -j"$(nproc)" >/dev/null
  fi
fi
if [[ ! -x "$GUI" ]]; then
  notify "Translator" "GUI binary missing. Build with cmake first."
  exit 1
fi

if [[ ! -f "$MODEL" ]]; then
  notify "Translator" "Model not found. Run scripts/download-model.sh"
  exit 1
fi

server_healthy() {
  curl -sf --connect-timeout 1 --max-time 2 "http://${HOST}:${PORT}/health" >/dev/null 2>&1
}

# Stop any llama-server listening on our port (frees Jetson RAM).
stop_server_on_port() {
  local pids="" pid
  if [[ -n "${SERVER_PID}" ]]; then
    pids="$SERVER_PID"
  fi
  if command -v lsof >/dev/null 2>&1; then
    pids="$pids $(lsof -t -iTCP:"${PORT}" -sTCP:LISTEN 2>/dev/null || true)"
  elif command -v ss >/dev/null 2>&1; then
    pids="$pids $(ss -ltnp 2>/dev/null | grep -E ":${PORT}\\b" | sed -n 's/.*pid=\([0-9]\+\).*/\1/p' || true)"
  fi
  if command -v fuser >/dev/null 2>&1; then
    pids="$pids $(fuser "${PORT}/tcp" 2>/dev/null || true)"
  fi
  if command -v pgrep >/dev/null 2>&1; then
    pids="$pids $(pgrep -f "[l]lama-server.*--port ${PORT}" 2>/dev/null || true)"
  fi
  pids="$(echo "$pids" | tr -cs '0-9' '\n' | grep -E '^[0-9]+$' | sort -u || true)"
  if [[ -z "$pids" ]]; then
    return 0
  fi
  echo "Stopping llama-server on port ${PORT}: $(echo $pids)"
  for pid in $pids; do
    kill "$pid" 2>/dev/null || true
  done
  for _ in 1 2 3 4 5 6 7 8; do
    local alive=0
    for pid in $pids; do
      if kill -0 "$pid" 2>/dev/null; then alive=1; break; fi
    done
    [[ "$alive" -eq 0 ]] && break
    sleep 0.35
  done
  for pid in $pids; do
    if kill -0 "$pid" 2>/dev/null; then
      kill -9 "$pid" 2>/dev/null || true
      pkill -P "$pid" 2>/dev/null || true
    fi
  done
}

cleanup() {
  local ec=$?
  trap - EXIT INT TERM
  stop_server_on_port
  rm -f "$PIDFILE" 2>/dev/null || true
  exit "$ec"
}

trap cleanup EXIT INT TERM

# Single-instance: if already running, focus/raise is not available; just refuse.
if [[ -f "$PIDFILE" ]]; then
  old="$(awk 'NR==1{print; exit}' "$PIDFILE" 2>/dev/null || true)"
  if [[ -n "$old" ]] && kill -0 "$old" 2>/dev/null; then
    notify "Translator" "Already running"
    # Do not run cleanup stop on this early exit — clear trap
    trap - EXIT INT TERM
    exit 0
  fi
fi

if server_healthy; then
  echo "Reusing llama-server at http://${HOST}:${PORT} (will stop on GUI close)"
  STARTED_SERVER=0
  SERVER_PID=""
else
  echo "Starting llama-server…"
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
      notify "Translator" "llama-server failed — see $LOG"
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
      notify "Translator" "Timed out loading model"
      exit 1
    fi
  done
fi

cd "$ROOT"
export TRANSLATOR_CONFIG="$CONFIG"
export TRANSLATOR_ROOT="$ROOT"

# Record launcher + GUI pids for debugging
echo "$$" >"$PIDFILE"
"$GUI" &
GUI_PID=$!
echo "$GUI_PID" >>"$PIDFILE"
wait "$GUI_PID"
# GUI exited → cleanup trap stops server
