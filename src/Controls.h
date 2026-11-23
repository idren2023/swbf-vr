#pragma once

/// Controls: controller buttons and sticks turned into the keyboard and mouse input the game already understands
/// (moving, turning, firing, crouch, the pause menu and the hand gestures; Controls.cpp).
///
/// OnPresent runs on the game's main thread once per presented frame; the setters are called from Settings (the ini
/// and the VR Settings screen) and from script commands, on the same thread.

namespace SwbfVr
{

namespace Controls
{

/// Switches the hand gestures on or off (swbf-vr.ini: ear_menu, reload_gesture, scope_gesture).
/// @param isEarMenuOn left hand at an ear with the grip held: the left stick picks pause menu, map or player list.
/// @param isReloadGestureOn left hand under the gun and a pull of the left trigger reloads.
/// @param isScopeGestureOn the weapon raised to the eye zooms in, lowered zooms out.
void SetGestures(bool isEarMenuOn, bool isReloadGestureOn, bool isScopeGestureOn);

/// Crouch when the head goes down (standing play; swbf-vr.ini: crouch = head | button).
/// @param isOn true = crouch by lowering the head as well as by B; false = by B only.
void SetCrouchByHead(bool isOn);

/// How much the player is being turned or moved by the sticks right now (for the comfort vignette).
/// @return 0..1; 0 while no controller is active or the pause menu is open.
float CurrentMotion();

/// How the left stick moves the player (swbf-vr.ini: move). Up / down / left / right are the player's W / S / A / D,
/// relative to the character's forward, which follows the left controller's heading.
/// @param isAnalog true = through the game's analog movement inputs; false = as the W/A/S/D keys.
void SetMove(bool isAnalog);

/// Which way the left stick's forward points (swbf-vr.ini: move_direction = controller | head): the character, and
/// the jet trooper's hover (Jet), follow the left controller or the head.
/// @param isByHead true = the head; false = the left controller.
void SetMoveByHead(bool isByHead);

/// The key the game has bound to "next secondary weapon" (swbf-vr.ini: secondary_weapon_key).
/// @param keyCode its DirectInput key code (only the low 8 bits are used).
void SetSecondaryKey(unsigned keyCode);

/// The key the game has bound to crouch (swbf-vr.ini: crouch_key; Right Ctrl by default).
/// @param keyCode its DirectInput key code (only the low 8 bits are used).
void SetCrouchKey(unsigned keyCode);

/// Reads the controllers and feeds the game's input: keys, mouse buttons, mouse movement for turning, the analog
/// movement values. Main thread, once per presented frame (called from the Present hook in D3dSurvey.cpp).
void OnPresent();

/// True while the statistics after a match are up (a large overlay while the player controls nothing, not the spawn
/// screen). Decided in OnPresent; main thread.
bool IsStatisticsScreen();

/// The game's reload control is tapped at the next OnPresent (Holster's manual reload, main thread).
void RequestReload();

/// The player is a droideka standing on its legs (not rolled up, rolling up or standing up), as of the last Present
/// (PLAN 58 f: its aiming lines only then).
bool IsDroidekaStanding();

/// Turning by the right stick (swbf-vr.ini: turn, snap_degrees, smooth_degrees_per_second; script "turn").
/// @param isSnap true = one step per flick; false = smooth turning.
/// @param degrees snap: degrees per step; smooth: degrees per second at full deflection.
void SetTurn(bool isSnap, float degrees);

/// Mouse counts the game needs for one degree of turn (depends on the game's mouse sensitivity; script "turn").
/// @param counts counts per degree; values outside 0.1..1000 are ignored (the current value is logged either way).
void SetCountsPerDegree(float counts);

} // namespace Controls

} // namespace SwbfVr
