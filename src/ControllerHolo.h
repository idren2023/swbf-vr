#pragma once

/// ControllerHolo: the player's two controllers drawn as red holograms in the view (idren2023 2026-10-27, the droid
/// starfighter's "detached head" HUD, docs/PLAN.md): the SteamVR render models of the Quest Touch Plus controllers
/// (SteamVR's resources/rendermodels, read at run time from the SteamVR install: never copied into the repository),
/// each drawn flat (orthographic, as the head sees it, turned exactly as the real controller is, live) into a screen
/// texture shown on a panel low in the view to one side of the middle, with horizontal scan lines scrolling down it;
/// a held grip, a pulled trigger and a pressed button glow; labels beside each screen, a line from a control to its
/// label while that control faces the viewer.
///
/// All functions run on the game's main thread (Draw from Aim's first-person vehicle draw, inside Render).

namespace SwbfVr
{

namespace ControllerHolo
{

/// Draws the holograms into the current eye image when they are shown: in the droid starfighter's pilot seat, or
/// anywhere with the test switch on. Models load on the first draw; without SteamVR's files nothing is drawn.
/// @param device the game's IDirect3DDevice9.
void Draw(void* device);

/// The droid starfighter's viewscreen over the whole eye image, in front of everything (the HUD included): its red
/// filter, wash and scan lines. At the end of the eye's frame (Stereo, after the HUD), in the droid's pilot seat.
/// @param device the game's IDirect3DDevice9.
void DrawViewscreen(void* device);

/// Releases the render targets and textures (the device is reset or lost; Stereo::OnReset).
void OnReset();

/// Test switch (script command `holo 0|1`): show the holograms in every vehicle seat.
void SetForced(bool isForced);

/// Test (script command `holo target <n>`): take a picture (as `grab 1 2`) when the game shows a target, at most this
/// many, 2 s apart.
void SetTargetGrabs(int count);

} // namespace ControllerHolo

} // namespace SwbfVr
