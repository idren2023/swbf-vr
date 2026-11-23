#pragma once

/// EarMenu: the radial menu shown while a hand at an ear holds its grip (PLAN 32, idren2023 2026-10-13). It names what
/// each direction of the left stick picks (up RECENTER, right PLAYER LIST, down MAP, left PAUSE) and lights the one
/// the stick points at. It is projected from that ear and stays fixed in front of the head, so it moves with the
/// headset (EarMenu.cpp).
///
/// Controls decides what the menu does and reports its state with Update; Hud draws it with Draw. Both run on the
/// game's main thread.

namespace SwbfVr
{

namespace EarMenu
{

/// What a direction of the left stick picks while the menu is open.
enum class Choice
{
    None,
    Recenter,   // up
    PlayerList, // right
    Map,        // down
    Pause,      // left
};

/// The menu's state for this frame (Controls, once per presented frame).
/// @param isOpen a hand is at an ear with its grip held.
/// @param isAtRightEar the menu comes from the right ear (else the left).
/// @param stickX the left stick across, -1 to 1 (right positive).
/// @param stickY the left stick up and down, -1 to 1 (up positive).
/// @param taken the choice made by a flick this frame, or None.
void Update(bool isOpen, bool isAtRightEar, float stickX, float stickY, Choice taken);

/// Draws the menu into the eye image being composited (Hud::Composite, inside its scene and its saved state; the
/// world transform is identity and view and projection are the eye's).
/// @param device the game's IDirect3DDevice9.
/// @param eyeMatrix the eye camera in the world (16 floats, rows right, up, back, position).
/// @param headPosition the point between the eyes in the world (3 floats).
void Draw(void* device, const float* eyeMatrix, const float* headPosition);

/// Where the ear menu's projector is (the ear the menu was last opened at, the left one at first), in the world. The
/// big map and the pause menu are lit from it too (Hud).
/// @param eyeMatrix the eye camera in the world (16 floats).
/// @param headPosition the point between the eyes in the world (3 floats).
/// @param position set to the projector's position (3 floats).
void ProjectorPosition(const float* eyeMatrix, const float* headPosition, float* position);

} // namespace EarMenu

} // namespace SwbfVr
