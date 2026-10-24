#ifndef MELEE_MN_MNFIGHT_H
#define MELEE_MN_MNFIGHT_H

#include "forward.h"

#include <sysdolphin/baselib/forward.h>

/* PC only: Options > Fight Controls (MENU_KIND_SETTINGS_FIGHT), what each
 * input does in a fight (src/pc/fight_map.h). An Options screen like Sound:
 * the Options rows leave, the frame's tab says "Fight Controls" (Erase
 * Data's panel animation, its title swapped for SIS text), and B goes back.
 * The hint box says what the cursor is on. */

/* Entered from the Options menu's think, as mnSound_8024A09C is. */
void mnFight_Init(void);

/* Each frame, from the panel's think (fn_80229BF4): the tab's title. The
 * panel is `panel`, showing `cur` (and leaving `prev` when `leaving`). */
void mnFight_PanelTitle(HSD_JObj* panel, MenuKind cur, MenuKind prev, bool leaving);

#endif
