/* The online screens inside the game: the ONLINE entry of the main menu (slot 6, filled by
 * scripts/apt-patch.py) and the screens it opens (hub, room list, room, fighter choice, venue
 * vote), drawn by the port in the game's look with the art scripts/online-art.py takes from the
 * player's own dump (<data>/online-art). See docs/research/frontend-menu-integration.md. */
#ifndef DEFJAM_ONLINE_SCREEN_H
#define DEFJAM_ONLINE_SCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

/* From the FuncCall hook (via automatch_on_funccall): screens, Online.Focus(n), Online.Open(). */
void online_menu_on_funccall(const char *text);

#ifdef __cplusplus
}

#include "imgui.h"

namespace online_screen {

/* Load fonts and textures; call with the overlay's device before ImGui::NewFrame. */
void prepare(void *d3d11_device);
#if defined(__ANDROID__)
/* Existing title fonts; null means the renderer should use its readable fallback. */
ImFont *menu_font145(int role); /* 0 stencil title,1 menu label,2 body */
#endif

/* The main menu is up: the ONLINE poster and, while slot 6 has the cursor, its door. */
bool decor_wanted();
void draw_decor(ImDrawList *dl, ImVec2 display);

/* The player chose ONLINE (once per choice). */
bool take_open_request();

/* The online screens; returns false when the player leaves them (back to the main menu). */
bool draw(ImVec2 display);

/* The last false from draw() was a match starting, not a press: nothing to keep from the game. */
bool closed_for_match();
#if defined(__ANDROID__)
bool take_close_request();
/* Same editable Android menu pad; called under the overlay's input lock. */
bool touch_button(int button, bool down);
void clear_touch_buttons();
void update_textures();
void release_textures();
#endif

}
#endif

#endif

