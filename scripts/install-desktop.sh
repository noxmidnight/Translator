#!/usr/bin/env bash
# Install Translator desktop launcher + icons for the current user.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICON_BASE="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor"
DESKTOP_SRC="$ROOT/assets/translator.desktop"
DESKTOP_DST="$APP_DIR/translator.desktop"

chmod +x "$ROOT/scripts/desktop-launch.sh" "$ROOT/scripts/translator.sh"

# Rewrite Exec/Icon/Path to this machine's ROOT (portable install).
mkdir -p "$APP_DIR"
sed \
  -e "s|^Exec=.*|Exec=$ROOT/scripts/desktop-launch.sh|" \
  -e "s|^Icon=.*|Icon=$ROOT/assets/translator.png|" \
  -e "s|^Path=.*|Path=$ROOT|" \
  "$DESKTOP_SRC" >"$DESKTOP_DST"
chmod 644 "$DESKTOP_DST"

# Install multi-resolution icons (hicolor)
for size in 16 24 32 48 64 128 256 512; do
  src="$ROOT/assets/icons/translator-${size}.png"
  if [[ -f "$src" ]]; then
    dest_dir="$ICON_BASE/${size}x${size}/apps"
    mkdir -p "$dest_dir"
    cp -f "$src" "$dest_dir/translator.png"
  fi
done
# Scalable SVG
if [[ -f "$ROOT/assets/translator.svg" ]]; then
  mkdir -p "$ICON_BASE/scalable/apps"
  cp -f "$ROOT/assets/translator.svg" "$ICON_BASE/scalable/apps/translator.svg"
fi

# Prefer named icon in desktop file after hicolor install
sed -i 's|^Icon=.*|Icon=translator|' "$DESKTOP_DST"

if command -v update-desktop-database >/dev/null 2>&1; then
  update-desktop-database "$APP_DIR" 2>/dev/null || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
  gtk-update-icon-cache -f -t "$ICON_BASE" 2>/dev/null || true
fi

# Optional: desktop shortcut
if [[ "${1:-}" == "--desktop" ]]; then
  DESK="$HOME/Desktop"
  if [[ -d "$DESK" ]]; then
    cp -f "$DESKTOP_DST" "$DESK/translator.desktop"
    chmod +x "$DESK/translator.desktop" 2>/dev/null || true
    # Mark trusted on some GNOME versions
    if command -v gio >/dev/null 2>&1; then
      gio set "$DESK/translator.desktop" metadata::trusted true 2>/dev/null || true
    fi
    echo "Desktop shortcut: $DESK/translator.desktop"
  fi
fi

echo "Installed: $DESKTOP_DST"
echo "Icons:     $ICON_BASE/*/apps/translator.*"
echo "Launch from the app menu as \"Translator\", or:"
echo "  $ROOT/scripts/desktop-launch.sh"
