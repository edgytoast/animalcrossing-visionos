/* pc_test_hooks.c - headless test hooks (macOS bench and the visionOS Simulator).
 *
 * Everything here is driven by environment variables and does nothing when
 * they are unset:
 *
 *   ACVR_TEST_ACTIONS  scripted pad input, comma separated:
 *                        NAME[=x,y]@start[~duration][*count/period]
 *                      NAME: A B X Y Z L R START UP DOWN LEFT RIGHT (D-pad),
 *                            SPEED=1|0 (the speed-up on or off while live),
 *                            HOUSE (walk to the nearest player house's door and press A),
 *                            GYROID (walk to the nearest gyroid and talk: A takes "save"),
 *                            GIVE=n (a tool in the pockets: 1 net 2 axe 3 shovel 4 rod; RIGHT takes it out),
 *                            STICK=x,y and CSTICK=x,y (-127..127, +y is up),
 *                            TEXT=word (typed into the game's text editor,
 *                            then Enter; '_' types a space)
 *                      e.g. "A@8~0.1*40/0.6,STICK=0,100@60~3,START@70~0.1"
 *   ACVR_TEST_SHOTS    seconds at which to save the frame, e.g. "10,20,30" or "100-200/5"
 *   ACVR_TEST_SHOT_DIR where shots go (default: shots), as <dir>/shot_<sec>.ppm
 *   ACVR_TEST_QUIT     seconds after which the game quits cleanly
 *   ACVR_TEST_CAMLOG=1 the game camera every frame it is set (process, eye,
 *                      center, fovy), to see what a scripted camera does
 *   ACVR_TEST_SPEEDHACK=1  the game's own speed-up (F3) from the start
 *
 * Time runs from the first PADRead. Logs are prefixed [ACVR]. */
#include "pc_platform.h"
#include "pc_typing.h"
#include "pc_test_hooks.h"
#include "dolphin/pad.h"

#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_ACTIONS 128
#define MAX_SHOTS 256

typedef struct {
    char name[16];
    int x, y;
    char text[64];
    double start, dur, period;
    int count;
    int done; /* one-shot actions (GIVE) */
} TestAction;

static int s_inited = 0;
static TestAction s_actions[MAX_ACTIONS];
static int s_num_actions = 0;
static double s_shots[MAX_SHOTS];
static int s_shot_done[MAX_SHOTS];
static int s_num_shots = 0;
static double s_quit_at = -1.0;
static double s_t0 = -1.0;
static char s_shot_dir[512] = "shots";
static int s_text_done[MAX_ACTIONS];

pc_test_capture_fn g_pc_test_capture = NULL;

static double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double elapsed(void) {
    if (s_t0 < 0) return -1.0;
    return now_seconds() - s_t0;
}

static void parse_actions(const char* spec) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", spec);
    char* save = NULL;
    for (char* tok = strtok_r(buf, ",", &save); tok && s_num_actions < MAX_ACTIONS;
         tok = strtok_r(NULL, ",", &save)) {
        TestAction a;
        memset(&a, 0, sizeof(a));
        a.dur = 0.1;
        a.count = 1;
        char* at = strchr(tok, '@');
        if (!at) continue;
        *at = '\0';
        char* eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0';
            if (strcmp(tok, "TEXT") == 0) {
                snprintf(a.text, sizeof(a.text), "%s", eq + 1);
            } else {
                /* value is "x;y" (normalize_spec turned the comma into ';') */
                char* end = NULL;
                a.x = (int)strtol(eq + 1, &end, 10);
                if (end && *end == ';') a.y = (int)strtol(end + 1, NULL, 10);
            }
        }
        snprintf(a.name, sizeof(a.name), "%s", tok);
        for (char* p = a.name; *p; p++) *p = (char)toupper((unsigned char)*p);
        char* rest = at + 1;
        a.start = strtod(rest, &rest);
        if (*rest == '~') a.dur = strtod(rest + 1, &rest);
        if (*rest == '*') {
            a.count = (int)strtol(rest + 1, &rest, 10);
            if (*rest == '/') a.period = strtod(rest + 1, &rest);
        }
        if (a.count < 1) a.count = 1;
        s_actions[s_num_actions++] = a;
    }
}

/* STICK=x,y collides with the ',' separator, so pre-rewrite "=x,y@" as "=x;y@". */
static void normalize_spec(const char* in, char* out, size_t cap) {
    size_t o = 0;
    int in_value = 0;
    for (size_t i = 0; in[i] && o + 1 < cap; i++) {
        char c = in[i];
        if (c == '=') in_value = 1;
        else if (c == '@') in_value = 0;
        else if (c == ',' && in_value) c = ';';
        out[o++] = c;
    }
    out[o] = '\0';
}

static void init(void) {
    if (s_inited) return;
    s_inited = 1;
    const char* v = getenv("ACVR_TEST_ACTIONS");
    if (v && *v) {
        char norm[4096];
        normalize_spec(v, norm, sizeof(norm));
        parse_actions(norm);
        fprintf(stderr, "[ACVR] test actions: %d\n", s_num_actions);
    }
    v = getenv("ACVR_TEST_SHOTS");
    if (v && *v) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s", v);
        char* save = NULL;
        for (char* tok = strtok_r(buf, ",", &save); tok && s_num_shots < MAX_SHOTS;
             tok = strtok_r(NULL, ",", &save)) {
            /* "from-to/step" is a range of shots */
            double a, b, step;
            if (sscanf(tok, "%lf-%lf/%lf", &a, &b, &step) == 3 && step > 0.0) {
                for (double t = a; t <= b + 1e-6 && s_num_shots < MAX_SHOTS; t += step) s_shots[s_num_shots++] = t;
            } else {
                s_shots[s_num_shots++] = atof(tok);
            }
        }
    }
    /* ACVR_TEST_SPEEDHACK=1: the game's own 5x speed-up (F3), for long scripted runs */
    v = getenv("ACVR_TEST_SPEEDHACK");
    if (v && *v == '1') {
        extern int g_pc_speedhack_enabled;
        g_pc_speedhack_enabled = 1;
        fprintf(stderr, "[ACVR] test speedhack on\n");
    }
    v = getenv("ACVR_TEST_SHOT_DIR");
    if (v && *v) snprintf(s_shot_dir, sizeof(s_shot_dir), "%s", v);
    v = getenv("ACVR_TEST_QUIT");
    if (v && *v) s_quit_at = atof(v);
}

int pc_test_hooks_active(void) {
    init();
    return s_num_actions > 0 || s_num_shots > 0 || s_quit_at >= 0;
}

static int action_live(const TestAction* a, double t) {
    for (int i = 0; i < a->count; i++) {
        double s = a->start + (double)i * a->period;
        if (t >= s && t < s + a->dur) return 1;
    }
    return 0;
}

void pc_test_hooks_pad(u16* buttons, s8* sx, s8* sy, s8* cx, s8* cy, u8* tl, u8* tr) {
    init();
    if (s_num_actions == 0) return;
    if (s_t0 < 0) s_t0 = now_seconds();
    double t = elapsed();
    /* TEXT actions type one at a time: the next waits until the editor has
     * closed since the last one. While the hook types, no buttons go in. */
    static int s_typing_owned = 0, s_editor_closed = 1;
    if (!g_pc_editor_active) {
        s_editor_closed = 1;
        if (s_typing_owned) { g_pc_typing_mode = 0; s_typing_owned = 0; }
    }
    if (s_typing_owned) return;
    /* TEXT actions first (a pass of their own), then the buttons. */
    for (int i = 0; i < s_num_actions; i++) {
        TestAction* a = &s_actions[i];
        if (!a->text[0]) continue;
        {
            if (!s_text_done[i] && s_editor_closed && t >= a->start && g_pc_editor_active) {
                s_editor_closed = 0;
                s_typing_owned = 1;
                s_text_done[i] = 1;
                g_pc_typing_mode = 1;
                pc_typing_queue_clear();
                for (const char* p = a->text; *p; p++) {
                    char ch[2] = { *p == '_' ? ' ' : *p, 0 };
                    int code = pc_utf8_to_game_code(ch);
                    if (code >= 0) pc_typing_queue_push(code);
                }
                pc_typing_queue_push(PC_TYPING_CMD_ENTER);
                fprintf(stderr, "[ACVR] typed '%s' at %.1fs\n", a->text, t);
                return;
            }
        }
    }
    /* An open editor waiting for a TEXT action takes no buttons (they'd type). */
    if (g_pc_editor_active) {
        for (int j = 0; j < s_num_actions; j++)
            if (s_actions[j].text[0] && !s_text_done[j]) return;
    }
    for (int i = 0; i < s_num_actions; i++) {
        TestAction* a = &s_actions[i];
        if (a->text[0]) continue;
        if (!action_live(a, t)) continue;
        const char* n = a->name;
        if (!strcmp(n, "A")) *buttons |= PAD_BUTTON_A;
        else if (!strcmp(n, "B")) *buttons |= PAD_BUTTON_B;
        else if (!strcmp(n, "X")) *buttons |= PAD_BUTTON_X;
        else if (!strcmp(n, "Y")) *buttons |= PAD_BUTTON_Y;
        else if (!strcmp(n, "Z")) *buttons |= PAD_TRIGGER_Z;
        else if (!strcmp(n, "L")) { *buttons |= PAD_TRIGGER_L; *tl = 255; }
        else if (!strcmp(n, "R")) { *buttons |= PAD_TRIGGER_R; *tr = 255; }
        else if (!strcmp(n, "START")) *buttons |= PAD_BUTTON_START;
        else if (!strcmp(n, "UP")) *buttons |= PAD_BUTTON_UP;
        else if (!strcmp(n, "DOWN")) *buttons |= PAD_BUTTON_DOWN;
        else if (!strcmp(n, "LEFT")) *buttons |= PAD_BUTTON_LEFT;
        else if (!strcmp(n, "RIGHT")) *buttons |= PAD_BUTTON_RIGHT;
        else if (!strcmp(n, "STICK")) { *sx = (s8)a->x; *sy = (s8)a->y; }
        else if (!strcmp(n, "CSTICK")) { *cx = (s8)a->x; *cy = (s8)a->y; }
        else if (!strcmp(n, "SPEED")) { extern int g_pc_speedhack_enabled; g_pc_speedhack_enabled = a->x != 0; }
        else if (!strcmp(n, "HOUSE")) {
            /* pc_test_autopilot.c: to the nearest player house's door, then A */
            extern int pc_test_autopilot_house(u16* buttons, s8* sx, s8* sy);
            pc_test_autopilot_house(buttons, sx, sy);
        }
        else if (!strcmp(n, "GIVE")) {
            /* once per action */
            extern int pc_test_give_tool(int which);
            if (!a->done) a->done = pc_test_give_tool(a->x);  /* retried while live until it lands */
        }
        else if (!strcmp(n, "GYROID")) {
            extern int pc_test_autopilot_gyroid(u16* buttons, s8* sx, s8* sy);
            pc_test_autopilot_gyroid(buttons, sx, sy);
        }
    }
}

static void write_ppm(const char* path, const u8* rgba, int w, int h, int bottom_up) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "[ACVR] shot: can't write %s (%s)\n", path, strerror(errno));
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    u8* row = (u8*)malloc((size_t)w * 3);
    for (int y = 0; y < h; y++) {
        int sy = bottom_up ? (h - 1 - y) : y;
        const u8* src = rgba + (size_t)sy * w * 4;
        for (int x = 0; x < w; x++) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        fwrite(row, 1, (size_t)w * 3, f);
    }
    free(row);
    fclose(f);
    fprintf(stderr, "[ACVR] shot %s (%dx%d)\n", path, w, h);
}

void pc_test_hooks_frame(void) {
    init();
    if (s_num_shots == 0 && s_quit_at < 0) return;
    if (s_t0 < 0) s_t0 = now_seconds();
    double t = elapsed();
    for (int i = 0; i < s_num_shots; i++) {
        if (s_shot_done[i] || t < s_shots[i]) continue;
        s_shot_done[i] = 1;
        if (!g_pc_test_capture) {
            fprintf(stderr, "[ACVR] shot at %.0fs: no capture function\n", s_shots[i]);
            continue;
        }
        int w = 0, h = 0, bottom_up = 0;
        u8* rgba = g_pc_test_capture(&w, &h, &bottom_up);
        if (!rgba) continue;
        mkdir(s_shot_dir, 0755);
        char path[600];
        /* Whole seconds keep their old names; fractions (e.g. "10-20/0.5") get a decimal */
        if (s_shots[i] == (double)(int)s_shots[i])
            snprintf(path, sizeof(path), "%s/shot_%03d.ppm", s_shot_dir, (int)s_shots[i]);
        else
            snprintf(path, sizeof(path), "%s/shot_%05.1f.ppm", s_shot_dir, s_shots[i]);
        write_ppm(path, rgba, w, h, bottom_up);
        free(rgba);
    }
    if (s_quit_at >= 0 && t >= s_quit_at) {
        fprintf(stderr, "[ACVR] test quit at %.1fs\n", t);
        g_pc_running = 0;
    }
}

void pc_test_log_camera(int process, const float eye[3], const float center[3], float fovy, int first_person) {
    static int s_on = -1;
    if (s_on < 0) s_on = getenv("ACVR_TEST_CAMLOG") != NULL;
    if (!s_on) return;
    if (s_t0 < 0) s_t0 = now_seconds();
    fprintf(stderr, "[ACVR] cam t=%.2f process=%d fp=%d eye=(%.1f,%.1f,%.1f) center=(%.1f,%.1f,%.1f) fovy=%.2f\n",
            elapsed(), process, first_person, eye[0], eye[1], eye[2], center[0], center[1], center[2], fovy);
}
