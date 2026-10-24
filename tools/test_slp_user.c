/* SPDX-License-Identifier: GPL-2.0-or-later */
/* slp_user (M4): Slippi account parsing and serialization.
 *
 * The account fields come from untrusted JSON (a discovered user.json, or our
 * own store written by a login flow), and a wrong read means the wrong connect
 * code or playKey on the wire. So this pins the parse of a real user.json
 * shape, the defaults for missing/incorrect fields, the 16-message chat rule,
 * a round-trip through our writer, and that hostile input is refused rather
 * than accepted with garbage fields.
 *
 * The writer is pure and is #included, so the HTTP edge (slp_http.c) is stubbed
 * here; nothing in this test touches the network. */
#include "../src/pc/slippi/slp_json.c"
#include "../src/pc/slippi/slp_user.c"

#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>

/* Stubs: the refresh's HTTP edge is not exercised here. */
bool pc_slippi_http_get(const char* url, const char* bearer, char* out, size_t cap, int* status) {
    (void)url;
    (void)bearer;
    if (out != NULL && cap > 0) {
        out[0] = '\0';
    }
    if (status != NULL) {
        *status = 0;
    }
    return false;
}
/* A scripted fake of the account endpoints, answered by URL from its state,
 * so the setup steps run with no network. */
static struct {
    bool verified;
    int rules;
    bool code;
    int sent;
    int calls;
    char last_body[2048];
    char last_bearer[64];
} fake;

static void reply(char* out, size_t cap, int* status, int code, const char* body) {
    snprintf(out, cap, "%s", body);
    *status = code;
}

bool pc_slippi_http_post_json(const char* url, const char* body, const char* bearer, char* out,
                              size_t cap, int* status) {
    fake.calls++;
    snprintf(fake.last_body, sizeof fake.last_body, "%s", body != NULL ? body : "");
    snprintf(fake.last_bearer, sizeof fake.last_bearer, "%s", bearer != NULL ? bearer : "");
    char buf[1024];
    if (strstr(url, "accounts:signInWithPassword") != NULL) {
        if (strstr(body, "\"password\":\"right\"") == NULL) {
            reply(out, cap, status, 400, "{\"error\":{\"code\":400,\"message\":\"INVALID_LOGIN_CREDENTIALS\"}}");
        } else {
            reply(out, cap, status, 200, "{\"idToken\":\"id1\",\"refreshToken\":\"r1\",\"localId\":\"uid1\"}");
        }
    } else if (strstr(url, "/v1/token") != NULL) {
        reply(out, cap, status, 200, "{\"id_token\":\"id2\",\"refresh_token\":\"r2\",\"user_id\":\"uid1\"}");
    } else if (strstr(url, "accounts:lookup") != NULL) {
        snprintf(buf, sizeof buf, "{\"users\":[{\"email\":\"a@b.c\",\"emailVerified\":%s}]}",
                 fake.verified ? "true" : "false");
        reply(out, cap, status, 200, buf);
    } else if (strstr(url, "accounts:sendOobCode") != NULL) {
        fake.sent++;
        reply(out, cap, status, 200, "{\"email\":\"a@b.c\"}");
    } else if (strstr(url, "createUserNew") != NULL) {
        if (strstr(body, "\"email\":\"taken@b.c\"") != NULL) {
            reply(out, cap, status, 400,
                  "{\"error\":{\"message\":\"The email address is already in use\",\"status\":\"ALREADY_EXISTS\"}}");
        } else {
            reply(out, cap, status, 200, "{\"result\":{}}");
        }
    } else if (strstr(url, "example.test/gql") != NULL) {
        if (strstr(body, "userAcceptRules") != NULL) {
            fake.rules = 1;
            reply(out, cap, status, 200, "{\"data\":{\"userAcceptRules\":{\"fbUid\":\"uid1\",\"rulesAccepted\":1}}}");
        } else if (strstr(body, "userInitNetplay") != NULL) {
            if (strstr(body, "\"codeStart\":\"BAD\"") != NULL) {
                reply(out, cap, status, 200, "{\"errors\":[{\"message\":\"That code is not allowed\"}],\"data\":null}");
            } else {
                fake.code = true;
                reply(out, cap, status, 200, "{\"data\":{\"userInitNetplay\":{\"fbUid\":\"uid1\"}}}");
            }
        } else {
            snprintf(buf, sizeof buf,
                     "{\"data\":{\"getUser\":{\"fbUid\":\"uid1\",\"displayName\":\"Dummy Rules\","
                     "\"connectCode\":%s,\"private\":%s,\"rulesAccepted\":%d},"
                     "\"getLatestDolphin\":{\"version\":\"3.6.4\"}}}",
                     fake.code ? "{\"code\":\"DUMM#123\"}" : "null",
                     fake.code ? "{\"playKey\":\"pk\"}" : "null", fake.rules);
            reply(out, cap, status, 200, buf);
        }
    } else {
        out[0] = '\0';
        *status = 0;
        return false;
    }
    return true;
}

static void test_parse_real(void) {
    const char* text =
        "{\"uid\":\"abc123\",\"playKey\":\"KEY-xyz\",\"displayName\":\"Player One\","
        "\"connectCode\":\"PLYR#123\",\"latestVersion\":\"3.6.4\","
        "\"chatMessages\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\",\"j\",\"k\",\"l\","
        "\"m\",\"n\",\"o\",\"p\"]}";
    PcSlippiAccount a;
    assert(pc_slippi_account_from_json(text, strlen(text), &a));
    assert(a.logged_in);
    assert(strcmp(a.uid, "abc123") == 0);
    assert(strcmp(a.play_key, "KEY-xyz") == 0);
    assert(strcmp(a.display_name, "Player One") == 0);
    assert(strcmp(a.connect_code, "PLYR#123") == 0);
    assert(strcmp(a.latest_version, "3.6.4") == 0);
    assert(strcmp(a.chat_messages[0], "a") == 0);
    assert(strcmp(a.chat_messages[15], "p") == 0);
}

static void test_defaults(void) {
    /* No chatMessages -> Slippi defaults; still usable if uid present. */
    PcSlippiAccount a;
    assert(pc_slippi_account_from_json("{\"uid\":\"u\",\"playKey\":\"k\",\"connectCode\":\"C#1\"}",
                                       strlen("{\"uid\":\"u\",\"playKey\":\"k\",\"connectCode\":\"C#1\"}"),
                                       &a));
    assert(strcmp(a.chat_messages[0], "ggs") == 0);
    assert(strcmp(a.chat_messages[15], "bad connection") == 0);
    /* Wrong-size chat array also falls back. */
    PcSlippiAccount b;
    assert(pc_slippi_account_from_json("{\"uid\":\"u\",\"chatMessages\":[\"only\"]}",
                                       strlen("{\"uid\":\"u\",\"chatMessages\":[\"only\"]}"), &b));
    assert(strcmp(b.chat_messages[0], "ggs") == 0);
}

static void test_empty_is_logged_out(void) {
    PcSlippiAccount a;
    assert(!pc_slippi_account_from_json("{}", 2, &a));
    assert(!a.logged_in);
    assert(a.uid[0] == '\0');
    /* Not an object at all. */
    assert(!pc_slippi_account_from_json("[]", 2, &a));
    assert(!pc_slippi_account_from_json("not json", 8, &a));
    assert(!pc_slippi_account_from_json("", 0, &a));
}

static void test_round_trip(void) {
    PcSlippiAccount a;
    memset(&a, 0, sizeof a);
    strcpy(a.uid, "u1");
    strcpy(a.play_key, "k1");
    strcpy(a.display_name, "Name \"quoted\"");
    strcpy(a.connect_code, "ME#1");
    strcpy(a.latest_version, "3.6.4");
    pc_slippi_account_default_chat(&a);
    a.logged_in = true;
    char buf[8192];
    size_t n = pc_slippi_account_to_json(&a, buf, sizeof buf);
    assert(n > 0);
    PcSlippiAccount b;
    assert(pc_slippi_account_from_json(buf, n, &b));
    assert(strcmp(b.uid, "u1") == 0);
    assert(strcmp(b.play_key, "k1") == 0);
    assert(strcmp(b.display_name, "Name \"quoted\"") == 0);
    assert(strcmp(b.connect_code, "ME#1") == 0);
    assert(strcmp(b.chat_messages[2], "brb") == 0);
    /* A buffer too small must fail, not emit a partial file. */
    char tiny[16];
    assert(pc_slippi_account_to_json(&a, tiny, sizeof tiny) == 0);
    assert(tiny[0] == '\0');
}

static void test_hostile(void) {
    PcSlippiAccount a;
    /* A very long display name is truncated, not overflowed. */
    char big[4096];
    int w = snprintf(big, sizeof big, "{\"uid\":\"u\",\"displayName\":\"");
    for (int i = 0; i < 1000; i++) {
        big[w++] = 'x';
    }
    w += snprintf(big + w, sizeof big - w, "\"}");
    assert(pc_slippi_account_from_json(big, (size_t)w, &a));
    /* An over-long field is dropped whole (the JSON reader never returns a
     * truncated string as success), so it is empty, not a 31-byte prefix. */
    assert(a.display_name[0] == '\0');
    assert(strcmp(a.uid, "u") == 0);
    /* A bad JSON type for a string field leaves it empty, not garbage. */
    assert(!pc_slippi_account_from_json("{\"uid\":123}", strlen("{\"uid\":123}"), &a));
    assert(a.uid[0] == '\0');
}

static void test_login_unconfigured(void) {
    /* No FIREBASE_KEY / GRAPHQL_ENDPOINT in the test env: login must refuse
     * cleanly, not attempt a request, and say why. */
    unsetenv("MELEE_SLIPPI_FIREBASE_KEY");
    unsetenv("MELEE_SLIPPI_GRAPHQL_ENDPOINT");
    assert(!pc_slippi_account_login_available());
    PcSlippiAccount a;
    char err[128];
    assert(!pc_slippi_account_login("a@b.c", "pw", &a, err, sizeof err));
    assert(err[0] != '\0');
    /* Empty credentials are refused too, once configured. */
    setenv("MELEE_SLIPPI_FIREBASE_KEY", "k", 1);
    setenv("MELEE_SLIPPI_GRAPHQL_ENDPOINT", "https://example.test/gql", 1);
    assert(pc_slippi_account_login_available());
    assert(!pc_slippi_account_login("", "", &a, err, sizeof err));
    assert(strstr(err, "email") != NULL);
    /* Configured, but the HTTP edge is stubbed to fail: a clean failure with a
     * message, never a crash. */
    assert(!pc_slippi_account_login("a@b.c", "pw", &a, err, sizeof err));
    assert(err[0] != '\0');
    unsetenv("MELEE_SLIPPI_FIREBASE_KEY");
    unsetenv("MELEE_SLIPPI_GRAPHQL_ENDPOINT");
}

/* The Linux netplay AppImage keeps user.json under ~/.config/SlippiOnline,
 * outside the Launcher's folder; discovery has to look there. */
static void test_discover_appimage_dir(void) {
    char root[] = "/tmp/slp_user_test_XXXXXX";
    assert(mkdtemp(root) != NULL);
    char path[512];
    snprintf(path, sizeof path, "%s/.config", root);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.config/SlippiOnline", root);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.config/SlippiOnline/Slippi", root);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.config/SlippiOnline/Slippi/user.json", root);
    FILE* f = fopen(path, "w");
    assert(f != NULL);
    fputs("{\"uid\":\"u1\",\"playKey\":\"k\",\"connectCode\":\"ABC#123\","
          "\"displayName\":\"abc\"}", f);
    fclose(f);
    setenv("HOME", root, 1);
    unsetenv("XDG_CONFIG_HOME");
    unsetenv("MELEE_SLIPPI_DIR");
    PcSlippiAccount a;
    assert(pc_slippi_account_discover(&a));
    assert(strcmp(a.connect_code, "ABC#123") == 0);
    remove(path);
}

static void test_connect_codes(void) {
    char out[32];
    assert(pc_slippi_connect_code_valid("MERC#931"));
    assert(pc_slippi_connect_code_valid("A#1"));
    assert(pc_slippi_connect_code_valid("ABCDEFG#999"));
    assert(pc_slippi_connect_code_valid("A1B2#07"));
    assert(!pc_slippi_connect_code_valid("ABCDEFGH#1")); /* tag too long */
    assert(!pc_slippi_connect_code_valid("#123"));       /* no tag */
    assert(!pc_slippi_connect_code_valid("ABC#"));       /* no digits */
    assert(!pc_slippi_connect_code_valid("ABC#1234"));   /* too many digits */
    assert(!pc_slippi_connect_code_valid("ABC#12A"));    /* letters after the hash */
    assert(!pc_slippi_connect_code_valid("abc#123"));    /* lower case */
    assert(!pc_slippi_connect_code_valid("AB-C#123"));
    assert(!pc_slippi_connect_code_valid("ABC123"));
    assert(!pc_slippi_connect_code_valid("A#1#2"));
    assert(!pc_slippi_connect_code_valid(""));
    assert(!pc_slippi_connect_code_valid(NULL));
    assert(pc_slippi_connect_code_normalize("  merc#931\n", out, sizeof out));
    assert(strcmp(out, "MERC#931") == 0);
    assert(pc_slippi_connect_code_normalize("merc#931 add me!", out, sizeof out));
    assert(strcmp(out, "MERC#931") == 0);
    assert(!pc_slippi_connect_code_normalize("hello world", out, sizeof out));
    assert(!pc_slippi_connect_code_normalize(NULL, out, sizeof out));
    assert(out[0] == '\0');
}

static void setup_env(void) {
    setenv("MELEE_SLIPPI_FIREBASE_KEY", "k", 1);
    setenv("MELEE_SLIPPI_GRAPHQL_ENDPOINT", "https://example.test/gql", 1);
    unsetenv("MELEE_SLIPPI_AUTH_HOST");
    static char dir[] = "/tmp/slp_setup_test_XXXXXX";
    static bool made;
    if (!made) {
        assert(mkdtemp(dir) != NULL);
        made = true;
    }
    setenv("MELEE_SLIPPI_DIR", dir, 1);
    memset(&fake, 0, sizeof fake);
}

/* A new account walks every step in the Launcher's order: email, rules, code. */
static void test_setup_steps(void) {
    setup_env();
    PcSlippiSetup s;
    PcSlippiSetupStep step;
    PcSlippiAccount a;
    char err[160];
    assert(!pc_slippi_setup_sign_in(&s, "a@b.c", "wrong", err, sizeof err));
    assert(strcmp(err, "Wrong email or password") == 0);
    bool in = pc_slippi_setup_sign_in(&s, "a@b.c", "right", err, sizeof err);
    if (!in) fprintf(stderr, "sign-in: %s\n", err);
    assert(in);
    assert(strcmp(s.uid, "uid1") == 0);
    assert(strstr(fake.last_body, "right") != NULL); /* sent, and not kept: */
    assert(strstr((const char*)&s, "right") == NULL);

    assert(pc_slippi_setup_check(&s, &step, &a, err, sizeof err));
    assert(step == PC_SLIPPI_SETUP_VERIFY_EMAIL);
    assert(strcmp(s.id_token, "id2") == 0); /* refreshed first */
    assert(strcmp(s.display_name, "Dummy Rules") == 0);
    assert(pc_slippi_setup_send_verification(&s, err, sizeof err));
    assert(fake.sent == 1 && strstr(fake.last_body, "VERIFY_EMAIL") != NULL);

    fake.verified = true;
    assert(pc_slippi_setup_check(&s, &step, &a, err, sizeof err));
    assert(step == PC_SLIPPI_SETUP_RULES);
    assert(pc_slippi_setup_accept_rules(&s, err, sizeof err));
    assert(strstr(fake.last_body, "\"num\":1") != NULL);
    assert(strcmp(fake.last_bearer, "id2") == 0);

    assert(pc_slippi_setup_check(&s, &step, &a, err, sizeof err));
    assert(step == PC_SLIPPI_SETUP_CONNECT_CODE);
    assert(!pc_slippi_setup_choose_code(&s, "A", err, sizeof err)); /* refused before any request */
    assert(!pc_slippi_setup_choose_code(&s, "BAD", err, sizeof err));
    assert(strcmp(err, "That code is not allowed") == 0);
    assert(pc_slippi_setup_choose_code(&s, "DUMM", err, sizeof err));

    assert(pc_slippi_setup_check(&s, &step, &a, err, sizeof err));
    assert(step == PC_SLIPPI_SETUP_DONE);
    assert(a.logged_in && strcmp(a.connect_code, "DUMM#123") == 0 && strcmp(a.play_key, "pk") == 0);
    PcSlippiAccount stored;
    assert(pc_slippi_account_load(&stored, false));
    assert(strcmp(stored.connect_code, "DUMM#123") == 0);
    pc_slippi_setup_end(&s);
    assert(s.id_token[0] == '\0' && s.refresh_token[0] == '\0');
}

/* An account that can already play signs in as before, even unverified; one
 * missing only the rules (the reported player) goes straight to them. */
static void test_setup_existing(void) {
    setup_env();
    fake.code = true;
    fake.rules = 1;
    PcSlippiAccount a;
    char err[160];
    assert(pc_slippi_account_login("a@b.c", "right", &a, err, sizeof err));
    assert(strcmp(a.connect_code, "DUMM#123") == 0);

    fake.rules = 0;
    assert(!pc_slippi_account_login("a@b.c", "right", &a, err, sizeof err));
    assert(strstr(err, "Online Mode") != NULL);
    fake.verified = true;
    PcSlippiSetup s;
    PcSlippiSetupStep step;
    assert(pc_slippi_setup_sign_in(&s, "a@b.c", "right", err, sizeof err));
    assert(pc_slippi_setup_check(&s, &step, &a, err, sizeof err));
    assert(step == PC_SLIPPI_SETUP_RULES);
}

static void test_setup_register(void) {
    setup_env();
    PcSlippiSetup s;
    char err[160];
    assert(!pc_slippi_setup_register(&s, "", "a@b.c", "right", err, sizeof err));
    assert(!pc_slippi_setup_register(&s, "sixteen chars xx", "a@b.c", "right", err, sizeof err));
    assert(!pc_slippi_setup_register(&s, "Name", "taken@b.c", "right", err, sizeof err));
    assert(strcmp(err, "The email address is already in use") == 0);
    assert(pc_slippi_setup_register(&s, "Dummy Rules", "a@b.c", "right", err, sizeof err));
    assert(strcmp(s.uid, "uid1") == 0);
}

static void test_setup_reset(void) {
    setup_env();
    char err[160];
    assert(!pc_slippi_setup_send_password_reset("", err, sizeof err));
    assert(fake.calls == 0);
    assert(pc_slippi_setup_send_password_reset("a@b.c", err, sizeof err));
    assert(strstr(fake.last_body, "PASSWORD_RESET") != NULL && strstr(fake.last_body, "a@b.c") != NULL);
}

static void test_setup_names(void) {
    char tag[8];
    assert(pc_slippi_display_name_valid("Player One"));
    assert(pc_slippi_display_name_valid("x"));
    assert(!pc_slippi_display_name_valid("back\\slash"));
    assert(!pc_slippi_display_name_valid("tick`"));
    assert(!pc_slippi_display_name_valid("0123456789abcdef"));
    assert(pc_slippi_code_start_valid("AB") && pc_slippi_code_start_valid("DUMM"));
    assert(!pc_slippi_code_start_valid("A") && !pc_slippi_code_start_valid("ABCDE"));
    assert(!pc_slippi_code_start_valid("ab") && !pc_slippi_code_start_valid("A1"));
    pc_slippi_code_start_suggest("dummy-rules", tag, sizeof tag);
    assert(strcmp(tag, "DUMM") == 0);
    pc_slippi_code_start_suggest("x1-y", tag, sizeof tag);
    assert(strcmp(tag, "XY") == 0);
}

int main(void) {
    test_connect_codes();
    test_discover_appimage_dir();
    test_parse_real();
    test_defaults();
    test_empty_is_logged_out();
    test_round_trip();
    test_hostile();
    test_login_unconfigured();
    test_setup_names();
    test_setup_steps();
    test_setup_existing();
    test_setup_register();
    test_setup_reset();
    printf("slp_user: all tests passed\n");
    return 0;
}
