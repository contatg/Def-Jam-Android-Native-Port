#if defined(__ANDROID__)
/* Adapted from sweeax-exe/ffny-online@5664adca (MIT); see FFNY-ONLINE-LICENSE.txt. */
/* Online play through a relay server. See netplay.h, docs/netplay.md and the wire
 * format in docs/netplay-protocol.md.
 *
 * Threads. A network thread owns the socket: it talks to the relay, keeps the
 * session alive, streams this PC's controller and stores what arrives. The game
 * thread calls the hooks below from the title's own functions; a fight step waits
 * there until every other player's input for that step has arrived. One critical
 * section covers the shared state.
 *
 * Two ways in. netplay_host/netplay_join make a two-player room by code; both
 * players then pick the match in the menus. netplay_lobby enters the hub: rooms,
 * quick match and the venue vote run on the server, and its START names the
 * match, which automatch.c sets up on every PC from the main menu.
 *
 * Lockstep. Fight step s uses, for every player, the pad-table entry (all 32
 * bytes: buttons, a second word, both sticks) that the player's PC captured
 * `delay` steps earlier. All PCs therefore feed the same entries to the same
 * steps, whatever their frame timing, and the first `delay` steps of a match get
 * an empty entry on all. The title polls the pad once per pass and may run
 * several steps a pass; the last word of an entry (always 0 from the poller) is
 * set to a marker when an entry is injected, so a reader can tell a fresh poll
 * from what it injected itself. A player is a position 0..players-1, the game's
 * player number; in a hub room positions follow the room slots in order. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _CRT_SECURE_NO_WARNINGS
#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif
#include "host.h"
#if defined(__ANDROID__)
#include "android_netplay.h"
#include "ffny_android_protocol.h"
#include "detmath/detmath.h"
#include "online_delay126.h"
#include "online_wait128.h"
#include "platform/android_hitch.h"
#include "platform/android_sched.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <netdb.h>
#include <unistd.h>
#include <strings.h>
#include <errno.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#define _stricmp strcasecmp
#endif
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp/gen/recomp_types.h"
#include "netplay.h"
#include "automatch.h"
#include "online_data.h"
#include "account.h"
#include "rollback.h"
#include "android_rollback131.h"
#include "platform/xbox_rollback_hooks.h"
#include "test_observations.h"

#define G_MATCH_PHASE   0x003B8F30u     /* test_telemetry.c G_PHASE */
#define G_MATCH_WINNER  0x003B8F40u     /* test_telemetry.c G_WINNER: the winner's fighter slot */
#define G_CROWD_RECORDS 0x003494E0u     /* count of the crowd's "audience public" records */

#if !defined(_WIN32) && !defined(__ANDROID__)

int netplay_host(const char *s, const char *n, int d) { (void)s; (void)n; (void)d; return -1; }
int netplay_join(const char *s, const char *n, const char *r) { (void)s; (void)n; (void)r; return -1; }
void netplay_stop(void) {}
enum NetplayState netplay_state(void) { return NETPLAY_OFF; }
void netplay_status(char *t, int size) { if (size > 0) snprintf(t, (size_t)size, "Online play is Windows only for now."); }
void netplay_room(char *c, int size) { if (size > 0) c[0] = 0; }
void netplay_autostart(void) {}
int netplay_active(void) { return 0; }
int netplay_in_fight(void) { return 0; }
int netplay_lobby(const char *s, const char *n) { (void)s; (void)n; return -1; }
void netplay_lobby_list(int m) { (void)m; }
void netplay_lobby_who(void) {}
int netplay_lobby_players(NetPlayer *o, int m, int *a, int *b) { (void)o; (void)m; if (a) *a = -1; if (b) *b = 0; return -1; }
int netplay_lobby_rooms(NetRoomInfo *o, int m, unsigned *v) { (void)o; (void)m; if (v) *v = 0; return 0; }
void netplay_lobby_create(const NetRoomSettings *s) { (void)s; }
void netplay_lobby_join(const char *c) { (void)c; }
void netplay_lobby_quick(int m, int x) { (void)m; (void)x; }
void netplay_lobby_settings(const NetRoomSettings *s) { (void)s; }
void netplay_lobby_ready(int r, int f) { (void)r; (void)f; }
void netplay_lobby_vote(int c) { (void)c; }
void netplay_lobby_leave(void) {}
void netplay_lobby_room(NetRoom *o) { memset(o, 0, sizeof(*o)); }
void netplay_lobby_ranked(int f) { (void)f; }
int netplay_lobby_queue(int *w, int *s, char *t, int n) { (void)w; (void)s; if (t && n > 0) t[0] = 0; return NETQUEUE_NONE; }
int netplay_in_lobby(void) { return 0; }
void netplay_sync_point(void) {}
void netplay_on_main_menu(void) {}
int netplay_match_abandoned(void) { return 0; }
void netplay_on_match_start(void) {}
void netplay_on_pad_pass(void) {}
void netplay_on_step(void) {}
void netplay_on_update(void) {}
extern void sub_00137010(void);
void netplay_sound_command(void) { sub_00137010(); }
void *netplay_sound_handler(uint32_t va) { (void)va; return NULL; }
void netplay_on_pad_read(uint32_t a) { (void)a; }
void netplay_on_seed(uint32_t e, int w, uint32_t x) { (void)e; (void)w; (void)x; }
void netplay_on_result(uint32_t w, uint32_t l, uint32_t c) { (void)w; (void)l; (void)c; }
int netplay_toast(char *t, int n) { (void)t; (void)n; return 0; }
int netplay_debug_json(char *o, int n) { return snprintf(o, (size_t)n, "{}"); }
extern void sub_00081170(void);
void netplay_crowd_update(void) { sub_00081170(); }

#else

#include "input_host.h"
#if defined(__ANDROID__)
#include "platform/android_thread_policy.h"
#endif

#ifndef DEFJAM_VERSION
#define DEFJAM_VERSION "dev"
#endif

#define DEFJAM_NETPLAY_PC_PROFILE "0.7.2" /* pinned FFNY Online CMake version, rules4 */
#define DEFAULT_PORT  27777
#define PROJECT_SERVER_NAME    "play.defjam.online"
#define PROJECT_SERVER_ADDRESS "49.12.78.32"
#define PROTO         1
#define T_HELLO       0x01
#define T_WELCOME     0x02
#define T_PEER        0x03
#define T_DATA        0x04
#define T_PING        0x05
#define T_PONG        0x06
#define T_BYE         0x07
#define T_ERROR       0x08
/* the lobby extension */
#define T_LIST        0x10
#define T_ROOMS       0x11
#define T_CREATE_PRIVATE 0x20
#define T_JOIN_PRIVATE 0x21
#define T_CREATE      0x12
#define T_JOIN        0x13
#define T_QUICK       0x14
#define T_SETTINGS    0x15
#define T_READY       0x16
#define T_VOTE        0x17
#define T_LEAVE       0x18
#define T_ROOM        0x19
#define T_START       0x1A
#define T_OVER        0x1B
#define T_WHO         0x1C      /* who is online (relays since build 13) */
#define T_PLAYERS     0x1D
#define T_RANKED      0x1E      /* the ranked queue (relays since build 15) */
#define T_QUEUE       0x1F
/* FFNY partner transport preparation. The Android relay remains disabled
 * until the full tagged game/client migration and a joint test are ready. */
#define T_DEVICE_ID 0x30
#define T_CAPS132     0x20
#define CAPS_ROLLBACK132 2u
#define TICKET_LEN    16        /* RANKED: the account service's one-time ticket */

/* DATA payloads between the game copies (opaque to the relay) */
#define P_GAMEHELLO   1     /* host -> guest: u32 session seed, u8 input delay, u8 lockstep version */
#define P_GAMEACK     2     /* guest -> host */
#define P_LIVEPAD     3     /* u32 sequence, 18 bytes XBOX_GAMEPAD */
#define P_STEPS       4     /* u32 match, u32 last step, u8 count, count x 32 bytes */
#define P_HASH        5     /* u32 match, u32 step, u32 hash */
#define P_RTT         6     /* hub: u32 START seed, u16 this PC's ping to the relay in ms */
#define LOCKSTEP_VERSION 4     /* 3: builds 19-20, rollback on; 4: build 21, rollback off again, reclaim on */

#define MAXP          NETPLAY_MAX_PLAYERS
#define PAD_TABLE     0x003B8F48u
#define ACTORS        0x003B92E0u
#define STRIDE        0x12E8u
#define ENTRY         32
#define MARK          0x594C504Eu   /* "NPLY" in the entry's last word */
#define RING          512           /* steps of input kept per player */
#define REDUNDANT     8             /* steps repeated in every STEPS packet */
#define HASH_EVERY    60
#define STALL_LIMIT_MS 10000        /* mid-fight: a player has gone */
#define START_LIMIT_MS 90000        /* the first steps: another PC may still be loading the venue */
#define DECIDED_LIMIT_MS 1000       /* after the KO: a PC that went on ahead is not a lost player */
#define LOBBY_DELAY   3             /* hub matches: until the delay is agreed (P_RTT) */
#define DELAY_MIN     2
#define DELAY_MAX     10
#define RTT_WAIT_MS   5000          /* at the sync point: how long to wait for every player's ping */
#define RETRY_MS      400           /* lobby requests are repeated until the server answers */
#define RETRIES       8
#define RB_AHEAD      8             /* rollback: steps a PC runs on predicted entries before it waits */
#define RB_LESS       4             /* hub matches where every PC rolls back: the delay is this much shorter */

static LONG np_load(const volatile LONG *p){return __atomic_load_n(p,__ATOMIC_ACQUIRE);}

static CRITICAL_SECTION g_cs;
static volatile LONG s_mobile_cancel;
/* NETREJECT132: retained across Stop/Start, but scoped to the selected endpoint.
 * A cooldown is not a server unlock, and no PC build number is fabricated. */
static ULONGLONG s_relay_retry_after132;
static char s_relay_rejected132[128];
static int project_server_endpoint(const char *server);
static int netplay_relay_allows_rollback132(void);
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static struct {
    volatile LONG state;            /* enum NetplayState */
    volatile LONG stop;
    HANDLE thread;
    SOCKET sock;
    struct sockaddr_storage server;
    int server_len;
    char server_text[128];
    int creating;                   /* code rooms: 1 host, 0 guest */
    int lobby;                      /* 1: the hub */
    char room[7];
    char name[25], peer_name[25];
    uint8_t build[8];
    uint8_t token[8];
    int have_token;
    volatile LONG relay_caps132; /* -1 until a public relay answers; atomic cross-thread */
    ULONGLONG caps_sent132;
    int caps_tries132;
    int device_tries, device_ack;
    ULONGLONG device_sent;
    int slot;                       /* this client's slot in its room */
    char status[192];
    /* session */
    uint32_t session;
    int delay;
    int acked;
    int players, pos;               /* players in the match, this PC's position */
    int fighters;                   /* in the fight, with the CPU ones */
    int pos_of_slot[MAXP];          /* room slot -> position, -1 none */
    /* controllers */
    XBOX_GAMEPAD remote_pad[MAXP];
    uint32_t remote_seq[MAXP], local_seq;
    DWORD remote_packet[MAXP];
    XBOX_GAMEPAD last_sent;
    ULONGLONG last_pad_sent;
    /* link */
    ULONGLONG last_rx, last_ping, hello_sent, started;
    uint32_t ping_nonce;
    int rtt_ms;
    int rtt_recent[5], rtt_next;    /* the last pings, for a jitter margin */
    /* lockstep (step numbers count from 1 at each match start) */
    volatile LONG lockstep;         /* connected and not abandoned */
    uint32_t match, match_count, step;
    uint8_t latest_local[ENTRY];
    uint8_t in[MAXP][RING][ENTRY];
    uint32_t in_tag[MAXP][RING];    /* step + 1 (0 = empty) */
    uint32_t in_match[MAXP][RING];
    HANDLE arrived;
    uint32_t local_hash[16][3], remote_hash[16][3];   /* match, step, hash */
    int desync;
    uint32_t desync_step;
    struct { uint32_t which, count; } seeds[32];
    /* the hub */
    NetRoom roomv;
    NetRoomInfo rooms[32];
    NetPlayer who[40];              /* the last PLAYERS */
    int who_count, who_online, who_rooms;
    int queue_state, queue_waited, queue_searching;     /* the ranked queue (NETQUEUE_*) */
    char queue_text[96];
    int room_count;
    unsigned rooms_version;
    uint8_t pend[300];              /* the request waiting for an answer */
    int pend_len, pend_tries;
    ULONGLONG pend_sent;
    int match_live;                 /* START received, OVER not yet sent */
    int match_seen;                 /* the game has started that match */
    int over_pending;
    int start_this_play;            /* a START arrived since the room entered phase 2 */
    int peer_delay126[MAXP];       /* immutable per START, from the same RTT packet */
    int peer_rtt[MAXP];             /* each position's ping as it reported it for this START, -1 not yet */
    int delay_fixed;                /* the match's delay is agreed */
    ULONGLONG last_rtt_sent;
    ULONGLONG last_recover;
    uint32_t start_seed;
    int start_slots[MAXP];
    /* the room match for its result report (account.c): set at START, sent once */
    AutoMatch report_m;
    char report_names[MAXP][25];
    char report_key[32];
    int reported, fighting_seen, last_late;
    int seed_bypass;                /* crowd_reset seeds a generator with its own words */
    /* rollback (rollback.h): a fighting step runs on a prediction of each entry that has not
     * arrived (that player's last one); when the real one differs, the game goes back to the
     * save state taken before that step and runs the steps again. */
    uint8_t used[MAXP][RING][ENTRY];    /* each other player's entry as its step ran */
    uint32_t used_tag[MAXP][RING];      /* step + 1 */
    uint8_t used_pred[MAXP][RING];      /* 1: a prediction */
    uint32_t final_step;                /* every step up to this one ran on the real entries */
    uint32_t updates;                   /* game updates since the match's reference copy */
    uint32_t hash_at[RING], hash_tag[RING], hash_sent;
    int rb_on, rb_ref_due, resim, predicting;
    int rb_fought;                      /* the phase word said "fighting" in this match */
    int peer_rb[MAXP];                  /* each position rolls back (its P_RTT says so) */
    struct {
        int valid;
        uint32_t step, update;          /* the step about to run, the update about to start */
        long long test_step;
        uint32_t crowd_gen[6];
        int crowd_gen_on;
        uint32_t seeds[32][2];
    } snap[ROLLBACK_SLOTS];
    unsigned rb_count, rb_steps, rb_deepest, rb_failed;
    double rb_ms_max, rb_ms_total;
    char toast[160];                /* a notice for the fight (netplay_toast), until toast_until */
    ULONGLONG toast_until;
} N;

/* Offline verification replays the ORIGINAL pad entries, never today's touch input. */
static uint8_t verify_pad131[MAXP][RING][ENTRY];
static uint32_t verify_pad_tag131[MAXP][RING],verify_pad_seen131[MAXP];
static uint32_t verify_warm_base131,verify_last_step131;

static HOST_TLS int replay_audio131,replay_input_failed131;
static void rollback_abort131(const char *reason);

/* Rollback (rollback.h) is on with RECOMP_ROLLBACK=on (off by default since build 21); off, every step waits for every entry. */
static int rollback_wanted(void) { return rollback_capable131(); }

static BOOL CALLBACK init_once(PINIT_ONCE o, PVOID p, PVOID *c)
{
#if defined(_WIN32)
    WSADATA wsa;
#endif
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_cs);
#if defined(_WIN32)
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    N.sock = INVALID_SOCKET;
    N.arrived = CreateEventW(NULL, FALSE, FALSE, NULL);
    N.who_online = -1;              /* no PLAYERS yet */
    N.queue_state = NETQUEUE_NONE;
    return TRUE;
}

static void set_status(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    EnterCriticalSection(&g_cs);
    vsnprintf(N.status, sizeof(N.status), format, args);
    LeaveCriticalSection(&g_cs);
    va_end(args);
    fprintf(stderr, "[NETPLAY] %s\n", N.status);
    fflush(stderr);
}

/* A short notice the overlay shows over the fight: a wait, a desync, a player gone. */
static void toast(unsigned ms, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    EnterCriticalSection(&g_cs);
    vsnprintf(N.toast, sizeof(N.toast), format, args);
    N.toast_until = GetTickCount64() + ms;
    LeaveCriticalSection(&g_cs);
    va_end(args);
    fprintf(stderr, "[NETPLAY] notice: %s\n", N.toast);
}

static const char *player_name(int pos);
static void put32(uint8_t *p, uint32_t v);
static void send_data(const uint8_t *payload, int len);

int netplay_toast(char *out, int size)
{
    int on;
    InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
    EnterCriticalSection(&g_cs);
    on = N.toast[0] && GetTickCount64() < N.toast_until;
    if (on) snprintf(out, size, "%s", N.toast);
    LeaveCriticalSection(&g_cs);
    return on;
}

static const char *player_name(int pos)
{
    return pos >= 0 && pos < MAXP && N.report_names[pos][0] ? N.report_names[pos] : "the other player";
}

static uint32_t mix32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

/* ---- wire helpers ------------------------------------------------------------ */

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

static int header(uint8_t *b, uint8_t type)
{
    b[0] = 'D'; b[1] = 'J'; b[2] = 'N'; b[3] = 'P'; b[4] = PROTO; b[5] = type;
    return 6;
}

static void send_raw(const uint8_t *b, int len)
{
    EnterCriticalSection(&g_cs);
    if (N.sock != INVALID_SOCKET)
        sendto(N.sock, (const char *)b, len, MSG_DONTWAIT, (const struct sockaddr *)&N.server, N.server_len);
    LeaveCriticalSection(&g_cs);
}

static void send_hello(void)
{
    uint8_t b[64];
    int n = header(b, T_HELLO), len = (int)strlen(N.name);
    b[n++] = N.lobby ? 2 : N.creating ? 0 : 1;
    memset(b + n, 0, 6);
    if (!N.lobby && !N.creating) memcpy(b + n, N.room, 6);
    n += 6;
    memcpy(b + n, N.build, 8); n += 8;
    b[n++] = (uint8_t)len;
    memcpy(b + n, N.name, (size_t)len); n += len;
    if (project_server_endpoint(N.server_text)) {
        ffny_android_hello_suffix(b + n, 0);
        n += 9;
    }
    send_raw(b, n);
}

static void netplay_send_caps132(void)
{
    uint8_t b[18];
    int n = header(b, T_CAPS132);
    memcpy(b + n, N.token, 8); n += 8;
    /* Ordinary rooms only. Do not claim King of the Hill/watch support. */
    put32(b + n, FFNY_ANDROID_CAPS); n += 4;
    send_raw(b, n);
}

static void send_partner_device_id(void)
{
    uint8_t id[16], b[6 + 8 + 17];
    if (!android_online_device_id(id)) return;
    int n = header(b, T_DEVICE_ID);
    memcpy(b + n, N.token, 8); n += 8;
    ffny_android_device_body(b + n, id);
    send_raw(b, n + 17);
}

static void send_token_only(uint8_t type, uint32_t extra, int with_extra)
{
    uint8_t b[32];
    int n = header(b, type);
    memcpy(b + n, N.token, 8); n += 8;
    if (with_extra) { put32(b + n, extra); n += 4; }
    send_raw(b, n);
}

static void send_data(const uint8_t *payload, int len)
{
    uint8_t b[1200];
    int n = header(b, T_DATA);
    if (!N.have_token || len <= 0 || len > 1150) return;
    memcpy(b + n, N.token, 8); n += 8;
    memcpy(b + n, payload, (size_t)len); n += len;
    send_raw(b, n);
}

/* A hub request: header, token, then `body`. Repeated every RETRY_MS until the server answers
 * (the relay treats a repeat as the same request). A new request replaces a waiting one. */
static void request(uint8_t type, const uint8_t *body, int len)
{
    uint8_t b[sizeof(N.pend)];
    int n;
    if (!N.lobby || !N.have_token || len < 0 || len > (int)sizeof(b) - 14) return;
    if (project_server_endpoint(N.server_text) &&
        (type == T_CREATE_PRIVATE || type == T_JOIN_PRIVATE || type == T_RANKED)) return;
    n = header(b, type);
    memcpy(b + n, N.token, 8); n += 8;
    if (len) memcpy(b + n, body, (size_t)len);
    n += len;
    EnterCriticalSection(&g_cs);
    memcpy(N.pend, b, (size_t)n);
    N.pend_len = n;
    N.pend_tries = 1;
    N.pend_sent = GetTickCount64();
    N.roomv.error[0] = 0;
    LeaveCriticalSection(&g_cs);
    send_raw(b, n);
}

static void answered(void)
{
    SecureZeroMemory(N.pend,sizeof(N.pend));
    N.pend_len = 0;
}

/* ---- the other players' controllers, as the title's other pads ---------------- */

static DWORD remote_slot_state(int slot, XBOX_INPUT_STATE *state)
{
    if (np_load(&N.state) != NETPLAY_CONNECTED || slot < 0 || slot >= N.players || slot == N.pos)
        return ERROR_DEVICE_NOT_CONNECTED;
    EnterCriticalSection(&g_cs);
    state->dwPacketNumber = N.remote_packet[slot];
    state->Gamepad = N.remote_pad[slot];
    LeaveCriticalSection(&g_cs);
    return ERROR_SUCCESS;
}

/* A hub match in lockstep: the title reads its pause (START) straight from the controllers, not
 * from the agreed inputs, so a press paused one game of the room only (and the other PCs saw it
 * on that player's port, through the live pads, and opened the pause there as that player): the
 * games parted, or a player kept everyone waiting. A port that read as unplugged mid-fight brought
 * up "Controller Disconnected" on one PC the same way. While a hub match runs, START reaches no
 * controller and every port reads as plugged in (players' Discord reports, 2026-10-22). */
static void fight_input(void)
{
    static LONG on = -1;
    LONG want = np_load(&N.lockstep) && N.match_count;    /* the match itself, not the menus before it */
    if (InterlockedExchange(&on, want) != want)
        xbox_HostInputSetNetplayFight(want ? 0x0010 : 0);   /* the START button */
}

static void assign_input(int on)
{
    unsigned mask = 0;
    int i;
    if (!on) { xbox_HostInputSetNetplay(0, 0, 0, NULL); fight_input(); return; }
    for (i = 0; i < N.players; i++)
        if (i != N.pos) mask |= 1u << i;
    xbox_HostInputSetNetplayMulti(1, N.pos, mask, remote_slot_state);
    fight_input();
}

static void reset_remote_pads(void)
{
    EnterCriticalSection(&g_cs);
    memset(N.remote_pad, 0, sizeof(N.remote_pad));
    memset(N.remote_seq, 0, sizeof(N.remote_seq));
    LeaveCriticalSection(&g_cs);
}

static void become_connected(void)
{
    if (!N.lobby) {
        N.match_count = N.step = 0;
        memset(N.in_tag, 0, sizeof(N.in_tag));
    }
    memset(N.seeds, 0, sizeof(N.seeds));
    N.desync = 0;
    InterlockedExchange(&N.state, NETPLAY_CONNECTED);
    InterlockedExchange(&N.lockstep, 1);
    assign_input(1);
    if (N.lobby)
        set_status("Match found: %d players, you are player %d. It starts by itself.", N.players, N.pos + 1);
    else
        set_status("Connected to %s. You are player %d. Both choose Versus; player 1 picks first.",
                   N.peer_name[0] ? N.peer_name : "the other player", N.pos + 1);
}

static void lose_peer(const char *why)
{
    InterlockedExchange(&N.lockstep, 0);
    assign_input(0);
    SetEvent(N.arrived);
    if (N.creating && !np_load(&N.stop)) {
        InterlockedExchange(&N.state, NETPLAY_WAITING);
        N.acked = 0;
        set_status("%s Room %s is open again; waiting for a player.", why, N.room);
    } else {
        InterlockedExchange(&N.state, NETPLAY_FAILED);
        set_status("%s", why);
    }
}

/* The hub match is over for this PC: tell the server and go back to the room. */
/* The contiguous window's use (64 MiB: textures, vertex data, rollback's audio arena) at a match's
 * start and end: when it runs dry the game draws with buffers it did not get (white blocks, the
 * announcer word sheet). Reports from build 19 showed it full at a second match's start. */
extern uint32_t xbox_ContiguousLiveBytes(void);
extern uint32_t xbox_ContiguousAllocatedBytes(void);
static void log_contiguous(const char *when)
{
    fprintf(stderr, "[MEM] contiguous memory %s: %.1f MB in use, %.1f MB reached\n", when,
            xbox_ContiguousLiveBytes() / 1048576.0, xbox_ContiguousAllocatedBytes() / 1048576.0);
}

static void end_hub_match(const char *why)
{
    log_contiguous("at the match end");
    InterlockedExchange(&N.lockstep, 0);
    assign_input(0);
    SetEvent(N.arrived);
    N.match_live = N.match_seen = 0;
    N.over_pending = 1;
    request(T_OVER, NULL, 0);
    InterlockedExchange(&N.state, NETPLAY_LOBBY);
    set_status("%s", why);
}

/* ---- what arrives -------------------------------------------------------------- */

static unsigned g_steps_rx[MAXP], g_steps_last[MAXP];
static void store_steps(int from, const uint8_t *p, int len)
{
    uint32_t match, last;
    int count, i;
    if (len < 9 || from < 0 || from >= MAXP) return;
    match = get32(p); last = get32(p + 4); count = p[8];
    g_steps_rx[from]++;
    g_steps_last[from] = last;
    if (count<1 || last<(uint32_t)count || len < 9 + count * ENTRY) return;
    EnterCriticalSection(&g_cs);
    /* A delayed packet from the previous match must not overwrite a valid
     * current-match ring slot and manufacture another missing-input wait. */
    if(match!=N.match){LeaveCriticalSection(&g_cs);return;}
    for (i = 0; i < count; i++) {
        uint32_t s = last - (uint32_t)(count - 1 - i);
        uint32_t k = s % RING;
        if (s == 0) continue;
        memcpy(N.in[from][k], p + 9 + i * ENTRY, ENTRY);
        N.in_tag[from][k] = s + 1;
        N.in_match[from][k] = match;
    }
    LeaveCriticalSection(&g_cs);
    SetEvent(N.arrived);
}

static unsigned g_hash_matches;     /* checksums compared and equal, this session */

/* Compare every checksum this PC and another have for the same match and step, once each. */
static void check_hashes(void)
{
    int i, j;
    for (i = 0; i < 16; i++)
        for (j = 0; j < 16; j++) {
            if (!N.local_hash[i][1] || N.local_hash[i][0] != N.remote_hash[j][0] ||
                N.local_hash[i][1] != N.remote_hash[j][1])
                continue;
            if (N.local_hash[i][2] != N.remote_hash[j][2]) {
                if (!N.desync) {
                    N.desync = 1;
                    N.desync_step = N.local_hash[i][1];
                    fprintf(stderr, "[NETPLAY] desync in match %08X at step %u (local %08X, remote %08X)\n",
                            N.local_hash[i][0], N.local_hash[i][1], N.local_hash[i][2], N.remote_hash[j][2]);
                    toast(8000, "The games went out of sync: this match won't count.");
#if defined(__ANDROID__)
                    InterlockedExchange(&N.lockstep, 0);
                    InterlockedExchange(&N.stop, 1);
                    InterlockedExchange(&N.state, NETPLAY_FAILED);
                    assign_input(0); SetEvent(N.arrived);
                    set_status("Crossplay desync: disconnected. This fight is now offline.");
#endif
                }
            } else if (++g_hash_matches % 10 == 0) {
                fprintf(stderr, "[NETPLAY] in step: %u checksums compared and equal (match %08X, step %u)\n",
                        g_hash_matches, N.local_hash[i][0], N.local_hash[i][1]);
            }
            N.remote_hash[j][1] = 0;    /* compared */
        }
}

/* Hub: this PC's ping for the current START, so every PC derives the same input delay. */
static void send_rtt(void)
{
    uint8_t p[10];
    int rtt = N.peer_rtt[N.pos] < 0 ? 0 : N.peer_rtt[N.pos];
    p[0] = P_RTT;
    put32(p + 1, N.start_seed);
    p[5] = (uint8_t)rtt;
    p[6] = (uint8_t)(rtt >> 8);
    p[7] = (uint8_t)N.peer_rb[N.pos];   /* builds before 17 send 7 bytes: no rollback */
    p[8]=0xA6;p[9]=1; /* optional capability, old clients ignore trailing bytes */
    send_data(p, 10);
    N.last_rtt_sent = GetTickCount64();
}

/* The delay every PC derives from the same pings: the slowest path between two players goes
 * through the relay, about half of each one's round trip, plus a step of margin. */
static int agreed_delay(void)
{
    int required=0,extended=0;
    int d=online_delay126(N.peer_rtt,N.peer_rb,N.peer_delay126,N.players,&required,&extended);
    fprintf(stderr,"[NETDELAY126] required=%d applied=%d budget_ms=%.1f all_capable=%d legacy_limit=10; fixed for this match\n",
            required,d,d>0?d*1000.0/60:0.0,extended);
    if(d>0&&required>d)fprintf(stderr,"[NETDELAY126] latency exceeds agreed budget; older peers or very high RTT can still stall lockstep\n");
    return d;
}

static void on_data(int from, const uint8_t *p, int len)
{
    if (len < 1 || from < 0 || from >= MAXP || from == N.pos) return;
    switch (p[0]) {
    case P_GAMEHELLO:
        if (!N.lobby && !N.creating && len >= 7 && p[6] == LOCKSTEP_VERSION) {
            uint8_t ack = P_GAMEACK;
            N.session = get32(p + 1);
            N.delay = p[5] >= 1 && p[5] <= 12 ? p[5] : 3;
            send_data(&ack, 1);
            if (np_load(&N.state) != NETPLAY_CONNECTED) become_connected();
        }
        break;
    case P_GAMEACK:
        if (!N.lobby && N.creating && np_load(&N.state) == NETPLAY_SYNCING) {
            N.acked = 1;
            become_connected();
        }
        break;
    case P_LIVEPAD:
        if (len >= 5 + (int)sizeof(XBOX_GAMEPAD)) {
            uint32_t seq = get32(p + 1);
            EnterCriticalSection(&g_cs);
            if ((int32_t)(seq - N.remote_seq[from]) > 0) {
                static int seen, pressed;
                N.remote_seq[from] = seq;
                memcpy(&N.remote_pad[from], p + 5, sizeof(XBOX_GAMEPAD));
                N.remote_packet[from]++;
                if (!seen++)
                    fprintf(stderr, "[NETPLAY] the other player's controller is arriving (player %d)\n", from + 1);
                if (!pressed && (N.remote_pad[from].wButtons || N.remote_pad[from].bAnalogButtons[0] > 32)) {
                    pressed = 1;
                    fprintf(stderr, "[NETPLAY] first press from the other player's controller (buttons %04X)\n",
                            N.remote_pad[from].wButtons);
                }
            }
            LeaveCriticalSection(&g_cs);
        }
        break;
    case P_STEPS:
        store_steps(from, p + 1, len - 1);
        break;
    case P_RTT:
        if (N.lobby && N.match_live && len >= 7 && get32(p + 1) == N.start_seed) {
            EnterCriticalSection(&g_cs);
            int had = N.peer_rtt[from];
            if(N.match_live && get32(p+1)==N.start_seed && !N.delay_fixed && had<0){
                N.peer_rtt[from] = p[5] | (p[6] << 8);
                N.peer_rb[from] = len >= 8 && p[7] == 1;
                N.peer_delay126[from] = len>=10&&p[8]==0xA6&&p[9]==1;
            }
            LeaveCriticalSection(&g_cs);
            SetEvent(N.arrived);
            if (N.delay_fixed && had < 0) send_rtt();   /* it may still be waiting for ours */
        }
        break;
    case P_HASH:
        if (len >= 13) {
            EnterCriticalSection(&g_cs);
            memmove(N.remote_hash[1], N.remote_hash[0], sizeof(N.remote_hash[0]) * 15);
            N.remote_hash[0][0] = get32(p + 1);
            N.remote_hash[0][1] = get32(p + 5);
            N.remote_hash[0][2] = get32(p + 9);
            check_hashes();
            LeaveCriticalSection(&g_cs);
        }
        break;
    }
}

/* ---- the hub's messages -------------------------------------------------------- */

typedef struct { const uint8_t *p; int n, i, bad; } Reader;

static int rd8(Reader *r)
{
    if (r->i >= r->n) { r->bad = 1; return 0; }
    return r->p[r->i++];
}

static void rdbytes(Reader *r, void *out, int count)
{
    if (r->i + count > r->n) { r->bad = 1; memset(out, 0, (size_t)count); return; }
    memcpy(out, r->p + r->i, (size_t)count);
    r->i += count;
}

static void rdstr(Reader *r, char *out, int size)
{
    int len = rd8(r), keep = len < size - 1 ? len : size - 1;
    if (r->i + len > r->n) { r->bad = 1; out[0] = 0; return; }
    memcpy(out, r->p + r->i, (size_t)keep);
    out[keep] = 0;
    r->i += len;
}

static void rdsettings(Reader *r, NetRoomSettings *s)
{
    memset(s, 0, sizeof(*s));
    rdstr(r, s->name, sizeof(s->name));
    s->is_public = rd8(r);
    s->mode = rd8(r);
    s->max = rd8(r);
    s->rounds = rd8(r);
    s->pool_count = rd8(r);
    if (s->pool_count > 32) { r->bad = 1; s->pool_count = 0; return; }
    rdbytes(r, s->pool, s->pool_count);
}

static int put_settings(uint8_t *b, const NetRoomSettings *s)
{
    int n = 0, len = (int)strlen(s->name), pool = s->pool_count;
    if (len > 24) len = 24;
    if (len == 0) { b[n++] = 4; memcpy(b + n, "Room", 4); n += 4; }
    else { b[n++] = (uint8_t)len; memcpy(b + n, s->name, (size_t)len); n += len; }
    b[n++] = s->is_public ? 1 : 0;
    b[n++] = (uint8_t)s->mode;
    b[n++] = (uint8_t)(s->max < 2 ? 2 : s->max > MAXP ? MAXP : s->max);
    b[n++] = (uint8_t)s->rounds;
    if (pool < 1) pool = 0;
    if (pool > 32) pool = 32;
    b[n++] = (uint8_t)pool;
    memcpy(b + n, s->pool, (size_t)pool); n += pool;
    return n;
}

static void on_rooms(const uint8_t *b, int len)
{
    Reader r = { b, len, 0, 0 };
    NetRoomInfo list[32];
    int count = rd8(&r), i;
    if (count > 32) count = 32;
    for (i = 0; i < count && !r.bad; i++) {
        rdbytes(&r, list[i].code, 6);
        list[i].code[6] = 0;
        rdstr(&r, list[i].name, sizeof(list[i].name));
        list[i].mode = rd8(&r);
        list[i].players = rd8(&r);
        list[i].max = rd8(&r);
        list[i].phase = rd8(&r);
    }
    if (r.bad) return;
    EnterCriticalSection(&g_cs);
    memcpy(N.rooms, list, sizeof(list[0]) * (size_t)count);
    {static unsigned reports;static int last_count=-1;
     if(reports++<8||last_count!=count)fprintf(stderr,"[NETLIST135] accepted room-list count=%d; unsupported hill rooms remain excluded\n",count);
     last_count=count;}
    N.room_count = count;
    N.rooms_version++;
    if (N.pend_len && (N.pend[5] == T_LIST || N.pend[5] == T_LEAVE)) answered();
    LeaveCriticalSection(&g_cs);
}

static void on_players(const uint8_t *b, int len)
{
    Reader r = { b, len, 0, 0 };
    NetPlayer list[40];
    int online, rooms, count, i;
    online = rd8(&r);
    online |= rd8(&r) << 8;
    rooms = rd8(&r);
    rooms |= rd8(&r) << 8;
    count = rd8(&r);
    if (count > 40) count = 40;
    for (i = 0; i < count && !r.bad; i++) {
        rdstr(&r, list[i].name, sizeof(list[i].name));
        list[i].state = rd8(&r);
    }
    if (r.bad) return;
    EnterCriticalSection(&g_cs);
    memcpy(N.who, list, sizeof(list[0]) * (size_t)count);
    N.who_count = count;
    N.who_online = online;
    N.who_rooms = rooms;
    LeaveCriticalSection(&g_cs);
}

static void on_queue(const uint8_t *b, int len)
{
    static const char *const say[] = { "Checking your account...", "Searching for an opponent...",
        "Ranked play needs a signed-in account (sign in with Discord in the launcher).", "", "Opponent found!" };
    int state;
    if (len < 5 || b[0] > NETQUEUE_FOUND) return;
    state = b[0];
    EnterCriticalSection(&g_cs);
    if (N.pend_len && N.pend[5] == T_RANKED) answered();
    if (N.queue_state != NETQUEUE_NONE || state == NETQUEUE_FOUND) {
        if (state != N.queue_state)
            fprintf(stderr, "[NETPLAY] ranked queue: state %d\n", state);
        N.queue_state = state == NETQUEUE_OFF ? NETQUEUE_NONE : state;
        N.queue_waited = b[1] | b[2] << 8;
        N.queue_searching = b[3] | b[4] << 8;
        snprintf(N.queue_text, sizeof(N.queue_text), "%s", say[state]);
    }
    LeaveCriticalSection(&g_cs);
}

static void on_room(const uint8_t *b, int len)
{
    Reader r = { b, len, 0, 0 };
    NetRoom v;
    int i, count, gone = 0;
    memset(&v, 0, sizeof(v));
    rdbytes(&r, v.code, 6);
    v.code[6] = 0;
    v.you = rd8(&r);
    v.host = rd8(&r);
    v.phase = rd8(&r);
    rdsettings(&r, &v.settings);
    v.candidate_count = rd8(&r);
    if (v.candidate_count > 3) return;
    rdbytes(&r, v.candidates, v.candidate_count);
    for (i = 0; i < v.candidate_count; i++) v.votes[i] = rd8(&r);
    v.ms_left = rd8(&r);
    v.ms_left |= rd8(&r) << 8;
    count = rd8(&r);
    if (count > MAXP) return;
    v.player_count = count;
    for (i = 0; i < count; i++) {
        v.players[i].slot = rd8(&r);
        rdstr(&r, v.players[i].name, sizeof(v.players[i].name));
        v.players[i].ready = rd8(&r);
        v.players[i].fighter = rd8(&r);
        v.players[i].voted = rd8(&r);
        if (v.players[i].voted == 0xFF) v.players[i].voted = -1;
    }
    if (r.bad) return;
    v.in_room = 1;
    v.received = GetTickCount64();
    EnterCriticalSection(&g_cs);
    memcpy(v.error, N.roomv.error, sizeof(v.error));
    v.version = N.roomv.version + 1;
    N.roomv = v;
    N.slot = v.you;
    memcpy(N.room, v.code, 7);
    if (N.pend_len && N.pend[5] != T_LIST && N.pend[5] != T_LEAVE) answered();
    if (v.phase != 2) N.over_pending = N.start_this_play = 0;
    /* A player of the running match has left: the others cannot go on in lockstep. */
    if (N.match_live) {
        int k, j;
        for (k = 0; k < N.players; k++) {
            int found = 0;
            for (j = 0; j < count; j++) if (v.players[j].slot == N.start_slots[k]) found = 1;
            if (!found) gone = 1;
        }
    }
    LeaveCriticalSection(&g_cs);
    if (gone && np_load(&N.lockstep)) {
        InterlockedExchange(&N.lockstep, 0);
        assign_input(0);
        SetEvent(N.arrived);
        set_status("A player left the match; it finishes offline.");
    }
}

static void on_start(const uint8_t *b, int len)
{
    Reader r = { b, len, 0, 0 };
    AutoMatch m;
    uint32_t seed;
    int count, i, j, slots[MAXP], fighters[MAXP], pos = -1;
    if (len < 4) return;
    seed = get32(b);
#if defined(__ANDROID__)
    if (np_load(&N.stop)) return;
    if (!automatch_android_main_menu() && (!N.start_seed || seed!=N.start_seed)) {
        set_status("Return to the main menu before starting an online match.");
        netplay_lobby_leave();return;
    }
#endif
    r.i = 4;
    memset(&m, 0, sizeof(m));
    m.type = rd8(&r);
    m.rules = rd8(&r);
    m.venue = rd8(&r);
    count = rd8(&r);
    if (count < 2 || count > MAXP) return;
    for (i = 0; i < count; i++) { slots[i] = rd8(&r); fighters[i] = rd8(&r); }
    if (r.bad) return;
    if (N.start_seed && seed == N.start_seed) { N.start_this_play = 1; return; }     /* a repeat */
    /* positions follow the room slots in order */
    for (i = 1; i < count; i++)
        for (j = i; j > 0 && slots[j - 1] > slots[j]; j--) {
            int t = slots[j]; slots[j] = slots[j - 1]; slots[j - 1] = t;
            t = fighters[j]; fighters[j] = fighters[j - 1]; fighters[j - 1] = t;
        }
    for (i = 0; i < MAXP; i++) N.pos_of_slot[i] = -1;
    for (i = 0; i < count; i++) {
        if (slots[i] < MAXP) N.pos_of_slot[slots[i]] = i;
        if (slots[i] == N.slot) pos = i;
        N.start_slots[i] = slots[i];
        m.fighter[i] = fighters[i];
        m.pad[i] = i;
    }
    if (pos < 0) return;
    m.count = count;
    {   /* the mode's free places go to CPU fighters, the same on every PC */
        const OnlineMode *mode = online_mode(m.type);
        const OnlineFighter *roster;
        int roster_count = online_fighters(&roster);
        for (i = count; mode && i < mode->fighters && i < MAXP; i++) {
            m.pad[i] = -1;
            m.fighter[i] = roster_count ? roster[mix32(seed ^ (0xC0FFEEu + (uint32_t)i)) % (uint32_t)roster_count].id : 55;
            m.count = i + 1;
        }
    }
    EnterCriticalSection(&g_cs);
    N.start_seed = seed;
    N.session = seed;
    N.players = count;
    N.fighters = m.count;
    N.pos = pos;
    N.delay = LOBBY_DELAY;
    N.delay_fixed = 0;
    for (i = 0; i < MAXP; i++) N.peer_rtt[i] = -1, N.peer_rb[i] = 0, N.peer_delay126[i]=0;
    N.peer_delay126[pos]=1;
    N.peer_rb[pos] = rollback_wanted() && netplay_relay_allows_rollback132();
    if (rollback_wanted() && !N.peer_rb[pos])
        fprintf(stderr, "[NETCAP132] relay has not allowed rollback; this match stays in lockstep\n");
    {   /* this PC's ping as of now, with the worst of the last few as the jitter margin */
        int k, worst = N.rtt_ms > 0 ? N.rtt_ms : 0;
        for (k = 0; k < 5; k++) if (N.rtt_recent[k] > worst) worst = N.rtt_recent[k];
        N.peer_rtt[pos] = worst > 2000 ? 2000 : worst;
    }
    N.match_count = 0;
    N.match_live = 1;
    N.match_seen = 0;
    N.over_pending = 0;
    N.start_this_play = 1;
    N.report_m = m;
    for (i = 0; i < MAXP; i++) {
        N.report_names[i][0] = 0;
        if (i < count)
            for (j = 0; j < N.roomv.player_count; j++)
                if (N.roomv.players[j].slot == slots[i])
                    snprintf(N.report_names[i], sizeof(N.report_names[i]), "%s", N.roomv.players[j].name);
    }
    snprintf(N.report_key, sizeof(N.report_key), "%.6s-%08X", N.roomv.code[0] ? N.roomv.code : N.room, seed);
    N.reported = N.fighting_seen = 0;
    LeaveCriticalSection(&g_cs);
    reset_remote_pads();
    fprintf(stderr, "[NETPLAY] START: seed %08X, type %d, rules %d, venue %d, %d players, this PC is player %d\n",
            seed, m.type, m.rules, m.venue, count, pos + 1);
    become_connected();
    automatch_request(&m);
}

/* Code 10 collides between the bundled private-room extension and the public
 * relay's newer ErrUpdateRequired. Only an authenticated private-room request
 * on a non-project endpoint can be a password refusal. HELLO has no password. */
static void netplay_relay_error132(int code, const uint8_t *message, size_t message_len)
{
    static const char *const why[] = { "", "No room with that code.", "That room is full.",
        "The other player has a different version of the game.", "That name is not allowed.",
        "The server is full.", "Too many attempts; wait at least 60 seconds.",
        "The server lost this session.",
        "Only the room's host can do that, and not while voting or playing.",
        "Not possible here: not in a room, or already in one." };
    const char *text = code >= 1 && code <= 9 ? why[code] : "The server refused the request.";
    int state = np_load(&N.state), private_password, recoverable;
    int partner = project_server_endpoint(N.server_text);
    if (state == NETPLAY_FAILED || state == NETPLAY_OFF) return;
    EnterCriticalSection(&g_cs);
    if (code == 7 && N.lobby && N.have_token && state != NETPLAY_CONNECTING) {
        /* Preserve reconnection after a relay restart. A refusal of that fresh
         * HELLO is terminal, so this cannot loop forever without a WELCOME. */
        answered();
        N.have_token = 0;
        N.started = GetTickCount64();
        N.hello_sent = 0;
        InterlockedExchange(&N.relay_caps132, -1);
        N.caps_sent132 = 0;
        N.caps_tries132 = 0;
        memset(&N.roomv, 0, sizeof(N.roomv));
        InterlockedExchange(&N.state, NETPLAY_CONNECTING);
        LeaveCriticalSection(&g_cs);
        set_status("Reconnecting to %s...", N.server_text);
        return;
    }
    private_password = code == 10 && N.lobby && N.have_token &&
        state != NETPLAY_CONNECTING && !partner &&
        N.pend_len >= 14 && (N.pend[5] == T_CREATE_PRIVATE || N.pend[5] == T_JOIN_PRIVATE);
    if (code == 10) text = private_password ? "Private room: password missing or incorrect." :
        "Update the app to connect to this relay.";
    if (code == 11 && partner)
        text = "This installation already has an active online session.";
    if (code == 14 && partner) {
        static const char unsupported[] = "platform not supported";
        if (message_len == sizeof unsupported - 1 && !memcmp(message, unsupported, message_len))
            text = "This relay does not support the Android platform.";
        else if (state == NETPLAY_CONNECTING)
            text = "FFNY Online has not enabled Android on this relay yet.";
        else
            text = "FFNY Online has not accepted this Android build's capabilities.";
    }
    recoverable = N.lobby && N.have_token && state != NETPLAY_CONNECTING &&
        (code == 1 || code == 2 || code == 3 || code == 8 || code == 9 || private_password);
    if (recoverable && code == 9 && N.roomv.in_room && N.pend_len &&
        N.pend[5] != T_CREATE && N.pend[5] != T_CREATE_PRIVATE &&
        N.pend[5] != T_JOIN && N.pend[5] != T_JOIN_PRIVATE && N.pend[5] != T_QUICK) {
        unsigned version = N.roomv.version;
        memset(&N.roomv, 0, sizeof(N.roomv));
        N.roomv.version = version + 1;
        N.room[0] = 0;
        text = "The room is gone (the other player left).";
    }
    /* Every ERROR acknowledges the request, including terminal errors. */
    answered();
    snprintf(N.roomv.error, sizeof(N.roomv.error), "%s", text);
    N.roomv.version++;
    if (!recoverable) {
        InterlockedExchange(&N.lockstep, 0);
        N.have_token = 0;
        N.match_live = N.start_this_play = N.over_pending = 0;
        if (code == 6 || code == 10) {
            s_relay_retry_after132 = GetTickCount64() + 60000;
            snprintf(s_relay_rejected132, sizeof(s_relay_rejected132), "%s", N.server_text);
        }
        InterlockedExchange(&N.state, NETPLAY_FAILED);
    }
    LeaveCriticalSection(&g_cs);
    if (!recoverable) {
        assign_input(0);
        SetEvent(N.arrived);
        set_status("%s", text);
    }
    fprintf(stderr, "[NETREJECT132] code=%d stage=%s action=%s retries_stopped=1 cooldown_ms=%d: %s\n",
        code, state == NETPLAY_CONNECTING ? "hello" : "session", recoverable ? "room-error" : "failed",
        !recoverable && (code == 6 || code == 10) ? 60000 : 0, text);
}

static int netplay_retry_blocked132(const char *server)
{
    ULONGLONG now = GetTickCount64(), remaining = 0;
    EnterCriticalSection(&g_cs);
    if (server && !_stricmp(server, s_relay_rejected132) && now < s_relay_retry_after132)
        remaining = s_relay_retry_after132 - now;
    LeaveCriticalSection(&g_cs);
    if (!remaining) return 0;
    InterlockedExchange(&N.state, NETPLAY_FAILED);
    set_status("Server refused the previous connection. Wait %u seconds before retrying.",
        (unsigned)((remaining + 999) / 1000));
    return 1;
}

static void on_packet(const uint8_t *b, int len)
{
    /* A late WELCOME/START must not resurrect a rejected session. */
    if (np_load(&N.state) == NETPLAY_FAILED || np_load(&N.state) == NETPLAY_OFF) return;
    if (len < 6 || memcmp(b, "DJNP", 4) || b[4] != PROTO) return;
    N.last_rx = GetTickCount64();
    switch (b[5]) {
    case T_WELCOME:
        if (len >= 6 + 8 + 1 + 6) {
            memcpy(N.token, b + 6, 8);
            N.have_token = 1;
            if (N.lobby) {
                if (np_load(&N.state) == NETPLAY_CONNECTING) {
                    if (project_server_endpoint(N.server_text)) {
                        netplay_send_caps132();
                        N.caps_tries132 = 1;
                        N.caps_sent132 = GetTickCount64();
                        send_partner_device_id();
                        N.device_tries = 1;
                        N.device_sent = N.caps_sent132;
                        N.device_ack = 0;
                    }
                    InterlockedExchange(&N.state, NETPLAY_LOBBY);
                    set_status("In the lobby of %s.", N.server_text);
                    netplay_lobby_list(0xFF);
                }
                break;
            }
            N.slot = b[14] ? 1 : 0;
            N.pos = N.slot;
            memcpy(N.room, b + 15, 6);
            N.room[6] = 0;
            if (np_load(&N.state) == NETPLAY_CONNECTING) {
                InterlockedExchange(&N.state, NETPLAY_WAITING);
                if (N.creating)
                    set_status("Room %s is open. Tell the other player this code; waiting for them to join.", N.room);
                else
                    set_status("In room %s; waiting for the host.", N.room);
            }
        }
        break;
    case T_PEER:
        if (len >= 8 && !N.lobby) {
            int n = b[7];
            if (n == 0) {
                if (np_load(&N.state) == NETPLAY_CONNECTED || np_load(&N.state) == NETPLAY_SYNCING)
                    lose_peer("The other player left.");
            } else if (len >= 8 + n && np_load(&N.state) == NETPLAY_WAITING) {
                memcpy(N.peer_name, b + 8, (size_t)(n < 24 ? n : 24));
                N.peer_name[n < 24 ? n : 24] = 0;
                InterlockedExchange(&N.state, NETPLAY_SYNCING);
                if (N.creating) {
                    LARGE_INTEGER t;
                    QueryPerformanceCounter(&t);
                    N.session = mix32((uint32_t)t.QuadPart ^ (uint32_t)GetCurrentProcessId() ^ 0x9E3779B9u);
                    N.acked = 0;
                    N.hello_sent = 0;
                }
                set_status("%s joined; agreeing on the session...", N.peer_name);
            }
        }
        break;
    case T_DATA:
        if (len >= 8 && b[7] == 7) {
#if defined(__ANDROID__)
            extern void android_voice_packet133(int slot,const uint8_t*p,int len);
            android_voice_packet133(b[6],b+7,len-7);
#endif
            break;
        }
        if (len >= 7) {
            int from = N.lobby ? (b[6] < MAXP ? N.pos_of_slot[b[6]] : -1) : (b[6] ? 1 : 0);
            on_data(from, b + 7, len - 7);
        }
        break;
    case T_DEVICE_ID:
        if (N.lobby && N.have_token && len >= 7 && b[6] == 0 &&
            project_server_endpoint(N.server_text)) N.device_ack = 1;
        break;
    case T_CAPS132:
        if (N.lobby && N.have_token && len >= 10 && project_server_endpoint(N.server_text)) {
            LONG caps = (LONG)(get32(b + 6) & 0x7FFFFFFFu);
            if (InterlockedExchange(&N.relay_caps132, caps) != caps)
                fprintf(stderr, "[NETCAP132] relay_caps=%08X rollback_allowed=%d; ordinary code rooms retained\n",
                    (unsigned)caps, !!(caps & CAPS_ROLLBACK132));
        }
        break;
    case T_PONG:
        if (len >= 10 && get32(b + 6) == N.ping_nonce)
        {
            N.rtt_ms = (int)(GetTickCount64() - N.last_ping);
            N.rtt_recent[N.rtt_next] = N.rtt_ms;
            N.rtt_next = (N.rtt_next + 1) % 5;
        }
        break;
    case T_ROOMS:
        if (N.lobby) on_rooms(b + 6, len - 6);
        break;
    case T_PLAYERS:
        if (N.lobby) on_players(b + 6, len - 6);
        break;
    case T_QUEUE:
        if (N.lobby) on_queue(b + 6, len - 6);
        break;
    case T_ROOM:
        if (N.lobby) on_room(b + 6, len - 6);
        break;
    case T_START:
        if (N.lobby) on_start(b + 6, len - 6);
        break;
    case T_ERROR:
        if (len >= 7) {
            size_t message_len = len >= 8 && b[7] <= len - 8 ? b[7] : 0;
            netplay_relay_error132(b[6], message_len ? b + 8 : NULL, message_len);
        }
        break;
    }
}

/* ---- the network thread -------------------------------------------------------- */

#if defined(__ANDROID__)
/* Ordinary room voice is host-only and never enters simulation/input hashes. */
extern int android_voice_begin_send133(void);
extern void android_voice_end_send133(void);
static uint32_t voice_context_locked133(void)
{
    int state=np_load(&N.state);
    if(!N.lobby||!N.have_token||!(N.roomv.in_room||N.match_live)||
       (state!=NETPLAY_LOBBY&&state!=NETPLAY_CONNECTED))return 0;
    uint32_t h=2166136261u;
    for(unsigned i=0;i<8;i++)h=(h^N.token[i])*16777619u;
    for(unsigned i=0;i<6;i++)h=(h^(uint8_t)N.roomv.code[i])*16777619u;
    return h?h:1;
}
uint32_t netplay_voice_context133(void)
{
    InitOnceExecuteOnce(&g_once,init_once,NULL,NULL);
    EnterCriticalSection(&g_cs);uint32_t h=voice_context_locked133();LeaveCriticalSection(&g_cs);return h;
}
void netplay_voice_send133(uint32_t room,const uint8_t*p,int len)
{
    if(!room||!p||len!=326||p[0]!=7)return;
    EnterCriticalSection(&g_cs);
    if(room==voice_context_locked133()&&android_voice_begin_send133()){
        send_data(p,len);android_voice_end_send133();
    }
    LeaveCriticalSection(&g_cs);
}

#endif

static void send_live_pad(ULONGLONG now)
{
    XBOX_INPUT_STATE s;
    uint8_t p[5 + sizeof(XBOX_GAMEPAD)];
    memset(&s, 0, sizeof(s));
    xbox_HostInputGetLocalState(&s);
    if (!memcmp(&s.Gamepad, &N.last_sent, sizeof(XBOX_GAMEPAD)) && now - N.last_pad_sent < 100) return;
    N.last_sent = s.Gamepad;
    N.last_pad_sent = now;
    p[0] = P_LIVEPAD;
    put32(p + 1, ++N.local_seq);
    memcpy(p + 5, &s.Gamepad, sizeof(XBOX_GAMEPAD));
    send_data(p, (int)sizeof(p));
}

/* RECOMP_NETPLAY_LAG_MS=<ms>[,<jitter ms>] (tests): what arrives is handled that much later, so
 * two copies on one PC play as if far apart. In order: jitter delays, it does not reorder. */
#define LAG_QUEUE 1024
static uint8_t lag_buf[LAG_QUEUE][1500];
static int lag_len[LAG_QUEUE], lag_head, lag_n, lag_ms = -1, lag_jitter;
static ULONGLONG lag_due[LAG_QUEUE];

static DWORD WINAPI net_thread(LPVOID unused)
{
    uint8_t b[1500];
    (void)unused;
#if defined(__ANDROID__)
    defjam_android_native_thread_enter("network", "net_thread", (uintptr_t)net_thread);
#endif
    if (lag_ms < 0) {
        const char *v = getenv("RECOMP_NETPLAY_LAG_MS");
        lag_ms = v ? atoi(v) : 0;
        lag_jitter = v && strchr(v, ',') ? atoi(strchr(v, ',') + 1) : 0;
        if (lag_ms > 0) fprintf(stderr, "[NETPLAY] tests: arriving packets held %d ms (+0..%d)\n", lag_ms, lag_jitter);
    }
    N.started = N.last_rx = GetTickCount64();
    while (!np_load(&N.stop)) {
        ULONGLONG now = GetTickCount64();
        int got;
        /* A terminal rejection stops all HELLO, pending, ping and match traffic.
         * Stop still joins this worker; retrying requires an explicit new Start. */
        if (np_load(&N.state) == NETPLAY_FAILED) { Sleep(20); continue; }
        if (np_load(&N.state) == NETPLAY_CONNECTING) {
            if (now - N.hello_sent >= 500) { send_hello(); N.hello_sent = now; }
            if (now - N.started > 10000) {
                InterlockedExchange(&N.state, NETPLAY_FAILED);
                if (N.lobby)
                    set_status("No answer from the server %s. It may be down, or an older server without the "
                               "online hub (play by code still works there).", N.server_text);
                else
                    set_status("No answer from the server %s. Check the address and that it is running.", N.server_text);
            }
        }
        if (np_load(&N.state) == NETPLAY_FAILED) continue;
        if (N.have_token) {
            if (now - N.last_ping >= 1000) {
                N.ping_nonce++;
                N.last_ping = now;
                send_token_only(T_PING, N.ping_nonce, 1);
            }
            if (now - N.last_rx > 15000) {
                InterlockedExchange(&N.lockstep, 0);
                assign_input(0);
                InterlockedExchange(&N.state, NETPLAY_FAILED);
                set_status("The connection to the server was lost.");
            }
        }
        if (N.pend_len && now - N.pend_sent >= RETRY_MS) {
            uint8_t again[sizeof(N.pend)];
            int n = 0;
            EnterCriticalSection(&g_cs);
            if (N.pend_len && N.pend_tries >= RETRIES) {
                answered();
                snprintf(N.roomv.error, sizeof(N.roomv.error), "The server did not answer; try again.");
                N.roomv.version++;
            } else if (N.pend_len) {
                memcpy(again, N.pend, (size_t)N.pend_len);
                n = N.pend_len;
                N.pend_tries++;
                N.pend_sent = now;
            }
            LeaveCriticalSection(&g_cs);
            if (n) send_raw(again, n);
        }
        /* Public relay capability negotiation is separate from room requests.
         * 0x20 is the old custom relay's password-create extension: never send
         * CAPS to that relay, and never advertise King of the Hill support. */
        if (N.lobby && N.have_token && np_load(&N.state) == NETPLAY_LOBBY &&
            project_server_endpoint(N.server_text) && np_load(&N.relay_caps132) < 0 &&
            now - N.caps_sent132 >= RETRY_MS) {
            if (N.caps_tries132++ < 5) netplay_send_caps132();
            else InterlockedExchange(&N.relay_caps132, 0);
            N.caps_sent132 = now;
        }
        if (N.lobby && N.have_token && !N.device_ack && N.device_tries < 5 &&
            project_server_endpoint(N.server_text) && now - N.device_sent >= RETRY_MS) {
            send_partner_device_id();
            ++N.device_tries;
            N.device_sent = now;
        }
        /* Hub: our ping for this START, until the match is under way. */
        if (N.lobby && N.match_live && !N.match_seen && now - N.last_rtt_sent >= 100)
            send_rtt();
        /* The room is playing but this PC never got its START: a VOTE makes the server send it again. */
        if (N.lobby && N.roomv.in_room && N.roomv.phase == 2 && !N.start_this_play && !N.over_pending &&
            now - N.last_recover >= 500) {
            uint8_t vb[6 + 8 + 1];
            int n = header(vb, T_VOTE);
            memcpy(vb + n, N.token, 8); n += 8;
            vb[n++] = 0;
            send_raw(vb, n);
            N.last_recover = now;
        }
        if (!N.lobby && np_load(&N.state) == NETPLAY_SYNCING && N.creating && now - N.hello_sent >= 300) {
            uint8_t p[7];
            p[0] = P_GAMEHELLO;
            put32(p + 1, N.session);
            p[5] = (uint8_t)N.delay;
            p[6] = LOCKSTEP_VERSION;
            send_data(p, 7);
            N.hello_sent = now;
        }
        if (np_load(&N.state) == NETPLAY_CONNECTED || np_load(&N.state) == NETPLAY_SYNCING)
            send_live_pad(now);
        got = recv(N.sock, (char *)b, sizeof(b), 0);
        if(np_load(&N.stop)) break; /* suspend/cancel wins over a late relay START */
        if (got > 0 && lag_ms > 0) {        /* tests: hold every arriving packet for a while */
            if (lag_n < LAG_QUEUE) {
                int at = (lag_head + lag_n++) % LAG_QUEUE;
                memcpy(lag_buf[at], b, (size_t)got);
                lag_len[at] = got;
                lag_due[at] = now + (ULONGLONG)lag_ms + (lag_jitter > 0 ? (ULONGLONG)(rand() % (lag_jitter + 1)) : 0);
            }
        } else if (got > 0)
            on_packet(b, got);
        while (lag_n && GetTickCount64() >= lag_due[lag_head]) {
            on_packet(lag_buf[lag_head], lag_len[lag_head]);
            lag_head = (lag_head + 1) % LAG_QUEUE;
            lag_n--;
        }
    }
    if (N.have_token)
        send_token_only(T_BYE, 0, 0);
    return 0;
}

/* Public FFNY relay: only its documented protocol, never our password extension. */
static int project_server_endpoint(const char *server)
{
    char host[128];
    if (!server || !*server) return 1;
    size_t n = strcspn(server, ":");
    if (n >= sizeof(host)) return 0;
    memcpy(host, server, n); host[n] = 0;
    if (n && host[n - 1] == '.') host[n - 1] = 0;
    return !_stricmp(host, PROJECT_SERVER_NAME) || !_stricmp(host, PROJECT_SERVER_ADDRESS) ||
        !_stricmp(host,"na.defjam.online") || !_stricmp(host,"5.161.206.134") ||
        !_stricmp(host,"west.defjam.online") || !_stricmp(host,"5.78.214.178");
}

static int netplay_relay_allows_rollback132(void)
{
    LONG caps = np_load(&N.relay_caps132);
    return !project_server_endpoint(N.server_text) || (caps >= 0 && (caps & CAPS_ROLLBACK132));
}

static int resolve(const char *server)
{
    char host[128], port[16];
    const char *colon = strrchr(server, ':');
    struct addrinfo hints, *res = NULL;
    if (colon && colon != server) {
        snprintf(host, sizeof(host), "%.*s", (int)(colon - server), server);
        snprintf(port, sizeof(port), "%s", colon + 1);
    } else {
        snprintf(host, sizeof(host), "%s", server);
        snprintf(port, sizeof(port), "%d", DEFAULT_PORT);
    }
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, port, &hints, &res) || !res) return -1;
    /* Also exclude official relay IPs reached through an alias in private mode. */
    char address[INET_ADDRSTRLEN];
    const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)res->ai_addr;
    if (!inet_ntop(AF_INET, &ipv4->sin_addr, address, sizeof(address)) ||
        project_server_endpoint(address)) {
        freeaddrinfo(res);
        return -1;
    }
    memcpy(&N.server, res->ai_addr, res->ai_addrlen);
    N.server_len = (int)res->ai_addrlen;
    freeaddrinfo(res);
    return 0;
}

static int start(const char *server, const char *name, int creating, const char *room, int delay, int lobby)
{
    LONG cancel_generation=np_load(&s_mobile_cancel);
#if defined(__ANDROID__)
    struct timeval timeout = {0, 20000};
#else
    DWORD timeout = 20;
#endif
    uint32_t h1 = 2166136261u, h2;
    const char *v;
    int i;
    InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
    netplay_stop();
    if (!server || !*server) { InterlockedExchange(&N.state, NETPLAY_FAILED); set_status("Enter the server address first."); return -1; }
    if (project_server_endpoint(server)) {
        InterlockedExchange(&N.state, NETPLAY_FAILED);
        set_status(NETPLAY_PARTNER_BUILD_REQUIRED);
        return -1;
    }
    if (netplay_retry_blocked132(server)) return -1;
    snprintf(N.server_text, sizeof(N.server_text), "%s", server);
    snprintf(N.name, sizeof(N.name), "%s", name && *name ? name : "Player");
    /* The verified Discord game name is the account identity for ranked play.
     * Guest names remain when not signed in; there is no fabricated login. */
    {char account_name133[25];if(account_game_name(account_name133,sizeof account_name133))
        snprintf(N.name,sizeof N.name,"%s",account_name133);}
    N.creating = creating;
    N.lobby = lobby;
    memset(N.room, 0, sizeof(N.room));
    if (!lobby && !creating) {
        for (i = 0; i < 6 && room && room[i]; i++) N.room[i] = (char)toupper((unsigned char)room[i]);
        if (i != 6) { InterlockedExchange(&N.state, NETPLAY_FAILED); set_status("A room code has 6 characters."); return -1; }
    }
    N.delay = delay >= 1 && delay <= 12 ? delay : 3;
    /* The build's fingerprint: the version and the lockstep rules. */
    for (v = DEFJAM_NETPLAY_PC_PROFILE; *v; v++) h1 = (h1 ^ (uint8_t)*v) * 16777619u;
    h2 = mix32(h1 ^ LOCKSTEP_VERSION);
    put32(N.build, h1); put32(N.build + 4, h2); // retain private legacy relay profile
    if(project_server_endpoint(N.server_text)){
        ffny_android_fingerprint(N.build, 0);
    }
    N.have_token = 0;
    InterlockedExchange(&N.relay_caps132, -1);
    N.caps_sent132 = 0;
    N.caps_tries132 = 0;
    N.peer_name[0] = 0;
    N.rtt_ms = -1;
    memset(N.rtt_recent, 0, sizeof(N.rtt_recent));
    N.local_seq = 0;
    N.players = N.fighters = 2;
    N.pos = 0;
    for (i = 0; i < MAXP; i++) N.pos_of_slot[i] = i < 2 ? i : -1;
    reset_remote_pads();
    EnterCriticalSection(&g_cs);
    memset(N.in_tag, 0, sizeof(N.in_tag));
    memset(&N.roomv, 0, sizeof(N.roomv));
    N.room_count = 0;
    N.pend_len = 0;
    N.match_live = N.match_seen = N.over_pending = N.start_this_play = 0;
    N.start_seed = 0;
    N.match_count = 0;
    LeaveCriticalSection(&g_cs);
    if (resolve(server)) {
        InterlockedExchange(&N.state, NETPLAY_FAILED);
        set_status("Cannot connect to %s.", server);
        return -1;
    }
    if(np_load(&s_mobile_cancel)!=cancel_generation){set_status("Online connection cancelled.");return -1;}
    N.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (N.sock == INVALID_SOCKET) { InterlockedExchange(&N.state, NETPLAY_FAILED); set_status("No network socket."); return -1; }
    if (setsockopt(N.sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) != 0) {
        closesocket(N.sock); N.sock = INVALID_SOCKET;
        InterlockedExchange(&N.state, NETPLAY_FAILED); set_status("Cannot set the receive timeout."); return -1;
    }
#if defined(__ANDROID__)
    /* Connected UDP rejects datagrams from anywhere except the selected relay. */
    if (connect(N.sock, (struct sockaddr *)&N.server, (socklen_t)N.server_len) != 0) {
        closesocket(N.sock); N.sock = INVALID_SOCKET;
        InterlockedExchange(&N.state, NETPLAY_FAILED); set_status("Cannot reach this relay."); return -1;
    }
    fprintf(stderr, "[NETPLAY] Android ARM64, private relay protocol\n");
#endif
    InterlockedExchange(&N.stop,0);
    if(np_load(&s_mobile_cancel)!=cancel_generation){closesocket(N.sock);N.sock=INVALID_SOCKET;return -1;}
    N.hello_sent = 0;
    InterlockedExchange(&N.state, NETPLAY_CONNECTING);
    set_status("Connecting to %s...", server);
    N.thread = CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
    if (!N.thread) {
        closesocket(N.sock); N.sock = INVALID_SOCKET;
        InterlockedExchange(&N.state, NETPLAY_FAILED); set_status("Cannot start the network worker.");
    }
    return N.thread ? 0 : -1;
}

int netplay_host(const char *server, const char *name, int input_delay)
{
    return start(server, name, 1, NULL, input_delay, 0);
}

int netplay_join(const char *server, const char *name, const char *room)
{
    return start(server, name, 0, room, 3, 0);
}

int netplay_lobby(const char *server, const char *name)
{
    return start(server, name, 0, NULL, LOBBY_DELAY, 1);
}

void netplay_stop(void)
{
    automatch_android_cancel();
    InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
    InterlockedExchange(&N.lockstep, 0);
    assign_input(0);
    if (N.thread) {
        InterlockedExchange(&N.stop, 1);
        SetEvent(N.arrived);
        WaitForSingleObject(N.thread, INFINITE); /* socket receive is bounded20ms */
        CloseHandle(N.thread);
        N.thread = NULL;
    }
    EnterCriticalSection(&g_cs);
    if (N.sock != INVALID_SOCKET) { closesocket(N.sock); N.sock = INVALID_SOCKET; }
    SecureZeroMemory(N.pend,sizeof(N.pend));N.pend_len=0;
    LeaveCriticalSection(&g_cs);
    N.have_token = 0;
    N.lobby = 0;
    N.match_live = N.match_seen = 0;
    InterlockedExchange(&N.state, NETPLAY_OFF);
    SetEvent(N.arrived);
}

enum NetplayState netplay_state(void) { return (enum NetplayState)np_load(&N.state); }

void netplay_status(char *text, int size)
{
    if (size <= 0) return;
    InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
    EnterCriticalSection(&g_cs);
    if (np_load(&N.state) == NETPLAY_CONNECTED && N.rtt_ms >= 0)
        snprintf(text, (size_t)size, "%s  Ping %d ms, input delay %d steps.%s", N.status, N.rtt_ms, N.delay,
                 N.desync ? "  The games went out of step in the last fight." : "");
    else if (np_load(&N.state) == NETPLAY_LOBBY && N.rtt_ms >= 0)
        snprintf(text, (size_t)size, "%s  Ping %d ms.", N.status, N.rtt_ms);
    else if (np_load(&N.state) == NETPLAY_OFF)
        snprintf(text, (size_t)size, "Not connected.");
    else
        snprintf(text, (size_t)size, "%s", N.status);
    LeaveCriticalSection(&g_cs);
}

void netplay_room(char *code, int size)
{
    if (size > 0) snprintf(code, (size_t)size, "%s", N.room);
}

/* Tests (src/hooks/test_control.c): the lockstep's state as one JSON object. */
int netplay_debug_json(char *out, int size)
{
    return snprintf(out, (size_t)size,
                    "{\"lockstep\":%d,\"lobby\":%d,\"players\":%d,\"pos\":%d,\"delay\":%d,\"step\":%u,"
                    "\"match\":\"%08X\",\"matches\":%u,\"desync\":%d,\"rtt_ms\":%d,\"match_live\":%d}",
                    (int)np_load(&N.lockstep), N.lobby, N.players, N.pos, N.delay, (unsigned)N.step, (unsigned)N.match,
                    (unsigned)N.match_count, N.desync, N.rtt_ms, N.match_live);
}

/* ---- the hub, for the launcher and overlay ------------------------------------ */

int netplay_in_lobby(void) { return N.lobby; }

void netplay_lobby_list(int mode)
{
    uint8_t m = (uint8_t)mode;
    request(T_LIST, &m, 1);
}

/* Not a pending request (the online screens ask every few seconds, and a lost answer is simply
 * asked again), so it never displaces a JOIN or READY that is waiting for its ROOM. */
void netplay_lobby_who(void)
{
    uint8_t b[32];
    int n;
    if (!N.lobby || !N.have_token) return;
    n = header(b, T_WHO);
    memcpy(b + n, N.token, 8);
    send_raw(b, n + 8);
}

int netplay_lobby_players(NetPlayer *out, int max, int *online, int *rooms)
{
    int n;
    InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
    EnterCriticalSection(&g_cs);
    n = N.who_online < 0 ? -1 : (N.who_count < max ? N.who_count : max);
    if (n > 0) memcpy(out, N.who, sizeof(out[0]) * (size_t)n);
    if (online) *online = N.who_online;
    if (rooms) *rooms = N.who_rooms;
    LeaveCriticalSection(&g_cs);
    return n;
}

int netplay_lobby_rooms(NetRoomInfo *out, int max, unsigned *version)
{
    int n;
    InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
    EnterCriticalSection(&g_cs);
    n = N.room_count < max ? N.room_count : max;
    if (n > 0) memcpy(out, N.rooms, sizeof(out[0]) * (size_t)n);
    if (version) *version = N.rooms_version;
    LeaveCriticalSection(&g_cs);
    return n;
}

void netplay_lobby_create(const NetRoomSettings *settings)
{
    uint8_t b[64];
    request(T_CREATE, b, put_settings(b, settings));
}

void netplay_lobby_join(const char *code)
{
    uint8_t b[6];
    int i;
    for (i = 0; i < 6; i++) b[i] = (uint8_t)toupper((unsigned char)(code && code[i] ? code[i] : 0));
    request(T_JOIN, b, 6);
}

void netplay_lobby_quick(int mode, int max)
{
    uint8_t b[2] = { (uint8_t)mode, (uint8_t)(max < 2 ? 2 : max > MAXP ? MAXP : max) };
    request(T_QUICK, b, 2);
}

/* Asks the account service for a ticket (it blocks), then joins the queue with it. */
static DWORD WINAPI ranked_thread(LPVOID param)
{
    char ticket[33];
    uint8_t b[TICKET_LEN + 1];
    int i, fighter = (int)(INT_PTR)param;
    if (!account_ranked_ticket(ticket)) {
        EnterCriticalSection(&g_cs);
        if (N.queue_state == NETQUEUE_TICKET) {
            N.queue_state = NETQUEUE_REFUSED;
            snprintf(N.queue_text, sizeof(N.queue_text), "%s", account_state() == ACCOUNT_IN ?
                     "The account service did not answer; try again." :
                     "Ranked play needs a signed-in account (sign in with Discord in the launcher).");
        }
        LeaveCriticalSection(&g_cs);
        return 0;
    }
    for (i = 0; i < TICKET_LEN; i++) {
        unsigned v;
        sscanf(ticket + 2 * i, "%2x", &v);
        b[i] = (uint8_t)v;
    }
    SecureZeroMemory(ticket, sizeof(ticket));
    b[TICKET_LEN] = (uint8_t)fighter;
    EnterCriticalSection(&g_cs);
    i = N.queue_state == NETQUEUE_TICKET;   /* not cancelled meanwhile */
    if (i) N.queue_state = NETQUEUE_CHECKING;
    LeaveCriticalSection(&g_cs);
    if (i) request(T_RANKED, b, (int)sizeof(b));
    return 0;
}

void netplay_lobby_ranked(int fighter)
{
    HANDLE h;
    if (!N.lobby || !N.have_token) return;
    EnterCriticalSection(&g_cs);
    if (N.queue_state != NETQUEUE_NONE && N.queue_state != NETQUEUE_REFUSED) {
        LeaveCriticalSection(&g_cs);
        return;                             /* already searching */
    }
    N.queue_state = NETQUEUE_TICKET;
    N.queue_waited = N.queue_searching = 0;
    snprintf(N.queue_text, sizeof(N.queue_text), "Checking your account...");
    LeaveCriticalSection(&g_cs);
    h = CreateThread(NULL, 0, ranked_thread, (LPVOID)(INT_PTR)fighter, 0, NULL);
    if (h) CloseHandle(h);
}

int netplay_lobby_queue(int *waited, int *searching, char *text, int size)
{
    int state;
    InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
    EnterCriticalSection(&g_cs);
    state = N.queue_state;
    if (waited) *waited = N.queue_waited;
    if (searching) *searching = N.queue_searching;
    if (text && size > 0) snprintf(text, (size_t)size, "%s", N.queue_text);
    LeaveCriticalSection(&g_cs);
    return state;
}

void netplay_lobby_settings(const NetRoomSettings *settings)
{
    uint8_t b[64];
    request(T_SETTINGS, b, put_settings(b, settings));
}

void netplay_lobby_ready(int ready, int fighter)
{
    uint8_t b[2] = { (uint8_t)(ready ? 1 : 0), (uint8_t)fighter };
    request(T_READY, b, 2);
}

void netplay_lobby_vote(int choice)
{
    uint8_t b = (uint8_t)choice;
    request(T_VOTE, &b, 1);
}

void netplay_lobby_leave(void)
{
    EnterCriticalSection(&g_cs);
    memset(&N.roomv, 0, sizeof(N.roomv));
    N.roomv.version = (unsigned)GetTickCount64();
    N.room[0] = 0;
    N.pend_len = 0;                 /* whatever was waiting belonged to the room */
    N.queue_state = NETQUEUE_NONE;  /* LEAVE also ends a ranked search */
    LeaveCriticalSection(&g_cs);
    /* The relay never answers LEAVE (docs/netplay-protocol.md), so it is sent twice and not
     * waited for; if both are lost the server still has this client in the room and says so
     * with its next ROOM, which brings the room back here. */
    if (N.lobby && N.have_token) {
        uint8_t b[6 + 8];
        int n = header(b, T_LEAVE);
        memcpy(b + n, N.token, 8);
        n += 8;
        send_raw(b, n);
        send_raw(b, n);
    }
    netplay_lobby_list(0xFF);
}

void netplay_lobby_room(NetRoom *out)
{
    InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
    EnterCriticalSection(&g_cs);
    *out = N.roomv;
    LeaveCriticalSection(&g_cs);
}

/* A JSON string member's value: the names here are the game's printable ASCII. */
static int json_put(char *out, int size, int n, const char *s)
{
    if (n < size) out[n++] = '"';
    for (; *s && n < size - 2; s++) {
        if (*s == '"' || *s == '\\') out[n++] = '\\';
        if ((unsigned char)*s >= 0x20) out[n++] = *s;
    }
    if (n < size) out[n++] = '"';
    return n;
}

/* The room match's result (winners as a mask of fighter slots; code 4 a draw), from
 * netplay_on_step. A signed-in player's game reports it to the account service (account.c),
 * once per START; the service counts it when every player's report agrees. Positions are the
 * fighters' controller slots: the room's players in slot order, then the CPU fighters. */
void netplay_on_result(uint32_t winners, uint32_t losers, uint32_t code)
{
    AutoMatch m;
    char names[MAXP][25], key[32], json[2048];
    const OnlineMode *mode;
    int i, n = 0, pos, players, first = 1;
    (void)losers;
    InitOnceExecuteOnce(&g_once, init_once, NULL, NULL);
    EnterCriticalSection(&g_cs);
    if (!N.lobby || !N.match_live || N.reported || !N.report_key[0] || N.desync || !np_load(&N.lockstep)) {
        LeaveCriticalSection(&g_cs);
        return;
    }
    N.reported = 1;
    m = N.report_m;
    memcpy(names, N.report_names, sizeof(names));
    memcpy(key, N.report_key, sizeof(key));
    pos = N.pos;
    players = N.players;
    LeaveCriticalSection(&g_cs);
    mode = online_mode(m.type);
    n += snprintf(json + n, sizeof(json) - n, "{\"key\":\"%s\",\"mode\":%d,\"mode_name\":", key, m.type);
    n = json_put(json, (int)sizeof(json), n, mode ? mode->name : "");
    n += snprintf(json + n, sizeof(json) - n, ",\"venue\":%d,\"venue_name\":", m.venue);
    n = json_put(json, (int)sizeof(json), n, online_venue_name(m.venue));
    n += snprintf(json + n, sizeof(json) - n, ",\"you\":%d,\"players\":[", pos);
    for (i = 0; i < m.count && i < MAXP && n < (int)sizeof(json) - 200; i++) {
        int cpu = i >= players;
        n += snprintf(json + n, sizeof(json) - n, "%s{\"slot\":%d,\"name\":", i ? "," : "", i);
        n = json_put(json, (int)sizeof(json), n, cpu ? "CPU" : names[i]);
        n += snprintf(json + n, sizeof(json) - n, ",\"fighter\":%d,\"fighter_name\":", m.fighter[i]);
        n = json_put(json, (int)sizeof(json), n, online_fighter_name(m.fighter[i]));
        n += snprintf(json + n, sizeof(json) - n, ",\"cpu\":%s}", cpu ? "true" : "false");
    }
    n += snprintf(json + n, sizeof(json) - n, "],\"winners\":[");
    for (i = 0; i < m.count && i < MAXP; i++)
        if (winners & (1u << i)) { n += snprintf(json + n, sizeof(json) - n, "%s%d", first ? "" : ",", i); first = 0; }
    n += snprintf(json + n, sizeof(json) - n, "],\"draw\":%s}", (code & 4u) ? "true" : "false");
    if (n >= (int)sizeof(json)) return;
    fprintf(stderr, "[NETPLAY] match result: winners 0x%X, code 0x%X\n",
            (unsigned)winners, (unsigned)code);
    if (project_server_endpoint(N.server_text)) account_report_match(json);
}

void netplay_sync_point(void)
{
    if (!np_load(&N.lockstep)) return;
    if (N.lobby && N.match_live && !N.delay_fixed) {
        /* Every player's ping for this START, then the delay they all derive from them. */
        ULONGLONG t0 = GetTickCount64();
        int known, i;
        for (;;) {
            known = 1;
            EnterCriticalSection(&g_cs);
            for (i = 0; i < N.players; i++) if (N.peer_rtt[i] < 0) known = 0;
            LeaveCriticalSection(&g_cs);
            if (known || GetTickCount64() - t0 > RTT_WAIT_MS || !np_load(&N.lockstep)) break;
            WaitForSingleObject(N.arrived, 20);
        }
        if(!known || !np_load(&N.lockstep)){
            InterlockedExchange(&N.lockstep,0);
            SetEvent(N.arrived);
            set_status("Latency negotiation incomplete; return to the room and retry.");
            fprintf(stderr,"[NETDELAY126] incomplete RTT set: match not started with an unsafe guessed delay\n");
            return;
        }
        EnterCriticalSection(&g_cs);
        N.delay = agreed_delay();
        N.delay_fixed = 1;
        LeaveCriticalSection(&g_cs);
        send_rtt();
        fprintf(stderr, "[NETPLAY] input delay %d steps (pings %d/%d/%d/%d ms%s)\n", N.delay,
                N.peer_rtt[0], N.peer_rtt[1], N.peer_rtt[2], N.peer_rtt[3], known ? "" : ", some missing");
    }
    EnterCriticalSection(&g_cs);
    memset(N.seeds, 0, sizeof(N.seeds));
    LeaveCriticalSection(&g_cs);
    fprintf(stderr, "[NETPLAY] sync point: setting up the room's match\n");
}

void netplay_on_main_menu(void)
{
    if (N.lobby && N.match_live && N.match_seen)
        end_hub_match("Match over; back in the room.");
}

/* At the sync point, before the match is set up: the room's START came, but a player has left
 * since (the room says so, and the lockstep is off). Then no match is set up at all: the match
 * is over for this PC and it stays in the menu, instead of starting a fight the other side will
 * never join and dropping back to the main menu without a word. */
int netplay_match_abandoned(void)
{
    if (!N.lobby || !N.match_live || N.match_seen || np_load(&N.lockstep)) return 0;
    end_hub_match("The other player left before the fight started.");
    toast(6000, "The other player left before the fight started.");
    fprintf(stderr, "[NETPLAY] a player left before the match was set up; it is not started\n");
    return 1;
}

/* Unattended sessions for tests: RECOMP_NETPLAY=host or join, RECOMP_NETPLAY_SERVER, and
 * RECOMP_NETPLAY_CODE_FILE, where the host writes its room code once it has one and the
 * guest waits for it (up to a minute). RECOMP_NETPLAY=quick enters the hub, quick-matches
 * (RECOMP_NETPLAY_QUICK="mode,players", default "0,2") and readies with RECOMP_NETPLAY_FIGHTER
 * (default 55), voting for the first venue; =ranked does the same through the ranked queue
 * (the copy must be signed in; RECOMP_ACCOUNT_SERVER for test accounts); =lobby only
 * enters the hub. */
static DWORD WINAPI autostart_thread(LPVOID unused)
{
    const char *mode = getenv("RECOMP_NETPLAY"), *server = getenv("RECOMP_NETPLAY_SERVER");
    const char *file = getenv("RECOMP_NETPLAY_CODE_FILE");
    int i;
    (void)unused;
    if (!strcmp(mode, "lobby")) {
        netplay_lobby(server, "Test player");
        return 0;
    }
    if (!strcmp(mode, "quick") || !strcmp(mode, "ranked")) {
        const char *f = getenv("RECOMP_NETPLAY_FIGHTER");
        int fighter = f && *f ? atoi(f) : 55, readied = 0;
        unsigned voted_in = 0;
        netplay_lobby(server, "Test player");
        for (i = 0; i < 600 && np_load(&N.state) == NETPLAY_CONNECTING; i++) Sleep(100);
        {   /* RECOMP_NETPLAY_QUICK="mode,players", default "0,2" (One on One) */
            const char *q = getenv("RECOMP_NETPLAY_QUICK");
            int qmode = 0, qmax = 2;
            if (q && *q) sscanf(q, "%d,%d", &qmode, &qmax);
            if (!strcmp(mode, "ranked")) netplay_lobby_ranked(fighter);
            else netplay_lobby_quick(qmode, qmax);
        }
        while (!np_load(&N.stop) && np_load(&N.state) != NETPLAY_FAILED && np_load(&N.state) != NETPLAY_OFF) {
            NetRoom r;
            netplay_lobby_room(&r);
            if (r.in_room && r.phase == 0 && !N.match_live) {
                /* Ready again whenever this player shows as not ready: after a
                   match, or after a scripted press toggled it in the overlay. */
                int me, mine = !readied;
                for (me = 0; me < r.player_count; me++)
                    if (r.players[me].slot == r.you) { mine = !r.players[me].ready; break; }
                if (mine && !readied) {
                    netplay_lobby_ready(1, fighter);
                    readied = 1;
                } else if (mine && readied) {
                    readied = 0;    /* the server shows us not ready: send it again next pass */
                }
            }
            if (r.in_room && r.phase == 1 && voted_in != r.version) {
                int me;
                for (me = 0; me < r.player_count; me++)
                    if (r.players[me].slot == r.you && r.players[me].voted < 0) { netplay_lobby_vote(0); break; }
                voted_in = r.version;
            }
            if (r.phase == 2) readied = 0;
            Sleep(100);
        }
        return 0;
    }
    if (!strcmp(mode, "host")) {
        netplay_host(server, "Test host", 3);
        for (i = 0; i < 600 && !N.room[0] && np_load(&N.state) != NETPLAY_FAILED; i++) Sleep(100);
        if (N.room[0] && file && *file) {
            FILE *f = fopen(file, "w");
            if (f) { fputs(N.room, f); fclose(f); }
        }
    } else {
        char code[16] = "";
        for (i = 0; i < 600 && file && *file; i++) {
            FILE *f = fopen(file, "r");
            if (f) {
                if (fgets(code, sizeof(code), f) && strlen(code) >= 6) { fclose(f); break; }
                fclose(f);
            }
            Sleep(100);
        }
        code[6] = 0;
        netplay_join(server, "Test guest", code);
    }
    return 0;
}

void netplay_autostart(void)
{
    const char *mode = getenv("RECOMP_NETPLAY"), *server = getenv("RECOMP_NETPLAY_SERVER");
    static LONG once;
    HANDLE t;
    if (InterlockedExchange(&once, 1)) return;
    if (!mode || !server || (strcmp(mode, "host") && strcmp(mode, "join") && strcmp(mode, "quick") && strcmp(mode, "ranked") && strcmp(mode, "lobby"))) return;
    t = CreateThread(NULL, 0, autostart_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}

int netplay_active(void) { return np_load(&N.lockstep) != 0; }
int netplay_in_fight(void) { return MEM32(G_MATCH_PHASE) == 1u; }

/* ---- the game's hooks ------------------------------------------------------------ */

static uint32_t fighter_hash(void)
{
    static const uint32_t fields[] = { 0x30, 0x44, 0x48, 0x4C, 0x54, 0x350 };
    uint32_t h = 2166136261u;
    int slot;
    unsigned i;
    for (slot = 0; slot < (N.fighters > N.players ? N.fighters : N.players); slot++)
        for (i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
            h = (h ^ MEM32(ACTORS + (uint32_t)slot * STRIDE + fields[i])) * 16777619u;
    return h;
}

/* The checksums of steps up to `upto` (the state each one started from), once they are final. */
static void send_hashes(uint32_t upto)
{
    uint32_t t;
    for (t = N.hash_sent + 1; t <= upto; t++) {
        uint8_t p[13];
        uint32_t h;
        if (t % HASH_EVERY || N.hash_tag[t % RING] != t + 1) continue;
        h = N.hash_at[t % RING];
        EnterCriticalSection(&g_cs);
        memmove(N.local_hash[1], N.local_hash[0], sizeof(N.local_hash[0]) * 15);
        N.local_hash[0][0] = N.match; N.local_hash[0][1] = t; N.local_hash[0][2] = h;
        check_hashes();
        LeaveCriticalSection(&g_cs);
        p[0] = P_HASH; put32(p + 1, N.match); put32(p + 5, t); put32(p + 9, h);
        send_data(p, 13);
    }
    if (upto > N.hash_sent) N.hash_sent = upto;
}

/* RECOMP_ROLLBACK_SOAK=<depth> (tests, offline): every few updates of a fight the game goes back
 * <depth> steps (2..12) and runs them again, and the per-step fingerprints of the second run
 * (test_telemetry.c) are compared with the first. A difference names the first step that came out
 * otherwise: what the save states miss. */
static int soak_depth(void)
{
    static int d = -1;
    if (d < 0) {
        const char *v = getenv("RECOMP_ROLLBACK_SOAK");
        d = v ? atoi(v) : 0;
        if (d && (d < 2 || d > 12)) d = 6;
    }
    return d;
}

int netplay_replaying131(void){return replay_audio131;}
int netplay_verifying131(void){return soak_depth()!=0 && rollback_enabled();}

static void soak_match_start(void)
{
    InitOnceExecuteOnce(&g_once,init_once,NULL,NULL);
    memset(verify_pad_tag131,0,sizeof verify_pad_tag131);
    memset(verify_pad_seen131,0,sizeof verify_pad_seen131);
    verify_warm_base131=verify_last_step131=0;N.rb_fought=0;
    N.step = 0;
    N.final_step = N.hash_sent = N.updates = 0;
    N.rb_on = N.rb_ref_due = N.resim = N.predicting = 0;
    memset(N.snap, 0, sizeof(N.snap));
    rollback_reset();
    fprintf(stderr, "[ROLLBACK] soak: going back %d steps every few updates\n", soak_depth());
}

void netplay_on_match_start(void)
{
    if (!np_load(&N.lockstep)) { if (soak_depth()) soak_match_start(); return; }
    EnterCriticalSection(&g_cs);
    N.match_count++;
    N.match = mix32(N.session ^ (N.match_count * 0x9E3779B9u));   /* the same on every PC, new per match */
    N.step = 0;
    /* every match of a START is reported on its own (the results screen's rematch runs a
     * second match without a new START): the key gets the match's number */
    if (N.report_key[0]) {
        char *dash = strchr(N.report_key, '-');
        if (dash && (dash = strchr(dash + 1, '-')) != NULL) *dash = 0;
        snprintf(N.report_key + strlen(N.report_key), sizeof(N.report_key) - strlen(N.report_key), "-%u",
                 (unsigned)N.match_count);
    }
    N.reported = N.fighting_seen = 0;
    memset(N.latest_local, 0, sizeof(N.latest_local));
    N.desync = 0;
    if (N.match_live) N.match_seen = 1;
    N.final_step = N.hash_sent = N.updates = 0;
    N.rb_on = N.rb_ref_due = N.resim = N.predicting = N.rb_fought = 0;
    memset(N.used_tag, 0, sizeof(N.used_tag));
    memset(N.hash_tag, 0, sizeof(N.hash_tag));
    memset(N.snap, 0, sizeof(N.snap));
    LeaveCriticalSection(&g_cs);
    rollback_reset();
    /* From the load on, not from the first step: a START pressed while the venue loads or the
     * fighters' intro runs (players press it to skip the intro) paused that game before its
     * first step, and the others waited for it until they gave up (seen on The Pit, whose load is
     * the longest). */
    fight_input();
    fprintf(stderr, "[NETPLAY] match %08X starts in lockstep (%d players, delay %d)\n", N.match, N.players, N.delay);
    if(N.lobby){
        int required128=0,extended128=0;
        online_delay126(N.peer_rtt,N.peer_rb,N.peer_delay126,N.players,&required128,&extended128);
        if(required128>N.delay){
            fprintf(stderr,"[NETBUDGET128] required=%d applied=%d all_capable=%d; %s\n",required128,N.delay,extended128,
                extended128?"RTT exceeds negotiated maximum; a lower-latency route is needed":"every player must update to negotiate the larger delay; unilateral changes would desync");
            toast(10000,extended128?"Connection latency exceeds this match's input buffer.":
                "A peer lacks extended input buffering. Update all clients to support this high ping.");
        }
    }
    log_contiguous("at the match start");
}

void netplay_on_pad_pass(void) {}

static void send_steps(uint32_t last)
{
    uint8_t p[1 + 9 + REDUNDANT * ENTRY];
    uint32_t first = last >= REDUNDANT ? last - REDUNDANT + 1 : 1, s;
    int count = 0, local = N.pos;
    p[0] = P_STEPS;
    put32(p + 1, N.match);
    put32(p + 5, last);
    EnterCriticalSection(&g_cs);
    for (s = first; s <= last; s++) {
        uint32_t k = s % RING;
        if (N.in_tag[local][k] == s + 1)
            memcpy(p + 10 + count * ENTRY, N.in[local][k], ENTRY);
        else
            memset(p + 10 + count * ENTRY, 0, ENTRY);
        count++;
    }
    LeaveCriticalSection(&g_cs);
    p[9] = (uint8_t)count;
    send_data(p, 10 + count * ENTRY);
}

/* ---- the crowd, the same on every PC ------------------------------------------------------------
 *
 * The arena's spectators start animating while the match loads, at host timing, and one of the
 * generators they draw from (0x33DB5C) is seeded from the wall clock at boot, so each PC's crowd
 * leaves the loading window in a state of its own. Through the crowd's record table that reaches
 * the fight as catches, holds and weapon offers, which parted the lockstep. At the first fight step
 * every PC therefore restarts the crowd the way the game does at a match start on a loaded arena
 * (sub_00080880), from shared seeds, and evens out what that restart leaves alone. From then on the
 * crowd updates once a step on every PC. docs/research/crowd-lockstep-reset.md (recipe a, G1). */
extern void sub_000AA0D0(void);     /* generator seed: ecx = generator, two words */
extern void sub_0002A720(void);     /* the global generator 0x33DB5C from one word */
extern void sub_00080880(void);     /* the crowd's match-start restart: ecx = manager */
extern void sub_0007F030(void);     /* publishes the spectators' records: ecx = manager */
extern void sub_0007D240(void);     /* resets one member's state: ecx = member */
extern void sub_0007F3D0(void);     /* crowd intensity: ecx = manager, one float */
extern void sub_0007F6E0(void);     /* deals the next idle state by quota: ecx = manager, returns it */
extern void sub_0007D2F0(void);     /* starts a member's clip: ecx = member, state, -1 */
extern void sub_0007D560(void);     /* samples a member's pose: ecx = member, 0 */

#define CROWD_VTABLE    0x0029EBE8u
#define GLOBAL_GEN      0x0033DB5Cu     /* 6 words; the crowd and other, host-timed users draw it */

/* The crowd's own copy of the global generator for the match (crowd_reset makes it from the match
 * seed); netplay_crowd_update swaps it in around every crowd update, so draws elsewhere at host
 * timing cannot reach the crowd. */
static uint32_t g_crowd_gen[6];
static int g_crowd_gen_on;
#define CROWD_MANAGER   0x0002B9F0u     /* the manager inside the arena object */

static void guest_call(void (*fn)(void), uint32_t ecx, int nargs, const uint32_t *args)
{
    uint32_t sp = g_esp;
    int i;
    g_esp = (sp - 0x80u) & ~3u;         /* below the hook's frame */
    for (i = nargs - 1; i >= 0; i--) PUSH32(g_esp, args[i]);
    PUSH32(g_esp, 0u);                  /* a return address the lifted ret only pops */
    g_ecx = ecx;
    fn();
    g_esp = sp;                         /* whatever the callee popped */
}

/* The crowd's update (vtable+8 of the crowd manager, called only indirectly by the arena), as
 * recomp_lookup_manual hands it out: in a lockstep fight it draws from the crowd's copy of the
 * global generator. */
extern void sub_00081170(void);
void netplay_crowd_update(void)
{
    uint32_t real[6], i;
    if (!np_load(&N.lockstep) || !g_crowd_gen_on) { sub_00081170(); return; }
    for (i = 0; i < 6; i++) { real[i] = MEM32(GLOBAL_GEN + 4u * i); MEM32(GLOBAL_GEN + 4u * i) = g_crowd_gen[i]; }
    sub_00081170();
    for (i = 0; i < 6; i++) { g_crowd_gen[i] = MEM32(GLOBAL_GEN + 4u * i); MEM32(GLOBAL_GEN + 4u * i) = real[i]; }
}

static void crowd_reset(uint32_t arena)
{
    uint32_t mgr = arena + CROWD_MANAGER, xml, n, i, args[2];
    int with_text;
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp, esp = g_esp;
    double fp[8];
    int fp_top = g_fp_top;
    uint16_t fp_cw = g_fp_control_word;
    if (MEM32(mgr) != CROWD_VTABLE) {
        fprintf(stderr, "[NETPLAY] crowd: no crowd manager at %08X (vtable %08X); left as it is\n", mgr, MEM32(mgr));
        return;
    }
    n = MEM32(mgr + 0x18u);
    xml = MEM32(mgr + 0x24u);
    if (!n || n > 256 || !MEM32(mgr + 0xA90u) || (MEM32(mgr + 0x20u) & 1u)) {
        fprintf(stderr, "[NETPLAY] crowd: not in a state to restart (members %u, flags %08X); left as it is\n",
                n, MEM32(mgr + 0x20u));
        return;
    }
    /* The game's restart re-reads the arena's crowd text; by the first fight step that text can be
     * gone, and then the same restart is done field by field (recipe b). */
    with_text = xml && MEM32(xml + 0xA4u) && (int32_t)MEM32(xml + 0xA8u) > 0;
    memcpy(fp, g_fp_stack, sizeof(fp));
    /* 1. the crowd's own generator, and the global one, from the match's seed */
    args[0] = mix32(N.session ^ 0xC0A7D001u);
    args[1] = mix32(N.session ^ 0x5EED0002u) | 1u;
    N.seed_bypass = 1;
    guest_call(sub_000AA0D0, mgr + 0xE18u, 2, args);
    N.seed_bypass = 0;
    {   /* the crowd's own copy of the global generator, made by the game's own seeding routine */
        uint32_t real[6];
        for (i = 0; i < 6; i++) real[i] = MEM32(GLOBAL_GEN + 4u * i);
        args[0] = mix32(N.session ^ 0x0033DB5Cu);
        guest_call(sub_0002A720, 0, 1, args);
        for (i = 0; i < 6; i++) { g_crowd_gen[i] = MEM32(GLOBAL_GEN + 4u * i); MEM32(GLOBAL_GEN + 4u * i) = real[i]; }
        g_crowd_gen_on = 1;
    }
    /* 2. the game's own restart: states, clip times, variants, offers, intensity */
    if (with_text) {
        guest_call(sub_00080880, mgr, 0, NULL);
    } else {
        uint32_t recs = MEM32(0x003494E4u);
        for (i = 0; i < n; i++) {
            uint32_t m = MEM32(mgr + 0xA90u + 4u * i), idx;
            if (!m) continue;
            guest_call(sub_0007D240, m, 0, NULL);
            MEM32(m + 0xC8u) = 0xFFFFFFFFu;
            MEM32(m + 0x10u) = MEM32(m);
            idx = MEM32(m + 0x0Cu);
            if (recs && idx != 0xFFFFFFFFu && idx < MEM32(0x003494E0u)) MEM32(recs + idx * 0x34u + 0x30u) = 0xFFFFFFFFu;
        }
        MEM32(mgr + 0xC98u) = 0;
        MEM32(mgr + 0xCD0u) = 0;
        MEM32(mgr + 0xCCCu) = 0xFFFFFFFFu;
        MEM32(mgr + 0xCD4u) = 0;
        MEM32(mgr + 0xC20u) = MEM32(mgr + 0xC24u) = 0x42700000u;     /* 60.0 */
        MEM32(mgr + 0xC2Cu) = 0xBF800000u;                           /* -1.0 */
        MEM32(mgr + 0xC34u) = 0;
        args[0] = 0;                                                  /* 0.0f */
        guest_call(sub_0007F3D0, mgr, 1, args);
        for (i = 0; i < n; i++) {                                     /* member order, as the game deals */
            uint32_t m = MEM32(mgr + 0xA90u + 4u * i), state;
            if (!m || MEM32(m) == 0xCu) continue;
            guest_call(sub_0007F6E0, mgr, 0, NULL);
            state = g_eax;
            args[0] = state;
            args[1] = 0xFFFFFFFFu;
            guest_call(sub_0007D2F0, m, 2, args);
            args[0] = 0;
            guest_call(sub_0007D560, m, 1, args);
        }
    }
    /* 3. what it leaves: the update rotation, each spectator's speed and two timers */
    MEM32(mgr + 0x1Cu) = 0;
    for (i = 0; i < n; i++) {
        uint32_t m = MEM32(mgr + 0xA90u + 4u * i), bits;
        float speed;
        if (!m) continue;
        speed = MEM32(m) == 0xDu ? 0.75f : (float)((int)(mix32(N.session ^ (0xC20D0000u + i)) % 60u) - 30) * 0.01f + 1.0f;
        memcpy(&bits, &speed, 4);
        MEM32(m + 0x68u) = bits;
        MEM8(m + 0xDCu) = 0;
        MEM32(m + 0xD8u) = 0;
    }
    /* 4. the records the fighters read, before the step's first update */
    guest_call(sub_0007F030, mgr, 0, NULL);
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi; g_ebp = ebp; g_esp = esp;
    memcpy(g_fp_stack, fp, sizeof(fp));
    g_fp_top = fp_top;
    g_fp_control_word = fp_cw;
    fprintf(stderr, "[NETPLAY] crowd restarted for the match (%u spectators, %s)\n", n,
            with_text ? "the game's restart" : "field by field");
}

static void netwait126_record(int peer,ULONGLONG ms,uint32_t step)
{
    static ULONGLONG epoch,total[MAXP],max_wait[MAXP];
    static unsigned calls[MAXP];static uint32_t match;
    ULONGLONG now=GetTickCount64();
    if(match!=N.match){match=N.match;epoch=now;memset(total,0,sizeof total);memset(max_wait,0,sizeof max_wait);memset(calls,0,sizeof calls);}
    if(peer>=0&&peer<N.players){total[peer]+=ms;calls[peer]++;if(ms>max_wait[peer])max_wait[peer]=ms;}
    if(now-epoch<10000)return;
    for(int i=0;i<N.players;i++)if(i!=N.pos)
        fprintf(stderr,"[NETWAIT126] peer=%d step=%u wait_calls=%u wait_ms=%llu max_event_wait_ms=%llu interval_ms=%llu delay=%d rtt_ms=%d; event waits include scheduling\n",
            i+1,step,calls[i],(unsigned long long)total[i],(unsigned long long)max_wait[i],(unsigned long long)(now-epoch),N.delay,N.peer_rtt[i]);
    ULONGLONG waited128=0;unsigned wakes128=0;
    for(int i=0;i<N.players;i++)if(i!=N.pos){waited128+=total[i];wakes128+=calls[i];}
    fprintf(stderr,"[NETPERF128] step=%u event_wait_ms=%llu interval_ms=%llu event_wait_pct=%.2f wake_calls=%u delay=%d; waiting is remote input plus scheduling, not renderer CPU\n",
        step,(unsigned long long)waited128,(unsigned long long)(now-epoch),100.0*waited128/(now-epoch),wakes128,N.delay);
    epoch=now;memset(total,0,sizeof total);memset(max_wait,0,sizeof max_wait);memset(calls,0,sizeof calls);
}

void netplay_on_step(void)
{
    uint32_t s, target, k, need;
    ULONGLONG waited_since, last_send;
    recomp_detmath_fenv_guard();
    fight_input();              /* also after a fight went on offline */
    if (!np_load(&N.lockstep)) {
        if (soak_depth()) {     /* tests: the soak counts steps and prepares the save states itself */
            s = ++N.step;
            if(s==1)InitOnceExecuteOnce(&g_once,init_once,NULL,NULL);
            if (s == 1 && !N.resim && rollback_prepare() == 0) N.rb_on = N.rb_ref_due = 1;
        }
        return;
    }
    /* RECOMP_ONLINE_CROWD=off (tests only): the spectators cannot touch the fight. Their animation
     * differs between the PCs (it starts while the match loads, at host timing) and reaches the
     * fighters through the crowd's record table as catches, holds and weapons, which parts the
     * lockstep. With the record count at 0 no scan sees a spectator. Catches and crowd weapons are
     * part of the game, so this is a comparison switch, not the fix. docs/research/spectator-shove.md */
    {
        static int crowd_off = -1;
        if (crowd_off < 0) crowd_off = getenv("RECOMP_ONLINE_CROWD") && !strcmp(getenv("RECOMP_ONLINE_CROWD"), "off");
        if (crowd_off) MEM32(G_CROWD_RECORDS) = 0;
    }
    /* The room match decided (the game's phase word: 1 fighting, 2 decided, 3 finished; see
     * test_telemetry.c): report its result. Only after this match was seen fighting, as the
     * phase still holds the last match's value for a moment after a START. */
    if (N.lobby && N.match_live && !N.reported && !N.resim && (!N.rb_on || N.step <= N.final_step)) {
        uint32_t phase = MEM32(G_MATCH_PHASE);
        if (phase == 1u) N.fighting_seen = 1;
        else if (N.fighting_seen && phase >= 2u && phase <= 3u) {
            uint32_t w = MEM32(G_MATCH_WINNER);
            netplay_on_result(w < MAXP ? 1u << w : 0u, 0, w < MAXP ? 1u : 4u);
        }
    }
    s = ++N.step;
    if (s == 1) {
        static int crowd_on = -1;   /* RECOMP_ONLINE_CROWD=keep: tests compare without the restart */
        if (crowd_on < 0) crowd_on = !(getenv("RECOMP_ONLINE_CROWD") && !strcmp(getenv("RECOMP_ONLINE_CROWD"), "keep"));
        if (crowd_on) crowd_reset(g_ecx);
        if (!N.resim && rollback_wanted()){
            if(rollback_prepare()==0)N.rb_on=N.rb_ref_due=1;
            else {rollback_abort131("negotiated rollback preparation failed");return;}
        }
    }
    {
        /* The arena hands its frame argument (edi, kept by sub_00087700 for the crowd update that
         * follows) to the crowd; it is the host's measured frame time, so the spectators' clips
         * advanced by a different amount on each PC. Online the crowd gets the fight's fixed step. */
        uint32_t dt = g_edi;
        float f;
        memcpy(&f, &dt, 4);
        if (s <= 3 || s == 300)
            fprintf(stderr, "[NETPLAY] crowd frame argument at step %u: %08X (%g as a float)\n", s, dt, (double)f);
        if (f > 0.001f && f < 0.25f) {
            f = 1.0f / 60.0f;
            memcpy(&g_edi, &f, 4);
        }
    }
    if (s <= 120 && getenv("RECOMP_CROWD_TRACE")) {
        /* tests: the crowd's three parts, every step, to see which parts first and when */
        uint32_t hr = 2166136261u, hg = 2166136261u, hu = 2166136261u, k, recs = MEM32(0x003494E4u);
        uint32_t count = MEM32(0x003494E0u);
        for (k = 0; recs && k < count * 0x34u && k < 0x4000u; k++) hr = (hr ^ MEM8(recs + k)) * 16777619u;
        for (k = 0; k < 0x88u; k++) hg = (hg ^ MEM8(g_ecx + CROWD_MANAGER + 0xE18u + k)) * 16777619u;
        for (k = 0; k < 6u; k++) hu = (hu ^ g_crowd_gen[k]) * 16777619u;
        fprintf(stderr, "[NETPLAY] crowd parts at step %u: records %08X crowd-gen %08X global-gen %08X\n", s, hr, hg, hu);
    }
    if (s == 1 || s == 2 || s == 60 || s == 300 || s == 900 || s == 1800) {
        /* the crowd's state, to compare between the PCs: the records, its generator, the global one */
        uint32_t h = 2166136261u, k, recs = MEM32(0x003494E4u), count = MEM32(0x003494E0u);
        for (k = 0; recs && k < count * 0x34u && k < 0x4000u; k++) h = (h ^ MEM8(recs + k)) * 16777619u;
        for (k = 0; k < 0x88u; k++) h = (h ^ MEM8(g_ecx + CROWD_MANAGER + 0xE18u + k)) * 16777619u;
        for (k = 0; k < 6u; k++) h = (h ^ g_crowd_gen[k]) * 16777619u;     /* the crowd's copy, not the real one */
        fprintf(stderr, "[NETPLAY] crowd hash at step %u: %08X (%u records)\n", s, h, count);
    }
    k = s % RING;
    N.hash_at[k] = fighter_hash();      /* the state as this step starts */
    N.hash_tag[k] = s + 1;
    if (N.resim) return;                /* a re-run: the entries are all known or predicted */
    target = s + (uint32_t)N.delay;
    k = target % RING;
    EnterCriticalSection(&g_cs);
    memcpy(N.in[N.pos][k], N.latest_local, ENTRY);
    N.in_tag[N.pos][k] = target + 1;
    N.in_match[N.pos][k] = N.match;
    LeaveCriticalSection(&g_cs);
    send_steps(target);

    if (!N.rb_on) send_hashes(s);      /* with rollback, once the steps before are final (netplay_on_update) */

    /* Wait for every other player's entry for this step; while they fight, with rollback, only
     * for the entries RB_AHEAD steps back (the newer ones are predicted). */
    if (s <= (uint32_t)N.delay) return;
    /* only once every other PC is under way (an entry of its own has arrived): one may still be
     * loading the venue, and a PC that ran ahead would give up on it after STALL_LIMIT_MS */
    N.predicting = N.rb_on && rollback_ready() && MEM32(G_MATCH_PHASE) == 1u && N.final_step > (uint32_t)N.delay;
    need = N.predicting ? (s > RB_AHEAD ? s - RB_AHEAD : 0) : s;
    if (need <= (uint32_t)N.delay) return;
    waited_since = last_send = GetTickCount64();
    for (;;) {
        int ready = 1, i, late = -1;
        uint32_t t = need;
        if (N.rb_on) { t = N.final_step + 1; if (t <= (uint32_t)N.delay) t = (uint32_t)N.delay + 1; }
        EnterCriticalSection(&g_cs);
        for (; t <= need; t++)
            for (i = 0; i < N.players; i++)
                if (i != N.pos && !(N.in_tag[i][t % RING] == t + 1 && N.in_match[i][t % RING] == N.match)) { ready = 0; late = i; }
        LeaveCriticalSection(&g_cs);
        if (ready || !np_load(&N.lockstep)) {
            ULONGLONG waited = GetTickCount64() - waited_since;
            /* a frozen picture of half a second or more: say why once it moves again */
            if (ready && waited >= 500 && s > (uint32_t)N.delay + 1)
                toast(4000, "Waited %.1f s for %s (connection).", waited / 1000.0, player_name(N.last_late));
            break;
        }
        N.last_late = late;
        unsigned scope126=dj_trace(DJ_SYNC);
        ULONGLONG wait126=GetTickCount64();
        /* Arrival/cancel signals wake immediately; no250Hz polling when
         * the remote player is late. Preserve the50ms retransmit deadline. */
        WaitForSingleObject(N.arrived, online_wait128(wait126,last_send));
        ULONGLONG waited128=GetTickCount64()-wait126;
        defjam_monitor_network_wait((uint64_t)waited128*1000000ull);
        netwait126_record(late,waited128,s);
        dj_trace_end(scope126);
        {
            ULONGLONG now = GetTickCount64();
            if (now - last_send >= 50) { send_steps(target); last_send = now; }   /* the others may be waiting for ours */
            /* The fight decided (KO, time): what follows is the winner's scene and the results, the same
             * on every PC however they part, and a PC that skips ahead stops sending steps. Waiting the
             * mid-fight ten seconds there froze the winner's scene and then said the fight would not
             * count, after it had (Farouk's F9 report 63684c, 2026-10-23). */
            int decided = N.fighting_seen && MEM32(G_MATCH_PHASE) >= 2u;
            if (now - waited_since > (decided ? DECIDED_LIMIT_MS :
                                      need <= (uint32_t)N.delay + 1 ? START_LIMIT_MS : STALL_LIMIT_MS)) {
                uint32_t u;
                if (decided) {
                    fprintf(stderr, "[NETPLAY] the match is decided and player %d went on ahead (step %u): the rest "
                            "runs on this PC alone\n", late, s);
                    InterlockedExchange(&N.lockstep, 0);
                    set_status("The match is over; the rest runs on this PC.");
                    break;
                }
                fprintf(stderr, "[NETPLAY] gave up at step %u waiting for the entries up to %u (final %u, phase %u, "
                        "predicting %d, rollback %d); player %d has:", s, need, N.final_step, MEM32(G_MATCH_PHASE),
                        N.predicting, N.rb_on, late);
                for (u = N.final_step + 1; u <= need + 2 && late >= 0; u++)
                    fprintf(stderr, " %u:%u/%08X", u, N.in_tag[late][u % RING], N.in_match[late][u % RING]);
                fprintf(stderr, " (match %08X; STEPS packets from it %u, last step %u)\n", N.match,
                        late >= 0 ? g_steps_rx[late] : 0, late >= 0 ? g_steps_last[late] : 0);
                InterlockedExchange(&N.lockstep, 0);
                set_status("A player stopped answering during the fight; continuing offline.");
                toast(10000, "%s stopped answering: the fight goes on offline and won't count.", player_name(late));
                break;
            }
        }
    }
}

void netplay_on_pad_read(uint32_t actor)
{
    uint32_t slot, base, s;
    int i;
    uint8_t entry[ENTRY];
    if (!np_load(&N.lockstep)){
        if(!soak_depth()||!N.rb_on||!N.step||actor<ACTORS||(actor-ACTORS)%STRIDE)return;
        uint32_t vs=(actor-ACTORS)/STRIDE,vk=N.step%RING;if(vs>=MAXP)return;
        uint32_t vb=PAD_TABLE+vs*ENTRY;
        if(N.resim){
            if(verify_pad_tag131[vs][vk]!=N.step+1){replay_input_failed131=1;return;}
            if(verify_pad_seen131[vs]!=N.step+1){
                for(unsigned n=0;n<ENTRY;n++)MEM8(vb+n)=verify_pad131[vs][vk][n];
                verify_pad_seen131[vs]=N.step+1;
            }
        }else if(verify_pad_tag131[vs][vk]!=N.step+1){
            for(unsigned n=0;n<ENTRY;n++)verify_pad131[vs][vk][n]=MEM8(vb+n);
            verify_pad_tag131[vs][vk]=N.step+1;
        }
        return;
    }
    if (actor < ACTORS || (actor - ACTORS) % STRIDE) return;
    slot = (actor - ACTORS) / STRIDE;
    if ((int)slot >= N.players) return;
    base = PAD_TABLE + slot * 0x20u;
    /* This PC's player: a fresh poll (no marker) is the input to send next. */
    if ((int)slot == N.pos && MEM32(base + 28) != MARK && !N.resim) {
        EnterCriticalSection(&g_cs);
        for (i = 0; i < ENTRY; i++) N.latest_local[i] = MEM8(base + (uint32_t)i);
        N.latest_local[28] = N.latest_local[29] = N.latest_local[30] = N.latest_local[31] = 0;
        LeaveCriticalSection(&g_cs);
    }
    s = N.step;
    memset(entry, 0, sizeof(entry));
    if (s > (uint32_t)N.delay) {
        uint32_t k = s % RING;
        int predicted = 0;
        EnterCriticalSection(&g_cs);
        if (N.in_tag[slot][k] == s + 1 && N.in_match[slot][k] == N.match)
            memcpy(entry, N.in[slot][k], ENTRY);
        else if (N.predicting || N.resim) {     /* not here yet: that player's last entry */
            uint32_t t;
            predicted = 1;
            for (t = s - 1; t > (uint32_t)N.delay && t + 64 > s; t--)
                if (N.in_tag[slot][t % RING] == t + 1 && N.in_match[slot][t % RING] == N.match) {
                    memcpy(entry, N.in[slot][t % RING], ENTRY);
                    break;
                }
        }
        if ((int)slot != N.pos) {
            memcpy(N.used[slot][k], entry, ENTRY);
            N.used_tag[slot][k] = s + 1;
            N.used_pred[slot][k] = (uint8_t)predicted;
        }
        LeaveCriticalSection(&g_cs);
    }
    {   /* RECOMP_NETPLAY_PADLOG=n: the first n reads, polled and injected, with the slot's bindings */
        static int budget = -1;
        if (budget < 0) budget = getenv("RECOMP_NETPLAY_PADLOG") ? atoi(getenv("RECOMP_NETPLAY_PADLOG")) : 0;
        if (budget > 0 && (s % 30 == 5 || s < 8)) {
            uint32_t bind = 0x003B90C8u + slot * 0x18u;
            budget--;
            fprintf(stderr, "[PADLOG] step %u slot %u polled %08X %08X %08X w7 %08X | injected %08X %08X %08X | "
                    "mask %08X bind %08X %08X %08X %08X %08X %08X\n", s, slot,
                    MEM32(base), MEM32(base + 4), MEM32(base + 8), MEM32(base + 28),
                    get32(entry), get32(entry + 4), get32(entry + 8), MEM32(0x003B8FC8u + slot * 4u),
                    MEM32(bind), MEM32(bind + 4), MEM32(bind + 8), MEM32(bind + 12), MEM32(bind + 16), MEM32(bind + 20));
        }
    }
    for (i = 0; i < 7; i++)
        MEM32(base + (uint32_t)i * 4) = get32(entry + i * 4);
    MEM32(base + 28) = MARK;
}

/* Android rollback replay uses the existing PC rules4 input protocol. */
/* ---- rollback ---------------------------------------------------------------------------
 *
 * At the start of every game update (the main loop's dt read, src/recomp_manual.c) the state is
 * saved for the step about to run. When an entry arrives that differs from the prediction a step
 * ran with, the save from before that step is loaded and the updates since are run again here,
 * the way the main loop runs them (sub_00011CD0: input, the screen manager, the event manager and
 * its snapshot, the render subsystem's tick), with the sound engine's commands silenced. */
extern void sub_000DD930(void);     /* the input subsystem's update: ecx = it, dt */
extern void sub_00077040(void);     /* the screen manager's update: ecx = it, dt; al = go on */
extern void sub_000BD860(void);     /* the event manager: ecx = it */
extern void sub_000BDF40(void);     /* its copy of the public buffers: ecx = it */
extern void sub_0001A240(void);     /* the render subsystem's per-update tick: ecx = it, dt */
extern void sub_00137010(void);     /* the sound engine's command call */
#define DT_BITS 0x3C888889u         /* 1/60, the constant every update gets */

void netplay_sound_command(void)
{
    if (replay_audio131) { g_eax = 0; g_esp += 4; return; }     /* cdecl: the caller pops the arguments */
    rollback_sound_context(1);
    sub_00137010();
    rollback_sound_context(0);
}

/* The audio subsystem's message-bus handlers (rollback-step-structure.md 2.1): while steps run
 * again they do nothing, so the game's sound state stays what the sound engine was told. cdecl. */
#define SOUND_HANDLERS(X) X(00034A60) X(00038440) X(00035250) X(00035670) X(00035890)                           X(00035970) X(00035A20) X(00035AE0) X(00038620) X(00038760)
#define SOUND_WRAP(a) extern void sub_##a(void);     static void snd_##a(void) { if (replay_audio131) { g_eax = 0; g_esp += 4; return; } sub_##a(); }
SOUND_HANDLERS(SOUND_WRAP)

void *netplay_sound_handler(uint32_t va)
{
#define SOUND_CASE(a) if (va == 0x##a##u) return (void *)snd_##a;
    SOUND_HANDLERS(SOUND_CASE)
    return NULL;
}

static uint32_t g_last_rb_target, g_last_rb_from;

static void rollback_crash_state(void)
{
    fprintf(stderr, "[ROLLBACK] at the crash: %s, step %u, final %u, update %u, %u rollbacks, the last back to %u from %u, thread %lu%s\n",
            N.resim ? "running steps again" : "a normal update", N.step, N.final_step, N.updates, N.rb_count,
            g_last_rb_target, g_last_rb_from, (unsigned long)GetCurrentThreadId(), N.predicting ? ", predicting" : "");
    {   /* EAGL's loaded objects (list head 0x3A52A8, next at +4): which one is bad, and its page */
        uint32_t o = MEM32(0x003A52A8u);
        int k;
        for (k = 0; k < 48 && o; k++) {
            int sane = rollback_in_heap(o);
            fprintf(stderr, "[ROLLBACK]   loaded object %08X (%s)%s\n", o, rollback_page_state(o), sane ? "" : " NOT A HEAP ADDRESS");
            if (!sane) break;
            {   /* dlsym's pointers: +8 string table, +0x10 symbol count, +0x14 symbol table, +0x420 chain, +0x424 flags */
                static const uint32_t off[] = { 8u, 0x10u, 0x14u, 0x18u, 0x1Cu, 0x420u, 0x424u, 0x42Cu };
                unsigned j;
                for (j = 0; j < sizeof(off) / sizeof(off[0]); j++) {
                    uint32_t v = MEM32(o + off[j]);
                    fprintf(stderr, "[ROLLBACK]     +%03X = %08X (%s)\n", off[j], v, rollback_page_state(v));
                }
            }
            o = MEM32(o + 4u);
        }
    }
    fflush(stderr);
}

static void rollback_abort131(const char *reason){
    N.rb_on=N.rb_ref_due=N.predicting=0;rollback_reset();netplay_stop();
    fprintf(stderr,"[ROLLBACK131] cancelled: %s; no fabricated final acknowledgement\n",reason);
}

static void snap_save(void)
{
    uint32_t step = N.step + 1;
    int slot = (int)(step % ROLLBACK_SLOTS);
    if (rollback_save(slot)) { N.snap[slot].valid = 0; return; }
    N.snap[slot].valid = 1;
    N.snap[slot].step = step;
    N.snap[slot].update = N.updates;
    N.snap[slot].test_step = defjam_test_current_step();
    memcpy(N.snap[slot].crowd_gen, g_crowd_gen, sizeof(g_crowd_gen));
    N.snap[slot].crowd_gen_on = g_crowd_gen_on;
    EnterCriticalSection(&g_cs);
    memcpy(N.snap[slot].seeds, N.seeds, sizeof(N.snap[slot].seeds));
    LeaveCriticalSection(&g_cs);
}

static void rollback_to(uint32_t f)
{
    int slot = (int)(f % ROLLBACK_SLOTS);
    uint32_t n, j;
    /* the objects the main loop updates, from the game's registry (sub_00012CF0 reads 0x2FB148[slot]) */
    uint32_t input = MEM32(0x002FB148u + 0x19u * 4u), machine = MEM32(0x002FB148u + 0x17u * 4u);
    uint32_t em = MEM32(0x002FB148u + 0x1Eu * 4u), render = MEM32(0x002FB148u + 4u * 4u);
    uint32_t dt = DT_BITS, depth = N.step + 1 - f;
    uint32_t eax = g_eax, ecx = g_ecx, edx = g_edx, ebx = g_ebx, esi = g_esi, edi = g_edi, ebp = g_ebp, esp = g_esp;
    double fp[8];
    int fp_top = g_fp_top;
    uint16_t fp_cw = g_fp_control_word;
    uint32_t seh131=g_seh_ebp,fs131=g_fs_base;
    int df131=g_df,fcmp131=g_fp_cmp;uint16_t fcc131=g_fp_cc;
    RecompXmm xmm131[8]={g_xmm0,g_xmm1,g_xmm2,g_xmm3,g_xmm4,g_xmm5,g_xmm6,g_xmm7};
    uint64_t mmx131[8];xbox_rollback_mmx_save131(mmx131);
    LARGE_INTEGER freq, t0, t1;
    double ms;
    unsigned loads_before;
    if (!N.snap[slot].valid || N.snap[slot].step != f || N.updates < N.snap[slot].update ||
        N.updates - N.snap[slot].update > 4 * ROLLBACK_SLOTS) {
        if (N.rb_failed++ < 5)
            fprintf(stderr, "[ROLLBACK] no save state for step %u (now %u): this match may go out of sync\n", f, N.step);
        rollback_abort131("required snapshot unavailable");
        return;
    }
    if(rollback_checkpoint131()){rollback_abort131("cannot allocate recovery checkpoint");return;}
    uint32_t original_step131=N.step,original_updates131=N.updates;
    long long original_test131=defjam_test_current_step();
    uint32_t original_crowd131[6],original_seeds131[32][2];int original_crowd_on131=g_crowd_gen_on;
    memcpy(original_crowd131,g_crowd_gen,sizeof original_crowd131);
    EnterCriticalSection(&g_cs);memcpy(original_seeds131,N.seeds,sizeof original_seeds131);LeaveCriticalSection(&g_cs);
    int replay_failed131=0;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    memcpy(fp, g_fp_stack, sizeof(fp));
    n = N.updates - N.snap[slot].update;
    g_last_rb_target = f;
    g_last_rb_from = N.step;
    rollback_crash_cb = rollback_crash_state;
    loads_before = rollback_dynamic_loads();
    if(rollback_load(slot)){
        rollback_drop_checkpoint131();rollback_abort131("snapshot load failed before replay");return;
    }
    rollback_check_tables("right after a load");
    N.step = f - 1;
    N.updates = N.snap[slot].update;
    defjam_test_set_step(N.snap[slot].test_step);
    memcpy(g_crowd_gen, N.snap[slot].crowd_gen, sizeof(g_crowd_gen));
    g_crowd_gen_on = N.snap[slot].crowd_gen_on;
    EnterCriticalSection(&g_cs);
    memcpy(N.seeds, N.snap[slot].seeds, sizeof(N.snap[slot].seeds));
    LeaveCriticalSection(&g_cs);
    memset(verify_pad_seen131,0,sizeof verify_pad_seen131);
    N.resim = 1;replay_audio131=1;replay_input_failed131=0;
    for (j = 0; j < n; j++) {
        if(j){snap_save();if(!N.snap[(N.step+1)%ROLLBACK_SLOTS].valid){replay_failed131=1;break;}}
        if(!android_rollback131_healthy()){replay_failed131=1;break;}
        guest_call(sub_000DD930, input, 1, &dt);
        guest_call(sub_00077040, machine, 1, &dt);
        if (g_eax & 0xFFu) {
            guest_call(sub_000BD860, em, 0, NULL);
            guest_call(sub_000BDF40, em, 0, NULL);
            guest_call(sub_0001A240, render, 1, &dt);
        }
        else {replay_failed131=1;break;}
        N.updates++;
    }
    if(N.step!=original_step131 || replay_input_failed131)replay_failed131=1;
    if(replay_failed131){
        if(rollback_restore_checkpoint131()){
            fprintf(stderr,"[ROLLBACK131] fatal: failed reexecution and unusable recovery checkpoint; stopping process\n");abort();
        }
        N.step=original_step131;N.updates=original_updates131;defjam_test_set_step(original_test131);
        memcpy(g_crowd_gen,original_crowd131,sizeof original_crowd131);g_crowd_gen_on=original_crowd_on131;
        EnterCriticalSection(&g_cs);memcpy(N.seeds,original_seeds131,sizeof original_seeds131);LeaveCriticalSection(&g_cs);
    }
    rollback_drop_checkpoint131();
    N.resim = 0;replay_audio131=0;
    g_eax = eax; g_ecx = ecx; g_edx = edx; g_ebx = ebx; g_esi = esi; g_edi = edi; g_ebp = ebp; g_esp = esp;
    memcpy(g_fp_stack, fp, sizeof(fp));
    g_fp_top = fp_top;
    g_fp_control_word = fp_cw;
    g_seh_ebp=seh131;g_fs_base=fs131;g_df=df131;g_fp_cmp=fcmp131;g_fp_cc=fcc131;
    g_xmm0=xmm131[0];g_xmm1=xmm131[1];g_xmm2=xmm131[2];g_xmm3=xmm131[3];
    g_xmm4=xmm131[4];g_xmm5=xmm131[5];g_xmm6=xmm131[6];g_xmm7=xmm131[7];
    xbox_rollback_mmx_restore131(mmx131);
    if(replay_failed131){rollback_abort131("reexecution failed; restored current state");return;}
    QueryPerformanceCounter(&t1);
    ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
    N.rb_count++;
    N.rb_steps += depth;
    if (depth > N.rb_deepest) N.rb_deepest = depth;
    if (ms > N.rb_ms_max) N.rb_ms_max = ms;
    if (ms > 30.0) {        /* a stutter: say how deep, and whether models were loaded in it */
        static unsigned told;
        if (told++ < 60)
            fprintf(stderr, "[ROLLBACK] slow: back %u step(s) in %.1f ms, %u model load(s) in it, step %u\n",
                    depth, ms, rollback_dynamic_loads() - loads_before, f);
    }
    N.rb_ms_total += ms;
    if (N.rb_count <= 10 || N.rb_count % 200 == 0)
        fprintf(stderr, "[ROLLBACK] #%u: back to step %u, %u updates again in %.2f ms (now at step %u)\n",
                N.rb_count, f, n, ms, N.step);
}

void netplay_on_update(void)
{
    uint32_t t, f = 0;
    int i;
    rollback_check_tables(N.resim ? "a re-run update starts" : "an update starts");
    if (N.resim || !N.rb_on) return;
    if(!android_rollback131_healthy()){
        rollback_abort131("tracking/allocator safety failure");return;
    }
    /* The fight is decided and every step ran on the real entries, or the match went on offline:
     * no more rollback in this match (write tracking off, no saves on the results screen). */
    if (MEM32(G_MATCH_PHASE) == 1u) N.rb_fought = 1;   /* a rematch starts with the last match's phase */
    if ((!N.lockstep && !soak_depth()) ||
        (N.lockstep && N.rb_fought && MEM32(G_MATCH_PHASE) >= 2u && N.final_step >= N.step && !N.predicting)) {
        N.rb_on = 0;
        rollback_reset();
        fprintf(stderr, "[ROLLBACK] off for the rest of this match (step %u, %u rollbacks)\n", N.step, N.rb_count);
        return;
    }
    if (N.rb_ref_due) {
        N.rb_ref_due = 0;
        if (rollback_reference()) { rollback_abort131("reference/write tracking unavailable");return; }
        N.updates = 0;
        memset(N.snap, 0, sizeof(N.snap));
    }
    rollback_discover(MEM32(G_MATCH_PHASE) == 1u ? 16 : 4);
    /* which steps ran on the real entries; the first that ran on a wrong prediction */
    if(np_load(&N.lockstep)){
    EnterCriticalSection(&g_cs);
    for (t = N.final_step + 1; t <= N.step; t++) {
        uint32_t k = t % RING;
        int all = 1, wrong = 0;
        if (t > (uint32_t)N.delay)
            for (i = 0; i < N.players; i++) {
                if (i == N.pos) continue;
                if (!(N.in_tag[i][k] == t + 1 && N.in_match[i][k] == N.match)) { all = 0; continue; }
                if (N.used_tag[i][k] == t + 1 && N.used_pred[i][k] && memcmp(N.used[i][k], N.in[i][k], ENTRY)) wrong = 1;
            }
        if (wrong) { f = t; break; }
        if (!all) break;
        N.final_step = t;
    }
    LeaveCriticalSection(&g_cs);
    }
    if (f) {rollback_to(f);if(!N.rb_on || !rollback_ready())return;}
    if (!N.lockstep && soak_depth()) {      /* tests: go back and compare */
        static unsigned soaks, same, differ,missing131;
        int continuous131=MEM32(G_MATCH_PHASE)==1u && N.step>verify_last_step131;
        if(!continuous131 || !verify_warm_base131)verify_warm_base131=N.step;
        verify_last_step131=N.step;
        long long t = defjam_test_current_step();
        int d = 2 + (int)((N.updates / 5) % (uint32_t)(soak_depth() - 1));
        if (continuous131 && N.step-verify_warm_base131>60 && t>40 && N.step>(uint32_t)d+2 && N.updates%5==0) {
            unsigned long long before[16];
            int k;
            for (k = 0; k < d; k++) before[k] = defjam_test_step_hash(t - d + k);
            rollback_to(N.step + 1 - (uint32_t)d);
            if(!N.rb_on || !rollback_ready())return; /* A failed attempt is never a matched verification. */
            soaks++;
            for (k = 0; k < d; k++) {       /* k = 0: the state the load put back */
                unsigned long long after = defjam_test_step_hash(t - d + k);
                if (!before[k] || !after){missing131++;continue;}
                if (before[k] == after) same++;
                else if (differ++ < 20)
                    fprintf(stderr, "[ROLLBACK] soak #%u: step %lld came out otherwise after going back %d "
                            "(%016llX, first %016llX)\n", soaks, t - d + k, d, after, before[k]);
            }
            if(differ){android_rollback131_poison();N.rb_on=0;rollback_reset();
                fprintf(stderr,"[ROLLBACK131] verification FAILED: replay hash differs; online rollback not validated\n");return;}
            if (soaks % 100 == 0)
                fprintf(stderr,"[ROLLBACK131-VERIFY] rollbacks=%u same=%u different=%u missing=%u status=%s; fingerprints are simulation evidence, not full gameplay certification\n",
                    soaks,same,differ,missing131,differ?"FAILED":missing131?"INCOMPLETE":"MATCHED");
        }
    }
    send_hashes(N.final_step + 1 < N.step ? N.final_step + 1 : N.step);
    snap_save();
    if(!N.snap[(N.step+1)%ROLLBACK_SLOTS].valid){
        rollback_abort131("snapshot allocation failed");return;
    }
    N.updates++;
    if (N.updates % 1800 == 0 && N.rb_count) {
        unsigned kb, changing, skipped;
        rollback_stats(&kb, &changing, &skipped);
        fprintf(stderr, "[ROLLBACK] %u rollbacks, %u steps run again, deepest %u, %.2f ms average, %.2f ms longest; "
                "save %u KB (%u heap pages change)\n", N.rb_count, N.rb_steps, N.rb_deepest,
                N.rb_ms_total / N.rb_count, N.rb_ms_max, kb, changing);
    }
}


void netplay_on_seed(uint32_t esp, int words, uint32_t which)
{
    int i, slot = -1;
    uint32_t count;
    if (!np_load(&N.lockstep) || N.seed_bypass) return;
    EnterCriticalSection(&g_cs);
    for (i = 0; i < 32; i++)
        if (N.seeds[i].which == which || !N.seeds[i].which) { slot = i; break; }
    if (slot < 0) slot = (int)(which % 32);
    N.seeds[slot].which = which;
    count = N.seeds[slot].count++;
    LeaveCriticalSection(&g_cs);
    for (i = 0; i < words; i++) {
        uint32_t v = mix32(N.session ^ mix32(which * 0x9E3779B9u + count * 0x85EBCA6Bu + (uint32_t)i));
        if (i == 1) v |= 1;     /* an additive term stays odd */
        MEM32(esp + 4 + 4 * (uint32_t)i) = v;
    }
}

#if defined(__ANDROID__)
/* Only the serialized Java worker calls command/snapshot. Suspend is nonblocking. */
extern void xbox_UsbSetPadCount(int count);
extern int usb_gamepad_configured(int pad);
int android_netplay_prepare_ports(int players){
    if(players<2||players>4)return 0;
    xbox_UsbSetPadCount(players);
    ULONGLONG until=GetTickCount64()+8000;
    while(netplay_active()&&GetTickCount64()<until){
        int ready=1;for(int i=0;i<players;i++)if(!usb_gamepad_configured(i))ready=0;
        if(ready)return 1;
        Sleep(5); /* IRQ/USB worker remains free to enumerate the remote ports. */
    }
    if(netplay_active()){
        android_netplay_abort();InterlockedExchange(&N.state,NETPLAY_FAILED);
        set_status("Online controllers are not ready. Return to the main menu and reconnect.");
    }
    return 0;
}
/* Fixed-size credentials are derived off the UI thread in Android. */
static int android_room_credential(const char *hex,uint8_t out[32]) {
    if(!hex||strlen(hex)!=64)return 0;
    for(int i=0;i<32;i++){
        int a=hex[i*2],b=hex[i*2+1];
        a=a>='0'&&a<='9'?a-'0':a>='a'&&a<='f'?a-'a'+10:-1;
        b=b>='0'&&b<='9'?b-'0':b>='a'&&b<='f'?b-'a'+10:-1;
        if(a<0||b<0)return 0;out[i]=(uint8_t)((a<<4)|b);
    }return 1;
}
int android_netplay_command(int action,const char *text,const char *extra,int value) {
    InitOnceExecuteOnce(&g_once,init_once,NULL,NULL);
    if(action==9){xbox_HostInputSetUiActive(value!=0);return 0;}
    if(action==8){netplay_stop();return 0;}
    if(action==0){
        if(!automatch_android_main_menu()) {set_status("Open Online at the game's main menu.");return -1;}
        if(!text||!*text)text=PROJECT_SERVER_NAME;
        if(strlen(text)>120||!extra||strlen(extra)>24){set_status("Invalid server or player name.");return -1;}
        xbox_UsbSetPadCount(2);
        return netplay_lobby(text,extra);
    }
    if(np_load(&N.state)!=NETPLAY_LOBBY&&np_load(&N.state)!=NETPLAY_CONNECTED){set_status("Connect to the online hub first.");return -1;}
    switch(action){
    case 1:netplay_lobby_list(255);return 0;
    case 2:if(!automatch_android_main_menu())return -1;netplay_lobby_quick(0,2);return 0;
    case 3:{
        if(!automatch_android_main_menu())return -1;
        /* 0 public; 1 own-server password room; 2 original code-only room. */
        if(value<0||value>2)return -1;
        if(value==1&&project_server_endpoint(N.server_text)){set_status("FFNY Online supports room codes, not passwords.");return -1;}
        if(value==2&&extra&&*extra)return -1;
        NetRoomSettings room={0};snprintf(room.name,sizeof room.name,"%.24s",text&&*text?text:"Android room");
        room.is_public=value?0:1;room.mode=0;room.max=2;
        const OnlineVenue *venues;int count=online_venues(&venues);
        for(int i=0;i<count&&room.pool_count<32;i++)if(venues[i].fe_id>0&&venues[i].fe_id<256)room.pool[room.pool_count++]=(uint8_t)venues[i].fe_id;
        if(!room.pool_count){set_status("Cannot read game/venue.xml from your dump.");return -1;}
        if(value==1){
            uint8_t packet[128],key[32];if(!android_room_credential(extra,key)){set_status("Private rooms require a password.");return -1;}
            int n=put_settings(packet,&room);memcpy(packet+n,key,32);SecureZeroMemory(key,sizeof key);
            request(T_CREATE_PRIVATE,packet,n+32);SecureZeroMemory(packet,sizeof packet);
        }else netplay_lobby_create(&room);
        return 0;
    }
    case 4:
        if(!automatch_android_main_menu()||strlen(text)!=6)return -1;
        for(int i=0;i<6;i++)if(!isalnum((unsigned char)text[i]))return -1;
        if(extra&&*extra){
            if(project_server_endpoint(N.server_text)){set_status("FFNY Online supports room codes, not passwords.");return -1;}
            uint8_t packet[38],key[32];if(!android_room_credential(extra,key))return -1;
            for(int i=0;i<6;i++)packet[i]=(uint8_t)toupper((unsigned char)text[i]);
            memcpy(packet+6,key,32);SecureZeroMemory(key,sizeof key);request(T_JOIN_PRIVATE,packet,38);SecureZeroMemory(packet,sizeof packet);
        }else netplay_lobby_join(text);
        return 0;
    case 5:{
        const OnlineFighter *roster;int count=online_fighters(&roster),valid=0;
        for(int i=0;i<count;i++)if(roster[i].id==value)valid=1;
        if(!valid||!N.roomv.in_room||N.roomv.phase!=0)return -1;
        xbox_UsbSetPadCount(N.roomv.settings.max);
        int ready=1;
        for(int i=0;i<N.roomv.player_count;i++)if(N.roomv.players[i].slot==N.roomv.you&&N.roomv.players[i].ready)ready=0;
        netplay_lobby_ready(ready,value);return 0;
    }
    case 6:if(value<0||value>=N.roomv.candidate_count||N.roomv.phase!=1)return -1;netplay_lobby_vote(value);return 0;
    case 7:automatch_android_cancel();netplay_lobby_leave();return 0;
    default:return -1;
    }
}
void android_netplay_abort(void){
    android_netplay_menu_cancel();
    InterlockedIncrement(&s_mobile_cancel);
    automatch_android_cancel();
    /* A pause never waits for DNS, the network worker or a guest critical section. */
    InterlockedExchange(&N.lockstep,0);InterlockedExchange(&N.stop,1);
    if(N.arrived)SetEvent(N.arrived);
    xbox_HostInputSetNetplay(0,0,0,NULL);xbox_HostInputSetNetplayFight(0);
    xbox_HostInputSetUiActive(0);
}
static int json_add(char *out,size_t size,size_t *at,const char *format,...){
    if(*at>=size)return 0;va_list args;va_start(args,format);
    int n=vsnprintf(out+*at,size-*at,format,args);va_end(args);
    if(n<0||(size_t)n>=size-*at){*at=size;return 0;}*at+=(size_t)n;return 1;
}
static void json_string(char *out,size_t size,size_t *at,const char *text){
    json_add(out,size,at,"\"");
    for(const unsigned char *p=(const unsigned char*)text;*p;p++){
        if(*p=='"'||*p=='\\')json_add(out,size,at,"\\%c",*p);
        else if(*p<32||*p>=127)json_add(out,size,at,"\\u%04x",*p);
        else json_add(out,size,at,"%c",*p);
    }json_add(out,size,at,"\"");
}
int android_netplay_snapshot(char *out,size_t size){
    if(!out||size<4)return 0;InitOnceExecuteOnce(&g_once,init_once,NULL,NULL);
    const OnlineFighter *fighters=NULL;int fighter_count=0;
    if(automatch_android_main_menu()||np_load(&N.state)!=NETPLAY_OFF)fighter_count=online_fighters(&fighters);
    size_t at=0;char status[256];netplay_status(status,sizeof status);
    EnterCriticalSection(&g_cs);
    json_add(out,size,&at,"{\"state\":%d,\"menu\":%d,\"desync\":%d,\"step\":%u,\"status\":",(int)np_load(&N.state),automatch_android_main_menu(),N.desync,N.step);
    json_string(out,size,&at,status);json_add(out,size,&at,",\"room\":{\"code\":");
    json_string(out,size,&at,N.roomv.code);json_add(out,size,&at,",\"in_room\":%d,\"phase\":%d,\"you\":%d,\"public\":%d,\"players\":[",N.roomv.in_room,N.roomv.phase,N.roomv.you,N.roomv.settings.is_public);
    for(int i=0;i<N.roomv.player_count;i++){
        if(i)json_add(out,size,&at,",");json_add(out,size,&at,"{\"name\":");json_string(out,size,&at,N.roomv.players[i].name);
        json_add(out,size,&at,",\"ready\":%d,\"slot\":%d}",N.roomv.players[i].ready,N.roomv.players[i].slot);
    }
    json_add(out,size,&at,"],\"candidates\":[");
    for(int i=0;i<N.roomv.candidate_count;i++){
        if(i)json_add(out,size,&at,",");json_add(out,size,&at,"{\"name\":");json_string(out,size,&at,online_venue_name(N.roomv.candidates[i]));
        json_add(out,size,&at,",\"votes\":%d}",N.roomv.votes[i]);
    }
    json_add(out,size,&at,"]},\"rooms\":[");
    for(int i=0;i<N.room_count;i++){
        if(i)json_add(out,size,&at,",");json_add(out,size,&at,"{\"name\":");json_string(out,size,&at,N.rooms[i].name);
        json_add(out,size,&at,",\"code\":");json_string(out,size,&at,N.rooms[i].code);
        json_add(out,size,&at,",\"players\":%d,\"max\":%d,\"phase\":%d}",N.rooms[i].players,N.rooms[i].max,N.rooms[i].phase);
    }
    json_add(out,size,&at,"],\"fighters\":[");
    for(int i=0;i<fighter_count;i++){
        if(i)json_add(out,size,&at,",");json_add(out,size,&at,"{\"id\":%d,\"name\":",fighters[i].id);json_string(out,size,&at,fighters[i].name);json_add(out,size,&at,"}");
    }
    json_add(out,size,&at,"],\"error\":");json_string(out,size,&at,N.roomv.error);json_add(out,size,&at,"}");
    LeaveCriticalSection(&g_cs);
    if(at>=size){snprintf(out,size,"{}");return 0;}return 1;
}
#endif

#endif


#endif /* Android integration only; desktop sources unchanged */
