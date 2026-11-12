/*
 * kernel_script_anchor.c -- clocks for unattended runs that start at a point
 * in the title rather than at process start. Shared by every host: the path
 * translators report each open to xbox_AnchorNoteOpen().
 */
#include "kernel.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A clock for unattended runs that starts at a point in the title rather than
 * at process start. The title reaches its title screen anywhere from 100 to
 * 190 s into a run, depending on how start-up goes, so a button script in
 * absolute seconds lands on different screens each time. Anchored to the Nth
 * open of a file -- Def Jam loads screens\feflow.xml once at boot and again
 * as the title screen comes up, so "feflow.xml#2" -- the same script reaches
 * the same screens.
 *
 *     RECOMP_SCRIPT_ANCHOR=feflow.xml#2
 */
static ULONGLONG s_anchor_ft;             /* FILETIME the anchor was reached */
static int s_anchor_state = -1;           /* -1 unread, 0 none, 1 waiting, 2 reached */
static char s_anchor_sub[128];
static int s_anchor_want = 1, s_anchor_seen;

#if !defined(_WIN32)
/* No process creation time to ask for here: the unanchored clock starts at
 * the first question, which the pad model asks during start-up. */
static ULONGLONG s_first_use_ft;
static ULONGLONG now_ft(void);
#endif

static void anchor_init(void)
{
#if !defined(_WIN32)
    if (!s_first_use_ft)
        s_first_use_ft = now_ft();
#endif
    const char *s = getenv("RECOMP_SCRIPT_ANCHOR");
    const char *hash;
    size_t i, n;

    s_anchor_state = 0;
    if (!s || !*s)
        return;
    hash = strchr(s, '#');
    n = hash ? (size_t)(hash - s) : strlen(s);
    if (n >= sizeof(s_anchor_sub))
        n = sizeof(s_anchor_sub) - 1;
    for (i = 0; i < n; i++)
        s_anchor_sub[i] = (char)tolower((unsigned char)s[i]);
    s_anchor_sub[n] = 0;
    if (hash && atoi(hash + 1) > 0)
        s_anchor_want = atoi(hash + 1);
    s_anchor_state = 1;
}

/* More anchors, for a script that crosses several screens whose timing varies:
 * xbox_FileOpenSeconds("main.mus#1") is the seconds since the first open of a
 * path containing "main.mus", negative before it. A spec is watched from its
 * first query, so ask before the file can open (the pad and the translator do,
 * from start-up). */
/* A screen-by-screen chain uses one per screen, the captures more. */
#define OPEN_WATCH_MAX 32
static struct { char sub[64]; int want, seen; volatile ULONGLONG ft; } s_open_watch[OPEN_WATCH_MAX];
static volatile LONG s_open_watches;
static CRITICAL_SECTION s_open_watch_lock;
static INIT_ONCE s_open_watch_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK open_watch_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&s_open_watch_lock);
    return TRUE;
}

static ULONGLONG now_ft(void)
{
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    return ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
}

double xbox_FileOpenSeconds(const char *spec)
{
    char sub[64];
    const char *hash = strchr(spec, '#');
    size_t i, n = hash ? (size_t)(hash - spec) : strlen(spec);
    int want = (hash && atoi(hash + 1) > 0) ? atoi(hash + 1) : 1;
    LONG k, count;
    ULONGLONG ft = 0;

    if (n >= sizeof sub)
        n = sizeof sub - 1;
    for (i = 0; i < n; i++)
        sub[i] = (char)tolower((unsigned char)spec[i]);
    sub[n] = 0;
    InitOnceExecuteOnce(&s_open_watch_once, open_watch_init, NULL, NULL);
    EnterCriticalSection(&s_open_watch_lock);
    count = s_open_watches;
    for (k = 0; k < count; k++)
        if (s_open_watch[k].want == want && !strcmp(s_open_watch[k].sub, sub))
            break;
    if (k == count && count < OPEN_WATCH_MAX) {
        memcpy(s_open_watch[k].sub, sub, n + 1);
        s_open_watch[k].want = want;
        s_open_watches = count + 1;
    } else if (k == count) {
        static int warned;
        if (!warned++)
            fprintf(stderr, "  [SCRIPT] more than %d anchors: \"%s\" is never reached\n",
                    OPEN_WATCH_MAX, sub);
    }
    if (k < OPEN_WATCH_MAX)
        ft = s_open_watch[k].ft;
    LeaveCriticalSection(&s_open_watch_lock);
    return ft ? (double)(now_ft() - ft) / 1e7 : -1.0;
}

void xbox_AnchorNoteOpen(const char *xbox_path)
{
    char low[512];
    size_t i;
    LONG k;

    for (i = 0; xbox_path[i] && i < sizeof(low) - 1; i++)
        low[i] = (char)tolower((unsigned char)xbox_path[i]);
    low[i] = 0;
    if (s_open_watches) {
        EnterCriticalSection(&s_open_watch_lock);
        for (k = 0; k < s_open_watches; k++) {
            if (s_open_watch[k].ft || !strstr(low, s_open_watch[k].sub)
                    || ++s_open_watch[k].seen < s_open_watch[k].want)
                continue;
            s_open_watch[k].ft = now_ft();
            fprintf(stderr, "  [SCRIPT] anchor \"%s\" #%d reached\n",
                    s_open_watch[k].sub, s_open_watch[k].want);
        }
        LeaveCriticalSection(&s_open_watch_lock);
    }
    if (s_anchor_state < 0)
        anchor_init();
    if (s_anchor_state != 1)
        return;
    if (!strstr(low, s_anchor_sub) || ++s_anchor_seen < s_anchor_want)
        return;
    s_anchor_ft = now_ft();
    s_anchor_state = 2;
    fprintf(stderr, "  [SCRIPT] anchor \"%s\" #%d reached: script time starts now\n",
            s_anchor_sub, s_anchor_want);
}

/* Anything else worth anchoring a script to -- a title's own events, such as
 * the screen its front end asks for -- is offered here, and matches the same
 * specs as a file path does. */
void xbox_NoteAnchorEvent(const char *text)
{
    if (text)
        xbox_AnchorNoteOpen(text);
}

double xbox_ScriptSeconds(void)
{
#if defined(_WIN32)
    FILETIME c, e, k, u, now;
#else
    FILETIME now;
#endif
    ULONGLONG from, to;

    if (s_anchor_state < 0)
        anchor_init();
    if (s_anchor_state == 1)
        return -1.0;
    GetSystemTimeAsFileTime(&now);
    to = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    if (s_anchor_state == 2) {
        from = s_anchor_ft;
    } else {
#if defined(_WIN32)
        if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
            return 0.0;
        from = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
#else
        from = s_first_use_ft;
#endif
    }
    return (double)(to - from) / 1e7;
}
