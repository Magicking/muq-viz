# Build environment: Debian 12 (glibc 2.36) so the binary runs on older/newer glibc targets (target: Void, glibc 2.41).
FROM debian:12.12-slim

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential cmake git ca-certificates pkg-config xz-utils patch \
        libgl-dev libegl-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev \
        libxinerama-dev libxss-dev libxfixes-dev libxxf86vm-dev libxkbcommon-dev \
        libpipewire-0.3-dev \
    && rm -rf /var/lib/apt/lists/*

ARG SDL_TAG=release-2.32.10
ARG PROJECTM_TAG=v4.1.8
ENV DEPS=/opt/deps

# SDL2, static. X11 client libs are dlopen()ed at runtime, so only libc/libdl remain as hard deps.
RUN git clone --depth 1 -b ${SDL_TAG} https://github.com/libsdl-org/SDL /tmp/sdl \
    && cmake -S /tmp/sdl -B /tmp/sdl/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${DEPS} \
        -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON -DSDL_TEST=OFF \
        -DSDL_AUDIO=OFF -DSDL_WAYLAND=OFF -DSDL_KMSDRM=OFF -DSDL_VULKAN=OFF -DSDL_X11_SHARED=ON \
    && cmake --build /tmp/sdl/build -j"$(nproc)" && cmake --install /tmp/sdl/build && rm -rf /tmp/sdl

# projectM 4, static, with render-to-FBO patch.
COPY patches /tmp/patches
RUN git clone --depth 1 -b ${PROJECTM_TAG} --recurse-submodules --shallow-submodules \
        https://github.com/projectM-visualizer/projectm /tmp/projectm \
    && cd /tmp/projectm && patch -p1 < /tmp/patches/projectm-4.1.8-render-fbo.patch \
    && cmake -S /tmp/projectm -B /tmp/projectm/build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=${DEPS} \
        -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DENABLE_PLAYLIST=ON \
        -DENABLE_SYSTEM_PROJECTM_EVAL=OFF -DENABLE_SYSTEM_GLM=OFF -DOpenGL_GL_PREFERENCE=LEGACY \
        -DBUILD_TESTING=OFF -DENABLE_SDL_UI=OFF \
    && cmake --build /tmp/projectm/build -j"$(nproc)" && cmake --install /tmp/projectm/build && rm -rf /tmp/projectm

ENV CMAKE_PREFIX_PATH=/opt/deps
WORKDIR /src
