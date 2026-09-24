#!/usr/bin/env bash
# Start llama-server with Jetson Orin Nano 8GB-safe defaults for ALLaM-7B Q4_K_M.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib.sh
source "$SCRIPT_DIR/lib.sh"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ensure_config "$ROOT" || exit 1

HOST="$(get_ini "$CONFIG" server host 127.0.0.1)"
PORT="$(get_ini "$CONFIG" server port 8080)"
MODEL="$(get_ini "$CONFIG" model path /home/nox/models/allam-7b-instruct-preview-q4_k_m.gguf)"
CTX="$(get_ini "$CONFIG" model ctx_size 2048)"
NGL="$(get_ini "$CONFIG" model gpu_layers 99)"

if ! LLAMA_SERVER="$(find_llama_server)"; then
  echo "llama-server not found in PATH" >&2
  exit 1
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
