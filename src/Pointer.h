#pragma once

/// Pointer: the right controller as a mouse pointer in menus (Pointer.cpp). The game's own cursor follows the point
/// where the controller's ray meets the menu screen; the trigger is the left mouse button (Controls.cpp).
///
/// All functions run on the game's main thread.

namespace SwbfVr
{

namespace Pointer
{

/// Moves the game's cursor toward where the controller points. Once per presented frame (called from the Present
/// hook in D3dSurvey.cpp).
void OnPresent();

/// @return true while the controller is steering the menu cursor (the mouse then belongs to the pointer).
bool IsActive();

/// Where the game's cursor is by the pointer's own count (the Keyboard asks which key is under it).
/// @param u, v out: fractions of the menu screen from its top left corner.
/// @return false while the controller is not steering the cursor or the cursor is still being sent to the corner.
bool GetCursor(float* u, float* v);

/// Switches the pointer on or off (swbf-vr.ini: menu_pointer).
/// @param isOn true switches it on.
void SetEnabled(bool isOn);

/// Mouse counts across and down the whole menu screen (swbf-vr.ini: menu_counts_x, menu_counts_y; script "pointer
/// counts <x> <y>"). Depends on the game's mouse sensitivity. The current values are logged either way.
/// @param countsX counts from the left to the right edge; values outside 100..20000 are ignored (0 = unchanged).
/// @param countsY counts from the top to the bottom edge; values outside 100..20000 are ignored (0 = unchanged).
void SetScreenCounts(float countsX, float countsY);

/// Test (script "pointer <u%> <v%>"): sends the cursor to a fixed point instead of following the controller.
/// @param u fraction across from the left edge; negative ends the test.
/// @param v fraction down from the top edge.
void SetTestTarget(float u, float v);

} // namespace Pointer

} // namespace SwbfVr
