#pragma once

/// ShellKit: a small hard-surface modelling kit for the cockpit shells (PLAN 54; ShellDesigns.cpp builds one shell
/// per stock tank with it, CockpitShell.cpp draws it). Pure geometry, no Direct3D and no game memory, so the same code
/// also runs in the preview tool (tools/shellpreview.cpp, which writes OBJ files for tools/meshview.py).
///
/// Space: the cockpit model's own (metres; x right, y up, z back toward the pilot). Every vertex carries a finish: a
/// patch of the cockpit's texture and how it is lit (the shell's finish table). Flat faces get a normal facing the
/// pilot's eye (the shells are seen from inside; faces turned away are hidden behind nearer ones); curved surfaces
/// (tubes, lathes, lofts) get smooth normals.
///
/// Texture mapping (idren2023 2026-10-17: "beautifully done"): every surface is mapped by real distances, the patch
/// standing for `meters` of surface along its u and in proportion along its v (so nothing is stretched), and every
/// face is cut exactly on the lines where the patch repeats (MappedPolygon), so no triangle straddles a repeat and
/// nothing smears. Flat faces are mapped by projecting their absolute position, so neighbouring faces of one plane
/// line up; strips, ribs, pipes and walls by their length along and across.

#include <cstdint>

namespace SwbfVr
{

namespace ShellKit
{

struct Vec3
{
    float x, y, z;
};

inline Vec3 operator+(Vec3 a, Vec3 b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(Vec3 a, Vec3 b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(Vec3 a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

inline Vec3 operator*(float s, Vec3 a)
{
    return {a.x * s, a.y * s, a.z * s};
}

inline Vec3 operator-(Vec3 a)
{
    return {-a.x, -a.y, -a.z};
}

float Dot(Vec3 a, Vec3 b);
Vec3 Cross(Vec3 a, Vec3 b);
float Length(Vec3 a);
Vec3 Normalized(Vec3 a);
Vec3 Lerp(Vec3 a, Vec3 b, float t);
/// A point of a cubic Bezier curve.
Vec3 Bezier(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t);

/// A place and orientation: origin and three unit axes (x right, y up, z out of a surface toward the viewer).
struct Frame
{
    Vec3 origin, x, y, z;
    /// A point given in this frame.
    Vec3 At(float localX, float localY, float localZ) const;
    /// A direction given in this frame.
    Vec3 Along(float localX, float localY, float localZ) const;
};

/// A frame at origin with x along xAxis and y as near yHint as possible (z = x cross y).
Frame MakeFrame(Vec3 origin, Vec3 xAxis, Vec3 yHint);
/// The same frame moved by a local offset.
Frame Offset(const Frame& frame, float localX, float localY, float localZ);

/// A rectangle of the shell's texture (u, v from the top left, 0..1).
struct Patch
{
    float u0, v0, u1, v1;
};

/// One surface finish of a shell: its texture patch (unless untextured), the surface it covers and its light.
struct Finish
{
    Patch patch;
    float meters;   // the patch covers this much surface along the faces' u (across in the patch's own proportion)
    int texture;    // 0 none (plain colour), 1 the cockpit's texture, 2 its effects texture (screens, lamps), 3 the
                    // vehicle's exterior texture (hull, deck, its own seats), 4 the cockpit's window texture (glass;
                    // its alpha is multiplied in), 5 the shell's screen texture (a gunner's stock targeting frame),
                    // 6 the scope pass's picture (Hud::ScopePicture: a periscope's view)
    uint32_t color; // 0xRRGGBB: the lit colour (times the texture, 2x modulated, when textured)
    float specular; // highlight strength 0..1
    float power;    // highlight sharpness
    float glow;     // 0..1: self-lit (lamps, screens)
    float opacity = 1.0f;  // below 1: see-through glass, drawn after everything else, blended
    bool isTurned = false; // the patch lies turned a quarter: its v runs along the faces' u (long strips, bands)
    bool isOwnScale = false; // keeps `meters` although the mesh sets one texel density (a picture meant at its size)
    float textureTexels = 512.0f; // the texture's width in texels (the patch's size at the mesh's density)
    bool isDense = false;         // at the mesh's density whatever its texture (else only textures 1 and 3 are)
};

struct Vertex
{
    Vec3 position, normal;
    float u, v;
    uint8_t finish;
    uint8_t part; // which moving part it belongs to (a gunner's turret: base, gun cradle, barrel); 0 for cockpits
};

/// A mesh being built: a fixed buffer of vertices (three per triangle), the shell's finishes and the pilot's eye.
struct Mesh
{
    Vertex* vertices;
    int capacity;
    int count;
    bool isFull; // a triangle did not fit
    const Finish* finishes;
    Vec3 eye;
    uint8_t part = 0; // the part the next vertices belong to (set by a design between its parts)
    /// Above 0: every finish on the cockpit's or the exterior texture (512 x 512) is mapped at this many texels a
    /// metre whatever its `meters` (idren2023 2026-10-19: texels consistently sized), unless isOwnScale.
    float texelsPerMeter = 0;
};

/// A point of a polygon to map: position, normal and texture position in patch repeats (s along the patch's u, t
/// along its v; any range: the polygon is cut where they cross whole numbers).
struct MapPoint
{
    Vec3 position, normal;
    float s, t;
};

/// A convex polygon, cut on the patch's repeat lines and written as triangles. Flat: one normal facing the eye
/// (normals ignored), else the given normals interpolated.
void MappedPolygon(Mesh& mesh, const MapPoint* points, int count, int finish, bool isFlat);
/// A flat convex polygon mapped by projecting its absolute positions on uAxis and the in-plane axis across it.
void PlanarPolygon(Mesh& mesh, const Vec3* positions, int count, Vec3 uAxis, int finish);
/// One triangle, flat: normal facing the eye. s, t in 0..1 of the finish's patch.
void Triangle(Mesh& mesh, Vec3 a, Vec3 b, Vec3 c, const float (*st)[2], int finish);
/// One triangle with its own vertex normals (smooth surfaces).
void SmoothTriangle(Mesh& mesh, const Vec3* positions, const Vec3* normals, const float (*st)[2], int finish);
/// A flat quad a, b, c, d (in order round it) mapped onto the patch rectangle s0..s1 x t0..t1 (a at s0 t0, b at
/// s1 t0, c at s1 t1, d at s0 t1): for small parts that show one piece of the patch.
void Quad(
    Mesh& mesh,
    Vec3 a,
    Vec3 b,
    Vec3 c,
    Vec3 d,
    int finish,
    float s0 = 0,
    float t0 = 0,
    float s1 = 1,
    float t1 = 1);
/// A flat quad mapped by its real size (PlanarPolygon), the patch's u along a to b.
void PlanarQuad(Mesh& mesh, Vec3 a, Vec3 b, Vec3 c, Vec3 d, int finish);
/// A flat parallelogram from origin along uSide and vSide, mapped by its real size (the patch's u along uSide).
void TiledQuad(Mesh& mesh, Vec3 origin, Vec3 uSide, Vec3 vSide, int finish);
/// A box with bevelled edges: centred on the frame, half sizes along its axes; faces mapped by their real size, the
/// patch's u along the frame's x (on the x faces along z), the bevels along their edge.
void Box(Mesh& mesh, const Frame& frame, Vec3 half, float bevel, int finish);
/// A panel on a wall: a back plate in the frame's x-y plane, a raised border of the given width standing out by
/// depth (bevelled toward the middle), the inset middle back at the plate; rivets along the border every
/// rivetSpacing metres (0: none).
void Panel(
    Mesh& mesh,
    const Frame& frame,
    float halfWidth,
    float halfHeight,
    float border,
    float depth,
    int borderFinish,
    int insetFinish,
    float rivetSpacing,
    int rivetFinish);
/// The metres the finish's patch covers along the faces' u and across (its v), as this mesh maps it.
float MetersU(const Mesh& mesh, int finish);
float MetersV(const Mesh& mesh, int finish);
/// A small rivet or bolt head on the frame's x-y plane, standing out along z (mapped by its real size).
void Rivet(Mesh& mesh, const Frame& frame, float radius, int finish);
/// A profile (x side, y up in the path's frame) swept along a path; the profile's frame turns with the path (parallel
/// transport from upHint). Smooth normals across the profile when isSmooth, else flat. The patch's u runs along the
/// path, its v round the profile, both by length. Ends closed when capEnds.
void Sweep(
    Mesh& mesh,
    const Vec3* path,
    int pathCount,
    const float (*profile)[2],
    int profileCount,
    bool isClosed,
    bool isSmooth,
    Vec3 upHint,
    int finish,
    bool capEnds);
/// A round tube along a path.
void Tube(Mesh& mesh, const Vec3* path, int pathCount, float radius, int sides, int finish, bool capEnds = true);
/// A surface turned about the frame's y axis through rings (height along y, radius), elliptic by depthScale along z.
void Lathe(
    Mesh& mesh,
    const Frame& frame,
    const float (*rings)[2],
    int ringCount,
    int sides,
    int finish,
    float depthScale = 1.0f);
/// A surface between two polylines of the same count (a ruled surface, cut `steps` times across): the patch's u along
/// the lines, its v across, both by length. Smooth normals.
void Loft(Mesh& mesh, const Vec3* first, const Vec3* second, int count, int steps, int finish);
/// A grille in the frame's x-y plane: a frame of the given border and slats across (along x), each a thin angled
/// louvre.
void Grille(
    Mesh& mesh,
    const Frame& frame,
    float halfWidth,
    float halfHeight,
    float border,
    int slats,
    float depth,
    int frameFinish,
    int slatFinish);

} // namespace ShellKit

} // namespace SwbfVr
