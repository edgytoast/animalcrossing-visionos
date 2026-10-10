// The host behind the acvr_visionos_* bridge, compiled INTO ACVRVisionGame.framework
// (pc/CMakeLists.txt, visionOS), with -fobjc-arc. From the scaffold's
// game_host.mm template; Animal Crossing's parts:
//   - pc_main.c's main() is renamed ac_pc_main and runs on a 16 MB pthread, its
//     working directory Documents (rom/, save/, settings.ini are relative).
//   - The game renders through the GL-on-Metal shim (pc/src/pc_gl_metal.mm). Each
//     finished frame ends here (g_pc_metal_present_hook): in the window it's copied
//     into the window's IOSurface slot (window_frames) in the frame's command buffer.
//   - SDL2 (static, no video) gives audio, timers and controllers.
//   - Full and Progressive: the VR mod's layer (pc/src/pc_vr.cpp) speaks OpenXR to the
//     vendored provider (visionos/openxr-provider), which this host hands the layer
//     renderer, spatial events and the safety boundary. The game runs with --vr; in
//     the window with --no-vr.

#include "acvr_visionos.h"
#include "scene_mirror.h"
#include "window_frames.h"
#include "sense_pad.h"
#include "vr/visionos/xr_visionos.h"

#define SDL_MAIN_HANDLED 1
#include <SDL.h>

#import <CompositorServices/CompositorServices.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <objc/objc.h>

#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

extern "C" {
int ac_pc_main(int argc, char* argv[]);
void pc_audio_set_paused(int paused);
extern void* g_pc_metal_device;
extern void (*g_pc_metal_present_hook)(void);
extern int g_pc_window_w;
extern int g_pc_window_h;
extern int g_pc_running;
void pcm_present_ex(void (*encode)(void* command_buffer, void* color_texture, void* ctx), void* encode_ctx,
                    void (*on_complete)(void* ctx), void* ctx);
void pcm_set_default_framebuffer(void* color_texture, int width, int height);
void pcm_encode_with_default_framebuffer(void (*fn)(void* command_buffer, void* color, void* depth, void* ctx),
                                         void* ctx);
void* pcm_make_target(int width, int height);
void* objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void* pool);
}

namespace {

constexpr size_t kGameThreadStackSize = 16 * 1024 * 1024;
// The window's flat frame: 16:9 (the PC port's hor+ widescreen), twice GC height.
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;

// The host's C++ uses the system's operator new: the game's (JKRHeap) is local to
// the game's prelinked object (pc/cmake/visionos_framework.cmake).
std::mutex g_mutex;
char g_assetPath[1024];
char g_lastError[512];
std::atomic_bool g_started{false};
std::atomic_bool g_running{false};
std::atomic_int g_exitCode{0};
std::atomic_bool g_roomBehindMenus{false};
std::atomic_bool g_paused{false};
std::atomic_bool g_audioInterrupted{false};
std::atomic_bool g_sessionFocusedOnce{false};
std::atomic_bool g_worldRecentered{false};
std::atomic_bool g_windowMode{false};
std::atomic_bool g_quitRequested{false};
std::atomic_bool g_vrHeld{false};
void* g_pool = nullptr;
uint64_t g_frames = 0;

// The game's sound plays unless the window holds it or the session is interrupted.
// From the main thread (interruptions, the window) and the game thread (the Crown hold): read and
// apply under one lock, so a stale answer can't land last and play sound while the game holds.
std::mutex g_audioPauseMutex;
void ApplyAudioPause() {
    std::lock_guard<std::mutex> lock(g_audioPauseMutex);
    if (SDL_WasInit(SDL_INIT_AUDIO))
        pc_audio_set_paused(g_paused.load() || g_audioInterrupted.load() || g_vrHeld.load() ? 1 : 0);
}

void SetError(const char* message) {
    NSLog(@"[ACVR] %s", message);
    std::lock_guard<std::mutex> lock(g_mutex);
    snprintf(g_lastError, sizeof(g_lastError), "%s", message);
}

struct EncodeContext {
    window_frames::Slot* slot;
};

void EncodeWindowCopy(void* command_buffer, void* color_texture, void* ctx) {
    auto* e = static_cast<EncodeContext*>(ctx);
    if (!e->slot) return;
    id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)command_buffer;
    id<MTLTexture> color = (__bridge id<MTLTexture>)color_texture;
    if (window_frames::metal::copy_colour(commands, color, e->slot, window_frames::Image::Final)) {
        window_frames::metal::publish(commands, e->slot);
    } else {
        window_frames::drop(e->slot);
    }
    e->slot = nullptr;
}

// End of every game frame (pc_platform_swap_buffers), on the game thread. In the
// immersive spaces the eyes went to the compositor already (pc_vr_compose_and_submit):
// this only ends the shim's frame.
void PresentHook(void) {
    EncodeContext e{nullptr};
    if (g_windowMode.load()) {
        // The frame's slot (the scene snapshot may have claimed it already); the mirror's frame
        // ended in the game (pc_mirror_end_frame, before this hook).
        e.slot = window_frames::claim();
        pcm_present_ex(&EncodeWindowCopy, &e, nullptr, nullptr);
        if (e.slot) window_frames::drop(e.slot);  // not consumed (no encode)
    } else {
        pcm_present_ex(nullptr, nullptr, nullptr, nullptr);
    }

    g_frames++;
    if (g_frames == 1 || g_frames % 600 == 0) {
        NSLog(@"[ACVR] frame %llu (%dx%d, window=%d)", g_frames, g_pc_window_w, g_pc_window_h,
              g_windowMode.load() ? 1 : 0);
    }
    // Drain Metal's autoreleased objects every frame: a plain pthread has no
    // run loop to do it, and they pile up at ~4 MB/s otherwise.
    if (g_pool) objc_autoreleasePoolPop(g_pool);
    g_pool = objc_autoreleasePoolPush();

    // The window is hidden or in the background: hold the game (clock, sound, GPU work)
    // here, between frames, until it's back or the game is asked to quit.
    if (g_paused.load()) {
        ApplyAudioPause();
        NSLog(@"[ACVR] held");
        while (g_paused.load() && g_pc_running && !g_quitRequested.load()) usleep(50000);
        ApplyAudioPause();
        NSLog(@"[ACVR] resumed");
    }

    // The window paces the game: the next frame starts on the window's tick.
    if (g_windowMode.load()) {
        const uint32_t n = window_frames::begin_frame();
        scene_mirror::begin_frame(n);
    }
}

void* GameThreadMain(void*) {
    g_pool = objc_autoreleasePoolPush();
    pthread_setname_np("Animal Crossing VR game");
    char asset[1024];
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        snprintf(asset, sizeof(asset), "%s", g_assetPath);
    }
    // Documents: the game reads rom/, save/ and its ini files relative to here.
    NSString* documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    if (documents) chdir(documents.fileSystemRepresentation);
    if (asset[0]) setenv("ACVR_DISC", asset, 1);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    g_pc_metal_device = (__bridge void*)device;  // the process keeps the system device alive
    g_pc_metal_present_hook = &PresentHook;
    g_pc_window_w = kWindowWidth;
    g_pc_window_h = kWindowHeight;

    // argv: the app name, VR on or off, plus ACVR_ARGS (test hooks, e.g. --verbose).
    static char arg0[] = "ACVRVision";
    static char vr_on[] = "--vr";
    static char vr_off[] = "--no-vr";
    char* argv[32] = {arg0, g_windowMode.load() ? vr_off : vr_on};
    int argc = 2;
    static char extra[1024];
    if (const char* e = getenv("ACVR_ARGS")) {
        snprintf(extra, sizeof(extra), "%s", e);
        char* save = nullptr;
        for (char* tok = strtok_r(extra, " ", &save); tok && argc < 31; tok = strtok_r(nullptr, " ", &save)) {
            argv[argc++] = tok;
        }
    }
    argv[argc] = nullptr;

    if (g_windowMode.load()) {
        const uint32_t n = window_frames::begin_frame();
        scene_mirror::begin_frame(n);
    }
    NSLog(@"[ACVR] game thread: disc %s, window %d", asset[0] ? asset : "(search Documents)",
          g_windowMode.load() ? 1 : 0);
    // Test runs: ACVR_TEST_SENSE_NAMES=1 logs which controller names count as a Sense half.
    if (getenv("ACVR_TEST_SENSE_NAMES")) {
        for (const char* name : {"DualSense Wireless Controller", "DualSense Edge Wireless Controller",
                                 "PlayStation VR2 Sense Controller (L)", "PlayStation VR2 Sense Controller (R)",
                                 "PS VR2 Sense Controller (L)", "Xbox Wireless Controller", "DUALSHOCK 4 Wireless Controller"}) {
            NSLog(@"[ACVR] sense name test: \"%s\" -> %s", name, sense_pad::is_sense_half(name) ? "Sense half" : "gamepad");
        }
    }
    const int code = ac_pc_main(argc, argv);
    if (g_pool) objc_autoreleasePoolPop(g_pool);
    g_pool = nullptr;
    g_exitCode.store(code);
    g_running.store(false);
    NSLog(@"[ACVR] game returned %d", code);
    return nullptr;
}

}  // namespace

extern "C" {

void acvr_visionos_set_asset_path(const char* path) {
    std::lock_guard<std::mutex> lock(g_mutex);
    snprintf(g_assetPath, sizeof(g_assetPath), "%s", path ? path : "");
}

// The provider takes the layer renderer (retained). A new one after the Digital Crown
// closed the space is the Resume: the waiting session begins again on it.
// Whether the space's layer is layered (the progressive space): pc_vr.cpp renders it a pass per
// eye, since the provider's layered path shows nothing for one side-by-side image (Simulator).
std::atomic<bool> g_layerLayered{false};

void acvr_visionos_set_layer_renderer(void* layer_renderer) {
    if (layer_renderer) {
        cp_layer_renderer_t renderer = (__bridge cp_layer_renderer_t)layer_renderer;
        g_layerLayered = cp_layer_renderer_configuration_get_layout(cp_layer_renderer_get_configuration(renderer)) ==
                         cp_layer_renderer_layout_layered;
    }
    xr_visionos_set_layer_renderer(layer_renderer);
}

extern "C" int acvr_visionos_layer_layered(void) { return g_layerLayered.load() ? 1 : 0; }

void acvr_visionos_spatial_event(uint64_t event_id, int phase, int chirality, bool has_ray, float origin_x,
                                 float origin_y, float origin_z, float direction_x, float direction_y,
                                 float direction_z, bool has_pose, float pose_x, float pose_y, float pose_z) {
    xr_visionos_spatial_event(event_id, phase, chirality, has_ray, origin_x, origin_y, origin_z, direction_x,
                              direction_y, direction_z, has_pose, pose_x, pose_y, pose_z);
}

bool acvr_visionos_layer_invalidated(void) { return xr_visionos_layer_invalidated(); }

bool acvr_visionos_can_resume(void) { return g_sessionFocusedOnce.load(); }

void acvr_visionos_world_recentered(void) { g_worldRecentered.store(true); }

void acvr_visionos_set_audio_interrupted(bool interrupted) {
    g_audioInterrupted.store(interrupted);
    ApplyAudioPause();
}

bool acvr_visionos_start_game(void) {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) {
        SetError("The game already ran in this process; relaunch the app to play again.");
        return false;
    }
    SDL_SetMainReady();
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kGameThreadStackSize);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    g_running.store(true);
    const int result = pthread_create(&thread, &attributes, &GameThreadMain, nullptr);
    pthread_attr_destroy(&attributes);
    if (result != 0) {
        g_running.store(false);
        char message[128];
        snprintf(message, sizeof(message), "Could not start the game thread (error %d).", result);
        SetError(message);
        return false;
    }
    return true;
}

bool acvr_visionos_game_running(void) { return g_running.load(); }

int acvr_visionos_exit_code(void) { return g_exitCode.load(); }

void acvr_visionos_request_quit(void) {
    g_quitRequested.store(true);  // also ends a hold (PresentHook), so the game can see the quit
    // What a closed desktop window sends; pc_platform_poll_events ends the loop.
    if (SDL_WasInit(SDL_INIT_EVENTS) != 0) {
        SDL_Event event{};
        event.type = SDL_QUIT;
        SDL_PushEvent(&event);
    } else {
        g_pc_running = 0;
    }
}

void acvr_visionos_set_room_behind_menus(bool enabled) { g_roomBehindMenus.store(enabled); }

void acvr_visionos_set_safety_boundary(bool enabled) { xr_visionos_set_safety_boundary(enabled); }

const char* acvr_visionos_last_error(void) {
    static char copy[512];
    std::lock_guard<std::mutex> lock(g_mutex);
    snprintf(copy, sizeof(copy), "%s", g_lastError[0] ? g_lastError : xr_visionos_last_error());
    return copy;
}

void acvr_visionos_set_paused(bool paused) { g_paused.store(paused); }

void acvr_visionos_set_window_mode(bool enabled) {
    g_windowMode.store(enabled);
    window_frames::set_enabled(enabled);
}

void acvr_visionos_window_tick(void) { window_frames::tick(); }

bool acvr_visionos_window_acquire(acvr_visionos_window_frame* frame) { return window_frames::acquire(frame); }

void acvr_visionos_window_release(uint64_t serial) { window_frames::release(serial); }

void acvr_visionos_set_mirror_enabled(bool enabled) { scene_mirror::set_enabled(enabled); }

bool acvr_visionos_mirror_acquire(acvr_visionos_mirror_frame* frame, uint32_t up_to) {
    return scene_mirror::acquire(frame, up_to);
}

}  // extern "C"

// Game -> bridge (not exported).
extern "C" bool acvr_visionos_room_behind_menus_enabled(void) { return g_roomBehindMenus.load(); }
extern "C" void acvr_visionos_mark_session_focused(void) { g_sessionFocusedOnce.store(true); }
extern "C" bool acvr_visionos_consume_world_recenter(void) { return g_worldRecentered.exchange(false); }
extern "C" bool acvr_visionos_paused(void) { return g_paused.load(); }
extern "C" bool acvr_visionos_quit_requested(void) { return g_quitRequested.load(); }

// pc_pad.c: the Window view's Sense halves as one pad (sense_pad), when it's the window and either
// half is connected. The immersive spaces read them through OpenXR instead.
extern "C" int acvr_visionos_sense_pad(unsigned* buttons, float* lx, float* ly, float* rx, float* ry, float* lt,
                                       float* rt) {
    if (!g_windowMode.load()) return 0;
    const sense_pad::State s = sense_pad::read();
    if (!s.connected) return 0;
    *buttons = s.buttons;
    *lx = s.left_x;
    *ly = s.left_y;
    *rx = s.right_x;
    *ry = s.right_y;
    *lt = s.left_trigger;
    *rt = s.right_trigger;
    return 1;
}

// pc_pad.c: a controller's name is a PS VR2 Sense half (sense_pad::is_sense_half, the one rule).
extern "C" int acvr_visionos_is_sense_half(const char* name) { return sense_pad::is_sense_half(name) ? 1 : 0; }

// pc_mirror.cpp: the 3D world is drawn and the HUD begins. The window gets the picture so far
// (Base and Scene: Animal Crossing has no screen effects between them) and its depth as
// distances, in the frame's own command buffer; Final follows in PresentHook.
extern "C" void acvr_visionos_window_scene_end(float tan_half_x, float tan_half_y, float focus, float near_plane,
                                               float far_plane) {
    if (!g_windowMode.load()) return;
    window_frames::Slot* slot = window_frames::claim();
    if (!slot) return;
    struct Context {
        window_frames::Slot* slot;
        float near_plane, far_plane;
    } context{slot, near_plane, far_plane};
    pcm_encode_with_default_framebuffer(
        [](void* command_buffer, void* color, void* depth, void* ctx) {
            auto* c = static_cast<Context*>(ctx);
            id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)command_buffer;
            id<MTLTexture> colour = (__bridge id<MTLTexture>)color;
            using window_frames::Image;
            if (!window_frames::metal::copy_colour(commands, colour, c->slot, Image::Base))
                window_frames::forget(c->slot, Image::Base);
            if (!window_frames::metal::copy_colour(commands, colour, c->slot, Image::Scene))
                window_frames::forget(c->slot, Image::Scene);
            if (depth)
                window_frames::metal::copy_distance(commands, (__bridge id<MTLTexture>)depth, c->slot, c->near_plane,
                                                    c->far_plane, window_frames::metal::Depth::GX);
        },
        &context);
    window_frames::set_camera(slot, tan_half_x, tan_half_y, focus);
}
// pc_vr.cpp: the game holds (the Crown closed the space, or the session isn't focused).
extern "C" void acvr_visionos_set_vr_held(int held) {
    g_vrHeld.store(held != 0);
    ApplyAudioPause();
}
// The launcher's comfort vignette (UserDefaults "comfortVignette", on unless set off).
extern "C" int acvr_visionos_comfort_vignette(void) {
    NSNumber* value = [[NSUserDefaults standardUserDefaults] objectForKey:@"comfortVignette"];
    return value ? value.boolValue : 1;
}
// The launcher's turning: 0 smooth, else the snap angle in degrees (UserDefaults "snapTurnDegrees").
// ACVR_SNAP_TURN overrides it for test runs.
// -1 when the launcher never set it: the game's own setting (its pause menu's Turning) stands.
extern "C" int acvr_visionos_snap_turn_degrees(void) {
    if (const char* e = getenv("ACVR_SNAP_TURN")) return atoi(e);
    NSNumber* value = [[NSUserDefaults standardUserDefaults] objectForKey:@"snapTurnDegrees"];
    return value ? value.intValue : -1;
}
// The game's turning as it stands (at Play, and whenever its pause menu's Turning changes it): kept in
// "snapTurnDegrees" so the launcher shows it and the next Play doesn't undo it. One source of truth.
extern "C" void acvr_visionos_note_snap_turn(int degrees) {
    if (getenv("ACVR_SNAP_TURN")) return;  // a test run's override isn't the player's choice
    if (degrees != 0 && degrees != 30 && degrees != 45 && degrees != 90) return;
    NSUserDefaults* defaults = [NSUserDefaults standardUserDefaults];
    NSNumber* value = [defaults objectForKey:@"snapTurnDegrees"];
    if (!value || value.intValue != degrees) [defaults setInteger:degrees forKey:@"snapTurnDegrees"];
}
