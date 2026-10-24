/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fight Controls: what each input does in a fight, per device type.
 *
 * Each input of the Touch controllers and of a gamepad gets a fight action
 * (Move, Smash attack for a stick; Shield, Grab, Jump, Attack, Special,
 * Taunt for the rest), or none. The mapping applies only while a fight runs:
 * not paused, not held for placing the stage, not under the F1 menu. Menus
 * and pauses read the usual buttons. One mapping per device type, kept in
 * fight-controls.txt beside launcher.cfg. Aurora reads the devices through
 * it (PADSetFightMapping), before netplay or a recording sees the pad, so
 * peers and replays see plain GameCube input. Start always pauses.
 */
#ifndef PC_FIGHT_MAP_H
#define PC_FIGHT_MAP_H

#include <dolphin/pad.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PC_FIGHT_NONE,
    PC_FIGHT_MOVE,  /* sticks */
    PC_FIGHT_SMASH, /* sticks */
    PC_FIGHT_SHIELD,
    PC_FIGHT_GRAB,
    PC_FIGHT_JUMP,
    PC_FIGHT_ATTACK,
    PC_FIGHT_SPECIAL,
    PC_FIGHT_TAUNT,
    PC_FIGHT_ACTION_COUNT,
} PcFightAction;

/* Reads fight-controls.txt from the user directory (before aurora starts). */
void pc_fight_map_configure(const char* user_path);
/* Per frame: whether a fight runs, for Aurora's readers. */
void pc_fight_map_frame(void);
bool pc_fight_map_live(void);

/* An input of a device (PAD_FIGHT_*) and its count. */
int pc_fight_map_input_count(PADFightDevice device);
bool pc_fight_map_is_stick(int input);
PcFightAction pc_fight_map_get(PADFightDevice device, int input);
PcFightAction pc_fight_map_default(PADFightDevice device, int input);
/* Sets one input and saves the file. */
void pc_fight_map_set(PADFightDevice device, int input, PcFightAction action);
void pc_fight_map_reset(PADFightDevice device);

#ifdef __cplusplus
}
#endif

#endif
