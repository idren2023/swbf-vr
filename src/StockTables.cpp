// StockTables: the game's model pieces our shells use, built at start from the player's level files (StockTables.h).
// Each part follows its Python original line for line (tools/stockexterior.py, tools/stockcockpit.py,
// tools/mirrorstock.py), in double precision as Python computes, so the tables match theirs (tools/inccompare.py).
#include "StockTables.h"

#include "LvlFile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>

namespace SwbfVr
{

namespace StockTables
{

namespace
{

using LvlFile::Point;

/// One run of triangles of a level-file segment and the .msh node they came from (tools/lvlmatch.py).
struct LabelRun
{
    int segment;
    const char* node;
    int count;
};

struct ModelLabels
{
    const char* mshPath; // the generators' key: the model's path under the mod tools' Sides folder
    const char* lvlPath; // under _lvl_pc
    const char* model;
    double shift[3]; // the root translation the munger dropped
    const LabelRun* runs;
    int runCount;
};

#if __has_include("LvlLabels.inc")
#include "LvlLabels.inc"
#else
constexpr LabelRun LABEL_RUNS_NONE[1] = {};
constexpr ModelLabels MODEL_LABELS[1] = {{"", "", "", {0, 0, 0}, LABEL_RUNS_NONE, 0}};
#endif

// --- small vector helpers (double precision) ---

Point Add(const Point& a, const Point& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Point Subtract(const Point& a, const Point& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Point Scale(const Point& a, double factor)
{
    return {a.x * factor, a.y * factor, a.z * factor};
}

double Dot(const Point& a, const Point& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Point Cross(const Point& a, const Point& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

double Component(const Point& p, int axis)
{
    return axis == 0 ? p.x : axis == 1 ? p.y : p.z;
}

Point Centre(const Point* points)
{
    return Scale(Add(Add(points[0], points[1]), points[2]), 1.0 / 3.0);
}

std::string Lower(std::string text)
{
    for (char& character : text)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

bool StartsWith(const std::string& text, const char* prefix)
{
    return text.compare(0, std::strlen(prefix), prefix) == 0;
}

bool Contains(const std::string& text, const char* part)
{
    return text.find(part) != std::string::npos;
}

// --- the game's model as the generators saw the .msh: nodes with their segments (tools/modelsource.py) ---

struct NodeSegment
{
    int source; // the level-file segment it came from
    std::string texture; // with .tga, as the .msh names it
    std::vector<Point> positions, normals;
    std::vector<double> u, v; // v as the .msh's (the level file's turned over)
    std::vector<int> triangles;
};

struct Node
{
    std::string name;
    std::vector<NodeSegment> segments;
};

/// mshgeo.is_drawn: not collision, hardpoints, shadow volumes or the low-detail copy.
bool IsDrawn(const std::string& name)
{
    const std::string lower = Lower(name);
    return !StartsWith(lower, "collision") && !StartsWith(lower, "hp_") && !StartsWith(lower, "p_") &&
           !StartsWith(lower, "shadowvolume") && !Contains(lower, "lowrez") && !Contains(lower, "lowres");
}

/// The model the .msh at `mshPath` names, from the level files, relabelled into the .msh's nodes; false without it.
bool LoadNodes(const std::string& lvlFolder, const char* mshPath, std::vector<Node>& nodes)
{
    nodes.clear();
    const ModelLabels* labels = nullptr;
    for (const ModelLabels& entry : MODEL_LABELS)
    {
        if (std::strcmp(entry.mshPath, mshPath) == 0)
        {
            labels = &entry;
        }
    }
    LvlFile::Model model;
    if (!labels || !LvlFile::LoadModel(lvlFolder + labels->lvlPath, labels->model, model))
    {
        return false;
    }
    const Point shift = {labels->shift[0], labels->shift[1], labels->shift[2]};
    std::vector<int> nextTriangle(model.segments.size(), 0);
    for (int runIndex = 0; runIndex < labels->runCount; ++runIndex)
    {
        const LabelRun& run = labels->runs[runIndex];
        if (run.segment < 0 || run.segment >= static_cast<int>(model.segments.size()))
        {
            return false;
        }
        const LvlFile::Segment& segment = model.segments[run.segment];
        Node* node = nullptr;
        for (Node& existing : nodes)
        {
            if (existing.name == run.node)
            {
                node = &existing;
            }
        }
        if (!node)
        {
            nodes.push_back(Node{run.node, {}});
            node = &nodes.back();
        }
        if (node->segments.empty() || node->segments.back().source != run.segment)
        {
            NodeSegment piece;
            piece.source = run.segment;
            piece.texture = segment.texture.empty() ? std::string() : segment.texture + ".tga";
            node->segments.push_back(piece);
        }
        NodeSegment& piece = node->segments.back();
        const int first = nextTriangle[run.segment];
        if ((first + run.count) * 3 > static_cast<int>(segment.triangles.size()))
        {
            return false; // the labels do not fit this file (another build of the game?)
        }
        piece.triangles.insert(
            piece.triangles.end(), segment.triangles.begin() + first * 3,
            segment.triangles.begin() + (first + run.count) * 3);
        nextTriangle[run.segment] = first + run.count;
    }
    // each node's segments keep only the vertices their triangles use, numbered in order of first use
    for (Node& node : nodes)
    {
        for (NodeSegment& piece : node.segments)
        {
            const LvlFile::Segment& segment = model.segments[piece.source];
            std::map<int, int> remap;
            std::vector<int> keep;
            for (int& vertex : piece.triangles)
            {
                auto found = remap.find(vertex);
                if (found == remap.end())
                {
                    found = remap.emplace(vertex, static_cast<int>(keep.size())).first;
                    keep.push_back(vertex);
                }
                vertex = found->second;
            }
            for (int vertex : keep)
            {
                piece.positions.push_back(Add(segment.positions[vertex], shift));
                piece.normals.push_back(segment.normals[vertex]);
                piece.u.push_back(segment.u[vertex]);
                piece.v.push_back(1.0 - segment.v[vertex]);
            }
        }
    }
    return true;
}

const Node* FindNode(const std::vector<Node>& nodes, const char* name)
{
    const std::string lower = Lower(name);
    const Node* found = nullptr;
    for (const Node& node : nodes)
    {
        if (Lower(node.name) == lower)
        {
            found = &node; // the last of that name, as Python's dict
        }
    }
    return found;
}

// --- output ---

struct OutTriangle
{
    Point positions[3];
    Point normals[3];
    double u[3], v[3];
    int kind;
};

std::map<std::string, std::vector<StockVertex>> _tables;
std::vector<ShellKit::Vec3> _speederMask;

/// Python writes %.4f positions and normals and %.5f texture coordinates: rounded the same so the tables agree.
float Round(double value, double step)
{
    return static_cast<float>(std::round(value / step) * step);
}

void Store(const std::string& name, const std::vector<OutTriangle>& triangles)
{
    std::vector<StockVertex>& table = _tables[name];
    table.clear();
    for (const OutTriangle& triangle : triangles)
    {
        for (int corner = 0; corner < 3; ++corner)
        {
            const Point& p = triangle.positions[corner];
            const Point& n = triangle.normals[corner];
            table.push_back(StockVertex{
                {Round(p.x, 1e-4), Round(p.y, 1e-4), Round(p.z, 1e-4)},
                {Round(n.x, 1e-4), Round(n.y, 1e-4), Round(n.z, 1e-4)},
                Round(triangle.u[corner], 1e-5),
                Round(triangle.v[corner], 1e-5),
                triangle.kind});
        }
    }
}

// ===================================================================================================================
// tools/stockexterior.py: a ship's exterior round its cockpit
// ===================================================================================================================

const Point COCKPIT_EYE = {0.0, 1.5, 1.5};
constexpr double DROID_CAP_Y = 1.47;

enum class RegionKind
{
    Facing, // the box: faces in it turned toward the eye left out
    All, // the box: everything in it left out
    Only, // the box: only what is in it kept (a moving part's own table)
    Sphere, // the ball: everything in it left out
    None, // nothing left out (a zero box)
};

struct Region
{
    RegionKind kind;
    Point low, high; // box
    Point centre; // sphere
    double radius;
};

struct Ship
{
    const char* table;
    const char* msh;
    Point eye;
    const char* texture;
    const char* glass[2];
    Region region;
};

const Point ATTE_BARREL_LOW = {-0.35, 7.05, 4.70}, ATTE_BARREL_HIGH = {0.35, 7.90, 14.10};

const Ship SHIPS[] = {
    {"XWING", "ALL/MSH/all_fly_xwing.msh", {0.0, 2.52, 1.30}, "all_fly_xwing.tga", {"all_fly_ywing_windscreen.tga"},
     {RegionKind::Facing, {-0.53, 1.20, 0.30}, {0.53, 2.80, 2.80}, {}, 0}},
    {"TIE", "IMP/MSH/imp_fly_tiefighter.msh", {0.0, 4.25, 0.00}, "imp_fly_tiefighter.tga", {},
     {RegionKind::Sphere, {}, {}, {0.0, 4.188, 0.0}, 1.45}},
    {"JEDI", "REP/MSH/rep_fly_jedifighter.msh", {0.0, 1.72, -1.35}, "rep_fly_fighter.tga", {"rep_fly_fighter_canopy.tga"},
     {RegionKind::Facing, {-0.62, 0.30, -2.70}, {0.62, 1.40, -0.20}, {}, 0}},
    {"DROID", "CIS/MSH/cis_fly_droidfighter.msh", {0.0, 2.55, 1.20}, "cis_fly_droidstarfighter.tga", {},
     {RegionKind::None, {0, 0, 0}, {0, 0, 0}, {}, 0}},
    {"GEO", "CIS/MSH/cis_fly_geofighter.msh", {0.0, 2.95, -0.25}, "cis_fly_fighter.tga", {"cis_fly_fighter_canopy.tga"},
     {RegionKind::All, {-0.58, 1.50, -1.40}, {0.58, 2.66, 0.80}, {}, 0}},
    {"YWING", "ALL/MSH/all_fly_ywing.msh", {0.0, 2.10, 5.15}, "all_fly_ywing.tga", {"all_fly_ywing_windscreen.tga"},
     {RegionKind::Facing, {-0.53, 1.00, 3.60}, {0.53, 2.43, 7.05}, {}, 0}},
    {"TIEBOMBER", "IMP/MSH/imp_fly_tiebomber.msh", {-1.75, 2.74, 1.45}, "imp_fly_tiebomber.tga", {},
     {RegionKind::All, {-2.85, 1.55, -3.25}, {-0.62, 3.85, 2.75}, {}, 0}},
    {"SNOWSPEEDER", "ALL/MSH/all_fly_snowspeeder.msh", {0.0, 1.15, 0.25}, "all_fly_speeder.tga",
     {"all_fly_snowspeeder_windscreen.tga"},
     {RegionKind::Facing, {-0.56, 0.15, -1.66}, {0.56, 1.40, 1.02}, {}, 0}},
    {"ATTE_CAB", "REP/MSH/rep_walk_atte.msh", {0.0, 5.60, 7.30}, "Rep_walk_sixleg.tga", {},
     {RegionKind::All, {-0.80, 3.60, 3.00}, {0.80, 6.70, 8.95}, {}, 0}},
    {"ATTE_CANNON", "REP/MSH/rep_walk_atte.msh", {0.0, 8.60, 1.72}, "Rep_walk_sixleg.tga", {},
     {RegionKind::All, {-1.00, 6.70, 0.60}, {1.00, 9.20, 13.30}, {}, 0}},
    {"ATTE_GUN", "REP/MSH/rep_walk_atte.msh", {0.0, 8.60, 1.72}, "Rep_walk_sixleg.tga", {},
     {RegionKind::Only, {-1.00, 6.70, 0.60}, {1.00, 9.20, 13.30}, {}, 0}},
    {"ATTE_BARREL", "REP/MSH/rep_walk_atte.msh", {0.0, 8.60, 1.72}, "Rep_walk_sixleg.tga", {},
     {RegionKind::Only, ATTE_BARREL_LOW, ATTE_BARREL_HIGH, {}, 0}},
    {"GUNSHIP_BALL2", "REP/MSH/REP_LA_AT_Gunship.msh", {-2.80, 1.06, 2.45}, "REP_LA_AT.tga", {},
     {RegionKind::None, {0, 0, 0}, {0, 0, 0}, {}, 0}},
    {"GUNSHIP_BALL3", "REP/MSH/REP_LA_AT_Gunship.msh", {2.93, 1.06, 2.45}, "REP_LA_AT.tga", {},
     {RegionKind::None, {0, 0, 0}, {0, 0, 0}, {}, 0}},
    {"GUNSHIP_DOOR", "REP/MSH/REP_LA_AT_Gunship.msh", {-0.97, 1.75, -0.19}, "REP_LA_AT.tga", {},
     {RegionKind::None, {0, 0, 0}, {0, 0, 0}, {}, 0}},
};

struct Box
{
    Point low, high;
};

/// Hull faces inside an ellipsoid also left out (the Geonosian's own pilot and dash under its bubble).
struct Bubble
{
    const char* table;
    Point centre, radii;
};

const Bubble BUBBLES[] = {{"GEO", {0.0, 2.67, -0.315}, {0.55, 0.46, 0.99}}};

/// Hull faces with their middles in a box also left out (stockexterior.py CUT_BOXES says why for each).
struct CutBox
{
    const char* table;
    Box box;
};

const CutBox CUT_BOXES[] = {
    {"JEDI", {{-0.5, 1.22, -1.10}, {0.5, 1.55, -0.70}}},
    {"SNOWSPEEDER", {{-0.56, 0.15, -1.66}, {0.56, 1.03, 1.02}}},
    {"YWING", {{-0.40, 2.00, 3.70}, {0.40, 2.16, 4.15}}},
    {"ATTE_GUN", {{-0.5, 7.7, 1.85}, {0.5, 8.5, 2.4}}},
    {"ATTE_GUN", {ATTE_BARREL_LOW, ATTE_BARREL_HIGH}},
};

/// Nodes kept whole and drawn in the design's cap finish (kind 5): the Y-wing's canopy inside.
struct KeptNode
{
    const char* table;
    const char* node;
};

const KeptNode KEPT_NODES[] = {{"YWING", "cockpit_interior"}};

bool StrictlyInside(const Point& p, const Point& low, const Point& high)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        const double value = Component(p, axis);
        if (!(Component(low, axis) < value && value < Component(high, axis)))
        {
            return false;
        }
    }
    return true;
}

/// A corner of a hull triangle: position, texture coordinates (v turned top-down), normal.
struct Corner
{
    Point position;
    double u, v;
    Point normal;
};

using Piece = std::vector<Corner>; // three corners

Point PieceCentre(const Piece& piece)
{
    const Point points[3] = {piece[0].position, piece[1].position, piece[2].position};
    return Centre(points);
}

bool InCutBox(const Piece& piece, const char* table)
{
    const Point c = PieceCentre(piece);
    for (const CutBox& cut : CUT_BOXES)
    {
        if (std::strcmp(cut.table, table) == 0 && StrictlyInside(c, cut.box.low, cut.box.high))
        {
            return true;
        }
    }
    return false;
}

bool InBubble(const Piece& piece, const char* table)
{
    for (const Bubble& bubble : BUBBLES)
    {
        if (std::strcmp(bubble.table, table) != 0)
        {
            continue;
        }
        const Point c = PieceCentre(piece);
        double sum = 0;
        for (int axis = 0; axis < 3; ++axis)
        {
            const double d = (Component(c, axis) - Component(bubble.centre, axis)) / Component(bubble.radii, axis);
            sum += d * d;
        }
        return c.y > bubble.centre.y - 0.03 && sum < 1;
    }
    return false;
}

Point ToCockpit(const Point& p, const Point& eye)
{
    return {-(p.x - eye.x) + COCKPIT_EYE.x, p.y - eye.y + COCKPIT_EYE.y, -(p.z - eye.z) + COCKPIT_EYE.z};
}

Corner Middle(const Corner& a, const Corner& b)
{
    return {Scale(Add(a.position, b.position), 0.5), (a.u + b.u) / 2, (a.v + b.v) / 2, Scale(Add(a.normal, b.normal), 0.5)};
}

double Distance(const Point& a, const Point& b)
{
    const Point d = Subtract(a, b);
    return std::sqrt(Dot(d, d));
}

/// The triangle as it is, or, where it crosses the edge of the region left out, cut into pieces no longer than
/// `longest` (their middles then decide), corners interpolated.
void Pieces(const Piece& corners, const Region& region, std::vector<Piece>& out, double longest = 0.15)
{
    const Point ps[3] = {corners[0].position, corners[1].position, corners[2].position};
    if (region.kind == RegionKind::Sphere)
    {
        Point nearest = {};
        double* n = &nearest.x;
        for (int axis = 0; axis < 3; ++axis)
        {
            const double low = std::min({Component(ps[0], axis), Component(ps[1], axis), Component(ps[2], axis)});
            const double high = std::max({Component(ps[0], axis), Component(ps[1], axis), Component(ps[2], axis)});
            n[axis] = std::min(std::max(Component(region.centre, axis), low), high);
        }
        const Point d = Subtract(nearest, region.centre);
        if (Dot(d, d) > region.radius * region.radius)
        {
            out.push_back(corners);
            return;
        }
        bool allInside = true;
        for (const Point& p : ps)
        {
            const Point e = Subtract(p, region.centre);
            allInside = allInside && Dot(e, e) < region.radius * region.radius;
        }
        if (allInside)
        {
            out.push_back(corners);
            return;
        }
    }
    else
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const double low = std::min({Component(ps[0], axis), Component(ps[1], axis), Component(ps[2], axis)});
            const double high = std::max({Component(ps[0], axis), Component(ps[1], axis), Component(ps[2], axis)});
            if (high < Component(region.low, axis) || low > Component(region.high, axis))
            {
                out.push_back(corners);
                return;
            }
        }
        if (StrictlyInside(ps[0], region.low, region.high) && StrictlyInside(ps[1], region.low, region.high) &&
            StrictlyInside(ps[2], region.low, region.high))
        {
            out.push_back(corners);
            return;
        }
    }
    const double edge =
        std::max({Distance(ps[0], ps[1]), Distance(ps[1], ps[2]), Distance(ps[2], ps[0])});
    if (edge <= longest)
    {
        out.push_back(corners);
        return;
    }
    const Corner& a = corners[0];
    const Corner& b = corners[1];
    const Corner& c = corners[2];
    const Corner ab = Middle(a, b), bc = Middle(b, c), ca = Middle(c, a);
    Pieces({a, ab, ca}, region, out, longest);
    Pieces({ab, b, bc}, region, out, longest);
    Pieces({ca, bc, c}, region, out, longest);
    Pieces({ab, bc, ca}, region, out, longest);
}

/// Whether a piece lies in the exterior's own cockpit: in the ball, or in the box facing the eye.
bool Dropped(const Piece& piece, const Region& region, const Point& eye, int kind)
{
    const Point c = PieceCentre(piece);
    if (region.kind == RegionKind::Sphere)
    {
        const Point d = Subtract(c, region.centre);
        return Dot(d, d) < region.radius * region.radius;
    }
    if (kind != 3 || !StrictlyInside(c, region.low, region.high))
    {
        return false;
    }
    if (region.kind == RegionKind::All)
    {
        return true;
    }
    const Point n = Add(Add(piece[0].normal, piece[1].normal), piece[2].normal);
    return Dot(n, Subtract(eye, c)) > 0;
}

bool IsKeptNode(const char* table, const std::string& node)
{
    for (const KeptNode& kept : KEPT_NODES)
    {
        if (std::strcmp(kept.table, table) == 0 && Lower(node) == kept.node)
        {
            return true;
        }
    }
    return false;
}

void ShipTriangles(const std::string& lvlFolder, const Ship& ship, std::vector<OutTriangle>& out)
{
    std::vector<Node> nodes;
    if (!LoadNodes(lvlFolder, ship.msh, nodes))
    {
        return;
    }
    const std::string texture = Lower(ship.texture);
    for (const Node& node : nodes)
    {
        if (!IsDrawn(node.name))
        {
            continue;
        }
        for (const NodeSegment& segment : node.segments)
        {
            const std::string name = Lower(segment.texture);
            int kind = name == texture ? 3 : 0;
            for (const char* glass : ship.glass)
            {
                if (kind == 0 && glass && name == Lower(glass))
                {
                    kind = 4;
                }
            }
            if (!kind || segment.positions.empty())
            {
                continue;
            }
            const auto& p = segment.positions;
            const auto& n = segment.normals;
            if (kind == 3 && IsKeptNode(ship.table, node.name))
            {
                for (size_t t = 0; t + 2 < segment.triangles.size(); t += 3)
                {
                    const int* tri = &segment.triangles[t];
                    Point ps[3];
                    for (int k = 0; k < 3; ++k)
                    {
                        ps[k] = ToCockpit(p[tri[k]], ship.eye);
                    }
                    const Point normal = Cross(Subtract(ps[1], ps[0]), Subtract(ps[2], ps[0]));
                    double length = std::sqrt(Dot(normal, normal));
                    length = length != 0 ? length : 1;
                    if (std::fabs(normal.y) / length > 0.9 && std::max({ps[0].y, ps[1].y, ps[2].y}) < 1.60)
                    {
                        continue; // its floor
                    }
                    OutTriangle triangle = {};
                    for (int k = 0; k < 3; ++k)
                    {
                        triangle.positions[k] = ps[k];
                        triangle.normals[k] = {-n[tri[k]].x, n[tri[k]].y, -n[tri[k]].z};
                    }
                    triangle.kind = 5;
                    out.push_back(triangle);
                }
                continue;
            }
            for (size_t t = 0; t + 2 < segment.triangles.size(); t += 3)
            {
                const int* tri = &segment.triangles[t];
                if (std::strcmp(ship.table, "DROID") == 0 && segment.u[tri[0]] > 0.88 && segment.v[tri[0]] < 0.50 &&
                    segment.u[tri[1]] > 0.88 && segment.v[tri[1]] < 0.50 && segment.u[tri[2]] > 0.88 &&
                    segment.v[tri[2]] < 0.50)
                {
                    // the droid's head: a flat cap where it sat, kind 5
                    OutTriangle triangle = {};
                    for (int k = 0; k < 3; ++k)
                    {
                        triangle.positions[k] = ToCockpit({p[tri[k]].x, DROID_CAP_Y, p[tri[k]].z}, ship.eye);
                        triangle.normals[k] = {0.0, 1.0, 0.0};
                    }
                    triangle.kind = 5;
                    out.push_back(triangle);
                    continue;
                }
                Piece corners;
                for (int k = 0; k < 3; ++k)
                {
                    corners.push_back(Corner{p[tri[k]], segment.u[tri[k]], 1 - segment.v[tri[k]], n[tri[k]]});
                }
                auto emit = [&](const Piece& piece)
                {
                    OutTriangle triangle = {};
                    for (int k = 0; k < 3; ++k)
                    {
                        triangle.positions[k] = ToCockpit(piece[k].position, ship.eye);
                        triangle.normals[k] = {-piece[k].normal.x, piece[k].normal.y, -piece[k].normal.z};
                        triangle.u[k] = piece[k].u;
                        triangle.v[k] = piece[k].v;
                    }
                    triangle.kind = kind;
                    out.push_back(triangle);
                };
                if (ship.region.kind == RegionKind::Only)
                {
                    const Point c = PieceCentre(corners);
                    if (kind == 3 && StrictlyInside(c, ship.region.low, ship.region.high) &&
                        !InCutBox(corners, ship.table))
                    {
                        emit(corners);
                    }
                    continue;
                }
                std::vector<Piece> split;
                Pieces(corners, ship.region, split);
                std::vector<Piece> kept;
                for (const Piece& piece : split)
                {
                    if (!Dropped(piece, ship.region, ship.eye, kind) &&
                        !(kind == 3 && (InBubble(piece, ship.table) || InCutBox(piece, ship.table))))
                    {
                        kept.push_back(piece);
                    }
                }
                if (kept.size() == split.size())
                {
                    emit(corners);
                }
                else
                {
                    for (const Piece& piece : kept)
                    {
                        emit(piece);
                    }
                }
            }
        }
    }
}

/// Leaves out triangles lying wholly on another of the same kind in the same plane (within 4 mm); of a pair, the one
/// facing the eye is kept.
std::vector<OutTriangle> WithoutTwins(const std::vector<OutTriangle>& triangles, const Point& eye = COCKPIT_EYE)
{
    struct Info
    {
        Point normal;
        bool hasNormal;
        Point centre;
        bool isFacing;
    };
    std::vector<Info> info;
    for (const OutTriangle& triangle : triangles)
    {
        const Point* ps = triangle.positions;
        Point normal = Cross(Subtract(ps[1], ps[0]), Subtract(ps[2], ps[0]));
        const double length = std::sqrt(Dot(normal, normal));
        const Point centre = Centre(ps);
        const bool hasNormal = length > 1e-9;
        if (hasNormal)
        {
            normal = Scale(normal, 1.0 / length);
        }
        info.push_back({normal, hasNormal, centre, hasNormal && Dot(normal, Subtract(eye, centre)) > 0});
    }
    std::vector<int> order(triangles.size());
    for (size_t index = 0; index < order.size(); ++index)
    {
        order[index] = static_cast<int>(index);
    }
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return info[a].isFacing && !info[b].isFacing; });
    auto covers = [&](int a, int b) -> bool
    {
        if (!info[a].hasNormal || !info[b].hasNormal || triangles[a].kind != triangles[b].kind ||
            std::fabs(Dot(info[a].normal, info[b].normal)) < 0.995)
        {
            return false;
        }
        const Point* ta = triangles[a].positions;
        const Point& na = info[a].normal;
        for (const Point& p : triangles[b].positions)
        {
            if (std::fabs(Dot(Subtract(p, ta[0]), na)) > 0.004)
            {
                return false;
            }
            for (int k = 0; k < 3; ++k)
            {
                const Point& e0 = ta[k];
                const Point& e1 = ta[(k + 1) % 3];
                const Point edge = Subtract(e1, e0);
                if (Dot(Cross(edge, Subtract(p, e0)), na) < -0.004 * std::sqrt(Dot(edge, edge)))
                {
                    return false;
                }
            }
        }
        return true;
    };
    std::map<std::tuple<int, int, int>, std::vector<int>> cells;
    std::vector<int> kept;
    for (int index : order)
    {
        const Point& c = info[index].centre;
        const int cx = static_cast<int>(std::floor(c.x / 0.5)), cy = static_cast<int>(std::floor(c.y / 0.5)),
                  cz = static_cast<int>(std::floor(c.z / 0.5));
        bool isCovered = false;
        for (int dx = -1; dx <= 1 && !isCovered; ++dx)
        {
            for (int dy = -1; dy <= 1 && !isCovered; ++dy)
            {
                for (int dz = -1; dz <= 1 && !isCovered; ++dz)
                {
                    auto found = cells.find({cx + dx, cy + dy, cz + dz});
                    if (found == cells.end())
                    {
                        continue;
                    }
                    for (int other : found->second)
                    {
                        if (covers(other, index))
                        {
                            isCovered = true;
                            break;
                        }
                    }
                }
            }
        }
        if (isCovered)
        {
            continue;
        }
        cells[{cx, cy, cz}].push_back(index);
        kept.push_back(index);
    }
    std::sort(kept.begin(), kept.end());
    std::vector<OutTriangle> result;
    for (int index : kept)
    {
        result.push_back(triangles[index]);
    }
    return result;
}

// ===================================================================================================================
// tools/stockcockpit.py: pieces of the game's first-person cockpits fitted into ours
// ===================================================================================================================

const Point EYE = {0.0, 1.5, 1.5};

enum class Move
{
    Scale, // about the eye by `factor`, then moved by `offset`
    Fit, // the fit nodes' box onto [targetLow, targetHigh] per axis
    Mirror, // cut at x = plane and mirrored about it (the walkers' cabs)
};

struct Part
{
    std::vector<const char*> nodes;
    Move move;
    double factor;
    Point offset;
    std::vector<const char*> fitNodes;
    Point targetLow, targetHigh;
    double plane;
    std::vector<const char*> keptOnce; // mirror: drawn whole, not mirrored
    std::vector<const char*> spanned; // mirror: a window quad spanned across both sides
};

struct CockpitShip
{
    const char* table;
    const char* msh;
    std::vector<Part> parts;
};

Part ScalePart(std::vector<const char*> nodes, double factor, Point offset)
{
    Part part = {};
    part.nodes = std::move(nodes);
    part.move = Move::Scale;
    part.factor = factor;
    part.offset = offset;
    return part;
}

const std::vector<const char*> XWING_GAUGES = {"radar1", "tach", "amp", "tubelong"};

std::vector<CockpitShip> CockpitShips()
{
    Part jedi = {};
    jedi.nodes = {"display1", "displayholder2", "holder1", "holder2", "holder3"};
    jedi.move = Move::Fit;
    jedi.fitNodes = {"displayholder2"};
    jedi.targetLow = {-0.11, 1.06, 0.83};
    jedi.targetHigh = {0.11, 1.36, 0.88};
    Part atat = {};
    atat.nodes = {"ATATdriver", "centerconsolescreen", "window", "panel2", "panel7", "illumination14", "panel9",
                  "panel8", "shiftcase", "illumination10", "illumination11", "illumination13", "panel11", "panel10",
                  "windowframe", "shell", "button1", "headstringertop", "illumination7", "centerconsole",
                  "dashstringer"};
    atat.move = Move::Mirror;
    atat.plane = 0.91;
    atat.keptOnce = {"centerconsole", "centerconsolescreen", "illumination13", "illumination14"};
    atat.spanned = {"window"};
    Part atst = {};
    atst.nodes = {"imp_1st_cockpit_ATST1", "longhose", "shock", "box3", "box1", "smltube2", "smltube3",
                  "centerconsole1", "box4", "smltube1", "dash", "windoor", "fx1", "centerconsole", "fx2", "fx3",
                  "ACvent"};
    atst.move = Move::Mirror;
    atst.plane = 1.30;
    return {
        {"XWING", "ALL/MSH/all_1st_cockpit_xwing.msh", {ScalePart(XWING_GAUGES, 0.45, {0.0, -0.02, 0.0})}},
        {"YWING", "ALL/MSH/all_1st_cockpit_ywing.msh", {ScalePart(XWING_GAUGES, 0.45, {0.0, -0.04, 0.0})}},
        {"TIE",
         "IMP/MSH/imp_1st_cockpit_tiefighter.msh",
         {ScalePart(
             {"dash", "target", "map", "map1", "dashfx", "dashfx2", "dashfx4", "dashfx5", "dashfx6", "dashfx7",
              "dashfx8"},
             0.55, {0.0, -0.45, 0.19})}},
        {"TIEBOMBER",
         "IMP/MSH/imp_1st_cockpit_tiebomber.msh",
         {ScalePart({"dash", "target", "dashfx", "dashfx1"}, 0.55, {0.0, -0.45, 0.19})}},
        {"JEDI", "REP/MSH/rep_1st_cockpit_jedistarfighter.msh", {jedi}},
        {"DROID",
         "CIS/MSH/cis_1st_cockpit_starfighter.msh",
         {ScalePart(
             {"cis_1st_cockpit_starfighter1", "mapball", "circle1", "circle2", "lightfx1", "lightfx2", "lightfx3",
              "lightfx4", "lightfx5"},
             0.30, {0.0, 0.0, 0.0})}},
        {"SNOW",
         "ALL/MSH/all_1st_cockpit_snowspeeder.msh",
         {ScalePart(
             {"leftside", "rightside", "dashcntr", "cube3", "cube4", "cube7", "cube10", "cube13", "cube16", "cube17",
              "cube18", "cube19", "cube20"},
             0.45, {0.0, 0.0, 0.0})}},
        {"GUNSHIP",
         "REP/MSH/rep_1st_cockpit_gunship.msh",
         {ScalePart({"rep_1st_cockpit_gunship", "frame", "glass"}, 1.0, {0.0, 0.0, 0.0}),
          ScalePart(
              {"scrolscrn1", "upperdash", "frontdial", "knob2", "knob1", "screen", "port2", "port5", "fx4",
               "dashboard", "fx3", "but4", "cylinderdial", "but1", "but3", "but2"},
              1.12, {-0.0000, 0.0684, 0.2088})}},
        {"ATAT", "IMP/MSH/imp_1st_cockpit_atat.msh", {atat}},
        {"ATST", "IMP/MSH/imp_1st_cockpit_ATST.msh", {atst}},
    };
}

int KindOf(const std::string& texture)
{
    const std::string t = Lower(texture);
    if (Contains(t, "win") || Contains(t, "canopy") || Contains(t, "glass"))
    {
        return 4;
    }
    if (Contains(t, "_fx"))
    {
        return 2;
    }
    return t.empty() ? 0 : 1;
}

bool Listed(const std::vector<const char*>& names, const char* name)
{
    for (const char* listed : names)
    {
        if (std::strcmp(listed, name) == 0)
        {
            return true;
        }
    }
    return false;
}

/// A polygon corner while clipping: position, normal, texture coordinates.
struct PolygonCorner
{
    Point position, normal;
    double u, v;
};

/// Sutherland-Hodgman against x = xMinimum, keeping x >= xMinimum (the normal of the corner before an inserted one).
std::vector<PolygonCorner> ClipX(const std::vector<PolygonCorner>& points, double xMinimum)
{
    std::vector<PolygonCorner> out;
    for (size_t index = 0; index < points.size(); ++index)
    {
        const PolygonCorner& p = points[index];
        const PolygonCorner& q = points[(index + 1) % points.size()];
        if (p.position.x >= xMinimum)
        {
            out.push_back(p);
        }
        if ((p.position.x >= xMinimum) != (q.position.x >= xMinimum))
        {
            const double f = (xMinimum - p.position.x) / (q.position.x - p.position.x);
            out.push_back(
                {Add(p.position, Scale(Subtract(q.position, p.position), f)), p.normal, p.u + (q.u - p.u) * f,
                 p.v + (q.v - p.v) * f});
        }
    }
    return out;
}

std::vector<PolygonCorner> ClipBelow(const std::vector<PolygonCorner>& points, double xMaximum)
{
    std::vector<PolygonCorner> flipped = points;
    for (PolygonCorner& corner : flipped)
    {
        corner.position.x = -corner.position.x;
    }
    std::vector<PolygonCorner> out = ClipX(flipped, -xMaximum);
    for (PolygonCorner& corner : out)
    {
        corner.position.x = -corner.position.x;
    }
    return out;
}

void AddFan(const std::vector<PolygonCorner>& polygon, int kind, std::vector<OutTriangle>& out)
{
    for (size_t k = 1; k + 1 < polygon.size(); ++k)
    {
        const PolygonCorner* corners[3] = {&polygon[0], &polygon[k], &polygon[k + 1]};
        OutTriangle triangle = {};
        for (int c = 0; c < 3; ++c)
        {
            triangle.positions[c] = corners[c]->position;
            triangle.normals[c] = corners[c]->normal;
            triangle.u[c] = corners[c]->u;
            triangle.v[c] = corners[c]->v;
        }
        triangle.kind = kind;
        out.push_back(triangle);
    }
}

void MirrorPieces(const std::vector<Node>& nodes, const Part& part, std::vector<OutTriangle>& out)
{
    const double plane = part.plane;
    for (const char* name : part.nodes)
    {
        const Node* node = FindNode(nodes, name);
        if (!node || !IsDrawn(node->name))
        {
            continue;
        }
        for (const NodeSegment& segment : node->segments)
        {
            const int kind = KindOf(segment.texture);
            if (!kind || segment.positions.empty())
            {
                continue;
            }
            std::vector<Point> positions = segment.positions;
            const bool isSpanned = Listed(part.spanned, name);
            if (isSpanned)
            {
                std::vector<Point> nearSide;
                for (const Point& p : positions)
                {
                    if (p.x < plane)
                    {
                        nearSide.push_back(p);
                    }
                }
                for (Point& p : positions)
                {
                    if (p.x > plane && !nearSide.empty())
                    {
                        const Point* best = &nearSide[0];
                        double bestDistance = 1e300;
                        for (const Point& q : nearSide)
                        {
                            const double d = (q.y - p.y) * (q.y - p.y) + (q.z - p.z) * (q.z - p.z);
                            if (d < bestDistance)
                            {
                                bestDistance = d;
                                best = &q;
                            }
                        }
                        p = {2 * plane - best->x, best->y, best->z};
                    }
                }
            }
            for (size_t t = 0; t + 2 < segment.triangles.size(); t += 3)
            {
                std::vector<PolygonCorner> polygon;
                for (int k = 0; k < 3; ++k)
                {
                    const int vertex = segment.triangles[t + k];
                    polygon.push_back({positions[vertex], segment.normals[vertex], segment.u[vertex], 1 - segment.v[vertex]});
                }
                if (Listed(part.keptOnce, name) || isSpanned)
                {
                    AddFan(polygon, kind, out);
                    continue;
                }
                const std::vector<PolygonCorner> cut = ClipBelow(polygon, plane);
                std::vector<PolygonCorner> mirrored(cut.rbegin(), cut.rend());
                for (PolygonCorner& corner : mirrored)
                {
                    corner.position.x = 2 * plane - corner.position.x;
                    corner.normal.x = -corner.normal.x;
                }
                AddFan(cut, kind, out);
                AddFan(mirrored, kind, out);
            }
        }
    }
}

void CockpitPieces(const std::string& lvlFolder, const CockpitShip& ship, std::vector<OutTriangle>& out)
{
    std::vector<Node> nodes;
    if (!LoadNodes(lvlFolder, ship.msh, nodes))
    {
        return;
    }
    for (const Part& part : ship.parts)
    {
        if (part.move == Move::Mirror)
        {
            MirrorPieces(nodes, part, out);
            continue;
        }
        Point low = {}, high = {}, scale = {part.factor, part.factor, part.factor};
        if (part.move == Move::Fit)
        {
            bool isFirst = true;
            for (const char* fitName : part.fitNodes)
            {
                const Node* fit = FindNode(nodes, fitName);
                if (!fit)
                {
                    return; // Python would stop here (KeyError)
                }
                for (const NodeSegment& segment : fit->segments)
                {
                    for (const Point& p : segment.positions)
                    {
                        if (isFirst)
                        {
                            low = high = p;
                            isFirst = false;
                        }
                        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
                        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
                    }
                }
            }
            for (int axis = 0; axis < 3; ++axis)
            {
                double span = Component(high, axis) - Component(low, axis);
                span = span != 0 ? span : 1;
                (&scale.x)[axis] = (Component(part.targetHigh, axis) - Component(part.targetLow, axis)) / span;
            }
        }
        auto move = [&](const Point& p) -> Point
        {
            if (part.move == Move::Fit)
            {
                Point result = {};
                for (int axis = 0; axis < 3; ++axis)
                {
                    double span = Component(high, axis) - Component(low, axis);
                    span = span != 0 ? span : 1;
                    (&result.x)[axis] = Component(part.targetLow, axis) +
                                        (Component(p, axis) - Component(low, axis)) / span *
                                            (Component(part.targetHigh, axis) - Component(part.targetLow, axis));
                }
                return result;
            }
            return Add(Add(EYE, Scale(Subtract(p, EYE), part.factor)), part.offset);
        };
        for (const char* name : part.nodes)
        {
            const Node* node = FindNode(nodes, name);
            if (!node || !IsDrawn(node->name))
            {
                continue;
            }
            for (const NodeSegment& segment : node->segments)
            {
                const int kind = KindOf(segment.texture);
                if (!kind || segment.positions.empty())
                {
                    continue;
                }
                for (size_t t = 0; t + 2 < segment.triangles.size(); t += 3)
                {
                    OutTriangle triangle = {};
                    for (int k = 0; k < 3; ++k)
                    {
                        const int vertex = segment.triangles[t + k];
                        triangle.positions[k] = move(segment.positions[vertex]);
                        // normals under a scale per axis: divided by it, made unit again
                        Point n = segment.normals[vertex];
                        for (int axis = 0; axis < 3; ++axis)
                        {
                            const double s = Component(scale, axis);
                            (&n.x)[axis] /= s != 0 ? s : 1;
                        }
                        double length = std::sqrt(Dot(n, n));
                        length = length != 0 ? length : 1;
                        triangle.normals[k] = Scale(n, 1.0 / length);
                        triangle.u[k] = segment.u[vertex];
                        triangle.v[k] = 1 - segment.v[vertex];
                    }
                    triangle.kind = kind;
                    out.push_back(triangle);
                }
            }
        }
    }
}

// ===================================================================================================================
// tools/mirrorstock.py: the combat speeder's passenger half as the mirror of the pilot's
// ===================================================================================================================

constexpr double MIDDLE = 0.58;
const char* const SPEEDER_NODES[] = {"window", "canopy", "dashboard", "floorboard", "lightfx"};
const Point CABIN = {MIDDLE, 1.5, 1.2};

struct SpeederCorner
{
    Point position;
    double u, v;
    Point normal;
};

std::vector<SpeederCorner> ClipMiddle(const std::vector<SpeederCorner>& polygon, bool keepLeft)
{
    std::vector<SpeederCorner> out;
    for (size_t index = 0; index < polygon.size(); ++index)
    {
        const SpeederCorner& p = polygon[index];
        const SpeederCorner& q = polygon[(index + 1) % polygon.size()];
        const bool isInsideP = keepLeft ? p.position.x <= MIDDLE : p.position.x >= MIDDLE;
        const bool isInsideQ = keepLeft ? q.position.x <= MIDDLE : q.position.x >= MIDDLE;
        if (isInsideP)
        {
            out.push_back(p);
        }
        if (isInsideP != isInsideQ)
        {
            const double k = (MIDDLE - p.position.x) / (q.position.x - p.position.x);
            out.push_back(
                {Add(p.position, Scale(Subtract(q.position, p.position), k)), p.u + (q.u - p.u) * k,
                 p.v + (q.v - p.v) * k, Add(p.normal, Scale(Subtract(q.normal, p.normal), k))});
        }
    }
    return out;
}

Point Unit(const Point& v)
{
    double length = std::sqrt(Dot(v, v));
    length = length != 0 ? length : 1;
    return Scale(v, 1.0 / length);
}

/// The face's normal turned toward the cabin.
Point FaceNormal(const Point& a, const Point& b, const Point& c)
{
    Point n = Unit(Cross(Subtract(b, a), Subtract(c, a)));
    const Point points[3] = {a, b, c};
    if (Dot(n, Subtract(CABIN, Centre(points))) < 0)
    {
        n = Scale(n, -1);
    }
    return n;
}

void SpeederMirror(const std::string& lvlFolder)
{
    std::vector<StockVertex>& mirror = _tables["SPEEDER_MIRROR"];
    mirror.clear();
    _speederMask.clear();
    std::vector<Node> nodes;
    if (!LoadNodes(lvlFolder, "ALL/MSH/all_1st_cockpit_combatspeeder.msh", nodes))
    {
        return;
    }
    for (const Node& node : nodes)
    {
        const std::string name = Lower(node.name);
        bool isListed = false;
        for (const char* prefix : SPEEDER_NODES)
        {
            isListed = isListed || StartsWith(name, prefix);
        }
        if (!IsDrawn(node.name) || !isListed)
        {
            continue;
        }
        for (const NodeSegment& segment : node.segments)
        {
            const std::string texture = Lower(segment.texture);
            const int kind = Contains(texture, "xwingwin") ? 4 : Contains(texture, "_fx") ? 2 : 1;
            for (size_t t = 0; t + 2 < segment.triangles.size(); t += 3)
            {
                const int* tri = &segment.triangles[t];
                const Point face =
                    FaceNormal(segment.positions[tri[0]], segment.positions[tri[1]], segment.positions[tri[2]]);
                std::vector<SpeederCorner> triangle;
                for (int k = 0; k < 3; ++k)
                {
                    const int vertex = tri[k];
                    const Point normal =
                        vertex < static_cast<int>(segment.normals.size()) ? Unit(segment.normals[vertex]) : face;
                    triangle.push_back({segment.positions[vertex], segment.u[vertex], 1 - segment.v[vertex], normal});
                }
                const std::vector<SpeederCorner> left = ClipMiddle(triangle, true);
                for (size_t k = 1; k + 1 < left.size(); ++k)
                {
                    for (const SpeederCorner* corner : {&left[0], &left[k], &left[k + 1]})
                    {
                        // mirrored, except on the middle line itself, where the game's own half goes on
                        const bool isOnLine = std::fabs(corner->position.x - MIDDLE) < 1e-4;
                        const Point n = isOnLine ? corner->normal
                                                 : Point{-corner->normal.x, corner->normal.y, corner->normal.z};
                        const Point p = {2 * MIDDLE - corner->position.x, corner->position.y, corner->position.z};
                        mirror.push_back(StockVertex{
                            {Round(p.x, 1e-4), Round(p.y, 1e-4), Round(p.z, 1e-4)},
                            {Round(n.x, 1e-4), Round(n.y, 1e-4), Round(n.z, 1e-4)},
                            Round(corner->u, 1e-5),
                            Round(corner->v, 1e-5),
                            kind});
                    }
                }
                if (kind == 4)
                {
                    continue; // the game's glass is see-through: nothing to hide behind it
                }
                const std::vector<SpeederCorner> right = ClipMiddle(triangle, false);
                for (size_t k = 1; k + 1 < right.size(); ++k)
                {
                    const Point corners[3] = {right[0].position, right[k].position, right[k + 1].position};
                    const Point n = FaceNormal(corners[0], corners[1], corners[2]);
                    for (const Point& p : corners)
                    {
                        const Point pushed = Add(p, Scale(n, 0.005));
                        _speederMask.push_back(
                            {Round(pushed.x, 1e-4), Round(pushed.y, 1e-4), Round(pushed.z, 1e-4)});
                    }
                }
            }
        }
    }
}

std::vector<std::string> _names;
std::vector<const char*> _namePointers;

} // namespace

std::string Build(const std::string& lvlFolder)
{
    _tables.clear();
    int exteriorTriangles = 0, cockpitTriangles = 0;
    for (const Ship& ship : SHIPS)
    {
        std::vector<OutTriangle> triangles;
        ShipTriangles(lvlFolder, ship, triangles);
        triangles = WithoutTwins(triangles);
        exteriorTriangles += static_cast<int>(triangles.size());
        Store(std::string("EXTERIOR_") + ship.table, triangles);
    }
    for (const CockpitShip& ship : CockpitShips())
    {
        std::vector<OutTriangle> triangles;
        CockpitPieces(lvlFolder, ship, triangles);
        cockpitTriangles += static_cast<int>(triangles.size());
        Store(std::string("STOCK_") + ship.table, triangles);
    }
    SpeederMirror(lvlFolder);
    _names.clear();
    for (const auto& entry : _tables)
    {
        _names.push_back(entry.first);
    }
    _namePointers.clear();
    for (const std::string& name : _names)
    {
        _namePointers.push_back(name.c_str());
    }
    _namePointers.push_back(nullptr);
    int emptyTables = 0;
    for (const auto& entry : _tables)
    {
        emptyTables += entry.second.empty() ? 1 : 0;
    }
    char summary[200];
    std::snprintf(
        summary, sizeof(summary),
        "%d tables from the game's level files: %d exterior triangles, %d cockpit triangles, %d speeder mirror "
        "vertices; %d empty",
        static_cast<int>(_tables.size()), exteriorTriangles, cockpitTriangles,
        static_cast<int>(_tables["SPEEDER_MIRROR"].size()), emptyTables);
    return summary;
}

Table Get(const char* name)
{
    auto found = _tables.find(name);
    if (found == _tables.end() || found->second.empty())
    {
        return {nullptr, 0};
    }
    return {found->second.data(), static_cast<int>(found->second.size())};
}

const ShellKit::Vec3* SpeederMask(int& count)
{
    count = static_cast<int>(_speederMask.size());
    return _speederMask.empty() ? nullptr : _speederMask.data();
}

const char* const* Names()
{
    return _namePointers.data();
}

} // namespace StockTables

} // namespace SwbfVr
