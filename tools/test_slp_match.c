/* SPDX-License-Identifier: GPL-2.0-or-later */
/* slp_match (M4): Slippi matchmaking ticket parsing.
 *
 * The player list in a get-ticket-resp indexes fixed per-player arrays in the
 * netplay client, and the address it yields is dialed. So this pins the happy
 * path (a real two-player ticket), the address LAN/WAN choice, the default
 * stages, and that every malformed ticket is refused whole rather than
 * returning a half-valid one. The local-peer mode runs the same parser, so it
 * is checked too.
 *
 * The parser is pure; the matchmaking client (the search thread) is not
 * exercised here, so this does not link the network or SDL beyond the header
 * include used by the compiled .c. */
#include "../src/pc/slippi/slp_json.c"
#include "../src/pc/slippi/slp_match.c"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The search client references the net driver's global init; the parser test
 * does not use it, so stub it rather than pull the whole driver in. */
bool pc_slippi_net_global_init(void) {
    return true;
}

static const char* kTicket =
    "{\"matchId\":\"12345\",\"isHost\":true,\"players\":["
    "{\"uid\":\"me\",\"displayName\":\"Me\",\"connectCode\":\"ME#1\",\"port\":1,"
    "\"isLocalPlayer\":true,\"ipAddress\":\"203.0.113.7\",\"ipAddressLan\":\"192.168.1.5\"},"
    "{\"uid\":\"you\",\"displayName\":\"You\",\"connectCode\":\"YOU#2\",\"port\":2,"
    "\"isLocalPlayer\":false,\"ipAddress\":\"198.51.100.9\",\"ipAddressLan\":\"192.168.1.6\"}"
    "],\"stages\":[3,8,28],\"items\":1}";

static void test_parse_two(void) {
    PcSlippiTicket t;
    assert(pc_slippi_ticket_parse(kTicket, strlen(kTicket), 40000, &t));
    assert(t.ok);
    assert(strcmp(t.match_id, "12345") == 0);
    assert(t.is_host);
    assert(t.player_count == 2);
    assert(t.local_player_index == 0);
    assert(t.remote_count == 1);
    assert(t.players[0].is_local);
    assert(!t.players[1].is_local);
    assert(strcmp(t.players[1].connect_code, "YOU#2") == 0);
    /* Different NAT: use the public address with the advertised port. */
    assert(strcmp(t.remote_addresses[0], "198.51.100.9:2") == 0);
    assert(t.stage_count == 3);
    assert(t.stages[2] == 28);
    assert(t.items == 1);
}

static void test_same_nat_uses_lan(void) {
    /* Both players share the 203.0.113.* external prefix -> LAN address. */
    const char* text =
        "{\"isHost\":false,\"players\":["
        "{\"uid\":\"me\",\"port\":1,\"isLocalPlayer\":true,\"ipAddress\":\"203.0.113.7\"},"
        "{\"uid\":\"you\",\"port\":2,\"ipAddress\":\"203.0.113.8\","
        "\"ipAddressLan\":\"192.168.1.42\"}]}";
    PcSlippiTicket t;
    assert(pc_slippi_ticket_parse(text, strlen(text), 40000, &t));
    assert(strcmp(t.remote_addresses[0], "192.168.1.42:2") == 0);
}

static void test_default_stages(void) {
    const char* text =
        "{\"isHost\":true,\"players\":[{\"uid\":\"a\",\"port\":1,\"isLocalPlayer\":true},"
        "{\"uid\":\"b\",\"port\":2}]}";
    PcSlippiTicket t;
    assert(pc_slippi_ticket_parse(text, strlen(text), 40000, &t));
    /* Singles adds Fountain of Dreams (0x2). */
    assert(t.stage_count == 6);
    assert(t.stages[5] == 0x2);
    uint16_t out[PC_SLIPPI_MATCH_STAGES_MAX];
    int n = 0;
    pc_slippi_ticket_default_stages(3, out, &n);
    assert(n == 5);
    assert(out[0] == 0x3 && out[4] == 0x20);
}

static void test_refusals(void) {
    PcSlippiTicket t;
    /* Error envelope. */
    assert(!pc_slippi_ticket_parse("{\"error\":\"no match\"}", strlen("{\"error\":\"no match\"}"),
                                   40000, &t));
    assert(strstr(t.error, "no match") != NULL);
    /* Too few / too many players. */
    assert(!pc_slippi_ticket_parse("{\"players\":[{\"port\":1,\"isLocalPlayer\":true}]}",
                                   strlen("{\"players\":[{\"port\":1,\"isLocalPlayer\":true}]}"), 1,
                                   &t));
    assert(strstr(t.error, "count") != NULL);
    /* Duplicate port. */
    const char* dup =
        "{\"players\":[{\"port\":1,\"isLocalPlayer\":true},{\"port\":1}]}";
    assert(!pc_slippi_ticket_parse(dup, strlen(dup), 1, &t));
    assert(strstr(t.error, "port") != NULL);
    /* Port out of range. */
    const char* bad =
        "{\"players\":[{\"port\":1,\"isLocalPlayer\":true},{\"port\":9}]}";
    assert(!pc_slippi_ticket_parse(bad, strlen(bad), 1, &t));
    /* No local player. */
    const char* nolocal =
        "{\"players\":[{\"port\":1},{\"port\":2}]}";
    assert(!pc_slippi_ticket_parse(nolocal, strlen(nolocal), 1, &t));
    assert(strstr(t.error, "local") != NULL);
    /* Not JSON at all. */
    assert(!pc_slippi_ticket_parse("hello", 5, 1, &t));
    assert(!pc_slippi_ticket_parse(NULL, 0, 1, &t));
}

static void test_local_peer(void) {
    PcSlippiTicket t;
    assert(pc_slippi_ticket_local(40000, 40001, 2, 0, &t));
    assert(t.ok);
    assert(t.player_count == 2);
    assert(t.local_player_index == 0);
    assert(t.is_host);
    assert(t.remote_count == 1);
    assert(strcmp(t.remote_addresses[0], "127.0.0.1:40002") == 0);
    assert(t.stage_count == 6); /* singles default */
    /* Bad args refused. */
    assert(!pc_slippi_ticket_local(40000, 40001, 1, 0, &t));
    assert(!pc_slippi_ticket_local(40000, 40001, 2, 5, &t));
}

/* The server's error replies, as the CSS shows them (48 characters). The
 * rules one came from a player's log (2026-10-24). */
static void test_server_errors(void) {
    char out[96];
    server_error_text("Please restart Slippi Launcher to accept rules. Update launcher if needed", "", out,
                      sizeof out);
    assert(strstr(out, "rules") != NULL && strlen(out) <= 48);
    server_error_text("Please restart Slippi Launcher", "", out, sizeof out);
    assert(strstr(out, "Update Xelee") != NULL);
    server_error_text("Please update", "9.9.9", out, sizeof out);
    assert(strstr(out, "9.9.9") != NULL);
    server_error_text("Some other problem", "", out, sizeof out);
    assert(strcmp(out, "Some other problem") == 0);
}

int main(void) {
    test_server_errors();
    test_parse_two();
    test_same_nat_uses_lan();
    test_default_stages();
    test_refusals();
    test_local_peer();
    printf("slp_match: all tests passed\n");
    return 0;
}
