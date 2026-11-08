#!/usr/bin/env bash
# Build the portable muq-viz binary inside the Debian 12 builder image.
# Output: build-docker/muq-viz (+ test_layout)
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
image=muq-viz-builder:4.1.8-sdl2.32.10
docker build -t "$image" -f "$here/docker/Dockerfile" "$here"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$here:/src" "$image" bash -c '
  set -e
  cmake -S /src -B /src/build-docker -DCMAKE_BUILD_TYPE=Release
  cmake --build /src/build-docker -j"$(nproc)"
  ctest --test-dir /src/build-docker --output-on-failure
  strip /src/build-docker/muq-viz
  echo "--- dynamic deps:"; objdump -p /src/build-docker/muq-viz | grep NEEDED
  echo "--- max GLIBC symbol version:"; objdump -T /src/build-docker/muq-viz | grep -o "GLIBC_[0-9.]*" | sort -Vu | tail -1
'
