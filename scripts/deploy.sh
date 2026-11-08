#!/usr/bin/env bash
# Copy sources + built binary to the target through the bastion.
# Config: .env at repo root (see .env.example); environment variables override it.
# Target layout: ~/muq-viz (git work tree), binary in ~/muq-viz/bin/muq-viz
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
if [ -f "$here/.env" ]; then
    while IFS='=' read -r k v; do
        k="${k//[[:space:]]/}"; v="${v%%#*}"; v="${v%"${v##*[![:space:]]}"}"
        case "$k" in BASTION|BASTION_PORT|TARGET|DEST|BIN) [ -n "${!k:-}" ] || export "$k=$v" ;; esac
    done < "$here/.env"
fi
BASTION="${BASTION:?set BASTION in .env (see .env.example)}"
BASTION_PORT="${BASTION_PORT:-22}"
TARGET="${TARGET:?set TARGET in .env (see .env.example)}"
DEST="${DEST:-muq-viz}"
BIN="${BIN:-$here/build-docker/muq-viz}"   # e.g. BIN=build-native/muq-viz

[ -x "$BIN" ] || { echo "build first: scripts/build.sh (or build-native.sh + BIN=...)" >&2; exit 1; }

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/bin"
tar -C "$here" --exclude=./build-docker --exclude=./build-native --exclude=./build --exclude=./.git --exclude=./presets -cf - . | tar -C "$stage" -xf -
cp "$BIN" "$stage/bin/muq-viz"

tar -C "$stage" -czf - . | ssh -p "$BASTION_PORT" "$BASTION" \
    "ssh $TARGET 'mkdir -p ~/$DEST && tar -C ~/$DEST -xzf - && echo deployed to \$(hostname):~/$DEST'"
