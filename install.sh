#!/bin/sh
set -eu

REPO="x5f3759df/prm"
BIN_DIR="${XDG_BIN_HOME:-$HOME/.local/bin}"
CFG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/prm"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

ZIP_URL="$(curl -fsSL "https://api.github.com/repos/$REPO/releases/latest" | grep -o 'https://[^"]*\.zip' | head -n1)"
if [ -z "$ZIP_URL" ]; then
    echo "Error: could not find release zip for $REPO" >&2
    exit 1
fi

echo "Downloading $ZIP_URL ..."
curl -fsSL "$ZIP_URL" -o "$TMP_DIR/prm.zip"
unzip -qo "$TMP_DIR/prm.zip" -d "$TMP_DIR"

install -Dm755 "$TMP_DIR/prm/prm" "$BIN_DIR/prm"
if [ ! -f "$CFG_DIR/runners.conf" ]; then
    install -Dm644 "$TMP_DIR/prm/config/runners.conf" "$CFG_DIR/runners.conf"
fi

echo "Installed prm to $BIN_DIR/prm (config: $CFG_DIR/runners.conf)"
case ":$PATH:" in
    *":$BIN_DIR:"*) ;;
    *) echo "Note: add $BIN_DIR to your PATH if not already present." ;;
esac
