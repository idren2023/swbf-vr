#pragma once

/// Sight: the aiming line, a thin ribbon from the muzzle along the hand-held weapon's aim, like a laser sight; in a
/// gunner seat and as a droideka one from each of the guns' fire points along their aim (Sight.cpp).
///
/// Draw and OnReset run on the render thread (called from Stereo.cpp); Flash and SetEnabled on the game's main
/// thread (Feedback, Settings), which is the same thread.

namespace SwbfVr
{

namespace Sight
{

/// A hit: the line is drawn bright and wide in this colour for 220 ms, also when it is switched off.
/// @param rgb the colour as 0xRRGGBB.
void Flash(unsigned rgb);

/// Draws the line into the current eye image. Once per eye, between the scene and the first-person pass (Stereo.cpp's
/// SetNearFar hook): the scene's depth and its VIEW and PROJECTION transforms are still set.
/// @param device the game's IDirect3DDevice9.
void Draw(void* device);

/// Whether the lines of a seat's guns are drawn now (aiming line on): the game's reticle is then left out (Hud).
bool IsReplacingReticle();

/// Before the device is reset: releases the state block (default pool).
void OnReset();

/// Switches the line on or off (swbf-vr.ini: aiming_line).
/// @param isOn true switches it on.
void SetEnabled(bool isOn);

} // namespace Sight

} // namespace SwbfVr
