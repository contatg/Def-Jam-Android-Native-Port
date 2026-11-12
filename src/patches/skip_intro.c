/* Startup only: select Press Start instead of legal1 on the first front-end load.
 *
 * USA guest 0x0006BC90 selects the screen in frontend+0x9C using the name table
 * at 0x002DFE08 (0 = legal1, 1 = preStart). Its generated body still initializes
 * the loader and selects the screen normally. Skipping legal1 also bypasses
 * bootLoad, whose Game.StartMainMenu(0) starts the EA/THX/intro movie sequence.
 *
 * legal1 normally calls Controller.ControllerBootup before leaving. Keep its
 * native controller bindings and rumble reset. PreStart still performs its own
 * storage check after Start is pressed.
 *
 * The audio engine is initialized twice on a normal boot. The state-0 entry
 * (0x00078090) runs AudioInit(0) first, which only marks it ready. When the
 * startup movies end, the game re-enters state 0 and runs AudioInit(1), which
 * loads the audio configuration and music tables and is what enables the menu
 * sound effects (Press Start, menu and sub-menu transitions). Skipping the
 * movies skips that second entry, so replay it here.
 * No movie decoder or later screen selection is changed.
 */
#include <stdint.h>
#include <stdio.h>
#include "recomp/gen/recomp_types.h"
#include "recomp_settings.h"

extern void sub_00055640(void);  /* Controller.ControllerBootup, ret 8; arguments unused */
extern void sub_0003DE20(void);  /* AudioInit(full), cdecl: shuts a prior init down first, then loads the config */

void defjam_skip_intro_enter(void)
{
    static int selected;
    uint32_t frontend = g_ecx;
    uint32_t eax, ecx, edx;

    if (selected)
        return;
    selected = 1;
    /* Read the effective file/environment setting after the launcher. Subsequent
     * overlay edits affect the next process, never an in-progress Story/movie. */
    if (!recomp_settings_get("debug", "skip_intro", 0))
        return;
    if (MEM32(frontend + 0x9C) != 0) {
        fprintf(stderr, "[STARTUP] skip_intro: first screen was not legal1; leaving it unchanged\n");
        return;
    }

    eax = g_eax; ecx = g_ecx; edx = g_edx;
    g_esp -= 12;
    MEM32(g_esp) = 0;       /* synthetic return address and the two unused script arguments */
    MEM32(g_esp + 4) = 0;
    MEM32(g_esp + 8) = 0;
    sub_00055640();
    g_eax = eax; g_ecx = ecx; g_edx = edx;

    /* Normal boot reaches the full audio init after the movies end. Same stack
     * shape as the game's own call: return word, then the single cdecl argument. */
    g_esp -= 8;
    MEM32(g_esp) = 0;
    MEM32(g_esp + 4) = 1;
    sub_0003DE20();
    g_esp += 4;
    g_eax = eax; g_ecx = ecx; g_edx = edx;

    MEM32(frontend + 0x9C) = 1;
    fprintf(stderr, "[STARTUP] skip_intro: controller boot initialized; legal1 -> preStart "
                    "(startup movies skipped)\n");
}
