/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Fight Controls screen (Options > Fight Controls; fight_map.h).
 *
 * The active device's picture in the middle, a box per input down both sides
 * with a gold line to its part, in Melee's line work. Picking a box lists
 * what that input can do in a fight, (none) first. Drawn in RmlUi into the
 * native Options page's frame (src/melee/mn/mnfight.c) over the running
 * menu, with no backdrop of its own: the page owns the frame, the tab, the
 * hint box and the input, and fades the screen in and out with its
 * transitions. Lasers and the mouse point and click; the menus' input moves
 * between boxes, A picks, B backs out.
 */
#ifndef PC_FIGHT_MENU_H
#define PC_FIGHT_MENU_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PC_FIGHT_MENU_UP,
    PC_FIGHT_MENU_DOWN,
    PC_FIGHT_MENU_LEFT,
    PC_FIGHT_MENU_RIGHT,
    PC_FIGHT_MENU_CONFIRM,
    PC_FIGHT_MENU_BACK,
};

void pc_fight_menu_show(bool show);
/* The page's fade, each frame while shown: 0 gone, 1 shown. Input waits for 1. */
void pc_fight_menu_presence(float presence);
/* The page's menu input; false when there was nothing to do (B with no list
 * open: the page goes back). */
bool pc_fight_menu_input(int what);
/* What the hint box says for the cursor; NULL when not shown. */
const char* pc_fight_menu_hint(void);
/* Once a frame (vi.c): placement, the list's pop, the lasers. */
void pc_fight_menu_update(void);

#ifdef __cplusplus
}
#endif

#endif
