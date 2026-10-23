// The launcher: the settings screen in a window of its own, before the game starts.
//
// It has its own small Direct3D 11 device and swap chain (the game's are not created until the
// guest boots), runs its own message loop, and returns when the player presses Play or quits. It
// takes the keyboard and mouse from its window and the pads from the host input layer, which
// pc_settings.c starts before this runs.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

extern "C" {
#include "recomp_settings.h"
#include "input_host.h"
}
#include "pc_input.h"
#include "pc_settings.h"
#include "pc_ui.h"
#include "pc_ui_actions.h"
#include "pc_ui_internal.h"

namespace {

ID3D11Device *g_dev;
ID3D11DeviceContext *g_ctx;
IDXGISwapChain *g_swap;
ID3D11RenderTargetView *g_rtv;
bool g_done;
int g_result;

void make_rtv()
{
    ID3D11Texture2D *buffer = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&buffer)))) {
        g_dev->CreateRenderTargetView(buffer, nullptr, &g_rtv);
        buffer->Release();
    }
}

void drop_rtv()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool make_device(HWND window)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = window;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &level, &g_ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &level, &g_ctx);
    if (FAILED(hr)) return false;
    IDXGIFactory *factory = nullptr;
    if (SUCCEEDED(g_swap->GetParent(IID_PPV_ARGS(&factory)))) {
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        factory->Release();
    }
    make_rtv();
    return true;
}

void drop_device()
{
    drop_rtv();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

LRESULT CALLBACK launcher_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    ui_lock();
    bool taken = ui_capture_message(h, msg, wp, lp);
    if (!taken && ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) {
        ui_unlock();
        return 1;
    }
    ui_unlock();
    if (taken) return 0;
    switch (msg) {
    case WM_SIZE:
        if (g_swap && wp != SIZE_MINIMIZED) {
            drop_rtv();
            g_swap->ResizeBuffers(0, LOWORD(lp), HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            make_rtv();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0;      // no menu on Alt
        break;
    case WM_DPICHANGED: {
        const RECT *r = (const RECT *)lp;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        g_done = true;
        g_result = 0;
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

extern "C" int pc_launcher_run(void)
{
    typedef BOOL (WINAPI *SetDpiContext)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    SetDpiContext set = user32 ? (SetDpiContext)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext") : nullptr;
    if (set) set((HANDLE)(INT_PTR)-4);          // per-monitor aware: sharp text on a scaled desktop

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = launcher_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));   // IDC_ARROW
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));      // the exe's own icon (src/defjam.rc.in)
    wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"DefJamRecompLauncher";
    RegisterClassExW(&wc);

    int dpi = 96;
    {
        HDC dc = GetDC(nullptr);
        if (dc) { dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); }
    }
    RECT r = {0, 0, MulDiv(1100, dpi, 96), MulDiv(720, dpi, 96)};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int w = r.right - r.left, h = r.bottom - r.top;
    RECT work;
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        if (w > work.right - work.left) w = work.right - work.left;
        if (h > work.bottom - work.top) h = work.bottom - work.top;
        x = work.left + (work.right - work.left - w) / 2;
        y = work.top + (work.bottom - work.top - h) / 2;
    }
    HWND window = CreateWindowExW(0, L"DefJamRecompLauncher", L"Def Jam: Fight for NY - Launcher",
                                  WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!window) return 1;                       // no window: do not hold the game back
    if (!make_device(window)) {
        fprintf(stderr, "[UI] launcher: no Direct3D 11 device; starting the game\n");
        DestroyWindow(window);
        return 1;
    }

    ui_lock();
    ui_context_begin(window);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    ui_unlock();
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    fprintf(stderr, "[UI] launcher shown\n");
    fflush(stderr);

    bool open_setup = false;
    g_done = false;
    g_result = 1;
    while (!g_done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) { g_done = true; g_result = 0; }
        }
        if (g_done) break;
        if (IsIconic(window)) { Sleep(50); continue; }

        ui_lock();
        ui_apply_scale(window);
        ui_feed_gamepad();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGuiIO &io = ImGui::GetIO();
        UiResult res = ui_draw(UI_LAUNCHER, 0, 0, io.DisplaySize.x, io.DisplaySize.y);
        ImGui::Render();
        const float clear[4] = { 0.07f, 0.06f, 0.06f, 1.0f };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        ui_unlock();
        g_swap->Present(1, 0);
        if (res.play) { g_done = true; g_result = 1; }
        if (res.leave)
        {
            g_done = true;
            g_result = 0;
            open_setup = res.setup;
        }
    }

    ui_lock();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ui_context_end();
    ui_unlock();
    drop_device();
    DestroyWindow(window);
    UnregisterClassW(L"DefJamRecompLauncher", GetModuleHandleW(nullptr));
    fprintf(stderr, "[UI] launcher closed: %s\n", g_result ? "play" : "quit");
    fflush(stderr);
    if (open_setup)
        pcui::setup_launch();
    return g_result;
}
