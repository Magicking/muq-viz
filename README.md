# muq-viz

MilkDrop preset renderer built on [libprojectM 4](https://github.com/projectM-visualizer/projectm)
(ideas from [frontend-sdl-cpp](https://github.com/projectM-visualizer/frontend-sdl-cpp)), with:

- **PipeWire capture**: control panel lists every output (captured through its monitor) and input; switch live.
- **Output modes**: `window`, `fullscreen` (any monitor), `layout` = one borderless window per
  surface of an OBS scene-collection JSON (all scene items whose name starts with `S`).
  Layout surfaces show **slices of one shared canvas** (projection-mapping style): projectM renders the
  bounding box of all surfaces once, each window shows its own rectangle.
- **Control window** (Dear ImGui): audio source, preset browser + search, next/prev/random/lock,
  shuffle, durations, beat sensitivity, output brightness (0–2, keys `[`/`]`), mode/monitor switch, (re)load layout JSON.

## OBS layout rules

For the scene `current_scene` (or `--scene`), each item with name prefix `S` (`--prefix`) and `visible`:

- size = source `settings.width/height` (default: canvas = item `scale_ref`, or `--canvas WxH`) minus crop, times `scale`;
  `bounds_type != 0` → `bounds`.
- position = `pos` corrected for `align` (left/right/top/bottom/center).
- Screen position = monitor origin (`--monitor`) or `--origin X,Y` + OBS position.

Number of surfaces and their geometry come only from the JSON. Inspect with `--print-layout`.

## Build (on dev machine)

```
scripts/build.sh      # Docker, Debian 12 toolchain -> build-docker/muq-viz
BASTION=user@bastion TARGET=user@target scripts/deploy.sh   # copy to target:~/muq-viz via bastion
```

Without Docker (host toolchain; builds static SDL2 + patched projectM once into `build-native/deps`):

```
scripts/build-native.sh                        # -> build-native/muq-viz
BIN=build-native/muq-viz BASTION=... TARGET=... scripts/deploy.sh   # deploy that binary instead
```

Host needs cmake, git, patch, pkg-config, a C++17 compiler and the X11/GL/PipeWire dev packages
(script prints the Arch/Debian package list if anything is missing). The binary links the **host**
glibc; the script warns if it needs a newer glibc than the target's 2.41 (`TARGET_GLIBC=`) — use the
Docker build for deployment then.

The binary statically contains projectM (+ a small render-to-FBO patch, `patches/`), SDL2, ImGui,
libstdc++. Runtime needs only glibc ≥ 2.36, `libGL.so.1`, `libpipewire-0.3.so.0`, X11 client libs.

## On the target

```
~/muq-viz/scripts/install-presets.sh            # once: presets -> ~/muq-viz/presets
DISPLAY=:0 ~/muq-viz/bin/muq-viz                # window
DISPLAY=:0 ~/muq-viz/bin/muq-viz -m fullscreen --monitor 1
DISPLAY=:0 ~/muq-viz/bin/muq-viz --layout layouts/Aeriechaffaudage1024x768_4.json --monitor 1
~/muq-viz/bin/muq-viz --list-sources            # PipeWire node names for --source
~/muq-viz/bin/muq-viz --list-monitors          # index + EDID name (e.g. 1: HDMI 22" = DP-0 1024x600)
```

Keys: `N` next, `P` previous, `R` random, `L` lock, `F` fullscreen toggle, `M`/`Tab` hide panel,
`Esc` (on output) or `Ctrl+Q` quit. `--help` for all options.
