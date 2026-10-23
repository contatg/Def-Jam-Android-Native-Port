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
    if (!strncmp(lower, "game.entertutorialmode(", 23)) return 1;
    return -1;
}

static inline int defjam_input_mode(int module, int menu_hint, int overlay)
{
    if (overlay) return 2;
    if (menu_hint) return 0;
    return module == 2 || module == 3 || module == 6 ? 1 : 0;
}
#endif
