#pragma once

/// StockTables: the pieces of the game's own models our shells use (starfighter exteriors round their cockpits, the
/// stock first-person cockpits' pieces, the combat speeder's mirrored half), built once at game start from the
/// player's own level files (LvlFile) instead of being compiled in (docs/research/runtime-assets.md). The C++ twins
/// of tools/stockexterior.py, tools/stockcockpit.py and tools/mirrorstock.py (run with SWBF_SOURCE=lvl); the triangle
/// labels that stand in for the .msh node names come from tools/lvllabels.json (build/gen/LvlLabels.inc).
///
/// Space and conventions as before: the cockpit model's (metres; x right, y up, z back), texture coordinates as
/// Direct3D's. Without the game files the tables are empty and the designs leave those pieces out.

#include "ShellKit.h"

#include <string>

namespace SwbfVr
{

namespace StockTables
{

/// One vertex of a stock piece: position, normal, texture coordinates, and which texture (exteriors: 3 hull, 4
/// glass, 5 the design's cap finish; first-person cockpits: 1 cockpit, 2 effects, 4 window glass).
struct StockVertex
{
    ShellKit::Vec3 position, normal;
    float u, v;
    int texture;
};

struct Table
{
    const StockVertex* vertices;
    int count; // vertices, three per triangle
};

/// Builds every table from the level files in `lvlFolder` (ending in a slash: GameData\Data\_lvl_pc\). Once, before
/// any shell is built; returns a one-line summary for the log.
std::string Build(const std::string& lvlFolder);

/// A table by its old include name (EXTERIOR_XWING, STOCK_ATAT, SPEEDER_MIRROR, ...); empty if unknown or not built.
Table Get(const char* name);

/// The combat speeder's depth mask (three corners per triangle).
const ShellKit::Vec3* SpeederMask(int& count);

/// Every table's name, for the test dump (tools/stocktables_dump.cpp); null-terminated.
const char* const* Names();

} // namespace StockTables

} // namespace SwbfVr
