// The launcher off Windows: the settings screen in a window of its own, before the game starts.
//
// The same screen as the Windows launcher (pc_ui.cpp), in an SDL window with SDL's renderer (Metal
// on a Mac) in place of Direct3D 11. The game's own window and Vulkan device do not exist yet, and
// on macOS a window's events can only be read on the process's first thread, so this runs there:
// pc_launcher_run hands the work to that thread (host_posix_call_on_main) and waits. The thread's
// ordinary event loop is not running meanwhile, so this one reads the events itself. The pads come
// from the host input layer, which pc_settings.c starts before this runs.
//
// Two test hooks, both off unless named, because the window cannot be looked at on a machine with
// nobody at it (a locked screen, a remote run):
//   RECOMP_LAUNCHER_SCRIPT   "<ms>:move:<x>:<y>;<ms>:click:<x>:<y>;<ms>:shot:<file.bmp>;<ms>:play;<ms>:quit", times
//                            from the window's first frame, in window points
//   Named controls: "<ms>:ui:<id>" clicks; "<ms>:hover:<id>" hovers, revealing scrolled controls.
//   RECOMP_WINDOW_SHOT       a file to write the first full frame to, as a BMP

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_impl_sdlrenderer3.h"

extern "C" {
#include "recomp_settings.h"
#include "input_host.h"
}
#include "pc_input.h"
#include "pc_settings.h"
#include "pc_ui.h"
#include "pc_ui_actions.h"
#include "pc_ui_internal.h"

extern "C" void host_posix_call_on_main(void (*fn)(void *), void *arg);
extern "C" int host_posix_have_video(void);

namespace {

struct ScriptStep {
    unsigned long long at_ms;
    enum Kind
    {
        CLICK,
        TARGET,
        HOVER,
        RESIZE,
        MOVE,
        SHOT,
        PLAY,
        QUIT
    } kind;
    float x, y;
    std::string file;
    bool done;
    unsigned attempts = 0;
};

std::vector<ScriptStep> parse_script(const char *text)
{
    std::vector<ScriptStep> steps;
    std::string all = text ? text : "";
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(';', pos);
        std::string item = all.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        pos = end == std::string::npos ? all.size() : end + 1;
        ScriptStep st = {};
        char kind[16] = {}, file[512] = {};
        unsigned long long ms = 0;
        float x = 0, y = 0;
        if (sscanf(item.c_str(), "%llu:%15[a-z]:%f:%f", &ms, kind, &x, &y) >= 2) {
            st.at_ms = ms;
            st.x = x;
            st.y = y;
            if ((!strcmp(kind, "ui") || !strcmp(kind, "hover")) &&
                sscanf(item.c_str(), "%llu:%*[^:]:%511[^;]", &ms, file) == 2)
            {
                st.kind = !strcmp(kind, "hover") ? ScriptStep::HOVER : ScriptStep::TARGET;
                st.file = file;
            }
            else if (!strcmp(kind, "resize"))
                st.kind = ScriptStep::RESIZE;
            else if (!strcmp(kind, "click"))
                st.kind = ScriptStep::CLICK;
            else if (!strcmp(kind, "move")) st.kind = ScriptStep::MOVE;
            else if (!strcmp(kind, "play")) st.kind = ScriptStep::PLAY;
            else if (!strcmp(kind, "quit")) st.kind = ScriptStep::QUIT;
            else if (!strcmp(kind, "shot") && sscanf(item.c_str(), "%llu:shot:%511[^;]", &ms, file) == 2) {
                st.kind = ScriptStep::SHOT;
                st.file = file;
            } else continue;
            steps.push_back(st);
        }
    }
    return steps;
}

void save_frame(SDL_Renderer *renderer, const char *path)
{
    SDL_Surface *pixels = SDL_RenderReadPixels(renderer, nullptr);
    if (!pixels) {
        fprintf(stderr, "[UI] launcher: cannot read the frame: %s\n", SDL_GetError());
        return;
    }
    if (SDL_SaveBMP(pixels, path))
        fprintf(stderr, "[UI] launcher: frame written to %s (%dx%d)\n", path, pixels->w, pixels->h);
    else
        fprintf(stderr, "[UI] launcher: cannot write %s: %s\n", path, SDL_GetError());
    SDL_DestroySurface(pixels);
}

void launcher_main(void *arg)
{
    int *result = (int *)arg;
    *result = 1;                                        // no window: do not hold the game back

    SDL_Window *window = SDL_CreateWindow("Def Jam: Fight for NY - Launcher", 1100, 720,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        fprintf(stderr, "[UI] launcher: no window (%s); starting the game\n", SDL_GetError());
        return;
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        fprintf(stderr, "[UI] launcher: no renderer (%s); starting the game\n", SDL_GetError());
        SDL_DestroyWindow(window);
        return;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowMinimumSize(window, 640, 480);

    ui_lock();
    ui_context_begin(window);
    ImGui_ImplSDLRenderer3_Init(renderer);
    ui_unlock();
    SDL_StartTextInput(window);
    SDL_RaiseWindow(window);
    fprintf(stderr, "[UI] launcher shown\n");
    fflush(stderr);

    const char *shot_first = getenv("RECOMP_WINDOW_SHOT");
    std::vector<ScriptStep> script = parse_script(getenv("RECOMP_LAUNCHER_SCRIPT"));
    unsigned long long start = SDL_GetTicks();
    unsigned frames = 0;
    bool open_setup = false;
    bool done = false;
    std::string shot_file;
    *result = 1;
    while (!done) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                done = true;
                *result = 0;
            } else if (e.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                pc_input_set_focus(1);
            } else if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                pc_input_set_focus(0);
            }
            ui_lock();
            if (!ui_capture_sdl_event(&e)) ui_sdl_event(&e, 1.0f, 1.0f);
            ui_unlock();
        }
        if (done) break;
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) { SDL_Delay(50); continue; }

        // Scripted input: a click is a pointer move, a press a moment later and a release a tenth of a
        // second after that, as a mouse makes it; the screen reacts to the pointer arriving first.
        unsigned long long now = SDL_GetTicks() - start;
        static float click_x, click_y;
        static unsigned long long press_at, release_at;
        if (press_at && now >= press_at) {
            SDL_Event down;
            SDL_zero(down);
            down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            down.button.button = SDL_BUTTON_LEFT;
            down.button.x = click_x; down.button.y = click_y;
            ui_lock();
            if (!ui_capture_sdl_event(&down)) ui_sdl_event(&down, 1.0f, 1.0f);
            ui_unlock();
            press_at = 0;
        }
        if (release_at && now >= release_at) {
            SDL_Event up;
            SDL_zero(up);
            up.type = SDL_EVENT_MOUSE_BUTTON_UP;
            up.button.button = SDL_BUTTON_LEFT;
            up.button.x = click_x; up.button.y = click_y;
            ui_lock(); ui_sdl_event(&up, 1.0f, 1.0f); ui_unlock();
            release_at = 0;
        }
        for (ScriptStep &st : script) {
            if (st.done || now < st.at_ms) continue;
            if (st.kind == ScriptStep::SHOT && frames < 3) continue;       // after the screen has been drawn
            st.done = true;
            if (st.kind == ScriptStep::TARGET || st.kind == ScriptStep::HOVER)
            {
                ui_lock();
                bool found = ui_test_target(st.file.c_str(), &st.x, &st.y);
                ui_unlock();
                if (!found)
                {
                    if (++st.attempts < 12)
                    {
                        st.done = false;
                        st.at_ms = now + 200;
                    }
                    else
                        fprintf(stderr, "[UI] script target missing: %s\n", st.file.c_str());
                    continue;
                }
            }
            if (st.kind == ScriptStep::RESIZE)
                SDL_SetWindowSize(window, (int)st.x, (int)st.y);
            else if (st.kind == ScriptStep::CLICK || st.kind == ScriptStep::TARGET)
            {
                SDL_Event m;
                SDL_zero(m);
                m.type = SDL_EVENT_MOUSE_MOTION;
                m.motion.x = st.x;
                m.motion.y = st.y;
                ui_lock();
                ui_sdl_event(&m, 1.0f, 1.0f);
                ui_unlock();
                click_x = st.x;
                click_y = st.y;
                press_at = now + 60;
                release_at = now + 160;
            }
            else if (st.kind == ScriptStep::MOVE || st.kind == ScriptStep::HOVER)
            {
                SDL_Event m;
                SDL_zero(m);
                m.type = SDL_EVENT_MOUSE_MOTION;
                m.motion.x = st.x;
                m.motion.y = st.y;
                ui_lock();
                ui_sdl_event(&m, 1.0f, 1.0f);
                ui_unlock();
            }
            else if (st.kind == ScriptStep::PLAY)
            {
                done = true;
                *result = 1;
            }
            else if (st.kind == ScriptStep::QUIT)
            {
                done = true; *result = 0;
            }
            else if (st.kind == ScriptStep::SHOT)
            {
                shot_file = st.file;                    // written after this frame is drawn
            }
        }
        if (done) break;

        int w = 0, h = 0;
        SDL_GetWindowSize(window, &w, &h);
        float density = SDL_GetWindowPixelDensity(window);
        ui_lock();
        ui_apply_scale(window);
        ui_feed_gamepad();
        ImGui_ImplSDLRenderer3_NewFrame();
        ui_sdl_frame((float)w, (float)h, density);
        ImGui::NewFrame();
        UiResult res = ui_draw(UI_LAUNCHER, 0, 0, (float)w, (float)h);
        ImGui::Render();
        SDL_SetRenderScale(renderer, density, density);
        SDL_SetRenderDrawColorFloat(renderer, 0.07f, 0.06f, 0.06f, 1.0f);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        ui_unlock();
        if (shot_first && frames == 4) save_frame(renderer, shot_first);
        if (!shot_file.empty()) { save_frame(renderer, shot_file.c_str()); shot_file.clear(); }
        SDL_RenderPresent(renderer);
        frames++;
        SDL_Delay(1);
        if (res.play) { done = true; *result = 1; }
        if (res.leave)
        {
            done = true;
            *result = 0;
            open_setup = res.setup;
        }
    }

    ui_lock();
    ImGui_ImplSDLRenderer3_Shutdown();
    ui_context_end();
    ui_unlock();
    SDL_StopTextInput(window);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    fprintf(stderr, "[UI] launcher closed: %s\n", *result ? "play" : "quit");
    fflush(stderr);
    if (open_setup)
        pcui::setup_launch();
}

} // namespace

extern "C" int pc_launcher_run(void)
{
    int result = 1;
    if (!host_posix_have_video()) {
        fprintf(stderr, "[UI] launcher: no display; starting the game\n");
        return 1;
    }
    host_posix_call_on_main(launcher_main, &result);
    return result;
}
