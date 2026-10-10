# visionOS: the game as ACVRVisionGame.framework, included from pc/CMakeLists.txt
# when CMAKE_SYSTEM_NAME is visionOS (visionos/scripts/build-visionos.sh).
#
# The framework is the game plus the host behind the acvr_visionos_* bridge
# (visionos/host), and exports only the bridge (visionos/exports.txt).
#
# The game replaces the global operator new/delete with its own heap
# (JKRHeap.cpp). Linked into one image with the host, the host's C++ (the
# window's frame builder and scene mirror, on the app's threads, before the game
# heap exists) would allocate from it too, and JKRHeap returns NULL without a
# current heap and isn't thread-safe. So the game is first linked on its own
# into one relocatable object (ld -r) that keeps only its C interface global:
# its operator new/delete and every C++ template instance it made become local
# to it, and the host links against the system's.

set(ACVR_VISIONOS_DIR "${DECOMP_ROOT}/visionos")
set(ACVR_HOST_DIR "${ACVR_VISIONOS_DIR}/host")

# The game: everything the macOS executable has, as one static library.
# Its main() runs on the host's game thread as ac_pc_main.
add_library(ac_game STATIC
    ${PC_SOURCES}
    ${FIXNES_SOURCES}
    ${GAME_C_SOURCES}
    ${GAME_CXX_SOURCES}
)
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/src/pc_main.c
    PROPERTIES COMPILE_DEFINITIONS "main=ac_pc_main")
target_compile_definitions(ac_game PRIVATE PC_VISIONOS)
if(TARGET SDL2::SDL2-static)
    target_include_directories(ac_game PRIVATE $<TARGET_PROPERTY:SDL2::SDL2-static,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Stereo: the VR mod's layer (pc/src/pc_vr.cpp) speaks OpenXR to the visionOS
# provider (visionos/openxr-provider, GPL-3.0-or-later: the app is GPL-3.0). There
# is no OpenXR loader or runtime on visionOS; only the Khronos headers are needed,
# the release TPVR pins.
include(FetchContent)
FetchContent_Declare(openxr_headers
    URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.61.tar.gz
    URL_HASH SHA256=d5e2773d282642e6e250bd10266f316fc3ff3b8602c83db009a1f434205998e6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR include-only-no-cmake)
FetchContent_MakeAvailable(openxr_headers)
set(ACVR_XR_PROVIDER_DIR "${ACVR_VISIONOS_DIR}/openxr-provider")
target_compile_definitions(ac_game PRIVATE PC_VR_OPENXR PC_MIRROR_HOST)
# The Window view's capture (pc/src/pc_mirror.cpp) hands its draws to the host's scene mirror.
target_include_directories(ac_game PRIVATE "${ACVR_HOST_DIR}")
target_include_directories(ac_game PRIVATE "${ACVR_XR_PROVIDER_DIR}/include")
target_include_directories(ac_game SYSTEM PRIVATE "${openxr_headers_SOURCE_DIR}/include")

# The game in one relocatable object. ACVR_LINK_TARGET is the clang target
# triple (arm64-apple-xros26.0[-simulator]), from the build script.
if(NOT ACVR_LINK_TARGET)
    message(FATAL_ERROR "ACVR_LINK_TARGET (e.g. arm64-apple-xros26.0-simulator) is required")
endif()
set(ACVR_GAME_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/ac_game_prelinked.o")
set(ACVR_GAME_EXPORTS "${ACVR_VISIONOS_DIR}/game_exports.txt")
add_custom_command(
    OUTPUT "${ACVR_GAME_OBJECT}"
    COMMAND "${CMAKE_CXX_COMPILER}" -r -nostdlib "--target=${ACVR_LINK_TARGET}" -isysroot "${CMAKE_OSX_SYSROOT}"
            "-Wl,-force_load,$<TARGET_FILE:ac_game>"
            "-Wl,-exported_symbols_list,${ACVR_GAME_EXPORTS}"
            -o "${ACVR_GAME_OBJECT}"
    DEPENDS ac_game "${ACVR_GAME_EXPORTS}"
    COMMENT "Linking the game into one object (its operator new stays its own)"
    VERBATIM
)
set_source_files_properties("${ACVR_GAME_OBJECT}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)

set(ACVR_HOST_SOURCES
    "${ACVR_HOST_DIR}/game_host.mm"
    "${ACVR_HOST_DIR}/scene_mirror.cpp"
    "${ACVR_HOST_DIR}/window_frames.mm"
    "${ACVR_HOST_DIR}/sense_pad.mm"
)
set_source_files_properties(${ACVR_HOST_SOURCES} PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-std=c++20;-Wno-unused-parameter")

# The provider runs on the host's side of the prelink (ARKit and GameController call
# it on their own threads), with the system allocator.
set(ACVR_XR_PROVIDER_SOURCES
    "${ACVR_XR_PROVIDER_DIR}/src/xr_visionos_runtime.mm"
    "${ACVR_XR_PROVIDER_DIR}/src/xr_visionos_input.mm"
    "${ACVR_XR_PROVIDER_DIR}/src/xr_visionos_hand_tracking.mm"
    "${ACVR_XR_PROVIDER_DIR}/src/xr_visionos_compositor.mm"
    "${ACVR_XR_PROVIDER_DIR}/src/xr_visionos_controllers.mm"
    "${ACVR_XR_PROVIDER_DIR}/src/xr_visionos_smaa.mm"
)
set_source_files_properties(${ACVR_XR_PROVIDER_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc;-std=c++20;-w"
    COMPILE_DEFINITIONS "XR_VISIONOS_TEST_ACTIONS_ENV=\"ACVR_TEST_XR_ACTIONS\";XR_VISIONOS_GPU_TIMING_ENV=\"ACVR_GPU_TIMING\"")
list(APPEND ACVR_HOST_SOURCES ${ACVR_XR_PROVIDER_SOURCES})

add_library(ACVRVisionGame SHARED ${ACVR_HOST_SOURCES} "${ACVR_GAME_OBJECT}")
target_include_directories(ACVRVisionGame PRIVATE "${ACVR_HOST_DIR}" "${ACVR_XR_PROVIDER_DIR}/include")
target_include_directories(ACVRVisionGame SYSTEM PRIVATE "${openxr_headers_SOURCE_DIR}/include")
set_target_properties(ACVRVisionGame PROPERTIES
    FRAMEWORK TRUE
    MACOSX_FRAMEWORK_IDENTIFIER dev.acvr.vision.game
    MACOSX_FRAMEWORK_BUNDLE_VERSION 1
    MACOSX_FRAMEWORK_SHORT_VERSION_STRING 0.1.0
    OUTPUT_NAME ACVRVisionGame
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    INSTALL_NAME_DIR "@rpath"
    BUILD_WITH_INSTALL_RPATH TRUE
)
target_link_options(ACVRVisionGame PRIVATE
    "-Wl,-exported_symbols_list,${ACVR_VISIONOS_DIR}/exports.txt")
target_link_libraries(ACVRVisionGame PRIVATE
    SDL2::SDL2-static
    "-framework Foundation" "-framework Metal" "-framework QuartzCore" "-framework IOSurface"
    "-framework CompositorServices" "-framework ARKit" "-framework GameController" "-framework UIKit"
    "-framework AVFoundation" "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreHaptics"
    "-framework CoreMotion" "-framework CoreBluetooth" "-framework CoreFoundation"
)
