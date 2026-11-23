#pragma once

/// ReloadZone: where the manual reload wants the canister, drawn as a red hologram under the gun (idren2023
/// 2026-11-10: it must be easy to see that a reload is needed and where the canister goes): a rectangular pyramid
/// standing on its tip, the tip in the gun about 5 cm ahead of the trigger finger, opening downward, with fine scan
/// lines and bands that rise into the gun as energy drawn in. It lies inside the sphere round the gun hand in which
/// the left hand lets the canister go (Holster); the sphere itself is not drawn.
///
/// Draw runs on the game's main thread, from Holster's draw inside the first-person render.

namespace SwbfVr
{

namespace ReloadZone
{

/// How much of the zone is shown.
enum class Level
{
    None,        // not drawn
    ReloadNeeded, // the clip is empty and a canister is on the belt: faint
    CanisterHeld, // the canister is in the left hand: clear
    CanisterIn,   // the held canister is inside the zone (letting go reloads): bright, the rings run faster
    Reloading     // the canister was let go in the zone and the game reloads: green; off when done
};

/// Draws the zone into the current eye image.
/// @param device the game's IDirect3DDevice9; its view and projection transforms are the eye's (first-person render).
/// @param gunWorld the gun hand's world matrix (16 floats, row vectors: right, up, back, position).
/// @param level how it is shown; None draws nothing.
void Draw(void* device, const float* gunWorld, Level level);

/// The canister's hologram at this moment: where Holster draws the canister's model, half transparent, under the
/// hologram layer that Draw adds.
/// @param matrix out, 16 floats: the gun hand's unit axes as rows (right, up, back) and the canister's middle.
/// @param size out: the canister's size (1 = its own). @param strength out: how solid, 0..1.
/// @return false when nothing is shown at this level.
bool GetCanister(const float* gunWorld, Level level, float* matrix, float* size, float* strength);

/// Releases the state block (the device is reset or lost; Stereo::OnReset).
void OnReset();

/// Test (script command `reloadzone <0..4>`): show the zone at this level whatever the ammunition; 0 = as the game says.
void SetForced(int level);

/// @return the level of SetForced, or None.
Level GetForced();

} // namespace ReloadZone

} // namespace SwbfVr
