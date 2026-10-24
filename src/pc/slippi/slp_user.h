/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi account identity for melee-pc.
 *
 * The Slippi ecosystem identifies a player by four fields carried in a
 * `user.json` (uid, playKey, connectCode, displayName, plus latestVersion and
 * 16 chat messages). Desktop Slippi gets that file written by the Slippi
 * Launcher, which logs in with Firebase and fetches the fields from Slippi's
 * GraphQL backend. melee-pc's primary target is Quest 3, where the Launcher
 * cannot run at all, so reading the Launcher's file can only ever be a
 * convenience: this module owns its own account store, and the Launcher file
 * is a discovery fallback for a desktop that happens to have one.
 *
 * Storage is a JSON file of our own (`<dir>/slippi-account.json`), written by
 * whatever login flow the platform provides. On-device that is our own UI
 * (email/password -> Slippi GraphQL -> fields); on desktop it may also be a
 * copy of a discovered user.json. The file format matches user.json exactly so
 * one reader serves both, and a user.json dropped in our directory works
 * without conversion.
 *
 * This module is pure where it can be: parsing account JSON is testable with no
 * filesystem or network (pc_slippi_account_from_json). Discovery, load, save
 * and the REST refresh are the I/O edges. */
#ifndef PC_SLIPPI_SLP_USER_H
#define PC_SLIPPI_SLP_USER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Slippi's own chat message count; the wire and the account both use 16. */
#define PC_SLIPPI_CHAT_MESSAGES 16
/* Field bounds. Slippi display names and the playKey can be long, so these are
 * generous; a value that still does not fit is dropped whole (the JSON reader
 * never returns a truncated string as success) rather than half-copied. */
#define PC_SLIPPI_DISPLAY_NAME_MAX 64
#define PC_SLIPPI_CONNECT_CODE_MAX 32
#define PC_SLIPPI_UID_MAX 128
#define PC_SLIPPI_PLAY_KEY_MAX 512
#define PC_SLIPPI_CHAT_MESSAGE_MAX 128

typedef struct PcSlippiAccount {
    char uid[PC_SLIPPI_UID_MAX];
    char play_key[PC_SLIPPI_PLAY_KEY_MAX];
    char display_name[PC_SLIPPI_DISPLAY_NAME_MAX];
    char connect_code[PC_SLIPPI_CONNECT_CODE_MAX];
    char latest_version[32];
    char chat_messages[PC_SLIPPI_CHAT_MESSAGES][PC_SLIPPI_CHAT_MESSAGE_MAX];
    /* True once any field was loaded (a stored file or a discovered user.json);
     * a logged-out account is all-empty and logged_in false. */
    bool logged_in;
} PcSlippiAccount;

/* Slippi's stock chat messages, used when an account carries none. */
void pc_slippi_account_default_chat(PcSlippiAccount* a);

/* Parse an account from JSON text (user.json or our own store; same shape).
 * Returns false if the text is not an object or uid/playKey/connectCode are
 * all empty (a file with none of them cannot play). Never throws. */
bool pc_slippi_account_from_json(const char* text, size_t len, PcSlippiAccount* out);

/* Serialize to JSON (the same shape user.json uses). Returns the length, or 0
 * if the buffer is too small. Always NUL-terminates when cap > 0. */
size_t pc_slippi_account_to_json(const PcSlippiAccount* a, char* out, size_t cap);

/* True for a Slippi connect code as typed in Direct: a tag of 1-7 characters
 * from A-Z and 0-9, a '#', then 1-3 digits ("ABCD#123"). Upper case only; use
 * pc_slippi_connect_code_normalize() first for user input. */
bool pc_slippi_connect_code_valid(const char* code);

/* Trim and upper-case `in` into `out`, and report whether the result is a
 * valid connect code. `out` is filled either way (best effort). */
bool pc_slippi_connect_code_normalize(const char* in, char* out, size_t cap);

/* The directory melee-pc keeps its Slippi files in. Resolution order:
 *   1. $MELEE_SLIPPI_DIR, when set (tests, and a player who moved it);
 *   2. <SDL pref dir>/Slippi, on desktop and Quest alike.
 * This is where our own store and the direct-codes file live. */
const char* pc_slippi_user_dir(void);

/* Load our own store. False when absent or unreadable; *out is cleared first.
 * `rediscover` also rescans the Launcher folders (see below). */
bool pc_slippi_account_load(PcSlippiAccount* out, bool rediscover);

/* Save to our own store (creating the directory if needed). */
bool pc_slippi_account_save(const PcSlippiAccount* a);

/* Remove our own store (logout). The account is left blank by the caller. */
void pc_slippi_account_forget(void);

/* Find a Slippi Launcher `user.json` on this machine and copy it into `out`.
 * This is the desktop convenience path only: it never runs on a platform
 * without a Launcher (Quest), where it just returns false. Searches the
 * documented Launcher folders and the Dolphin build paths its settings name.
 * Returns false when nothing was found or the file had no usable fields. */
bool pc_slippi_account_discover(PcSlippiAccount* out);

/* Refresh display name / connect code / latest version / chat messages from
 * Slippi's REST API (users.slippi.gg/user/<uid>) on a background thread. This
 * is optional: the stored fields are already usable without it, and this only
 * corrects a name changed on the website since login. No-op when not logged in.
 * `version_newer` receives true when the server's latestVersion is newer than
 * PC_SLIPPI_SEMVER, so the caller can warn that online play needs an update. */
void pc_slippi_account_refresh_async(const PcSlippiAccount* a);

/* HTTP fetch used by the refresh and the login flow. Returns true and fills
 * `out` (NUL-terminated, up to cap) with the body on HTTP 200. `bearer` may be
 * NULL. Implemented in slp_http.c (curl), so the account logic stays free of
 * the TLS dependency. */
bool pc_slippi_http_get(const char* url, const char* bearer, char* out, size_t cap, int* status);
bool pc_slippi_http_post_json(const char* url, const char* body, const char* bearer, char* out,
                              size_t cap, int* status);
/* POST `len` bytes of any type, for the launcher's log upload (src/pc/log_report.cpp):
 * curl, java.net on Android, WinHTTP on Windows. True when a response came
 * back (any status, in `status`), with its body in `out`. */
bool pc_http_post(const char* url, const char* content_type, const char* body, size_t len, char* out,
                  size_t cap, int* status);

/* ---- login (launcher auth surface) ----------------------------------- */

/* True when the login flow is configured: the Firebase API key and the Slippi
 * GraphQL endpoint are both set (MELEE_SLIPPI_FIREBASE_KEY,
 * MELEE_SLIPPI_GRAPHQL_ENDPOINT). When false the launcher offers Direct with a
 * manual code instead of Unranked. */
bool pc_slippi_account_login_available(void);

/* Log in with a Slippi (Firebase) email/password and fetch the account fields.
 * This is blocking (several HTTPS round trips), so call it off the main thread.
 * Returns true and fills `out` on success; on failure returns false and writes
 * a human-readable reason into `err` (up to err_cap). On success the account is
 * saved to our store. An account that still needs setting up (below) fails
 * with a message pointing at the game's Slippi pages. */
bool pc_slippi_account_login(const char* email, const char* password, PcSlippiAccount* out,
                             char* err, size_t err_cap);

/* ---- account setup ----------------------------------------------------
 * What the Slippi Launcher's quick start does, for a player with no Launcher:
 * make an account, then, only for what is missing, verify the email, accept
 * Slippi's online rules and choose a connect code. Every call is blocking
 * HTTPS (run it off the main thread) and returns false with a reason in `err`.
 * The session holds the Firebase tokens between steps, in memory only; the
 * password is never kept. pc_slippi_setup_end wipes it. */

/* Slippi's current rules version (the Launcher's currentRulesVersion). */
#define PC_SLIPPI_RULES_VERSION 1

typedef enum PcSlippiSetupStep {
    PC_SLIPPI_SETUP_DONE,         /* playable; the account is saved */
    PC_SLIPPI_SETUP_VERIFY_EMAIL, /* the email address is not verified */
    PC_SLIPPI_SETUP_RULES,        /* the rules are not accepted */
    PC_SLIPPI_SETUP_CONNECT_CODE, /* no connect code yet */
} PcSlippiSetupStep;

typedef struct PcSlippiSetup {
    char uid[PC_SLIPPI_UID_MAX];
    char id_token[4096];
    char refresh_token[1024];
    char email[256];
    char display_name[PC_SLIPPI_DISPLAY_NAME_MAX];
    bool email_verified;
} PcSlippiSetup;

bool pc_slippi_setup_sign_in(PcSlippiSetup* s, const char* email, const char* password, char* err,
                             size_t cap);
/* Make an account with the Launcher's sign-up function, then sign in. */
bool pc_slippi_setup_register(PcSlippiSetup* s, const char* name, const char* email, const char* password,
                              char* err, size_t cap);
/* Fresh tokens and the account's state: the next step, or DONE with the
 * account saved and in `out`. */
bool pc_slippi_setup_check(PcSlippiSetup* s, PcSlippiSetupStep* step, PcSlippiAccount* out, char* err,
                           size_t cap);
bool pc_slippi_setup_send_verification(PcSlippiSetup* s, char* err, size_t cap);
/* Slippi's password reset email, to `email`; no session needed. */
bool pc_slippi_setup_send_password_reset(const char* email, char* err, size_t cap);
bool pc_slippi_setup_accept_rules(PcSlippiSetup* s, char* err, size_t cap);
/* `tag` is the code's start, 2-4 capitals; Slippi adds the "#123". */
bool pc_slippi_setup_choose_code(PcSlippiSetup* s, const char* tag, char* err, size_t cap);
void pc_slippi_setup_end(PcSlippiSetup* s);

/* The Launcher's rules for a display name (1-15 printable characters, no \
 * or `) and a code's start (2-4 of A-Z), and its suggested start: the name's
 * first four letters, upper-cased. */
bool pc_slippi_display_name_valid(const char* name);
bool pc_slippi_code_start_valid(const char* tag);
void pc_slippi_code_start_suggest(const char* display_name, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
