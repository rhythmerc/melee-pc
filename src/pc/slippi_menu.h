/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Slippi pages (Online Mode > Slippi > Login or Register): sign in, make
 * an account, and the Launcher's setup steps for what an account still lacks
 * (verify the email, accept Slippi's rules, choose a connect code), on the
 * account calls in slp_user.h.
 *
 * Drawn in RmlUi into the native page's frame (src/melee/mn/mnslippi.c, the
 * red VS. Mode frame) over the running menu, as Fight Controls is: the page
 * owns the frame, the tab, the hint box and the input. Typing is Melee's own
 * keyboard: the pages ask for it (pc_slippi_menu_keyboard) and the page opens
 * it in place of the frame, then hands back what was typed. */
#ifndef PC_SLIPPI_MENU_H
#define PC_SLIPPI_MENU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PC_SLIPPI_MENU_LOGIN,
    PC_SLIPPI_MENU_REGISTER,
};

enum {
    PC_SLIPPI_IN_UP,
    PC_SLIPPI_IN_DOWN,
    PC_SLIPPI_IN_LEFT,
    PC_SLIPPI_IN_RIGHT,
    PC_SLIPPI_IN_CONFIRM,
    PC_SLIPPI_IN_BACK,
    PC_SLIPPI_IN_X,
    PC_SLIPPI_IN_Y,
    PC_SLIPPI_IN_START,
};

/* A keyboard the pages want opened. */
typedef struct PcSlippiKeyboard {
    char initial[100];
    char prompt[100]; /* the keyboard's hint box */
    int max_len;
    bool mask;      /* a password */
    bool shiftable; /* lowercase with Shift (an email, a password); a name too */
} PcSlippiKeyboard;

/* A new visit: Login's form, or Register's first keyboard. */
void pc_slippi_menu_begin(int mode);
/* The visit is over: the tokens are wiped and the overlay hidden. */
void pc_slippi_menu_end(void);
void pc_slippi_menu_show(bool show);
/* The page's fade, each frame while shown: 0 gone, 1 shown. Input waits for 1. */
void pc_slippi_menu_presence(float presence);
void pc_slippi_menu_input(int what);
/* True once, with the keyboard to open, when a page asked for one. */
bool pc_slippi_menu_keyboard(PcSlippiKeyboard* out);
/* What the keyboard ended with (`confirmed` false for Back). */
void pc_slippi_menu_keyboard_done(bool confirmed, const char* text);
/* True when the pages are done (Back, or Continue after setting up). */
bool pc_slippi_menu_leave(void);
/* The tab's sub-title and the hint box's text. */
const char* pc_slippi_menu_title(void);
const char* pc_slippi_menu_hint(void);
/* Once a frame (vi.c): placement, the account calls' answers, the lasers. */
void pc_slippi_menu_update(void);

#ifdef __cplusplus
}
#endif

#endif
