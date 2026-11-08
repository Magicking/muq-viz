#!/usr/bin/env bash
# Run ON THE TARGET: start the visualizer on the X display with an OBS layout.
# Usage: scripts/run-layout.sh [layout.json] [extra muq-viz args...]
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
layout="${1:-$here/layouts/Aeriechaffaudage1024x768_4.json}"; shift || true
export DISPLAY="${DISPLAY:-:0}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
exec "$here/bin/muq-viz" --layout "$layout" "$@"
