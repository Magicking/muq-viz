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

- **Web UI** (always on, `http://<host>:8080/`): same controls as the control window, from any
  phone/laptop on the LAN; both stay in sync.
- **Config file** (INI): *Save config* in either UI, loaded automatically at start.

## Web UI

Embedded HTTP server (cpp-httplib, page compiled into the binary). Listens on `0.0.0.0:8080` by default.
**No authentication**: anyone who can reach the port can control the visualizer — keep it on a trusted
network or use `--web-bind 127.0.0.1` (+ SSH tunnel).

```
--no-web            disable
--web-port N        port (default 8080)
--web-bind ADDR     listen address (default 0.0.0.0)
```

API (used by the page, handy for scripts):

```
GET  /api/state                     full state (audio, preset, output, config, info)
GET  /api/presets                   {"version": N, "labels": [...]}  (index = position)
POST /api/cmd  {"cmd": "next"}      also: prev, random, saveConfig
               {"cmd": "play", "index": 42}
               {"cmd": "set", "key": "brightness", "value": 0.8}
                   keys: lock shuffle smooth hardCuts (bool), duration softCut beatSensitivity brightness (number)
               {"cmd": "source", "name": "<node.name>"}   ("" = default output monitor)
               {"cmd": "mode", "mode": "window|fullscreen|layout"}
               {"cmd": "monitor", "index": 1}
               {"cmd": "loadLayout", "path": "layouts/x.json"}   (path on the visualizer host)
               {"cmd": "ui", "visible": false}            (show/hide the control window)

curl -XPOST -d '{"cmd":"next"}' http://192.168.1.53:8080/api/cmd
```

## Config file

Default `$XDG_CONFIG_HOME/muq-viz/muq-viz.ini` (else `~/.config/muq-viz/muq-viz.ini`); loaded at start,
written by *Save config* (control window *Config* section, web UI, or `Ctrl+S`). Precedence:
built-in defaults < config file < command line. `--config FILE` uses another file (load + save),
`--no-config` skips loading. Comments (`;` or `#`) must be on their own line. Saving stores the live state: current mode, audio source, current preset
(resumed at next start), sliders/toggles, monitor, layout path.

```ini
[output]
; window | fullscreen | layout
mode = layout
; index or name substring
monitor = 1
; X,Y (empty = monitor origin)
origin =
size = 1280x720
fps = 60
vsync = false
override_redirect = true
brightness = 1
layout = /home/muq/muq-viz/layouts/Aeriechaffaudage1024x768_4.json
prefix = S
scene =
; WxH (empty = from item scale_ref)
canvas =

[audio]
; PipeWire node.name, empty = default output monitor
source =

[presets]
; colon-separated, empty = auto-detect
dirs =
; start preset (exact label, else substring)
preset = cream-of-the-crop/...
duration = 30
transition = 3
shuffle = true
smooth = true
hard_cuts = false
lock = false
beat_sensitivity = 1

[ui]
show_ui = true
ui_scale = 1

[web]
enabled = true
bind = 0.0.0.0
port = 8080
```

A layout that fails to load at start (e.g. stale path in the config) no longer aborts: muq-viz falls back
to window mode so another layout can be loaded from the UI.

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
cp .env.example .env  # once: set BASTION / BASTION_PORT / TARGET
scripts/deploy.sh     # copy to target:~/muq-viz via bastion
```

Without Docker (host toolchain; builds static SDL2 + patched projectM once into `build-native/deps`):

```
scripts/build-native.sh                        # -> build-native/muq-viz
BIN=build-native/muq-viz scripts/deploy.sh     # deploy that binary instead
```

Host needs cmake, git, patch, pkg-config, a C++17 compiler and the X11/GL/PipeWire dev packages
(script prints the Arch/Debian package list if anything is missing). The binary links the **host**
glibc; the script warns if it needs a newer glibc than the target's 2.41 (`TARGET_GLIBC=`) — use the
Docker build for deployment then.

The binary statically contains projectM (+ a small render-to-FBO patch, `patches/`), SDL2, ImGui,
cpp-httplib + the web page, libstdc++. Runtime needs only glibc ≥ 2.36, `libGL.so.1`, `libpipewire-0.3.so.0`, X11 client libs.

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
`Ctrl+S` save config, `Esc` (on output) or `Ctrl+Q` quit. `--help` for all options.
