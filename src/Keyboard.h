#pragma once

/// Keyboard: a virtual keyboard for the menus that want typed text (idren2023 2026-11-10: profile name, server name,
/// address, password; nothing could be typed in the headset). It is drawn into the flat menu picture before that
/// goes to the headset: a small KEYBOARD tab at the bottom middle while the pointer is on the screen, and after a
/// click on it the keys over the lower part of the screen. The pointer (the right controller, Pointer.h) is the
/// finger and the trigger the press; a key goes to the game as the same DirectInput key code a real keyboard gives
/// (Input.h; run 727: the name field takes those).
///
/// Draw, OnPresent and OnReset run on the main thread; OnMouseButton on whatever thread presses the mouse button
/// (the main thread for the controller, the script thread for a script: it only notes the press).

namespace SwbfVr
{

namespace Keyboard
{

/// Draws the tab or the keys into the back buffer. Called for a flat frame (a menu, not the 3D scene) before it is
/// presented or sent to the headset.
/// @param device the game's IDirect3DDevice9. @param width, height the back buffer's size in pixels.
void Draw(void* device, int width, int height);

/// The left mouse button is about to be pressed or released for the game (Input::MouseButton).
/// @return true when the keyboard took it (the cursor is on the tab or on the keys): the game must not get it.
bool OnMouseButton(bool isDown);

/// Presses and releases the keys taken by OnMouseButton. Once per presented frame.
void OnPresent();

/// Releases the state block (the device is reset or lost; Stereo::OnReset).
void OnReset();

/// Script command `keyboard <0|1>`: hide or show the keys, as a click on the tab or on HIDE does.
void SetOpen(bool isOpen);

/// @return true while the keys are shown.
bool IsOpen();

} // namespace Keyboard

} // namespace SwbfVr
