#ifndef MELEE_MN_MNSLIPPI_H
#define MELEE_MN_MNSLIPPI_H

#include <stdbool.h>

/* PC only: Online Mode > Slippi > Login and Register, and the setup steps
 * after them (src/pc/slippi_menu.h), in the online lobby scene. A page in
 * VS. Mode's red frame (MENU_KIND_SLIPPI_PAGE: Name Entry's panel animation,
 * its sub-title swapped for "Login", "Register" or "Setup"), with Melee's
 * keyboard opened in its place for each field. */

/* From the scene's enter: `register_` for Register, else Login. */
void mnSlippi_Begin(bool register_);
/* Each scene frame while the keyboard isn't up. */
void mnSlippi_Frame(void);
/* From the scene's exit. */
void mnSlippi_End(void);

/* The tab's sub-title while the page is up (mnfight.c draws it). */
const char* mnSlippi_Title(void);

#endif
