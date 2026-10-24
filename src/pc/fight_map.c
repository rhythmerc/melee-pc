/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Fight Controls (fight_map.h). */
#include "pc/fight_map.h"

#include "pc/launcher.h"
#include "pc/pc.h"
#include "pc/xr_place.h"

#include <melee/gm/forward.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* gmscene.c, gmvs.c, net.c */
bool gm_GetDbPauseFlag(int bit);
bool gm_vs_soft_paused(void);
int scene_kind(void);

static const char* const kActionNames[PC_FIGHT_ACTION_COUNT] = {
    "none", "move", "smash", "shield", "grab", "jump", "attack", "special", "taunt",
};

/* The inputs' names in the file, by PAD_FIGHT_* index. */
static const char* const kGamepadInputs[PAD_FIGHT_PAD_COUNT] = {
    "left_stick", "right_stick", "left_stick_click", "right_stick_click", "left_trigger", "right_trigger",
    "south",      "east",        "west",             "north",             "left_bumper",  "right_bumper",
    "dpad_up",    "dpad_down",   "dpad_left",        "dpad_right",
};
static const char* const kTouchInputs[PAD_FIGHT_TOUCH_COUNT] = {
    "left_stick", "right_stick", "left_stick_click", "right_stick_click", "left_trigger", "right_trigger",
    "a",          "b",           "x",                "y",                 "left_grip",    "right_grip",
};
static const char* const kDeviceNames[PAD_FIGHT_DEVICE_COUNT] = {"gamepad", "touch"};

/* What the usual GameCube mapping does in a fight. */
static const PcFightAction kGamepadDefaults[PAD_FIGHT_PAD_COUNT] = {
    PC_FIGHT_MOVE,   PC_FIGHT_SMASH,   PC_FIGHT_NONE,   PC_FIGHT_NONE,  PC_FIGHT_SHIELD, PC_FIGHT_SHIELD,
    PC_FIGHT_ATTACK, PC_FIGHT_SPECIAL, PC_FIGHT_JUMP,   PC_FIGHT_JUMP,  PC_FIGHT_NONE,   PC_FIGHT_GRAB,
    PC_FIGHT_TAUNT,  PC_FIGHT_NONE,    PC_FIGHT_NONE,   PC_FIGHT_NONE,
};
static const PcFightAction kTouchDefaults[PAD_FIGHT_TOUCH_COUNT] = {
    PC_FIGHT_MOVE,   PC_FIGHT_SMASH,   PC_FIGHT_NONE, PC_FIGHT_NONE, PC_FIGHT_SHIELD, PC_FIGHT_SHIELD,
    PC_FIGHT_ATTACK, PC_FIGHT_SPECIAL, PC_FIGHT_JUMP, PC_FIGHT_JUMP, PC_FIGHT_GRAB,   PC_FIGHT_GRAB,
};

static PcFightAction s_map[PAD_FIGHT_DEVICE_COUNT][PAD_FIGHT_INPUT_MAX];
static char s_path[1024];
static bool s_live;

int pc_fight_map_input_count(PADFightDevice device) {
    return device == PAD_FIGHT_DEVICE_TOUCH ? PAD_FIGHT_TOUCH_COUNT : PAD_FIGHT_PAD_COUNT;
}

bool pc_fight_map_is_stick(int input) {
    return input == PAD_FIGHT_LEFT_STICK || input == PAD_FIGHT_RIGHT_STICK;
}

static const char* const* input_names(PADFightDevice device) {
    return device == PAD_FIGHT_DEVICE_TOUCH ? kTouchInputs : kGamepadInputs;
}

PcFightAction pc_fight_map_default(PADFightDevice device, int input) {
    if (input < 0 || input >= pc_fight_map_input_count(device)) {
        return PC_FIGHT_NONE;
    }
    return device == PAD_FIGHT_DEVICE_TOUCH ? kTouchDefaults[input] : kGamepadDefaults[input];
}

PcFightAction pc_fight_map_get(PADFightDevice device, int input) {
    if (device < 0 || device >= PAD_FIGHT_DEVICE_COUNT || input < 0 || input >= pc_fight_map_input_count(device)) {
        return PC_FIGHT_NONE;
    }
    return s_map[device][input];
}

/* An input's GameCube output for an action. Its default action keeps the
 * input's usual output (Y jumps as Y, the right trigger shields as R), so
 * an untouched mapping reads exactly as the usual one; any other action
 * gives that action's GameCube button. */
static PADFightOutput output_for(PADFightDevice device, int input, PcFightAction action) {
    PADFightOutput o = {0, PAD_FIGHT_STICK_NONE};
    if (action != PC_FIGHT_NONE && action == pc_fight_map_default(device, input)) {
        return PADFightStandardMapping(device)[input];
    }
    switch (action) {
    case PC_FIGHT_MOVE:
        o.stick = PAD_FIGHT_STICK_MAIN;
        break;
    case PC_FIGHT_SMASH:
        o.stick = PAD_FIGHT_STICK_C;
        break;
    case PC_FIGHT_SHIELD:
        o.button = input == PAD_FIGHT_RIGHT_TRIGGER ? PAD_TRIGGER_R : PAD_TRIGGER_L;
        break;
    case PC_FIGHT_GRAB:
        o.button = PAD_TRIGGER_Z;
        break;
    case PC_FIGHT_JUMP:
        o.button = PAD_BUTTON_X;
        break;
    case PC_FIGHT_ATTACK:
        o.button = PAD_BUTTON_A;
        break;
    case PC_FIGHT_SPECIAL:
        o.button = PAD_BUTTON_B;
        break;
    case PC_FIGHT_TAUNT:
        o.button = PAD_BUTTON_UP;
        break;
    default:
        break;
    }
    return o;
}

static void publish(PADFightDevice device) {
    PADFightOutput out[PAD_FIGHT_INPUT_MAX] = {{0}};
    const int n = pc_fight_map_input_count(device);
    for (int i = 0; i < n; i++) {
        out[i] = output_for(device, i, s_map[device][i]);
    }
    PADSetFightMapping(device, out, (u32) n);
}

/* A stick takes only stick actions, the rest only button ones. */
static bool fits(int input, PcFightAction action) {
    if (action == PC_FIGHT_NONE) {
        return true;
    }
    const bool stick_action = action == PC_FIGHT_MOVE || action == PC_FIGHT_SMASH;
    return stick_action == pc_fight_map_is_stick(input);
}

static void save(void) {
    if (s_path[0] == '\0') {
        return;
    }
    char tmp[sizeof s_path + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", s_path);
    FILE* f = fopen(tmp, "w");
    if (f == NULL) {
        pc_log_line("fight controls: can't write %s", tmp);
        return;
    }
    fprintf(f, "# Fight Controls: what each input does in a fight (Options > Fight Controls).\n");
    for (int d = 0; d < PAD_FIGHT_DEVICE_COUNT; d++) {
        const int n = pc_fight_map_input_count((PADFightDevice) d);
        for (int i = 0; i < n; i++) {
            if (s_map[d][i] != pc_fight_map_default((PADFightDevice) d, i)) {
                fprintf(f, "%s.%s %s\n", kDeviceNames[d], input_names((PADFightDevice) d)[i],
                        kActionNames[s_map[d][i]]);
            }
        }
    }
    fclose(f);
    remove(s_path);
    if (rename(tmp, s_path) != 0) {
        pc_log_line("fight controls: can't replace %s", s_path);
    }
}

static void load(void) {
    FILE* f = fopen(s_path, "r");
    if (f == NULL) {
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f) != NULL) {
        char key[96], value[32];
        if (line[0] == '#' || sscanf(line, "%95s %31s", key, value) != 2) {
            continue;
        }
        char* dot = strchr(key, '.');
        if (dot == NULL) {
            continue;
        }
        *dot = '\0';
        for (int d = 0; d < PAD_FIGHT_DEVICE_COUNT; d++) {
            if (strcmp(key, kDeviceNames[d]) != 0) {
                continue;
            }
            const int n = pc_fight_map_input_count((PADFightDevice) d);
            for (int i = 0; i < n; i++) {
                if (strcmp(dot + 1, input_names((PADFightDevice) d)[i]) != 0) {
                    continue;
                }
                for (int a = 0; a < PC_FIGHT_ACTION_COUNT; a++) {
                    if (strcmp(value, kActionNames[a]) == 0 && fits(i, (PcFightAction) a)) {
                        s_map[d][i] = (PcFightAction) a;
                    }
                }
            }
        }
    }
    fclose(f);
}

void pc_fight_map_configure(const char* user_path) {
    snprintf(s_path, sizeof s_path, "%s/fight-controls.txt", user_path != NULL ? user_path : ".");
    for (int d = 0; d < PAD_FIGHT_DEVICE_COUNT; d++) {
        const int n = pc_fight_map_input_count((PADFightDevice) d);
        for (int i = 0; i < n; i++) {
            s_map[d][i] = pc_fight_map_default((PADFightDevice) d, i);
        }
    }
    load();
    for (int d = 0; d < PAD_FIGHT_DEVICE_COUNT; d++) {
        publish((PADFightDevice) d);
    }
}

void pc_fight_map_set(PADFightDevice device, int input, PcFightAction action) {
    if (device < 0 || device >= PAD_FIGHT_DEVICE_COUNT || input < 0 || input >= pc_fight_map_input_count(device) ||
        !fits(input, action))
    {
        return;
    }
    s_map[device][input] = action;
    publish(device);
    save();
}

void pc_fight_map_reset(PADFightDevice device) {
    if (device < 0 || device >= PAD_FIGHT_DEVICE_COUNT) {
        return;
    }
    const int n = pc_fight_map_input_count(device);
    for (int i = 0; i < n; i++) {
        s_map[device][i] = pc_fight_map_default(device, i);
    }
    publish(device);
    save();
}

/* A fight's sim ticking: a match scene (VS, which 1P modes play their
 * fights in too; Sudden Death; Training), not paused by a player or by
 * Slippi's overlay, not held for placing the stage, and not under the F1
 * menu. From game state only, never from what the headset shows, so flat
 * builds and netplay agree. */
static bool fight_running(void) {
    const int kind = scene_kind();
    if (kind != GS_VS && kind != GS_SUDDEN_DEATH && kind != GS_TRAINING) {
        return false;
    }
    if (gm_GetDbPauseFlag(1) || gm_GetDbPauseFlag(2) || gm_vs_soft_paused()) {
        return false;
    }
    return !pc_xr_place_holding() && !pc_menu_is_open();
}

void pc_fight_map_frame(void) {
    const bool live = fight_running();
    if (live != s_live) {
        s_live = live;
        pc_log_line("fight controls: %s", live ? "on (a fight runs)" : "off");
    }
    PADSetFightMappingActive(live);
}

bool pc_fight_map_live(void) {
    return s_live;
}
