#!/usr/bin/env bash
# Run ON THE TARGET: fetch the MilkDrop preset packs (pinned commits) into ~/muq-viz/presets.
# Needs only git (no root, no packages).
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)/presets"
mkdir -p "$root"

fetch() { # name repo commit
    local dir="$root/$1"
    if [ ! -d "$dir/.git" ]; then
        git clone --filter=blob:none "https://github.com/projectM-visualizer/$2" "$dir"
    fi
    git -C "$dir" fetch --quiet origin
    git -C "$dir" -c advice.detachedHead=false checkout --quiet "$3"
    echo "$1: $(git -C "$dir" rev-parse --short HEAD), $(find "$dir" -name '*.milk' | wc -l) presets, $(find "$dir" -iname '*.jpg' -o -iname '*.png' | wc -l) textures"
}

fetch milkdrop-original presets-milkdrop-original     e03b83e3338d8f1ed6cbcf908c719f249ef24288
fetch texture-pack      presets-milkdrop-texture-pack 6368812f27bc747b517218fbf89d21d59afce4d9
fetch cream-of-the-crop presets-cream-of-the-crop     0180df21f5e0bd39b9060cc5de420ed2f1f9e509
