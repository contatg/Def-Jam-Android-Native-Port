#define _CRT_SECURE_NO_WARNINGS
#include "pc_ui_actions.h"
#include "pc_ui_model.h"
#include "pc_settings.h"
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <mutex>
#include <thread>
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#else
#include <SDL3/SDL.h>
#include <spawn.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace pcui
{
static std::string status, picked;
static std::mutex picker_mutex;
static bool pick_done = false;
static std::string env(const char *name)
{
#if defined(_WIN32)
    std::wstring key(name, name + strlen(name));
    const wchar_t *s = _wgetenv(key.c_str());
    return s ? fs::path(s).u8string() : "";
#else
    const char *s = getenv(name);
    return s ? s : "";
#endif
}
std::string folder_path(Folder which)
{
    fs::path settings = fs::u8path(pc_settings_path());
    fs::path data = !env("DEFJAM_DATA").empty() ? fs::u8path(env("DEFJAM_DATA"))
                    : settings.empty()          ? fs::current_path()
                                                : settings.parent_path();
    if (data.empty())
        data = fs::current_path();
    switch (which)
    {
    case Folder::Settings:
        return settings.u8string();
    case Folder::Data:
        return data.u8string();
    case Folder::Saves:
        return (data / "save").u8string();
    case Folder::Mods: {
        auto root = env("RECOMP_TEXTURE_ROOT");
        return root.empty() ? (data / "mods").u8string() : root;
    }
    case Folder::Install: {
        auto root = env("DEFJAM_INSTALL_DIR");
        return root.empty() ? fs::current_path().u8string() : root;
    }
    case Folder::Logs: {
        auto logs = env("DEFJAM_LOG_DIR");
        return logs.empty() ? (fs::current_path() / "logs").u8string() : logs;
    }
    }
    return "";
}
void clear_action_status()
{
    status.clear();
}
const std::string &action_status()
{
    return status;
}
#if defined(_WIN32)
static std::wstring wide(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (!n)
        return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(n - 1);
    return w;
}
static std::string narrow(const wchar_t *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, nullptr, 0, nullptr, nullptr);
    if (!n)
        return "";
    std::string a(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, -1, a.data(), n, nullptr, nullptr);
    a.resize(n - 1);
    return a;
}
#else
static bool spawn_detached(const std::string &program, std::vector<std::string> args)
{
    std::vector<char *> argv;
    for (auto &a : args)
        argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawn_file_actions_t files;
    posix_spawn_file_actions_init(&files);
#if defined(__APPLE__)
    // The installed app's play.lock fd survives exec; it must not reach setup.
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_CLOEXEC_DEFAULT);
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&files, 3);
#else
    long limit = sysconf(_SC_OPEN_MAX);
    if (limit < 0)
        limit = 1024;
    for (int fd = 3; fd < limit; fd++)
        if (fcntl(fd, F_GETFD) >= 0)
            posix_spawn_file_actions_addclose(&files, fd);
#endif
    pid_t pid;
    int error = posix_spawnp(&pid, program.c_str(), &files, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&files);
    posix_spawnattr_destroy(&attr);
    if (!error)
        std::thread([pid] {
            int status;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            {
            };
        }).detach();
    return error == 0;
}
#endif
bool copy_text(const std::string &text)
{
    bool ok = false;
#if defined(_WIN32)
    auto value = wide(text);
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (value.size() + 1) * sizeof(wchar_t));
    if (data)
    {
        void *memory = GlobalLock(data);
        if (memory)
        {
            memcpy(memory, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
            GlobalUnlock(data);
            if (OpenClipboard(GetForegroundWindow()))
            {
                if (EmptyClipboard())
                    ok = SetClipboardData(CF_UNICODETEXT, data) != nullptr;
                CloseClipboard();
            }
        }
        if (!ok)
            GlobalFree(data);
    }
#else
    // The overlay draws off the main thread. Queue without waiting while holding the UI lock.
    auto *value = new std::string(text);
    ok = SDL_RunOnMainThread(
        [](void *arg) {
            auto *value = static_cast<std::string *>(arg);
            if (!SDL_SetClipboardText(value->c_str()))
                fprintf(stderr, "[UI] Clipboard copy failed: %s\n", SDL_GetError());
            delete value;
        },
        value, false);
    if (!ok)
        delete value;
#endif
    status = ok ? "Copy requested." : "Could not access the system clipboard.";
    return ok;
}
bool open_path(const std::string &path, bool reveal)
{
    if (path.empty())
    {
        status = "No path is available for this run.";
        return false;
    }
    bool url = path.compare(0, 8, "https://") == 0;
    std::error_code ec;
    if (!url && !fs::exists(fs::u8path(path), ec))
    {
        status = "Path is unavailable: " + path + ". Use Copy path in Maintenance.";
        return false;
    }
    bool ok = false;
#if defined(_WIN32)
    if (reveal)
    {
        std::wstring args = L"/select,\"" + wide(path) + L"\"";
        ok = (INT_PTR)ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL) >
             32;
    }
    else
        ok = (INT_PTR)ShellExecuteW(nullptr, L"open", wide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL) >
             32;
#elif defined(__APPLE__)
    ok = spawn_detached("/usr/bin/open", reveal ? std::vector<std::string>{"open", "-R", path}
                                                : std::vector<std::string>{"open", path});
#else
    std::string target = reveal ? fs::u8path(path).parent_path().u8string() : path;
    ok = spawn_detached("xdg-open", {"xdg-open", target});
#endif
    status = ok ? "Requested opening: " + path
                : "Could not open: " + path + ". Use Copy path, or open it in Desktop Mode.";
    return ok;
}
static fs::path setup_note()
{
    return fs::u8path(folder_path(Folder::Data)) / "launcher-setup-path.txt";
}
std::string setup_path()
{
    auto value = env("DEFJAM_SETUP_PATH");
    if (!value.empty())
        return value;
    std::ifstream file(setup_note());
    std::getline(file, value);
    return trim(value);
}
static fs::path setup_program(const std::string &path)
{
    fs::path p = fs::u8path(path);
#if defined(__APPLE__)
    if (p.extension() == ".app")
        p /= "Contents/MacOS/DefJamSetup";
#endif
    return p;
}
static bool setup_usable(const std::string &path)
{
    std::error_code ec;
    auto program = setup_program(path);
    if (!fs::is_regular_file(program, ec))
        return false;
#if !defined(_WIN32)
    return access(program.c_str(), X_OK) == 0;
#else
    return true;
#endif
}
bool setup_ready()
{
    return setup_usable(setup_path());
}
#if !defined(_WIN32)
static void dialog_done(void *, const char *const *files, int)
{
    std::lock_guard<std::mutex> lock(picker_mutex);
    picked = files && files[0] ? files[0] : "";
    pick_done = true;
}
#endif
void setup_choose()
{
#if defined(_WIN32)
    wchar_t file[32768] = {};
    OPENFILENAMEW chooser = {};
    chooser.lStructSize = sizeof(chooser);
    chooser.lpstrFile = file;
    chooser.nMaxFile = 32768;
    chooser.lpstrFilter = L"Def Jam Setup\0*.exe\0\0";
    chooser.lpstrTitle = L"Choose Def Jam Setup";
    chooser.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&chooser))
    {
        std::lock_guard<std::mutex> lock(picker_mutex);
        picked = narrow(file);
        pick_done = true;
    }
#else
    SDL_ShowOpenFileDialog(dialog_done, nullptr, nullptr, nullptr, 0, nullptr, false);
#endif
}
bool setup_poll()
{
    std::lock_guard<std::mutex> lock(picker_mutex);
    if (!pick_done)
        return false;
    pick_done = false;
    if (picked.empty())
        return false;
    if (!setup_usable(picked))
    {
        status = "Choose an executable setup application; open or extract its dmg/archive first.";
        return false;
    }
    std::ofstream note(setup_note(), std::ios::trunc);
    note << picked << '\n';
    status = note ? "Setup selected. Use Open Setup to continue." : "Could not remember the setup path.";
    picked.clear();
    return true;
}
bool setup_launch()
{
    auto path = setup_path();
    if (!setup_ready())
    {
        status = "Setup is unavailable. Choose its application again.";
        return false;
    }
    bool ok = false;
#if defined(_WIN32)
    ok = (INT_PTR)ShellExecuteW(nullptr, L"open", wide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
#else
    auto program = setup_program(path).u8string();
    ok = spawn_detached(program, {program});
#endif
    status = ok ? "Setup opened." : "Could not open setup. Check its permissions and path.";
    if (!ok)
    {
        fprintf(stderr, "[UI] %s\n", status.c_str());
#if defined(_WIN32)
        MessageBoxW(nullptr, wide(status).c_str(), L"Def Jam Setup", MB_OK | MB_ICONERROR);
#else
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Def Jam Setup", status.c_str(), nullptr);
#endif
    }
    return ok;
}
} // namespace pcui
