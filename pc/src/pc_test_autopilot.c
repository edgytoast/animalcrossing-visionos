/* pc_test_autopilot.c - test hooks that need the game's own state (pc_test_hooks.c's
 * actions call them). Trevorbilt, 2026.
 *
 * HOUSE: walks the player to the nearest player house's door, faces it and
 * presses A: the new game's house choice (ac_my_house_move.c_inc's
 * aMHS_check_player_sub), which lands somewhere different in every new town, so
 * a fixed stick script can't find it. Used to script a new game through to a
 * save on the macOS bench and in the Simulator.
 *
 * GYROID: walks to the nearest gyroid (ac_haniwa, the one by the house just
 * chosen) and presses A while leaning into it: talking to it is how the game saves.
 *
 * GIVE=n: puts a tool in the first empty pocket (1 net, 2 axe, 3 shovel, 4 rod); the
 * D-pad's right then takes it out (m_player.c's tool cycling), to test tools in hand. */
#include "m_play.h"
#include "m_actor.h"
#include "ac_structure.h"
#include "ac_haniwa.h"
#include "m_camera2.h"
#include "m_player_lib.h"
#include "m_private.h"
#include "m_common_data.h"
#include "m_name_table.h"
#include "game.h"
#include "dolphin/pad.h"

#include <math.h>
#include <stdio.h>

#define AP_TAU 6.28318530718f

/* The stick that walks the player towards world heading h (binary angle: x += sin, z += cos):
 * heading = 270 deg + stick angle + camera yaw (m_player_common.c_inc). */
static void ap_stick_for_heading(GAME_PLAY* play, float heading_bang, float magnitude, s8* sx, s8* sy) {
    const float cam = (float)(s16)getCamera2AngleY(play);
    const float stick = heading_bang - 49152.0f - cam;
    const float a = stick * (AP_TAU / 65536.0f);
    *sx = (s8)(cosf(a) * magnitude);
    *sy = (s8)(sinf(a) * magnitude);
}

/* Only while the running game is play (the title, the select screens and the
 * trademark are other game types: reading play state there reads garbage). */
static int ap_in_play(void) {
    return gamePT != NULL && gamePT->exec == play_main;
}

int pc_test_autopilot_house(u16* buttons, s8* sx, s8* sy) {
    static int s_phase = 0, s_frames = 0;
    if (!ap_in_play()) return 0;
    GAME_PLAY* play = (GAME_PLAY*)gamePT;
    PLAYER_ACTOR* player = GET_PLAYER_ACTOR(play);
    if (player == NULL) return 0;
    const float px = player->actor_class.world.position.x, pz = player->actor_class.world.position.z;

    STRUCTURE_ACTOR* best = NULL;
    float best_d = 1e30f;
    for (ACTOR* a = Actor_info_name_search(&play->actor_info, mAc_PROFILE_MYHOUSE, ACTOR_PART_ITEM); a != NULL;
         a = Actor_info_name_search_sub(a->next_actor, mAc_PROFILE_MYHOUSE)) {
        STRUCTURE_ACTOR* h = (STRUCTURE_ACTOR*)a;
        const float dx = h->arg0_f - px, dz = h->arg1_f - pz;
        const float d = dx * dx + dz * dz;
        if (d < best_d) {
            best_d = d;
            best = h;
        }
    }
    if (best == NULL) return 0;

    const float dx = best->arg0_f - px, dz = best->arg1_f - pz;
    const float dist = sqrtf(dx * dx + dz * dz);
    /* aMHS_player_chk_angl0/1: facing (-180, -90) degrees for even houses, (90, 180) for odd */
    const float face = (best->action & 1) ? 24576.0f /* 135 deg */ : 40960.0f /* -135 deg */;
    if (s_frames++ % 60 == 0)
        fprintf(stderr, "[ACVR] autopilot: house %d at %.0f units, phase %d\n", best->action, dist, s_phase);

    if (dist > 14.0f) {
        /* Not moving while pushed: a message is up (Nook's "take your pick"): tap A through it. */
        static float last_x = 0.0f, last_z = 0.0f;
        static int still = 0;
        if (fabsf(px - last_x) + fabsf(pz - last_z) < 0.5f) still++;
        else still = 0;
        last_x = px;
        last_z = pz;
        s_phase = 0;
        if (still > 20) {
            if (still > 44) still = 0;  /* then push again */
            if ((still / 4) % 2 == 0) *buttons |= PAD_BUTTON_A;
            return 1;
        }
        const float heading = atan2f(dx, dz) * (65536.0f / AP_TAU);
        ap_stick_for_heading(play, heading, dist > 40.0f ? 90.0f : 45.0f, sx, sy);
        return 1;
    }
    /* At the door: turn to it (a short, slow push), then press A on alternate frames. */
    if (s_phase < 12) {
        s_phase++;
        ap_stick_for_heading(play, face, 30.0f, sx, sy);
        return 1;
    }
    s_phase++;
    if ((s_phase / 3) % 2 == 0) *buttons |= PAD_BUTTON_A;
    return 1;
}

int pc_test_autopilot_gyroid(u16* buttons, s8* sx, s8* sy) {
    static int s_frames = 0, s_near = 0;
    if (!ap_in_play()) return 0;
    GAME_PLAY* play = (GAME_PLAY*)gamePT;
    PLAYER_ACTOR* player = GET_PLAYER_ACTOR(play);
    if (player == NULL) {
        if ((s_frames++ / 4) % 2 == 0) *buttons |= PAD_BUTTON_A;
        return 1;
    }
    const float px = player->actor_class.world.position.x, pz = player->actor_class.world.position.z;

    ACTOR* best = NULL;
    float best_d = 1e30f;
    for (ACTOR* a = Actor_info_name_search(&play->actor_info, mAc_PROFILE_HANIWA, ACTOR_PART_BG); a != NULL;
         a = Actor_info_name_search_sub(a->next_actor, mAc_PROFILE_HANIWA)) {
        const float dx = a->world.position.x - px, dz = a->world.position.z - pz;
        const float d = dx * dx + dz * dz;
        if (d < best_d) {
            best_d = d;
            best = a;
        }
    }
    if (best == NULL) {
        /* No gyroid here: the save scene that follows the talk (a villager in a spotlight,
         * ac_npc_restart): A goes through it and takes its first choice, save. */
        if ((s_frames++ / 4) % 2 == 0) *buttons |= PAD_BUTTON_A;
        return 1;
    }
    const float dx = best->world.position.x - px, dz = best->world.position.z - pz;
    const float dist = sqrtf(dx * dx + dz * dz);
    const float heading = atan2f(dx, dz) * (65536.0f / AP_TAU);
    if (s_frames++ % 60 == 0) fprintf(stderr, "[ACVR] autopilot: gyroid at %.0f units\n", dist);
    if (dist > 36.0f && s_near < 30) {
        /* Not moving while pushed: someone is talking: tap A through it. */
        static float last_x = 0.0f, last_z = 0.0f;
        static int still = 0;
        if (fabsf(px - last_x) + fabsf(pz - last_z) < 0.5f) still++;
        else still = 0;
        last_x = px;
        last_z = pz;
        if (still > 20) {
            if (still > 44) still = 0;
            if ((still / 4) % 2 == 0) *buttons |= PAD_BUTTON_A;
            return 1;
        }
        ap_stick_for_heading(play, heading, dist > 60.0f ? 80.0f : 40.0f, sx, sy);
        return 1;
    }
    /* Beside it: lean in to face it, then A on alternate frames (it talks; A goes through
     * the messages and takes the first choice, "save"). */
    s_near++;
    if (s_near < 16) ap_stick_for_heading(play, heading, 25.0f, sx, sy);
    else if ((s_near / 3) % 2 == 0) *buttons |= PAD_BUTTON_A;
    return 1;
}

/* Returns 1 once the tool is in the pockets; 0 to try again (not in play yet, or full). */
int pc_test_give_tool(int which) {
    static const mActor_name_t tools[] = { ITM_NET, ITM_AXE, ITM_SHOVEL, ITM_ROD };
    if (which < 1 || which > 4) return 1;  /* nothing to give: done */
    if (!ap_in_play() || Now_Private == NULL || GET_PLAYER_ACTOR((GAME_PLAY*)gamePT) == NULL) return 0;
    const int slot = mPr_GetPossessionItemIdx(Now_Private, EMPTY_NO);
    if (slot < 0) {
        fprintf(stderr, "[ACVR] give: pockets full\n");
        return 0;
    }
    mPr_SetPossessionItem(Now_Private, slot, tools[which - 1], mPr_ITEM_COND_NORMAL);
    fprintf(stderr, "[ACVR] give: tool %d in pocket %d\n", which, slot);
    return 1;
}
