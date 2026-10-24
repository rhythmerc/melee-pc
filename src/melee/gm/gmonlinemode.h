#ifndef MELEE_GM_GMONLINEMODE_H
#define MELEE_GM_GMONLINEMODE_H

#include <melee/gm/types.h>

/* PC only: GM_ONLINE. Lobby (GS_ONLINE_LOBBY) -> CSS -> SSS -> VS -> results
 * -> CSS, every scene after the lobby driven by synced inputs
 * (docs/netcode-plan.md §8, §10). */

extern GameModeState gm_Mode_Online_States[];

/* Which online flavour the menu picked before entering GM_ONLINE. */
typedef enum OnlineKind {
    ONLINE_KIND_LAN = 0,
    ONLINE_KIND_DIRECT = 1,
    ONLINE_KIND_UNRANKED = 2,
    ONLINE_KIND_RANKED = 3,
    ONLINE_KIND_PROFILE = 4,
    ONLINE_KIND_LOGIN = 5, /* Slippi sign-in: the keyboard screen, not a match */
    ONLINE_KIND_PLACE = 6, /* XR: the stage-placement scene, then back to the menu */
    ONLINE_KIND_REGISTER = 7, /* Slippi sign-up: the keyboard and pages, not a match */
} OnlineKind;

void gmOnline_SetKind(OnlineKind kind);
OnlineKind gmOnline_GetKind(void);
/* True while a Slippi account is signed in and the mode is Unranked or Direct:
 * the CSS comes first and the search follows (see gmonlinemode.c). */
bool gmOnline_SlippiFlow(void);
/* After a placement scene started by Unranked or Direct on a launch with no
 * placement yet, the mode goes on to that one instead of back to the menu. */
void gmOnline_SetKindAfterPlace(OnlineKind kind);
/* XR: the stage-placement scene, then on to game mode `mode` (a
 * GameModeKind: the menu's fight mode that asked for it). */
void gmOnline_SetModeAfterPlace(int mode);
/* The placement scene was opened from Options: the menu goes back there.
 * Taken (and cleared) by the menu as it comes back. */
void gmOnline_SetPlaceFromOptions(bool from_options);
bool gmOnline_TakePlaceFromOptions(void);

/* The online CSS's status text (src/melee/mn/mnonlinecss.c), drawn by the
 * Slippi flow's state in gmonlinemode.c. */
void mnOnlineCss_Create(int ctx);
#define CSS_STEPS_MAX 3
typedef struct OnlineCssStep {
    char mark;      /* '-' done, '+' to do, 'x' in progress, '!' error */
    char text[56];
} OnlineCssStep;
/* Direct shows the player's own connect code under the name; "" hides it. */
void mnOnlineCss_SetCode(const char* code);
/* "Playing: <name>" across the top while connected; "" hides it. */
void mnOnlineCss_SetPlaying(const char* name);
void mnOnlineCss_Update(const char* title, const char* user, const OnlineCssStep* steps, int n,
                        const char* hint);
void mnOnlineCss_Destroy(void);
/* Called every CSS frame while the Slippi flow owns the screen; true when the
 * match is agreed and the CSS should leave now. */
bool gmOnline_SlippiCssFrame(void);
/* START was pressed with a valid pick: true if the flow handled it (the CSS
 * stays up and locks) rather than leaving the scene. */
bool gmOnline_SlippiCssStart(int character, int color);
/* True while the CSS must ignore the controller (a search is running). */
bool gmOnline_SlippiCssLocked(void);

/* Where a point (640 x 480 logical pixels, as the pointer reports it) lands on
 * the lobby's key grid: the key's index, 100 + n for the nth button, or -1. */
int mnOnlineLobby_KeyHit(float x, float y);

void gm_Scene_OnlineLobby_OnFrame(void);
void gm_Scene_OnlineLobby_OnEnter(void*);
void gm_Scene_OnlineLobby_OnExit(void*);

/* ---- lobby view (src/melee/mn/mnonlinelobby.c) --------------------------
 * The lobby scene owns the flow and the network; the view only draws. The
 * scene fills an OnlineLobbyView every frame and calls mnOnlineLobby_Update. */

#define ONLINE_LOBBY_MAX_PLAYERS 8
#define ONLINE_LOBBY_NAME_LEN 16
#define ONLINE_LOBBY_MSG_LEN 96

typedef struct OnlineLobbyPlayer {
    char name[ONLINE_LOBBY_NAME_LEN];
    int ping_ms;   /* -1 = unknown */
    bool is_host;
    bool is_local;
    bool incompatible; /* other protocol/build; listed, never counted */
} OnlineLobbyPlayer;

typedef enum OnlineLobbyPhase {
    LOBBY_PHASE_SEARCHING,  /* announcing, nobody found yet */
    LOBBY_PHASE_FOUND,      /* players listed; Start begins */
    LOBBY_PHASE_CONNECTING, /* host elected, session connecting */
    LOBBY_PHASE_STARTING,   /* handshake done, counting down to CSS */
    LOBBY_PHASE_ERROR,      /* message explains; B goes back */
} OnlineLobbyPhase;

/* Direct connect's own screens, drawn in place of the player list. */
#define ONLINE_LOBBY_MENU_ROWS 8
#define ONLINE_LOBBY_KEY_ROWS 6
#define ONLINE_LOBBY_KEY_BUTTONS 3
#define ONLINE_LOBBY_KEY_COLS 8

typedef enum OnlineLobbyScreen {
    LOBBY_SCREEN_PLAYERS, /* the player list (LAN, searching, connecting) */
    LOBBY_SCREEN_MENU,    /* a list of choices with a cursor */
    LOBBY_SCREEN_KEYS,    /* the code keyboard */
} OnlineLobbyScreen;

typedef struct OnlineLobbyView {
    const char* title;                 /* "LAN PLAY" / "DIRECT CONNECT" */
    OnlineLobbyScreen screen;
    char subtitle[ONLINE_LOBBY_MSG_LEN]; /* top right: e.g. your own code */
    /* MENU: rows and the cursor; KEYS: `menu[0]` is the entry line. */
    char menu[ONLINE_LOBBY_MENU_ROWS][ONLINE_LOBBY_MSG_LEN];
    int menu_count;
    int cursor;
    const char* keys; /* KEYS: ROWS*COLS characters, row-major */
    int key_cursor;
    /* KEYS: buttons under the grid for what has no key (Shift, Show, Next), so
     * a pointer can press them; a NULL label is not drawn. */
    const char* key_buttons[ONLINE_LOBBY_KEY_BUTTONS];
    int key_button_hot; /* the one the pointer is over, or -1 */
    const char* hint; /* the button line, or NULL for the default */
    OnlineLobbyPhase phase;
    OnlineLobbyPlayer players[ONLINE_LOBBY_MAX_PLAYERS];
    int player_count;                  /* includes the local player */
    char message[ONLINE_LOBBY_MSG_LEN]; /* one status line, may be "" */
    const char* link;                  /* quality word by the ping, or NULL */
    int countdown_frames;              /* STARTING only, else 0 */
} OnlineLobbyView;

void mnOnlineLobby_Create(void);
void mnOnlineLobby_Update(const OnlineLobbyView* view);
void mnOnlineLobby_Destroy(void);

#endif
