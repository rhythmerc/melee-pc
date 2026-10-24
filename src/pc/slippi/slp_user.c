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

/* ---- sign-in and account setup (the Launcher's quick start) ------------ */

/* The Firebase endpoints the Slippi Launcher's SDK calls underneath: sign-in,
 * token refresh, account lookup, the verification email, and the Launcher's
 * own sign-up function. MELEE_SLIPPI_AUTH_HOST swaps the scheme and host of
 * all of them for a local fake (tools/slippi_fake_auth.py); the paths stay. */
#define PC_SLIPPI_IDENTITY_HOST "https://identitytoolkit.googleapis.com"
#define PC_SLIPPI_TOKEN_HOST "https://securetoken.googleapis.com"
#define PC_SLIPPI_FUNCTIONS_HOST "https://us-central1-slippi.cloudfunctions.net"

/* The account fields the GraphQL query asks for, with $fbUid substituted. */
#define PC_SLIPPI_GET_USER_QUERY                                                              \
    "{\"query\":\"query getUserKeyQuery($fbUid: String) { getUser(fbUid: $fbUid) { "         \
    "fbUid displayName connectCode { code } private { playKey } rulesAccepted } "             \
    "getLatestDolphin { version } }\",\"variables\":{\"fbUid\":\"%s\"}}"
#define PC_SLIPPI_ACCEPT_RULES_MUTATION                                                       \
    "{\"query\":\"mutation AcceptRules($num: Int!) { userAcceptRules(num: $num) { fbUid "    \
    "rulesAccepted } }\",\"variables\":{\"num\":%d}}"
#define PC_SLIPPI_INIT_NETPLAY_MUTATION                                                       \
    "{\"query\":\"mutation InitNetplay($codeStart: String!) { userInitNetplay(codeStart: "   \
    "$codeStart) { fbUid } }\",\"variables\":{\"codeStart\":\"%s\"}}"

/* Release builds compile these in (CMakeLists.txt: from the configure
 * environment, CI's repository secrets); the environment still overrides, for
 * testing. Empty: no in-app login. */
#ifndef PC_SLIPPI_FIREBASE_KEY_BUILTIN
#define PC_SLIPPI_FIREBASE_KEY_BUILTIN ""
#endif
#ifndef PC_SLIPPI_GRAPHQL_ENDPOINT_BUILTIN
#define PC_SLIPPI_GRAPHQL_ENDPOINT_BUILTIN ""
#endif

#define SETUP_RESP_CAP 16384

static const char* login_setting(const char* name, const char* builtin) {
    const char* v = getenv(name);
    return v != NULL && v[0] != '\0' ? v : builtin;
}

static const char* firebase_key(void) {
    return login_setting("MELEE_SLIPPI_FIREBASE_KEY", PC_SLIPPI_FIREBASE_KEY_BUILTIN);
}

static const char* graphql_endpoint(void) {
    return login_setting("MELEE_SLIPPI_GRAPHQL_ENDPOINT", PC_SLIPPI_GRAPHQL_ENDPOINT_BUILTIN);
}

bool pc_slippi_account_login_available(void) {
    const char* key = firebase_key();
    const char* gql = graphql_endpoint();
    return key != NULL && key[0] != '\0' && gql != NULL && gql[0] != '\0';
}

static void setup_err(char* err, size_t cap, const char* text) {
    if (err != NULL && cap > 0) {
        snprintf(err, cap, "%s", text);
    }
}

/* `path` under one of the Firebase hosts, with the API key. */
static bool auth_url(char* out, size_t cap, const char* host, const char* path) {
    const char* fake = getenv("MELEE_SLIPPI_AUTH_HOST");
    int n = snprintf(out, cap, "%s%s?key=%s", fake != NULL && fake[0] != '\0' ? fake : host, path,
                     firebase_key());
    return n > 0 && (size_t)n < cap;
}

/* The account's own words for a Firebase error code ("EMAIL_NOT_FOUND", and
 * "TOO_MANY_ATTEMPTS_TRY_LATER : ..." with a detail after the code). */
static const char* firebase_error_text(const char* code) {
    static const struct {
        const char* code;
        const char* text;
    } known[] = {
        {"EMAIL_NOT_FOUND", "Wrong email or password"},
        {"INVALID_PASSWORD", "Wrong email or password"},
        {"INVALID_LOGIN_CREDENTIALS", "Wrong email or password"},
        {"INVALID_EMAIL", "That email address doesn't look right"},
        {"USER_DISABLED", "This Slippi account is disabled"},
        {"TOO_MANY_ATTEMPTS_TRY_LATER", "Too many tries. Try again in a few minutes"},
        {"TOKEN_EXPIRED", "Your sign-in expired. Sign in again"},
        {"USER_NOT_FOUND", "Your sign-in expired. Sign in again"},
        {"INVALID_REFRESH_TOKEN", "Your sign-in expired. Sign in again"},
    };
    for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
        size_t n = strlen(known[i].code);
        if (strncmp(code, known[i].code, n) == 0 && (code[n] == '\0' || code[n] == ' ')) {
            return known[i].text;
        }
    }
    return NULL;
}

/* A POST to a Firebase or Slippi endpoint, parsed. False with `err` set when
 * the request fails, the reply is not JSON, or the reply is an error. Error
 * replies come as {"error":{"message":...}} (Firebase, and the sign-up
 * function's HttpsError) or {"errors":[{"message":...}]} (GraphQL, often with
 * HTTP 200); the message is shown as it is unless it is a known code. */
typedef struct Reply {
    PcSlippiJson* root;
    char* text; /* the tree's strings point into it */
} Reply;

static void reply_free(Reply* r) {
    pc_slippi_json_free(r->root);
    free(r->text);
    r->root = NULL;
    r->text = NULL;
}

static bool post_parsed(const char* url, const char* body, const char* bearer, Reply* r, const char* fallback,
                        char* err, size_t cap) {
    r->root = NULL;
    r->text = NULL;
    char* resp = (char*)malloc(SETUP_RESP_CAP);
    PcSlippiJson** root = &r->root;
    if (resp == NULL) {
        setup_err(err, cap, "Out of memory");
        return false;
    }
    int status = 0;
    bool got = pc_slippi_http_post_json(url, body, bearer, resp, SETUP_RESP_CAP, &status);
    char jerr[48];
    if (!got || !pc_slippi_json_parse(resp, strlen(resp), root, jerr, sizeof jerr)) {
        free(resp);
        *root = NULL;
        setup_err(err, cap, status == 0 ? "Could not reach Slippi. Check your internet" : fallback);
        return false;
    }
    char msg[256] = {0};
    const PcSlippiJson* e = pc_slippi_json_get(*root, "error");
    if (e != NULL && e->type == PC_SLIPPI_JSON_OBJECT) {
        pc_slippi_json_string(pc_slippi_json_get(e, "message"), msg, sizeof msg);
    } else if (e != NULL) {
        pc_slippi_json_string(e, msg, sizeof msg);
    }
    const PcSlippiJson* errs = pc_slippi_json_get(*root, "errors");
    if (msg[0] == '\0' && pc_slippi_json_is_array(errs) && errs->child_count > 0) {
        pc_slippi_json_string(pc_slippi_json_get(pc_slippi_json_at(errs, 0), "message"), msg, sizeof msg);
    }
    bool failed = msg[0] != '\0' || e != NULL || (status != 200 && status != 0);
    if (failed) {
        const char* known = firebase_error_text(msg);
        setup_err(err, cap, known != NULL ? known : msg[0] != '\0' ? msg : fallback);
        pc_slippi_json_free(*root);
        *root = NULL;
        free(resp);
        return false;
    }
    r->text = resp;
    return true;
}

static void take_tokens(PcSlippiSetup* s, const PcSlippiJson* root, const char* id_key,
                        const char* refresh_key, const char* uid_key) {
    pc_slippi_json_string(pc_slippi_json_get(root, id_key), s->id_token, sizeof s->id_token);
    char refresh[sizeof s->refresh_token];
    if (pc_slippi_json_string(pc_slippi_json_get(root, refresh_key), refresh, sizeof refresh) &&
        refresh[0] != '\0') {
        snprintf(s->refresh_token, sizeof s->refresh_token, "%s", refresh);
    }
    char uid[sizeof s->uid];
    if (pc_slippi_json_string(pc_slippi_json_get(root, uid_key), uid, sizeof uid) && uid[0] != '\0') {
        snprintf(s->uid, sizeof s->uid, "%s", uid);
    }
}

bool pc_slippi_display_name_valid(const char* name) {
    /* The Launcher's rule: 1-15 characters the game can show (printable ASCII
     * here; the Launcher also allows kana, which the keyboard cannot type), no
     * backslash or backtick. */
    if (name == NULL || name[0] == '\0' || strlen(name) > 15) {
        return false;
    }
    for (const char* p = name; *p != '\0'; p++) {
        if (*p < ' ' || *p > '~' || *p == '\\' || *p == '`') {
            return false;
        }
    }
    return true;
}

bool pc_slippi_code_start_valid(const char* tag) {
    size_t n = tag != NULL ? strlen(tag) : 0;
    if (n < 2 || n > 4) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (tag[i] < 'A' || tag[i] > 'Z') {
            return false;
        }
    }
    return true;
}

void pc_slippi_code_start_suggest(const char* display_name, char* out, size_t cap) {
    /* As the Launcher does: the name's letters, upper-cased, the first four. */
    size_t w = 0;
    for (const char* p = display_name != NULL ? display_name : ""; *p != '\0' && w < 4 && w + 1 < cap; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        if (c >= 'A' && c <= 'Z') {
            out[w++] = c;
        }
    }
    if (cap > 0) {
        out[w] = '\0';
    }
}

void pc_slippi_setup_end(PcSlippiSetup* s) {
    if (s != NULL) {
        memset(s, 0, sizeof *s);
    }
}

bool pc_slippi_setup_sign_in(PcSlippiSetup* s, const char* email, const char* password, char* err,
                             size_t cap) {
    pc_slippi_setup_end(s);
    setup_err(err, cap, "");
    if (!pc_slippi_account_login_available()) {
        setup_err(err, cap, "Online login is not configured in this build");
        return false;
    }
    if (email == NULL || email[0] == '\0' || password == NULL || password[0] == '\0') {
        setup_err(err, cap, "Enter your Slippi email and password");
        return false;
    }
    char url[512], em[256], pw[256];
    if (!auth_url(url, sizeof url, PC_SLIPPI_IDENTITY_HOST, "/v1/accounts:signInWithPassword") ||
        !pc_slippi_json_escape(email, em, sizeof em) || !pc_slippi_json_escape(password, pw, sizeof pw)) {
        setup_err(err, cap, "Login credentials are too long");
        return false;
    }
    char body[768];
    snprintf(body, sizeof body, "{\"email\":\"%s\",\"password\":\"%s\",\"returnSecureToken\":true}", em,
             pw);
    memset(pw, 0, sizeof pw);
    /* The API key is a URL parameter and the body is a secret pair; neither is
     * ever logged here. */
    Reply rep;
    bool ok = post_parsed(url, body, NULL, &rep, "Could not reach Slippi login", err, cap);
    memset(body, 0, sizeof body);
    if (!ok) {
        return false;
    }
    take_tokens(s, rep.root, "idToken", "refreshToken", "localId");
    reply_free(&rep);
    if (s->id_token[0] == '\0' || s->uid[0] == '\0') {
        pc_slippi_setup_end(s);
        setup_err(err, cap, "Slippi login returned an unexpected reply");
        return false;
    }
    snprintf(s->email, sizeof s->email, "%s", email);
    return true;
}

bool pc_slippi_setup_register(PcSlippiSetup* s, const char* name, const char* email, const char* password,
                              char* err, size_t cap) {
    pc_slippi_setup_end(s);
    setup_err(err, cap, "");
    if (!pc_slippi_account_login_available()) {
        setup_err(err, cap, "Online login is not configured in this build");
        return false;
    }
    if (!pc_slippi_display_name_valid(name)) {
        setup_err(err, cap, "Names are 1 to 15 letters, numbers or symbols");
        return false;
    }
    char url[512], nm[96], em[256], pw[256];
    if (!auth_url(url, sizeof url, PC_SLIPPI_FUNCTIONS_HOST, "/createUserNew") ||
        !pc_slippi_json_escape(name, nm, sizeof nm) || !pc_slippi_json_escape(email, em, sizeof em) ||
        !pc_slippi_json_escape(password, pw, sizeof pw)) {
        setup_err(err, cap, "Too long");
        return false;
    }
    /* A callable function: the arguments go in "data", as the Firebase
     * Functions SDK sends them. */
    char body[896];
    snprintf(body, sizeof body, "{\"data\":{\"email\":\"%s\",\"password\":\"%s\",\"displayName\":\"%s\"}}",
             em, pw, nm);
    memset(pw, 0, sizeof pw);
    Reply rep;
    bool ok = post_parsed(url, body, NULL, &rep, "Could not create the account", err, cap);
    memset(body, 0, sizeof body);
    if (!ok) {
        return false;
    }
    reply_free(&rep);
    /* Then sign in, as the Launcher does right after. */
    return pc_slippi_setup_sign_in(s, email, password, err, cap);
}

/* A fresh ID token from the refresh token, so a just-verified email shows in
 * its claims; Slippi reads them. */
static bool refresh_tokens(PcSlippiSetup* s, char* err, size_t cap) {
    char url[512];
    char body[1200];
    if (!auth_url(url, sizeof url, PC_SLIPPI_TOKEN_HOST, "/v1/token") ||
        snprintf(body, sizeof body, "{\"grant_type\":\"refresh_token\",\"refresh_token\":\"%s\"}",
                 s->refresh_token) >= (int)sizeof body) {
        setup_err(err, cap, "Your sign-in expired. Sign in again");
        return false;
    }
    Reply rep;
    if (!post_parsed(url, body, NULL, &rep, "Your sign-in expired. Sign in again", err, cap)) {
        return false;
    }
    take_tokens(s, rep.root, "id_token", "refresh_token", "user_id");
    reply_free(&rep);
    return s->id_token[0] != '\0';
}

bool pc_slippi_setup_check(PcSlippiSetup* s, PcSlippiSetupStep* step, PcSlippiAccount* out, char* err,
                           size_t cap) {
    setup_err(err, cap, "");
    if (s->refresh_token[0] != '\0' && !refresh_tokens(s, err, cap)) {
        return false;
    }
    /* 1. Whether the email address is verified. */
    char url[512];
    char body[4200];
    if (!auth_url(url, sizeof url, PC_SLIPPI_IDENTITY_HOST, "/v1/accounts:lookup")) {
        setup_err(err, cap, "Could not reach Slippi login");
        return false;
    }
    snprintf(body, sizeof body, "{\"idToken\":\"%s\"}", s->id_token);
    Reply rep;
    if (!post_parsed(url, body, NULL, &rep, "Could not reach Slippi login", err, cap)) {
        return false;
    }
    const PcSlippiJson* user = pc_slippi_json_at(pc_slippi_json_get(rep.root, "users"), 0);
    s->email_verified = pc_slippi_json_bool(pc_slippi_json_get(user, "emailVerified"), false);
    char email[sizeof s->email];
    if (pc_slippi_json_string(pc_slippi_json_get(user, "email"), email, sizeof email) && email[0] != '\0') {
        snprintf(s->email, sizeof s->email, "%s", email);
    }
    reply_free(&rep);

    /* 2. The Slippi account: connect code, playKey, rules. */
    char gql_body[1024];
    snprintf(gql_body, sizeof gql_body, PC_SLIPPI_GET_USER_QUERY, s->uid);
    if (!post_parsed(graphql_endpoint(), gql_body, s->id_token, &rep, "Could not fetch your Slippi account",
                     err, cap)) {
        return false;
    }
    PcSlippiAccount a;
    account_clear(&a);
    snprintf(a.uid, sizeof a.uid, "%s", s->uid);
    const PcSlippiJson* data = pc_slippi_json_get(rep.root, "data");
    const PcSlippiJson* u = pc_slippi_json_get(data, "getUser");
    int rules = 0;
    if (u != NULL && u->type == PC_SLIPPI_JSON_OBJECT) {
        pc_slippi_json_string(pc_slippi_json_get(u, "displayName"), a.display_name, sizeof a.display_name);
        pc_slippi_json_string(pc_slippi_json_get(pc_slippi_json_get(u, "connectCode"), "code"), a.connect_code,
                              sizeof a.connect_code);
        pc_slippi_json_string(pc_slippi_json_get(pc_slippi_json_get(u, "private"), "playKey"), a.play_key,
                              sizeof a.play_key);
        rules = pc_slippi_json_int(pc_slippi_json_get(u, "rulesAccepted"), 0);
    }
    pc_slippi_json_string(pc_slippi_json_get(pc_slippi_json_get(data, "getLatestDolphin"), "version"),
                          a.latest_version, sizeof a.latest_version);
    reply_free(&rep);
    if (a.display_name[0] != '\0') {
        snprintf(s->display_name, sizeof s->display_name, "%s", a.display_name);
    }

    /* The Launcher's order. An account that can already play signs straight
     * in, verified email or not, as before; the steps are only for what is
     * missing, and the email comes first because Slippi wants it verified
     * before the rest. */
    bool has_code = a.connect_code[0] != '\0' && a.play_key[0] != '\0';
    bool has_rules = rules >= PC_SLIPPI_RULES_VERSION;
    if (has_code && has_rules) {
        *step = PC_SLIPPI_SETUP_DONE;
        a.logged_in = true;
        pc_slippi_account_save(&a);
        if (out != NULL) {
            *out = a;
        }
    } else if (!s->email_verified) {
        *step = PC_SLIPPI_SETUP_VERIFY_EMAIL;
    } else if (!has_rules) {
        *step = PC_SLIPPI_SETUP_RULES;
    } else {
        *step = PC_SLIPPI_SETUP_CONNECT_CODE;
    }
    return true;
}

bool pc_slippi_setup_send_verification(PcSlippiSetup* s, char* err, size_t cap) {
    char url[512];
    char body[4300];
    if (!auth_url(url, sizeof url, PC_SLIPPI_IDENTITY_HOST, "/v1/accounts:sendOobCode")) {
        setup_err(err, cap, "Could not reach Slippi login");
        return false;
    }
    snprintf(body, sizeof body, "{\"requestType\":\"VERIFY_EMAIL\",\"idToken\":\"%s\"}", s->id_token);
    Reply rep;
    if (!post_parsed(url, body, NULL, &rep, "Could not send the email", err, cap)) {
        return false;
    }
    reply_free(&rep);
    return true;
}

bool pc_slippi_setup_send_password_reset(const char* email, char* err, size_t cap) {
    char url[512], em[256];
    if (email == NULL || strchr(email, '@') == NULL) {
        setup_err(err, cap, "Type your email first");
        return false;
    }
    if (!pc_slippi_account_login_available() ||
        !auth_url(url, sizeof url, PC_SLIPPI_IDENTITY_HOST, "/v1/accounts:sendOobCode") ||
        !pc_slippi_json_escape(email, em, sizeof em)) {
        setup_err(err, cap, "Could not reach Slippi login");
        return false;
    }
    char body[400];
    snprintf(body, sizeof body, "{\"requestType\":\"PASSWORD_RESET\",\"email\":\"%s\"}", em);
    Reply rep;
    if (!post_parsed(url, body, NULL, &rep, "Could not send the email", err, cap)) {
        return false;
    }
    reply_free(&rep);
    return true;
}

bool pc_slippi_setup_accept_rules(PcSlippiSetup* s, char* err, size_t cap) {
    char body[256];
    snprintf(body, sizeof body, PC_SLIPPI_ACCEPT_RULES_MUTATION, PC_SLIPPI_RULES_VERSION);
    Reply rep;
    if (!post_parsed(graphql_endpoint(), body, s->id_token, &rep, "Could not accept the rules", err, cap)) {
        return false;
    }
    int got = pc_slippi_json_int(
        pc_slippi_json_get(pc_slippi_json_get(pc_slippi_json_get(rep.root, "data"), "userAcceptRules"),
                           "rulesAccepted"),
        0);
    reply_free(&rep);
    if (got != PC_SLIPPI_RULES_VERSION) {
        setup_err(err, cap, "Could not accept the rules");
        return false;
    }
    return true;
}

bool pc_slippi_setup_choose_code(PcSlippiSetup* s, const char* tag, char* err, size_t cap) {
    if (!pc_slippi_code_start_valid(tag)) {
        setup_err(err, cap, "2 to 4 letters, A to Z");
        return false;
    }
    char body[256];
    snprintf(body, sizeof body, PC_SLIPPI_INIT_NETPLAY_MUTATION, tag);
    Reply rep;
    if (!post_parsed(graphql_endpoint(), body, s->id_token, &rep, "Could not set your connect code", err,
                     cap)) {
        return false;
    }
    reply_free(&rep);
    return true;
}

bool pc_slippi_account_login(const char* email, const char* password, PcSlippiAccount* out, char* err,
                             size_t err_cap) {
    if (out != NULL) {
        account_clear(out);
    }
    PcSlippiSetup* s = (PcSlippiSetup*)calloc(1, sizeof *s);
    if (s == NULL) {
        setup_err(err, err_cap, "Out of memory");
        return false;
    }
    PcSlippiSetupStep step = PC_SLIPPI_SETUP_DONE;
    bool ok = pc_slippi_setup_sign_in(s, email, password, err, err_cap) &&
              pc_slippi_setup_check(s, &step, out, err, err_cap);
    if (ok && step != PC_SLIPPI_SETUP_DONE) {
        /* Only the game's Slippi pages walk the account through the rest. */
        setup_err(err, err_cap, "Finish setting up in game: Online Mode, Slippi, Login");
        ok = false;
    }
    pc_slippi_setup_end(s);
    free(s);
    return ok;
}
