#pragma once

/// Input: synthetic keyboard and mouse input merged into what the game reads from DirectInput (Input.cpp). Used by
/// the test scripts (Script), the controller mapping (Controls) and the menu pointer (Pointer.cpp).
///
/// The functions that queue input may be called from any thread (the script thread, the main thread); they take a
/// lock shared with the hooked device reads.

namespace SwbfVr
{

namespace Input
{

/// Hooks IDirectInput8::CreateDevice so the game's keyboard and mouse can be hooked when it creates them. Called with
/// the IDirectInput8 object the real dinput8 handed to the game (Main.cpp, the DirectInput8Create export); only the
/// first call does anything.
/// @param directInput the game's IDirectInput8A object.
void OnDirectInputCreated(void* directInput);

/// @return true once the game has created a device and its read methods are hooked, so injected input reaches it.
bool IsReady();

/// Switches script mode on or off. While active, the real keyboard and mouse are ignored; switching off clears all
/// held keys, buttons and pending motion.
/// @param isActive true to ignore the real devices.
void SetActive(bool isActive);

/// Presses or releases a key.
/// @param keyCode DirectInput key code (DIK_*); codes from 256 up are ignored.
/// @param isDown true to press, false to release.
void Key(unsigned keyCode, bool isDown);

/// Moves the mouse.
/// @param countsX, countsY relative movement in mouse counts (x right, y down).
void MouseMove(long countsX, long countsY);

/// Presses or releases a mouse button.
/// @param buttonIndex 0 left, 1 right, 2 middle (up to 7).
/// @param isDown true to press, false to release.
void MouseButton(unsigned buttonIndex, bool isDown);

/// Turns the mouse wheel.
/// @param delta wheel movement, 120 (WHEEL_DELTA) per notch, positive is up.
void MouseWheel(long delta);

/// Logs, for every device the game read, how often it was read with GetDeviceState and GetDeviceData.
void LogStats();

} // namespace Input

} // namespace SwbfVr
