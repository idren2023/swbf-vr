#pragma once

/// LvlFile: reads models out of the game's own munged level files (GameData\Data\_lvl_pc\*.lvl), so the mod builds
/// its tables from the player's copy of the game at start instead of carrying the game's data
/// (docs/research/runtime-assets.md). The C++ twin of tools/lvlgeo.py; pure file reading, no game memory, so the
/// shell preview tool links it too.
///
/// A level file is a tree of chunks (four-letter tag, 32-bit size, body padded to 4 bytes): ucfb at the top, lvl_
/// sub-levels (a name hash and a size before their children), and in them skel (a model's bones) and modl (the model:
/// segm segments, one per material). Positions come out in the model's space (bone frames applied, skinned vertices
/// moved out of their bone's space), texture coordinates as stored (v runs the other way from the mod tools' .msh).

#include <string>
#include <vector>

namespace SwbfVr
{

namespace LvlFile
{

struct Point
{
    double x, y, z;
};

struct Segment
{
    std::string texture; // the base texture (TNAM slot 0)
    std::vector<Point> positions;
    std::vector<Point> normals;
    std::vector<double> u, v;
    std::vector<int> triangles; // three vertex indices each
};

struct Model
{
    std::vector<Segment> segments;
    unsigned hash = 0; // FNV-1a of the modl chunk's bytes (tells the player's copy from another)
};

/// The first (high-detail) model of this name in the file, false when the file or the model is missing.
bool LoadModel(const std::string& path, const char* modelName, Model& model);

struct Texture
{
    int width = 0, height = 0;
    std::vector<unsigned> pixels; // 0xAARRGGBB, rows from the top
    unsigned hash = 0;            // FNV-1a of the stored level's bytes (tells the player's copy from another)
};

/// The top level of the texture's best uncompressed copy (Direct3D A8R8G8B8, R5G6B5 or A4R4G4B4; the DXT copies are
/// not decoded), expanded to 8 bits per channel by integers (tools/lvlgeo.py load_texture: the same values).
bool LoadTexture(const std::string& path, const char* textureName, Texture& texture);

} // namespace LvlFile

} // namespace SwbfVr
