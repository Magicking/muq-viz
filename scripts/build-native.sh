#!/usr/bin/env bash
# Build muq-viz with the host toolchain (no Docker).
# Mirrors docker/Dockerfile + scripts/build.sh: static SDL2 + patched static projectM
# are built once into $BUILD_DIR/deps, then the app is built into $BUILD_DIR.
# Output: build-native/muq-viz (+ test_layout)
#
# Note: the binary links against the HOST glibc. For deployment to the target
# (glibc 2.41) prefer scripts/build.sh unless the host glibc is <= target.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"

SDL_TAG="${SDL_TAG:-release-2.32.10}"
PROJECTM_TAG="${PROJECTM_TAG:-v4.1.8}"
BUILD_DIR="${BUILD_DIR:-$here/build-native}"
DEPS="${DEPS:-$BUILD_DIR/deps}"
JOBS="${JOBS:-$(nproc)}"
TARGET_GLIBC="${TARGET_GLIBC:-2.41}"
PATCH="$here/patches/projectm-4.1.8-render-fbo.patch"

# --- preflight -------------------------------------------------------------
missing=()
for t in cmake git patch pkg-config c++ cc objdump strip sha256sum; do
    command -v "$t" >/dev/null || missing+=("$t")
done
if command -v pkg-config >/dev/null; then
    for p in libpipewire-0.3 gl x11; do
        pkg-config --exists "$p" || missing+=("pkg-config:$p")
    done
fi
if [ ${#missing[@]} -gt 0 ]; then
    cat >&2 <<EOF
missing: ${missing[*]}
Install build dependencies, e.g.
  Arch:   pacman -S --needed base-devel cmake git pkgconf libpipewire libglvnd libx11 libxext \\
              libxrandr libxcursor libxi libxinerama libxss libxfixes libxxf86vm libxkbcommon
  Debian: apt install build-essential cmake git pkg-config patch libgl-dev libegl-dev libx11-dev \\
              libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxinerama-dev libxss-dev \\
              libxfixes-dev libxxf86vm-dev libxkbcommon-dev libpipewire-0.3-dev
EOF
    exit 1
fi

mkdir -p "$DEPS" "$BUILD_DIR/src"

# --- SDL2 (static; X11 libs dlopen()ed at runtime) ---------------------------
sdl_stamp="$DEPS/.sdl-$SDL_TAG"
if [ ! -f "$sdl_stamp" ]; then
    src="$BUILD_DIR/src/sdl"
    rm -rf "$src"
    git clone --depth 1 -b "$SDL_TAG" https://github.com/libsdl-org/SDL "$src"
    cmake -S "$src" -B "$src/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEPS" \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF \
        -DSDL_AUDIO=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_VULKAN=OFF -DSDL_X11_SHARED=ON
    cmake --build "$src/build" -j"$JOBS"
    cmake --install "$src/build"
    rm -rf "$src"
    touch "$sdl_stamp"
else
    echo "--- SDL2 $SDL_TAG: cached"
fi

# --- projectM 4 (static, render-to-FBO patch) --------------------------------
patch_sum="$(sha256sum "$PATCH" | cut -c1-12)"
pm_stamp="$DEPS/.projectm-$PROJECTM_TAG-$patch_sum"
if [ ! -f "$pm_stamp" ]; then
    src="$BUILD_DIR/src/projectm"
    rm -rf "$src"
    git clone --depth 1 -b "$PROJECTM_TAG" --recurse-submodules --shallow-submodules \
        https://github.com/projectM-visualizer/projectm "$src"
    patch -d "$src" -p1 < "$PATCH"
    cmake -S "$src" -B "$src/build" -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$DEPS" \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DENABLE_PLAYLIST=ON \
        -DENABLE_SYSTEM_PROJECTM_EVAL=OFF -DENABLE_SYSTEM_GLM=OFF -DOpenGL_GL_PREFERENCE=LEGACY \
        -DBUILD_TESTING=OFF -DENABLE_SDL_UI=OFF
    cmake --build "$src/build" -j"$JOBS"
    cmake --install "$src/build"
    rm -rf "$src"
    rm -f "$DEPS"/.projectm-*
    touch "$pm_stamp"
else
    echo "--- projectM $PROJECTM_TAG: cached"
fi

# --- muq-viz -----------------------------------------------------------------
cmake -S "$here" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$DEPS"
cmake --build "$BUILD_DIR" -j"$JOBS"
ctest --test-dir "$BUILD_DIR" --output-on-failure
strip "$BUILD_DIR/muq-viz"

echo "--- dynamic deps:"; objdump -p "$BUILD_DIR/muq-viz" | grep NEEDED
max_glibc="$(objdump -T "$BUILD_DIR/muq-viz" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -1)"
echo "--- max GLIBC symbol version: $max_glibc"
if [ "$(printf '%s\n%s\n' "${max_glibc#GLIBC_}" "$TARGET_GLIBC" | sort -V | tail -1)" != "$TARGET_GLIBC" ]; then
    echo "WARNING: needs $max_glibc > target glibc $TARGET_GLIBC; use scripts/build.sh for deployment." >&2
fi
echo "--- built: $BUILD_DIR/muq-viz"
