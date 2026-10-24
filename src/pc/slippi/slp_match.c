/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Slippi matchmaking ticket parse + local peer mode + the ENet MM client.
 *
 * The parser is the part that must be exactly right: the reply's player list
 * indexes fixed per-player arrays, so pc_slippi_ticket_parse() validates the
 * whole structure and refuses it whole rather than returning a partially
 * valid ticket. It is pure and is the core of the slp_match test.
 *
 * Wire constants (from Ishiiruka SlippiMatchmaking.cpp and melee-unlocked):
 *   host               mm.slippi.gg (dev mm2.slippi.gg), port 43113
 *   request types      "create-ticket", "get-ticket-resp"
 *   appVersion         PC_SLIPPI_SEMVER
 *   default stages     0x3,0x8,0x1C,0x1F,0x20 (+0x2 when two players)
 *   OnlinePlayMode     RANKED 0, UNRANKED 1, DIRECT 2, TEAMS 3, PARTY 4 */
#include "pc/slippi/slp_match.h"

#include "pc/slippi/slp_json.h"
#include "pc/slippi/slp_net.h"
#include "pc/slippi/slp_session.h"

#include <SDL3/SDL.h>
#include <enet/enet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#elif !defined(__EMSCRIPTEN__)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* The opaque client. The search runs on its own thread and publishes state and
 * result under `lock`; the state atomic is the cheap poll the UI reads. */
struct PcSlippiMatch {
    SDL_Thread* thread;
    SDL_Mutex* lock;
    SDL_AtomicInt state;
    bool stop;
    bool use_local_peer;
    int mode;
    char connect_code[PC_SLIPPI_CONNECT_CODE_MAX];
    PcSlippiAccount account;
    uint16_t local_port;
    char error[128];
    PcSlippiTicket ticket;
};

#define PC_SLIPPI_MM_HOST "mm.slippi.gg"
#define PC_SLIPPI_MM_DEV_HOST "mm2.slippi.gg"
#define PC_SLIPPI_MM_PORT 43113
#define PC_SLIPPI_MM_TIMEOUT_MS 10000

/* ---- stage defaults --------------------------------------------------- */

void pc_slippi_ticket_default_stages(int player_count, uint16_t out[PC_SLIPPI_MATCH_STAGES_MAX],
                                     int* count) {
    static const uint16_t kSingles[] = {0x3, 0x8, 0x1C, 0x1F, 0x20, 0x2};
    static const uint16_t kBase[] = {0x3, 0x8, 0x1C, 0x1F, 0x20};
    const uint16_t* src = player_count == 2 ? kSingles : kBase;
    int n = player_count == 2 ? (int)(sizeof kSingles / sizeof kSingles[0])
                              : (int)(sizeof kBase / sizeof kBase[0]);
    for (int i = 0; i < n; i++) {
        out[i] = src[i];
    }
    if (count != NULL) {
        *count = n;
    }
}

/* ---- address selection ------------------------------------------------ */

/* Pull the low two octets of an "a.b.c.d" string, for the external-IP prefix
 * comparison Slippi's matchmaking uses to decide LAN vs WAN address. */
static bool ip_prefix2(const char* s, uint32_t* out) {
    unsigned a, b;
    if (s == NULL || sscanf(s, "%u.%u.", &a, &b) != 2 || a > 255 || b > 255) {
        return false;
    }
    *out = (a << 8) | b;
    return true;
}

/* Choose the reachable address for one player. Slippi sends both a public and
 * a LAN address; when the remote's public prefix matches ours, both peers are
 * behind the same NAT and the LAN address is the one that works. Otherwise the
 * public one plus the advertised port. */
static void choose_address(const PcSlippiJson* p, const char* local_external, uint16_t fallback_port,
                           char* out, size_t cap) {
    char ip[PC_SLIPPI_ADDR_MAX] = {0};
    char lan[PC_SLIPPI_ADDR_MAX] = {0};
    /* pc_slippi_json_str() points into the input buffer without a terminator,
     * so decode into the fixed buffers. */
    pc_slippi_json_string(pc_slippi_json_get(p, "ipAddress"), ip, sizeof ip);
    pc_slippi_json_string(pc_slippi_json_get(p, "ipAddressLan"), lan, sizeof lan);
    int port = pc_slippi_json_int(pc_slippi_json_get(p, "port"), 0);
    if (port < 1 || port > 255) {
        port = fallback_port;
    }
    const char* chosen = ip[0] ? ip : NULL;
    if (ip[0] && lan[0]) {
        uint32_t mine = 0, theirs = 0;
        if (ip_prefix2(local_external, &mine) && ip_prefix2(ip, &theirs) && mine == theirs) {
            chosen = lan;
        }
    }
    if (chosen == NULL || chosen[0] == '\0') {
        snprintf(out, cap, "127.0.0.1:%d", port);
        return;
    }
    /* The ticket's ipAddress may already carry a port; if so keep it, else add
     * the advertised one. */
    if (strrchr(chosen, ':') != NULL) {
        snprintf(out, cap, "%s", chosen);
    } else {
        snprintf(out, cap, "%s:%d", chosen, port);
    }
}

/* ---- ticket parse ----------------------------------------------------- */

bool pc_slippi_ticket_parse(const char* text, size_t len, uint16_t local_port,
                            PcSlippiTicket* out) {
    memset(out, 0, sizeof *out);
    if (text == NULL) {
        snprintf(out->error, sizeof out->error, "empty reply");
        return false;
    }
    PcSlippiJson* root = NULL;
    char jerr[64];
    if (!pc_slippi_json_parse(text, len, &root, jerr, sizeof jerr)) {
        snprintf(out->error, sizeof out->error, "bad json");
        return false;
    }
    /* An error envelope is delivered with 200 and no players. */
    const PcSlippiJson* err = pc_slippi_json_get(root, "error");
    if (err != NULL && !pc_slippi_json_is_null(err)) {
        char msg[64];
        pc_slippi_json_string(err, msg, sizeof msg);
        snprintf(out->error, sizeof out->error, "%s", msg[0] ? msg : "matchmaking error");
        pc_slippi_json_free(root);
        return false;
    }

    char match_id[64];
    pc_slippi_json_string(pc_slippi_json_get(root, "matchId"), match_id, sizeof match_id);
    snprintf(out->match_id, sizeof out->match_id, "%s", match_id);

    out->is_host = pc_slippi_json_bool(pc_slippi_json_get(root, "isHost"), false);

    const PcSlippiJson* players = pc_slippi_json_get(root, "players");
    if (!pc_slippi_json_is_array(players)) {
        snprintf(out->error, sizeof out->error, "no players");
        pc_slippi_json_free(root);
        return false;
    }
    int count = players->child_count;
    if (count < 2 || count > PC_SLIPPI_MATCH_PLAYERS_MAX) {
        snprintf(out->error, sizeof out->error, "player count %d", count);
        pc_slippi_json_free(root);
        return false;
    }

    /* The local external IP is not in the ticket; derive it from the local
     * player's own public address so the same-NAT test can run. */
    char local_external[PC_SLIPPI_ADDR_MAX] = {0};
    bool seen_port[PC_SLIPPI_MATCH_PLAYERS_MAX + 1] = {false};
    int local_index = -1;
    for (int i = 0; i < count; i++) {
        const PcSlippiJson* p = pc_slippi_json_at(players, i);
        if (p == NULL) {
            snprintf(out->error, sizeof out->error, "player %d missing", i);
            pc_slippi_json_free(root);
            return false;
        }
        PcSlippiMatchPlayer* mp = &out->players[i];
        int port = pc_slippi_json_int(pc_slippi_json_get(p, "port"), 0);
        if (port < 1 || port > PC_SLIPPI_MATCH_PLAYERS_MAX || seen_port[port]) {
            snprintf(out->error, sizeof out->error, "bad port %d", port);
            pc_slippi_json_free(root);
            return false;
        }
        seen_port[port] = true;
        mp->port = (uint8_t)port;
        mp->is_local = pc_slippi_json_bool(pc_slippi_json_get(p, "isLocalPlayer"), false);
        mp->is_bot = pc_slippi_json_bool(pc_slippi_json_get(p, "isBot"), false);
        pc_slippi_json_string(pc_slippi_json_get(p, "uid"), mp->uid, sizeof mp->uid);
        pc_slippi_json_string(pc_slippi_json_get(p, "displayName"), mp->display_name,
                              sizeof mp->display_name);
        pc_slippi_json_string(pc_slippi_json_get(p, "connectCode"), mp->connect_code,
                              sizeof mp->connect_code);
        choose_address(p, local_external, local_port, mp->address, sizeof mp->address);
        if (mp->is_local) {
            if (local_index >= 0) {
                snprintf(out->error, sizeof out->error, "two local players");
                pc_slippi_json_free(root);
                return false;
            }
            local_index = i;
            pc_slippi_json_string(pc_slippi_json_get(p, "ipAddress"), local_external,
                                  sizeof local_external);
        }
    }

    /* A second pass once local_external is known, so every remote's address is
     * chosen against it uniformly. */
    for (int i = 0; i < count; i++) {
        const PcSlippiJson* p = pc_slippi_json_at(players, i);
        choose_address(p, local_external, local_port, out->players[i].address,
                       sizeof out->players[i].address);
    }

    if (local_index < 0) {
        snprintf(out->error, sizeof out->error, "no local player");
        pc_slippi_json_free(root);
        return false;
    }
    out->player_count = count;
    out->local_player_index = local_index;
    out->local_port = local_port;

    /* Remotes in ticket order, local excluded. */
    for (int i = 0; i < count; i++) {
        if (i == local_index) {
            continue;
        }
        snprintf(out->remote_addresses[out->remote_count], PC_SLIPPI_ADDR_MAX, "%s",
                 out->players[i].address);
        out->remote_count++;
    }

    const PcSlippiJson* stages = pc_slippi_json_get(root, "stages");
    if (pc_slippi_json_is_array(stages)) {
        for (int i = 0; i < stages->child_count && out->stage_count < PC_SLIPPI_MATCH_STAGES_MAX;
             i++) {
            out->stages[out->stage_count++] = (uint16_t)pc_slippi_json_int(
                pc_slippi_json_at(stages, i), 0);
        }
    }
    if (out->stage_count == 0) {
        pc_slippi_ticket_default_stages(count, out->stages, &out->stage_count);
    }
    out->items = (uint32_t)pc_slippi_json_num(pc_slippi_json_get(root, "items"), 0);

    pc_slippi_json_free(root);
    out->ok = true;
    return true;
}

bool pc_slippi_ticket_local(uint16_t local_port, uint16_t base_remote_port, int n, int local_index,
                            PcSlippiTicket* out) {
    if (n < 2 || n > PC_SLIPPI_MATCH_PLAYERS_MAX || local_index < 0 || local_index >= n) {
        memset(out, 0, sizeof *out);
        snprintf(out->error, sizeof out->error, "bad local peer args");
        return false;
    }
    /* Build the same JSON a server would send, then run it through the real
     * parser: the local test path and the server path cannot drift. Shape
     * matches melee-unlocked's local_peer_ticket: all players on 127.0.0.1,
     * player 1 the decider, the six-stage singles list only for two players. */
    char buf[2048];
    int w = snprintf(buf, sizeof buf,
                     "{\"type\":\"get-ticket-resp\",\"matchId\":\"mode.direct-local-test\","
                     "\"isHost\":%s,\"players\":[",
                     local_index == 0 ? "true" : "false");
    for (int i = 0; i < n; i++) {
        int port = i + 1;
        int udp = (i == local_index) ? local_port : base_remote_port + i;
        w += snprintf(buf + w, sizeof buf - w,
                      "%s{\"uid\":\"local%d\",\"displayName\":\"Player %d\","
                      "\"connectCode\":\"PEER#00%d\",\"port\":%d,\"isLocalPlayer\":%s,"
                      "\"ipAddress\":\"127.0.0.1:%d\",\"ipAddressLan\":\"127.0.0.1:%d\"}",
                      i ? "," : "", i, i + 1, i + 1, port, i == local_index ? "true" : "false", udp,
                      udp);
    }
    w += snprintf(buf + w, sizeof buf - w, "]");
    if (n == 2) {
        w += snprintf(buf + w, sizeof buf - w, ",\"stages\":[2,3,8,28,31,32]");
    }
    w += snprintf(buf + w, sizeof buf - w, "}");
    return pc_slippi_ticket_parse(buf, (size_t)w, local_port, out);
}

/* ---- matchmaking client ----------------------------------------------- */

/* The local machine's outbound LAN address, as the server wants it in
 * ipAddressLan. A UDP socket "connect" (which sends nothing) makes the OS
 * pick the interface it would use, so getsockname() names that address. Trying
 * a routable address is deliberate: a wrong-interface guess would hand peers an
 * unreachable LAN address. Empty on failure, which the server treats as
 * "no LAN hint". */
static void local_lan_address(char* out, size_t cap) {
    out[0] = '\0';
#if defined(_WIN32)
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) {
        return;
    }
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = htonl(0x08080808u); /* 8.8.8.8 */
    if (connect(s, (struct sockaddr*)&dst, sizeof dst) == 0) {
        struct sockaddr_in self = {0};
        int len = sizeof self;
        if (getsockname(s, (struct sockaddr*)&self, &len) == 0) {
            snprintf(out, cap, "%s", inet_ntoa(self.sin_addr));
        }
    }
    closesocket(s);
#elif !defined(__EMSCRIPTEN__)
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return;
    }
    struct sockaddr_in dst = {0};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(53);
    dst.sin_addr.s_addr = htonl(0x08080808u);
    if (connect(s, (struct sockaddr*)&dst, sizeof dst) == 0) {
        struct sockaddr_in self = {0};
        socklen_t len = sizeof self;
        if (getsockname(s, (struct sockaddr*)&self, &len) == 0) {
            snprintf(out, cap, "%s", inet_ntoa(self.sin_addr));
        }
    }
    close(s);
#else
    (void)cap;
#endif
}

/* Wait for one reliable message and return its body. The Slippi matchmaking
 * server pushes get-ticket-resp on its own schedule: after create-ticket it
 * sends the reply, and later it pushes the match, so there is no get-ticket
 * request. `await_connect` additionally treats the initial ENet CONNECT as a
 * successful result (with an empty reply). Returns 0 on a message, -1 on
 * timeout (still searching), -2 on a disconnect, -3 on a send failure. */
static int mm_receive(ENetHost* host, ENetPeer* peer, const char* send_body, int timeout_ms,
                      char* reply, size_t reply_cap, int* stopped, bool await_connect) {
    if (reply_cap > 0) {
        reply[0] = '\0';
    }
    if (send_body != NULL) {
        ENetPacket* pkt = enet_packet_create(send_body, strlen(send_body), ENET_PACKET_FLAG_RELIABLE);
        if (enet_peer_send(peer, 0, pkt) != 0) {
            enet_packet_destroy(pkt);
            return -3;
        }
        enet_host_flush(host);
    }
    uint64_t deadline = SDL_GetTicks() + (uint64_t)timeout_ms;
    do {
        ENetEvent ev;
        int r = enet_host_service(host, &ev, 250);
        if (r < 0) {
            return -3;
        }
        if (r == 0) {
            continue;
        }
        switch (ev.type) {
        case ENET_EVENT_TYPE_CONNECT:
            if (await_connect) {
                return 0;
            }
            break;
        case ENET_EVENT_TYPE_RECEIVE: {
            size_t len = ev.packet->dataLength;
            const char* data = (const char*)ev.packet->data;
            size_t copy = len < reply_cap - 1 ? len : reply_cap - 1;
            memcpy(reply, data, copy);
            reply[copy] = '\0';
            enet_packet_destroy(ev.packet);
            return 0;
        }
        case ENET_EVENT_TYPE_DISCONNECT:
            return -2;
        default:
            break;
        }
    } while (!(stopped != NULL && *stopped) && SDL_GetTicks() < deadline);
    return -1;
}

/* The value of a top-level string member, without the JSON reader's raw-pointer
 * lifetime trap. */
static void resp_str(const char* body, const char* key, char* out, size_t cap) {
    out[0] = '\0';
    PcSlippiJson* root = NULL;
    char err[32];
    if (pc_slippi_json_parse(body, strlen(body), &root, err, sizeof err)) {
        pc_slippi_json_string(pc_slippi_json_get(root, key), out, cap);
        pc_slippi_json_free(root);
    }
}

/* The account's strings escaped for the create-ticket body. A quote or
 * backslash doubles a byte, so twice each field's bound always fits one that
 * has no control bytes. */
typedef struct EscapedUser {
    char uid[PC_SLIPPI_UID_MAX * 2];
    char play_key[PC_SLIPPI_PLAY_KEY_MAX * 2];
    char connect_code[PC_SLIPPI_CONNECT_CODE_MAX * 2];
    char display_name[PC_SLIPPI_DISPLAY_NAME_MAX * 2];
} EscapedUser;

static bool escape_user(const PcSlippiAccount* a, EscapedUser* out) {
    return pc_slippi_json_escape(a->uid, out->uid, sizeof out->uid) &&
           pc_slippi_json_escape(a->play_key, out->play_key, sizeof out->play_key) &&
           pc_slippi_json_escape(a->connect_code, out->connect_code, sizeof out->connect_code) &&
           pc_slippi_json_escape(a->display_name, out->display_name, sizeof out->display_name);
}

/* The search thread. Owns its ENet host and writes state under the lock. */
static int match_thread(void* user) {
    PcSlippiMatch* m = (PcSlippiMatch*)user;
    char error[128] = {0};
    PcSlippiTicket ticket;
    memset(&ticket, 0, sizeof ticket);
    bool ok = false;

    if (m->use_local_peer) {
        /* No server: synthesize the ticket, as melee-unlocked's local_peer. */
        /* Test overrides, so two instances can pair on loopback in either
         * seat: MELEE_SLIPPI_LOCAL_INDEX (0|1) is this instance's seat,
         * MELEE_SLIPPI_PEER_PORT the other's UDP port, MELEE_SLIPPI_LOCAL_HOST
         * (0|1) who the ticket names the decider. */
        const char* li = getenv("MELEE_SLIPPI_LOCAL_INDEX");
        int local_index = li != NULL && li[0] == '1' ? 1 : 0;
        const char* pp = getenv("MELEE_SLIPPI_PEER_PORT");
        uint16_t base = m->local_port > 0 ? (uint16_t)(m->local_port + 1) : 1;
        if (pp != NULL && pp[0]) {
            base = (uint16_t)(atoi(pp) - (1 - local_index));
        }
        ok = pc_slippi_ticket_local(m->local_port, base, 2, local_index, &ticket);
        if (!ok) {
            snprintf(error, sizeof error, "%s", ticket.error);
        } else {
            const char* lh = getenv("MELEE_SLIPPI_LOCAL_HOST");
            if (lh != NULL && (lh[0] == '0' || lh[0] == '1')) {
                ticket.is_host = lh[0] == '1';
            }
        }
        goto done;
    }

    if (!pc_slippi_net_global_init()) {
        snprintf(error, sizeof error, "enet init failed");
        goto done;
    }
    {
        /* Bind the MM socket to the port we advertise as ipAddressLan: the
         * netplay peer dials it, so it must be the port this host listens on
         * for the game. 0 (an env override) asks ENet to bind and lets the
         * kernel pick, which it then writes back into m->local_port. */
        ENetAddress bind;
        bind.host = ENET_HOST_ANY;
        bind.port = m->local_port;
        ENetHost* host = enet_host_create(&bind, 1, 3, 0, 0);
        if (host == NULL) {
            snprintf(error, sizeof error, "host create failed");
            goto done;
        }
        if (m->local_port == 0) {
            ENetAddress got;
            if (enet_socket_get_address(host->socket, &got) == 0) {
                m->local_port = got.port;
            }
        }
        const char* host_name = getenv("MELEE_SLIPPI_MM_HOST");
        if (host_name == NULL || host_name[0] == '\0') {
            host_name = PC_SLIPPI_MM_HOST;
        }
        const char* port_env = getenv("MELEE_SLIPPI_MM_PORT");
        enet_uint16 port =
            port_env && port_env[0] ? (enet_uint16)atoi(port_env) : PC_SLIPPI_MM_PORT;
        ENetAddress addr;
        if (enet_address_set_host(&addr, host_name) != 0) {
            snprintf(error, sizeof error, "resolve failed");
        } else {
            addr.port = port;
            ENetPeer* peer = enet_host_connect(host, &addr, 3, 0);
            if (peer == NULL) {
                snprintf(error, sizeof error, "connect failed");
            } else {
                char reply[8192];
                EscapedUser esc;
                int stopped = 0;
                int r = mm_receive(host, peer, NULL, PC_SLIPPI_MM_TIMEOUT_MS, reply, sizeof reply,
                                   &stopped, true);
                if (r != 0) {
                    snprintf(error, sizeof error, "no connect to mm server");
                } else if (!escape_user(&m->account, &esc)) {
                    snprintf(error, sizeof error, "account details too long");
                } else {
                    /* create-ticket. connectCode is a byte array; for Direct it
                     * is the target code, for Unranked empty. */
                    char body[2048];
                    int w = snprintf(body, sizeof body, "{\"type\":\"create-ticket\","
                                                       "\"user\":{\"uid\":\"%s\","
                                                       "\"playKey\":\"%s\","
                                                       "\"connectCode\":\"%s\","
                                                       "\"displayName\":\"%s\"},"
                                                       "\"search\":{\"mode\":%d,\"connectCode\":[",
                                     esc.uid, esc.play_key, esc.connect_code, esc.display_name,
                                     m->mode);
                    if (m->mode == PC_SLIPPI_MODE_DIRECT && m->connect_code[0]) {
                        for (const unsigned char* p = (const unsigned char*)m->connect_code; *p; p++) {
                            w += snprintf(body + w, sizeof body - w, "%s%u", p ==
                                              (const unsigned char*)m->connect_code ? "" : ",",
                                          (unsigned)*p);
                        }
                    }
                    char lan[64];
                    local_lan_address(lan, sizeof lan);
                    char lan_addr[80] = "";
                    if (lan[0]) {
                        snprintf(lan_addr, sizeof lan_addr, "%s:%u", lan, (unsigned)m->local_port);
                    }
                    w += snprintf(body + w, sizeof body - w,
                                  "]},\"appVersion\":\"%s\",\"ipAddressLan\":\"%s\"}",
                                  PC_SLIPPI_SEMVER, lan_addr);

                    r = mm_receive(host, peer, body, 5000, reply, sizeof reply, &stopped, false);
                    char type[32];
                    resp_str(reply, "type", type, sizeof type);
                    char err[128];
                    resp_str(reply, "error", err, sizeof err);
                    if (r == -3) {
                        snprintf(error, sizeof error, "send failed");
                    } else if (r != 0) {
                        snprintf(error, sizeof error, "failed to join mm queue");
                    } else if (strcmp(type, "create-ticket-resp") != 0) {
                        snprintf(error, sizeof error, "bad create-ticket response");
                    } else if (err[0]) {
                        snprintf(error, sizeof error, "%s", err);
                    } else {
                        /* Now the server pushes a get-ticket-resp when a match
                         * is assigned. Poll until then (or a 2-minute cap). */
                        uint64_t mm_deadline = SDL_GetTicks() + 120000;
                        while (!m->stop && SDL_GetTicks() < mm_deadline) {
                            r = mm_receive(host, peer, NULL, 2000, reply, sizeof reply, &stopped, false);
                            if (r == -1) {
                                continue; /* still searching */
                            }
                            if (r != 0) {
                                snprintf(error, sizeof error, "lost connection to mm server");
                                break;
                            }
                            resp_str(reply, "type", type, sizeof type);
                            if (strcmp(type, "get-ticket-resp") != 0) {
                                snprintf(error, sizeof error, "invalid seek result");
                                break;
                            }
                            resp_str(reply, "error", err, sizeof err);
                            if (err[0]) {
                                snprintf(error, sizeof error, "%s", err);
                                break;
                            }
                            if (pc_slippi_ticket_parse(reply, strlen(reply), m->local_port,
                                                       &ticket)) {
                                ok = true;
                            } else {
                                snprintf(error, sizeof error, "%s", ticket.error);
                            }
                            break;
                        }
                        if (!ok && error[0] == '\0') {
                            snprintf(error, sizeof error, "search timed out");
                        }
                    }
                }
                enet_peer_disconnect(peer, 0);
            }
        }
        enet_host_destroy(host);
    }

done:
    SDL_LockMutex(m->lock);
    if (ok) {
        m->ticket = ticket;
        SDL_SetAtomicInt(&m->state, PC_SLIPPI_MATCH_SUCCESS);
    } else {
        snprintf(m->error, sizeof m->error, "%s", error[0] ? error : "matchmaking failed");
        SDL_SetAtomicInt(&m->state, PC_SLIPPI_MATCH_ERROR);
    }
    SDL_UnlockMutex(m->lock);
    return 0;
}

PcSlippiMatch* pc_slippi_match_start(const PcSlippiAccount* account, int mode,
                                     const char* connect_code, uint16_t local_port) {
    if (account == NULL || !account->logged_in) {
        return NULL;
    }
    /* Ranked is out of scope; refuse it rather than mis-search. */
    if (mode == PC_SLIPPI_MODE_RANKED) {
        return NULL;
    }
    if (mode == PC_SLIPPI_MODE_DIRECT && (connect_code == NULL || connect_code[0] == '\0')) {
        return NULL;
    }
    PcSlippiMatch* m = (PcSlippiMatch*)calloc(1, sizeof *m);
    if (m == NULL) {
        return NULL;
    }
    m->lock = SDL_CreateMutex();
    if (m->lock == NULL) {
        free(m);
        return NULL;
    }
    m->mode = mode;
    m->account = *account;
    if (connect_code != NULL) {
        snprintf(m->connect_code, sizeof m->connect_code, "%s", connect_code);
    }
    /* Local test peering: synthesize the ticket, no server. */
    const char* local = getenv("MELEE_SLIPPI_LOCAL_PEER");
    m->use_local_peer = local != NULL && local[0] == '1';
    const char* lp = getenv("MELEE_SLIPPI_LOCAL_PORT");
    m->local_port = lp != NULL && lp[0] ? (uint16_t)atoi(lp) : local_port;
    SDL_SetAtomicInt(&m->state, m->use_local_peer ? PC_SLIPPI_MATCH_SEARCHING
                                                  : PC_SLIPPI_MATCH_INITIALIZING);
    m->thread = SDL_CreateThread(match_thread, "slippi match", m);
    if (m->thread == NULL) {
        SDL_DestroyMutex(m->lock);
        free(m);
        return NULL;
    }
    return m;
}

void pc_slippi_match_stop(PcSlippiMatch* m) {
    if (m == NULL) {
        return;
    }
    SDL_LockMutex(m->lock);
    m->stop = true;
    SDL_UnlockMutex(m->lock);
    if (m->thread != NULL) {
        SDL_WaitThread(m->thread, NULL);
    }
    SDL_DestroyMutex(m->lock);
    free(m);
}

PcSlippiMatchState pc_slippi_match_state(const PcSlippiMatch* m) {
    if (m == NULL) {
        return PC_SLIPPI_MATCH_IDLE;
    }
    return (PcSlippiMatchState)SDL_GetAtomicInt((SDL_AtomicInt*)&m->state);
}

const char* pc_slippi_match_error(const PcSlippiMatch* m) {
    return m == NULL ? "" : m->error;
}

bool pc_slippi_match_result(const PcSlippiMatch* m, PcSlippiTicket* out) {
    if (m == NULL || out == NULL) {
        return false;
    }
    /* The result is written before SUCCESS, so a reader that sees SUCCESS has a
     * complete ticket. */
    if (pc_slippi_match_state(m) != PC_SLIPPI_MATCH_SUCCESS) {
        return false;
    }
    *out = m->ticket;
    return true;
}
