#!/usr/bin/env bash
# Download MUSE Arabic–English bilingual dictionary for lexical RAG.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST_DIR="${1:-$ROOT/data}"
OUT="$DEST_DIR/muse_ar_en.txt"
URL="https://dl.fbaipublicfiles.com/arrival/dictionaries/ar-en.txt"

mkdir -p "$DEST_DIR"

if [[ -f "$OUT" && -s "$OUT" ]]; then
  echo "Already present: $OUT ($(wc -l < "$OUT") lines)"
  ls -lh "$OUT"
  exit 0
fi

echo "Downloading MUSE ar-en dictionary…"
TMP="$OUT.partial"
curl -L --fail --retry 3 -o "$TMP" "$URL"
mv "$TMP" "$OUT"
wc -l "$OUT"
ls -lh "$OUT"
echo "Done. Lexicon path: $OUT"
