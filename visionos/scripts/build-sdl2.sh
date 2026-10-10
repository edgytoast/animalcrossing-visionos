#!/usr/bin/env bash
# Builds SDL 2 (static, no video) for visionOS into visionos/third_party/install/SDL2-<sdk>:
# audio, timers, events and gamepads for the game. The source is SDL's release-2.32.10
# tag with visionos/patches/sdl2/0001-visionos.patch (the patch says why).
#
#   visionos/scripts/build-sdl2.sh xros|xrsimulator
set -euo pipefail

sdk="${1:?usage: build-sdl2.sh xros|xrsimulator}"
root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
third="${root}/visionos/third_party"
src="${third}/SDL2"
tag="release-2.32.10"

if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
if [[ ! -d "${src}/.git" ]]; then
    mkdir -p "${third}"
    git clone --quiet --depth 1 --branch "${tag}" https://github.com/libsdl-org/SDL.git "${src}"
    git -C "${src}" apply "${root}/visionos/patches/sdl2/0001-visionos.patch"
fi
cmake -S "${src}" -B "${third}/build/SDL2-${sdk}" -G Ninja \
    -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT="${sdk}" -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="${third}/install/SDL2-${sdk}" \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF -DSDL2_DISABLE_SDL2MAIN=ON \
    -DSDL_VIDEO=OFF -DSDL_RENDER=OFF -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_METAL=OFF -DSDL_VULKAN=OFF \
    -DSDL_HIDAPI=ON -DSDL_VIRTUAL_JOYSTICK=ON >/dev/null
cmake --build "${third}/build/SDL2-${sdk}" --target install >/dev/null
echo "SDL 2: ${third}/install/SDL2-${sdk}"
