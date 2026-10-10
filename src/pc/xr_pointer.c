/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "compat.h"
#include "pc/xr_pointer.h"

#ifdef AURORA_ENABLE_OPENXR

#include "pc/net.h"
#include "pc/widescreen.h"

#include <SDL3/SDL.h>
#include <aurora/xr.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/mtx.h>
#include <sysdolphin/baselib/cobj.h>
#include <sysdolphin/baselib/video.h>

#include <math.h>
#include <stdlib.h>

/* Logical pixels the pointer moves from where the pad took over before it
 * leads again: more than a hand's tremor at arm's length. */
#define POINTER_WAKE 8.f

static bool s_valid, s_pressed, s_leads;
static float s_x, s_y, s_rest_x, s_rest_y;
static bool s_armed;           /* this click presses A */
static unsigned s_frame;       /* pc_xr_pointer_frame calls */
static unsigned s_target_frame; /* the last frame a menu followed the pointer */
static bool s_target_over;

static bool mouse_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char* v = getenv("MELEE_POINTER_MOUSE");
        on = v != NULL && *v == '1';
    }
    return on;
}

/* MELEE_POINTER_MOUSE=1: the mouse over the window, in the frame's 0..1. */
static bool mouse_pointer(float* x, float* y, bool* pressed)
{
    const bool on = mouse_on();
    /* The global state over the game's window: synthetic test input
     * (xdotool) never gives the window mouse focus. */
    SDL_Window* win = NULL;
    if (on) {
        int n = 0;
        SDL_Window** wins = SDL_GetWindows(&n);
        if (wins != NULL && n > 0) {
            win = wins[0];
        }
        SDL_free(wins);
    }
    int ww, wh, wx, wy;
    u32 rw, rh;
    float mx, my;
    if (win == NULL || !SDL_GetWindowSize(win, &ww, &wh) || !SDL_GetWindowPosition(win, &wx, &wy) ||
        ww <= 0 || wh <= 0)
    {
        return false;
    }
    AuroraGetRenderSize(&rw, &rh);
    if (rw == 0 || rh == 0) {
        return false;
    }
    /* The frame is letterboxed in the window at its own aspect. */
    const float k = fminf((float) ww / (float) rw, (float) wh / (float) rh);
    const float cw = (float) rw * k, ch = (float) rh * k;
    const SDL_MouseButtonFlags b = SDL_GetGlobalMouseState(&mx, &my);
    mx -= (float) wx;
    my -= (float) wy;
    *x = (mx - ((float) ww - cw) * 0.5f) / cw;
    *y = (my - ((float) wh - ch) * 0.5f) / ch;
    *pressed = (b & SDL_BUTTON_LMASK) != 0;
    return *x >= 0.f && *x <= 1.f && *y >= 0.f && *y <= 1.f;
}

u16 pc_xr_pointer_frame(const PADStatus* pad)
{
    const GXRenderModeObj* rmode = HSD_VIGetRenderMode();
    const bool was_valid = s_valid, was_pressed = s_pressed;
    float x = 0.f, y = 0.f;
    bool pressed = false;
    u16 back = 0;
    ++s_frame;
    /* The Back button beside the screen's bar (hands), or the right mouse
     * button standing in for it. */
    if (!pc_net_active() &&
        (aurora_xr_screen_back() ||
         (mouse_on() && (SDL_GetGlobalMouseState(NULL, NULL) & SDL_BUTTON_RMASK))))
    {
        back = PAD_BUTTON_B;
    }
    s_valid = !pc_net_active() && (aurora_xr_screen_pointer(&x, &y, &pressed) ||
                                   mouse_pointer(&x, &y, &pressed));
    if (!s_valid) {
        s_pressed = s_armed = false;
        return back;
    }
    s_x = x * rmode->fbWidth;
    s_y = y * rmode->efbHeight;
    s_pressed = pressed;
    /* The pad steering takes the lead; the pointer coming onto the
     * picture, moving off from where the pad took over, or clicking takes
     * it back. */
    if (abs(pad->stickX) > 30 || abs(pad->stickY) > 30 ||
        (pad->button & (PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT)))
    {
        s_leads = false;
        s_rest_x = s_x;
        s_rest_y = s_y;
    } else if (!was_valid || hypotf(s_x - s_rest_x, s_y - s_rest_y) > POINTER_WAKE ||
               (s_pressed && !was_pressed))
    {
        s_leads = true;
    }
    if (s_pressed && !was_pressed) {
        /* A menu that follows the pointer took last frame's spot; a click
         * on nothing there selects nothing. Elsewhere it is plain A. */
        const bool following = s_frame - s_target_frame <= 2;
        s_armed = following ? s_target_over : true;
    } else if (!s_pressed) {
        s_armed = false;
    }
    return back | (s_armed ? PAD_BUTTON_A : 0);
}

bool pc_xr_pointer_at(float* x, float* y, bool* leads)
{
    if (!s_valid) {
        return false;
    }
    *x = s_x;
    *y = s_y;
    *leads = s_leads;
    return true;
}

void pc_xr_pointer_target(bool over)
{
    s_target_frame = s_frame;
    s_target_over = over;
}

/* The camera's view and projection as setupNormalCamera submits them, and
 * its viewport in logical pixels. */
static void camera(HSD_CObj* cobj, Mtx view, Mtx44 proj, float vp[4])
{
    const GXRenderModeObj* rmode = HSD_VIGetRenderMode();
    const float xs = (float) rmode->fbWidth / (float) rmode->viWidth;
    const float ys = (float) rmode->efbHeight / (float) rmode->viHeight;
    HSD_CObjGetViewingMtx(cobj, view);
    const GXProjectionType type = makeProjectionMtx(cobj, proj);
    const float s = pc_widescreen_cobj_scale(cobj);
    proj[0][0] /= s;
    if (type == GX_ORTHOGRAPHIC) {
        proj[0][3] /= s;
    } else {
        proj[0][2] /= s;
    }
    vp[0] = cobj->viewport.xmin * xs;
    vp[1] = cobj->viewport.ymin * ys;
    vp[2] = (cobj->viewport.xmax - cobj->viewport.xmin) * xs;
    vp[3] = (cobj->viewport.ymax - cobj->viewport.ymin) * ys;
}

/* clip = proj * view, as a 4x4 acting on (x, y, z, 1). */
static void clip_matrix(HSD_CObj* cobj, float m[4][4], float vp[4])
{
    Mtx view;
    Mtx44 proj;
    camera(cobj, view, proj, vp);
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            m[r][c] = proj[r][0] * view[0][c] + proj[r][1] * view[1][c] + proj[r][2] * view[2][c] +
                      (c == 3 ? proj[r][3] : 0.f);
        }
    }
}

bool pc_xr_pointer_project(HSD_CObj* cobj, const float p[3], float* x, float* y)
{
    float m[4][4], vp[4], c[4];
    clip_matrix(cobj, m, vp);
    for (int r = 0; r < 4; r++) {
        c[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3];
    }
    if (c[3] <= 1e-6f) {
        return false;
    }
    *x = vp[0] + (c[0] / c[3] + 1.f) * 0.5f * vp[2];
    *y = vp[1] + (1.f - c[1] / c[3]) * 0.5f * vp[3];
    return true;
}

bool pc_xr_pointer_unproject(HSD_CObj* cobj, float x, float y, float z, float out[3])
{
    float m[4][4], vp[4];
    clip_matrix(cobj, m, vp);
    if (vp[2] == 0.f || vp[3] == 0.f) {
        return false;
    }
    const float nx = (x - vp[0]) / vp[2] * 2.f - 1.f;
    const float ny = 1.f - (y - vp[1]) / vp[3] * 2.f;
    /* clip.x - nx * clip.w = 0 and clip.y - ny * clip.w = 0, solved for
     * the world x and y at this z. */
    const float a = m[0][0] - nx * m[3][0], b = m[0][1] - nx * m[3][1];
    const float e = -((m[0][2] - nx * m[3][2]) * z + m[0][3] - nx * m[3][3]);
    const float c = m[1][0] - ny * m[3][0], d = m[1][1] - ny * m[3][1];
    const float f = -((m[1][2] - ny * m[3][2]) * z + m[1][3] - ny * m[3][3]);
    const float det = a * d - b * c;
    if (fabsf(det) < 1e-9f) {
        return false;
    }
    out[0] = (e * d - b * f) / det;
    out[1] = (a * f - e * c) / det;
    out[2] = z;
    return true;
}

#endif
