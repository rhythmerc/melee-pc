/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi account identity (slp_user.h). The JSON parsing and serialization are
 * pure; discovery, load, save and the REST refresh are the I/O edges. */
#include "pc/slippi/slp_user.h"

#include "pc/slippi/slp_json.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---------------------------------------------------------- */

static void copy_field(char* dst, size_t cap, const char* src) {
    if (cap == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* True when the account has the fields a Slippi session needs. connectCode is
 * what the servers key on for Direct; a login without one cannot play. */
static bool account_usable(const PcSlippiAccount* a) {
    return a->uid[0] != '\0' || a->connect_code[0] != '\0' || a->play_key[0] != '\0';
}

void pc_slippi_account_default_chat(PcSlippiAccount* a) {
    static const char* const defaults[PC_SLIPPI_CHAT_MESSAGES] = {
        "ggs", "one more", "brb", "good luck", "well played", "that was fun", "thanks", "too good",
        "sorry", "my b", "lol", "wow", "gotta go", "one sec", "let's play again later",
        "bad connection",
    };
    for (int i = 0; i < PC_SLIPPI_CHAT_MESSAGES; i++) {
        copy_field(a->chat_messages[i], sizeof a->chat_messages[i], defaults[i]);
    }
}

static void account_clear(PcSlippiAccount* a) {
    memset(a, 0, sizeof *a);
    pc_slippi_account_default_chat(a);
}

/* Fill the chat slots from a JSON array of strings, or the defaults when it is
 * missing or not exactly 16 entries (as Slippi's sanitize does). */
static void read_chat(const PcSlippiJson* obj, PcSlippiAccount* out) {
    const PcSlippiJson* cm = pc_slippi_json_get(obj, "chatMessages");
    if (!pc_slippi_json_is_array(cm) || cm->child_count != PC_SLIPPI_CHAT_MESSAGES) {
        pc_slippi_account_default_chat(out);
        return;
    }
    for (int i = 0; i < PC_SLIPPI_CHAT_MESSAGES; i++) {
        const PcSlippiJson* e = pc_slippi_json_at(cm, i);
        if (!pc_slippi_json_string(e, out->chat_messages[i], sizeof out->chat_messages[i])) {
            pc_slippi_account_default_chat(out);
            return;
        }
    }
}

/* A short decoded-string helper: decode `key` from `obj` into a fixed buffer,
 * falling back to empty. */
static void get_str(const PcSlippiJson* obj, const char* key, char* out, size_t cap) {
    if (!pc_slippi_json_string(pc_slippi_json_get(obj, key), out, cap)) {
        if (cap > 0) {
            out[0] = '\0';
        }
    }
}

bool pc_slippi_account_from_json(const char* text, size_t len, PcSlippiAccount* out) {
    account_clear(out);
    if (text == NULL) {
        return false;
    }
    PcSlippiJson* root = NULL;
    char err[64];
    if (!pc_slippi_json_parse(text, len, &root, err, sizeof err)) {
        return false;
    }
    if (root->type != PC_SLIPPI_JSON_OBJECT) {
        pc_slippi_json_free(root);
        return false;
    }
    get_str(root, "uid", out->uid, sizeof out->uid);
    get_str(root, "playKey", out->play_key, sizeof out->play_key);
    get_str(root, "displayName", out->display_name, sizeof out->display_name);
    get_str(root, "connectCode", out->connect_code, sizeof out->connect_code);
    get_str(root, "latestVersion", out->latest_version, sizeof out->latest_version);
    read_chat(root, out);
    pc_slippi_json_free(root);
    out->logged_in = account_usable(out);
    return out->logged_in;
}

/* Escape a string into a JSON literal, returning false when it does not fit.
 * Account fields are short and ASCII in practice; control bytes are escaped so
 * a display name can never break the file. */
static bool json_escape(const char* s, char* out, size_t cap, size_t* w) {
    for (const unsigned char* p = (const unsigned char*)s; *p != '\0'; p++) {
        char esc[8];
        const char* add = NULL;
        switch (*p) {
        case '"': add = "\\\""; break;
        case '\\': add = "\\\\"; break;
        case '\n': add = "\\n"; break;
        case '\r': add = "\\r"; break;
        case '\t': add = "\\t"; break;
        default:
            if (*p < 0x20) {
                snprintf(esc, sizeof esc, "\\u%04x", *p);
                add = esc;
            }
            break;
        }
        if (add == NULL) {
            if (*w + 1 >= cap) {
                return false;
            }
            out[(*w)++] = (char)*p;
        } else {
            size_t n = strlen(add);
            if (*w + n >= cap) {
                return false;
            }
            memcpy(out + *w, add, n);
            *w += n;
        }
    }
    return true;
}

size_t pc_slippi_account_to_json(const PcSlippiAccount* a, char* out, size_t cap) {
    if (out == NULL || cap == 0) {
        return 0;
    }
    out[0] = '\0';
    size_t w = 0;
    bool ok = true;
/* One `"key":"value",` member, escaping the value. */
#define MEMBER(key, field)                                                                    \
    do {                                                                                     \
        int n = snprintf(out + w, cap - w, "\"" key "\":");                                  \
        if (n < 0 || (size_t)n >= cap - w) {                                                 \
            ok = false;                                                                      \
            break;                                                                           \
        }                                                                                    \
        w += (size_t)n;                                                                      \
        if (w + 1 >= cap) {                                                                  \
            ok = false;                                                                      \
            break;                                                                           \
        }                                                                                    \
        out[w++] = '"';                                                                      \
        if (!json_escape((field), out, cap - 1, &w)) {                                       \
            ok = false;                                                                      \
            break;                                                                           \
        }                                                                                    \
        if (w + 2 >= cap) {                                                                  \
            ok = false;                                                                      \
            break;                                                                           \
        }                                                                                    \
        out[w++] = '"';                                                                      \
        out[w++] = ',';                                                                      \
    } while (0)
    if (w + 1 >= cap) {
        ok = false;
    } else {
        out[w++] = '{';
    }
    if (ok) MEMBER("uid", a->uid);
    if (ok) MEMBER("playKey", a->play_key);
    if (ok) MEMBER("displayName", a->display_name);
    if (ok) MEMBER("connectCode", a->connect_code);
    if (ok) MEMBER("latestVersion", a->latest_version);
    if (ok) {
        /* chatMessages: an array of 16 strings. Slippi's shape. */
        int n = snprintf(out + w, cap - w, "\"chatMessages\":[");
        if (n < 0 || (size_t)n >= cap - w) {
            ok = false;
        } else {
            w += (size_t)n;
        }
    }
    for (int i = 0; ok && i < PC_SLIPPI_CHAT_MESSAGES; i++) {
        if (w + 1 >= cap) {
            ok = false;
            break;
        }
        out[w++] = '"';
        if (!json_escape(a->chat_messages[i], out, cap - 1, &w)) {
            ok = false;
            break;
        }
        if (w + 2 >= cap) {
            ok = false;
            break;
        }
        out[w++] = '"';
        out[w++] = (i + 1 < PC_SLIPPI_CHAT_MESSAGES) ? ',' : ']';
    }
    if (ok && w + 1 < cap) {
        out[w++] = '}';
        out[w] = '\0';
    } else {
        ok = false;
    }
    if (!ok) {
        out[0] = '\0';
        return 0;
    }
    return w;
#undef MEMBER
}

/* ---- directory / store ------------------------------------------------ */

const char* pc_slippi_user_dir(void) {
    static char dir[1024];
    static bool done;
    if (!done) {
        done = true;
        const char* override = getenv("MELEE_SLIPPI_DIR");
        if (override != NULL && override[0] != '\0') {
            snprintf(dir, sizeof dir, "%s", override);
        } else {
            char* pref = SDL_GetPrefPath(NULL, "melee-pc");
            if (pref != NULL) {
                snprintf(dir, sizeof dir, "%sSlippi", pref);
                SDL_free(pref);
            } else {
                snprintf(dir, sizeof dir, "Slippi");
            }
        }
    }
    return dir;
}

static bool read_file(const char* path, char* out, size_t cap, size_t* len) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = '\0';
    *len = n;
    return true;
}

bool pc_slippi_connect_code_valid(const char* code) {
    size_t i = 0;
    size_t tag = 0;
    size_t digits = 0;
    if (code == NULL) {
        return false;
    }
    for (; code[i] != '\0' && code[i] != '#'; i++, tag++) {
        char c = code[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    if (tag < 1 || tag > 7 || code[i] != '#') {
        return false;
    }
    for (i++; code[i] != '\0'; i++, digits++) {
        if (code[i] < '0' || code[i] > '9') {
            return false;
        }
    }
    return digits >= 1 && digits <= 3;
}

bool pc_slippi_connect_code_normalize(const char* in, char* out, size_t cap) {
    size_t n = 0;
    if (cap == 0) {
        return false;
    }
    out[0] = '\0';
    if (in == NULL) {
        return false;
    }
    while (*in == ' ' || *in == '\t' || *in == '\r' || *in == '\n') {
        in++;
    }
    for (; *in != '\0' && n + 1 < cap; in++) {
        char c = *in;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            break; /* trailing text after the code is not part of it */
        }
        out[n++] = (c >= 'a' && c <= 'z') ? (char) (c - 'a' + 'A') : c;
    }
    out[n] = '\0';
    return pc_slippi_connect_code_valid(out);
}

/* Signing out has to stick: without a marker the next load would find the
 * Launcher's own user.json again and sign straight back in. */
static void signed_out_marker_path(char* out, size_t cap) {
    snprintf(out, cap, "%s/signed-out", pc_slippi_user_dir());
}

static bool signed_out_marker_exists(void) {
    char path[1200];
    signed_out_marker_path(path, sizeof path);
    FILE* f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

bool pc_slippi_account_load(PcSlippiAccount* out, bool rediscover) {
    account_clear(out);
    char path[1200];
    snprintf(path, sizeof path, "%s/slippi-account.json", pc_slippi_user_dir());
    char buf[16384];
    size_t len = 0;
    if (read_file(path, buf, sizeof buf, &len) && pc_slippi_account_from_json(buf, len, out)) {
        return true;
    }
    if (rediscover && !signed_out_marker_exists()) {
        return pc_slippi_account_discover(out);
    }
    return false;
}

bool pc_slippi_account_save(const PcSlippiAccount* a) {
    const char* dir = pc_slippi_user_dir();
    SDL_CreateDirectory(dir);
    char path[1200];
    snprintf(path, sizeof path, "%s/slippi-account.json", dir);
    char buf[32768];
    size_t n = pc_slippi_account_to_json(a, buf, sizeof buf);
    if (n == 0) {
        return false;
    }
    FILE* f = fopen(path, "wb");
    if (f == NULL) {
        return false;
    }
    bool ok = fwrite(buf, 1, n, f) == n;
    fclose(f);
    if (ok) {
        char marker[1200];
        signed_out_marker_path(marker, sizeof marker);
        remove(marker); /* signing in again lifts a sign-out */
    }
    return ok;
}

void pc_slippi_account_forget(void) {
    char path[1200];
    snprintf(path, sizeof path, "%s/slippi-account.json", pc_slippi_user_dir());
    remove(path);
    SDL_CreateDirectory(pc_slippi_user_dir());
    char marker[1200];
    signed_out_marker_path(marker, sizeof marker);
    FILE* f = fopen(marker, "wb");
    if (f != NULL) {
        fclose(f);
    }
}

/* ---- Launcher discovery (desktop convenience) ------------------------- */

/* Try one candidate user.json path. Returns true and fills `out` on success. */
static bool try_user_json(const char* path, PcSlippiAccount* out) {
    char buf[16384];
    size_t len = 0;
    if (!read_file(path, buf, sizeof buf, &len)) {
        return false;
    }
    return pc_slippi_account_from_json(buf, len, out);
}

/* The documented Launcher layout under a root: <root>/<build>/User/Slippi/
 * user.json. `build` names the Dolphin folder the Launcher installs. */
static bool try_root(const char* root, PcSlippiAccount* out) {
    if (root == NULL || root[0] == '\0') {
        return false;
    }
    static const char* const builds[] = {"netplay", "netplay-beta", "playback"};
    char path[1400];
    for (size_t i = 0; i < sizeof builds / sizeof builds[0]; i++) {
        snprintf(path, sizeof path, "%s/%s/User/Slippi/user.json", root, builds[i]);
        if (try_user_json(path, out)) {
            return true;
        }
    }
    return false;
}

bool pc_slippi_account_discover(PcSlippiAccount* out) {
    account_clear(out);
    /* A user.json dropped directly in our own directory wins: it is explicit
     * and needs no Launcher. */
    char own[1200];
    snprintf(own, sizeof own, "%s/user.json", pc_slippi_user_dir());
    if (try_user_json(own, out)) {
        return true;
    }
    /* Standard Launcher roots. APPDATA/LOCALAPPDATA on Windows; on Linux and
     * macOS the Launcher uses a Slippi Launcher folder under the platform's
     * app-data dir. Everything is best-effort and silently absent when there
     * is no Launcher (Quest, a fresh desktop). */
    const char* roots[8];
    int n = 0;
    const char* appdata = getenv("APPDATA");
    const char* localappdata = getenv("LOCALAPPDATA");
    const char* xdg = getenv("XDG_CONFIG_HOME");
    const char* home = getenv("HOME");
    char win1[1200], win2[1200], lin[1200], mac[1200], xdgRoot[1200];
    if (appdata != NULL && appdata[0] != '\0') {
        snprintf(win1, sizeof win1, "%s/Slippi Launcher", appdata);
        roots[n++] = win1;
    }
    if (localappdata != NULL && localappdata[0] != '\0') {
        snprintf(win2, sizeof win2, "%s/Slippi Launcher", localappdata);
        roots[n++] = win2;
    }
    if (home != NULL && home[0] != '\0') {
        snprintf(lin, sizeof lin, "%s/.config/Slippi Launcher", home);
        roots[n++] = lin;
        snprintf(mac, sizeof mac, "%s/Library/Application Support/Slippi Launcher", home);
        roots[n++] = mac;
    }
    if (xdg != NULL && xdg[0] != '\0') {
        snprintf(xdgRoot, sizeof xdgRoot, "%s/Slippi Launcher", xdg);
        roots[n++] = xdgRoot;
    }
    for (int i = 0; i < n; i++) {
        if (try_root(roots[i], out)) {
            return true;
        }
    }
    /* Linux: the Launcher's netplay Dolphin is an AppImage that keeps its
     * user folder at ~/.config/SlippiOnline (or $XDG_CONFIG_HOME), outside the
     * Launcher's own folder. */
    char appimage[1400];
    if (xdg != NULL && xdg[0] != '\0') {
        snprintf(appimage, sizeof appimage, "%s/SlippiOnline/Slippi/user.json", xdg);
        if (try_user_json(appimage, out)) {
            return true;
        }
    }
    if (home != NULL && home[0] != '\0') {
        snprintf(appimage, sizeof appimage, "%s/.config/SlippiOnline/Slippi/user.json", home);
        if (try_user_json(appimage, out)) {
            return true;
        }
    }
    return false;
}

/* ---- REST refresh ----------------------------------------------------- */

typedef struct RefreshArgs {
    PcSlippiAccount snapshot;
} RefreshArgs;

static int refresh_thread(void* user) {
    RefreshArgs* args = (RefreshArgs*)user;
    const char* uid = args->snapshot.uid;
    if (uid[0] == '\0') {
        free(args);
        return 0;
    }
    char url[256];
    snprintf(url, sizeof url, "https://users.slippi.gg/user/%s", uid);
    char* body = (char*)malloc(16384);
    if (body == NULL) {
        free(args);
        return 0;
    }
    int status = 0;
    if (pc_slippi_http_get(url, NULL, body, 16384, &status) && status == 200) {
        /* Patch the display name / connect code / latest version / chat from
         * the server's copy; the stored file's are what we keep on failure. A
         * name changed on the website is the point of this refresh. */
        PcSlippiJson* root = NULL;
        char err[64];
        if (pc_slippi_json_parse(body, strlen(body), &root, err, sizeof err)) {
            PcSlippiAccount* fresh = (PcSlippiAccount*)malloc(sizeof *fresh);
            if (fresh != NULL) {
                *fresh = args->snapshot;
                char name[PC_SLIPPI_DISPLAY_NAME_MAX];
                char code[PC_SLIPPI_CONNECT_CODE_MAX];
                char latest[32];
                if (pc_slippi_json_string(pc_slippi_json_get(root, "displayName"), name,
                                          sizeof name)
                    && name[0] != '\0') {
                    copy_field(fresh->display_name, sizeof fresh->display_name, name);
                }
                if (pc_slippi_json_string(pc_slippi_json_get(root, "connectCode"), code,
                                          sizeof code)
                    && code[0] != '\0') {
                    copy_field(fresh->connect_code, sizeof fresh->connect_code, code);
                }
                if (pc_slippi_json_string(pc_slippi_json_get(root, "latestVersion"), latest,
                                          sizeof latest)
                    && latest[0] != '\0') {
                    copy_field(fresh->latest_version, sizeof fresh->latest_version, latest);
                }
                const PcSlippiJson* cm = pc_slippi_json_get(root, "chatMessages");
                if (pc_slippi_json_is_array(cm) && cm->child_count == PC_SLIPPI_CHAT_MESSAGES) {
                    for (int i = 0; i < PC_SLIPPI_CHAT_MESSAGES; i++) {
                        pc_slippi_json_string(pc_slippi_json_at(cm, i), fresh->chat_messages[i],
                                              sizeof fresh->chat_messages[i]);
                    }
                }
                pc_slippi_account_save(fresh);
                free(fresh);
            }
            pc_slippi_json_free(root);
        }
    }
    free(body);
    free(args);
    return 0;
}

void pc_slippi_account_refresh_async(const PcSlippiAccount* a) {
    if (a == NULL || a->uid[0] == '\0') {
        return;
    }
    /* Local test peering must never touch Slippi's servers. */
    const char* local = getenv("MELEE_SLIPPI_LOCAL_PEER");
    if (local != NULL && local[0] == '1') {
        return;
    }
    RefreshArgs* args = (RefreshArgs*)malloc(sizeof *args);
    if (args == NULL) {
        return;
    }
    args->snapshot = *a;
    SDL_Thread* t = SDL_CreateThread(refresh_thread, "slippi refresh", args);
    if (t == NULL) {
        free(args);
        return;
    }
    SDL_DetachThread(t);
}

/* ---- login (launcher auth surface) ----------------------------------- */

/* The Firebase identity toolkit endpoint. Sign-in is one POST that returns an
 * ID token and the Firebase uid; both are needed for the GraphQL query. This
 * is exactly the call the Slippi Launcher's Firebase SDK makes underneath. */
#define PC_SLIPPI_FIREBASE_SIGNIN \
    "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=%s"

/* The account fields the GraphQL query asks for, with $fbUid substituted. */
#define PC_SLIPPI_GET_USER_QUERY                                                              \
    "{\"query\":\"query getUserKeyQuery($fbUid: String) { getUser(fbUid: $fbUid) { "         \
    "fbUid displayName connectCode { code } private { playKey } } getLatestDolphin { "        \
    "version } }\",\"variables\":{\"fbUid\":\"%s\"}}"

/* Release builds compile these in (CMakeLists.txt: from the configure
 * environment, CI's repository secrets); the environment still overrides, for
 * testing. Empty: no in-app login. */
#ifndef PC_SLIPPI_FIREBASE_KEY_BUILTIN
#define PC_SLIPPI_FIREBASE_KEY_BUILTIN ""
#endif
#ifndef PC_SLIPPI_GRAPHQL_ENDPOINT_BUILTIN
#define PC_SLIPPI_GRAPHQL_ENDPOINT_BUILTIN ""
#endif

static const char* login_setting(const char* name, const char* builtin) {
    const char* v = getenv(name);
    return v != NULL && v[0] != '\0' ? v : builtin;
}

bool pc_slippi_account_login_available(void) {
    const char* key = login_setting("MELEE_SLIPPI_FIREBASE_KEY", PC_SLIPPI_FIREBASE_KEY_BUILTIN);
    const char* gql = login_setting("MELEE_SLIPPI_GRAPHQL_ENDPOINT", PC_SLIPPI_GRAPHQL_ENDPOINT_BUILTIN);
    return key != NULL && key[0] != '\0' && gql != NULL && gql[0] != '\0';
}

bool pc_slippi_account_login(const char* email, const char* password, PcSlippiAccount* out,
                             char* err, size_t err_cap) {
    if (out != NULL) {
        account_clear(out);
    }
    if (err != NULL && err_cap > 0) {
        err[0] = '\0';
    }
    const char* key = login_setting("MELEE_SLIPPI_FIREBASE_KEY", PC_SLIPPI_FIREBASE_KEY_BUILTIN);
    const char* gql = login_setting("MELEE_SLIPPI_GRAPHQL_ENDPOINT", PC_SLIPPI_GRAPHQL_ENDPOINT_BUILTIN);
    if (key == NULL || key[0] == '\0' || gql == NULL || gql[0] == '\0') {
        if (err != NULL && err_cap > 0) {
            snprintf(err, err_cap, "Online login is not configured in this build");
        }
        return false;
    }
    if (email == NULL || email[0] == '\0' || password == NULL || password[0] == '\0') {
        if (err != NULL && err_cap > 0) {
            snprintf(err, err_cap, "Enter your Slippi email and password");
        }
        return false;
    }

    /* 1. Firebase sign-in -> ID token + uid. */
    char url[512];
    int n = snprintf(url, sizeof url, PC_SLIPPI_FIREBASE_SIGNIN, key);
    char em[256], pw[256];
    if (n < 0 || (size_t)n >= sizeof url || !pc_slippi_json_escape(email, em, sizeof em) ||
        !pc_slippi_json_escape(password, pw, sizeof pw)) {
        if (err != NULL && err_cap > 0) {
            snprintf(err, err_cap, "Login credentials are too long");
        }
        return false;
    }
    char body[768];
    snprintf(body, sizeof body,
             "{\"email\":\"%s\",\"password\":\"%s\",\"returnSecureToken\":true}", em, pw);
    char* resp = (char*)malloc(16384);
    if (resp == NULL) {
        if (err != NULL && err_cap > 0) {
            snprintf(err, err_cap, "Out of memory");
        }
        return false;
    }
    /* The API key is a URL parameter and the body is a secret pair; neither is
     * ever logged here. */
    int status = 0;
    if (!pc_slippi_http_post_json(url, body, NULL, resp, 16384, &status) || status != 200) {
        if (err != NULL && err_cap > 0) {
            snprintf(err, err_cap, status == 400 ? "Wrong email or password"
                                                 : "Could not reach Slippi login");
        }
        free(resp);
        return false;
    }
    char id_token[4096];
    char fb_uid[PC_SLIPPI_UID_MAX];
    PcSlippiJson* root = NULL;
    char jerr[48];
    bool token_ok = pc_slippi_json_parse(resp, strlen(resp), &root, jerr, sizeof jerr);
    if (token_ok) {
        pc_slippi_json_string(pc_slippi_json_get(root, "idToken"), id_token, sizeof id_token);
        pc_slippi_json_string(pc_slippi_json_get(root, "localId"), fb_uid, sizeof fb_uid);
        token_ok = id_token[0] != '\0' && fb_uid[0] != '\0';
        pc_slippi_json_free(root);
    }
    if (!token_ok) {
        if (err != NULL && err_cap > 0) {
            snprintf(err, err_cap, "Slippi login returned an unexpected reply");
        }
        free(resp);
        return false;
    }

    /* 2. GraphQL getUserKeyQuery -> connect code, playKey, display name. */
    char gql_body[1024];
    snprintf(gql_body, sizeof gql_body, PC_SLIPPI_GET_USER_QUERY, fb_uid);
    if (!pc_slippi_http_post_json(gql, gql_body, id_token, resp, 16384, &status) ||
        status != 200) {
        if (err != NULL && err_cap > 0) {
            snprintf(err, err_cap, "Could not fetch your Slippi account");
        }
        free(resp);
        return false;
    }

    PcSlippiAccount a;
    account_clear(&a);
    snprintf(a.uid, sizeof a.uid, "%s", fb_uid);
    bool got = false;
    if (pc_slippi_json_parse(resp, strlen(resp), &root, jerr, sizeof jerr)) {
        const PcSlippiJson* data = pc_slippi_json_get(root, "data");
        const PcSlippiJson* user = pc_slippi_json_get(data, "getUser");
        if (user != NULL && user->type == PC_SLIPPI_JSON_OBJECT) {
            pc_slippi_json_string(pc_slippi_json_get(user, "displayName"), a.display_name,
                                  sizeof a.display_name);
            pc_slippi_json_string(
                pc_slippi_json_get(pc_slippi_json_get(user, "connectCode"), "code"),
                a.connect_code, sizeof a.connect_code);
            pc_slippi_json_string(
                pc_slippi_json_get(pc_slippi_json_get(user, "private"), "playKey"), a.play_key,
                sizeof a.play_key);
            pc_slippi_json_string(
                pc_slippi_json_get(pc_slippi_json_get(data, "getLatestDolphin"), "version"),
                a.latest_version, sizeof a.latest_version);
            got = a.connect_code[0] != '\0' && a.play_key[0] != '\0';
        }
        pc_slippi_json_free(root);
    }
    free(resp);
    if (!got) {
        if (err != NULL && err_cap > 0) {
            snprintf(err, err_cap, "This account has no Slippi connect code yet. "
                                   "Finish setup on slippi.gg, then sign in again");
        }
        return false;
    }
    a.logged_in = true;
    pc_slippi_account_default_chat(&a);
    pc_slippi_account_save(&a);
    if (out != NULL) {
        *out = a;
    }
    return true;
}
