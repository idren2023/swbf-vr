#pragma once

/// GameMeshes: our game-derived meshes (the units' forearms, the droid arms, the player body, the seated trooper),
/// made at game start from the player's own game models instead of being compiled in
/// (docs/research/runtime-assets.md, the meshes step). Each vertex is a place on a triangle of the game model (after
/// our similarity transform) plus our offset in that triangle's frame, our normal in that frame and our uv as a
/// difference from the game's (tools/meshpatch.py, build/gen/MeshPatches.inc). The tables' other columns (bones,
/// weights) and their triangles are ours and stay compiled in; position, normal and uv are compiled in as zeros.
/// When a game model is missing or not the one the offsets were made against, its tables stay zero (drawn as nothing).

#include <cstddef>
#include <string>

namespace SwbfVr
{

namespace GameMeshes
{

/// Called by the tables' own static initialisers (the stripped BodyArm, PlayerBody and SeatedTrooper includes): each
/// vertex starts with eight floats (position, normal, uv) that Build fills. Returns true (for the initialiser).
bool Register(const char* table, void* vertices, size_t stride, int count);

/// Fills every registered table from the level files in `lvlFolder` (ending in a slash). Once at start, before
/// anything draws; a summary for the log.
std::string Build(const std::string& lvlFolder);

} // namespace GameMeshes

} // namespace SwbfVr
