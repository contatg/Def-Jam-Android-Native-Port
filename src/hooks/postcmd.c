/*
 * Post a synthetic command to the front end, to find out what the front end
 * would have done if anything had ever posted one.
 *
 * The title's front-end loop at guest 0x0019AB60 is healthy and idle. Its two
 * early gates are open (0x3B808C and 0x3B8090 both read zero), both of its
 * manager objects hold valid pointers, and it is parked on its third test:
 *
 *     if (MEM32(0x2EB73C) == 0) idle
 *
 * Nothing in the lifted image ever writes that pointer to a non-null value, so
 * the title sits there for ever and never loads its first screen. Reading
 * outwards from the loop has not found the poster, and there is a second
 * question worth answering on its own: whether the dispatch behind that
 * pointer still works at all. If the loop is fed a command and does nothing,
 * the damage is deeper than a missing post; if it is fed a command and starts
 * loading a screen, then the machinery is intact and only the trigger is
 * missing -- which is a much smaller thing to go looking for.
 *
 * The loop reads the command id from MEM32(ptr + 8), subtracts one, rejects
 * anything above 0x1E and otherwise dispatches through a 31-entry jump table.
 * So a command is a small structure whose only field this code needs to know
 * about is the id at +8. The rest is left zero, which is a real limitation:
 * a handler that dereferences another field will be reading zeros, and that
 * is a plausible way to crash. It is behind an environment variable and it is
 * a diagnostic, not a fix.
 *
 *     $env:RECOMP_POST_CMD="7"        post command id 7, once
 *     $env:RECOMP_POST_CMD="sweep"    post 1..31 in turn, pausing between
 *     $env:RECOMP_POST_CMD_MS="25000" how long to wait first (default 25s)
 *     $env:RECOMP_POST_CMD_STEP="2500" pause between ids in a sweep
 *
 * A sweep is the useful mode: a command that loads a screen has to open a
 * file to do it, and file opens are already logged, so the id that means
 * "load a screen" announces itself in the log next to the [POST] line that
 * caused it.
 *
 * The structure is placed high in the contiguous window, well above anything
 * the title's allocator hands out -- it starts at 0x80001000 and the addresses
 * seen in practice are five digits, so the top of the window is not in use.
 * The pointer is only written when it is already null, so a real command, if
 * one ever appears, is never overwritten.
 */

#include "host.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

/* The front end's command pointer, and where to build a command. */
#define FE_COMMAND_PTR   0x002EB73Cu
#define FE_COMMAND_SCRAP 0x83F00000u
#define FE_RESULT_PTR    0x003B8090u
#define FE_COMMAND_MAX   31

static int   s_id;                  /* 0 means sweep 1..FE_COMMAND_MAX */
static DWORD s_delay = 25000;
static DWORD s_step  = 2500;

static volatile uint32_t *guest32(uint32_t va)
{
    return (volatile uint32_t *)((uintptr_t)va + (uintptr_t)g_xbox_mem_offset);
}

static void post_one(int id)
{
    volatile uint32_t *slot = guest32(FE_COMMAND_PTR);
    int i;

    /* A handler answers by leaving a result pointer at 0x3B8090, and the
     * loop refuses to take another command while one is outstanding. On a
     * real run whoever posted the command collects that result; here nobody
     * did, so the first injected command was also the last and the rest of a
     * sweep reported nothing but "already holds". Collecting it is what the
     * absent requester would have done. */
    {
        volatile uint32_t *result = guest32(FE_RESULT_PTR);
        if (*result != 0) {
            fprintf(stderr, "  [POST] collecting result 0x%08X from 0x%08X\n",
                    *result, FE_RESULT_PTR);
            *result = 0;
        }
    }

    /* Never displace a real command. If the title has started posting for
     * itself then this has nothing to add and should keep out of the way. */
    if (*slot != 0) {
        fprintf(stderr, "  [POST] 0x%08X already holds 0x%08X; leaving it\n",
                FE_COMMAND_PTR, *slot);
        fflush(stderr);
        return;
    }

    for (i = 0; i < 16; i++)
        *guest32(FE_COMMAND_SCRAP + (uint32_t)(i * 4)) = 0;
    *guest32(FE_COMMAND_SCRAP + 8) = (uint32_t)id;

    fprintf(stderr, "  [POST] command id %d at guest 0x%08X\n",
            id, FE_COMMAND_SCRAP);
    fflush(stderr);

    *slot = FE_COMMAND_SCRAP;
}

static DWORD WINAPI post_thread(LPVOID unused)
{
    (void)unused;
    Sleep(s_delay);

    if (s_id) {
        post_one(s_id);
        return 0;
    }

    {
        int id;
        for (id = 1; id <= FE_COMMAND_MAX; id++) {
            post_one(id);
            Sleep(s_step);
        }
        fprintf(stderr, "  [POST] sweep finished\n");
        fflush(stderr);
    }
    return 0;
}

int postcmd_init(void)
{
    const char *want = getenv("RECOMP_POST_CMD");
    const char *ms   = getenv("RECOMP_POST_CMD_MS");
    const char *step = getenv("RECOMP_POST_CMD_STEP");

    if (!want || !*want)
        return 0;

    if (_stricmp(want, "sweep") != 0) {
        s_id = (int)strtol(want, NULL, 0);
        if (s_id < 1 || s_id > FE_COMMAND_MAX) {
            fprintf(stderr, "  [POST] id %d is outside 1..%d\n",
                    s_id, FE_COMMAND_MAX);
            return 0;
        }
    }
    if (ms && *ms)
        s_delay = (DWORD)strtoul(ms, NULL, 0);
    if (step && *step)
        s_step = (DWORD)strtoul(step, NULL, 0);

    if (!CreateThread(NULL, 0, post_thread, NULL, 0, NULL)) {
        fprintf(stderr, "  [POST] could not start (error %lu)\n",
                GetLastError());
        return 0;
    }

    if (s_id)
        fprintf(stderr, "  POST: command id %d after %lu ms\n",
                s_id, (unsigned long)s_delay);
    else
        fprintf(stderr, "  POST: sweeping ids 1..%d after %lu ms, %lu ms apart\n",
                FE_COMMAND_MAX, (unsigned long)s_delay,
                (unsigned long)s_step);
    fflush(stderr);
    return 1;
}
