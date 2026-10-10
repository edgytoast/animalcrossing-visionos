#if defined(TARGET_PC) && __SIZEOF_POINTER__ == 8
/* pc_main.c - PC entry point: SDL2/GL init, crash protection, boot sequence */
#ifndef _WIN32
#define _GNU_SOURCE  /* needed for dladdr */
#endif
#else
/* pc_main.c - PC entry point: SDL2/GL init and boot sequence */
#endif
#include "pc_platform.h"
#include "pc_gx_internal.h"
#include "pc_texture_pack.h"
#include "pc_settings.h"
#include "pc_keybindings.h"
#include "pc_assets.h"
#include "pc_disc.h"
#include "pc_typing.h"
#include "pc_pause_menu.h"
#include "pc_settings_menu.h"
#include "pc_profiler.h"
#include "pc_test_hooks.h"
#include "pc_vr.h"
#include "pc_fp_camera.h"
#include "pc_model_viewer.h"
#include "m_kankyo.h"

#ifdef TARGET_ANDROID
#include "android_glue.h"
#endif

/* prefer discrete GPU on laptops */
#ifdef _WIN32
__declspec(dllexport) unsigned long NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;

/* Last-chance crash report: the GUI build has no console, so without this a
 * crash is a silent instant close. The module+offset lets a dev map the
 * fault to a function against the same build. */
static LONG WINAPI pc_crash_handler(EXCEPTION_POINTERS* info) {
    FILE* f = fopen("crash.txt", "w");
    if (f && info && info->ExceptionRecord) {
        void* addr = info->ExceptionRecord->ExceptionAddress;
        HMODULE mod = NULL;
        char modname[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod);
        if (mod) GetModuleFileNameA(mod, modname, sizeof(modname));
        fprintf(f, "exception 0x%08lX at %p\nmodule %s\nbase %p offset 0x%lX\n",
                (unsigned long)info->ExceptionRecord->ExceptionCode, addr, modname,
                (void*)mod, (unsigned long)((char*)addr - (char*)mod));
        fclose(f);
    } else if (f) {
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

SDL_Window*   g_pc_window = NULL;
SDL_GLContext  g_pc_gl_context = NULL;
int           g_pc_running = 1;
int           g_pc_frame_limit_override = -1;
int           g_pc_speedhack_enabled = 0;
int           g_pc_verbose = 0;
int           g_pc_time_override = -1; /* -1=system clock, 0-23=override hour */
int           g_pc_min_override = -1; /* -1=system clock, 0-59=override minute */
int           g_pc_sec_override = -1; /* -1=system clock, 0-59=override second */
int           g_pc_date_month = -1; /* -1=system clock, 1-12=override month */
int           g_pc_date_day = -1; /* -1=system clock, 1-31=override day */
int           g_pc_date_year = -1; /* -1=system clock, else override year */
int           g_pc_weather_override = -1;
int           g_pc_weather_intensity_override = mEnv_WEATHER_INTENSITY_HEAVY;
int           g_pc_window_w = PC_SCREEN_WIDTH;
int           g_pc_window_h = PC_SCREEN_HEIGHT;
int           g_pc_widescreen_stretch = 0;
static int    g_pc_vr_override = -1; /* -1=use settings, 0=--no-vr, 2=--vr */

#if defined(TARGET_PC) && __SIZEOF_POINTER__ == 8
/* exe image range — used by seg2k0 to distinguish pointers from segment addresses */
uintptr_t pc_image_base = 0;
uintptr_t pc_image_end  = 0;

#ifdef __APPLE__
/* Mach-O equivalent of the ELF/PE image range below: the extent of every
 * mapped segment of the image holding this code (the executable on macOS,
 * the game framework on visionOS), slid to where dyld put it. __PAGEZERO
 * maps nothing and is skipped. */
static void pc_image_range_macho(void) {
    Dl_info dl;
    if (!dladdr((void*)&pc_image_range_macho, &dl) || !dl.dli_fbase) return;
    const struct mach_header_64* mh = (const struct mach_header_64*)dl.dli_fbase;
    if (mh->magic != MH_MAGIC_64) return;
    const struct load_command* lc = (const struct load_command*)(mh + 1);
    uintptr_t text_vmaddr = 0, lo = UINTPTR_MAX, hi = 0;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64* seg = (const struct segment_command_64*)lc;
            if (seg->vmsize != 0 && strcmp(seg->segname, "__PAGEZERO") != 0) {
                if (strcmp(seg->segname, "__TEXT") == 0) text_vmaddr = (uintptr_t)seg->vmaddr;
                if (seg->vmaddr < lo) lo = (uintptr_t)seg->vmaddr;
                if (seg->vmaddr + seg->vmsize > hi) hi = (uintptr_t)(seg->vmaddr + seg->vmsize);
            }
        }
        lc = (const struct load_command*)((const char*)lc + lc->cmdsize);
    }
    if (hi == 0) return;
    uintptr_t slide = (uintptr_t)dl.dli_fbase - text_vmaddr;
    pc_image_base = lo + slide;
    pc_image_end = hi + slide;
    fprintf(stderr, "[PC] image %p-%p\n", (void*)pc_image_base, (void*)pc_image_end);
}
#endif

static jmp_buf* pc_active_jmpbuf = NULL;
static volatile uintptr_t pc_last_crash_addr = 0;

static volatile uintptr_t pc_last_crash_data_addr = 0;

#ifdef _WIN32
/* longjmp from VEH is technically UB, but works on x86 MinGW (no SEH to corrupt).
 * GCC doesn't have __try/__except and checking every pointer in emu64 is impractical. */
static LONG WINAPI pc_veh_handler(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (pc_active_jmpbuf != NULL &&
        (code == EXCEPTION_ACCESS_VIOLATION ||
         code == EXCEPTION_ILLEGAL_INSTRUCTION ||
         code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
         code == EXCEPTION_PRIV_INSTRUCTION)) {
        pc_last_crash_addr = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
        if (code == EXCEPTION_ACCESS_VIOLATION)
            pc_last_crash_data_addr = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        else
            pc_last_crash_data_addr = 0;
        jmp_buf* buf = pc_active_jmpbuf;
        pc_active_jmpbuf = NULL;
        longjmp(*buf, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
/* POSIX equivalent of VEH — longjmp from signal handler (POSIX-defined for program faults) */
static void pc_signal_handler(int sig, siginfo_t* info, void* ucontext) {
    (void)ucontext;
    if (pc_active_jmpbuf != NULL) {
        pc_last_crash_addr = (uintptr_t)info->si_addr;
        pc_last_crash_data_addr = (sig == SIGSEGV) ?
            (uintptr_t)info->si_addr : 0;
        jmp_buf* buf = pc_active_jmpbuf;
        pc_active_jmpbuf = NULL;
        longjmp(*buf, 1);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

uintptr_t pc_crash_get_data_addr(void) {
    return pc_last_crash_data_addr;
}

void pc_crash_protection_init(void) {
    static int installed = 0;
    if (!installed) {
#ifdef _WIN32
        AddVectoredExceptionHandler(1, pc_veh_handler);
#else
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = pc_signal_handler;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
        sigaction(SIGFPE, &sa, NULL);
#endif
        installed = 1;
    }
}

void pc_crash_set_jmpbuf(jmp_buf* buf) {
    pc_active_jmpbuf = buf;
}

uintptr_t pc_crash_get_addr(void) {
    return pc_last_crash_addr;
}
#else
/* exe image range -- used by seg2k0 to distinguish pointers from segment addresses */
unsigned int pc_image_base = 0;
unsigned int pc_image_end  = 0;
#endif

#if defined(__APPLE__) && !defined(PC_METAL) && !defined(TARGET_ANDROID)
/* Headless GL (ACVR_HEADLESS=1): a CGL context with no window and an FBO as
 * the frame. For test runs where there's no window server (CI, sandboxed
 * shells); the test hooks read frames back from the FBO. */
#include <OpenGL/OpenGL.h>
static int s_pc_headless = 0;
static GLADapiproc pc_cgl_get_proc(const char* name) {
    static void* lib = NULL;
    if (!lib) lib = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_LAZY);
    return lib ? (GLADapiproc)dlsym(lib, name) : NULL;
}
static int pc_headless_gl_init(void) {
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAAccelerated, kCGLPFAAllowOfflineRenderers, (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj pf = NULL;
    GLint npix = 0;
    CGLContextObj ctx = NULL;
    if (CGLChoosePixelFormat(attrs, &pf, &npix) != kCGLNoError || !pf) return 0;
    if (CGLCreateContext(pf, NULL, &ctx) != kCGLNoError || !ctx) return 0;
    CGLSetCurrentContext(ctx);
    if (!gladLoadGL(pc_cgl_get_proc)) return 0;
    g_pc_window_w = g_pc_settings.window_width > 0 ? g_pc_settings.window_width : PC_SCREEN_WIDTH;
    g_pc_window_h = g_pc_settings.window_height > 0 ? g_pc_settings.window_height : PC_SCREEN_HEIGHT;
    GLuint fbo, color, depth;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &color);
    glBindRenderbuffer(GL_RENDERBUFFER, color);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, g_pc_window_w, g_pc_window_h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, g_pc_window_w, g_pc_window_h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 0;
    fprintf(stderr, "[ACVR] headless GL %s, %dx%d\n", (const char*)glGetString(GL_RENDERER),
            g_pc_window_w, g_pc_window_h);
    return 1;
}
#endif

#ifdef PC_METAL
/* Metal renderer (pc_gl_metal.mm). With no host-provided target (macOS
 * bench) the game renders into its own offscreen texture; the visionOS host
 * hands its own targets over through pcm_set_default_framebuffer. */
void* g_pc_metal_device = NULL;          /* set by the visionOS host before init */
void (*g_pc_metal_present_hook)(void) = NULL; /* visionOS host: publish + next target */
static u8* pc_metal_capture_frame(int* w, int* h, int* bottom_up) {
    *bottom_up = 0;
    return pcm_read_default_framebuffer(w, h);
}
#endif

void pc_platform_init(void) {
#ifdef PC_METAL
    {
        /* No SDL video and no game controllers on the bench: input comes from
         * the host (visionOS) or ACVR_TEST_ACTIONS. */
        Uint32 sdl_flags = SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS;
        if (getenv("ACVR_SDL_PADS")) sdl_flags |= SDL_INIT_GAMECONTROLLER;
#ifdef PC_VISIONOS
        sdl_flags |= SDL_INIT_GAMECONTROLLER; /* gamepads (and, from M3, the Sense pair) */
#endif
        if (SDL_Init(sdl_flags) < 0) {
            fprintf(stderr, "[ACVR] SDL_Init failed: %s\n", SDL_GetError());
            exit(1);
        }
        if (!pcm_init(g_pc_metal_device)) {
            fprintf(stderr, "[ACVR] Metal init failed\n");
            exit(1);
        }
        if (g_pc_window_w <= 0 || g_pc_window_h <= 0 || !g_pc_metal_present_hook) {
            g_pc_window_w = g_pc_settings.window_width > 0 ? g_pc_settings.window_width : PC_SCREEN_WIDTH;
            g_pc_window_h = g_pc_settings.window_height > 0 ? g_pc_settings.window_height : PC_SCREEN_HEIGHT;
        }
        /* The frame's target. The visionOS host copies it into the window's
         * slot (or, from M2, renders the eyes into the provider's images). */
        pcm_set_default_framebuffer(pcm_make_target(g_pc_window_w, g_pc_window_h), g_pc_window_w, g_pc_window_h);
        g_pc_test_capture = pc_metal_capture_frame;
        fprintf(stderr, "[ACVR] Metal renderer, %dx%d\n", g_pc_window_w, g_pc_window_h);
        pc_gx_init();
        pc_texture_pack_init();
        return;
    }
#endif
#if defined(__APPLE__) && !defined(PC_METAL) && !defined(TARGET_ANDROID)
    if (getenv("ACVR_HEADLESS")) {
        s_pc_headless = 1;
        /* No game controllers here: GameController.framework needs services a
         * sandboxed test shell doesn't have. Input comes from ACVR_TEST_ACTIONS. */
        if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_EVENTS) < 0 ||
            !pc_headless_gl_init()) {
            fprintf(stderr, "[ACVR] headless GL init failed\n");
            exit(1);
        }
        pc_gx_init();
        pc_texture_pack_init();
        return;
    }
#endif
#ifdef _WIN32
    SetProcessDPIAware();
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
#endif
#ifdef TARGET_ANDROID
    /* The system back button/gesture opens the pause menu instead of closing the activity. */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif
    /* Keep pad input alive when the window is unfocused — in VR the flat
     * mirror window is rarely the foreground window. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(1);
    }

#ifdef TARGET_ANDROID
    /* Android: OpenGL ES 3 context (the renderer subset is ES3 compatible) */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
#ifdef PC_ENHANCEMENTS
    if (g_pc_settings.msaa > 0) {
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
        SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, g_pc_settings.msaa);
    }
#endif

    {
        Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
        int win_w = g_pc_settings.window_width;
        int win_h = g_pc_settings.window_height;
#ifdef TARGET_ANDROID
        /* SDL requests immersive Android system UI and the full drawable. */
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#else
        if (g_pc_model_viewer_shot_path) {
            flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN;
        } else if (g_pc_settings.fullscreen == 1) {
            flags |= SDL_WINDOW_FULLSCREEN;
        } else if (g_pc_settings.fullscreen == 2) {
            flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        }
#endif
        g_pc_window = SDL_CreateWindow(
            PC_WINDOW_TITLE,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            win_w, win_h, flags
        );
    }
    if (!g_pc_window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        exit(1);
    }

    g_pc_gl_context = SDL_GL_CreateContext(g_pc_window);
    if (!g_pc_gl_context) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(g_pc_window);
        SDL_Quit();
        exit(1);
    }

#ifndef TARGET_ANDROID
    if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress)) {
        fprintf(stderr, "gladLoadGL failed\n");
        SDL_GL_DeleteContext(g_pc_gl_context);
        SDL_DestroyWindow(g_pc_window);
        SDL_Quit();
        exit(1);
    }
#endif

#ifdef TARGET_ANDROID
    /* Present on vsync: unthrottled swaps make the compositor queue several
     * frames late (FrameIsLate: queued_frames >= 2). Timer pacing stays 60 Hz. */
    SDL_GL_SetSwapInterval(1);
#else
    SDL_GL_SetSwapInterval(g_pc_settings.vsync);
#endif

    pc_platform_update_window_size();

#ifdef PC_ENHANCEMENTS
    if (g_pc_settings.msaa > 0) {
        glEnable(GL_MULTISAMPLE);
    }
#endif

    pc_gx_init();
    pc_texture_pack_init();
#ifdef PC_ENHANCEMENTS
    if (g_pc_settings.preload_textures) {
        pc_texture_pack_preload_all();
    }
#endif
}

extern void PADCleanup(void);

static void pc_speedhack_toggle(void) {
    g_pc_speedhack_enabled = !g_pc_speedhack_enabled;
    if (g_pc_window != NULL) {
        SDL_SetWindowTitle(g_pc_window, g_pc_speedhack_enabled ? "Animal Crossing [5x]" : PC_WINDOW_TITLE);
    }

    if (g_pc_verbose) {
        printf("[PC] speedhack %s\n", g_pc_speedhack_enabled ? "5x" : "off");
    }
}

void pc_platform_shutdown(void) {
    pc_audio_shutdown();
    pc_audio_mq_shutdown();
    PADCleanup();
    pc_texture_pack_shutdown();
    pc_gx_shutdown();

    if (g_pc_gl_context) {
        SDL_GL_DeleteContext(g_pc_gl_context);
        g_pc_gl_context = NULL;
    }
    if (g_pc_window) {
        SDL_DestroyWindow(g_pc_window);
        g_pc_window = NULL;
    }
    SDL_Quit();
}

void pc_platform_update_window_size(void) {
#ifdef PC_METAL
    return;
#endif
#if defined(__APPLE__) && !defined(PC_METAL) && !defined(TARGET_ANDROID)
    if (s_pc_headless) return;
#endif
    SDL_GL_GetDrawableSize(g_pc_window, &g_pc_window_w, &g_pc_window_h);
    if (g_pc_window_w <= 0) g_pc_window_w = PC_SCREEN_WIDTH;
    if (g_pc_window_h <= 0) g_pc_window_h = PC_SCREEN_HEIGHT;
    /* Keep target dims following the window except mid-VR-pass (the event
     * poll that calls this runs between frames, outside scene passes). */
    if (!pc_vr_in_scene_pass()) {
        g_pc_target_w = g_pc_window_w;
        g_pc_target_h = g_pc_window_h;
    }
}

#ifndef PC_METAL
/* Test hook capture (ACVR_TEST_SHOTS): the back buffer, bottom-up. */
static u8* pc_gl_capture_frame(int* w, int* h, int* bottom_up) {
    int dw = g_pc_window_w, dh = g_pc_window_h;
    if (g_pc_window) SDL_GL_GetDrawableSize(g_pc_window, &dw, &dh);
    if (dw <= 0 || dh <= 0) return NULL;
    u8* rgba = (u8*)malloc((size_t)dw * dh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    *w = dw; *h = dh; *bottom_up = 1;
    return rgba;
}
#endif

void pc_platform_swap_buffers(void) {
    pc_gx_draw_pending();
    pc_model_viewer_before_swap();
#ifdef PC_METAL
    if (g_pc_metal_present_hook) g_pc_metal_present_hook();
    else pcm_present(NULL, NULL);
#else
    if (g_pc_window) SDL_GL_SwapWindow(g_pc_window);
    else glFlush();
#endif
}

int pc_platform_poll_events(void) {
    SDL_Event event;

    pc_typing_update();

    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                g_pc_running = 0;
                return 0;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    pc_platform_update_window_size();
                }
                break;
            case SDL_KEYDOWN:
#ifdef TARGET_ANDROID
                /* Android back acts like Esc: toggles the pause menu / cancels inside it. */
                if (event.key.keysym.sym == SDLK_AC_BACK) {
                    event.key.keysym.sym = SDLK_ESCAPE;
                    event.key.keysym.scancode = SDL_SCANCODE_ESCAPE;
                }
#endif
                /* Keybinding capture eats all input first (works from both
                 * the pause menu and the title Options menu). */
                if (pc_settings_menu_capture_active()) {
                    pc_settings_menu_handle_capture_event(&event);
                    break;
                }
                if (event.key.keysym.sym == SDLK_F3 && !event.key.repeat) {
                    pc_speedhack_toggle();
                    break;
                }
                if (event.key.keysym.sym == SDLK_F5 && !event.key.repeat) {
                    /* Not while paused: the camera is frozen, so the switch
                     * couldn't take effect until unpause anyway */
                    if (!g_pc_paused) {
                        pc_fp_toggle();
                    }
                    break;
                }
                if (event.key.keysym.sym == SDLK_ESCAPE && !event.key.repeat) {
                    if (g_pc_paused) {
                        pc_pause_menu_handle_event(&event);
                    } else {
                        pc_pause_menu_toggle();
                    }
                    break;
                }
                if (g_pc_paused) {
                    pc_pause_menu_handle_event(&event);
                    break; /* swallow all keys while paused */
                }
                pc_typing_handle_event(&event);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (pc_settings_menu_capture_active()) {
                    pc_settings_menu_handle_capture_event(&event);
                }
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                if (pc_settings_menu_capture_active()) {
                    pc_settings_menu_handle_capture_event(&event);
                    break;
                }
                if (g_pc_paused) {
                    pc_pause_menu_handle_event(&event);
                    break;
                }
                /* Back/Select opens the pause menu (controller Esc). */
                if (event.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
                    pc_pause_menu_toggle();
                }
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (pc_settings_menu_capture_active()) {
                    pc_settings_menu_handle_capture_event(&event);
                    break;
                }
                if (g_pc_paused) {
                    pc_pause_menu_handle_event(&event);
                }
                break;
            case SDL_CONTROLLERDEVICEADDED: {
                /* Bluetooth pads connect (and reconnect after sleeping)
                 * mid-session — open them on arrival. */
                extern void pc_pad_device_added(int device_index);
                pc_pad_device_added(event.cdevice.which);
                break;
            }
            case SDL_CONTROLLERDEVICEREMOVED: {
                extern void pc_pad_device_removed(int instance_id);
                pc_pad_device_removed((int)event.cdevice.which);
                break;
            }
            case SDL_TEXTINPUT:
                if (g_pc_paused) break;
                pc_typing_handle_event(&event);
                break;
        }
    }
    return 1;
}

/* game's main() renamed to ac_entry via -Dmain=ac_entry, boot.c's to boot_main */
extern void ac_entry(void);
extern int boot_main(int argc, const char** argv);

static int pc_parse_rain_intensity(const char* text) {
    if (strcmp(text, "light") == 0) {
        return mEnv_WEATHER_INTENSITY_LIGHT;
    }

    if (strcmp(text, "normal") == 0) {
        return mEnv_WEATHER_INTENSITY_NORMAL;
    }

    if (strcmp(text, "heavy") == 0) {
        return mEnv_WEATHER_INTENSITY_HEAVY;
    }

    return -1;
}

int main(int argc, char* argv[]) {
#ifdef TARGET_ANDROID
    android_bootstrap(); /* chdir to app storage, log files */
#endif

#ifdef _WIN32
    SetUnhandledExceptionFilter(pc_crash_handler);
#endif
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: AnimalCrossing [options]\n");
            printf("  --verbose, -v       Enable diagnostic output\n");
            printf("  --no-framelimit     Alias for --framelimit 0 (uncapped)\n");
            printf("  --framelimit N      Set the target frame rate (default 60, 0 = uncapped)\n");
            printf("  --profile [N]       Print frame profiler summary every N frames (default 120)\n");
            printf("  --model-viewer [N]  Launch model viewer (optional start index)\n");
            printf("  --model-viewer-solid  Include the production building fill in the viewer\n");
            printf("  --model-viewer-angle DEG  Set viewer orbit angle (default 270)\n");
            printf("  --model-viewer-distance N  Set viewer distance (1..100000 model units)\n");
            printf("  --model-viewer-height N  Set viewer look-at height (-50000..50000 model units)\n");
            printf("  --model-viewer-shot PATH  Save a fixed hidden-window BMP render and exit (no VR)\n");
            printf("  --time H[:M[:S]]    Override in-game time (e.g. 5, 17:30, 5:55:00)\n");
            printf("  --date M/D[/Y]      Override in-game date (e.g. 7/4, 12/24/2026)\n");
            printf("  --rain [intensity]  Force rainy weather; intensity is light, normal, or heavy\n");
            printf("  --uber-shader       Disable shader specialization (single uber shader)\n");
            printf("  --vr                Force SteamVR mode (error message if unavailable)\n");
            printf("  --no-vr             Disable VR for this session\n");
            printf("  --help, -h          Show this help message\n");
            return 0;
        } else if (strcmp(argv[i], "--framelimit") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                int v = atoi(argv[i + 1]);
                if (v > 0) {
                    g_pc_frame_limit_override = v;
                } else if (v == 0) {
                    g_pc_frame_limit_override = 0;
                }
                i++;
            }
        } else if (strcmp(argv[i], "--no-framelimit") == 0) {
            g_pc_frame_limit_override = 0;
        } else if (strcmp(argv[i], "--uber-shader") == 0) {
            extern int g_pc_uber_shader_only;
            g_pc_uber_shader_only = 1;
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            g_pc_verbose = 1;
        } else if (strcmp(argv[i], "--profile") == 0) {
            g_pc_profile_enabled = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                int interval = atoi(argv[i + 1]);
                if (interval > 0) g_pc_profile_interval = interval;
                i++;
            }
        } else if (strcmp(argv[i], "--model-viewer") == 0) {
            g_pc_model_viewer = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                g_pc_model_viewer_start = atoi(argv[i + 1]);
                i++;
            }
        } else if (strcmp(argv[i], "--model-viewer-solid") == 0) {
            g_pc_model_viewer = 1;
            g_pc_model_viewer_solid = 1;
        } else if (strcmp(argv[i], "--model-viewer-angle") == 0) {
            char* end;
            float angle;
            if (i + 1 >= argc) {
                fprintf(stderr, "--model-viewer-angle requires a finite angle in degrees\n");
                return 2;
            }
            angle = strtof(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || !isfinite(angle)) {
                fprintf(stderr, "Invalid model-viewer angle: %s\n", argv[i]);
                return 2;
            }
            g_pc_model_viewer = 1;
            g_pc_model_viewer_angle = fmodf(angle, 360.0f);
        } else if (strcmp(argv[i], "--model-viewer-distance") == 0 ||
                   strcmp(argv[i], "--model-viewer-height") == 0) {
            const char* option = argv[i];
            int distance = strcmp(option, "--model-viewer-distance") == 0;
            char* end;
            float value;
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a finite value in model units\n", option);
                return 2;
            }
            value = strtof(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || !isfinite(value) ||
                value < (distance ? 1.0f : -50000.0f) || value > (distance ? 100000.0f : 50000.0f)) {
                fprintf(stderr, "Invalid %s: %s\n", option, argv[i]);
                return 2;
            }
            g_pc_model_viewer = 1;
            if (distance) g_pc_model_viewer_distance = value;
            else g_pc_model_viewer_height = value;
        } else if (strcmp(argv[i], "--model-viewer-shot") == 0) {
            if (i + 1 >= argc || argv[i + 1][0] == '\0' || argv[i + 1][0] == '-') {
                fprintf(stderr, "--model-viewer-shot requires an output BMP path\n");
                return 2;
            }
            g_pc_model_viewer = 1;
            g_pc_model_viewer_shot_path = argv[++i];
        } else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
            int h = -1, m = -1, s = -1;
            sscanf(argv[i + 1], "%d:%d:%d", &h, &m, &s);
            if (h >= 0 && h <= 23) g_pc_time_override = h;
            if (m >= 0 && m <= 59) g_pc_min_override = m;
            if (s >= 0 && s <= 59) g_pc_sec_override = s;
            i++;
        } else if (strcmp(argv[i], "--date") == 0 && i + 1 < argc) {
            int mo = -1, d = -1, y = -1;
            sscanf(argv[i + 1], "%d/%d/%d", &mo, &d, &y);
            if (mo >= 1 && mo <= 12 && d >= 1 && d <= 31) {
                g_pc_date_month = mo;
                g_pc_date_day = d;
                if (y >= 2000) g_pc_date_year = y;
            }
            i++;
        } else if (strcmp(argv[i], "--rain") == 0) {
            g_pc_weather_override = mEnv_WEATHER_RAIN;
            g_pc_weather_intensity_override = mEnv_WEATHER_INTENSITY_HEAVY;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                int intensity = pc_parse_rain_intensity(argv[i + 1]);
                if (intensity >= 0) {
                    g_pc_weather_intensity_override = intensity;
                    i++;
                }
            }
        } else if (strcmp(argv[i], "--vr") == 0) {
            g_pc_vr_override = 2;
        } else if (strcmp(argv[i], "--no-vr") == 0) {
            g_pc_vr_override = 0;
        }
    }

    /* Redirect stdout/stderr to NUL unless verbose — unbuffered terminal writes
     * are extremely slow on Windows and tank FPS. */
#if !defined(TARGET_ANDROID) && !defined(PC_VISIONOS) /* visionOS: stderr reaches the Simulator console */
    if (!g_pc_verbose && !g_pc_profile_enabled && !g_pc_model_viewer_shot_path) {
#ifdef _WIN32
        freopen("NUL", "w", stdout);
        freopen("NUL", "w", stderr);
#else
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
#endif
    } else {
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
#endif

    /* exe image range for seg2k0 — BSS can overlap N64 segment addresses */
#ifdef _WIN32
    {
        HMODULE exe = GetModuleHandle(NULL);
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)exe;
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((char*)exe + dos->e_lfanew);
        pc_image_base = (uintptr_t)exe;
        pc_image_end = pc_image_base + nt->OptionalHeader.SizeOfImage;
#if defined(TARGET_PC) && __SIZEOF_POINTER__ == 8
    }
#elif defined(__APPLE__)
    {
        /* macOS: use dladdr — no ELF headers available */
        Dl_info dl;
        if (dladdr((void*)main, &dl) && dl.dli_fbase) {
            pc_image_base = (uintptr_t)dl.dli_fbase;
            /* Estimate image end — on 64-bit, seg2k0 uses threshold check
             * instead of image range, so this is defense-in-depth only. */
            pc_image_end = pc_image_base + 0x10000000;
        }
#else
#endif
    }
#elif defined(__APPLE__)
    pc_image_range_macho();
#else
    {
        Dl_info dl;
        if (dladdr((void*)main, &dl) && dl.dli_fbase) {
#if defined(TARGET_PC) && __SIZEOF_POINTER__ == 8
            pc_image_base = (uintptr_t)dl.dli_fbase;
#if __SIZEOF_POINTER__ == 8
            /* 64-bit ELF */
            Elf64_Ehdr* ehdr = (Elf64_Ehdr*)dl.dli_fbase;
            Elf64_Phdr* phdr = (Elf64_Phdr*)((char*)dl.dli_fbase + ehdr->e_phoff);
            uintptr_t max_end = 0;
            for (int i = 0; i < ehdr->e_phnum; i++) {
                if (phdr[i].p_type == PT_LOAD) {
                    uintptr_t seg_end = phdr[i].p_vaddr + phdr[i].p_memsz;
                    if (seg_end > max_end) max_end = seg_end;
                }
            }
#else
            /* 32-bit ELF */
            Elf32_Ehdr* ehdr = (Elf32_Ehdr*)dl.dli_fbase;
            Elf32_Phdr* phdr = (Elf32_Phdr*)((char*)dl.dli_fbase + ehdr->e_phoff);
            uintptr_t max_end = 0;
            for (int i = 0; i < ehdr->e_phnum; i++) {
                if (phdr[i].p_type == PT_LOAD) {
                    uintptr_t seg_end = phdr[i].p_vaddr + phdr[i].p_memsz;
                    if (seg_end > max_end) max_end = seg_end;
                }
            }
#endif
#else
            pc_image_base = (unsigned int)(uintptr_t)dl.dli_fbase;
            Elf32_Ehdr* ehdr = (Elf32_Ehdr*)dl.dli_fbase;
            Elf32_Phdr* phdr = (Elf32_Phdr*)((char*)dl.dli_fbase + ehdr->e_phoff);
            unsigned int max_end = 0;
            for (int i = 0; i < ehdr->e_phnum; i++) {
                if (phdr[i].p_type == PT_LOAD) {
                    unsigned int seg_end = phdr[i].p_vaddr + phdr[i].p_memsz;
                    if (seg_end > max_end) max_end = seg_end;
                }
            }
#endif
            /* ET_EXEC: p_vaddr is absolute. ET_DYN (PIE): relative to load address. */
            if (ehdr->e_type == ET_DYN) {
                pc_image_end = pc_image_base + max_end;
            } else {
                pc_image_end = max_end;
            }
        }
    }
#endif

    SDL_SetMainReady();
#ifndef PC_METAL
    g_pc_test_capture = pc_gl_capture_frame;
#endif
    pc_settings_load();
    if (g_pc_model_viewer_shot_path) g_pc_vr_override = 0;
    if (g_pc_vr_override >= 0) {
        g_pc_settings.vr_mode = g_pc_vr_override;
    }
    pc_keybindings_load();
    pc_fp_init();
    pc_platform_init();
    pc_vr_init();   /* needs the GL context; no-op when vr_mode=0 or no HMD */
#ifdef PC_VISIONOS
    /* Apple Vision Pro: first person in the immersive spaces (a life-size town),
     * the game's own camera in the window. */
    if (pc_vr_active() != g_pc_fp_mode) pc_fp_toggle();
#endif
    pc_disc_init();
    if (!pc_assets_init()) {
        const char* msg =
            "No game data found.\n\n"
            "Animal Crossing needs the original GameCube ROM to run.\n"
            "Place a disc image (.iso, .gcm, or .ciso) to the \"rom\" subfolder.";
        fprintf(stderr, "[PC] %s\n", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,
                                 "Animal Crossing - Missing ROM", msg, g_pc_window);
        pc_platform_shutdown();
        return 1;
    }

    ac_entry();                         /* sets HotStartEntry = &entry */
    boot_main(argc, (const char**)argv); /* full init → HotStartEntry → game loop */

    pc_vr_shutdown();
    pc_disc_shutdown();
    pc_platform_shutdown();
    return pc_model_viewer_shot_exit_code();
}
