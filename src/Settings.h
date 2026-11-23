#pragma once

/// Settings: the mod's settings, their file GameData\swbf-vr.ini and the "VR Settings" screen in the pause menu
/// (Settings.cpp). Each value is handed to the subsystem that uses it (Controls, Hud, Aim, ...) when the file is read
/// and whenever it changes in the menu.
///
/// All functions run on the game's main thread (the Present hook in D3dSurvey.cpp, and the Lua callback the screen
/// calls from the same thread).

namespace SwbfVr
{

namespace Settings
{

/// Reads GameData\swbf-vr.ini, logs the values that differ from the defaults and applies every setting. Only the
/// first call does anything (OnPresent calls it on the first presented frame).
void Load();

/// Reads one setting straight from swbf-vr.ini, for start-up code that runs before Load (the first frame).
/// @param name the setting's name.
/// @param value set to the file's value; empty when the file or the line is missing.
void ReadEarly(const char* name, char (&value)[64]);

/// Main thread, once per presented frame: loads the settings the first time, and once a second checks that the
/// mission's Lua interpreter has the VR Settings screen, installing it when not.
void OnPresent();

/// @return true while the pause menu (or a screen under it, such as VR Settings) is showing.
bool IsMenuOpen();

} // namespace Settings

} // namespace SwbfVr
