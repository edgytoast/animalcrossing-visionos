#!/usr/bin/env bash
# Builds Animal Crossing VR for Apple Vision Pro: the game (ACVRVisionGame.framework),
# then the SwiftUI app around it (xcodegen + xcodebuild), and optionally installs
# it on the paired headset.
#
#   visionos/scripts/build-visionos.sh --team TEAMID [--bundle-id ID] [--debug]
#                                      [--install [--device UDID]] [--game-only]
#   visionos/scripts/build-visionos.sh --simulator [--debug] [--game-only]
#
# --simulator builds for the visionOS Simulator, unsigned (no team needed).
# The bundle ID defaults to dev.acvr.vision.<TEAMID>: a bare ID may already be
# registered to another team, and automatic signing then falls back to a wildcard
# profile without the memory entitlement.
set -euo pipefail

root="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
visionos="${root}/visionos"
team=""
bundle_id=""
config="RelWithDebInfo"
xcode_config="Release"
install=0
device=""
game_only=0
simulator=0
stub=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --team) team="${2:?}"; shift 2 ;;
        --bundle-id) bundle_id="${2:?}"; shift 2 ;;
        --debug) config="Debug"; xcode_config="Debug"; shift ;;
        --install) install=1; shift ;;
        --device) device="${2:?}"; shift 2 ;;
        --game-only) game_only=1; shift ;;
        --simulator) simulator=1; shift ;;
        -h|--help) sed -n '2,13p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

# CMake's Xcode lookup breaks when the Xcode path has a space in it; keep the
# active Xcode at /Applications/Xcode.app.
if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
for tool in xcodegen xcrun cmake ninja; do
    command -v "${tool}" >/dev/null || { echo "ERROR: ${tool} not found (brew install ${tool})" >&2; exit 1; }
done
if [[ ${simulator} -eq 1 ]]; then flavour="xrsimulator"; else flavour="visionos"; fi
build="${root}/.scratch/build-${flavour}-${config}"

# 1. The game framework.
build_game() {
    # The decomp plus the PC port, with Apple Clang for visionOS: pc/CMakeLists.txt's
    # visionOS branch (pc/cmake/visionos_framework.cmake) makes ACVRVisionGame.framework.
    # SDL 2 (static, no video: audio, timers, gamepads) comes from visionos/third_party,
    # built once by visionos/scripts/build-sdl2.sh.
    local sdk triple sdl
    if [[ ${simulator} -eq 1 ]]; then
        sdk="xrsimulator"; triple="arm64-apple-xros26.0-simulator"
    else
        sdk="xros"; triple="arm64-apple-xros26.0"
    fi
    sdl="${visionos}/third_party/install/SDL2-${sdk}"
    [[ -f "${sdl}/lib/libSDL2.a" ]] || "${visionos}/scripts/build-sdl2.sh" "${sdk}"
    cmake -S "${root}/pc" -B "${build}" -G Ninja \
        -DCMAKE_SYSTEM_NAME=visionOS -DCMAKE_OSX_SYSROOT="${sdk}" -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=26.0 -DCMAKE_BUILD_TYPE="${config}" \
        -DACVR_LINK_TARGET="${triple}" -DSDL2_DIR="${sdl}/lib/cmake/SDL2" \
        -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=ON \
        "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local;${HOME}/miniconda3;${HOME}/anaconda3" \
        >/dev/null
    cmake --build "${build}" --target ACVRVisionGame --parallel "$(sysctl -n hw.ncpu)"
}
if [[ ${stub} -eq 0 ]]; then
    build_game
    [[ -d "${build}/ACVRVisionGame.framework" ]] || {
        echo "ERROR: ${build}/ACVRVisionGame.framework was not produced" >&2; exit 1; }
    echo "Game framework: ${build}/ACVRVisionGame.framework"
fi
[[ ${game_only} -eq 1 ]] && exit 0

# 2. The app. The launcher's kit is vendored, never fetched at build time.
[[ -f "${visionos}/TrevorbiltKit/Package.swift" ]] || {
    echo "ERROR: visionos/TrevorbiltKit is missing: run visionos/scripts/sync-trevorbilt-kit.sh" >&2; exit 1; }
# Clean-built every time: Xcode's incremental build (especially in a
# shared custom build location) has kept stale embedded frameworks and stale
# Swift objects. The app is a handful of files; the clean costs seconds.
# A clean of one platform's products can delete the other's in a shared build
# location, so rebuild the Simulator app before Simulator tests after a device build.
#
# Where xcodebuild put the product: Xcode's "Custom" build-location preference
# (IDEBuildLocationStyle) overrides -derivedDataPath, so SYMROOT/OBJROOT keep this
# port's products out of the shared location other ports build into; ask xcodebuild.
products_dir() {
    xcodebuild "$@" -showBuildSettings 2>/dev/null | awk -F' = ' '/^ *BUILT_PRODUCTS_DIR = / {print $2; exit}'
}
app_dir="${visionos}/App"
if [[ ${simulator} -eq 1 ]]; then
    export ACVR_TEAM="${team:-NONE}" ACVR_BUNDLE_ID="${bundle_id:-dev.acvr.vision.simulator}" ACVR_GAME_BUILD_DIR="${build}"
    (cd "${app_dir}" && xcodegen generate --quiet)
    args=(-project "${app_dir}/ACVRVision.xcodeproj" -scheme ACVRVision -configuration "${xcode_config}"
          -destination 'generic/platform=visionOS Simulator' -derivedDataPath "${root}/.scratch/DerivedData-simulator"
          SYMROOT="${root}/.scratch/DerivedData-simulator/Products" OBJROOT="${root}/.scratch/DerivedData-simulator/Intermediates"
          ARCHS=arm64 CODE_SIGNING_ALLOWED=NO)
    xcodebuild "${args[@]}" clean build
    echo "App: $(products_dir "${args[@]}")/ACVRVision.app"
    exit 0
fi
[[ -n "${team}" ]] || { echo "ERROR: --team TEAMID is needed to sign the app" >&2; exit 2; }
[[ -n "${bundle_id}" ]] || bundle_id="dev.acvr.vision.${team}"
export ACVR_TEAM="${team}" ACVR_BUNDLE_ID="${bundle_id}" ACVR_GAME_BUILD_DIR="${build}"
(cd "${app_dir}" && xcodegen generate --quiet)
args=(-project "${app_dir}/ACVRVision.xcodeproj" -scheme ACVRVision -configuration "${xcode_config}"
      -destination 'generic/platform=visionOS' -derivedDataPath "${root}/.scratch/DerivedData"
      SYMROOT="${root}/.scratch/DerivedData/Products" OBJROOT="${root}/.scratch/DerivedData/Intermediates"
      DEVELOPMENT_TEAM="${team}" PRODUCT_BUNDLE_IDENTIFIER="${bundle_id}")
xcodebuild "${args[@]}" -allowProvisioningUpdates clean build
app="$(products_dir "${args[@]}")/ACVRVision.app"
echo "App: ${app}"

# 3. Install. A locked or asleep headset makes devicectl hang: wake and unlock it first.
if [[ ${install} -eq 1 ]]; then
    if [[ -z "${device}" ]]; then
        json="$(mktemp)"
        xcrun devicectl list devices --json-output "${json}" >/dev/null 2>&1 || true
        device="$(python3 - "${json}" <<'PY'
import json, sys
try:
    devices = json.load(open(sys.argv[1]))["result"]["devices"]
except Exception:
    devices = []
for d in devices:
    hw = d.get("hardwareProperties", {})
    if hw.get("platform") == "visionOS" and hw.get("reality") == "physical":
        print(hw.get("udid", d.get("identifier", "")))
        break
PY
)"
        rm -f "${json}"
    fi
    [[ -n "${device}" ]] || { echo "ERROR: no paired Apple Vision Pro found; pass --device UDID" >&2; exit 1; }
    xcrun devicectl device install app --device "${device}" "${app}"
fi
