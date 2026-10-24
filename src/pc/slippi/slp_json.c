/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal JSON reader for Slippi matchmaking and account files (slp_json.h).
 *
 * Total, bounded, non-throwing: every failure path latches an error and
 * releases whatever was allocated, so a hostile server response can only make
 * the parse fail, never exhaust memory or crash the game. See the header for
 * why this exists and what it covers. */
#include "pc/slippi/slp_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bounds. Slippi's own replies are a few KB; a ticket with four players and
 * their rank blocks is the largest. These are generous, and together they cap
 * a hostile response's memory at depth * nodes. */
#define JSON_MAX_DEPTH 32
#define JSON_MAX_NODES 4096

typedef struct Parse {
    const char* p;
    const char* end;
    int nodes;
    char* err;
    size_t err_cap;
    bool failed;
} Parse;

static void set_err(Parse* st, const char* msg) {
    if (st->failed) {
        return;
    }
    st->failed = true;
    if (st->err != NULL && st->err_cap > 0) {
        size_t n = strlen(msg);
        if (n >= st->err_cap) {
            n = st->err_cap - 1;
        }
        memcpy(st->err, msg, n);
        st->err[n] = '\0';
    }
}

static PcSlippiJson* node_new(Parse* st, PcSlippiJsonType type) {
    if (st->nodes >= JSON_MAX_NODES) {
        set_err(st, "too many values");
        return NULL;
    }
    PcSlippiJson* n = (PcSlippiJson*)calloc(1, sizeof *n);
    if (n == NULL) {
        set_err(st, "out of memory");
        return NULL;
    }
    st->nodes++;
    n->type = type;
    return n;
}

static void skip_ws(Parse* st) {
    while (st->p < st->end) {
        char c = *st->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            st->p++;
        } else {
            break;
        }
    }
}

static bool parse_value(Parse* st, PcSlippiJson** out, int depth);

/* A JSON string's raw contents, escape sequences left intact (they are decoded
 * lazily by pc_slippi_json_string). Advances past the closing quote. */
static bool parse_string_raw(Parse* st, const char** start, size_t* len) {
    if (st->p >= st->end || *st->p != '"') {
        set_err(st, "expected string");
        return false;
    }
    st->p++;
    *start = st->p;
    while (st->p < st->end) {
        unsigned char c = (unsigned char)*st->p;
        if (c == '"') {
            *len = (size_t)(st->p - *start);
            st->p++;
            return true;
        }
        if (c == '\\') {
            st->p++;
            if (st->p >= st->end) {
                set_err(st, "truncated escape");
                return false;
            }
            /* A \u escape may be followed by four hex digits, but the raw
             * scan only needs to not treat them specially. */
        } else if (c < 0x20) {
            set_err(st, "control byte in string");
            return false;
        }
        st->p++;
    }
    set_err(st, "unterminated string");
    return false;
}

static bool parse_number(Parse* st, double* out) {
    const char* s = st->p;
    if (st->p < st->end && (*st->p == '-' || *st->p == '+')) {
        st->p++;
    }
    bool digits = false;
    while (st->p < st->end && *st->p >= '0' && *st->p <= '9') {
        st->p++;
        digits = true;
    }
    if (st->p < st->end && *st->p == '.') {
        st->p++;
        while (st->p < st->end && *st->p >= '0' && *st->p <= '9') {
            st->p++;
            digits = true;
        }
    }
    if (!digits) {
        set_err(st, "bad number");
        return false;
    }
    if (st->p < st->end && (*st->p == 'e' || *st->p == 'E')) {
        st->p++;
        if (st->p < st->end && (*st->p == '-' || *st->p == '+')) {
            st->p++;
        }
        bool exp_digits = false;
        while (st->p < st->end && *st->p >= '0' && *st->p <= '9') {
            st->p++;
            exp_digits = true;
        }
        if (!exp_digits) {
            set_err(st, "bad exponent");
            return false;
        }
    }
    /* strtod needs a NUL-terminated run; Slippi numbers are short and the
     * source buffer is the caller's, so copy into a small stack buffer. */
    size_t n = (size_t)(st->p - s);
    char buf[64];
    if (n >= sizeof buf) {
        set_err(st, "number too long");
        return false;
    }
    memcpy(buf, s, n);
    buf[n] = '\0';
    *out = strtod(buf, NULL);
    return true;
}

static bool match_literal(Parse* st, const char* word, size_t n) {
    if ((size_t)(st->end - st->p) < n || memcmp(st->p, word, n) != 0) {
        return false;
    }
    st->p += n;
    return true;
}

static bool parse_array(Parse* st, PcSlippiJson* node, int depth) {
    st->p++; /* [ */
    skip_ws(st);
    if (st->p < st->end && *st->p == ']') {
        st->p++;
        return true;
    }
    PcSlippiJson* tail = NULL;
    for (;;) {
        PcSlippiJson* child = NULL;
        if (!parse_value(st, &child, depth + 1)) {
            return false;
        }
        if (tail == NULL) {
            node->child = child;
        } else {
            tail->next = child;
        }
        tail = child;
        node->child_count++;
        skip_ws(st);
        if (st->p >= st->end) {
            set_err(st, "unterminated array");
            return false;
        }
        if (*st->p == ',') {
            st->p++;
            skip_ws(st);
            continue;
        }
        if (*st->p == ']') {
            st->p++;
            return true;
        }
        set_err(st, "expected , or ]");
        return false;
    }
}

static bool parse_object(Parse* st, PcSlippiJson* node, int depth) {
    st->p++; /* { */
    skip_ws(st);
    if (st->p < st->end && *st->p == '}') {
        st->p++;
        return true;
    }
    PcSlippiJson* tail = NULL;
    for (;;) {
        const char* key = NULL;
        size_t key_len = 0;
        if (!parse_string_raw(st, &key, &key_len)) {
            return false;
        }
        skip_ws(st);
        if (st->p >= st->end || *st->p != ':') {
            set_err(st, "expected :");
            return false;
        }
        st->p++;
        skip_ws(st);
        PcSlippiJson* child = NULL;
        if (!parse_value(st, &child, depth + 1)) {
            return false;
        }
        child->key = key;
        child->key_len = key_len;
        if (tail == NULL) {
            node->child = child;
        } else {
            tail->next = child;
        }
        tail = child;
        node->child_count++;
        skip_ws(st);
        if (st->p >= st->end) {
            set_err(st, "unterminated object");
            return false;
        }
        if (*st->p == ',') {
            st->p++;
            skip_ws(st);
            continue;
        }
        if (*st->p == '}') {
            st->p++;
            return true;
        }
        set_err(st, "expected , or }");
        return false;
    }
}

static bool parse_value(Parse* st, PcSlippiJson** out, int depth) {
    if (depth > JSON_MAX_DEPTH) {
        set_err(st, "too deep");
        return false;
    }
    skip_ws(st);
    if (st->p >= st->end) {
        set_err(st, "unexpected end");
        return false;
    }
    char c = *st->p;
    if (c == '{') {
        PcSlippiJson* n = node_new(st, PC_SLIPPI_JSON_OBJECT);
        if (n == NULL) {
            return false;
        }
        if (!parse_object(st, n, depth)) {
            pc_slippi_json_free(n);
            return false;
        }
        *out = n;
        return true;
    }
    if (c == '[') {
        PcSlippiJson* n = node_new(st, PC_SLIPPI_JSON_ARRAY);
        if (n == NULL) {
            return false;
        }
        if (!parse_array(st, n, depth)) {
            pc_slippi_json_free(n);
            return false;
        }
        *out = n;
        return true;
    }
    if (c == '"') {
        const char* s = NULL;
        size_t len = 0;
        if (!parse_string_raw(st, &s, &len)) {
            return false;
        }
        PcSlippiJson* n = node_new(st, PC_SLIPPI_JSON_STRING);
        if (n == NULL) {
            return false;
        }
        n->str = s;
        n->str_len = len;
        *out = n;
        return true;
    }
    if (c == 't' && match_literal(st, "true", 4)) {
        PcSlippiJson* n = node_new(st, PC_SLIPPI_JSON_BOOL);
        if (n == NULL) {
            return false;
        }
        n->boolean = true;
        *out = n;
        return true;
    }
    if (c == 'f' && match_literal(st, "false", 5)) {
        PcSlippiJson* n = node_new(st, PC_SLIPPI_JSON_BOOL);
        if (n == NULL) {
            return false;
        }
        *out = n;
        return true;
    }
    if (c == 'n' && match_literal(st, "null", 4)) {
        PcSlippiJson* n = node_new(st, PC_SLIPPI_JSON_NULL);
        if (n == NULL) {
            return false;
        }
        *out = n;
        return true;
    }
    if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
        double v = 0;
        if (!parse_number(st, &v)) {
            return false;
        }
        PcSlippiJson* n = node_new(st, PC_SLIPPI_JSON_NUMBER);
        if (n == NULL) {
            return false;
        }
        n->number = v;
        *out = n;
        return true;
    }
    set_err(st, "unexpected byte");
    return false;
}

bool pc_slippi_json_parse(const char* text, size_t len, PcSlippiJson** out, char* err,
                          size_t err_cap) {
    if (out != NULL) {
        *out = NULL;
    }
    if (err != NULL && err_cap > 0) {
        err[0] = '\0';
    }
    if (text == NULL || out == NULL) {
        if (out != NULL) {
            *out = NULL;
        }
        if (err != NULL && err_cap > 0) {
            const char* msg = "no input";
            size_t n = strlen(msg);
            if (n >= err_cap) {
                n = err_cap - 1;
            }
            memcpy(err, msg, n);
            err[n] = '\0';
        }
        return false;
    }
    Parse st;
    st.p = text;
    st.end = text + len;
    st.nodes = 0;
    st.err = err;
    st.err_cap = err_cap;
    st.failed = false;
    PcSlippiJson* root = NULL;
    if (!parse_value(&st, &root, 0)) {
        if (err != NULL && err_cap > 0 && err[0] == '\0') {
            err[0] = '\0';
        }
        return false;
    }
    skip_ws(&st);
    if (st.p != st.end) {
        pc_slippi_json_free(root);
        set_err(&st, "trailing bytes");
        return false;
    }
    *out = root;
    return true;
}

void pc_slippi_json_free(PcSlippiJson* node) {
    while (node != NULL) {
        PcSlippiJson* next = node->next;
        pc_slippi_json_free(node->child);
        free(node);
        node = next;
    }
}

static bool key_equals(const PcSlippiJson* n, const char* key) {
    size_t n_key = strlen(key);
    if (n->key == NULL || n->key_len != n_key) {
        return false;
    }
    return memcmp(n->key, key, n_key) == 0;
}

const PcSlippiJson* pc_slippi_json_get(const PcSlippiJson* obj, const char* key) {
    if (obj == NULL || key == NULL || obj->type != PC_SLIPPI_JSON_OBJECT) {
        return NULL;
    }
    for (const PcSlippiJson* c = obj->child; c != NULL; c = c->next) {
        if (key_equals(c, key)) {
            return c;
        }
    }
    return NULL;
}

const PcSlippiJson* pc_slippi_json_at(const PcSlippiJson* node, int index) {
    if (node == NULL || index < 0
        || (node->type != PC_SLIPPI_JSON_ARRAY && node->type != PC_SLIPPI_JSON_OBJECT)) {
        return NULL;
    }
    const PcSlippiJson* c = node->child;
    for (int i = 0; c != NULL && i < index; i++) {
        c = c->next;
    }
    return c;
}

const char* pc_slippi_json_str(const PcSlippiJson* node, const char* def) {
    return node != NULL && node->type == PC_SLIPPI_JSON_STRING ? node->str : def;
}

double pc_slippi_json_num(const PcSlippiJson* node, double def) {
    return node != NULL && node->type == PC_SLIPPI_JSON_NUMBER ? node->number : def;
}

int pc_slippi_json_int(const PcSlippiJson* node, int def) {
    if (node == NULL || node->type != PC_SLIPPI_JSON_NUMBER) {
        return def;
    }
    double v = node->number;
    if (v < (double)INT32_MIN || v > (double)INT32_MAX) {
        return def;
    }
    return (int)v;
}

bool pc_slippi_json_bool(const PcSlippiJson* node, bool def) {
    return node != NULL && node->type == PC_SLIPPI_JSON_BOOL ? node->boolean : def;
}

bool pc_slippi_json_is_null(const PcSlippiJson* node) {
    return node != NULL && node->type == PC_SLIPPI_JSON_NULL;
}

bool pc_slippi_json_is_array(const PcSlippiJson* node) {
    return node != NULL && node->type == PC_SLIPPI_JSON_ARRAY;
}

/* Decode four hex digits after \u into a code point. */
static bool hex4(const char* p, const char* end, uint32_t* out) {
    if ((size_t)(end - p) < 4) {
        return false;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else {
            return false;
        }
        v = (v << 4) | d;
    }
    *out = v;
    return true;
}

static size_t utf8_encode(uint32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

bool pc_slippi_json_string(const PcSlippiJson* node, char* out, size_t out_cap) {
    if (out != NULL && out_cap > 0) {
        out[0] = '\0';
    }
    if (node == NULL || node->type != PC_SLIPPI_JSON_STRING || out == NULL || out_cap == 0) {
        return false;
    }
    const char* p = node->str;
    const char* end = node->str + node->str_len;
    size_t w = 0;
    bool truncated = false;
    while (p < end) {
        char c = *p++;
        if (c != '\\') {
            if (w + 1 >= out_cap) {
                truncated = true;
                break;
            }
            out[w++] = c;
            continue;
        }
        if (p >= end) {
            return false; /* parser already rejects this, but stay total */
        }
        char esc = *p++;
        uint32_t cp = 0;
        switch (esc) {
        case '"': cp = '"'; break;
        case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u': {
            if (!hex4(p, end, &cp)) {
                return false;
            }
            p += 4;
            /* A surrogate pair: keep the pair as one code point if the low
             * half follows, else pass the lone unit through as UTF-8. */
            if (cp >= 0xD800 && cp <= 0xDBFF && (size_t)(end - p) >= 6 && p[0] == '\\'
                && p[1] == 'u') {
                uint32_t lo = 0;
                if (hex4(p + 2, end, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
            }
            break;
        }
        default:
            return false;
        }
        char enc[4];
        size_t n = utf8_encode(cp, enc);
        if (w + n >= out_cap) {
            truncated = true;
            break;
        }
        memcpy(out + w, enc, n);
        w += n;
    }
    out[w] = '\0';
    if (truncated) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool pc_slippi_json_escape(const char* s, char* out, size_t cap) {
    size_t w = 0;
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
        const char* src = add != NULL ? add : (const char*)p;
        size_t n = add != NULL ? strlen(add) : 1;
        if (w + n >= cap) {
            return false;
        }
        memcpy(out + w, src, n);
        w += n;
    }
    out[w] = '\0';
    return true;
}
