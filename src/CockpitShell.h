#pragma once

/// CockpitShell: draws the stock tanks' cockpit shells (ShellDesigns) around the game's first-person cockpit model, so
/// the pilot sits in a closed cabin with a seat instead of a hood open at the floor and back (PLAN 54; idren2023
/// 2026-10-17; stock vehicles only, modded ones get none).
///
/// Main thread, inside the first-person pass (Aim's vehicle draw), right after the game's cockpit model and with that
/// model's own matrix, so the shell sits exactly where the cockpit does.
///
/// The HUD on the dashboard (idren2023 2026-10-17): each shell's displays (ShellDesigns::Display) show the game's HUD
/// groups (HudDraws: the pilot's and the vehicle's health, the weapon's heat or ammunition, the target's health, the
/// score, the minimap) from the game's own textures, in the same pass with the depth test, so the levers and arms
/// drawn after cover them.

namespace SwbfVr
{

namespace CockpitShell
{

/// Draws the shell of this stock vehicle, if it has one, built on first use (and again when the vehicle changes).
/// @param device the game's IDirect3DDevice9.
/// @param cockpitMatrix the cockpit model's world matrix as drawn this pass (16 floats, row vectors).
/// @param stockName the stock vehicle's ODF name (Vehicle::CurrentStockName), or null: nothing is drawn.
void Draw(void* device, const float* cockpitMatrix, const char* stockName);

/// When the game's cockpit model is drawn (same pass, same matrix as Draw's): the next shell draw then also hides the
/// parts of it the shell replaces (ShellDesigns::Shell::maskVertices, the combat speeder's right half), by depth (the
/// game draws its model later in the pass). Nothing for a shell without a mask.
/// @param device the game's IDirect3DDevice9.
/// @param cockpitMatrix the cockpit model's world matrix as it will be drawn (16 floats, row vectors).
/// @param stockName the stock vehicle's ODF name, or null.
void CutOpenings(void* device, const float* cockpitMatrix, const char* stockName);

/// Draws a design made of moving parts (a gunner's turret, ShellDesigns.h), each part with its own world matrix, built
/// on first use like Draw's shells (one design is held at a time).
/// @param device the game's IDirect3DDevice9.
/// @param partMatrices each part's world matrix this pass (16 floats each, row vectors; null: that part is not drawn);
/// part 0's (never null) turns the lights.
/// @param partCount how many matrices there are (parts beyond it are drawn with part 0's if the shell draws them at
/// rest, ShellDesigns::Shell::restPartCount, else not at all).
/// @param designName the design's name (e.g. "rep_hover_fightertank_gunner").
void DrawParts(void* device, const float* const* partMatrices, int partCount, const char* designName);

/// Whether a shell with HUD displays was drawn lately (the last quarter second): Hud then leaves the groups the
/// displays show out of the panel and off the wrist. Render thread.
bool HasDisplays();

/// Whether a display of the shell drawn lately shows this HUD group (HudDraws::Group as int): Hud leaves just those
/// off its panel (a gunner's screens show some groups, the rest stay on the panel). Render thread.
bool ShowsGroup(int group);

/// Whether the groups on the stock screens take the screen's readout colour (default) or keep the game's colours
/// (script `shell tint 0|1`, PLAN 57 j).
void SetReadoutTint(bool isOn);

/// The pilot's resting-pose space as the cockpit carries it (idren2023 2026-10-19: the tank levers slid off their tracks
/// over rough ground: they hung on the level camera while the cockpit pitches and rolls with the hull): the matrix of
/// the shell drawn last in a pilot seat (Draw) with its pilot's eye as the origin, rest -> world (row vectors). False
/// when no pilot seat's shell was drawn in the last 100 ms.
/// @param frame set to 16 floats when the result is true.
bool GetPilotFrame(float* frame);

/// Logs, once, each displayed group's batches with their bounds as fractions of the group as drawn (0 left / bottom),
/// to choose Display regions (script `shell groups`).
void RequestGroupLog();

/// Before the device is reset: releases the state block (default pool); the vertex buffer is managed and stays.
void OnReset();

} // namespace CockpitShell

} // namespace SwbfVr
