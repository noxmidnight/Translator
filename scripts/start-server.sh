#!/usr/bin/env bash
# Start llama-server with Jetson Orin Nano 8GB-safe defaults for ALLaM-7B Q4_K_M.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONFIG="${TRANSLATOR_CONFIG:-$ROOT/config.ini}"

if [[ ! -f "$CONFIG" ]]; then
  if [[ -f "$ROOT/config.ini.example" ]]; then
    cp "$ROOT/config.ini.example" "$ROOT/config.ini"
    CONFIG="$ROOT/config.ini"
    echo "Created $CONFIG from example — edit model path if needed."
  else
    echo "Missing config.ini" >&2
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
    echo "llama-server not found in PATH" >&2
    exit 1
  fi
fi

if [[ ! -f "$MODEL" ]]; then
  echo "Model not found: $MODEL" >&2
  echo "Run: ./scripts/download-model.sh" >&2
  exit 1
fi

echo "Starting $LLAMA_SERVER"
echo "  model: $MODEL"
echo "  listen: http://${HOST}:${PORT}"
echo "  ctx: $CTX  ngl: $NGL"
echo "Tip: close browsers/IDEs first — need ~6 GB free for ALLaM-7B Q4 on 8GB Jetson."

exec "$LLAMA_SERVER" \
  -m "$MODEL" \
  --host "$HOST" \
  --port "$PORT" \
  -c "$CTX" \
  -ngl "$NGL" \
  -fa on \
  --jinja
