#!/usr/bin/env bash
# Shared helpers for Translator shell scripts. Source from other scripts:
#   # shellcheck source=lib.sh
#   source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib.sh"

translator_root() {
  cd "$(dirname "${BASH_SOURCE[1]}")/.." && pwd
}

ensure_config() {
  # Sets CONFIG from TRANSLATOR_CONFIG or $1/config.ini (ROOT).
  local root="$1"
  CONFIG="${TRANSLATOR_CONFIG:-$root/config.ini}"
  if [[ ! -f "$CONFIG" ]]; then
    if [[ -f "$root/config.ini.example" ]]; then
      cp "$root/config.ini.example" "$root/config.ini"
      CONFIG="$root/config.ini"
      echo "Created $CONFIG from example."
    else
      echo "Missing config.ini" >&2
      return 1
    fi
  fi
}

get_ini() {
  # Usage: get_ini <config-file> <section> <key> [default]
  local file="$1" section="$2" key="$3" default="${4:-}"
  awk -F'=' -v s="[$section]" -v k="$key" -v d="$default" '
    $0 ~ /^\[/ { insec = ($0 == s) }
    insec && $1 ~ "^[[:space:]]*"k"[[:space:]]*$" {
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; found=1; exit
    }
    END { if (!found) print d }
  ' "$file"
}

find_llama_server() {
  if [[ -n "${LLAMA_SERVER:-}" ]] && command -v "$LLAMA_SERVER" >/dev/null 2>&1; then
    echo "$LLAMA_SERVER"
    return 0
  fi
  if command -v llama-server >/dev/null 2>&1; then
    echo llama-server
    return 0
  fi
  if [[ -x /usr/local/bin/llama-server ]]; then
    echo /usr/local/bin/llama-server
    return 0
  fi
  return 1
}
