/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal JSON reader for the Slippi matchmaking and account files.
 *
 * Slippi's matchmaking protocol is ENet plus JSON: the server sends
 * create-ticket-resp / get-ticket-resp objects, and the Launcher's user.json
 * is JSON too (SlippiMatchmaking.cpp uses nlohmann::json; SlippiUser reads the
 * same file). melee-pc is C and has no JSON library, and every server value is
 * untrusted, so this parses the subset Slippi actually uses -- objects, arrays,
 * strings, numbers, true/false/null -- with a bounded, non-throwing reader that
 * refuses anything malformed instead of guessing. There is no writer here: the
 * requests we send are small enough to format directly, with
 * pc_slippi_json_escape() for the strings in them.
 *
 * Strings keep pointing into the caller's input buffer (no copy); the parser
 * only decodes escapes into a caller-provided scratch buffer when asked, via
 * pc_slippi_json_string(). Nodes are allocated with malloc and must be released
 * with pc_slippi_json_free(). Parsing runs on the matchmaking thread, never on
 * the per-frame path, so allocation is acceptable.
 *
 * The parser is deliberately total: any malformed input yields a false return
 * and a short reason, never a crash or a partial tree. */
#ifndef PC_SLIPPI_SLP_JSON_H
#define PC_SLIPPI_SLP_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum PcSlippiJsonType {
    PC_SLIPPI_JSON_NULL,
    PC_SLIPPI_JSON_BOOL,
    PC_SLIPPI_JSON_NUMBER,
    PC_SLIPPI_JSON_STRING,
    PC_SLIPPI_JSON_ARRAY,
    PC_SLIPPI_JSON_OBJECT,
} PcSlippiJsonType;

typedef struct PcSlippiJson {
    PcSlippiJsonType type;
    bool boolean;          /* BOOL */
    double number;         /* NUMBER */
    const char* str;       /* STRING: into the source, undecoded, not NUL-terminated */
    size_t str_len;        /* STRING: byte length */
    const char* key;       /* OBJECT member: key, into the source, undecoded */
    size_t key_len;
    struct PcSlippiJson* child; /* first child (array element or member value) */
    struct PcSlippiJson* next;  /* next sibling */
    int child_count;
} PcSlippiJson;

/* Parse `text` (len bytes, may be NUL-terminated and len == strlen). On success
 * *out holds the root and the caller must free it with pc_slippi_json_free. On
 * failure returns false, writes a short reason into err (when err != NULL) and
 * leaves *out NULL. Depth and node counts are bounded so hostile input cannot
 * exhaust memory. */
bool pc_slippi_json_parse(const char* text, size_t len, PcSlippiJson** out, char* err,
                          size_t err_cap);

void pc_slippi_json_free(PcSlippiJson* node);

/* Object member lookup: the value for `key`, or NULL. Does not decode escapes
 * in the key (Slippi's keys are plain ASCII). */
const PcSlippiJson* pc_slippi_json_get(const PcSlippiJson* obj, const char* key);

/* Array element / object member by position (object order is source order). */
const PcSlippiJson* pc_slippi_json_at(const PcSlippiJson* node, int index);

/* Typed accessors with defaults; a wrong type yields the default, never a
 * crash. `def` is returned when the value is absent or of another type. */
/* Raw STRING pointer into the source (undecoded, NOT NUL-terminated) -- only
 * for comparing fixed ASCII values. Use pc_slippi_json_string for real text. */
const char* pc_slippi_json_str(const PcSlippiJson* node, const char* def);
double pc_slippi_json_num(const PcSlippiJson* node, double def);
int pc_slippi_json_int(const PcSlippiJson* node, int def);
bool pc_slippi_json_bool(const PcSlippiJson* node, bool def);
/* Discriminated helpers: false when the node is NULL or not that type. */
bool pc_slippi_json_is_null(const PcSlippiJson* node);
bool pc_slippi_json_is_array(const PcSlippiJson* node);

/* Copy a STRING node's value into `out`, decoding \" \\ \/ \b \f \n \r \t and
 * \uXXXX (to UTF-8), and NUL-terminate. Returns false (and writes "" when
 * out_cap > 0) when the node is not a string or the decoded value does not fit;
 * a truncated copy is never returned as success. */
bool pc_slippi_json_string(const PcSlippiJson* node, char* out, size_t out_cap);

/* Escape `s` for embedding in a JSON string literal, into a bounded buffer.
 * Returns false when the escaped value does not fit. A display name or a
 * password may contain a quote or backslash, which must not break the
 * request. */
bool pc_slippi_json_escape(const char* s, char* out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
