#!/usr/bin/env bash
# Download ALLaM-7B-Instruct Q4_K_M GGUF for llama.cpp (~4.3 GB).
set -euo pipefail

DEST_DIR="${1:-/home/nox/models}"
REPO="Omartificial-Intelligence-Space/ALLaM-7B-Instruct-preview-Q4_K_M-GGUF"
FILE="allam-7b-instruct-preview-q4_k_m.gguf"
OUT="$DEST_DIR/$FILE"
URL="https://huggingface.co/${REPO}/resolve/main/${FILE}"

mkdir -p "$DEST_DIR"

if [[ -f "$OUT" ]]; then
  echo "Already present: $OUT"
  ls -lh "$OUT"
  exit 0
fi

echo "Downloading $FILE (~4.3 GB) into $DEST_DIR ..."
echo "Source: $URL"

if command -v huggingface-cli >/dev/null 2>&1; then
  huggingface-cli download "$REPO" "$FILE" --local-dir "$DEST_DIR" --local-dir-use-symlinks False
elif command -v hf >/dev/null 2>&1; then
  hf download "$REPO" "$FILE" --local-dir "$DEST_DIR"
else
  TMP="$OUT.partial"
  curl -L --fail --retry 3 -C - -o "$TMP" "$URL"
  mv "$TMP" "$OUT"
fi

ls -lh "$OUT"
echo "Done. Point config.ini [model] path=$OUT"
echo "Then: ./scripts/start-server.sh"
