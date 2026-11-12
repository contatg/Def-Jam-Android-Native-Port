#ifndef DEFJAM_ANDROID_INPUT_MODE_H
#define DEFJAM_ANDROID_INPUT_MODE_H
#include <ctype.h>
#include <stddef.h>
#include <string.h>

/* Front-end pages can live inside the Story/game run module. Module identity
 * alone therefore cannot select the fighting buttons. These commands are
 * observed in the actual style-selection -> tutorial flow. */
static inline int defjam_frontend_input_hint(const char *text)
{
    char lower[192];
    size_t n = 0;
    if (!text) return -1;
    while (text[n] && n + 1 < sizeof lower) {
        lower[n] = (char)tolower((unsigned char)text[n]);
        ++n;
    }
    lower[n] = 0;
    if (!strncmp(lower, "game.showfightingmovie(", 23)
            || !strncmp(lower, "game.choosefightingstyle(", 25)
            || strstr(lower, "getscreeninfo(story/chardec.")
            || strstr(lower, "getscreeninfo(story/sketch.")) return 0;
    if (!strncmp(lower, "game.entertutorialmode(", 23)
            || !strncmp(lower, "game.startgame(", 15)
            || !strncmp(lower, "crib.fightmatch(", 16)) return 1;
    if (!strncmp(lower, "game.getmatchsummary(", 21)) return 0;
    return -1;
}

static inline int defjam_input_mode(int module, int menu_hint, int overlay)
{
    if (overlay) return 2;
    if (menu_hint) return 0;
    return module == 2 || module == 3 || module == 6 ? 1 : 0;
}
/* CPausedMenu+24 is the renderer's state: 0 is closed, nonzero draws a page. */
static inline int defjam_pause_menu_active(unsigned state) { return state != 0; }
static inline int defjam_input_mode_with_pause(int module, int menu_hint, int overlay, int paused)
{
    if (overlay) return 2;
    if (paused) return 0;
    return defjam_input_mode(module, menu_hint, 0);
}
/* The live fight-step hook is stronger evidence than a retained frontend hint
 * or a pause animation state. It expires so real pause pages still get A/B. */
static inline int defjam_input_mode_with_fight_step(int module, int menu_hint, int overlay,
                                                   int paused, int recent_fight_step)
{
    if (overlay) return 2;
    if (recent_fight_step) return 1;
    return defjam_input_mode_with_pause(module, menu_hint, 0, paused);
}
#endif
