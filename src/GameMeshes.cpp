// GameMeshes: our game-derived meshes rebuilt from the player's game models (GameMeshes.h). The arithmetic is the same
// as tools/meshpatch.py's replay (double, then float), which checks it gives our tables within 1e-5.
#include "GameMeshes.h"

#include "LvlFile.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace SwbfVr
{

namespace GameMeshes
{

namespace
{

/// One table: its game model, our similarity (game model space -> the table's space: x' = M x + t, M row-major),
/// and per vertex a triangle of the model and ten values (b1, b2, the offset and the normal in the triangle's frame,
/// the uv difference).
struct MeshPatch
{
    const char* table;
    const char* lvl;
    const char* model;
    unsigned hash; // the game model the offsets were made against (FNV-1a of its modl chunk)
    int flipV;     // the table's v runs the other way from the level file's
    float transform[12];
    int count;
    const int* triangles;
    const float* values;
};

#if __has_include("MeshPatches.inc")
#include "MeshPatches.inc"
#else
constexpr MeshPatch MESH_PATCHES[1] = {{"", "", "", 0, 0, {}, 0, nullptr, nullptr}};
#endif

struct Registered
{
    const char* table;
    void* vertices;
    size_t stride;
    int count;
};

std::vector<Registered>& Tables()
{
    static std::vector<Registered> tables; // a function's static: built on first use, whatever the initialiser order
    return tables;
}

struct V3
{
    double x, y, z;
};

V3 Sub(const V3& a, const V3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

V3 Cross(const V3& a, const V3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

V3 Unit(const V3& a)
{
    const double length = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return {a.x / length, a.y / length, a.z / length};
}

/// The model's vertices after our transform, its uvs and its triangles, over all segments in order.
struct Flat
{
    std::vector<V3> positions;
    std::vector<double> u, v;
    std::vector<int> triangles;
};

Flat Flatten(const LvlFile::Model& model, const float* transform)
{
    double m[12];
    for (int k = 0; k < 12; ++k)
    {
        m[k] = transform[k];
    }
    Flat flat;
    for (const LvlFile::Segment& segment : model.segments)
    {
        const int base = static_cast<int>(flat.positions.size());
        for (size_t k = 0; k < segment.positions.size(); ++k)
        {
            const LvlFile::Point& p = segment.positions[k];
            flat.positions.push_back(
                {m[0] * p.x + m[1] * p.y + m[2] * p.z + m[9],
                 m[3] * p.x + m[4] * p.y + m[5] * p.z + m[10],
                 m[6] * p.x + m[7] * p.y + m[8] * p.z + m[11]});
            flat.u.push_back(segment.u[k]);
            flat.v.push_back(segment.v[k]);
        }
        for (int index : segment.triangles)
        {
            flat.triangles.push_back(base + index);
        }
    }
    return flat;
}

/// Writes the patch's vertices (eight floats at the start of every `stride` bytes); false when the model does not
/// fit it.
bool Rebuild(const MeshPatch& patch, const Flat& flat, unsigned char* out, size_t stride)
{
    const int triangleCount = static_cast<int>(flat.triangles.size() / 3);
    for (int vertex = 0; vertex < patch.count; ++vertex)
    {
        if (patch.triangles[vertex] < 0 || patch.triangles[vertex] >= triangleCount)
        {
            return false;
        }
    }
    for (int vertex = 0; vertex < patch.count; ++vertex)
    {
        const int* corner = &flat.triangles[static_cast<size_t>(patch.triangles[vertex]) * 3];
        const V3& a0 = flat.positions[corner[0]];
        const V3 e1 = Sub(flat.positions[corner[1]], a0);
        const V3 f2 = Sub(flat.positions[corner[2]], a0);
        const V3 axisX = Unit(e1);
        const V3 axisZ = Unit(Cross(e1, f2));
        const V3 axisY = Cross(axisZ, axisX);
        const float* value = patch.values + static_cast<size_t>(vertex) * 10;
        const double b1 = value[0], b2 = value[1];
        const V3 base = {a0.x + b1 * e1.x + b2 * f2.x, a0.y + b1 * e1.y + b2 * f2.y, a0.z + b1 * e1.z + b2 * f2.z};
        // a vector given in the triangle's frame, added to `origin`
        auto inFrame = [&](const float* c, const V3& origin) -> V3
        {
            const double x = c[0], y = c[1], z = c[2];
            return {
                origin.x + x * axisX.x + y * axisY.x + z * axisZ.x,
                origin.y + x * axisX.y + y * axisY.y + z * axisZ.y,
                origin.z + x * axisX.z + y * axisY.z + z * axisZ.z};
        };
        const V3 position = inFrame(value + 2, base);
        const V3 normal = inFrame(value + 5, {0, 0, 0});
        const double u0 = flat.u[corner[0]], v0 = flat.v[corner[0]];
        const double u = u0 + b1 * (flat.u[corner[1]] - u0) + b2 * (flat.u[corner[2]] - u0);
        double v = v0 + b1 * (flat.v[corner[1]] - v0) + b2 * (flat.v[corner[2]] - v0);
        if (patch.flipV)
        {
            v = 1 - v;
        }
        const float result[8] = {
            static_cast<float>(position.x),
            static_cast<float>(position.y),
            static_cast<float>(position.z),
            static_cast<float>(normal.x),
            static_cast<float>(normal.y),
            static_cast<float>(normal.z),
            static_cast<float>(u + value[8]),
            static_cast<float>(v + value[9])};
        std::memcpy(out + static_cast<size_t>(vertex) * stride, result, sizeof(result));
    }
    return true;
}

} // namespace

bool Register(const char* table, void* vertices, size_t stride, int count)
{
    Tables().push_back({table, vertices, stride, count});
    return true;
}

std::string Build(const std::string& lvlFolder)
{
    std::map<std::string, LvlFile::Model> models; // by level file and model name: each read once
    int made = 0, notRegistered = 0, missing = 0, different = 0;
    for (const MeshPatch& patch : MESH_PATCHES)
    {
        if (!patch.table[0])
        {
            continue;
        }
        const Registered* target = nullptr;
        for (const Registered& table : Tables())
        {
            if (!std::strcmp(table.table, patch.table) && table.count == patch.count)
            {
                target = &table;
            }
        }
        if (!target)
        {
            ++notRegistered;
            continue;
        }
        const std::string key = std::string(patch.lvl) + "|" + patch.model;
        auto found = models.find(key);
        if (found == models.end())
        {
            LvlFile::Model model;
            if (!LvlFile::LoadModel(lvlFolder + patch.lvl, patch.model, model))
            {
                model.segments.clear();
            }
            found = models.emplace(key, std::move(model)).first;
        }
        if (found->second.segments.empty())
        {
            ++missing;
        }
        else if (
            found->second.hash != patch.hash || !Rebuild(
                                                    patch,
                                                    Flatten(found->second, patch.transform),
                                                    static_cast<unsigned char*>(target->vertices),
                                                    target->stride))
        {
            ++different;
        }
        else
        {
            ++made;
        }
    }
    char summary[200];
    std::snprintf(
        summary,
        sizeof(summary),
        "%d meshes made from the game's models, %d missing, %d from a different game model (left out), %d not in "
        "this build",
        made,
        missing,
        different,
        notRegistered);
    return summary;
}

} // namespace GameMeshes

} // namespace SwbfVr
