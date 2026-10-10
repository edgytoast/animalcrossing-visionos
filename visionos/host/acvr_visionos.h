#pragma once

// The bridge between the Apple Vision Pro app (SwiftUI, visionos/App) and the
// game. main() belongs to SwiftUI, so the game runs on a thread of its own,
// started once the immersive space's CompositorLayer exists. When the game is a
// framework these are the only symbols it exports (an exported_symbols_list),
// which also keeps the game's replacement operator new/delete out of the rest of
// the process. Every function is safe to call from the main thread.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The user's game file (disc image, ROM, data folder) in Documents, or NULL.
// Call before acvr_visionos_start_game.
void acvr_visionos_set_asset_path(const char* path);

// The cp_layer_renderer_t of the immersive space's CompositorLayer (Swift's
// LayerRenderer, passed unretained); the bridge retains it. NULL detaches it.
void acvr_visionos_set_layer_renderer(void* layer_renderer);

// A spatial event of the immersive space (CompositorLayer onSpatialEvent):
// visionOS's look-and-pinch. `phase`: 0 active, 1 ended, 2 cancelled.
// `chirality`: 0 unknown, 1 left, 2 right. Ray and pose in the space's
// coordinates.
void acvr_visionos_spatial_event(uint64_t event_id, int phase, int chirality, bool has_ray, float origin_x,
                              float origin_y, float origin_z, float direction_x, float direction_y,
                              float direction_z, bool has_pose, float pose_x, float pose_y, float pose_z);

// True once the layer renderer was invalidated (the immersive space closed).
bool acvr_visionos_layer_invalidated(void);

// The Digital Crown pause. True once the game was seen in the space (its XR session reached
// FOCUSED): a space closed after that holds the game where it is, its clock and sound paused,
// until a new layer renderer arrives (acvr_visionos_set_layer_renderer) or a quit. Never end the
// session on invalidation alone. False: closed before the game was seen; nothing to resume.
bool acvr_visionos_can_resume(void);

// The progressive space's onWorldRecenter: a Digital Crown recenter moved the space's forward
// (and the portal) to where the wearer faces. The game turns its world to keep the view.
void acvr_visionos_world_recentered(void);

// An audio interruption (Siri, a call, an alarm) began or ended: pause or resume the game's
// audio device (OpenAL Soft: alcDevicePauseSOFT / alcDeviceResumeSOFT).
void acvr_visionos_set_audio_interrupted(bool interrupted);

// Starts the game on its thread. False when it already ran in this process or
// the thread could not be created (see acvr_visionos_last_error).
bool acvr_visionos_start_game(void);
bool acvr_visionos_game_running(void);

// The game's exit code once it returned; 0 before.
int acvr_visionos_exit_code(void);

// Asks the game to quit the way closing its desktop window would (it saves
// first). Returns once the request is posted.
void acvr_visionos_request_quit(void);

// The last error the bridge or the provider reported, or "".
const char* acvr_visionos_last_error(void);

// Whether the room shows wherever the game's frames are transparent (the space
// opened mixed). The game then makes menus, loading and black screens
// see-through. Decided before the space opens; call before starting the game.
void acvr_visionos_set_room_behind_menus(bool enabled);

// A mixed space has no visionOS movement boundary: with this on, the game fades
// into the room as the user walks away from where they started.
void acvr_visionos_set_safety_boundary(bool enabled);

// Holds the game clock and sound (the headset came off, the window went to the
// background or was hidden, or closed where that pauses), or lets them run. Time
// spent held never reaches gameplay as one long frame: cap the frame delta.
void acvr_visionos_set_paused(bool paused);

// --- Window mode: the game in a window in the shared space, beside other apps
// (GameWindowView.swift; host/window_frames.h). visionOS gives an app no head or
// controller tracking outside a Full Space, so the game plays flat, with its own
// camera and a gamepad.

// Call before acvr_visionos_start_game, with no layer renderer.
void acvr_visionos_set_window_mode(bool enabled);

// Once per window update (RealityKit's SceneEvents.Update): lets the game build its
// next frame. The window paces the game.
void acvr_visionos_window_tick(void);

// One finished frame, as IOSurfaceRefs the game owns. Reads must wait for `event`
// (an MTLSharedEvent, or NULL when the game wrote them on the CPU) to reach
// `value`; the frame goes back with acvr_visionos_window_release once they finish.
// A `generation` that changed means the surfaces are new ones.
typedef struct acvr_visionos_window_frame {
    uint64_t serial;
    uint64_t generation;
    void* scene;     // BGRA8: the 3D scene before the HUD (screen effects included), or NULL
    void* distance;  // RGBA16F: depth along the view axis in .r (game units); with scene
    void* final;     // BGRA8: the finished frame, HUD and all
    void* ui;        // BGRA8, premultiplied: an overlay UI drawn outside the game's frame, or NULL
    void* base;      // BGRA8: the 3D scene before the screen effects (with the mirror), or NULL
    void* event;     // MTLSharedEvent, borrowed, or NULL
    uint64_t value;
    uint32_t game_frame;           // the game's frame number (24 bits), as the mirror's
    float tan_half_x, tan_half_y;  // the scene camera's half field of view, as tangents
    float focus;                   // the camera's distance to what it looks at (game units)
} acvr_visionos_window_frame;

// The newest frame not yet taken, or false.
bool acvr_visionos_window_acquire(acvr_visionos_window_frame* frame);
void acvr_visionos_window_release(uint64_t serial);

// --- The window's scene mirror (host/scene_mirror.h, MirrorScene.swift): the 3D
// scene's draws as triangles in the camera's view space (right-handed, -z ahead),
// for RealityKit to render from the viewer's real eyes. Each vertex carries the
// material as an affine function of its texture's sample T, in the game's gamma
// space: colour = T.rgb * mul.rgb + add.rgb, alpha = T.a * mul.a + add.a (mul and
// add half floats).
typedef struct acvr_visionos_mirror_vertex {
    float position[3];
    float uv[2];
    uint16_t mul[4];
    uint16_t add[4];
} acvr_visionos_mirror_vertex;

#define ACVR_VISIONOS_MIRROR_PART_OPAQUE 0u
#define ACVR_VISIONOS_MIRROR_PART_CUTOUT 1u
#define ACVR_VISIONOS_MIRROR_PART_BLEND 2u
#define ACVR_VISIONOS_MIRROR_CULL_BACK 1u
#define ACVR_VISIONOS_MIRROR_CULL_FRONT 2u
#define ACVR_VISIONOS_MIRROR_DEPTH_WRITE 4u
#define ACVR_VISIONOS_MIRROR_DEPTH_TEST 8u
#define ACVR_VISIONOS_MIRROR_BACKGROUND 16u
#define ACVR_VISIONOS_MIRROR_WRAP_CLAMP 0u
#define ACVR_VISIONOS_MIRROR_WRAP_REPEAT 1u
#define ACVR_VISIONOS_MIRROR_WRAP_MIRROR 2u

// A run of triangles with one material. Blends are premultiplied
// (colour + dst * (1 - opacity)): colour = rgb * (colour_base + colour_alpha * a),
// opacity = opacity_base + opacity_alpha * a + opacity_luma * luma(rgb).
typedef struct acvr_visionos_mirror_part {
    uint32_t first_index, index_count;
    uint64_t texture;  // 0: none (white)
    uint32_t kind;     // ACVR_VISIONOS_MIRROR_PART_*
    uint32_t flags;
    float cutoff;
    float colour_base, colour_alpha, opacity_base, opacity_alpha, opacity_luma;
    uint8_t wrap_s, wrap_t;
    uint8_t pad[6];
} acvr_visionos_mirror_part;

// A texture, sent once (again for new pixels): RGBA8, the top row first.
typedef struct acvr_visionos_mirror_texture {
    uint64_t id;
    const uint8_t* rgba;
    uint32_t width, height;
    bool mipmapped, cutout;
} acvr_visionos_mirror_texture;

typedef struct acvr_visionos_mirror_frame {
    uint64_t serial;
    uint32_t game_frame;  // the game's frame number (24 bits), as the window frame's
    bool scene;           // false: no 3D scene this frame
    const acvr_visionos_mirror_vertex* vertices;
    uint32_t vertex_count;
    const uint32_t* indices;
    uint32_t index_count;
    const acvr_visionos_mirror_part* parts;
    uint32_t part_count;
    float bounds_min[3], bounds_max[3];
    float tan_half_x, tan_half_y, focus;
    // Where the window's glass goes (game units); what's nearer comes out in front of it,
    // within the front allowance.
    float plane;
    const acvr_visionos_mirror_texture* textures;  // new since the last acquire
    uint32_t texture_count;
    const uint64_t* removed;  // textures no longer used
    uint32_t removed_count;
} acvr_visionos_mirror_frame;

// Whether the window wants the mirror. Turning it on sends every texture again.
void acvr_visionos_set_mirror_enabled(bool enabled);
// How far in front of its glass the window lets the scene come out (window widths), and
// the narrowest half-view tangent it lays out as is. Defined in host/scene_mirror.cpp.
void acvr_visionos_set_mirror_front_allowance(float widths, float min_view_tangent);
// The newest frame no later than game frame `up_to` (the window frame shown with
// it, so the two match); its pointers last until the next call (main thread).
// False before the first.
bool acvr_visionos_mirror_acquire(acvr_visionos_mirror_frame* frame, uint32_t up_to);

#ifdef __cplusplus
}
#endif
