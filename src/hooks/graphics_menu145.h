#ifndef DEFJAM_GRAPHICS_MENU145_H
#define DEFJAM_GRAPHICS_MENU145_H
#ifdef __cplusplus
extern "C" {
#endif
void graphics145_ready(void);
void graphics145_request(void);
void graphics147_request_game(void);
void graphics148_closed(void);
void graphics148_editor(void);
int graphics145_take_request(void);
/* UI bridge: bit0 HUD visible, bits1..2 position, bit3 classic style, bit8 requests existing touch editor. */
int graphics145_android_state(int legacy_hud,int language);
#ifdef __cplusplus
}
#include "imgui.h"
namespace graphics145 {
void open();
void open_game147();
bool draw(ImVec2 display); /* false closes this port menu; online session is retained */
}
#endif
#endif
