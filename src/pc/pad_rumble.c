/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Rumble to the hardware the player is holding.
 *
 * Port 1 is several devices at once: the keyboard, the touchscreen overlay,
 * the headset's Touch controllers, a GameCube adapter's first slot and the
 * SDL gamepad aurora gives port 1 all merge into one pad (keyboard.c, then
 * aurora's PADRead). Only the one being played with should shake, so each
 * frame keyboard.c reports which of them show input, and the one that most
 * recently started to becomes port 1's rumble device. A motor that is on
 * when the player picks up another device moves over to it.
 *
 * Online, the local player's pad is read from port 1 whatever port they play
 * as, so their port's motor goes to port 1's hardware and the other players'
 * motors go nowhere.
 */
#include "pc/pc.h"
#include "pc/net.h"

#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_properties.h>
#include <dolphin/pad.h>
#include <stdatomic.h>
#include <stdlib.h>

/* Port 1's motor command (PAD_MOTOR_*) and the device that has it. */
static _Atomic unsigned s_command0 = PAD_MOTOR_STOP_HARD;
static _Atomic int s_device0 = PC_PAD_DEVICE_NONE;
static bool s_trace;

static const char* device_name(int device) {
    static const char* const names[] = {"none", "keys", "gamepad", "xr", "gc adapter"};
    return device >= 0 && device < (int)(sizeof names / sizeof names[0]) ? names[device] : "?";
}

/* Port 1's motor on one device. The keyboard and touchscreen have no motor of
 * their own; with no gamepad on port 1, aurora plays PADControlMotor on the
 * phone's vibrator where there is one. */
static void motor0(int device, unsigned command) {
    switch (device) {
    case PC_PAD_DEVICE_GAMEPAD:
        PADControlMotor(0, command);
        break;
    case PC_PAD_DEVICE_XR:
#ifdef AURORA_ENABLE_OPENXR
        aurora_xr_set_rumble(command == PAD_MOTOR_RUMBLE);
#endif
        break;
    case PC_PAD_DEVICE_GCADAPTER:
        pc_gcadapter_motor(0, command);
        break;
    case PC_PAD_DEVICE_KEYS:
    case PC_PAD_DEVICE_NONE:
        if (SDL_GetGamepadFromPlayerIndex(0) == NULL)
            PADControlMotor(0, command);
        break;
    }
}

/* Whether port 1's SDL gamepad shows input: a button, or a stick or trigger
 * well off rest. */
static bool gamepad_engaged(void) {
    SDL_Gamepad* pad = SDL_GetGamepadFromPlayerIndex(0);
    if (pad == NULL)
        return false;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
        if (SDL_GetGamepadButton(pad, (SDL_GamepadButton)b))
            return true;
    }
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) {
        if (abs(SDL_GetGamepadAxis(pad, (SDL_GamepadAxis)a)) > 12000)
            return true;
    }
    return false;
}

bool pc_pad_engaged(const struct PADStatus* st) {
    return st->button != 0 || abs(st->stickX) > 40 || abs(st->stickY) > 40 || abs(st->substickX) > 40 ||
           abs(st->substickY) > 40 || st->triggerLeft > 80 || st->triggerRight > 80;
}

void pc_pad_rumble_engaged(unsigned engaged) {
    static unsigned s_prev;
    static bool s_init;
    if (!s_init) {
        s_init = true;
        s_trace = getenv("MELEE_INPUT_TRACE") != NULL;
    }
    if (gamepad_engaged())
        engaged |= 1u << PC_PAD_DEVICE_GAMEPAD;
    const unsigned started = engaged & ~s_prev;
    s_prev = engaged;
    if (started == 0)
        return;
    const int current = atomic_load_explicit(&s_device0, memory_order_relaxed);
    if (started & (1u << current))
        return;
    int device = PC_PAD_DEVICE_NONE;
    for (int d = PC_PAD_DEVICE_KEYS; d <= PC_PAD_DEVICE_GCADAPTER; d++) {
        if (started & (1u << d))
            device = d;
    }
    atomic_store_explicit(&s_device0, device, memory_order_relaxed);
    const unsigned command = atomic_load_explicit(&s_command0, memory_order_relaxed);
    if (command == PAD_MOTOR_RUMBLE) {
        motor0(current, PAD_MOTOR_STOP);
        motor0(device, PAD_MOTOR_RUMBLE);
    }
    SDL_Gamepad* pad = device == PC_PAD_DEVICE_GAMEPAD ? SDL_GetGamepadFromPlayerIndex(0) : NULL;
    if (pad != NULL) {
        /* A Bluetooth pad on a generic HID driver (Android) has no motor to
         * drive: say so, rather than leave rumble silently missing. */
        const bool motor =
            SDL_GetBooleanProperty(SDL_GetGamepadProperties(pad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
        pc_log_line("rumble: port 1 device %s -> gamepad '%s' (%s)", device_name(current), SDL_GetGamepadName(pad),
            motor ? "rumbles" : "no rumble reported");
    } else {
        pc_log_line("rumble: port 1 device %s -> %s", device_name(current), device_name(device));
    }
}

int pc_pad_active_device(void) {
    return atomic_load_explicit(&s_device0, memory_order_relaxed);
}

void pc_pad_rumble_command(unsigned port, unsigned command) {
    if (pc_net_active()) {
        if ((int)port != pc_net_local_player())
            return;
        port = 0;
    }
    if (port != 0) {
        PADControlMotor(port, command);
        pc_gcadapter_motor((int)port, command);
        return;
    }
    atomic_store_explicit(&s_command0, command, memory_order_relaxed);
    const int device = atomic_load_explicit(&s_device0, memory_order_relaxed);
    motor0(device, command);
    if (s_trace)
        pc_log_line("rumble: port 1 %s on %s", command == PAD_MOTOR_RUMBLE ? "on" : "off", device_name(device));
}
