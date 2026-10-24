/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * VI layer. aurora provides the window/framebuffer (VIInit/VIConfigure/VIFlush);
 * retrace timing and the XFB flip are emulated here on top of aurora's frame
 * loop. Melee waits in HSD_VIWaitXFBFlush -> VIWaitForRetrace once per frame,
 * so VIWaitForRetrace is the frame boundary.
 */
#ifdef AURORA_ENABLE_OPENXR
#include <aurora/xr.h>
#endif
#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include <SDL3/SDL_timer.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
extern void browser_yield(void);
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pc/pc.h"
#include "pc/fight_map.h"
#include "pc/fight_menu.h"
#include "pc/slippi_menu.h"
#include "pc/launcher.h"
#include "pc/touch.h"
#include "pc/xr_scene.h"
#include "pc/widescreen.h"
#include "pc/net.h"
#include "pc/slp.h"
#include "pc/net_chat.h"
#include <melee/if/ifnetchat.h>
#include <dolphin/pad.h>
#include <melee/lb/lb_0195.h>
#include "pc/net_lan.h"
#include "pc/net_match.h"
#include "pc/net_rank_session.h"

bool pc_exit_requested;

static u32 s_retrace_count;
static VIRetraceCallback s_pre_cb;
static VIRetraceCallback s_post_cb;
static void* s_next_fb;
static void* s_current_fb;
static BOOL s_black;
static bool s_in_frame;

void pc_os_run_alarms(void);
void aurora_heap_check(void);

uint32_t pc_gfx_prewarm(uint32_t max_wait_ms) {
    return aurora_wait_pipelines(max_wait_ms);
}

uint64_t pc_monotonic_ns(void) {
    return SDL_GetTicksNS();
}

#ifdef __EMSCRIPTEN__
/* A precise native sleep would block the browser event loop; the frame
 * boundary is also the only place the page gets control back. */
#define PC_PACE_ALWAYS 1
static bool s_pace_waited;
static void pc_pace_wait(u64 ns) {
    s_pace_waited = true;
    if (ns >= 1000000ull)
        emscripten_sleep((unsigned)(ns / 1000000ull));
    else
        browser_yield();
}
#else
#define PC_PACE_ALWAYS 0
#define pc_pace_wait SDL_DelayPrecise
#endif

static int cmp_float(const void* a, const void* b) {
    const float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}

/* Every 600 frames: how long each frame waited for its pad sample (the pad
 * alarm runs on its own 60 Hz grid; in XR the frame boundary follows the
 * headset's), as median/95%/max, and how many frames waited over 2 ms. */
static void log_pad_wait(void) {
    static float ms[600];
    static int n;
    ms[n++] = (float)pc_os_take_alarm_wait_ns() / 1e6f;
    if (n < 600)
        return;
    int over2 = 0;
    for (int i = 0; i < n; i++)
        over2 += ms[i] > 2.f;
    qsort(ms, (size_t)n, sizeof ms[0], cmp_float);
    pc_log_line("pad wait per frame (ms, median/95%%/max): %.1f/%.1f/%.1f, %d of %d over 2 ms", ms[n / 2],
        ms[n * 95 / 100], ms[n - 1], over2, n);
    n = 0;
}

#ifdef AURORA_ENABLE_OPENXR
/* Netplay in XR (pc_frame_boundary): the headset tick the next boundary is
 * due at (0: not paced so), and time sync's lengthening not yet paid as a
 * whole tick (ns). */
static u64 s_xr_due;
static u64 s_xr_carry;
#endif

static bool unpaced_playback(void) {
    static int on = -1;
    if (on < 0) {
        on = pc_slp_play_active() && getenv("MELEE_SLP_PLAY_PACED") == NULL;
    }
    return on > 0;
}

void pc_frame_boundary(void) {
    log_pad_wait();
    static int timing_debug = -1;
    static int fps_log = -1;
    static u64 fps_t0;
    static u32 fps_n;
    /* An average hides stutter: one 60ms hitch a second barely moves it.
     * Track the outliers instead, plus how far the pacing sleep overshoots
     * its request -- on Windows a coarse timer resolution turns a 5ms wait
     * into ~16ms, which is stutter the frame average never shows. */
    static u64 frame_prev_ns;
    static u64 frame_worst_ns;
    static u32 frame_late_20;
    static u32 frame_late_33;
    static u64 sleep_worst_over_ns;

    if (timing_debug < 0)
        timing_debug = getenv("MELEE_NET_DEBUG") != NULL;
    if (s_in_frame) {
        u64 started = timing_debug ? SDL_GetTicksNS() : 0;
        aurora_end_frame();
        if (timing_debug) {
            u64 elapsed = SDL_GetTicksNS() - started;
            if (elapsed > 20000000ull)
                pc_log_line("net timing: aurora_end_frame %.1f ms at retrace %u frame %d",
                    elapsed / 1e6, s_retrace_count, pc_net_frame());
        }
        s_in_frame = false;
    }
    /* Session messages are host-side work, never rollback simulation. The
     * matcher owns them until both peers cross its READY barrier. */
    if (pc_net_match_state(NULL) == PC_MATCH_READY)
        pc_rank_session_poll();
    /* A Slippi link outside a match (Direct's lobby: the stage select, the
     * splash) is serviced every frame, whatever scene is up, so the peer
     * never times it out. A no-op with no link. */
    pc_net_slippi_link_pump();
#ifdef __EMSCRIPTEN__
    extern void pc_audio_pump(void);
    pc_audio_pump();
#endif
    aurora_heap_check();    /* no-op unless MELEE_HEAP_CHECK is set */
    pc_widescreen_update(); /* Auto mode follows window resizes. */
    /* MELEE_LAN_TEST=1|host: the LAN lobby without the menu; "host" starts
     * a match with the first peer found. MELEE_LAN_DIRECT=ip:port: the same
     * with a known peer, no discovery (src/pc/net_lan.c). */
    static int lan_test = -1;
    static u32 lan_frames;
    if (lan_test < 0) {
        const char* t = getenv("MELEE_LAN_TEST");
        lan_test = getenv("MELEE_LAN_DIRECT") != NULL ? 3 :
                   t == NULL                          ? 0 :
                   strcmp(t, "host") == 0             ? 2 :
                                                        1;
    }
    if (lan_test) {
        pc_lan_poll();
        /* Both fixtures start only once the game has its rules loaded (the
         * title screen), not at frame 0: RULES would carry zeros. */
        lan_frames++;
        if (lan_test == 2 && lan_frames >= 300 && pc_lan_state(NULL) == 0) {
            pc_lan_start_match();
        }
        if (lan_test == 3 && lan_frames == 300) {
            const char* d = getenv("MELEE_LAN_DIRECT");
            char host[64];
            const char* colon = strrchr(d, ':');
            if (colon != NULL && (size_t)(colon - d) < sizeof host) {
                memcpy(host, d, (size_t)(colon - d));
                host[colon - d] = '\0';
                pc_lan_connect_direct(host, (uint16_t)atoi(colon + 1));
            } else {
                pc_log_line("lan: MELEE_LAN_DIRECT must be ip:port");
            }
        }
    }
    if (fps_log < 0) {
        fps_log = getenv("MELEE_FPS") != NULL;
        fps_t0 = SDL_GetTicks();
    }
    if (fps_log) {
        u64 now = SDL_GetTicks();
        u64 now_ns = SDL_GetTicksNS();
        fps_n++;
        if (frame_prev_ns != 0) {
            u64 delta = now_ns - frame_prev_ns;
            if (delta > frame_worst_ns) {
                frame_worst_ns = delta;
            }
            if (delta > 20000000ull) {
                frame_late_20++;
            }
            if (delta > 33000000ull) {
                frame_late_33++;
            }
            /* Put the stall in the main log too, where it sits next to
             * whatever aurora reported loading at that moment. An average
             * cannot tell a shader compile from a disc read; a timestamped
             * marker beside the surrounding records can. */
            if (delta > 50000000ull) {
                pc_log_line("STALL %.1fms at frame %u", delta / 1e6, s_retrace_count);
            }
        }
        frame_prev_ns = now_ns;
        if (now - fps_t0 >= 1000) {
            /* Through pc_log_line, not stderr: on Android stderr goes
             * nowhere, so MELEE_FPS printed nothing there. pc_log_line also
             * reaches logcat and MELEE_LOG_FILE. */
            pc_log_line("fps %.1f worst %.1fms late>20ms %u late>33ms %u "
                        "sleep_overshoot %.1fms",
                fps_n * 1000.0 / (double)(now - fps_t0), frame_worst_ns / 1e6, frame_late_20,
                frame_late_33, sleep_worst_over_ns / 1e6);
            fps_t0 = now;
            fps_n = 0;
            frame_worst_ns = 0;
            frame_late_20 = 0;
            frame_late_33 = 0;
            sleep_worst_over_ns = 0;
        }
    }

    u64 update_started = timing_debug ? SDL_GetTicksNS() : 0;
    const AuroraEvent* event = aurora_update();
    if (timing_debug) {
        u64 elapsed = SDL_GetTicksNS() - update_started;
        if (elapsed > 50000000ull)
            pc_log_line("net timing: aurora_update %.1f ms at retrace %u frame %d", elapsed / 1e6,
                s_retrace_count, pc_net_frame());
    }
    while (event != NULL && event->type != AURORA_NONE) {
        if (event->type == AURORA_EXIT) {
            pc_exit_requested = true;
        } else if (event->type == AURORA_CONTROLLER_ADDED) {
            pc_xr_gamepad_added(event->controller);
        } else if (event->type == AURORA_SDL_EVENT) {
            if (event->sdl.type == SDL_EVENT_KEY_DOWN &&
                event->sdl.key.scancode == SDL_SCANCODE_F1 && !event->sdl.key.repeat)
                pc_menu_toggle();
            /* In XR the gamepad's Select does nothing, rather than open the
             * port menu over the game. (It switched to full VR; the first
             * release is mixed reality only, VR waits for its own polish:
             * MELEE_XR_MODE=vr and MELEE_XR_CONTROL still reach it.) */
            const bool xr_select = event->sdl.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
                                   event->sdl.gbutton.button == SDL_GAMEPAD_BUTTON_BACK &&
                                   pc_xr_presenting();
            if (!xr_select)
                pc_menu_event(&event->sdl);
            pc_keyboard_event(&event->sdl);
            pc_touch_event(&event->sdl);
        }
        ++event;
    }
    pc_xr_poll_control();
    pc_menu_update();
    pc_fight_menu_update();
    pc_slippi_menu_update();
    /* Nothing draws while the overlay pauses the game, so hold the last
     * frame instead of clearing the EFB to black underneath the menu. */
    aurora_preserve_frame_buffer(pc_menu_is_open());
    /* Before the pads are read: a fight reads them through Fight Controls. */
    pc_fight_map_frame();
    pc_keyboard_apply();
    /* MELEE_EXIT_AFTER_FRAMES=<n>: bound a scripted run without needing
     * synthetic input, which is unreliable under Xwayland. Setting
     * pc_exit_requested instead of exiting here on purpose: the window-close
     * path is the one that runs atexit(pc_shutdown_once), and skipping it is
     * what makes Dawn's static destructors race the live device. */
    static int exit_after = -1;
    if (exit_after < 0) {
        const char* n = getenv("MELEE_EXIT_AFTER_FRAMES");
        exit_after = n != NULL ? atoi(n) : 0;
    }
    if (exit_after > 0 && s_retrace_count >= (u32)exit_after && !pc_exit_requested) {
        pc_log_line("MELEE_EXIT_AFTER_FRAMES: reached frame %u, exiting", s_retrace_count);
        pc_exit_requested = true;
    }
    if (pc_exit_requested) {
        pc_net_match_stop();
        pc_net_disconnect();
        pc_lan_stop();
        exit(0);
    }

#ifdef __EMSCRIPTEN__
    s_pace_waited = false;
#endif
    /* Enforce deterministic 60 Hz simulation pacing regardless of display refresh rate
     * (e.g. 120 Hz, 144 Hz, 240 Hz high-refresh monitors). When VSync is enabled on high-refresh
     * displays, aurora_begin_frame() unblocks at monitor refresh rate. Without this check,
     * the simulation would run at 2x-4x speed. Pacing strictly to 60.000 Hz ensures physics,
     * hitboxes, and timers remain bit-identical. */
    static u64 next_sim_ns;
    const u64 sim_period = pc_sim_period_ns();
    u64 now = SDL_GetTicksNS();
    /* How far behind its schedule this boundary is, and how much of that to
     * run off by skipping the sleep below. Offline a large hitch is dropped:
     * there is nothing to stay in step with. In netplay dropping it left the
     * peer that froze behind the other one by the whole freeze, which time
     * sync then took seconds to close (pc_net_catch_up_ns). */
    u64 late = next_sim_ns != 0 && now > next_sim_ns ? now - next_sim_ns : 0;
#ifdef AURORA_ENABLE_OPENXR
    /* Netplay in XR keeps its schedule in the headset's ticks instead
     * (s_xr_due, below): how far behind it is is counted in those. */
    const u64 xr_now = pc_net_active() ? aurora_xr_pace_count() : 0;
    if (xr_now == 0) {
        s_xr_due = 0;
    } else {
        late = s_xr_due != 0 && xr_now > s_xr_due ? (xr_now - s_xr_due) * sim_period : 0;
    }
#endif
    late = pc_net_active() ? pc_net_catch_up_ns(late) : late > sim_period * 2 ? 0 : late;
#ifdef AURORA_ENABLE_OPENXR
    /* XR build: when the headset runs at a multiple of 60 Hz, take the frame
     * cadence from it, so each game frame is shown for the same number of
     * display frames (a free-running timer drifts against the display and
     * repeats frames unevenly). Offline every boundary waits for the next
     * tick. Netplay too, but by a schedule of whole ticks that time sync
     * moves: a boundary is due at tick s_xr_due, a frame later each
     * boundary, later still by what time sync asks (its nudges, which can't
     * lengthen one display frame, are added up into whole ticks), and as
     * far behind the headset as the frames a freeze left owing, which then
     * run back to back (pc_net_catch_up_ns). On its own clock, netplay's
     * frames drifted against the display and carried the tick the menus
     * last paced to, which SpaceWarp read as a gap before every frame. */
    if (xr_now != 0) {
        if (s_xr_due == 0 || xr_now >= s_xr_due) {
            s_xr_due = xr_now - late / sim_period; /* due, or owing `late` */
        }
        if (aurora_xr_pace_until(s_xr_due)) {
            next_sim_ns = SDL_GetTicksNS();
        } else {
            s_xr_due = 0;      /* the headset stopped pacing: the timer below */
            next_sim_ns = now; /* from now, not from the frames paced */
        }
    } else if (!pc_net_active() && aurora_xr_pace()) {
        next_sim_ns = SDL_GetTicksNS();
    } else
#endif
    /* A Slippi replay played back (src/pc/slp_play.c) runs as fast as it
     * renders, unless MELEE_SLP_PLAY_PACED=1 (to watch it). */
    if (unpaced_playback()) {
        next_sim_ns = now;
    } else if (next_sim_ns == 0 || now > next_sim_ns) {
        next_sim_ns = now - late;
    } else if (now < next_sim_ns) {
        const u64 want = next_sim_ns - now;
        /* On standard 60 Hz VSync, aurora_begin_frame already waited for VBlank. On high-refresh
         * (120/144/240 Hz) or VSync-off, this throttles simulation to exact 60 Hz. */
        if (PC_PACE_ALWAYS || !aurora_vsync_enabled() || want > 2000000ull) {
            u64 started = (fps_log || timing_debug) ? SDL_GetTicksNS() : 0;
            pc_pace_wait(want);
            if (fps_log || timing_debug) {
                u64 slept = SDL_GetTicksNS() - started;
                if (fps_log && slept > want && slept - want > sleep_worst_over_ns) {
                    sleep_worst_over_ns = slept - want;
                }
                if (timing_debug && slept > 20000000ull)
                    pc_log_line(
                        "net timing: pacing sleep %.1f ms (asked %.1f) at retrace %u frame %d",
                        slept / 1e6, want / 1e6, s_retrace_count, pc_net_frame());
            }
        }
    }
    next_sim_ns += sim_period;
#ifdef __EMSCRIPTEN__
    if (!s_pace_waited)
        browser_yield(); /* every frame returns to the event loop at least once */
#endif

    /* aurora_begin_frame returns false while minimized/paused; keep pumping.
     * Sleep a frame between attempts: without it a minimized window spins a
     * core at 100% polling SDL. */
    for (;;) {
        u64 started = timing_debug ? SDL_GetTicksNS() : 0;
        bool begun = aurora_begin_frame();
        if (timing_debug) {
            u64 elapsed = SDL_GetTicksNS() - started;
            if (elapsed > 20000000ull)
                pc_log_line("net timing: aurora_begin_frame %.1f ms at retrace %u frame %d",
                    elapsed / 1e6, s_retrace_count, pc_net_frame());
        }
        if (begun)
            break;
        update_started = timing_debug ? SDL_GetTicksNS() : 0;
        event = aurora_update();
        if (timing_debug) {
            u64 elapsed = SDL_GetTicksNS() - update_started;
            if (elapsed > 50000000ull)
                pc_log_line("net timing: aurora_update %.1f ms at retrace %u frame %d",
                    elapsed / 1e6, s_retrace_count, pc_net_frame());
        }
        while (event != NULL && event->type != AURORA_NONE) {
            if (event->type == AURORA_EXIT) {
                pc_net_match_stop();
                pc_net_disconnect();
                pc_lan_stop();
                exit(0);
            }
            ++event;
        }
#ifdef __EMSCRIPTEN__
        emscripten_sleep(16);
#else
        SDL_Delay(16);
#endif
    }
    s_in_frame = true;

    /* Presentation-only chat reads the physical local controller, never a
     * rewound/synchronized pad. It cannot change the game simulation. */
    bool chat_eligible = pc_net_chat_available();
    PADStatus chat_pads[4] = {0};
    if (chat_eligible)
        PADRead(chat_pads);
    pc_net_chat_poll(chat_pads[0].button, chat_eligible, SDL_GetTicks());
    ifNetChat_Update(chat_eligible);

    s_retrace_count++;
#ifdef __EMSCRIPTEN__
    // clang-format off
    EM_ASM({if(Module.onFrame)Module.onFrame($0);},s_retrace_count);
    // clang-format on
#endif
    /* Age of the 1000 Hz sample the sim is about to consume, before the pad
     * alarms (fn_800195FC -> PADRead) fire from pc_os_run_alarms. */
    pc_input_latency_record();
    /* Offline, every boundary hands the frame its pad sample (lb_0195.c):
     * the boundary follows the headset's tick or a monitor's VBlank, or a
     * timer a hitch has re-anchored, and the pad alarm's own 60 Hz grid
     * drifts or stays offset against any of them, which every frame then
     * slept through. Netplay pins the queue to one sample per boundary
     * itself (pad_queue_pin, net_sync.c). MELEE_PAD_ALARM_FREE=1 keeps the
     * alarm's own grid, for comparison. */
    static int pad_alarm_free = -1;
    if (pad_alarm_free < 0)
        pad_alarm_free = getenv("MELEE_PAD_ALARM_FREE") != NULL;
    if (!pc_net_active() && !pad_alarm_free) {
        lb_pc_pad_alarm_to_frame();
    }
    u64 alarms_started = timing_debug ? SDL_GetTicksNS() : 0;
    pc_os_run_alarms();
    if (timing_debug) {
        u64 elapsed = SDL_GetTicksNS() - alarms_started;
        if (elapsed > 50000000ull)
            pc_log_line("net timing: pc_os_run_alarms %.1f ms at retrace %u frame %d",
                elapsed / 1e6, s_retrace_count, pc_net_frame());
    }
    /* Time sync spreads its correction here: a per-frame lengthening of the
     * next wait, plus a whole frame when a gap is too big to nudge away. */
    const u64 adjust = pc_net_pace_adjust_ns();
    next_sim_ns += adjust;
#ifdef AURORA_ENABLE_OPENXR
    if (s_xr_due != 0) { /* the next boundary: a tick on, and any whole ticks time sync's asked for */
        s_xr_carry += adjust;
        s_xr_due += 1 + s_xr_carry / sim_period;
        s_xr_carry %= sim_period;
    } else {
        s_xr_carry = 0;
    }
#endif
    if (s_pre_cb) {
        u64 started = timing_debug ? SDL_GetTicksNS() : 0;
        s_pre_cb(s_retrace_count);
        if (timing_debug) {
            u64 elapsed = SDL_GetTicksNS() - started;
            if (elapsed > 50000000ull)
                pc_log_line("net timing: pre-retrace callback %.1f ms at retrace %u frame %d",
                    elapsed / 1e6, s_retrace_count, pc_net_frame());
        }
    }
    s_current_fb = s_next_fb;
    if (s_post_cb) {
        u64 started = timing_debug ? SDL_GetTicksNS() : 0;
        s_post_cb(s_retrace_count);
        if (timing_debug) {
            u64 elapsed = SDL_GetTicksNS() - started;
            if (elapsed > 50000000ull)
                pc_log_line("net timing: post-retrace callback %.1f ms at retrace %u frame %d",
                    elapsed / 1e6, s_retrace_count, pc_net_frame());
        }
    }
}

void VIWaitForRetrace(void) {
    pc_frame_boundary();
    /* The overlay pauses the game. Melee's whole simulation hangs off this
     * call returning, so keep presenting frames and pumping input here and
     * simply do not hand one back until the menu closes. */
    while (pc_menu_is_open() && !pc_exit_requested) {
        pc_frame_boundary();
    }
}

u32 VIGetRetraceCount(void) {
    return s_retrace_count;
}

u64 pc_sim_period_ns(void) {
    return 1000000000ull / 60;
}

u32 VIGetNextField(void) {
    return s_retrace_count & 1;
}

u32 VIGetDTVStatus(void) {
    return 0;
}

void* VIGetCurrentFrameBuffer(void) {
    return s_current_fb;
}

void* VIGetNextFrameBuffer(void) {
    return s_next_fb;
}

void VISetNextFrameBuffer(void* fb) {
    s_next_fb = fb;
}

void VISetBlack(BOOL black) {
    s_black = black;
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = s_pre_cb;
    s_pre_cb = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    VIRetraceCallback old = s_post_cb;
    s_post_cb = cb;
    return old;
}

u16 VIPadFrameBufferWidth(u16 width) {
    return (u16)((width + 15) & ~15);
}
