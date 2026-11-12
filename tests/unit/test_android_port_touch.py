"""Exercise the actual SDL-to-ImGui touch adapter with the vendored Dear ImGui."""
from pathlib import Path
import shutil
import subprocess
import pytest
ROOT=Path(__file__).resolve().parents[2]


def test_touch_adapter_with_real_imgui(tmp_path):
    compiler=shutil.which('c++')
    if not compiler:pytest.skip('C++ compiler required')
    source=(ROOT/'src/hooks/pc_ui.cpp').read_text()
    keys=source[source.index('static ImGuiKey imgui_key('):source.index('static float g_extra_scale_set')]
    begin=source.index('bool ui_sdl_event(')
    adapter=source[begin:source.index('\nvoid ui_sdl_frame(',begin)]
    code=r'''
#include <SDL3/SDL_events.h>
#include "imgui.h"
#include "imgui_internal.h"
#include <cassert>
''' + keys + '\n' + adapter + r'''
int main() {
 ImGui::CreateContext();
 ImGuiIO &io=ImGui::GetIO();io.DisplaySize=ImVec2(1000,500);
 SDL_Event e={};e.type=SDL_EVENT_FINGER_DOWN;e.tfinger.x=.25f;e.tfinger.y=.5f;
 assert(ui_sdl_event(&e,3,4));
 bool position=false,down=false;
 for(const auto &q:ImGui::GetCurrentContext()->InputEventsQueue) {
  if(q.Type==ImGuiInputEventType_MousePos)position=q.MousePos.PosX==250&&q.MousePos.PosY==250;
  if(q.Type==ImGuiInputEventType_MouseButton)down=q.MouseButton.Button==0&&q.MouseButton.Down;
 }
 assert(position&&down);assert(io.ConfigFlags&ImGuiConfigFlags_IsTouchScreen);
 e.type=SDL_EVENT_FINGER_UP;assert(ui_sdl_event(&e,3,4));
 bool up=false;for(const auto &q:ImGui::GetCurrentContext()->InputEventsQueue)
  if(q.Type==ImGuiInputEventType_MouseButton&&!q.MouseButton.Down)up=true;
 assert(up);
 e.type=SDL_EVENT_FINGER_DOWN;ui_sdl_event(&e,1,1);
 e.type=SDL_EVENT_FINGER_CANCELED;ui_sdl_event(&e,1,1);
 const auto &last=ImGui::GetCurrentContext()->InputEventsQueue.back();
 assert(last.Type==ImGuiInputEventType_MouseButton&&!last.MouseButton.Down);
 e.type=SDL_EVENT_MOUSE_MOTION;e.motion.x=10;e.motion.y=20;ui_sdl_event(&e,2,3);
 const auto &mouse=ImGui::GetCurrentContext()->InputEventsQueue.back();
 assert(mouse.Type==ImGuiInputEventType_MousePos&&mouse.MousePos.PosX==20&&mouse.MousePos.PosY==60);
 ImGui::DestroyContext();
}
'''
    src=tmp_path/'check.cpp';src.write_text(code)
    imgui=ROOT/'android/vendor/imgui';binary=tmp_path/'check'
    subprocess.run([compiler,'-std=c++17','-O0','-D__ANDROID__','-I'+str(imgui),'-I'+str(ROOT/'android/vendor/SDL/include'),str(src),
                    *[str(imgui/n) for n in ['imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp']],'-o',str(binary)],check=True,capture_output=True,text=True)
    subprocess.run([str(binary)],check=True,capture_output=True,text=True)
