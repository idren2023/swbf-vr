// Settings.cpp: the mod's settings, their file (GameData\swbf-vr.ini) and the "VR Settings" screen in the pause menu.
//
// One table holds every setting: its name in the file, its value as text, and for those shown in the menu a label
// and the list of values the menu steps through. A change from the menu is applied at once (the match is still
// drawn behind the pause menu) and the whole file is written again.
//
// The screen is Lua, built with the game's own interface functions and run in the mission's interpreter
// (Lua.cpp). The pause menu already has a button the retail game hides ("Fake Console"); the mod shows it under
// the name "VR Settings" and sends it to its own screen, so no game file is changed. Each mission creates a new
// interpreter: once a second OnPresent asks the interpreter whether the screen is there (the Lua side
// answers through the registered function swbfvr_cmd) and installs it if not.

#include "Settings.h"

#include "Aim.h"
#include "Body.h"
#include "Controls.h"
#include "Feedback.h"
#include "FpGuard.h"
#include "Handlebars.h"
#include "Holster.h"
#include "Hud.h"
#include "Log.h"
#include "Lua.h"
#include "Main.h"
#include "Pointer.h"
#include "Sight.h"
#include "Squad.h"
#include "Vehicle.h"
#include "Voice.h"
#include "Zoom.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace SwbfVr
{

namespace Settings
{

namespace
{

struct Setting
{
    const char* name;   // in swbf-vr.ini
    const char* label;  // in the menu; null = not in the menu (file only)
    const char* values; // menu steps, separated by spaces
    const char* unit;   // shown after a number
    char value[32];     // current value; empty = not set (file-only settings keep the module's default)
    int tab;            // which tab of the menu (1-based, TABS); 0 = not in the menu
};

// The menu shows one tab at a time (idren2023, headset run 8: the single list had become too long), everything in
// capitals.
const char* const TABS[] = {"MOVEMENT", "HUD", "MOTION", "FEEDBACK"};

Setting _settings[] = {
    {"turn", "Turning", "smooth snap", "", "smooth", 1},
    {"smooth_degrees_per_second", "Smooth turn speed", "60 90 120 150 180 230 270 300", " deg/s", "230", 1},
    {"snap_degrees", "Snap turn step", "15 30 45 60 90", " deg", "30", 1},
    {"move", "Stick movement", "analog keys", "", "analog", 1},
    {"move_direction", "Movement direction", "controller head", "", "controller", 1}, // and the jet hover (PLAN 36)
    {"crouch", "Crouch", "button head", "", "head", 1}, // head by default since b206 (headset run 14: expected on)
    {"vignette", "Comfort vignette", "off light strong", "", "off", 1},
    {"bike_throttle", "Bike throttle", "lean twist", "", "lean", 1}, // PLAN 57 f: lean over the bars by default
    {"vehicle_controls", "Vehicle controls", "motion sticks", "", "motion", 1}, // idren2023 2026-11-04
    {"hud", "HUD layout", "classic compact immersive", "", "immersive", 2},
    {"hud_spread", "Compact HUD spread", "45 60 75 90", " %", "60", 2},
    {"hud_size", "Compact HUD size", "100 125 150 200", " %", "100", 2},
    {"crosshair", "Centre crosshair", "auto on", "", "auto", 2},
    {"aiming_line", "Aiming line", "1 0", "", "1", 2},
    {"ear_menu", "Hand at ear + grip: menu", "1 0", "", "1", 3},
    {"reload_gesture", "Reload: hand under gun + left trigger", "1 0", "", "1", 3},
    {"manual_reload", "Manual reload: ammo canister on the belt", "1 0", "", "1", 3},
    {"two_handed", "Two-handed grip (left grip)", "1 0", "", "1", 3},
    {"weapon_weight", "Weapon weight", "off light medium heavy", "", "heavy", 3},
    {"virtual_stock", "Virtual stock (two-handed)", "1 0", "", "0", 3},
    {"full_body", "Full body (Rebel soldier only for now)", "0 1", "", "0", 3},
    // idren2023 2026-10-26: the body sized to the player's height, in inches (shown in feet and inches), 6 ft 1 by default
    {"height", "Your height (full body)",
     "58 59 60 61 62 63 64 65 66 67 68 69 70 71 72 73 74 75 76 77 78 79 80 81 82 83 84", "", "73", 3},
    {"squad_gestures", "Squad gestures", "directions immersive", "", "directions", 3}, // headset run 15
    {"scope_gesture", "Zoom: weapon to the eye", "1 0", "", "0", 3},
    {"scope_head_aim", "Head aim when zoomed (no lens)", "1 0", "", "0", 3},
    {"scope_view", "Scope picture on", "lens window", "", "lens", 3},
    {"scope_window", "Scope window size", "30 40 50 60", " deg", "60", 3},
    {"hit_feedback", "Hit feedback (buzz, flash)", "1 0", "", "1", 4},
    {"hit_sound", "Hit sound", "1 0", "", "1", 4},
    {"recoil", "Recoil kick", "1 0", "", "1", 4},
    {"haptics", "Controller vibration", "1 0", "", "1", 4},
    // Voice squad commands (idren2023, 2026-10-17): off by default, as they open the microphone.
    {"voice_commands", "Voice squad commands", "0 1", "", "0", 4},
    {"voice_confidence", "Voice: confidence at least", "30 40 50 60 70 80 90", " %", "60", 4},
    {"voice_loudness", "Voice: loudness at least", "-60 -50 -45 -40 -35 -30 -25 -20 -15", " dB", "-35", 4},
    {"voice_device", nullptr, "", "", "", 0}, // part of the microphone's name; empty = Steam Link's, else default
    {"menu_pointer", nullptr, "", "", "", 0},
    {"menu_counts_x", nullptr, "", "", "", 0},
    {"menu_counts_y", nullptr, "", "", "", 0},
    {"secondary_weapon_key", nullptr, "", "", "", 0},
    {"crouch_key", nullptr, "", "", "", 0},
    {"gpu_transfer", nullptr, "", "", "", 0}, // read at game start by GpuTransfer: 0 (default) off, 1 with SteamVR, 2
                                              // always
    {"render_size", nullptr, "", "", "", 0}, // <width>x<height>: read at game start by D3dSurvey (ApplyRenderSize)
    {"log_debug", nullptr, "", "", "", 0},
    // Test switches read by GameBuild::Identify at start (CODE-STYLE section 7): treat the game as an unknown build,
    // and make one address's check fail (an Address name from GameBuild.h).
    {"test_unknown_build", nullptr, "", "", "", 0},
    {"test_break_check", nullptr, "", "", "", 0},
};

constexpr size_t SETTING_COUNT = sizeof(_settings) / sizeof(_settings[0]);
/// 'a' - 'A': subtracted from a lower-case ASCII letter gives its capital.
constexpr int LOWER_TO_UPPER_CASE = 32;
/// How often OnPresent asks the interpreter whether the screen is there.
constexpr DWORD PING_MILLISECONDS = 1000;

bool _isLoaded = false;
bool _isMenuOpen = false;
bool _hasPonged = false;         // the screen answered the latest ping
void* _screenLuaState = nullptr; // interpreter the screen was last installed in (for the log only)

Setting* FindSetting(const char* name)
{
    for (Setting& setting : _settings)
    {
        if (!strcmp(setting.name, name))
        {
            return &setting;
        }
    }
    return nullptr;
}

const char* ValueOf(const char* name)
{
    return FindSetting(name)->value;
}

// What the menu shows for a value.
std::string ComputeShownText(const Setting& setting, const std::string& value)
{
    if (!strcmp(setting.values, "1 0"))
    {
        return value == "1" ? "On" : "Off";
    }
    if (!strcmp(setting.name, "move"))
    {
        return value == "analog" ? "Analog" : "On/off (W A S D)";
    }
    if (!strcmp(setting.name, "move_direction"))
    {
        return value == "head" ? "Head" : "Left controller";
    }
    if (!strcmp(setting.name, "crouch"))
    {
        return value == "head" ? "B button or lower your head" : "B button";
    }
    if (!strcmp(setting.name, "height"))
    {
        const int inches = atoi(value.c_str());
        return std::to_string(inches / 12) + " ft " + std::to_string(inches % 12) + " in (" +
               std::to_string(static_cast<int>(inches * 2.54f + 0.5f)) + " cm)";
    }
    if (!strcmp(setting.name, "crosshair"))
    {
        return value == "auto" ? "Off with controller" : "On";
    }
    std::string text = value + setting.unit;
    if (text[0] >= 'a' && text[0] <= 'z')
    {
        text[0] = static_cast<char>(text[0] - LOWER_TO_UPPER_CASE);
    }
    return text;
}

// Hands one setting (or all, name = null) to the module that uses it.
void ApplySettings(const char* name)
{
    FpGuard guard;
    const auto isAffected = [name](const char* first, const char* second = nullptr, const char* third = nullptr)
    {
        return !name || !strcmp(name, first) || (second && !strcmp(name, second)) || (third && !strcmp(name, third));
    };
    if (isAffected("turn", "smooth_degrees_per_second", "snap_degrees"))
    {
        const bool isSnap = !strcmp(ValueOf("turn"), "snap");
        float snapDegrees = static_cast<float>(atof(ValueOf("snap_degrees"))),
              smoothDegrees = static_cast<float>(atof(ValueOf("smooth_degrees_per_second")));
        if (snapDegrees < 5 || snapDegrees > 180)
        {
            snapDegrees = 30.0f;
        }
        if (smoothDegrees < 10 || smoothDegrees > 720)
        {
            smoothDegrees = 180.0f;
        }
        Controls::SetTurn(isSnap, isSnap ? snapDegrees : smoothDegrees);
    }
    if (isAffected("move"))
    {
        Controls::SetMove(strcmp(ValueOf("move"), "keys") != 0);
    }
    if (isAffected("move_direction"))
    {
        Controls::SetMoveByHead(!strcmp(ValueOf("move_direction"), "head"));
    }
    if (isAffected("aiming_line"))
    {
        Sight::SetEnabled(atoi(ValueOf("aiming_line")) != 0);
    }
    if (isAffected("scope_head_aim"))
    {
        Aim::SetScopeHeadAim(atoi(ValueOf("scope_head_aim")) != 0);
    }
    if (isAffected("scope_view"))
    {
        Zoom::SetLensView(strcmp(ValueOf("scope_view"), "window") != 0);
    }
    if (isAffected("scope_window"))
    {
        Zoom::SetWindowDegrees(atoi(ValueOf("scope_window")));
    }
    if (isAffected("hud", "hud_spread", "hud_size"))
    {
        Hud::SetLayout(
            !strcmp(ValueOf("hud"), "classic")   ? 0
            : !strcmp(ValueOf("hud"), "compact") ? 1
                                                 : 2,
            static_cast<float>(atof(ValueOf("hud_spread"))) * 0.01f,
            static_cast<float>(atof(ValueOf("hud_size"))) * 0.01f);
    }
    if (isAffected("crosshair"))
    {
        Hud::SetCrosshair(strcmp(ValueOf("crosshair"), "on") != 0);
    }
    if (isAffected("ear_menu", "reload_gesture", "scope_gesture"))
    {
        Controls::SetGestures(
            atoi(ValueOf("ear_menu")) != 0,
            atoi(ValueOf("reload_gesture")) != 0,
            atoi(ValueOf("scope_gesture")) != 0);
    }
    if (isAffected("manual_reload"))
    {
        Holster::SetManualReload(atoi(ValueOf("manual_reload")) != 0);
    }
    if (isAffected("hit_feedback", "recoil", "haptics") || isAffected("hit_sound"))
    {
        Feedback::SetOptions(
            atoi(ValueOf("hit_feedback")) != 0,
            atoi(ValueOf("haptics")) != 0,
            atoi(ValueOf("recoil")) != 0,
            atoi(ValueOf("hit_sound")) != 0);
    }
    if (isAffected("two_handed"))
    {
        Aim::SetTwoHandedAllowed(atoi(ValueOf("two_handed")) != 0);
    }
    if (isAffected("weapon_weight", "virtual_stock"))
    {
        const char* weight = ValueOf("weapon_weight");
        Aim::SetWeaponHandling(
            !strcmp(weight, "light")    ? 1
            : !strcmp(weight, "medium") ? 2
            : !strcmp(weight, "heavy")  ? 3
                                        : 0,
            atoi(ValueOf("virtual_stock")) != 0);
    }
    if (isAffected("voice_commands", "voice_confidence", "voice_loudness") || isAffected("voice_device"))
    {
        Voice::SetOptions(
            atoi(ValueOf("voice_commands")) != 0,
            atoi(ValueOf("voice_confidence")),
            atoi(ValueOf("voice_loudness")),
            ValueOf("voice_device"));
    }
    if (isAffected("squad_gestures"))
    {
        Squad::SetGestureSet(strcmp(ValueOf("squad_gestures"), "immersive") == 0);
    }
    if (isAffected("vehicle_controls"))
    {
        Vehicle::SetSticksMode(!strcmp(ValueOf("vehicle_controls"), "sticks"));
    }
    if (isAffected("bike_throttle"))
    {
        Handlebars::SetTwistThrottle(!strcmp(ValueOf("bike_throttle"), "twist"));
    }
    if (isAffected("full_body"))
    {
        Body::SetFullBody(atoi(ValueOf("full_body")) != 0);
    }
    if (isAffected("height"))
    {
        const int inches = atoi(ValueOf("height"));
        Body::SetPlayerHeight(static_cast<int>((inches > 0 ? inches : 73) * 2.54f + 0.5f));
    }
    if (isAffected("crouch"))
    {
        Controls::SetCrouchByHead(!strcmp(ValueOf("crouch"), "head"));
    }
    if (isAffected("vignette"))
    {
        Hud::SetVignette(!strcmp(ValueOf("vignette"), "light") ? 1 : !strcmp(ValueOf("vignette"), "strong") ? 2 : 0);
    }
    if (isAffected("menu_pointer") && *ValueOf("menu_pointer"))
    {
        Pointer::SetEnabled(atoi(ValueOf("menu_pointer")) != 0);
    }
    if (isAffected("menu_counts_x") && *ValueOf("menu_counts_x"))
    {
        Pointer::SetScreenCounts(static_cast<float>(atof(ValueOf("menu_counts_x"))), 0);
    }
    if (isAffected("menu_counts_y") && *ValueOf("menu_counts_y"))
    {
        Pointer::SetScreenCounts(0, static_cast<float>(atof(ValueOf("menu_counts_y"))));
    }
    if (isAffected("secondary_weapon_key") && *ValueOf("secondary_weapon_key"))
    {
        Controls::SetSecondaryKey(static_cast<unsigned>(strtoul(ValueOf("secondary_weapon_key"), nullptr, 16)));
    }
    if (isAffected("crouch_key") && *ValueOf("crouch_key"))
    {
        Controls::SetCrouchKey(static_cast<unsigned>(strtoul(ValueOf("crouch_key"), nullptr, 16)));
    }
    if (isAffected("log_debug"))
    {
        Log::SetDebugEnabled(!strcmp(ValueOf("log_debug"), "1"));
    }
}

void SaveFile()
{
    FILE* file = Main::OpenModFile("swbf-vr.ini", "w");
    if (!file)
    {
        Log::Warning("settings", "swbf-vr.ini cannot be written");
        return;
    }
    fputs(
        "# swbf-vr settings. Written by the mod (pause menu, VR Settings); may also be edited by hand while the game "
        "is closed.\n",
        file);
    for (const Setting& setting : _settings)
    {
        if (!*setting.value)
        {
            continue;
        }
        if (setting.label)
        {
            fprintf(file, "# %s: %s\n", setting.label, setting.values);
        }
        fprintf(file, "%s = %s\n", setting.name, setting.value);
    }
    fclose(file);
}

void SetFromMenu(const char* name, const char* value)
{
    Setting* setting = FindSetting(name);
    if (!setting || !*value || strlen(value) >= sizeof(setting->value))
    {
        Log::Warning("settings", "unknown setting or value: %s = %s", name, value);
        return;
    }
    strcpy(setting->value, value);
    Log::Info("settings", "%s = %s (from the menu)", name, value);
    ApplySettings(name);
    SaveFile();
}

// Lua calls swbfvr_cmd("<text>"):
//   ping            the screen exists in this interpreter
//   loaded          it has just been built
//   menu 1 | 0      the pause menu was entered / left
//   set <name> <value>
int __cdecl OnLuaCommand(void* state)
{
    const char* text = Lua::ToString(state, 1);
    if (!text)
    {
        return 0;
    }
    char name[64], value[64];
    if (!strcmp(text, "ping"))
    {
        _hasPonged = true;
    }
    else if (!strcmp(text, "loaded"))
    {
        _hasPonged = true;
        Log::Info("settings", "VR settings screen installed in the pause menu (interpreter %p)", state);
    }
    else if (!strncmp(text, "menu ", 5))
    {
        _isMenuOpen = text[5] == '1';
        Log::Info("settings", "pause menu %s", _isMenuOpen ? "open" : "closed");
    }
    else if (sscanf(text, "set %63s %63s", name, value) == 2)
    {
        SetFromMenu(name, value);
    }
    else
    {
        Log::Info("settings", "from Lua: %s", text);
    }
    return 0;
}

// The Lua side. No closures and nothing newer than what the game's own scripts use.
const char LUA_SCREEN[] = R"lua(
swbfvr_tab = 1

-- Shows the options of the current tab only, writes every label, and links tabs and list for the stick and the keys.
function swbfvr_refresh(this)
	local i
	for i = 1, getn(swbfvr_opts) do
		local o = swbfvr_opts[i]
		if (o.tab == swbfvr_tab) then this.buttons[o.tag].hidden = nil else this.buttons[o.tag].hidden = 1 end
		RoundIFButtonLabel_fnSetUString(this.buttons[o.tag], ScriptCB_tounicode(o.label .. ": " .. o.shown[o.cur]))
	end
	for i = 1, getn(swbfvr_tabs) do
		-- The tab's name is on its orange bar (tb1, ...); the button under it only takes the stick and the pointer.
		RoundIFButtonLabel_fnSetUString(this.buttons["t" .. i], ScriptCB_tounicode(""))
		if (i == swbfvr_tab) then IFObj_fnSetAlpha(this.buttons["tb" .. i], 1) else IFObj_fnSetAlpha(this.buttons["tb" .. i], 0.4) end
		this.buttons["t" .. i].link_down = swbfvr_opts[1].tag
		this.buttons["t" .. i].link_up = "back"
	end
	this.buttons[swbfvr_opts[1].tag].link_up = "t" .. swbfvr_tab
	this.buttons.back.link_down = "t" .. swbfvr_tab
	ShowHideVerticalButtons(this.buttons, swbfvr_layout)
end

-- Tab buttons are tagged t1, t2, ...: the tab's number, or nil for any other button.
function swbfvr_tabOf(tag)
	if (tag and strsub(tag, 1, 1) == "t") then return tonumber(strsub(tag, 2)) end
	return nil
end

function swbfvr_showTab(this, n)
	if (n < 1) then n = getn(swbfvr_tabs) end
	if (n > getn(swbfvr_tabs)) then n = 1 end
	swbfvr_tab = n
	swbfvr_refresh(this)
	SetCurButton("t" .. n)
end

-- The selected setting moves on (or back) one value; the mod applies and saves it at once.
function swbfvr_step(this, by)
	local i
	for i = 1, getn(swbfvr_opts) do
		local o = swbfvr_opts[i]
		if (o.tag == this.CurButton) then
			o.cur = o.cur + by
			if (o.cur > getn(o.vals)) then o.cur = 1 end
			if (o.cur < 1) then o.cur = getn(o.vals) end
			swbfvr_cmd("set " .. o.key .. " " .. o.vals[o.cur])
		end
	end
	swbfvr_refresh(this)
end

swbfvr_layout = { ySpacing = 5, width = 520, font = "gamefont_medium", buttonlist = {}, yTop = -110 }
for i = 1, getn(swbfvr_opts) do
	swbfvr_layout.buttonlist[i] = { tag = swbfvr_opts[i].tag, string = "common.back" }
end
swbfvr_layout.buttonlist[getn(swbfvr_opts) + 1] = { tag = "back", string = "common.back" }
swbfvr_tablayout = { xWidth = 130, xSpacing = 26, height = 30, font = "gamefont_medium", yTop = -147, xLeft = 65, buttonlist = {} }
for i = 1, getn(swbfvr_tabs) do
	swbfvr_tablayout.buttonlist[i] = { tag = "t" .. i, string = "common.back" }
end

ifs_vr = NewIFShellScreen {
	nologo = 1,
	movieIntro = nil,
	movieBackground = nil,
	buttons = NewIFContainer {
		ScreenRelativeX = 0.5,
		ScreenRelativeY = 0.5,
		y = 20,
	},
	Enter = function(this, bFwd)
		gIFShellScreenTemplate_fnEnter(this, bFwd)
		swbfvr_refresh(this)
		SetCurButton("t" .. swbfvr_tab)
	end,
	Input_Accept = function(this)
		ifelm_shellscreen_fnPlaySound(this.acceptSound)
		if (this.CurButton == "back") then
			ScriptCB_PopScreen()
			return
		end
		local n = swbfvr_tabOf(this.CurButton)
		if (n) then swbfvr_showTab(this, n) else swbfvr_step(this, 1) end
	end,
	Input_GeneralLeft = function(this)
		local n = swbfvr_tabOf(this.CurButton)
		if (n) then swbfvr_showTab(this, n - 1) else swbfvr_step(this, -1) end
	end,
	Input_GeneralRight = function(this)
		local n = swbfvr_tabOf(this.CurButton)
		if (n) then swbfvr_showTab(this, n + 1) else swbfvr_step(this, 1) end
	end,
}
ifs_vr.CurButton = AddVerticalButtons(ifs_vr.buttons, swbfvr_layout)
AddHorizontalButtons(ifs_vr.buttons, swbfvr_tablayout)
-- Tabs drawn like the game's own (ifs_opt_pccontrols' ifs_build_background_and_tabs): flashy text on the orange
-- headerbutton bar, black letters, the current tab at full strength and the others at 0.4.
for i = 1, getn(swbfvr_tabs) do
	local b = ifs_vr.buttons["t" .. i]
	ifs_vr.buttons["tb" .. i] = NewIFText {
		ZPos = 255, x = (b.x or 0) - swbfvr_tablayout.xWidth / 2, y = (b.y or 0) - 20,
		textw = swbfvr_tablayout.xWidth, texth = 40, halign = "hcenter", valign = "vcenter", font = "gamefont_medium",
		ustring = ScriptCB_tounicode(swbfvr_tabs[i]), ColorR = 255, ColorG = 255, ColorB = 255,
		textcolorr = 0, textcolorg = 0, textcolorb = 0,
		bgleft = "headerbuttonleft", bgmid = "headerbuttonmid", bgright = "headerbuttonright",
		bg_width = swbfvr_tablayout.xWidth * 0.85,
	}
end
AddIFScreen(ifs_vr, "ifs_vr")

swbfvr_pmEnter = ifs_pausemenu.Enter
swbfvr_pmAccept = ifs_pausemenu.Input_Accept
swbfvr_pmExit = ifs_pausemenu.Exit
ifs_pausemenu.Enter = function(this, bFwd)
	swbfvr_pmEnter(this, bFwd)
	swbfvr_cmd("menu 1")
	if (not ScriptCB_IsSplitscreen()) then
		this.buttons.console.hidden = nil
		RoundIFButtonLabel_fnSetUString(this.buttons.console, ScriptCB_tounicode("VR SETTINGS"))
		this.CurButton = ShowHideVerticalButtons(this.buttons, ifspausemenu_vbutton_layout)
		SetCurButton(this.CurButton)
	end
end
ifs_pausemenu.Input_Accept = function(this)
	if (this.CurButton == "console") then
		ifelm_shellscreen_fnPlaySound(this.acceptSound)
		ifs_movietrans_PushScreen(ifs_vr)
	else
		swbfvr_pmAccept(this)
	end
end
ifs_pausemenu.Exit = function(this, bFwd)
	if (swbfvr_pmExit) then swbfvr_pmExit(this, bFwd) end
	if (not bFwd) then swbfvr_cmd("menu 0") end
end
swbfvr = 1
swbfvr_cmd("loaded")
)lua";

std::string ToUpperCase(std::string text)
{
    for (char& character : text)
    {
        if (character >= 'a' && character <= 'z')
        {
            character = static_cast<char>(character - LOWER_TO_UPPER_CASE);
        }
    }
    return text;
}

std::string BuildScreenChunk()
{
    std::string lua = "if swbfvr or not ifs_pausemenu or not ifs_pausemenu.buttons or not "
                      "ifs_pausemenu.buttons.console or not AddVerticalButtons then return end\nswbfvr_try = "
                      "(swbfvr_try or 0) + 1\nif swbfvr_try > 1 then return end\nswbfvr_opts = {\n";
    int index = 0;
    for (const Setting& setting : _settings)
    {
        if (!setting.label)
        {
            continue;
        }
        std::string valueList, shownList;
        int currentIndex = 1, count = 0;
        const char* cursor = setting.values;
        while (*cursor)
        {
            const char* end = strchr(cursor, ' ');
            const std::string item = end ? std::string(cursor, end) : std::string(cursor);
            ++count;
            if (item == setting.value)
            {
                currentIndex = count;
            }
            valueList += "\"" + item + "\",";
            shownList += "\"" + ComputeShownText(setting, item) + "\",";
            cursor = end ? end + 1 : cursor + item.size();
        }
        // A value from the file that the menu does not offer is shown as it is, and the first click moves on from it.
        bool isListed = false;
        for (const char* found = strstr(setting.values, setting.value); found && !isListed;
             found = strstr(found + 1, setting.value))
        {
            const char after = found[strlen(setting.value)];
            isListed = (found == setting.values || found[-1] == ' ') && (after == 0 || after == ' ');
        }
        if (!isListed)
        {
            valueList += std::string("\"") + setting.value + "\",";
            shownList += "\"" + ComputeShownText(setting, setting.value) + "\",";
            currentIndex = count + 1;
        }
        lua += "\t{ tag = \"o" + std::to_string(++index) + "\", key = \"" + setting.name +
               "\", tab = " + std::to_string(setting.tab) + ", label = \"" + ToUpperCase(setting.label) +
               "\", cur = " + std::to_string(currentIndex) + ", vals = {" + valueList + "}, shown = {" +
               ToUpperCase(shownList) + "} },\n";
    }
    lua += "}\nswbfvr_tabs = {";
    for (const char* tab : TABS)
    {
        lua += std::string("\"") + tab + "\",";
    }
    lua += "}\n";
    lua += LUA_SCREEN;
    return lua;
}

} // namespace

bool IsMenuOpen()
{
    return _isMenuOpen;
}

void ReadEarly(const char* name, char (&value)[64])
{
    value[0] = 0;
    FILE* file = Main::OpenModFile("swbf-vr.ini", "r");
    if (!file)
    {
        return;
    }
    char line[160], lineName[64], lineValue[64];
    while (fgets(line, sizeof(line), file))
    {
        if (sscanf(line, " %63[a-z_] = %63s", lineName, lineValue) == 2 && strcmp(lineName, name) == 0)
        {
            strcpy(value, lineValue);
        }
    }
    fclose(file);
}

void Load()
{
    if (_isLoaded)
    {
        return;
    }
    _isLoaded = true;
    // The values in the table are the defaults until the file is read; kept to log what the file changes.
    static char defaultValues[SETTING_COUNT][sizeof(_settings[0].value)];
    for (size_t index = 0; index < SETTING_COUNT; ++index)
    {
        strcpy(defaultValues[index], _settings[index].value);
    }
    FILE* file = Main::OpenModFile("swbf-vr.ini", "r");
    if (!file)
    {
        Log::Info("settings", "no swbf-vr.ini, using defaults");
    }
    else
    {
        char line[160], name[64], value[64];
        while (fgets(line, sizeof(line), file))
        {
            if (sscanf(line, " %63[a-z_] = %63s", name, value) != 2)
            {
                continue;
            }
            Setting* setting = FindSetting(name);
            if (setting && strlen(value) < sizeof(setting->value))
            {
                strcpy(setting->value, value);
            }
            else
            {
                Log::Warning("settings", "swbf-vr.ini: unknown line ignored: %s = %s", name, value);
            }
        }
        fclose(file);
        for (size_t index = 0; index < SETTING_COUNT; ++index)
        {
            if (strcmp(defaultValues[index], _settings[index].value) != 0)
            {
                Log::Info(
                    "settings",
                    "swbf-vr.ini: %s = %s (default %s)",
                    _settings[index].name,
                    _settings[index].value,
                    *defaultValues[index] ? defaultValues[index] : "none");
            }
        }
    }
    ApplySettings(nullptr);
}

// Main thread, once per presented frame.
void OnPresent()
{
    Load();
    static DWORD lastPingTick = 0;
    const DWORD now = GetTickCount();
    if (now - lastPingTick < PING_MILLISECONDS)
    {
        return;
    }
    lastPingTick = now;
    void* state = Lua::CurrentState(false);
    if (!state)
    {
        if (_isMenuOpen)
        {
            _isMenuOpen = false;
            Log::Info("settings", "mission over, pause menu taken as closed");
        }
        return;
    }
    static const char PING_CHUNK[] = "if swbfvr then swbfvr_cmd(\"ping\") end";
    _hasPonged = false;
    Lua::Execute(false, PING_CHUNK, sizeof(PING_CHUNK) - 1);
    if (_hasPonged)
    {
        return;
    }
    // A new interpreter (new mission), or one whose pause menu is not loaded yet: (try to) install the screen.
    // The chunk does nothing before the pause menu exists and builds the screen only once per interpreter, so a
    // Lua error half way is not repeated every second.
    if (_isMenuOpen)
    {
        _isMenuOpen = false;
        Log::Info("settings", "new mission, pause menu taken as closed");
    }
    Lua::Register(false, "swbfvr_cmd", &OnLuaCommand);
    const std::string chunk = BuildScreenChunk();
    const int status = Lua::Execute(false, chunk.c_str(), chunk.size());
    if (status != 0 || state != _screenLuaState)
    {
        Log::Info(
            "settings",
            "installing the VR settings screen in interpreter %p: status %d%s",
            state,
            status,
            _hasPonged    ? ""
            : status == 0 ? " (pause menu not loaded yet)"
                          : " (Lua error)");
    }
    _screenLuaState = state;
}

} // namespace Settings

} // namespace SwbfVr
