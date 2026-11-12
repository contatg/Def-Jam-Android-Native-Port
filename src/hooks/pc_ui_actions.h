#pragma once
#include <string>
namespace pcui
{
enum class Folder
{
    Data,
    Saves,
    Mods,
    Install,
    Settings,
    Logs
};
std::string folder_path(Folder which);
bool copy_text(const std::string &text);
bool open_path(const std::string &path, bool reveal = false);
const std::string &action_status();
void clear_action_status();
void setup_choose();
bool setup_poll();
std::string setup_path();
bool setup_ready();
// Called after launcher teardown. Never starts installation; opens the setup wizard.
bool setup_launch();
} // namespace pcui
