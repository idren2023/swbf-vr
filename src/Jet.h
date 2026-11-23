#pragma once

/// Jet: jet packs steered for VR (PLAN 36, idren2023 2026-10-13; Jet.cpp).
///   jump pack (dark trooper, one big jet jump): the jump's angle above the ground follows the headset's pitch.
///   hover jet pack (jet trooper, JetType "hover"): the left stick flies along the left controller's pointing
///   direction (or the head's with head-based movement; pitch included), back, and level sideways; the right stick's
///   up / down adds a climb or sink; the right stick's left / right still turns.
/// Only the player's soldier and only while VR runs; everything else is the game's own.
///
/// OnPresent runs on the game's main thread; the hooks run inside the soldier update on the same thread.

namespace SwbfVr
{

namespace Jet
{

/// Once per presented frame (Controls): installs the hooks when VR first runs, takes this frame's input and
/// steering direction.
/// @param rightStickY the right stick up and down, -1 to 1 (up positive).
/// @param leftStickX the left stick across, -1 to 1 (right positive).
/// @param leftStickY the left stick up and down, -1 to 1 (forward positive).
/// @param isSteerByHead true = steer by the head (move_direction = head); false = by the left controller.
void OnPresent(float rightStickY, float leftStickX, float leftStickY, bool isSteerByHead);

/// Whether the player is hovering with a jet pack right now (the right stick's up / down belongs to the jet then).
bool IsHovering();

} // namespace Jet

} // namespace SwbfVr
