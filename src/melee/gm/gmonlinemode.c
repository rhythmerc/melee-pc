#include "gmonlinemode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <melee/lb/forward.h>

#include "forward.h"
#include "gm_1601.h"
#include "gm_1A36.h"
#include "gm_1A3F.h"
#include "gm_unsplit.h"
#include "gmresult.h"
#include "gmscene.h"
#include "gmvsmode.h"
#include "gmvsmelee.h"
#include "types.h"
#include <dolphin/pad.h>
#include <melee/if/if_2FD9.h>
#include <melee/lb/lbarchive.h>
#include <melee/lb/lblanguage.h>
#include <melee/lb/lbaudio_ax.h>
#include <melee/mn/mnmain.h>
#include <melee/mn/mnnamenew.h>
#include <melee/mn/mnslippi.h>
#include <melee/lb/lbdvd.h>
#include <melee/lb/types.h>
#include <melee/mn/inlines.h>
#include <melee/mn/types.h>
#include <sysdolphin/baselib/controller.h>
#include <sysdolphin/baselib/random.h>
#ifdef TARGET_PC
#include <SDL3/SDL.h>
#include "pc/net.h"
#include "pc/net_lan.h"
#include "pc/net_identity.h"
#include "pc/net_match.h"
#include "pc/slippi/slp_user.h"
#include "pc/net_rank_session.h"
#include "pc/xr_pointer.h"
#include "pc/xr_place.h"
extern bool pc_net_slippi_active(void);
extern const char* pc_get_net_target(void);
extern int gm_vs_slippi_quitter;
#include <melee/mn/mnstagesel.h>
extern void pc_set_net_target(const char* code);
#include "pc/pc.h"
#endif

/* GM_ONLINE: lobby -> CSS -> SSS -> VS -> (sudden death) -> results -> CSS,
 * the vanilla VS flow (gmvsmode.c) on a private VsModeData that both peers
 * reset identically on entering the lobby. The lobby is the Double Dash LAN
 * counter screen (docs/netcode-plan.md §8): pc_lan_* announces us, counts
 * peers, and the first Start elects the host; once pc_lan_state() reports
 * the match (2) both peers leave for the CSS on the same synced frame. From
 * there every scene runs on synced inputs: the local player is P1 when
 * hosting and P2 as guest, ports 3/4 report no controller, and the RULES
 * handshake (net.c) has made GameRules/GamePrefs/unlock/frozen-stadium
 * identical on both sides. B on the CSS goes back to the lobby. */

enum {
    state_lobby = 0,
    state_css = 1,
    state_sss = 2,
    state_vs = 3,
    state_sudden_death = 4,
    state_results = 5, /* last: gmVsMelee_ExitResults skips challengers */
    state_intro = 6,   /* the VS splash: the Classic stage intro, between the stage and the fight */
    state_place = 7,   /* XR: the stage-placement dummy (ONLINE_KIND_PLACE) */
};

#ifdef TARGET_PC
/* Why the session ended, for the CSS to show: Slippi's flow has no lobby to
 * return to, so a failure lands on the CSS with the reason on it. */
static char slpcss_notice[48];
static void routeAfterFailure(void);
#endif
static bool awaiting_rank_result;
static unsigned lobby_frame_n;
static void onEnterLobby(GameModeState*);
static void onExitLobby(GameModeState*);
static void onEnterCss(GameModeState*);
static void onExitCss(GameModeState*);
static void onEnterSss(GameModeState*);
static void onExitSss(GameModeState*);
static void onEnterVs(GameModeState*);
static void onExitVs(GameModeState*);
static void onEnterSuddenDeath(GameModeState*);
static void onExitSuddenDeath(GameModeState*);
static void onEnterResults(GameModeState*);
static void onExitResults(GameModeState*);
static void onEnterPlace(GameModeState*);
static void onExitPlace(GameModeState*);
static void onEnterIntro(GameModeState*);
static void onExitIntro(GameModeState*);

/* The Classic stage intro's enter data (gmclassic.c's gmClassicIntroData): the
 * allies on one side, the enemies on the other, each with a costume. Slippi's
 * VS splash is this scene with the two players in it. */
typedef struct OnlineIntroData {
    s32 kind;          /* 0: plain models */
    s32 sub;
    u8 slot, nametag, stage_no, ally_n, enemy_n;
    u8 ally[3], enemy[3], ally_color[3], enemy_color[3], ally_flag[3], enemy_flag[3];
    u8 pad;
} OnlineIntroData;
#ifdef TARGET_PC
extern bool gm_vs_splash_online;
#endif
static OnlineIntroData online_intro_data;
static u8 online_intro_exit[0x20];

GameModeState gm_Mode_Online_States[] = {
    {
        state_lobby,
        lbDvdPreload_3,
        0,
        onEnterLobby,
        onExitLobby,
        {
            GS_ONLINE_LOBBY,
            NULL,
            NULL,
        },
    },
    {
        state_css,
        lbDvdPreload_3,
        0,
        onEnterCss,
        onExitCss,
        {
            GS_CSS,
            &gmVsMelee_CssData,
            &gmVsMelee_CssData,
        },
    },
    {
        state_sss,
        lbDvdPreload_3,
        0,
        onEnterSss,
        onExitSss,
        {
            GS_SSS,
            &gmVsMelee_SssData,
            &gmVsMelee_SssData,
        },
    },
    {
        state_vs,
        lbDvdPreload_3,
        0,
        onEnterVs,
        onExitVs,
        {
            GS_VS,
            &gmVsMelee_StartData,
            &gmVsMelee_VsExitInfo,
        },
    },
    {
        state_sudden_death,
        lbDvdPreload_3,
        0,
        onEnterSuddenDeath,
        onExitSuddenDeath,
        {
            GS_SUDDEN_DEATH,
            &gmVsMelee_StartData,
            &gmVsMelee_SuddenDeathExitInfo,
        },
    },
    {
        state_intro,
        lbDvdPreload_3,
        0,
        onEnterIntro,
        onExitIntro,
        {
            GS_INTRO_EASY,
            &online_intro_data,
            &online_intro_exit,
        },
    },
    {
        state_place,
        lbDvdPreload_2,
        0,
        onEnterPlace,
        onExitPlace,
        {
            GS_VS,
            &gmVsMelee_StartData,
            &gmVsMelee_VsExitInfo,
        },
    },
    {
        state_results,
        lbDvdPreload_3,
        0,
        onEnterResults,
        onExitResults,
        {
            GS_RESULTS,
            &gmVsMelee_ResultsEnterData,
            NULL,
        },
    },
    { GM_GAMEMODESTATE_TERMINATE },
};

static OnlineKind online_kind;
static VsModeData online_vs;

#ifdef TARGET_PC
/* Slippi's order (docs/slippi-online-flow-plan.md): the CSS comes first, the
 * character is picked, and only then does the search start. So for Unranked
 * and Direct with a Slippi account, the lobby scene is a pass-through to the
 * CSS the first time, the CSS leads back to the lobby once the pick is made,
 * and the lobby searches. A matched lobby goes straight to the VS scene (the
 * selections have already been exchanged, there is nothing left to choose). */
static bool slippi_picked;
enum { SLPCSS_SELECT, SLPCSS_SEARCH, SLPCSS_LOBBY };
static int slpcss_state;
static bool slpcss_leave; /* the match is agreed, the next exit is for it */
/* Slippi's Direct lobby (HandleInputsOnCSS.asm, main.asm VSSceneDecide):
 * after a game both players are back on the CSS, still connected. The loser
 * of the last game presses START for the stage select and the stage picked
 * goes with the lock-in; the winner locks in with no stage and plays the
 * loser's. A draw makes both "losers", and the lower port's pick wins. */
enum { DIRECT_FIRST = -1, DIRECT_LOST, DIRECT_WON };
static int direct_result = DIRECT_FIRST;
static bool direct_chose_stage; /* the loser has picked this game's stage */
static bool direct_to_sss;      /* the CSS leaves for the stage select */
static bool direct_picking;     /* that stage select is up */
static int direct_z_hold;       /* frames Z has been held: hold to disconnect */
#define DIRECT_DISCONNECT_HOLD 0x30 /* Slippi's DISCONNECT_HOLD_DELAY */
static void directLobbyEnd(void);
/* The CSS opens with your last pick already placed (the vanilla CSS puts the
 * token on whatever character the door starts with), so a rematch is START.
 * MELEE_CSS_PICK=<character>,<color> seeds it, for tests. */
static int last_pick_char = -1;
static int last_pick_color;
static bool slippiFlow(void)
{
    return (online_kind == ONLINE_KIND_UNRANKED || online_kind == ONLINE_KIND_DIRECT) &&
           pc_net_match_slippi_ready();
}

/* Slippi's Unranked searches on the CSS and has no lobby screen: if the lobby
 * scene is reached at all (the match is agreed, or a stray exit), it only passes
 * through. The lobby view is for LAN and the login flows. */
static bool unrankedPassThrough(void)
{
    return online_kind == ONLINE_KIND_UNRANKED && slippiFlow();
}

bool gmOnline_SlippiFlow(void)
{
    /* The kind outlasts the mode: VS. Mode's own CSS must not be Slippi's. */
    return gm_GetCurrentGameMode() == GM_ONLINE && slippiFlow();
}
#endif

void gmOnline_SetKind(OnlineKind kind)
{
    online_kind = kind;
#ifdef TARGET_PC
    /* Entered from the menu: a pick from an earlier visit (START on the CSS,
     * then backed out) would skip this visit's CSS for the lobby view. */
    slippi_picked = false;
#endif
}

OnlineKind gmOnline_GetKind(void)
{
    return online_kind;
}

void onEnterLobby(UNUSED GameModeState* state)
{
#ifdef TARGET_PC
    /* Terminal sets leave Results deterministically. Keep transport alive
     * here while both peers finish signing and durable saving. */
    awaiting_rank_result = online_kind == ONLINE_KIND_RANKED &&
        (pc_rank_session_set_complete() ||
         pc_rank_session_state(NULL) == PC_RANK_SESSION_FAILED);
    if (!awaiting_rank_result) {
        pc_net_match_stop();
        pc_net_disconnect();
    }
    pc_lan_stop();
#endif
    /* Same CSS start state on both peers: two human doors, nothing picked. */
    gm_InitVsMode(&online_vs);
    online_vs.start.players[0].slot_type = Gm_PKind_Human;
    online_vs.start.players[1].slot_type =
#ifdef TARGET_PC
        slippiFlow() ? Gm_PKind_NA :
#endif
                       Gm_PKind_Human;
    for (int i = 2; i < GM_MAX_PLAYERS; ++i)
        online_vs.start.players[i].slot_type = Gm_PKind_NA;
}

void onExitLobby(UNUSED GameModeState* state)
{
#ifdef TARGET_PC
    if (online_kind == ONLINE_KIND_PLACE) {
        gm_SetNextGameModeStateId(state_place);
        return;
    }
    PcNetSlippiMatch m;
    if (slippiFlow() && slippi_picked && pc_net_match_slippi_match(&m)) {
        gm_SetNextGameModeStateId(state_sss); /* forced stage: it exits at once */
    }
#endif
}

#ifdef TARGET_PC
static bool rankedMode(void) { return online_kind == ONLINE_KIND_RANKED; }

static void rankedRules(StartMeleeData* start, UNUSED StartMeleeData* previous)
{
    if (!rankedMode() || !pc_rank_session_active()) return;
    start->rules.match_kind = MatchKind_Stock;
    start->rules.is_stock = true;
    start->rules.is_teams = false;
    start->rules.timer_enabled = true;
    start->rules.timer_counts_up = false;
    start->rules.time_limit = pc_rank_session_seconds();
    start->rules.disable_pausing = true;
    start->rules.item_freq = -1;
    start->rules.x20 = 0;
    start->rules.x30 = 1.0f;
    start->rules.game_speed = 1.0f;
    start->rules.stkind = pc_rank_session_stage();
}

/* Pausing is local to a peer and the other never sees it, so it is off in
 * every Slippi match (Slippi itself allows it in Direct only: that needs the
 * pause synced, which is not built). */
static void slippiOrRankedRules(StartMeleeData* start, StartMeleeData* previous)
{
    if (slippiFlow() && pc_net_slippi_active() && online_kind != ONLINE_KIND_DIRECT) {
        /* Unranked: the real pause stays off; Slippi's own (the overlay with
         * the match running on, L+R+A+Start to quit) is gmvs.c's
         * slippiSoftPause. Direct keeps Melee's pause (InitPause.asm only
         * installs the client pause when pausing is off): the pads are
         * synced, so Start pauses both games on the same frame. */
        start->rules.disable_pausing = true;
    }
    rankedRules(start, previous);
}

static void rankedPlayer(PlayerInitData* start, PlayerInitData* previous)
{
    if (!rankedMode() || !pc_rank_session_active()) return;
    start->stocks = pc_rank_session_stocks();
    start->handicap = 5;
    start->attack_ratio = start->defense_ratio = start->model_scale = 1.0f;
    start->damage = start->damage1 = 0;
    start->vs_metal = start->vs_invisible = false;
    if (previous == &online_vs.start.players[0] || previous == &online_vs.start.players[1])
        start->slot_type = Gm_PKind_Human;
    else
        start->slot_type = Gm_PKind_NA;
}
#endif

#ifdef TARGET_PC
static void routeAfterFailure(void)
{
    static const char* const why[] = { "", "Opponent left", "Connection timed out", "Desync",
                                       "Incompatible version", "Could not resume" };
    if (slippiFlow()) {
        int reason = pc_net_peer_status();
        snprintf(slpcss_notice, sizeof slpcss_notice, "%s",
                 reason > 0 && reason < (int) (sizeof why / sizeof why[0]) ? why[reason] : "Connection lost");
        pc_net_disconnect();
        pc_net_match_stop();
        pc_net_peer_status_clear();
        gm_SetNextGameModeStateId(state_css);
        return;
    }
    gm_SetNextGameModeStateId(state_lobby);
}
#endif

void onEnterCss(GameModeState* state)
{
#ifdef TARGET_PC
    pc_log_line("online: enter CSS at frame %d", pc_net_frame());
    slippi_picked = false;
    if (slippiFlow()) {
        /* Slippi assumes everything is unlocked; the player's own progress is
         * put back when this screen leaves for the menu. */
        pc_net_unlock_pin();
        slpcss_leave = false;
        direct_to_sss = false;
        direct_z_hold = 0;
        slpcss_state = pc_net_match_slippi_lobby() ? SLPCSS_LOBBY :
                       pc_net_match_is_slippi()    ? SLPCSS_SEARCH :
                                                     SLPCSS_SELECT;
        /* One human door: the opponent does not exist until a match is found. */
        online_vs.start.players[0].slot_type = Gm_PKind_Human;
        online_vs.start.players[1].slot_type = Gm_PKind_NA;
        if (last_pick_char < 0) {
            const char* seed = getenv("MELEE_CSS_PICK");
            int c, k = 0;
            if (seed != NULL && sscanf(seed, "%d,%d", &c, &k) >= 1 && c >= 0 && c < 26) {
                last_pick_char = c;
                last_pick_color = k;
            }
        }
        if (last_pick_char >= 0) {
            online_vs.start.players[0].ckind = last_pick_char;
            online_vs.start.players[0].color = last_pick_color;
        }
    }
#endif
#ifdef TARGET_PC
    if (slippiFlow()) {
        /* Slippi's CSS is a one-player layout (wide card, "1-P" badge, no
         * rules bar). Event Match's is the nearest; its "MELEE" title is put
         * in by mncharsel.c (cssSlippiTitle). */
        gmVsMelee_EnterCss(state, &online_vs, EVENT_MATCH);
        return;
    }
#endif
    gmVsMelee_EnterCss(state, &online_vs, VS_MELEE);
}

void onExitCss(GameModeState* state)
{
#ifdef TARGET_PC
    if (slippiFlow()) {
        /* B leaves the mode; START with a pick goes on to the search. The
         * sound banks for the characters and stage are loaded at the VS
         * entry, once both players' picks are known. */
        CSSData* css = gm_GetGameModeStateExitData(state);
        if (css->pending_scene_change == CSSPendingSceneChange_2) {
            pc_net_match_stop();
            pc_net_unlock_release();
            directLobbyEnd();
            gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        } else if (pc_net_peer_status() != PC_NET_PEER_OK) {
            /* A failure (a timeout while searching, a late desync) ended this
             * scene: back to a fresh CSS with the reason on it, never the
             * lobby. */
            routeAfterFailure();
        } else if (slpcss_leave) {
            online_vs = css->vs;
            gm_SetNextGameModeStateId(state_sss); /* forced stage: it exits at once */
        } else if (direct_to_sss) {
            /* Direct's loser picks the next stage on the real stage select. */
            online_vs = css->vs;
            direct_to_sss = false;
            direct_picking = true;
            gm_SetNextGameModeStateId(state_sss);
        } else {
            online_vs = css->vs;
            const PlayerInitData* me = &online_vs.start.players[0];
            last_pick_char = me->ckind;
            last_pick_color = me->color;
            pc_net_match_set_slippi_pick((uint8_t) me->ckind, (uint8_t) me->color);
            slippi_picked = true;
            gm_SetNextGameModeStateId(state_lobby);
        }
        return;
    }
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        routeAfterFailure();
        return;
    }
#endif
    CSSData* css = gm_GetGameModeStateExitData(state);
    if (css->pending_scene_change == CSSPendingSceneChange_2) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#ifdef TARGET_PC
    if (rankedMode() && !pc_rank_session_active()) {
        gm_SetNextGameModeStateId(state_lobby);
        return;
    }
#endif
    gmVsMelee_ExitCss(state, &online_vs);
}

#ifdef TARGET_PC
/* ---- Slippi's CSS: the pick, the search and the status text -------------
 * The CSS is the lobby (docs/slippi-online-flow-plan.md): START with a pick
 * starts the search on this very screen, the controller is locked, and
 * Slippi's own lines say what is happening until the match is agreed. Direct
 * still leaves for the code entry and comes back here to wait. */
extern bool mnCharSel_SlippiStartReady(void);
static char direct_calling[18]; /* defined with the rest of the direct entry */

static const char* slpcssModeName(void)
{
    return online_kind == ONLINE_KIND_DIRECT ? "Direct" : "Unranked";
}

bool gmOnline_SlippiCssLocked(void)
{
    return slippiFlow() && (slpcss_state == SLPCSS_SEARCH ||
                            (slpcss_state == SLPCSS_LOBBY && pc_net_match_slippi_locked_in()));
}

static void directCall(const char* code);
static void directLobbyEnd(void)
{
    direct_result = DIRECT_FIRST;
    direct_chose_stage = direct_to_sss = direct_picking = false;
    direct_z_hold = 0;
}
extern void mnCharSel_SlippiKeyboard(const char* initial, int max_len, const char* description,
                                     MnNameNewDoneFn done, MnNameNewFillFn fill, void* user);
static char slpcss_code[16]; /* the last code typed, so a refused one can be fixed */

/* The Name Entry screen ended: a good code starts the call, and the CSS shows
 * the search. */
static bool slpDirectDone(bool confirmed, const char* text, UNUSED void* user)
{
    char code[PC_SLIPPI_CONNECT_CODE_MAX];
    if (!confirmed) {
        return true;
    }
    snprintf(slpcss_code, sizeof slpcss_code, "%s", text);
    if (!pc_slippi_connect_code_normalize(text, code, sizeof code)) {
        snprintf(slpcss_notice, sizeof slpcss_notice, "A code looks like NAME#123");
        return true;
    }
    pc_net_peer_status_clear();
    directCall(code);
    slpcss_state = SLPCSS_SEARCH;
    pc_log_line("css: direct call to %s", code);
    return true;
}

/* Y fills the entry: a code on the clipboard, else the last one played. */
static const char* slpDirectFill(UNUSED void* user)
{
    static char code[18];
    PcNetContact last;
    if (pc_net_match_clipboard_code(code)) {
        return code;
    }
    if (pc_net_match_contacts(&last, 1) > 0) {
        snprintf(code, sizeof code, "%s", last.code);
        return code;
    }
    return NULL;
}

bool gmOnline_SlippiCssStart(int character, int color)
{
    if (!slippiFlow()) {
        return false;
    }
    last_pick_char = character;
    last_pick_color = color;
    pc_net_match_set_slippi_pick((uint8_t) character, (uint8_t) color);
    slippi_picked = true;
    if (slpcss_state == SLPCSS_LOBBY) {
        if (pc_net_match_slippi_locked_in()) {
            return true;
        }
        if (direct_result == DIRECT_LOST && !direct_chose_stage) {
            direct_to_sss = true; /* the CSS leaves on its next frame */
        } else if (pc_net_match_slippi_lock_in(PC_SLIPPI_STAGE_NONE, 0)) {
            pc_log_line("css: locked in, playing the opponent's stage");
        }
        return true;
    }
    if (online_kind == ONLINE_KIND_DIRECT) {
        directLobbyEnd();
        /* The code is typed on Melee's Name Entry screen, straight from here. */
        mnCharSel_SlippiKeyboard(slpcss_code, 11, "Enter your opponent's connect code.",
                                 slpDirectDone, slpDirectFill, NULL);
        return true;
    }
    pc_net_peer_status_clear();
    pc_net_match_start(PC_MATCH_UNRANKED, NULL);
    slpcss_state = SLPCSS_SEARCH;
    pc_log_line("css: search started");
    return true;
}

static void cssStep(OnlineCssStep* steps, int* n, char mark, const char* fmt, const char* arg)
{
    if (*n < CSS_STEPS_MAX) {
        steps[*n].mark = mark;
        snprintf(steps[*n].text, sizeof steps[*n].text, fmt, arg != NULL ? arg : "");
        (*n)++;
    }
}

bool gmOnline_SlippiCssFrame(void)
{
    char title[32], hint[64] = "";
    OnlineCssStep steps[CSS_STEPS_MAX];
    int n = 0;
    u64 z = 0;
    PcNetSlippiMatch sm;

    snprintf(title, sizeof title, "%s Mode", slpcssModeName());
    mnOnlineCss_SetCode(online_kind == ONLINE_KIND_DIRECT ? pc_net_match_slippi_code() : "");
    /* Slippi's "Playing: <name>" line, whenever the opponent is connected. */
    mnOnlineCss_SetPlaying(slpcss_state == SLPCSS_LOBBY || slpcss_leave ||
                                   (slpcss_state == SLPCSS_SEARCH &&
                                    pc_net_match_state(NULL) == PC_MATCH_READY) ?
                               pc_net_match_opponent_name() :
                               "");
    for (int p = 0; p < PAD_MAX_CONTROLLERS; p++) {
        z |= HSD_PadCopyStatus[p].trigger & HSD_PAD_Z;
    }
    if (slpcss_state == SLPCSS_LOBBY) {
        const char* why = NULL;
        const char* opp = pc_net_match_opponent_code();
        bool zheld = false;
        int st;
        for (int p = 0; p < PAD_MAX_CONTROLLERS; p++) {
            zheld |= (HSD_PadCopyStatus[p].button & HSD_PAD_Z) != 0;
        }
        pc_net_match_poll();
        st = pc_net_match_state(&why);
        if (st == PC_MATCH_FAIL || pc_net_peer_status() != PC_NET_PEER_OK) {
            /* The opponent left the lobby: the error stands until Z, then the
             * CSS is a fresh one, as after a failed search. */
            directLobbyEnd();
            slpcss_state = SLPCSS_SEARCH;
            return gmOnline_SlippiCssFrame();
        }
        if (slpcss_leave || pc_net_match_slippi_match(&sm)) {
            slpcss_leave = true;
            cssStep(steps, &n, '-', "Character selected", NULL);
            cssStep(steps, &n, '-', "Locked in", NULL);
            mnOnlineCss_Update(title, pc_net_match_slippi_name(), steps, n, hint);
            return true;
        }
        if (direct_to_sss) {
            mnOnlineCss_Update(title, pc_net_match_slippi_name(), steps, n, hint);
            return true;
        }
        if (mnCharSel_SlippiStartReady() || pc_net_match_slippi_locked_in()) {
            cssStep(steps, &n, '-', "Character selected", NULL);
        } else {
            cssStep(steps, &n, 'x', "Select your character", NULL);
        }
        if (pc_net_match_slippi_locked_in()) {
            cssStep(steps, &n, '-', "Locked in", NULL);
            cssStep(steps, &n, 'x', "Waiting on opponent", NULL); /* Slippi's words */
        } else if (n > 0 && steps[0].mark == '-') {
            cssStep(steps, &n, '+', "Press START to %s",
                    direct_result == DIRECT_LOST && !direct_chose_stage ? "select stage" : "lock in");
        }
        snprintf(hint, sizeof hint, "Hold Z to disconnect");
        direct_z_hold = zheld ? direct_z_hold + 1 : 0;
        if (direct_z_hold > DIRECT_DISCONNECT_HOLD) {
            pc_log_line("css: Direct lobby left (Z held)");
            pc_net_match_stop();
            pc_net_peer_status_clear();
            directLobbyEnd();
            slpcss_state = SLPCSS_SELECT;
        }
        mnOnlineCss_Update(title, pc_net_match_slippi_name(), steps, n, hint);
        return false;
    }
    if (slpcss_state == SLPCSS_SELECT) {
        if (slpcss_notice[0]) {
            cssStep(steps, &n, '!', "Error: %.48s", slpcss_notice);
            snprintf(hint, sizeof hint, "Press Z to clear error");
            if (z) {
                slpcss_notice[0] = '\0';
            }
        }
        if (mnCharSel_SlippiStartReady()) {
            cssStep(steps, &n, '-', "Character selected", NULL);
            cssStep(steps, &n, '+', "Press START to %s",
                    online_kind == ONLINE_KIND_DIRECT ? "enter code" : "search");
        } else {
            cssStep(steps, &n, 'x', "Select your character", NULL);
        }
    } else {
        /* The steps stack, as Slippi's: what is done stays, the current one
         * follows it. */
        const char* why = NULL;
        const char* opp = pc_net_match_opponent_code();
        int st;
        pc_net_match_poll();
        st = pc_net_match_state(&why);
        cssStep(steps, &n, '-', "Character selected", NULL);
        cssStep(steps, &n, '-', "Locked in", NULL);
        if (slpcss_leave) {
            /* agreed: the CSS is on its way out */
        } else if (pc_net_peer_status() != PC_NET_PEER_OK || st == PC_MATCH_FAIL) {
            cssStep(steps, &n, '!', "Error: %.48s", why != NULL ? why : "Connection lost");
            snprintf(hint, sizeof hint, "Press Z to clear error");
            if (z) {
                pc_net_match_stop();
                pc_net_peer_status_clear();
                slpcss_state = SLPCSS_SELECT;
            }
            mnOnlineCss_Update(title, pc_net_match_slippi_name(), steps, n, hint);
            return false;
        } else if (pc_net_match_slippi_match(&sm)) {
            slpcss_leave = true;
            mnOnlineCss_Update(title, pc_net_match_slippi_name(), steps, n, hint);
            return true;
        } else if (st == PC_MATCH_CONNECT) {
            cssStep(steps, &n, 'x', "Connecting to %.17s", opp);
            snprintf(hint, sizeof hint, "Press Z to disconnect");
        } else if (st == PC_MATCH_READY) {
            cssStep(steps, &n, 'x', "Waiting on opponent", NULL); /* Slippi's words */
            snprintf(hint, sizeof hint, "Press Z to disconnect");
        } else {
            if (online_kind == ONLINE_KIND_DIRECT && direct_calling[0]) {
                cssStep(steps, &n, 'x', "Searching for %.17s", direct_calling);
            } else {
                cssStep(steps, &n, 'x', "Searching for opponent", NULL);
            }
            snprintf(hint, sizeof hint, "Press Z to cancel");
        }
        if (z && !slpcss_leave) {
            pc_log_line("css: search cancelled");
            pc_net_match_stop();
            slpcss_state = SLPCSS_SELECT;
        }
    }
    mnOnlineCss_Update(title, pc_net_match_slippi_name(), steps, n, hint);
    return false;
}

/* Fill the private VS data from the exchanged selections, under Slippi's
 * rules, and do what leaving the CSS would have done for the game's file and
 * sound loading (the characters are only known now). The scene sequence stays
 * the vanilla one: the stage select follows, with the stage forced, so the
 * game's own preloading and heap handling run between the CSS and the VS. */
static void slippiPrepareMatch(const PcNetSlippiMatch* sm)
{
    pc_net_slippi_rules_force(sm->alt_stage_mode != 0);
    for (int port = 0; port < 2; port++) {
        PlayerInitData* p = &online_vs.start.players[port];
        p->slot_type = Gm_PKind_Human;
        p->ckind = sm->character[port];
        p->color = sm->color[port];
    }
    for (int i = 2; i < GM_MAX_PLAYERS; ++i)
        online_vs.start.players[i].slot_type = Gm_PKind_NA;
    online_vs.start.rules.stkind = sm->stage;
    {
        struct GameCache* cache = &lbDvd_GetPreloadCacheScene()->game_cache;
        for (int port = 0; port < 2; port++) {
            cache->entries[port].char_id = sm->character[port];
            cache->entries[port].color = sm->color[port];
        }
        u64 mask = 0;
        ssize_t i;
        for (i = 0; i < GM_MAX_PLAYERS; i++) {
            mask |= lbAudioAx_80026E84(online_vs.start.players[i].ckind);
        }
        lbAudioAx_80026F2C(20);
        lbAudioAx_8002702C(4, mask);
        lbAudioAx_80027168();
    }
}
#endif

void onEnterSss(GameModeState* state)
{
#ifdef TARGET_PC
    pc_log_line("online: enter SSS at frame %d", pc_net_frame());
    if (direct_picking) {
        /* Nothing is agreed yet: an ordinary pick, with Slippi's Frozen
         * Stadium toggle (Z) on. */
        gmVsMelee_EnterSss(state, &online_vs);
        ((SSSData*) gm_GetGameModeStateEnterData(state))->no_lras = true;
        mnStageSel_SetFrozenToggle(true);
        return;
    }
    {
        PcNetSlippiMatch sm;
        if (slippiFlow() && pc_net_match_slippi_match(&sm)) {
            slippiPrepareMatch(&sm);
            gmVsMelee_EnterSss(state, &online_vs);
            SSSData* sss = gm_GetGameModeStateEnterData(state);
            sss->force_stage_id = (s8) sm.stage;
            sss->no_lras = true;
            return;
        }
    }
#endif
    gmVsMelee_EnterSss(state, &online_vs);
#ifdef TARGET_PC
    if (rankedMode() && pc_rank_session_active()) {
        SSSData* sss = gm_GetGameModeStateEnterData(state);
        pc_rank_session_stage_begin();
        sss->force_stage_id = pc_rank_session_stage() ? (int) pc_rank_session_stage() : -1;
        sss->no_lras = true;
    }
#endif
}

void onExitSss(GameModeState* state)
{
#ifdef TARGET_PC
    if (direct_picking) {
        SSSData* sss = gm_GetGameModeStateExitData(state);
        bool frozen = mnStageSel_FrozenToggled();
        direct_picking = false;
        mnStageSel_SetFrozenToggle(false);
        if (sss->start_game) {
            int stage = sss->vs.start.rules.stkind;
            uint8_t alt = stage == 3 && frozen ? 1 : 0; /* Pokemon Stadium */
            direct_chose_stage = true;
            if (pc_net_match_slippi_lock_in(stage, alt)) {
                pc_log_line("css: locked in with stage %d%s", stage, alt ? " (frozen)" : "");
            }
        }
        /* Back to the CSS either way: it waits there for the opponent (B on
         * the stage select only goes back to choose again). */
        gm_SetNextGameModeStateId(state_css);
        return;
    }
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        routeAfterFailure();
        return;
    }
#endif
    gmVsMelee_ExitSss(state, &online_vs, state_css);
#ifdef TARGET_PC
    {
        PcNetSlippiMatch sm;
        if (slippiFlow() && pc_net_match_slippi_match(&sm) &&
            ((SSSData*) gm_GetGameModeStateExitData(state))->start_game) {
            gm_SetNextGameModeStateId(state_intro); /* the VS splash, then the fight */
        }
    }
    if (rankedMode() && !((SSSData*) gm_GetGameModeStateExitData(state))->start_game) {
        pc_rank_session_abort("ranked stage selection cancelled");
        gm_SetNextGameModeStateId(state_lobby);
    }
#endif
}

#ifdef TARGET_PC
/* XR: Battlefield with two idle fighters, tinted, while the player places the
 * stage (gmvs.c, xr_place.c); A or B ends it and the mode goes on to the
 * kind that asked, or back to the menu it was entered from. */
static OnlineKind kind_after_place = ONLINE_KIND_PLACE;
static int mode_after_place = -1;
extern bool gm_vs_place_dummy;

void gmOnline_SetKindAfterPlace(OnlineKind kind)
{
    kind_after_place = kind;
}

void gmOnline_SetModeAfterPlace(int mode)
{
    mode_after_place = mode;
}

static bool place_from_options;

void gmOnline_SetPlaceFromOptions(bool from_options)
{
    place_from_options = from_options;
}

bool gmOnline_TakePlaceFromOptions(void)
{
    const bool r = place_from_options;
    place_from_options = false;
    return r;
}

void onEnterPlace(GameModeState* state)
{
    pc_log_line("online: stage placement scene");
    gm_vs_place_dummy = true;
    pc_xr_place_dummy_start();
    gmVsMode_PlaceDummyEnter(state);
}

void onExitPlace(UNUSED GameModeState* state)
{
    gm_vs_place_dummy = false;
    if (mode_after_place >= 0) {
        const int mode = mode_after_place; /* straight on to VS, 1P... */
        mode_after_place = -1;
        gm_ChangeGameModeAfterCurrentScene(mode);
    } else if (kind_after_place != ONLINE_KIND_PLACE) {
        online_kind = kind_after_place; /* straight on to Unranked or Direct */
        kind_after_place = ONLINE_KIND_PLACE;
        gm_ChangeGameModeAfterCurrentScene(GM_ONLINE);
    } else {
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
    }
}
#else
void onEnterPlace(UNUSED GameModeState* state) {}
void onExitPlace(UNUSED GameModeState* state) {}
void gmOnline_SetPlaceFromOptions(UNUSED bool from_options) {}
bool gmOnline_TakePlaceFromOptions(void)
{
    return false;
}
#endif

void onEnterIntro(GameModeState* state)
{
    OnlineIntroData* d = gm_GetGameModeStateEnterData(state);
#ifdef TARGET_PC
    PcNetSlippiMatch sm;
    pc_log_line("online: enter VS splash at frame %d", pc_net_frame());
    memset(d, 0, sizeof *d);
    if (slippiFlow() && pc_net_match_slippi_match(&sm)) {
        d->kind = 0;
        d->slot = 0;
        d->nametag = 0x78;
        d->stage_no = 1;
        d->ally_n = 1;
        d->enemy_n = 1;
        /* The local player is on the left, the opponent on the right, and the
         * announcer calls the opponent's character, as Slippi's. */
        d->ally[0] = sm.character[sm.local_player];
        d->ally[1] = d->ally[2] = 0x21;
        d->ally_color[0] = sm.color[sm.local_player];
        d->enemy[0] = sm.character[1 - sm.local_player];
        d->enemy[1] = d->enemy[2] = 0x21;
        d->enemy_color[0] = sm.color[1 - sm.local_player];
        gm_vs_splash_online = true;
        return;
    }
#endif
    /* Any other online mode: the scene ends on its first frame. */
    d->enemy[0] = 0x1A;
}

void onExitIntro(UNUSED GameModeState* state)
{
#ifdef TARGET_PC
    gm_vs_splash_online = false;
    if (slippiFlow() && pc_net_peer_status() != PC_NET_PEER_OK) {
        routeAfterFailure();
        return;
    }
#endif
    gm_SetNextGameModeStateId(state_vs);
}

void onEnterVs(GameModeState* state)
{
#ifdef TARGET_PC
    pc_log_line("online: enter VS at frame %d", pc_net_frame());
#endif
#ifdef TARGET_PC
    PcNetSlippiMatch sm;
    if (slippiFlow() && pc_net_match_slippi_match(&sm)) {
        /* The match was built when the stage select was entered; start the
         * rollback engine on the link now, before the scene's first tick. */
        if (!pc_net_slippi_adopt(sm.local_player, sm.rng_offset)) {
            gm_SetNextGameModeStateId(state_css);
            return;
        }
    }
    gmVsMelee_EnterVs(state, &online_vs, slippiOrRankedRules, rankedPlayer);
#else
    gmVsMelee_EnterVs(state, &online_vs, NULL, NULL);
#endif
}

void onExitVs(GameModeState* state)
{
#ifdef TARGET_PC
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        if (rankedMode()) {
            pc_rank_session_abort("peer disconnected");
        }
        routeAfterFailure();
        return;
    }
#endif
    MatchExitInfo* mei;
    ssize_t i;

    gmVsMelee_ExitVs(state, state_results, state_sudden_death);
    mei = gm_GetGameModeStateExitData(state);
#ifdef TARGET_PC
    if (rankedMode()) {
        MatchEnd* end = &mei->match_end;
        if (gm_WasMatchCanceled(end->outcome) || end->is_teams ||
            end->player_standings[0].pkind != Gm_PKind_Human ||
            end->player_standings[1].pkind != Gm_PKind_Human) {
            pc_rank_session_abort("ranked game cancelled or invalid");
        } else {
            unsigned winner = end->n_winners == 1 ? end->winners[0] : PC_RANK_TIE;
            int a = end->player_standings[0].stocks;
            int b = end->player_standings[1].stocks;
            pc_rank_session_game(winner, a < 0 ? 0 : a, b < 0 ? 0 : b,
                                 gmVsMelee_StartData.rules.stkind, end->frame_count);
        }
        /* Ties return through CSS to a fresh one-stock, three-minute game;
         * vanilla sudden death would begin at 300 percent. */
        gm_SetNextGameModeStateId(state_results);
    }
    if (slippiFlow() && online_kind == ONLINE_KIND_DIRECT && pc_net_slippi_active()) {
        /* Slippi's Direct: no results and no sudden death, back to the CSS
         * with the link up; who won decides who picks the next stage. */
        MatchEnd* end = &mei->match_end;
        PcNetSlippiMatch sm;
        int me = pc_net_match_slippi_match(&sm) ? sm.local_player : pc_net_local_player();
        if (end->outcome == OUTCOME_NO_CONTEST && gm_vs_slippi_quitter >= 0) {
            direct_result = gm_vs_slippi_quitter == me ? DIRECT_LOST : DIRECT_WON;
        } else if (end->n_winners == 1 && !gm_WasMatchCanceled(end->outcome)) {
            direct_result = end->winners[0] == me ? DIRECT_WON : DIRECT_LOST;
        } else {
            direct_result = DIRECT_LOST; /* a draw: both pick, the lower port's counts */
        }
        direct_chose_stage = false;
        pc_log_line("online: Direct game over at frame %d, we %s", pc_net_frame(),
                    direct_result == DIRECT_WON ? "won" : "lost or drew");
        if (!pc_net_match_slippi_rearm()) {
            pc_net_disconnect();
            pc_net_match_stop();
            directLobbyEnd();
        }
        gm_SetNextGameModeStateId(state_css);
    } else if (slippiFlow() && online_kind == ONLINE_KIND_UNRANKED && pc_net_slippi_active() &&
        !gm_MatchHasMultipleWinners(&mei->match_end)) {
        /* Slippi's Unranked has no results screen: the set is over, so the
         * link closes and the next search starts from the CSS. A tie still
         * goes to sudden death first. */
        pc_log_line("online: set over at frame %d, skipping the results", pc_net_frame());
        pc_net_disconnect();
        pc_net_match_stop();
        gm_SetNextGameModeStateId(state_css);
    }
#endif
    for (i = 0; i < GM_MAX_PLAYERS; i++) {
        if (mei->match_end.player_standings[i].pkind != Gm_PKind_NA) {
            gm_80162A98(mei->match_end.player_standings[i].x20);
            gm_RecordSelfDestructs(
                mei->match_end.player_standings[i].self_destructs);
            gm_80162A4C(mei->match_end.player_standings[i].x44);
        }
    }
}

void onEnterSuddenDeath(GameModeState* state)
{
    gmVsMelee_EnterSuddenDeath(state, &online_vs, NULL, NULL);
}

void onExitSuddenDeath(GameModeState* state)
{
#ifdef TARGET_PC
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        routeAfterFailure();
        return;
    }
#endif
    gmVsMelee_ExitSuddenDeath(state);
}

void onEnterResults(GameModeState* state)
{
#ifdef TARGET_PC
    pc_log_line("online: enter RESULTS at frame %d", pc_net_frame());
    if (slippiFlow() && pc_net_slippi_active()) {
        /* Slippi's match is over: the results and what follows are local. */
        pc_net_disconnect();
        pc_net_match_stop(); /* the next search starts from the CSS */
    }
#endif
    gmVsMelee_EnterResults(state);
}

void onExitResults(GameModeState* state)
{
#ifdef TARGET_PC
    if (pc_net_peer_status() != PC_NET_PEER_OK) {
        routeAfterFailure();
        return;
    }
#endif
    gmVsMelee_ExitResults(state, &online_vs, state_css);
#ifdef TARGET_PC
    if (rankedMode()) {
        /* The results exit is driven by synchronized pads. Reliable-message
         * arrival and disk speed must never choose different next scenes.
         * Both peers leave a terminal set for the lobby; that scene keeps
         * the connection alive until signing/saving finishes. */
        if (pc_rank_session_set_complete() ||
            gm_WasMatchCanceled(gmVsMelee_ResultsEnterData.match_end.outcome))
            gm_SetNextGameModeStateId(state_lobby);
    }
#endif
    if (!gm_WasMatchCanceled(gmVsMelee_ResultsEnterData.match_end.outcome)) {
        gm_801623A4(&gmVsMelee_ResultsEnterData.match_end);
    }
}

/* ---- lobby scene ------------------------------------------------------- */
#ifdef TARGET_PC
static char profile_message[ONLINE_LOBBY_MSG_LEN];
static void profileRefresh(void) {
    const char* code = pc_net_match_local_code();
    if (!code || !*code) {
        snprintf(profile_message, sizeof profile_message, "Identity unavailable. Check your profile files.");
        return;
    }
    PcNetRankStoreResult result;
    PcNetRankStore* store = pc_rank_store_open(pc_net_match_profile_directory(),
        pc_net_match_identity()->public_key, &result);
    PcNetRating rating;
    unsigned count = 0;
    if (store && pc_rank_store_current(store, &rating, NULL, &count)) {
        double score = pc_rank_display(&rating);
        const char* tier = count < 5 ? "Placement" : score < 1050 ? "Bronze" :
            score < 1200 ? "Silver" : score < 1350 ? "Gold" : score < 1500 ? "Platinum" :
            score < 1650 ? "Diamond" : "Master";
        snprintf(profile_message, sizeof profile_message, "%s %.0f - %u sets. Community rating, unverified. B: back", tier, score, count);
    } else snprintf(profile_message, sizeof profile_message, "Rating history unavailable or damaged. B: back");
    pc_rank_store_close(store);
}
static bool internetLobby(void) {
    return online_kind != ONLINE_KIND_LAN && online_kind != ONLINE_KIND_PROFILE &&
           online_kind != ONLINE_KIND_LOGIN && online_kind != ONLINE_KIND_REGISTER &&
           !(online_kind == ONLINE_KIND_DIRECT && getenv("MELEE_LAN_DIRECT"));
}

/* Direct Connect. From the online-UX research (2026-09): the code is
 * your identity but you should rarely have to type it. So the first screen
 * is a hub -- a code found on the clipboard, everyone you have played,
 * "enter a code", "wait for a friend" -- and typing, when it is needed, is
 * eight characters on a keyboard grid rather than seventeen slots on a
 * wheel. Only the part after the '#' is typed: it is derived from the
 * friend's key and is all that identifies them (net_match.c pairing_topic),
 * so the name can be wrong or missing and the call still connects. Both
 * players may dial each other; they meet on a topic both compute.
 *
 * States: HUB -> KEYS -> (call) ; HUB -> (call or wait). The call itself is
 * the internet lobby below, with a progress line and a clock. */
#define DIRECT_SUFFIX 8
enum { DIRECT_OFF, DIRECT_HUB, DIRECT_KEYS };
enum { ROW_PASTE, ROW_CONTACT, ROW_TYPE, ROW_WAIT };
static const char direct_keys[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
/* A Slippi connect code: a tag of up to 7 letters/digits, '#', up to 3 digits. */
#define DIRECT_SLIPPI_MAX 11
static const char direct_keys_slippi[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789#";
static bool directSlippi(void)
{
    return pc_net_match_slippi_ready();
}
static const char* directKeys(void)
{
    return directSlippi() ? direct_keys_slippi : direct_keys;
}
static int directKeyCount(void)
{
    return (int) strlen(directKeys());
}
static const char* directOwnCode(void)
{
    return directSlippi() ? pc_net_match_slippi_code() : pc_net_match_local_code();
}
static int direct_screen;
static int direct_cursor;       /* hub row */
static int direct_key;          /* keyboard cell */
static char direct_suffix[DIRECT_SLIPPI_MAX + 1];
static char direct_error[ONLINE_LOBBY_MSG_LEN];
static char direct_clip[18];    /* a code seen on the clipboard, or "" */
static PcNetContact direct_contacts[ONLINE_LOBBY_MENU_ROWS];
static int direct_contact_n;
static int direct_row_kind[ONLINE_LOBBY_MENU_ROWS];
static int direct_row_arg[ONLINE_LOBBY_MENU_ROWS];
static int direct_rows;
static char direct_calling[18]; /* who the current call is to, "" hosting */
static bool direct_copied;

static void directHubBuild(void)
{
    direct_rows = 0;
    if (pc_net_match_clipboard_code(direct_clip)) {
        direct_row_kind[direct_rows++] = ROW_PASTE;
    } else {
        direct_clip[0] = '\0';
    }
    direct_contact_n = pc_net_match_contacts(direct_contacts, ONLINE_LOBBY_MENU_ROWS - 3);
    for (int i = 0; i < direct_contact_n; i++) {
        direct_row_kind[direct_rows] = ROW_CONTACT;
        direct_row_arg[direct_rows++] = i;
    }
    direct_row_kind[direct_rows++] = ROW_TYPE;
    if (!directSlippi()) {
        /* Slippi Direct connects two players who each type the other's code;
         * there is no hosting without one. */
        direct_row_kind[direct_rows++] = ROW_WAIT;
    }
    if (direct_cursor >= direct_rows) {
        direct_cursor = 0;
    }
}

static void directEntryBegin(void)
{
    direct_screen = DIRECT_HUB;
    direct_cursor = 0;
    direct_error[0] = '\0';
    direct_copied = false;
    directHubBuild();
    pc_log_line("lobby: direct connect hub, %d recent, clipboard %s", direct_contact_n,
                direct_clip[0] ? direct_clip : "-");
}

static void directCall(const char* code)
{
    snprintf(direct_calling, sizeof direct_calling, "%s", code ? code : "");
    direct_screen = DIRECT_OFF;
    if (code != NULL) {
        pc_set_net_target(code);
    }
    pc_log_line("lobby: direct connect %s '%s'", code ? "calling" : "waiting as",
                code ? code : pc_net_match_local_code());
    pc_net_match_start(PC_MATCH_DIRECT, code);
}

/* "RYAN#K3XQ2M7A" -> "RYAN #K3X-Q2M-7A": the suffix in groups of three is
 * how a person reads a code aloud and checks it against a friend's. */
static void directShow(char* out, size_t n, const char* code)
{
    const char* hash = strchr(code, '#');
    if (hash == NULL || strlen(hash + 1) != DIRECT_SUFFIX) {
        snprintf(out, n, "%s", code);
        return;
    }
    const char* s = hash + 1;
    snprintf(out, n, "%.*s#%.3s-%.3s-%.2s", (int) (hash - code), code, s, s + 3, s + 6);
}

static void directAgo(char* out, size_t n, int64_t when)
{
    int64_t d = (int64_t) time(NULL) - when;
    if (d < 3600) {
        snprintf(out, n, "%dm ago", (int) (d / 60));
    } else if (d < 86400) {
        snprintf(out, n, "%dh ago", (int) (d / 3600));
    } else {
        snprintf(out, n, "%dd ago", (int) (d / 86400));
    }
}

/* One frame of the hub or the keyboard; true while it owns the screen. */
static bool directEntryFrame(OnlineLobbyView* view, u64 input)
{
    u64 repeat = gm_801A36C0(PAD_MAX_CONTROLLERS);
    char buf[ONLINE_LOBBY_MSG_LEN], shown[40];

    directShow(shown, sizeof shown, directOwnCode());
    snprintf(view->subtitle, sizeof view->subtitle, "YOU %s", shown);
    view->phase = LOBBY_PHASE_FOUND;

    if (direct_screen == DIRECT_HUB) {
        /* The clipboard is read again as the player moves, so a code copied
         * from a chat while the hub is open shows up without leaving it. */
        if ((lobby_frame_n++ % 30) == 0) {
            int was = direct_rows;
            directHubBuild();
            if (direct_rows != was) {
                direct_cursor = 0;
            }
        }
        if (repeat & PAD_ANY_UP) {
            direct_cursor = (direct_cursor + direct_rows - 1) % direct_rows;
            sfxMove();
        } else if (repeat & PAD_ANY_DOWN) {
            direct_cursor = (direct_cursor + 1) % direct_rows;
            sfxMove();
        }
        view->screen = LOBBY_SCREEN_MENU;
        view->menu_count = direct_rows;
        view->cursor = direct_cursor;
        for (int i = 0; i < direct_rows; i++) {
            switch (direct_row_kind[i]) {
            case ROW_PASTE:
                directShow(shown, sizeof shown, direct_clip);
                snprintf(view->menu[i], sizeof view->menu[i], "Call %s  from clipboard", shown);
                break;
            case ROW_CONTACT: {
                const PcNetContact* c = &direct_contacts[direct_row_arg[i]];
                char ago[16];
                directShow(shown, sizeof shown, c->code);
                directAgo(ago, sizeof ago, c->last_played);
                snprintf(view->menu[i], sizeof view->menu[i], "%-22s played %s", shown, ago);
                break;
            }
            case ROW_TYPE:
                snprintf(view->menu[i], sizeof view->menu[i], "Enter a friend's code");
                break;
            default:
                snprintf(view->menu[i], sizeof view->menu[i], "Wait for a friend to call me");
                break;
            }
        }
        snprintf(view->message, sizeof view->message, "%s",
                 direct_error[0] ? direct_error :
                 direct_copied ? "Your code is copied - send it to a friend" :
                 directSlippi() ? "Enter your friend's connect code. They enter yours." :
                 "Pick who to play. Both of you can call each other.");
        view->hint = "A: select   Y: copy my code   B: back";
        if (input & HSD_PAD_Y) {
            direct_copied = pc_net_match_copy_code();
            sfxForward();
        }
        if (input & (HSD_PAD_A | HSD_PAD_START)) {
            sfxForward();
            switch (direct_row_kind[direct_cursor]) {
            case ROW_PASTE:
                directCall(direct_clip);
                break;
            case ROW_CONTACT:
                directCall(direct_contacts[direct_row_arg[direct_cursor]].code);
                break;
            case ROW_TYPE:
                direct_screen = DIRECT_KEYS;
                direct_suffix[0] = '\0';
                direct_key = 0;
                direct_error[0] = '\0';
                break;
            default:
                directCall(NULL);
                break;
            }
        }
        return direct_screen != DIRECT_OFF;
    }

    /* KEYS. A Slippi code is typed whole (letters and digits, one '#'); a DHT
     * code is eight characters of A-Z and 2-7 (no 0, 1, 8 or 9, so there is
     * nothing to mistype them as). The grid wraps either way. */
    const bool slp = directSlippi();
    const char* keys = directKeys();
    const int nkeys = directKeyCount();
    const int maxlen = slp ? DIRECT_SLIPPI_MAX : DIRECT_SUFFIX;
    int len = (int) strlen(direct_suffix);
    {
        /* The pointer follows the keys, its click types one; "Call" is START. */
        float px, py;
        bool leads = false;
        int hit = -1;
        view->key_button_hot = -1;
        if (pc_xr_pointer_at(&px, &py, &leads)) {
            hit = mnOnlineLobby_KeyHit(px, py);
            if ((hit >= nkeys && hit < 100) || (hit >= 100 && hit != 102)) {
                hit = -1;
            }
            pc_xr_pointer_target(hit >= 0);
            if (leads && hit >= 0 && hit < 100) {
                direct_key = hit;
            }
            view->key_button_hot = leads && hit == 102 ? 2 : -1;
            if (leads && hit == 102 && (input & HSD_PAD_A)) {
                input = (input & ~(u64) HSD_PAD_A) | HSD_PAD_START;
            }
        }
    }
    if (repeat & PAD_ANY_LEFT) {
        direct_key = direct_key / ONLINE_LOBBY_KEY_COLS * ONLINE_LOBBY_KEY_COLS +
                     (direct_key + ONLINE_LOBBY_KEY_COLS - 1) % ONLINE_LOBBY_KEY_COLS;
        if (direct_key >= nkeys) {
            direct_key = nkeys - 1;
        }
        sfxMove();
    } else if (repeat & PAD_ANY_RIGHT) {
        direct_key = direct_key / ONLINE_LOBBY_KEY_COLS * ONLINE_LOBBY_KEY_COLS +
                     (direct_key + 1) % ONLINE_LOBBY_KEY_COLS;
        if (direct_key >= nkeys) {
            direct_key = direct_key / ONLINE_LOBBY_KEY_COLS * ONLINE_LOBBY_KEY_COLS;
        }
        sfxMove();
    } else if (repeat & PAD_ANY_UP) {
        direct_key = (direct_key + nkeys - ONLINE_LOBBY_KEY_COLS) % nkeys;
        sfxMove();
    } else if (repeat & PAD_ANY_DOWN) {
        direct_key = (direct_key + ONLINE_LOBBY_KEY_COLS) % nkeys;
        sfxMove();
    }
    if ((input & HSD_PAD_A) && len < maxlen) {
        char key = keys[direct_key];
        const char* hash = strchr(direct_suffix, '#');
        const char* refuse = NULL;
        if (slp && key == '#' && (len == 0 || hash != NULL)) {
            refuse = len == 0 ? "A code starts with its name, then #" : "A code has only one #";
        } else if (slp && hash != NULL && (key < '0' || key > '9')) {
            refuse = "After the # only digits (1-3)";
        } else if (slp && hash != NULL && (int) strlen(hash + 1) >= 3) {
            refuse = "A code has at most 3 digits after the #";
        } else if (slp && hash == NULL && len >= 7 && key != '#') {
            refuse = "A name has at most 7 characters - press #";
        }
        if (refuse != NULL) {
            snprintf(direct_error, sizeof direct_error, "%s", refuse);
            sfxBack();
        } else {
            direct_suffix[len++] = key;
            direct_suffix[len] = '\0';
            direct_error[0] = '\0';
            sfxForward();
        }
    } else if (input & HSD_PAD_B) {
        if (len > 0) {
            direct_suffix[--len] = '\0';
            direct_error[0] = '\0';
            sfxBack();
        } else {
            direct_screen = DIRECT_HUB; /* B on an empty code goes back a step */
            sfxBack();
            return true;
        }
    } else if (input & HSD_PAD_START) {
        if (slp && pc_slippi_connect_code_valid(direct_suffix)) {
            sfxForward();
            directCall(direct_suffix);
            return false;
        } else if (!slp && len == DIRECT_SUFFIX) {
            char code[18];
            snprintf(code, sizeof code, "#%s", direct_suffix);
            sfxForward();
            directCall(code);
            return false;
        }
        snprintf(direct_error, sizeof direct_error, "%s",
                 slp ? "A code looks like NAME#123" : "A code has 8 characters after the #");
        sfxBack();
    }
    view->screen = LOBBY_SCREEN_KEYS;
    view->keys = keys;
    view->key_cursor = direct_key;
    view->menu_count = 1;
    view->key_buttons[2] = "Call";
    if (slp) {
        snprintf(buf, sizeof buf, "Friend's code   %s_", direct_suffix);
        snprintf(view->menu[0], sizeof view->menu[0], "%s", buf);
    } else {
        char slots[DIRECT_SUFFIX + 1];
        for (int i = 0; i < DIRECT_SUFFIX; i++) {
            slots[i] = i < len ? direct_suffix[i] : '_';
        }
        slots[DIRECT_SUFFIX] = '\0';
        snprintf(buf, sizeof buf, "Friend's code   # %.3s-%.3s-%.2s", slots, slots + 3, slots + 6);
        snprintf(view->menu[0], sizeof view->menu[0], "%s", buf);
    }
    snprintf(view->message, sizeof view->message, "%s",
             direct_error[0] ? direct_error :
             slp ? "Type their name, #, then their digits" :
             "Type the 8 characters after the #");
    view->hint = "A: type   B: delete   START: call";
    return true;
}
#endif


void gm_Scene_OnlineLobby_OnEnter(UNUSED void* unused)
{
#ifdef TARGET_PC
    if (online_kind == ONLINE_KIND_LOGIN || online_kind == ONLINE_KIND_REGISTER) {
        direct_screen = DIRECT_OFF;
        mnSlippi_Begin(online_kind == ONLINE_KIND_REGISTER); /* its own screens */
        return;
    }
    if (online_kind == ONLINE_KIND_PLACE || unrankedPassThrough()) {
        return; /* a pass-through: to the placement scene, or on to the CSS */
    }
#endif
    mnOnlineLobby_Create();
#ifdef TARGET_PC
    direct_screen = DIRECT_OFF;
    if (slippiFlow() && !slippi_picked) {
        return; /* straight on to the CSS: the pick comes before the search */
    }
    if (online_kind == ONLINE_KIND_PROFILE) {
        profileRefresh();
    } else if (internetLobby() && online_kind == ONLINE_KIND_DIRECT) {
        /* Ask for the code first: starting on a stale launcher pref is how
         * two players both ended up hosting their own codes forever. */
        directEntryBegin();
    } else if (internetLobby() && !awaiting_rank_result &&
               (online_kind != ONLINE_KIND_RANKED ||
                                  pc_net_match_publication(NULL) == 0)) {
        if (pc_net_peer_status() == PC_NET_PEER_OK) {
            pc_net_match_start(online_kind == ONLINE_KIND_UNRANKED ? PC_MATCH_UNRANKED :
                               PC_MATCH_RANKED, NULL);
        }
    } else if (!internetLobby()) pc_lan_start();
#endif
}

void gm_Scene_OnlineLobby_OnExit(UNUSED void* unused)
{
#ifdef TARGET_PC
    if (online_kind == ONLINE_KIND_LOGIN || online_kind == ONLINE_KIND_REGISTER) {
        mnSlippi_End();
        return;
    }
    if (online_kind == ONLINE_KIND_PLACE || unrankedPassThrough()) {
        return;
    }
#endif
    mnOnlineLobby_Destroy();
}

#ifdef TARGET_PC
static const char* const peer_word[] = { "", "Peer left",
                                         "Connection timed out", "Desync",
                                         "Incompatible version",
                                         "Could not resume" };

static void lobbyCopyName(char* dst, const char* src)
{
    snprintf(dst, ONLINE_LOBBY_NAME_LEN, "%s", src);
}

/* Peers on our protocol and build: the only ones counted or started with. */
static int lobbyCompatible(const PcLanPeer* peers, int n)
{
    int i, nc = 0;
    for (i = 0; i < n; i++) {
        nc += peers[i].compatible;
    }
    return nc;
}

/* Fill the view from the LAN state; logs the status line when it changes. */
static void lobbyFillView(OnlineLobbyView* view, int state, const char* why,
                          const PcLanPeer* peers, int n)
{
    /* Indexed by pc_net_quality() 0..3 and pc_net_peer_status() 0..5;
     * anything outside stays blank. */
    static const char* const link_word[] = { "stable", "warning", "stalling",
                                             "reconnecting" };
    static char last_status[ONLINE_LOBBY_MSG_LEN];
    char status[ONLINE_LOBBY_MSG_LEN];
    bool connected = state == 1 || state == 2;
    bool host = connected && pc_lan_is_host();
    int ping = -1, delay;
    unsigned rollbacks;
    int quality, reason;
    int nc = lobbyCompatible(peers, n);
    int i;

    memset(view, 0, sizeof *view);
    view->title =
        online_kind == ONLINE_KIND_DIRECT ? "DIRECT CONNECT" : "LAN PLAY";
    if (connected && !pc_net_stats(&ping, &delay, &rollbacks)) {
        ping = -1;
    }
    quality = connected ? pc_net_quality() : -1;
    if (quality >= 0 && quality < (int) ARRAY_SIZE(link_word)) {
        view->link = link_word[quality];
    }

    lobbyCopyName(view->players[0].name, pc_lan_local_name());
    view->players[0].ping_ms = -1;
    view->players[0].is_host = host;
    view->players[0].is_local = true;
    view->player_count = 1;
    for (i = 0; i < n && view->player_count < ONLINE_LOBBY_MAX_PLAYERS; i++) {
        OnlineLobbyPlayer* p = &view->players[view->player_count++];
        lobbyCopyName(p->name, peers[i].name);
        p->is_host = peers[i].host;
        p->incompatible = !peers[i].compatible;
        /* The session peer: the host we joined, or our first peer as host. */
        p->ping_ms = connected && (peers[i].host || (host && i == 0)) ? ping : -1;
    }

    switch (state) {
    case 0:
        view->phase = nc == 0 ? LOBBY_PHASE_SEARCHING : LOBBY_PHASE_FOUND;
        if (nc == 0 && pc_lan_discovery_unavailable()) {
            /* Five seconds without even our own announce looping back:
             * this network (often guest or campus Wi-Fi) hides players
             * from each other, and waiting longer will not change it. */
            snprintf(status, sizeof status,
                     "This network hides other players - try DIRECT CONNECT");
        } else if (nc == 0) {
            snprintf(status, sizeof status, "LAN: searching...%s",
                     pc_lan_full() ? " - Lobby full" : "");
        } else {
            snprintf(status, sizeof status, "%d players found - press START%s",
                     nc + 1, pc_lan_full() ? " - Lobby full" : "");
        }
        break;
    case 1:
        view->phase = LOBBY_PHASE_CONNECTING;
        snprintf(status, sizeof status, "Connecting...");
        break;
    case 4:
        view->phase = LOBBY_PHASE_CONNECTING;
        snprintf(status, sizeof status, "Ready - waiting for host...");
        break;
    case 2:
        view->phase = LOBBY_PHASE_STARTING;
        view->countdown_frames = pc_lan_start_frame() - pc_net_frame();
        if (view->countdown_frames < 0) {
            view->countdown_frames = 0;
        }
        snprintf(status, sizeof status, "Starting...");
        break;
    default:
        view->phase = LOBBY_PHASE_ERROR;
        reason = pc_net_peer_status();
        if (reason <= 0 || reason >= (int) ARRAY_SIZE(peer_word)) {
            reason = 0;
        }
        /* Still in the lobby: Start elects again, and a peer's proposal is
         * still joined (net_lan.c), so leaving to retry is never needed. */
        snprintf(status, sizeof status, "Failed: %s%s%s%s",
                 why != NULL ? why : "unknown error", reason ? " - " : "",
                 peer_word[reason], nc ? " - START: retry" : "");
        break;
    }
    memcpy(view->message, status, sizeof view->message);
    if (strcmp(status, last_status) != 0) {
        memcpy(last_status, status, sizeof last_status);
        pc_log_line("lobby: %s", status);
    }
}
#endif

void gm_Scene_OnlineLobby_OnFrame(void)
{
#ifdef TARGET_PC
    PcLanPeer peers[PC_LAN_MAX_PEERS];
    OnlineLobbyView view;
    const char* why = NULL;
    int state;
    int n;
    u64 input = gm_GetButtonsTriggered(pc_net_active() ? pc_net_local_player() : PAD_MAX_CONTROLLERS);

    if (mnNameNew_PcActive()) {
        return; /* the keyboard runs its own procs and takes the pad */
    }
    if (online_kind == ONLINE_KIND_PLACE || unrankedPassThrough()) {
        gm_801A4B60(); /* nothing to show: on to the placement scene, or the CSS */
        return;
    }
    if (online_kind == ONLINE_KIND_LOGIN || online_kind == ONLINE_KIND_REGISTER) {
        mnSlippi_Frame();
        return;
    }
    if (slippiFlow() && !slippi_picked) {
        gm_801A4B60(); /* first pass: nothing to search yet, go pick */
        return;
    }
    if (slippiFlow() && online_kind == ONLINE_KIND_DIRECT && direct_screen == DIRECT_OFF &&
        pc_net_match_is_slippi()) {
        gm_801A4B60(); /* the code is dialled: the CSS shows the rest */
        return;
    }
    if (online_kind == ONLINE_KIND_PROFILE || internetLobby()) {
        memset(&view, 0, sizeof view);
        view.title = online_kind == ONLINE_KIND_PROFILE ? "PROFILE" :
                     online_kind == ONLINE_KIND_UNRANKED ? "UNRANKED" :
                     online_kind == ONLINE_KIND_RANKED ? "RANKED" : "DIRECT CONNECT";
        view.player_count = 1;
        view.players[0].is_local = true;
        view.players[0].ping_ms = -1;
        lobbyCopyName(view.players[0].name, pc_net_match_local_code());
        if (online_kind == ONLINE_KIND_PROFILE) {
            view.phase = LOBBY_PHASE_FOUND;
            snprintf(view.message, sizeof view.message, "%s", profile_message);
        } else if (direct_screen != DIRECT_OFF && directEntryFrame(&view, input)) {
            /* the hub or the keyboard drew the screen and took the input */
            input &= ~(u64) (HSD_PAD_B | PAD_CANCEL);
        } else if (awaiting_rank_result && pc_net_match_publication(NULL) == 0) {
            pc_net_poll();
            pc_rank_session_poll();
            int result = pc_rank_session_state(&why);
            if (result == PC_RANK_SESSION_SAVED) {
                pc_net_match_publish_rank();
            }
            view.phase = result == PC_RANK_SESSION_FAILED ? LOBBY_PHASE_ERROR : LOBBY_PHASE_CONNECTING;
            snprintf(view.message, sizeof view.message, "%s",
                     why ? why : "Finishing signed result...");
            if (result == PC_RANK_SESSION_FAILED)
                snprintf(view.message, sizeof view.message, "%.54s - START: retry",
                         why ? why : "Set could not be rated");
            if (result == PC_RANK_SESSION_FAILED && (input & HSD_PAD_START)) {
                awaiting_rank_result = false;
                pc_net_match_start(PC_MATCH_RANKED, NULL);
            }
        } else if (online_kind == ONLINE_KIND_RANKED && pc_net_match_publication(NULL) != 0) {
            pc_net_match_poll_publication();
            int publication = pc_net_match_publication(&why);
            view.phase = publication < 0 ? LOBBY_PHASE_ERROR :
                         publication == 2 ? LOBBY_PHASE_FOUND : LOBBY_PHASE_CONNECTING;
            snprintf(view.message, sizeof view.message, "%s",
                     publication == 2 ? "Rating saved and published. START: next set" :
                     publication < 0 ? "Rating saved locally. START: retry publication" :
                     "Rating saved. Publishing...");
            if ((input & HSD_PAD_START) && publication != 1) {
                if (publication < 0) pc_net_match_publish_rank();
                else {
                    awaiting_rank_result = false;
                    pc_net_match_start(PC_MATCH_RANKED, NULL);
                }
            }
        } else {
            pc_net_match_poll();
            state = pc_net_match_state(&why);
#ifdef ANDROID
            /* Build cached device shaders while Direct Connect is still waiting. */
            if (online_kind == ONLINE_KIND_DIRECT && state == PC_MATCH_SEARCH)
                pc_gfx_prewarm(8);
#endif
            int reason = pc_net_peer_status();
            view.phase = state == PC_MATCH_READY ? LOBBY_PHASE_STARTING :
                         (state == PC_MATCH_FAIL || reason != PC_NET_PEER_OK) ? LOBBY_PHASE_ERROR :
                         state == PC_MATCH_CONNECT ? LOBBY_PHASE_CONNECTING : LOBBY_PHASE_SEARCHING;
            if (reason != PC_NET_PEER_OK && reason < (int) ARRAY_SIZE(peer_word)) {
                snprintf(view.message, sizeof view.message, "%s - START: search", peer_word[reason]);
            } else if (state == PC_MATCH_SEARCH && why == NULL) {
                /* Every wait shows what it is waiting on and for how long;
                 * "Searching..." alone looked the same whether the network
                 * was down, the friend was away or the code was wrong. */
                PcNetMatchProgress pr;
                pc_net_match_progress(&pr);
                unsigned secs = (unsigned) (pr.elapsed_ms / 1000);
                char who[40];
                directShow(who, sizeof who, direct_calling);
                if (!pr.network_ready) {
                    snprintf(view.message, sizeof view.message,
                             pr.slippi ? "Connecting to Slippi...  %u:%02u" :
                                         "Joining the online network...  %u:%02u",
                             secs / 60, secs % 60);
                } else if (online_kind == ONLINE_KIND_DIRECT && pr.calling) {
                    snprintf(view.message, sizeof view.message,
                             secs < 45 ? "Calling %s  %u:%02u" :
                                         "%s isn't answering yet. Still calling  %u:%02u",
                             who, secs / 60, secs % 60);
                } else if (online_kind == ONLINE_KIND_DIRECT) {
                    snprintf(view.message, sizeof view.message,
                             "Waiting for a friend to call you  %u:%02u", secs / 60, secs % 60);
                } else {
                    snprintf(view.message, sizeof view.message,
                             "Looking for an opponent  %u:%02u", secs / 60, secs % 60);
                }
                if (online_kind == ONLINE_KIND_DIRECT) {
                    char me[40];
                    directShow(me, sizeof me, pc_net_match_local_code());
                    snprintf(view.subtitle, sizeof view.subtitle, "YOU %s", me);
                    view.player_count = 0; /* the subtitle already says who we are */
                    view.hint = "Y: copy my code   B: stop";
                    if (input & HSD_PAD_Y) {
                        pc_net_match_copy_code();
                        sfxForward();
                    }
                }
            } else if (state == PC_MATCH_CONNECT && pc_net_match_is_slippi()) {
                snprintf(view.message, sizeof view.message, "Connecting to %.17s...",
                         pc_net_match_opponent_code());
            } else if (state == PC_MATCH_READY && pc_net_match_is_slippi()) {
                snprintf(view.message, sizeof view.message, "Matched with %.17s - starting...",
                         pc_net_match_opponent_code());
            } else {
                snprintf(view.message, sizeof view.message, "%s", why ? why : "Searching for an opponent...");
            }
            {
                static char last_msg[ONLINE_LOBBY_MSG_LEN];
                /* Logged on change, minus the ticking timer, so a test can
                 * assert what the lobby told the player. */
                if (strncmp(view.message, last_msg, 24) != 0) {
                    snprintf(last_msg, sizeof last_msg, "%s", view.message);
                    pc_log_line("lobby: %s [%s]", view.message, pc_net_match_local_code());
                }
            }
            const char* peer = pc_net_match_opponent_code();
            if (peer && peer[0]) {
                view.player_count = 2;
                lobbyCopyName(view.players[1].name, peer);
                view.players[1].ping_ms = -1;
            }
            if (state == PC_MATCH_READY && pc_net_match_is_slippi()) {
                pc_log_line("lobby: Slippi match found, starting");
                gm_801A4B60(); /* onExitLobby routes to the VS scene */
            } else if (state == PC_MATCH_READY && pc_net_frame() >= pc_net_match_start_frame()) {
                *HSD_RandSeedPtr = pc_net_match_seed();
                pc_log_line("lobby: entering CSS at frame %d, seed %u", pc_net_frame(), pc_net_match_seed());
                gm_801A4B60();
            }
            if ((input & HSD_PAD_START) && (state == PC_MATCH_FAIL || reason != PC_NET_PEER_OK)) {
                pc_net_peer_status_clear();
                /* Retry the mode we are actually in: a direct session used to
                 * restart as public matchmaking, dropping the friend's code. */
                if (online_kind == ONLINE_KIND_DIRECT) {
                    directEntryBegin();
                } else {
                    pc_net_match_start(online_kind == ONLINE_KIND_RANKED ? PC_MATCH_RANKED :
                                       PC_MATCH_UNRANKED, NULL);
                }
            }
        }
        mnOnlineLobby_Update(&view);
        if (input & (HSD_PAD_B | PAD_CANCEL)) {
            sfxBack();
            pc_net_peer_status_clear();
            pc_net_match_stop();
            if (slippiFlow()) {
                /* Cancelling a search returns to the CSS, as in Slippi. */
                slippi_picked = false;
            } else {
                gm_ChangeGameModeAfterCurrentScene(GM_MENU);
            }
            gm_801A4B60();
        }
        return;
    }
    pc_lan_poll();
    state = pc_lan_state(&why);
    n = pc_lan_peers(peers, PC_LAN_MAX_PEERS);
    lobbyFillView(&view, state, why, peers, n);
    mnOnlineLobby_Update(&view);

    if (state == 2) {
        /* Both peers tick in lockstep once connected, so leaving on the
         * agreed frame puts the CSS on the same synced frame everywhere. */
        if (pc_net_frame() >= pc_lan_start_frame()) {
            *HSD_RandSeedPtr = pc_lan_seed();
            pc_log_line("lobby: entering CSS at frame %d, seed %u",
                        pc_net_frame(), pc_lan_seed());
            gm_801A4B60();
        }
        return;
    }
    if (input & (HSD_PAD_B | PAD_CANCEL)) {
        sfxBack();
        pc_net_peer_status_clear();
        pc_lan_stop();
        gm_ChangeGameModeAfterCurrentScene(GM_MENU);
        gm_801A4B60();
    } else if ((input & HSD_PAD_START) && (state == 0 || state == 3) &&
               lobbyCompatible(peers, n)) {
        sfxForward();
        pc_net_peer_status_clear(); /* a retry from 3 is not the last session's */
        pc_lan_start_match();
    }
#endif
}
