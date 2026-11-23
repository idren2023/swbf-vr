// ShellKit.cpp: the cockpit shells' modelling kit (see ShellKit.h).

#include "ShellKit.h"

#include <cmath>

namespace SwbfVr
{

namespace ShellKit
{

namespace
{

constexpr float PI = 3.14159265f;
/// Patches are sampled this far inside their edges (in texture units, about 1.5 texels of a 512 texture), so
/// filtering never picks up the texels around them.
constexpr float PATCH_INSET = 0.003f;
/// The most points a clipped polygon can have.
constexpr int MOST_POINTS = 24;

// The texels a patch spans along and across the faces' u, as sampled (inside PATCH_INSET).
void PatchTexels(const Finish& finish, float* along, float* across)
{
    const float texels = finish.textureTexels;
    const float width = (finish.patch.u1 - finish.patch.u0 - 2 * PATCH_INSET) * texels;
    const float height = (finish.patch.v1 - finish.patch.v0 - 2 * PATCH_INSET) * texels;
    *along = fmaxf(finish.isTurned ? height : width, 0.01f);
    *across = fmaxf(finish.isTurned ? width : height, 0.01f);
}

// A vertex into the mesh (patch-local s, t in 0..1 into texture space). False when full.
bool Emit(Mesh& mesh, Vec3 position, Vec3 normal, float s, float t, int finish)
{
    if (mesh.count >= mesh.capacity)
    {
        mesh.isFull = true;
        return false;
    }
    const Patch& patch = mesh.finishes[finish].patch;
    const float u0 = patch.u0 + PATCH_INSET, u1 = patch.u1 - PATCH_INSET;
    const float v0 = patch.v0 + PATCH_INSET, v1 = patch.v1 - PATCH_INSET;
    Vertex& vertex = mesh.vertices[mesh.count++];
    vertex.position = position;
    vertex.normal = normal;
    const float along = fminf(fmaxf(s, 0.0f), 1.0f), across = fminf(fmaxf(t, 0.0f), 1.0f);
    const bool isTurned = mesh.finishes[finish].isTurned;
    vertex.u = u0 + (u1 - u0) * (isTurned ? across : along);
    vertex.v = v0 + (v1 - v0) * (isTurned ? along : across);
    vertex.finish = static_cast<uint8_t>(finish);
    vertex.part = mesh.part;
    return true;
}

MapPoint Mix(const MapPoint& a, const MapPoint& b, float t)
{
    return {Lerp(a.position, b.position, t), Lerp(a.normal, b.normal, t), a.s + (b.s - a.s) * t, a.t + (b.t - a.t) * t};
}

// Sutherland-Hodgman: keeps the part of the polygon where value(point) * sign >= limit * sign; value 0 = s, 1 = t.
int Clip(const MapPoint* in, int count, MapPoint* out, int axis, float limit, bool keepAbove)
{
    int written = 0;
    for (int i = 0; i < count; ++i)
    {
        const MapPoint& a = in[i];
        const MapPoint& b = in[(i + 1) % count];
        const float va = axis == 0 ? a.s : a.t, vb = axis == 0 ? b.s : b.t;
        const bool insideA = keepAbove ? va >= limit : va <= limit;
        const bool insideB = keepAbove ? vb >= limit : vb <= limit;
        if (insideA && written < MOST_POINTS)
        {
            out[written++] = a;
        }
        if (insideA != insideB && written < MOST_POINTS)
        {
            out[written++] = Mix(a, b, (limit - va) / (vb - va));
        }
    }
    return written;
}

// A frame across a path at one of its points (tangent = z of the result's sweep; x side, y up).
struct PathFrame
{
    Vec3 side, up;
};

} // namespace

float Dot(Vec3 a, Vec3 b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

Vec3 Cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float Length(Vec3 a)
{
    return sqrtf(Dot(a, a));
}

Vec3 Normalized(Vec3 a)
{
    const float length = Length(a);
    return length > 1e-9f ? a * (1.0f / length) : Vec3{0, 1, 0};
}

Vec3 Lerp(Vec3 a, Vec3 b, float t)
{
    return a + (b - a) * t;
}

Vec3 Bezier(Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, float t)
{
    const float u = 1 - t;
    return p0 * (u * u * u) + p1 * (3 * u * u * t) + p2 * (3 * u * t * t) + p3 * (t * t * t);
}

float MetersU(const Mesh& mesh, int finish)
{
    const Finish& f = mesh.finishes[finish];
    float along, across;
    PatchTexels(f, &along, &across);
    if (mesh.texelsPerMeter > 0 && (f.texture == 1 || f.texture == 3 || f.isDense) && !f.isOwnScale)
    {
        return along / mesh.texelsPerMeter;
    }
    return f.meters;
}

float MetersV(const Mesh& mesh, int finish)
{
    float along, across;
    PatchTexels(mesh.finishes[finish], &along, &across);
    return MetersU(mesh, finish) * across / along; // square texels
}

Vec3 Frame::At(float localX, float localY, float localZ) const
{
    return origin + x * localX + y * localY + z * localZ;
}

Vec3 Frame::Along(float localX, float localY, float localZ) const
{
    return x * localX + y * localY + z * localZ;
}

Frame MakeFrame(Vec3 origin, Vec3 xAxis, Vec3 yHint)
{
    const Vec3 x = Normalized(xAxis);
    const Vec3 z = Normalized(Cross(x, yHint));
    const Vec3 y = Cross(z, x);
    return {origin, x, y, z};
}

Frame Offset(const Frame& frame, float localX, float localY, float localZ)
{
    Frame moved = frame;
    moved.origin = frame.At(localX, localY, localZ);
    return moved;
}

void MappedPolygon(Mesh& mesh, const MapPoint* points, int count, int finish, bool isFlat)
{
    if (count < 3 || count > MOST_POINTS)
    {
        return;
    }
    float sMin = points[0].s, sMax = sMin, tMin = points[0].t, tMax = tMin;
    for (int i = 1; i < count; ++i)
    {
        sMin = fminf(sMin, points[i].s);
        sMax = fmaxf(sMax, points[i].s);
        tMin = fminf(tMin, points[i].t);
        tMax = fmaxf(tMax, points[i].t);
    }
    Vec3 flatNormal = {0, 1, 0};
    if (isFlat)
    {
        flatNormal =
            Normalized(Cross(points[1].position - points[0].position, points[2].position - points[0].position));
        if (Dot(flatNormal, mesh.eye - points[0].position) < 0)
        {
            flatNormal = -flatNormal;
        }
    }
    // Each repeat cell the polygon touches: clip to it, fan it out, map it onto the patch.
    const int s0 = static_cast<int>(floorf(sMin + 1e-4f)), s1 = static_cast<int>(ceilf(sMax - 1e-4f));
    const int t0 = static_cast<int>(floorf(tMin + 1e-4f)), t1 = static_cast<int>(ceilf(tMax - 1e-4f));
    for (int cellT = t0; cellT < (t1 > t0 ? t1 : t0 + 1); ++cellT)
    {
        for (int cellS = s0; cellS < (s1 > s0 ? s1 : s0 + 1); ++cellS)
        {
            MapPoint a[MOST_POINTS], b[MOST_POINTS];
            int n = Clip(points, count, a, 0, static_cast<float>(cellS), true);
            n = Clip(a, n, b, 0, static_cast<float>(cellS + 1), false);
            n = Clip(b, n, a, 1, static_cast<float>(cellT), true);
            n = Clip(a, n, b, 1, static_cast<float>(cellT + 1), false);
            for (int k = 1; k + 1 < n; ++k)
            {
                const MapPoint* corners[3] = {&b[0], &b[k], &b[k + 1]};
                for (const MapPoint* corner : corners)
                {
                    Emit(
                        mesh,
                        corner->position,
                        isFlat ? flatNormal : Normalized(corner->normal),
                        corner->s - cellS,
                        corner->t - cellT,
                        finish);
                }
            }
        }
    }
}

void PlanarPolygon(Mesh& mesh, const Vec3* positions, int count, Vec3 uAxis, int finish)
{
    if (count < 3)
    {
        return;
    }
    const Vec3 normal = Normalized(Cross(positions[1] - positions[0], positions[2] - positions[0]));
    const Vec3 u = Normalized(uAxis - normal * Dot(uAxis, normal));
    const Vec3 v = Cross(normal, u);
    const float metersU = MetersU(mesh, finish), metersV = MetersV(mesh, finish);
    MapPoint points[MOST_POINTS];
    for (int i = 0; i < count && i < MOST_POINTS; ++i)
    {
        points[i] = {positions[i], normal, Dot(positions[i], u) / metersU, Dot(positions[i], v) / metersV};
    }
    MappedPolygon(mesh, points, count < MOST_POINTS ? count : MOST_POINTS, finish, true);
}

void Triangle(Mesh& mesh, Vec3 a, Vec3 b, Vec3 c, const float (*st)[2], int finish)
{
    Vec3 normal = Normalized(Cross(b - a, c - a));
    const Vec3 center = (a + b + c) * (1.0f / 3.0f);
    if (Dot(normal, mesh.eye - center) < 0)
    {
        normal = -normal;
    }
    Emit(mesh, a, normal, st[0][0], st[0][1], finish);
    Emit(mesh, b, normal, st[1][0], st[1][1], finish);
    Emit(mesh, c, normal, st[2][0], st[2][1], finish);
}

void SmoothTriangle(Mesh& mesh, const Vec3* positions, const Vec3* normals, const float (*st)[2], int finish)
{
    for (int corner = 0; corner < 3; ++corner)
    {
        Emit(mesh, positions[corner], normals[corner], st[corner][0], st[corner][1], finish);
    }
}

void Quad(Mesh& mesh, Vec3 a, Vec3 b, Vec3 c, Vec3 d, int finish, float s0, float t0, float s1, float t1)
{
    // The whole patch asked for (the defaults) on a textured finish: kept only where the quad has about the patch's
    // own proportions; a quad of other proportions (a long wall, a strip) is mapped by its real size instead
    // (2026-11-09 texturing pass, idren2023: stretching all over; tools/uvaudit.py measured it).
    if (s0 == 0 && t0 == 0 && s1 == 1 && t1 == 1 && mesh.finishes && mesh.finishes[finish].texture != 0)
    {
        const float across = Length(b - a), down = Length(d - a);
        const float quadRatio = across / (down > 1e-6f ? down : 1e-6f);
        const float patchRatio = MetersU(mesh, finish) / MetersV(mesh, finish);
        const float stretch = quadRatio > patchRatio ? quadRatio / patchRatio : patchRatio / quadRatio;
        if (stretch > 1.6f)
        {
            PlanarQuad(mesh, a, b, c, d, finish);
            return;
        }
    }
    const float first[3][2] = {{s0, t0}, {s1, t0}, {s1, t1}};
    const float second[3][2] = {{s0, t0}, {s1, t1}, {s0, t1}};
    Triangle(mesh, a, b, c, first, finish);
    Triangle(mesh, a, c, d, second, finish);
}

void PlanarQuad(Mesh& mesh, Vec3 a, Vec3 b, Vec3 c, Vec3 d, int finish)
{
    const Vec3 corners[4] = {a, b, c, d};
    PlanarPolygon(mesh, corners, 4, b - a, finish);
}

void TiledQuad(Mesh& mesh, Vec3 origin, Vec3 uSide, Vec3 vSide, int finish)
{
    const Vec3 corners[4] = {origin, origin + uSide, origin + uSide + vSide, origin + vSide};
    PlanarPolygon(mesh, corners, 4, uSide, finish);
}

void Box(Mesh& mesh, const Frame& frame, Vec3 half, float bevel, int finish)
{
    const float hx = half.x, hy = half.y, hz = half.z;
    const float b = fminf(bevel, fminf(hx, fminf(hy, hz)) * 0.9f);
    const Vec3 X = frame.x, Y = frame.y, Z = frame.z, O = frame.origin;
    // Faces (shrunk by the bevel): the patch's u along x, on the x faces along z.
    const auto face = [&](Vec3 center, Vec3 uAxis, Vec3 vAxis, float uHalf, float vHalf)
    {
        TiledQuad(mesh, center - uAxis * uHalf - vAxis * vHalf, uAxis * (2 * uHalf), vAxis * (2 * vHalf), finish);
    };
    face(O + Z * hz, X, Y, hx - b, hy - b);
    face(O - Z * hz, X, Y, hx - b, hy - b);
    face(O + X * hx, Z, Y, hz - b, hy - b);
    face(O - X * hx, Z, Y, hz - b, hy - b);
    face(O + Y * hy, X, Z, hx - b, hz - b);
    face(O - Y * hy, X, Z, hx - b, hz - b);
    if (b <= 0)
    {
        return;
    }
    // Edge strips, mapped along their edge; corner triangles.
    const Vec3 axes[3] = {X, Y, Z};
    const float halves[3] = {hx, hy, hz};
    for (int along = 0; along < 3; ++along)
    {
        const int i = (along + 1) % 3, j = (along + 2) % 3;
        for (int si = -1; si <= 1; si += 2)
        {
            for (int sj = -1; sj <= 1; sj += 2)
            {
                const Vec3 edgeA = O + axes[i] * (si * halves[i]) + axes[j] * (sj * (halves[j] - b));
                const Vec3 edgeB = O + axes[i] * (si * (halves[i] - b)) + axes[j] * (sj * halves[j]);
                const Vec3 run = axes[along] * (halves[along] - b);
                PlanarQuad(mesh, edgeA - run, edgeA + run, edgeB + run, edgeB - run, finish);
            }
        }
    }
    for (int sx = -1; sx <= 1; sx += 2)
    {
        for (int sy = -1; sy <= 1; sy += 2)
        {
            for (int sz = -1; sz <= 1; sz += 2)
            {
                const Vec3 corner[3] = {
                    O + X * (sx * hx) + Y * (sy * (hy - b)) + Z * (sz * (hz - b)),
                    O + X * (sx * (hx - b)) + Y * (sy * hy) + Z * (sz * (hz - b)),
                    O + X * (sx * (hx - b)) + Y * (sy * (hy - b)) + Z * (sz * hz)};
                PlanarPolygon(mesh, corner, 3, X, finish);
            }
        }
    }
}

void Rivet(Mesh& mesh, const Frame& frame, float radius, int finish)
{
    constexpr int SIDES = 8;
    const float metersU = MetersU(mesh, finish), metersV = MetersV(mesh, finish);
    // Mapped as seen from above, by its real size (no patch squeezed onto a head 1 cm across).
    const auto mapped = [&](float x, float y, float z)
    {
        return MapPoint{frame.At(x, y, z), frame.Along(x, y, radius * 0.9f), x / metersU + 0.5f, y / metersV + 0.5f};
    };
    for (int side = 0; side < SIDES; ++side)
    {
        const float a0 = 2 * PI * side / SIDES, a1 = 2 * PI * (side + 1) / SIDES;
        const float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        const MapPoint ring[4] = {
            mapped(radius * c0, radius * s0, 0),
            mapped(radius * c1, radius * s1, 0),
            mapped(radius * 0.6f * c1, radius * 0.6f * s1, radius * 0.4f),
            mapped(radius * 0.6f * c0, radius * 0.6f * s0, radius * 0.4f)};
        MappedPolygon(mesh, ring, 4, finish, false);
        const MapPoint cap[3] = {ring[3], ring[2], mapped(0, 0, radius * 0.55f)};
        MappedPolygon(mesh, cap, 3, finish, false);
    }
}

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
    int rivetFinish)
{
    const float w = halfWidth, h = halfHeight, iw = halfWidth - border, ih = halfHeight - border;
    const float lip = border * 0.35f; // the bevel's run toward the middle
    // Inset middle (at the plate).
    TiledQuad(
        mesh,
        frame.At(-iw - lip, -ih - lip, 0),
        frame.x * (2 * (iw + lip)),
        frame.y * (2 * (ih + lip)),
        insetFinish);
    // Border: the outer faces (top at depth), bevel down to the inset, and the outer sides down to the plate; each
    // mapped along its edge.
    const float corners[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int edge = 0; edge < 4; ++edge)
    {
        const float* c0 = corners[edge];
        const float* c1 = corners[(edge + 1) % 4];
        const Vec3 outer0 = frame.At(c0[0] * w, c0[1] * h, depth), outer1 = frame.At(c1[0] * w, c1[1] * h, depth);
        const Vec3 inner0 = frame.At(c0[0] * iw, c0[1] * ih, depth), inner1 = frame.At(c1[0] * iw, c1[1] * ih, depth);
        const Vec3 low0 = frame.At(c0[0] * (iw + lip), c0[1] * (ih + lip), 0);
        const Vec3 low1 = frame.At(c1[0] * (iw + lip), c1[1] * (ih + lip), 0);
        const Vec3 base0 = frame.At(c0[0] * w, c0[1] * h, 0), base1 = frame.At(c1[0] * w, c1[1] * h, 0);
        PlanarQuad(mesh, outer0, outer1, inner1, inner0, borderFinish);
        PlanarQuad(mesh, inner0, inner1, low1, low0, borderFinish);
        PlanarQuad(mesh, base0, base1, outer1, outer0, borderFinish);
    }
    if (rivetSpacing <= 0)
    {
        return;
    }
    const float r = border * 0.22f, mid = (w + iw) * 0.5f, midH = (h + ih) * 0.5f;
    const int alongX = static_cast<int>(2 * mid / rivetSpacing), alongY = static_cast<int>(2 * midH / rivetSpacing);
    for (int i = 0; i <= alongX; ++i)
    {
        const float x = -mid + 2 * mid * i / (alongX > 0 ? alongX : 1);
        Rivet(mesh, Offset(frame, x, -midH, depth), r, rivetFinish);
        Rivet(mesh, Offset(frame, x, midH, depth), r, rivetFinish);
    }
    for (int i = 1; i < alongY; ++i)
    {
        const float y = -midH + 2 * midH * i / alongY;
        Rivet(mesh, Offset(frame, -mid, y, depth), r, rivetFinish);
        Rivet(mesh, Offset(frame, mid, y, depth), r, rivetFinish);
    }
}

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
    bool capEnds)
{
    if (pathCount < 2 || profileCount < 2)
    {
        return;
    }
    constexpr int MOST = 128;
    PathFrame frames[MOST];
    float along[MOST];
    const int count = pathCount < MOST ? pathCount : MOST;
    Vec3 up = upHint;
    along[0] = 0;
    for (int i = 0; i < count; ++i)
    {
        const Vec3 tangent = Normalized(
            i == 0           ? path[1] - path[0]
            : i == count - 1 ? path[i] - path[i - 1]
                             : path[i + 1] - path[i - 1]);
        Vec3 side = Cross(tangent, up);
        if (Length(side) < 1e-4f)
        {
            side = Cross(tangent, Vec3{1, 0, 0});
        }
        side = Normalized(side);
        up = Normalized(Cross(side, tangent)); // carried on: parallel transport
        frames[i] = {side, up};
        if (i > 0)
        {
            along[i] = along[i - 1] + Length(path[i] - path[i - 1]);
        }
    }
    // Distance round the profile, for the patch's v.
    float round[64 + 1];
    const int profileUsed = profileCount < 64 ? profileCount : 64;
    round[0] = 0;
    for (int k = 1; k <= profileUsed; ++k)
    {
        const float* a = profile[k - 1];
        const float* b = profile[k % profileUsed];
        round[k] = round[k - 1] + sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]));
    }
    const float metersU = MetersU(mesh, finish), metersV = MetersV(mesh, finish);
    const int ringEnd = isClosed ? profileUsed : profileUsed - 1;
    const auto at = [&](int i, int k)
    {
        return path[i] + frames[i].side * profile[k][0] + frames[i].up * profile[k][1];
    };
    for (int i = 0; i + 1 < count; ++i)
    {
        // The patch's u: the distance along the path plus each profile point's offset along this stretch's own
        // straight line, so the stretch is mapped as its flat development (round a bend the outside is longer than the
        // inside: one u per ring stretched it there and squeezed it here); a sharp corner is a mitre seam.
        const Vec3 straight = Normalized(path[i + 1] - path[i]);
        for (int k = 0; k < ringEnd; ++k)
        {
            const int k1 = (k + 1) % profileUsed;
            const int rings[4] = {i, i + 1, i + 1, i};
            const int corners[4] = {k, k, k1, k1};
            MapPoint points[4];
            for (int c = 0; c < 4; ++c)
            {
                const PathFrame& fr = frames[rings[c]];
                const float* q = profile[corners[c]];
                points[c].position = at(rings[c], corners[c]);
                points[c].normal = Normalized(fr.side * q[0] + fr.up * q[1]);
                points[c].s = (along[rings[c]] + Dot(points[c].position - path[rings[c]], straight)) / metersU;
                points[c].t = (c >= 2 ? round[k + 1] : round[k]) / metersV;
            }
            MappedPolygon(mesh, points, 4, finish, !isSmooth);
        }
    }
    if (!capEnds || !isClosed)
    {
        return;
    }
    for (int end = 0; end < 2; ++end)
    {
        const int i = end == 0 ? 0 : count - 1;
        const PathFrame& fr = frames[i];
        Vec3 cap[64];
        for (int k = 0; k < profileUsed; ++k)
        {
            cap[k] = path[i] + fr.side * profile[k][0] + fr.up * profile[k][1];
        }
        // Fan from the first point (profiles are convex), mapped flat.
        for (int k = 1; k + 1 < profileUsed; ++k)
        {
            const Vec3 triangle[3] = {cap[0], cap[k], cap[k + 1]};
            PlanarPolygon(mesh, triangle, 3, fr.side, finish);
        }
    }
}

void Tube(Mesh& mesh, const Vec3* path, int pathCount, float radius, int sides, int finish, bool capEnds)
{
    float profile[32][2];
    const int count = sides < 32 ? sides : 32;
    for (int k = 0; k < count; ++k)
    {
        const float angle = 2 * PI * k / count;
        profile[k][0] = radius * cosf(angle);
        profile[k][1] = radius * sinf(angle);
    }
    const Vec3 hint = fabsf(Normalized(path[pathCount - 1] - path[0]).y) > 0.9f ? Vec3{0, 0, 1} : Vec3{0, 1, 0};
    Sweep(mesh, path, pathCount, profile, count, true, true, hint, finish, capEnds);
}

void Lathe(
    Mesh& mesh,
    const Frame& frame,
    const float (*rings)[2],
    int ringCount,
    int sides,
    int finish,
    float depthScale)
{
    const float metersU = MetersU(mesh, finish), metersV = MetersV(mesh, finish);
    // Distance along the profile, for the patch's v.
    float along[64];
    along[0] = 0;
    for (int r = 1; r < ringCount && r < 64; ++r)
    {
        const float dh = rings[r][0] - rings[r - 1][0], dr = rings[r][1] - rings[r - 1][1];
        along[r] = along[r - 1] + sqrtf(dh * dh + dr * dr);
    }
    for (int r = 0; r + 1 < ringCount && r + 1 < 64; ++r)
    {
        // Profile normals per ring from the neighbours' slope (smooth across rings).
        float slope[2][2];
        for (int end = 0; end < 2; ++end)
        {
            const int index = r + end;
            const int below = index > 0 ? index - 1 : index, above = index + 1 < ringCount ? index + 1 : index;
            const float dh = rings[above][0] - rings[below][0], dr = rings[above][1] - rings[below][1];
            const float length = sqrtf(dh * dh + dr * dr);
            slope[end][0] = length > 1e-6f ? dh / length : 1.0f;
            slope[end][1] = length > 1e-6f ? -dr / length : 0.0f;
        }
        // The band unrolled flat (a cone's development; a flat ring is its own plan, a cylinder a rectangle): each
        // point's place in it gives the patch's u and v, so that the band keeps square texels with no shear (until
        // b320 u was the arc at the band's mean radius: a cone's or a disc's rims were squeezed and stretched). Round
        // the axis by arc length (elliptic by depthScale); bands meet at their rings with v continuous where u is 0.
        float round[2][129];
        const int around = sides < 128 ? sides : 128;
        for (int end = 0; end < 2; ++end)
        {
            const float ringRadius = rings[r + end][1];
            round[end][0] = 0;
            for (int side = 1; side <= around; ++side)
            {
                const float a0 = 2 * PI * (side - 1) / around, a1 = 2 * PI * side / around;
                const float dx = ringRadius * (cosf(a1) - cosf(a0));
                const float dz = ringRadius * depthScale * (sinf(a1) - sinf(a0));
                round[end][side] = round[end][side - 1] + sqrtf(dx * dx + dz * dz);
            }
        }
        const float slant = along[r + 1] - along[r], widening = rings[r + 1][1] - rings[r][1];
        const bool isCone = fabsf(widening) > 1e-4f * fmaxf(slant, 1e-4f) && slant > 1e-6f;
        const float apexStart = isCone ? rings[r][1] * slant / widening : 0; // signed distance from the apex
        for (int side = 0; side < around; ++side)
        {
            MapPoint points[4];
            const int ends[4] = {0, 0, 1, 1};
            const int arounds[4] = {side, side + 1, side + 1, side};
            for (int c = 0; c < 4; ++c)
            {
                const int end = ends[c];
                const float angle = 2 * PI * arounds[c] / around;
                const float cs = cosf(angle), sn = sinf(angle);
                const float height = rings[r + end][0], ringRadius = rings[r + end][1];
                points[c].position = frame.At(ringRadius * cs, height, ringRadius * sn * depthScale);
                points[c].normal = frame.Along(slope[end][0] * cs, slope[end][1], slope[end][0] * sn / depthScale);
                float u = round[end][arounds[c]], v = along[r + end];
                if (isCone)
                {
                    const float fromApex = apexStart + end * slant;
                    const float phi = (ringRadius > 1e-6f ? u / ringRadius : angle) * widening / slant;
                    u = fromApex * sinf(phi);
                    v = along[r] + fromApex * cosf(phi) - apexStart;
                }
                points[c].s = u / metersU;
                points[c].t = v / metersV;
            }
            MappedPolygon(mesh, points, 4, finish, false);
        }
    }
}

void Loft(Mesh& mesh, const Vec3* first, const Vec3* second, int count, int steps, int finish)
{
    if (count < 2 || steps < 1)
    {
        return;
    }
    constexpr int MOST = 128;
    const int n = count < MOST ? count : MOST;
    const float metersU = MetersU(mesh, finish), metersV = MetersV(mesh, finish);
    const auto point = [&](int i, int step)
    {
        return Lerp(first[i], second[i], static_cast<float>(step) / steps);
    };
    const auto normal = [&](int i, int step)
    {
        const int i0 = i > 0 ? i - 1 : i, i1 = i + 1 < n ? i + 1 : i;
        const int s0 = step > 0 ? step - 1 : step, s1 = step < steps ? step + 1 : step;
        Vec3 nrm = Normalized(Cross(point(i1, step) - point(i0, step), point(i, s1) - point(i, s0)));
        if (Dot(nrm, mesh.eye - point(i, step)) < 0)
        {
            nrm = -nrm;
        }
        return nrm;
    };
    // Each point mapped by its place in a local frame of the band: along the middle row's direction (u, from the
    // middle row's running distance) and across it in the surface (v, from the first line): a flat tapering or skewed
    // band is mapped exactly as its plan (no stretch, no shear: until b320 every row ran from 0 by its own length),
    // a curved one nearly so; neighbouring quads share each point's u and v.
    for (int step = 0; step < steps; ++step)
    {
        float middle[MOST];
        Vec3 tangents[MOST];
        const float half = (step + 0.5f) / steps;
        middle[0] = 0;
        for (int i = 0; i < n; ++i)
        {
            if (i > 0)
            {
                middle[i] = middle[i - 1] + Length(Lerp(first[i], second[i], half) - Lerp(first[i - 1], second[i - 1], half));
            }
            const int i0 = i > 0 ? i - 1 : i, i1 = i + 1 < n ? i + 1 : i;
            tangents[i] = Normalized(Lerp(first[i1], second[i1], half) - Lerp(first[i0], second[i0], half));
        }
        for (int i = 0; i + 1 < n; ++i)
        {
            MapPoint points[4];
            const int is[4] = {i, i + 1, i + 1, i};
            const int rows[4] = {0, 0, 1, 1};
            for (int c = 0; c < 4; ++c)
            {
                const int k = is[c], row = step + rows[c];
                const Vec3 center = Lerp(first[k], second[k], half), rung = second[k] - first[k];
                const Vec3 across = Normalized(rung - tangents[k] * Dot(rung, tangents[k]));
                points[c].position = point(k, row);
                points[c].normal = normal(k, row);
                points[c].s = (middle[k] + Dot(points[c].position - center, tangents[k])) / metersU;
                points[c].t = Dot(points[c].position - first[k], across) / metersV;
            }
            MappedPolygon(mesh, points, 4, finish, false);
        }
    }
}

void Grille(
    Mesh& mesh,
    const Frame& frame,
    float halfWidth,
    float halfHeight,
    float border,
    int slats,
    float depth,
    int frameFinish,
    int slatFinish)
{
    Panel(mesh, frame, halfWidth, halfHeight, border, depth, frameFinish, slatFinish, 0, frameFinish);
    const float iw = halfWidth - border, ih = halfHeight - border;
    for (int slat = 0; slat < slats; ++slat)
    {
        const float y = -ih + (2 * ih) * (slat + 0.5f) / slats;
        const float half = ih / slats * 0.8f;
        // A louvre: tilted down toward the viewer, mapped along its length.
        PlanarQuad(
            mesh,
            frame.At(-iw, y + half, depth * 0.2f),
            frame.At(iw, y + half, depth * 0.2f),
            frame.At(iw, y - half, depth * 0.9f),
            frame.At(-iw, y - half, depth * 0.9f),
            slatFinish);
    }
}

} // namespace ShellKit

} // namespace SwbfVr
