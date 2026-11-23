// ShellDesigns.cpp: the cockpit shells of the stock tanks (see ShellDesigns.h).
//
// Each design starts from the game's first-person cockpit model (BFBuilder Assets/Sides/*/MSH/*_1st_cockpit_*.msh,
// studied with tools/mshgeo.py and tools/meshview.py): its open rims are copied here as point lists in the model's
// space and the shell's walls, floor and roof start just behind them (pushed away from the eye and tucked under the
// game's edge, so that edge always hides the joint). Nothing of the shell may lie in front of the cockpit's window
// (its glass is drawn see-through); tools/shellcheck.py checks both from the eye. Measures that matter for the
// player: the eye at (0, 1.5, 1.5), the seat cushion 0.8 m under it, the levers' pivots at (+-0.27, 0.85, 1.15) on
// consoles beside the cushion (LeverPlacement.h).
//
// Texture patches are picked from each cockpit's own texture (and its effects texture for screens and lamps), each
// with the real size it stands for, so every surface shows the texture at about 200 texels a metre, unstretched
// (ShellKit's mapping).

#include "ShellDesigns.h"

#include "GameMeshes.h"
#include "LeverPlacement.h"
#include "StockTables.h"

#include <cmath>
#include <cstring>
#include <iterator>

namespace SwbfVr
{

namespace ShellDesigns
{

namespace
{

using namespace ShellKit;

constexpr float PI = 3.14159265f;
/// The eye in every stock cockpit model as the game draws it: the rims are pushed away from it (Pushed), and the gunner
/// seats built at it are moved from it.
const Vec3 GAME_EYE = {LeverPlacement::COCKPIT_EYE[0], LeverPlacement::COCKPIT_EYE[1], LeverPlacement::COCKPIT_EYE[2]};
/// Where each shell seats its pilot (PLAN 57 e, idren2023 2026-10-18: closer to the front window in every cabin; headset
/// run 16 found the windows 2.3 to 3 m ahead): 0.8 m ahead of the game's eye in the tanks, 0.4 m in the MTT and the
/// combat speeder (its canopy comes down close over the head, the MTT's gantry ends 0.95 m ahead of the old eye).
/// Aim draws the cockpit model so that this eye is at the game camera (PilotEye).
/// Every surface of the cockpits' and exteriors' textures is mapped at this density (idren2023 2026-10-19: texels
/// consistently sized and unstretched; the game's own cockpits show about 120-130, with half their area stretched).
constexpr float TEXELS_PER_METER = 160.0f;
constexpr Vec3 TANK_PILOT_EYE = {0.0f, 1.5f, 0.7f};
constexpr Vec3 MTT_PILOT_EYE = {0.0f, 1.5f, 1.1f};
constexpr Vec3 SPEEDER_PILOT_EYE = {0.0f, 1.5f, 1.1f};
/// The pilot's eye of the design being built (its eye field): the seat, the front console and the faces turned to the
/// eye are placed from it.
Vec3 _eye = GAME_EYE;

// ---- Shared helpers ----

// A rim point of the game's cockpit pushed away from the eye, so the game's edge covers the joint.
Vec3 Pushed(Vec3 point, float factor = 1.015f)
{
    return GAME_EYE + (point - GAME_EYE) * factor;
}

// A smooth curve through the points (Catmull-Rom, ends held), `perSegment` points a stretch. Returns the count.
int Smooth(const Vec3* points, int count, int perSegment, Vec3* out, int most)
{
    int written = 0;
    for (int i = 0; i + 1 < count && written < most; ++i)
    {
        const Vec3 p0 = points[i > 0 ? i - 1 : i], p1 = points[i], p2 = points[i + 1];
        const Vec3 p3 = points[i + 2 < count ? i + 2 : i + 1];
        for (int step = 0; step < perSegment && written < most; ++step)
        {
            const float t = static_cast<float>(step) / perSegment, t2 = t * t, t3 = t2 * t;
            out[written++] = (p1 * 2.0f + (p2 - p0) * t + (p0 * 2.0f - p1 * 5.0f + p2 * 4.0f - p3) * t2 +
                              (p1 * 3.0f - p0 - p2 * 3.0f + p3) * t3) *
                             0.5f;
        }
    }
    if (written < most)
    {
        out[written++] = points[count - 1];
    }
    return written;
}

// The same curve with every point's z set.
void AtDepth(const Vec3* curve, int count, float z, Vec3* out)
{
    for (int i = 0; i < count; ++i)
    {
        out[i] = {curve[i].x, curve[i].y, z};
    }
}

// Every vertex written since `from` moved by an offset (to place a part built at the driver's spot elsewhere).
void MoveSince(Mesh& mesh, int from, Vec3 offset)
{
    for (int i = from; i < mesh.count; ++i)
    {
        mesh.vertices[i].position = mesh.vertices[i].position + offset;
    }
}

// Every vertex written since `from` turned about the vertical line through pivot by yawRadians (positive turns +z
// toward +x), normals with it.
void TurnSince(Mesh& mesh, int from, Vec3 pivot, float yawRadians)
{
    const float c = cosf(yawRadians), s = sinf(yawRadians);
    for (int i = from; i < mesh.count; ++i)
    {
        Vertex& v = mesh.vertices[i];
        const Vec3 d = v.position - pivot;
        v.position = pivot + Vec3{d.x * c + d.z * s, d.y, -d.x * s + d.z * c};
        v.normal = {v.normal.x * c + v.normal.z * s, v.normal.y, -v.normal.x * s + v.normal.z * c};
    }
}

void RoundedBar(Mesh& mesh, const Frame& frame, float halfLength, float halfHeight, float halfDepth, float radius, int finish);

/// How a seat looks (each tank its own).
struct SeatLook
{
    float halfWidth;   // cushion
    float backHeight;  // backrest above the cushion
    float tiltRadians; // backrest leaning back
    bool hasWings;     // side wings at shoulder height (Imperial)
    bool isForDroid;   // a battle droid's seat (CIS): DroidSeat
    int pad, frame, metal, accent, dark;
};

// A battle droid's seat (idren2023 2026-10-19: no cushions, no headrest, a low back, but solid, not a cheap chair; it
// also leaves the cabin in view): under the eye (its x too), a thick moulded metal pan at the cushion's height (0.8 m
// under the eye) with a rolled front edge, a low rounded shell back wrapping round the pan's rear (an arc of 150 deg,
// 0.30 m high, leaning back, its top edge rolled), on a heavy pedestal with a flared foot.
void DroidSeat(Mesh& mesh, Vec3 eye, float floorY, const SeatLook& look)
{
    const Vec3 up = {0, 1, 0};
    const float top = eye.y - 0.80f, front = eye.z - 0.26f, back = eye.z + 0.14f, w = look.halfWidth - 0.02f;
    const float middleZ = (front + back) * 0.5f;
    Box(mesh,
        MakeFrame({eye.x, top - 0.04f, middleZ}, {1, 0, 0}, up),
        {w, 0.04f, (back - front) * 0.5f},
        0.025f,
        look.frame);
    const Vec3 lip[2] = {{eye.x - w + 0.02f, top - 0.02f, front}, {eye.x + w - 0.02f, top - 0.02f, front}};
    Tube(mesh, lip, 2, 0.024f, 10, look.metal);
    // The shell back: an arc round the pan's rear, inner and outer faces and a rolled top edge.
    constexpr int STEPS = 12;
    constexpr float HEIGHT = 0.30f, LEAN = 0.06f, THICK = 0.025f, SPAN = 150.0f * PI / 180.0f;
    const float radius = w, centreZ = back - radius * 0.55f;
    Vec3 innerLow[STEPS + 1], innerHigh[STEPS + 1], outerLow[STEPS + 1], outerHigh[STEPS + 1], rim[STEPS + 1];
    for (int step = 0; step <= STEPS; ++step)
    {
        const float angle = -SPAN * 0.5f + SPAN * step / STEPS; // 0 straight back
        const float sx = sinf(angle), sz = cosf(angle);
        const Vec3 base = {eye.x + radius * sx, top - 0.06f, centreZ + radius * sz * 0.75f};
        const Vec3 lean = {sx * LEAN * 0.3f, HEIGHT, sz * LEAN};
        const Vec3 outward = Normalized(Vec3{sx, 0, sz});
        innerLow[step] = base;
        innerHigh[step] = base + lean;
        outerLow[step] = base + outward * THICK;
        outerHigh[step] = base + lean + outward * THICK;
        rim[step] = base + lean + outward * (THICK * 0.5f);
    }
    Loft(mesh, innerLow, innerHigh, STEPS + 1, 2, look.frame);
    Loft(mesh, outerLow, outerHigh, STEPS + 1, 2, look.frame);
    Tube(mesh, rim, STEPS + 1, 0.02f, 8, look.metal);
    for (int end = 0; end <= STEPS; end += STEPS)
    {
        const Vec3 edge[2] = {
            innerHigh[end] + (outerHigh[end] - innerHigh[end]) * 0.5f,
            innerLow[end] + (outerLow[end] - innerLow[end]) * 0.5f};
        Tube(mesh, edge, 2, 0.018f, 8, look.metal);
    }
    // Pedestal: a heavy column on a flared foot.
    const Frame foot = MakeFrame({eye.x, floorY, middleZ}, {1, 0, 0}, up); // Lathe turns about the frame's y
    const float flare[4][2] = {{0, 0.20f}, {0.03f, 0.20f}, {0.10f, 0.07f}, {top - 0.08f - floorY, 0.06f}};
    Lathe(mesh, foot, flare, 4, 16, look.metal);
}

// A seat, centred under an eye: cushion top 0.8 m below the eye, from 0.30 ahead of it to 0.19 behind; the backrest
// from there, the headrest behind the head; a pedestal down to the floor (platform) at floorY. A droid's seat
// (look.isForDroid) is DroidSeat.
void Seat(Mesh& mesh, Vec3 eye, float floorY, const SeatLook& look)
{
    if (look.isForDroid)
    {
        DroidSeat(mesh, eye, floorY, look);
        return;
    }
    const float top = eye.y - 0.80f, front = eye.z - 0.30f, back = eye.z + 0.19f;
    const float w = look.halfWidth, midZ = (front + back) * 0.5f, halfDepth = (back - front) * 0.5f;
    // Cushion: a padded slab, its sides and front rolled (bolsters), two stitched channels along it.
    RoundedBar(mesh, MakeFrame({0, top - 0.055f, midZ}, {1, 0, 0}, {0, 1, 0}), w - 0.035f, 0.05f, halfDepth, 0.04f, look.pad);
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 roll[2] = {
            {side * (w - 0.03f), top - 0.03f, back - 0.02f},
            {side * (w - 0.03f), top - 0.03f, front + 0.03f}};
        Tube(mesh, roll, 2, 0.04f, 10, look.pad);
        const Vec3 stitch[2] = {
            {side * 0.07f, top + 0.001f, back - 0.04f},
            {side * 0.07f, top + 0.001f, front + 0.06f}};
        Tube(mesh, stitch, 2, 0.004f, 5, look.dark, false);
    }
    const Vec3 frontRoll[2] = {{-w + 0.06f, top - 0.045f, front + 0.02f}, {w - 0.06f, top - 0.045f, front + 0.02f}};
    Tube(mesh, frontRoll, 2, 0.035f, 10, look.pad);
    // Cushion pan and its frame underneath.
    Box(mesh,
        MakeFrame({0, top - 0.13f, midZ}, {1, 0, 0}, {0, 1, 0}),
        {w, 0.025f, halfDepth + 0.02f},
        0.01f,
        look.frame);
    // Backrest: leaning back from the cushion's rear.
    const float c = cosf(look.tiltRadians), s = sinf(look.tiltRadians);
    const float bh = look.backHeight * 0.5f;
    const Frame backFrame = MakeFrame({0, top + bh * c, back + 0.04f + bh * s}, {1, 0, 0}, {0, c, s});
    if (look.isForDroid)
    {
        // CIS: a pod-like shell (half a lathe, its open side toward the pilot) holding the pad.
        float rings[8][2];
        for (int r = 0; r < 8; ++r)
        {
            const float t = static_cast<float>(r) / 7.0f;
            rings[r][0] = t * (2 * bh + 0.08f);
            rings[r][1] = (w + 0.04f) * (0.80f + 0.20f * sinf(PI * (0.2f + 0.8f * t)));
        }
        Frame pod = Offset(backFrame, 0, -bh - 0.03f, 0.05f);
        pod.z = -pod.z;
        pod.x = -pod.x;
        Lathe(mesh, pod, rings, 8, 18, look.frame, 0.32f);
    }
    else
    {
        Box(mesh, Offset(backFrame, 0, 0, 0.045f), {w + 0.01f, bh + 0.02f, 0.02f}, 0.012f, look.frame);
    }
    RoundedBar(mesh, backFrame, w - 0.03f, bh, 0.04f, 0.035f, look.pad);
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 bolster[2] = {
            backFrame.At(side * (w - 0.02f), -bh + 0.03f, -0.035f),
            backFrame.At(side * (w - 0.02f), bh - 0.04f, -0.035f)};
        Tube(mesh, bolster, 2, 0.045f, 10, look.pad);
        const Vec3 stitch[2] = {
            backFrame.At(side * 0.07f, -bh + 0.05f, -0.0405f),
            backFrame.At(side * 0.07f, bh - 0.05f, -0.0405f)};
        Tube(mesh, stitch, 2, 0.004f, 5, look.dark, false);
        if (look.hasWings)
        {
            // Imperial: wings round the shoulders, angled in.
            const Frame wing = MakeFrame(
                backFrame.At(side * (w + 0.02f), bh * 0.45f, -0.08f),
                backFrame.Along(side * 0.35f, 0, -1),
                backFrame.y);
            Box(mesh, wing, {0.07f, bh * 0.45f, 0.018f}, 0.01f, look.frame);
        }
    }
    // Headrest behind the head (its face 0.17 m behind the eye), rounded, on two posts.
    const Frame head = MakeFrame({0, eye.y + 0.02f, eye.z + 0.22f}, {1, 0, 0}, {0, 1, 0.08f});
    RoundedBar(mesh, head, 0.115f, 0.08f, 0.045f, 0.042f, look.pad);
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 post[2] = {head.At(side * 0.07f, -0.07f, 0.01f), backFrame.At(side * 0.07f, bh - 0.02f, 0.01f)};
        Tube(mesh, post, 2, 0.011f, 8, look.metal);
    }
    // Harness: two straps over the shoulders down the backrest, each with a buckle.
    for (int side = -1; side <= 1; side += 2)
    {
        const float strap[4][2] = {{-0.022f, 0}, {0.022f, 0}, {0.022f, 0.004f}, {-0.022f, 0.004f}};
        Vec3 path[5];
        for (int i = 0; i < 5; ++i)
        {
            const float t = static_cast<float>(i) / 4.0f;
            path[i] = backFrame.At(side * (0.09f - 0.02f * t), bh + 0.02f - t * (1.6f * bh), -0.046f - 0.01f * t);
        }
        Sweep(mesh, path, 5, strap, 4, true, false, backFrame.z, look.accent, true);
        Box(mesh,
            Frame{path[4] + backFrame.z * (-0.006f), backFrame.x, backFrame.y, backFrame.z},
            {0.03f, 0.022f, 0.006f},
            0.004f,
            look.metal);
    }
    // Pedestal: a column from the floor to the pan, a foot plate, two rails.
    const float rings[6][2] = {
        {0, 0.09f},
        {0.02f, 0.09f},
        {0.03f, 0.06f},
        {top - 0.15f - floorY - 0.03f, 0.055f},
        {top - 0.15f - floorY - 0.01f, 0.08f},
        {top - 0.15f - floorY, 0.08f}};
    Lathe(mesh, MakeFrame({0, floorY, midZ}, {1, 0, 0}, {0, 1, 0}), rings, 6, 16, look.metal);
    for (int side = -1; side <= 1; side += 2)
    {
        Box(mesh,
            MakeFrame({side * 0.12f, floorY + 0.015f, midZ}, {1, 0, 0}, {0, 1, 0}),
            {0.02f, 0.015f, halfDepth + 0.12f},
            0.005f,
            look.frame);
    }
}

// A picture of the texture put whole onto a rectangle in the frame's x-y plane, 1 mm before it (s, t: the part of
// the patch it shows).
void Picture(
    Mesh& mesh,
    const Frame& frame,
    float halfWidth,
    float halfHeight,
    int finish,
    float s0 = 0,
    float t0 = 0,
    float s1 = 1,
    float t1 = 1)
{
    Quad(
        mesh,
        frame.At(-halfWidth, halfHeight, 0.001f),
        frame.At(halfWidth, halfHeight, 0.001f),
        frame.At(halfWidth, -halfHeight, 0.001f),
        frame.At(-halfWidth, -halfHeight, 0.001f),
        finish,
        s0,
        t0,
        s1,
        t1);
}

// ---- Trim (the cleanup pass, idren2023 2026-10-17: about 2 000 triangles a shell) ----

// A frame at origin whose z faces the eye (for bolt heads and studs on curved trim).
Frame FacingEye(Vec3 origin)
{
    const Vec3 z = Normalized(_eye - origin);
    const Vec3 x = fabsf(z.y) > 0.95f ? Vec3{1, 0, 0} : Normalized(Cross({0, 1, 0}, z));
    return {origin, x, Cross(z, x), z};
}

// A moulding over a seam where the shell meets the game's cockpit: a round bead along the rim (pushed away from the
// eye as the shell's own joints are, so it covers the joint from wherever a head can move), smoothed `perSegment`
// times a stretch (1: straight), with a bolt head facing the eye every boltSpacing metres (0: none).
void SeamTrim(
    Mesh& mesh,
    const Vec3* rim,
    int count,
    int perSegment,
    float radius,
    int sides,
    float boltSpacing,
    int finish,
    int boltFinish)
{
    Vec3 pushed[48], path[192];
    count = count < 48 ? count : 48;
    for (int i = 0; i < count; ++i)
    {
        pushed[i] = Pushed(rim[i], 1.012f);
    }
    int pathCount = count;
    if (perSegment > 1)
    {
        pathCount = Smooth(pushed, count, perSegment, path, 192);
    }
    else
    {
        memcpy(path, pushed, sizeof(Vec3) * count);
    }
    Tube(mesh, path, pathCount, radius, sides, finish);
    if (boltSpacing <= 0)
    {
        return;
    }
    float along = boltSpacing * 0.5f;
    for (int i = 0; i + 1 < pathCount; ++i)
    {
        const float length = Length(path[i + 1] - path[i]);
        while (along < length)
        {
            const Vec3 at = Lerp(path[i], path[i + 1], along / length);
            const Frame bolt = FacingEye(at);
            Rivet(mesh, Offset(bolt, 0, 0, radius * 0.85f), radius * 0.45f, boltFinish);
            along += boltSpacing;
        }
        along -= length;
    }
}

// A bevelled strip from a to b (a skirting board, a cornice, a deck seam): `width` across in the direction of
// `across`, `depth` out along `out`.
void Strip(Mesh& mesh, Vec3 a, Vec3 b, Vec3 across, Vec3 out, float width, float depth, int finish)
{
    const Vec3 along = b - a;
    const Frame frame = MakeFrame((a + b) * 0.5f + Normalized(out) * (depth * 0.5f), along, across);
    Box(mesh, frame, {Length(along) * 0.5f, width * 0.5f, depth * 0.5f}, fminf(width, depth) * 0.3f, finish);
}

// A hatch let into a floor: a framed plate with a recessed grip, its frame at the floor.
void FloorHatch(Mesh& mesh, Vec3 center, float halfWidth, float halfDepth, int frameFinish, int plateFinish, int metal)
{
    const Frame hatch = MakeFrame(center + Vec3{0, 0.004f, 0}, {1, 0, 0}, {0, 0, -1});
    Panel(mesh, hatch, halfWidth, halfDepth, 0.035f, 0.012f, frameFinish, plateFinish, 0, metal);
    Box(mesh, Offset(hatch, 0, halfDepth * 0.55f, 0.004f), {0.07f, 0.018f, 0.006f}, 0.003f, metal);
}

// A seam line in a floor: a dark strip flush with it, 2 mm above, from a to b, `width` wide (two triangles).
void DeckSeam(Mesh& mesh, Vec3 a, Vec3 b, float width, int finish)
{
    const Vec3 side = Normalized(Cross(b - a, {0, 1, 0})) * (width * 0.5f), up = {0, 0.002f, 0};
    Quad(mesh, a - side + up, b - side + up, b + side + up, a + side + up, finish);
}

// ---- The HUD on the dashboard (idren2023 2026-10-17) ----

/// HudDraws::Group numbers (ShellDesigns does not depend on the HUD code).
constexpr int GROUP_HEALTH = 0, GROUP_AMMUNITION = 1, GROUP_TARGET = 2, GROUP_SCORE = 3, GROUP_MAP = 4;

/// The displays of the shell being built (Build hands them out with the shell).
Display _displays[MOST_DISPLAYS];
int _displayCount = 0;
/// The readout colour AddDisplay gives its displays (0: the game's colours) and the stock screen's glass finish and
/// alpha floor (scan lines over the readouts; -1: none); set while a stock screen's displays are added
/// (SetDisplayScreen).
uint32_t _displayTint = 0;
/// The stock screens' glass of the shell being built (Build hands them out with the shell).
ScanArea _scanAreas[MOST_SCAN_AREAS];
int _scanAreaCount = 0;

/// The depth mask of the shell being built (Shell::maskVertices; Build hands it out with the shell).
Vec3 _mask[MOST_MASK_VERTICES];
int _maskCount = 0;

// Triangles (three points each) added to the depth mask that hides parts of the game's cockpit model.
void AddMask(const Vec3* points, int count)
{
    for (int i = 0; i + 2 < count && _maskCount + 3 <= MOST_MASK_VERTICES; i += 3)
    {
        for (int k = 0; k < 3; ++k)
        {
            _mask[_maskCount++] = points[i + k];
        }
    }
}

void AddDisplay(int group, const Frame& face, float x, float y, float width, float height, int part = 0)
{
    if (_displayCount < MOST_DISPLAYS)
    {
        _displays[_displayCount++] =
            {group, face.At(x, y, 0.0025f), face.x, face.y, width, height, part, _displayTint, 0, 0, 1, 1};
    }
}

/// A part of a group on a display (Display region*: fractions of the group's drawn bounds, 0 left / bottom; batches by
/// their middle).
struct GroupRegion
{
    float left, bottom, right, top;
};

void AddDisplayRegion(
    int group,
    const GroupRegion& region,
    const Frame& face,
    float x,
    float y,
    float width,
    float height,
    int part)
{
    AddDisplay(group, face, x, y, width, height, part);
    if (_displayCount > 0)
    {
        Display& display = _displays[_displayCount - 1];
        display.regionLeft = region.left;
        display.regionBottom = region.bottom;
        display.regionRight = region.right;
        display.regionTop = region.top;
    }
}

/// The HealthGroup's two parts in a vehicle (run324, `shell groups`: 118 x 49 units, ten batches): the player's bar,
/// its frame and the soldier's hexagon in the upper part (batch middles 0.67..0.83 of the height), the vehicle's bar,
/// frame, hexagon and marks in the lower (0.19..0.51).
constexpr GroupRegion PLAYER_HEALTH = {0, 0.6f, 1, 1};
constexpr GroupRegion VEHICLE_HEALTH = {0, 0, 1, 0.6f};

// ---- Stock screens (PLAN 57 j, idren2023 2026-10-19: every tank's HUD as computer screens in its dashboard, in the
// style of the speeder gunner's periscope display) ----

/// A side's stock gunner screen, <side>_1st_cockpit_securitycam (512 x 512 texels, the screen's left half, which the
/// stock frame mirrors about its right edge; texels from the top left): the frame's outer left, top and bottom edges,
/// the glass's left, top and bottom edges (where the texture's alpha falls below 200 across most of a row or column),
/// how far the glass's corner shape reaches in (rounded, or the Imperial's cut corners), the top left of a plain square
/// of its glass (SCREEN_GLASS_PATCH texels: four periods of its scan lines in the alpha, no reticle art in the colour;
/// the square of least colour variance), the dark behind the glass, and the colour the HUD's readouts take on it.
/// Measured from the mod tools' TGAs (BFBuilder Assets/Sides/<SIDE>/MSH), the art the game loads.
struct ScreenArt
{
    float outerLeft, outerTop, outerBottom, glassLeft, glassTop, glassBottom, cornerInset, plainLeft, plainTop;
    uint32_t backing, readout;
    float glassAlphaFloor; // the glass's alpha between its scan lines (the lowest in the plain square)
    uint32_t glassTint;    // the glass finish's colour (the texture's own at 0x808080)
};

constexpr ScreenArt ALL_SCREEN_ART = {16, 48, 481, 164, 124, 401, 16, 176, 128, 0x04200A, 0x70FF90, 0.27f, 0x808080};
constexpr ScreenArt CIS_SCREEN_ART = {24, 40, 488, 157, 105, 431, 12, 456, 234, 0x260806, 0xFF7A5E, 0.15f, 0xFF3828};
constexpr ScreenArt IMP_SCREEN_ART = {44, 14, 492, 152, 70, 427, 64, 248, 86, 0x08183A, 0xC8F0FF, 0.27f, 0x808080};
constexpr ScreenArt REP_SCREEN_ART = {40, 30, 480, 139, 82, 387, 18, 163, 94, 0x1A2002, 0xDCF050, 0.27f, 0x808080};

constexpr float SCREEN_TEXELS = 512.0f, SCREEN_GLASS_PATCH = 28.0f;
/// One plain glass square covers this much of a screen (its scan lines 5 mm apart: visible, not a moire).
constexpr float SCREEN_GLASS_TILE_METERS = 0.02f;
/// The frame's texels on the dashboards' screens (the column 2-3 cm wide).
constexpr float SCREEN_TEXEL_METERS = 0.0002f;

/// The finishes of a stock screen (texture 5, the design's screenName): its frame (whole texture, cut by its alpha),
/// its glass (the plain square tiled, cut by its alpha: the scan lines) and the backing.
Finish ScreenFrameFinish()
{
    return {{0, 0, 1, 1}, 1, 5, 0x808080, 0.25f, 16, 0.1f, 0.999f};
}

Finish ScreenGlassFinish(const ScreenArt& art)
{
    const float u0 = art.plainLeft / SCREEN_TEXELS, v0 = art.plainTop / SCREEN_TEXELS;
    const float size = SCREEN_GLASS_PATCH / SCREEN_TEXELS;
    return {{u0, v0, u0 + size, v0 + size}, SCREEN_GLASS_TILE_METERS, 5, art.glassTint, 0.3f, 30, 0.7f, 0.999f};
}

Finish ScreenBackingFinish(const ScreenArt& art)
{
    return {{0, 0, 1, 1}, 1, 0, art.backing, 0.1f, 8, 0.85f};
}

/// A design's stock screens: the side's art, the finishes made from it, and the finish of a case behind a free
/// standing screen.
struct StockScreenLook
{
    const ScreenArt* art;
    int frame, glass, backing, casing;
};

// A face bent round a vertical axis curveRadius before it (toward the eye): the frame at x along the face, its x the
// curve's tangent, z toward the axis. curveRadius 0: the flat face moved by x.
Frame CurvedFrame(const Frame& face, float x, float curveRadius)
{
    if (curveRadius <= 0)
    {
        return Offset(face, x, 0, 0);
    }
    const float angle = x / curveRadius;
    const Vec3 axis = face.At(0, 0, curveRadius);
    const Vec3 outward = face.x * sinf(angle) - face.z * cosf(angle);
    return MakeFrame(axis + outward * curveRadius, face.x * cosf(angle) + face.z * sinf(angle), face.y);
}

// The displays added next show their groups as this stock screen's readouts (its colour); null: the game's colours.
void SetDisplayScreen(const StockScreenLook* screen)
{
    _displayTint = screen ? screen->art->readout : 0;
}

// The HUD groups on the displays added next drawn in this colour (0xRRGGBB; 0: their own).
void SetDisplayTint(uint32_t tint)
{
    _displayTint = tint;
}

// A stock screen in a face (frame z toward the eye): the side's frame round a glass of these half sizes (metres) in
// nine pieces of the texture, so it keeps its look at any size: corners at metersPerTexel, the bars and columns
// stretched along; the glass's middle a plain square of its own glass tiled (its scan lines) over a dark backing 4 mm
// behind. Frame and glass lie in the face, both drawn as glass (the texture's alpha cuts them). caseDepth > 0: a case
// of that depth behind the whole screen (look.casing). curveRadius > 0: the screen bent round a vertical axis that far
// before it (a curved monitor; CurvedFrame places what sits on it), cut into strips 3 cm wide. hasGlass false: the
// frame alone (a periscope's picture shows through it). Returns the frame's
// outer half width; its top and bottom (from the glass's middle) go to top / bottom when given.
float StockScreen(
    Mesh& mesh,
    const Frame& face,
    float halfWidth,
    float halfHeight,
    float metersPerTexel,
    const StockScreenLook& look,
    float caseDepth = 0,
    float* top = nullptr,
    float* bottom = nullptr,
    float curveRadius = 0,
    bool hasGlass = true)
{
    const ScreenArt& art = *look.art;
    const float inset = art.cornerInset * metersPerTexel;
    const float innerHalfWidth = halfWidth - inset, innerHalfHeight = halfHeight - inset;
    const float outerHalfWidth = halfWidth + (art.glassLeft - art.outerLeft) * metersPerTexel;
    const float ys[4] = {
        halfHeight + (art.glassTop - art.outerTop) * metersPerTexel,
        innerHalfHeight,
        -innerHalfHeight,
        -halfHeight - (art.outerBottom - art.glassBottom) * metersPerTexel};
    const float vs[4] = {
        art.outerTop / SCREEN_TEXELS,
        (art.glassTop + art.cornerInset) / SCREEN_TEXELS,
        (art.glassBottom - art.cornerInset) / SCREEN_TEXELS,
        art.outerBottom / SCREEN_TEXELS};
    const auto at = [&](float x, float y, float z)
    {
        return CurvedFrame(face, x, curveRadius).At(0, y, z);
    };
    constexpr float STRIP_METERS = 0.03f;
    const auto stripsOf = [&](float from, float to)
    {
        return curveRadius > 0 ? 1 + static_cast<int>(fabsf(to - from) / STRIP_METERS) : 1;
    };
    // A band from x0 to x1 between two heights, the texture's u running u0 -> u1 along it, in strips when curved.
    const auto band = [&](float x0,
                          float x1,
                          float u0,
                          float u1,
                          float yTop,
                          float yBottom,
                          float vTop,
                          float vBottom,
                          float z,
                          int finish,
                          bool isTiled)
    {
        const int strips = stripsOf(x0, x1);
        for (int strip = 0; strip < strips; ++strip)
        {
            const float t0 = static_cast<float>(strip) / strips, t1 = static_cast<float>(strip + 1) / strips;
            const float xa = x0 + (x1 - x0) * t0, xb = x0 + (x1 - x0) * t1;
            if (isTiled)
            {
                PlanarQuad(mesh, at(xa, yTop, z), at(xb, yTop, z), at(xb, yBottom, z), at(xa, yBottom, z), finish);
            }
            else
            {
                Quad(
                    mesh,
                    at(xa, yTop, z),
                    at(xb, yTop, z),
                    at(xb, yBottom, z),
                    at(xa, yBottom, z),
                    finish,
                    u0 + (u1 - u0) * t0,
                    vTop,
                    u0 + (u1 - u0) * t1,
                    vBottom);
            }
        }
    };
    const float xs[3] = {outerHalfWidth, innerHalfWidth, 0};
    const float us[3] = {art.outerLeft / SCREEN_TEXELS, (art.glassLeft + art.cornerInset) / SCREEN_TEXELS, 1.0f};
    for (int side = -1; side <= 1; side += 2)
    {
        for (int column = 0; column < 2; ++column)
        {
            // Left half: outer -> middle runs left to right; the right half (mirrored) the other way.
            const float x0 = static_cast<float>(side) * xs[column], x1 = static_cast<float>(side) * xs[column + 1];
            const float left = side < 0 ? x0 : x1, right = side < 0 ? x1 : x0;
            const float uLeft = side < 0 ? us[column] : us[column + 1];
            const float uRight = side < 0 ? us[column + 1] : us[column];
            for (int row = 0; row < 3; ++row)
            {
                if (column == 1 && row == 1)
                {
                    continue; // the glass's middle
                }
                band(left, right, uLeft, uRight, ys[row], ys[row + 1], vs[row], vs[row + 1], 0, look.frame, false);
            }
        }
    }
    if (hasGlass)
    {
        // The glass's scan lines over the HUD (CockpitShell): one area flat, one per strip curved.
        const int strips = stripsOf(-halfWidth, halfWidth);
        for (int strip = 0; strip < strips && _scanAreaCount < MOST_SCAN_AREAS; ++strip)
        {
            const float x0 = -halfWidth + 2 * halfWidth * strip / strips;
            const float x1 = -halfWidth + 2 * halfWidth * (strip + 1) / strips;
            const Frame middle = CurvedFrame(face, (x0 + x1) * 0.5f, curveRadius);
            _scanAreas[_scanAreaCount++] = {
                middle.At(0, 0, 0.0035f),
                middle.x,
                middle.y,
                x1 - x0 + 0.002f,
                2 * halfHeight,
                mesh.part,
                look.glass,
                art.glassAlphaFloor};
        }
        band(-innerHalfWidth, innerHalfWidth, 0, 1, innerHalfHeight, -innerHalfHeight, 0, 1, 0, look.glass, true);
        band(-halfWidth, halfWidth, 0, 1, halfHeight, -halfHeight, 0, 1, -0.004f, look.backing, false);
    }
    if (caseDepth > 0)
    {
        const float caseHalfWidth = outerHalfWidth + 0.004f, caseHalfHeight = (ys[0] - ys[3]) * 0.5f + 0.004f;
        const int strips = stripsOf(-caseHalfWidth, caseHalfWidth);
        for (int strip = 0; strip < strips; ++strip)
        {
            const float middle = -caseHalfWidth + caseHalfWidth * 2 * (strip + 0.5f) / strips;
            const float half = caseHalfWidth / strips + (curveRadius > 0 ? 0.002f : 0.0f);
            Box(mesh,
                Offset(CurvedFrame(face, middle, curveRadius), 0, (ys[0] + ys[3]) * 0.5f, -caseDepth * 0.5f - 0.002f),
                {half, caseHalfHeight, caseDepth * 0.5f},
                0.002f,
                look.casing);
        }
    }
    if (top)
    {
        *top = ys[0];
    }
    if (bottom)
    {
        *bottom = ys[3];
    }
    return outerHalfWidth;
}

/// A picture from the cockpit's own texture on a housing's front plate: its finish (mapped whole), its middle and half
/// sizes (metres at scale 1, from the plate's middle; half sizes in the picture's own proportion, so nothing
/// stretches).
struct Decal
{
    int finish;
    float x, y, halfWidth, halfHeight;
};

/// A tank's screens in the front console (idren2023 2026-10-17: a housing for each HUD element, modelled after the tank's
/// interior; 2026-10-18: in the console's front round the slide handles): their size, the finishes, and the cockpit's
/// own artwork on each screen's front plate. The minimap's housing may instead show one picture of the cockpit's own
/// gauge or display as its whole front (mapArt), the map in its glass: mapArtWidth metres wide, mapArtAspect its width
/// / height, the map's middle mapArtX / mapArtY from the picture's middle and its size mapArtSize, as fractions of the
/// picture's width.
struct DashboardLook
{
    float scale;
    int body, bezel, face, metal, lamp;
    Decal left[4], right[4], map[3];
    int mapArt = -1;
    float mapArtWidth = 0, mapArtAspect = 1, mapArtX = 0, mapArtY = 0, mapArtSize = 0;
    /// The picture's own screen, covered with dark glass under the map (fractions of the picture from its top left;
    /// all 0: the picture's glass is dark already).
    float glassLeft = 0, glassTop = 0, glassRight = 0, glassBottom = 0;
};

// A rounded rectangle in the frame's x-y plane at height z, starting in the middle of its bottom edge and going round
// counter-clockwise (seen from +z) back to it: 4 corners of `steps` segments. Returns the count (4 * (steps + 1) + 2).
int RoundedRectangle(
    const Frame& frame,
    float x,
    float y,
    float halfWidth,
    float halfHeight,
    float radius,
    float z,
    int steps,
    Vec3* out)
{
    int count = 0;
    out[count++] = frame.At(x, y - halfHeight, z);
    const float corners[4][2] = {{1, -1}, {1, 1}, {-1, 1}, {-1, -1}};
    for (int corner = 0; corner < 4; ++corner)
    {
        const float cx = x + corners[corner][0] * (halfWidth - radius),
                    cy = y + corners[corner][1] * (halfHeight - radius);
        const float start = -PI * 0.5f + PI * 0.5f * corner;
        for (int step = 0; step <= steps; ++step)
        {
            const float angle = start + PI * 0.5f * step / steps;
            out[count++] = frame.At(cx + radius * cosf(angle), cy + radius * sinf(angle), z);
        }
    }
    out[count++] = out[0];
    return count;
}

// One screen facet of the front console, its front plate in the frame's x-y plane (z toward the eye): a bevelled body
// `depth` deep behind the plate, the screen (middle screenX / screenY, half sizes) as dark glass 3 mm before the plate
// inside a raised moulded bezel with rounded corners, a hood over the top leaning out. Sizes in metres (already
// scaled). With a stock screen (PLAN 57 j) the screen is that instead of the glass and the moulded bezel.
void ScreenFacet(
    Mesh& mesh,
    const Frame& plate,
    float halfWidth,
    float halfHeight,
    float screenX,
    float screenY,
    float screenHalfWidth,
    float screenHalfHeight,
    float depth,
    const DashboardLook& look,
    const StockScreenLook* screen)
{
    Box(mesh, Offset(plate, 0, 0, -depth * 0.5f), {halfWidth, halfHeight, depth * 0.5f}, 0.012f, look.body);
    const Frame hood = MakeFrame(plate.At(0, halfHeight + 0.004f, 0.02f), plate.x, plate.y + plate.z * 0.35f);
    if (screen)
    {
        StockScreen(
            mesh,
            Offset(plate, screenX, screenY, 0.003f),
            screenHalfWidth,
            screenHalfHeight,
            SCREEN_TEXEL_METERS * screenHalfHeight / 0.0675f,
            *screen);
        Box(mesh, hood, {halfWidth + 0.006f, 0.005f, 0.045f}, 0.003f, look.bezel);
        const Vec3 lip[2] = {hood.At(-halfWidth - 0.006f, 0, 0.045f), hood.At(halfWidth + 0.006f, 0, 0.045f)};
        Tube(mesh, lip, 2, 0.007f, 6, look.bezel);
        return;
    }
    Quad(
        mesh,
        plate.At(screenX - screenHalfWidth - 0.004f, screenY + screenHalfHeight + 0.004f, 0.003f),
        plate.At(screenX + screenHalfWidth + 0.004f, screenY + screenHalfHeight + 0.004f, 0.003f),
        plate.At(screenX + screenHalfWidth + 0.004f, screenY - screenHalfHeight - 0.004f, 0.003f),
        plate.At(screenX - screenHalfWidth - 0.004f, screenY - screenHalfHeight - 0.004f, 0.003f),
        look.face);
    // Bezel: a ridge 2 cm wide and 1 cm high whose inner foot is the screen's edge.
    const float ridge[5][2] = {{-0.010f, 0.0f}, {-0.008f, 0.008f}, {-0.004f, 0.011f}, {0.006f, 0.010f}, {0.010f, 0.0f}};
    Vec3 path[24];
    const int pathCount = RoundedRectangle(
        plate,
        screenX,
        screenY,
        screenHalfWidth + 0.010f,
        screenHalfHeight + 0.010f,
        0.016f,
        0,
        3,
        path);
    Sweep(mesh, path, pathCount, ridge, 5, false, false, plate.z, look.bezel, false);
    // Hood: over the plate's top edge, leaning out over the screen, its front edge rolled.
    Box(mesh, hood, {halfWidth + 0.006f, 0.005f, 0.045f}, 0.003f, look.bezel);
    const Vec3 lip[2] = {hood.At(-halfWidth - 0.006f, 0, 0.045f), hood.At(halfWidth + 0.006f, 0, 0.045f)};
    Tube(mesh, lip, 2, 0.007f, 6, look.bezel);
}

void Decals(Mesh& mesh, const Frame& plate, const Decal* decals, int count, float k)
{
    for (int index = 0; index < count; ++index)
    {
        const Decal& decal = decals[index];
        if (decal.halfWidth > 0)
        {
            Picture(
                mesh,
                Offset(plate, decal.x * k, decal.y * k, 0.0f),
                decal.halfWidth * k,
                decal.halfHeight * k,
                decal.finish);
        }
    }
}

// A round indicator lamp set into a surface (frame y out of it): a bezel ring and a domed lens, `radius` the lens's.
void IndicatorLamp(Mesh& mesh, const Frame& frame, float radius, int bezel, int lens)
{
    const float ring[3][2] = {{0, radius + 0.003f}, {0.0025f, radius + 0.0025f}, {0.0025f, radius}};
    Lathe(mesh, frame, ring, 3, 12, bezel);
    const float dome[3][2] = {{0.0015f, radius}, {0.003f, radius * 0.75f}, {0.0035f, 0}};
    Lathe(mesh, frame, dome, 3, 12, lens);
}

// ---- Detail kit (idren2023 2026-10-19: every cabin to about 50 000 triangles, polished and professionally made; shared
// by the designs, each in its own finishes) ----

/// The finishes a design's details are made in.
struct DetailLook
{
    int body;   // housings, lockers, brackets
    int frame;  // frames, edges, mouldings
    int metal;  // bright metal: pipes, handles, bolts, toggles
    int dark;   // recesses, rubber, cables, seams
    int lamp;   // small self-lit lamps and pointers
    int accent; // a coloured part (an extinguisher's body, placards); may equal body
};

// The frame turned so that its z (out of a surface) becomes y: the axis Lathe and IndicatorLamp turn about.
Frame Upright(const Frame& frame)
{
    return {frame.origin, frame.x, frame.z, -frame.y};
}

// A padded block with rounded long edges (cushions, backrests, headrests): centred on the frame, halfLength along its
// x, its section a rounded rectangle (halfHeight along y, halfDepth along z, corner radius `radius`, 5 steps a
// corner), ends flat.
void RoundedBar(Mesh& mesh, const Frame& frame, float halfLength, float halfHeight, float halfDepth, float radius, int finish)
{
    constexpr int STEPS = 5;
    radius = fminf(radius, fminf(halfHeight, halfDepth) * 0.95f);
    float profile[4 * (STEPS + 1)][2];
    int n = 0;
    const float centres[4][2] = {
        {halfDepth - radius, halfHeight - radius},
        {-halfDepth + radius, halfHeight - radius},
        {-halfDepth + radius, -halfHeight + radius},
        {halfDepth - radius, -halfHeight + radius}};
    for (int corner = 0; corner < 4; ++corner)
    {
        for (int step = 0; step <= STEPS; ++step)
        {
            const float angle = (corner + static_cast<float>(step) / STEPS) * PI * 0.5f;
            profile[n][0] = centres[corner][0] + radius * cosf(angle);
            profile[n][1] = centres[corner][1] + radius * sinf(angle);
            ++n;
        }
    }
    const Vec3 path[2] = {frame.At(-halfLength, 0, 0), frame.At(halfLength, 0, 0)};
    Sweep(mesh, path, 2, profile, n, true, true, frame.y, finish, true);
}

// A loom of `cables` round cables side by side along a smooth curve through the points, each `radius` thick, spread
// across the path toward `across`, a clamp band every clampSpacing metres (0: none).
void CableLoom(
    Mesh& mesh,
    const Vec3* points,
    int count,
    int cables,
    float radius,
    Vec3 across,
    int cableFinish,
    int clampFinish,
    float clampSpacing)
{
    Vec3 path[160];
    int pathCount = count;
    if (count > 2)
    {
        pathCount = Smooth(points, count, 6, path, 160);
    }
    else
    {
        memcpy(path, points, sizeof(Vec3) * count);
    }
    const auto sideAt = [&](int i)
    {
        const Vec3 tangent = Normalized(path[i + 1 < pathCount ? i + 1 : i] - path[i > 0 ? i - 1 : i]);
        return Normalized(across - tangent * Dot(across, tangent));
    };
    for (int cable = 0; cable < cables; ++cable)
    {
        const float offset = (cable - (cables - 1) * 0.5f) * radius * 2.1f;
        // Every other cable lifted a little: the bundle reads as round cables, not a ribbon.
        const float lift = (cable % 2) * radius * 0.7f;
        Vec3 line[160];
        for (int i = 0; i < pathCount; ++i)
        {
            const Vec3 tangent = Normalized(path[i + 1 < pathCount ? i + 1 : i] - path[i > 0 ? i - 1 : i]);
            const Vec3 side = sideAt(i);
            line[i] = path[i] + side * offset + Cross(tangent, side) * lift;
        }
        Tube(mesh, line, pathCount, radius, 8, cableFinish, true);
    }
    if (clampSpacing <= 0)
    {
        return;
    }
    const float halfSpan = cables * radius * 1.08f + 0.006f;
    float along = clampSpacing * 0.5f;
    for (int i = 0; i + 1 < pathCount; ++i)
    {
        const float length = Length(path[i + 1] - path[i]);
        while (along < length)
        {
            const Vec3 at = Lerp(path[i], path[i + 1], along / length);
            const Vec3 tangent = Normalized(path[i + 1] - path[i]);
            const Vec3 side = sideAt(i);
            Box(mesh, MakeFrame(at, side, Cross(tangent, side)), {halfSpan, radius * 1.7f, 0.009f}, 0.003f, clampFinish);
            along += clampSpacing;
        }
        along -= length;
    }
}

// A grab handle: a round bar from a to b standing `standoff` off the surface along `out`, bent down at both ends into
// footed mounts.
void GrabHandle(Mesh& mesh, Vec3 a, Vec3 b, Vec3 out, float standoff, float radius, int barFinish, int footFinish)
{
    const Vec3 o = Normalized(out), run = b - a;
    const Vec3 bar[6] = {
        a,
        a + o * (standoff * 0.75f) + run * 0.02f,
        a + o * standoff + run * 0.12f,
        b + o * standoff - run * 0.12f,
        b + o * (standoff * 0.75f) - run * 0.02f,
        b};
    Vec3 path[48];
    Tube(mesh, path, Smooth(bar, 6, 6, path, 48), radius, 10, barFinish, false);
    for (const Vec3 foot : {a, b})
    {
        const Vec3 x = Normalized(run - o * Dot(run, o));
        const float rings[4][2] = {
            {0, radius * 2.4f}, {0.004f, radius * 2.4f}, {0.008f, radius * 1.6f}, {0.014f, radius * 1.2f}};
        Lathe(mesh, Frame{foot, x, o, Cross(x, o)}, rings, 4, 12, footFinish);
    }
}

// A quilted wall: rows x columns padded cushions in the frame's x-y plane within halfWidth x halfHeight (each a
// bevelled block standing `depth` out along z, `gap` between them over a dark backing).
void QuiltedPanel(
    Mesh& mesh,
    const Frame& frame,
    float halfWidth,
    float halfHeight,
    int rows,
    int columns,
    float depth,
    float gap,
    int padFinish,
    int backFinish)
{
    TiledQuad(
        mesh, frame.At(-halfWidth, -halfHeight, 0.001f), frame.x * (2 * halfWidth), frame.y * (2 * halfHeight), backFinish);
    const float cellW = 2 * halfWidth / columns, cellH = 2 * halfHeight / rows;
    const float bevel = fminf(depth * 0.45f, fminf(cellW, cellH) * 0.2f);
    for (int row = 0; row < rows; ++row)
    {
        for (int column = 0; column < columns; ++column)
        {
            const Frame pad =
                Offset(frame, -halfWidth + cellW * (column + 0.5f), -halfHeight + cellH * (row + 0.5f), depth * 0.5f);
            Box(mesh, pad, {(cellW - gap) * 0.5f, (cellH - gap) * 0.5f, depth * 0.5f}, bevel, padFinish);
        }
    }
}

// A tread plate on a floor: a plate from origin along uSide and vSide (in the floor), raised 4 mm, with rows of short
// raised bars turned alternately (a deck plate's pattern), `pitch` metres apart.
void TreadPlate(Mesh& mesh, Vec3 origin, Vec3 uSide, Vec3 vSide, float pitch, int plateFinish, int barFinish)
{
    Vec3 up = Normalized(Cross(uSide, vSide));
    if (up.y < 0)
    {
        up = -up;
    }
    const Vec3 u = Normalized(uSide), v = Normalized(vSide);
    const float uLength = Length(uSide), vLength = Length(vSide);
    Box(mesh, MakeFrame(origin + (uSide + vSide) * 0.5f + up * 0.002f, u, v), {uLength * 0.5f, vLength * 0.5f, 0.002f}, 0.0015f, plateFinish);
    const int across = static_cast<int>(uLength / pitch), along = static_cast<int>(vLength / pitch);
    for (int i = 0; i < across; ++i)
    {
        for (int j = 0; j < along; ++j)
        {
            const Vec3 at = origin + u * (pitch * (i + 0.5f) + (uLength - across * pitch) * 0.5f) +
                            v * (pitch * (j + 0.5f) + (vLength - along * pitch) * 0.5f) + up * 0.0055f;
            const float turn = (i + j) % 2 ? 0.7854f : -0.7854f;
            const Vec3 barX = u * cosf(turn) + v * sinf(turn);
            Box(mesh, MakeFrame(at, barX, Cross(up, barX)), {pitch * 0.3f, 0.0035f, 0.0015f}, 0, barFinish);
        }
    }
}

// A row of `count` toggle switches on a strip in the frame's x-y plane (z out), `spacing` apart along x: a dark guard
// block, a round boss and a bat lever leaning up; a lamp over every other one.
void ToggleBank(Mesh& mesh, const Frame& frame, int count, float spacing, const DetailLook& look)
{
    Box(mesh, Offset(frame, 0, 0.005f, 0.003f), {spacing * count * 0.5f + 0.012f, 0.032f, 0.003f}, 0.0015f, look.frame);
    for (int k = 0; k < count; ++k)
    {
        const float x = (k - (count - 1) * 0.5f) * spacing;
        const Frame at = Offset(frame, x, -0.006f, 0.006f);
        Box(mesh, Offset(at, 0, 0, 0.004f), {spacing * 0.32f, 0.011f, 0.004f}, 0.0015f, look.dark);
        const float boss[3][2] = {{0.008f, 0.0045f}, {0.012f, 0.0045f}, {0.014f, 0.003f}};
        Lathe(mesh, Upright(at), boss, 3, 10, look.metal);
        const Vec3 bat[2] = {at.At(0, 0, 0.013f), at.At(0, 0.008f, 0.026f)};
        Tube(mesh, bat, 2, 0.0018f, 6, look.metal);
        if (k % 2 == 0)
        {
            IndicatorLamp(mesh, Upright(Offset(frame, x, 0.022f, 0.006f)), 0.004f, look.metal, look.lamp);
        }
    }
}

// A row of `count` rotary knobs on a plate (frame's x-y plane, z out), `spacing` apart: a dark collar, a stepped cap
// and a lit pointer line.
void KnobRow(Mesh& mesh, const Frame& frame, int count, float spacing, float radius, const DetailLook& look)
{
    for (int k = 0; k < count; ++k)
    {
        const Frame at = Offset(frame, (k - (count - 1) * 0.5f) * spacing, 0, 0);
        const float collar[3][2] = {{0, radius * 1.35f}, {0.002f, radius * 1.35f}, {0.003f, radius * 1.1f}};
        Lathe(mesh, Upright(at), collar, 3, 14, look.dark);
        const float knob[6][2] = {
            {0.003f, radius},
            {0.010f, radius},
            {0.013f, radius * 0.92f},
            {0.016f, radius * 0.75f},
            {0.017f, radius * 0.4f},
            {0.0175f, 0.0f}};
        Lathe(mesh, Upright(at), knob, 6, 14, look.frame);
        Box(mesh, Offset(at, 0, radius * 0.45f, 0.0178f), {0.0012f, radius * 0.4f, 0.0006f}, 0, look.lamp);
    }
}

// A wall locker (the frame's z out of the wall, its back at the origin): a bevelled body, a panelled door with hinge
// knuckles, a grab handle, louvres over a placard.
void Locker(Mesh& mesh, const Frame& wall, float halfWidth, float halfHeight, float depth, const DetailLook& look)
{
    Box(mesh, Offset(wall, 0, 0, depth * 0.5f), {halfWidth, halfHeight, depth * 0.5f}, 0.012f, look.body);
    const Frame face = Offset(wall, 0, 0, depth);
    Panel(mesh, face, halfWidth - 0.015f, halfHeight - 0.015f, 0.025f, 0.008f, look.frame, look.body, 0, look.metal);
    for (int k = 0; k < 3; ++k)
    {
        const float y = -halfHeight + 0.05f + k * (halfHeight - 0.05f);
        const Vec3 knuckle[2] = {face.At(-halfWidth + 0.028f, y, 0.012f), face.At(-halfWidth + 0.028f, y + 0.06f, 0.012f)};
        Tube(mesh, knuckle, 2, 0.008f, 8, look.metal);
    }
    GrabHandle(
        mesh,
        face.At(halfWidth - 0.06f, 0.07f, 0.008f),
        face.At(halfWidth - 0.06f, -0.07f, 0.008f),
        face.z,
        0.03f,
        0.007f,
        look.metal,
        look.frame);
    if (halfHeight > 0.12f)
    {
        Grille(
            mesh,
            Offset(face, -0.02f, halfHeight * 0.55f, 0.008f),
            halfWidth * 0.55f,
            halfHeight * 0.18f,
            0.012f,
            4,
            0.008f,
            look.frame,
            look.dark);
    }
    Box(mesh, Offset(face, -0.02f, -halfHeight * 0.45f, 0.01f), {halfWidth * 0.35f, 0.022f, 0.002f}, 0.001f, look.accent);
}

// A fire extinguisher in a wall bracket (the frame's z out of the wall, y up): a cylinder with a domed top in the
// accent finish, a valve and lever, a hose looping down, two straps.
void Extinguisher(Mesh& mesh, const Frame& wall, const DetailLook& look)
{
    const Frame axis = Offset(wall, 0, 0, 0.075f);
    const float body[9][2] = {
        {-0.22f, 0.0f},
        {-0.22f, 0.055f},
        {-0.21f, 0.062f},
        {0.12f, 0.062f},
        {0.15f, 0.055f},
        {0.17f, 0.04f},
        {0.18f, 0.02f},
        {0.19f, 0.016f},
        {0.21f, 0.016f}};
    Lathe(mesh, axis, body, 9, 20, look.accent);
    const float valve[4][2] = {{0.21f, 0.022f}, {0.24f, 0.022f}, {0.25f, 0.012f}, {0.26f, 0.0f}};
    Lathe(mesh, axis, valve, 4, 12, look.metal);
    Box(mesh, Offset(axis, 0.03f, 0.245f, 0), {0.045f, 0.006f, 0.01f}, 0.002f, look.metal);
    const Vec3 hose[5] = {
        axis.At(-0.02f, 0.23f, 0.01f),
        axis.At(-0.07f, 0.20f, 0.03f),
        axis.At(-0.08f, 0.05f, 0.06f),
        axis.At(-0.07f, -0.10f, 0.065f),
        axis.At(-0.05f, -0.16f, 0.06f)};
    Vec3 path[40];
    Tube(mesh, path, Smooth(hose, 5, 8, path, 40), 0.008f, 8, look.dark);
    for (const float y : {-0.12f, 0.08f})
    {
        Box(mesh, Offset(wall, 0, y, 0.01f), {0.075f, 0.018f, 0.01f}, 0.003f, look.frame);
        const float strap[6][2] = {
            {-0.07f, 0.0f}, {-0.066f, 0.05f}, {-0.04f, 0.13f}, {0.04f, 0.13f}, {0.066f, 0.05f}, {0.07f, 0.0f}};
        Vec3 loop[6];
        for (int k = 0; k < 6; ++k)
        {
            loop[k] = wall.At(strap[k][0], y, strap[k][1] + 0.008f);
        }
        const float profile[4][2] = {{-0.012f, -0.0015f}, {0.012f, -0.0015f}, {0.012f, 0.0015f}, {-0.012f, 0.0015f}};
        Vec3 smooth[40];
        Sweep(mesh, smooth, Smooth(loop, 6, 5, smooth, 40), profile, 4, true, false, wall.y, look.dark, true);
    }
}

// A pipe along the points with a flange at each end and joint, on saddle brackets every bracketSpacing metres standing
// it off a wall (`out`: away from the wall).
void FlangedPipe(
    Mesh& mesh,
    const Vec3* points,
    int count,
    float radius,
    Vec3 out,
    float bracketSpacing,
    int pipeFinish,
    int flangeFinish)
{
    Tube(mesh, points, count, radius, 14, pipeFinish);
    for (int i = 0; i < count; ++i)
    {
        const Vec3 tangent = Normalized(points[i + 1 < count ? i + 1 : i] - points[i > 0 ? i - 1 : i]);
        const Vec3 x = Normalized(Cross(tangent, out));
        const float flange[5][2] = {
            {-0.012f, radius * 1.05f},
            {-0.012f, radius * 1.5f},
            {0.012f, radius * 1.5f},
            {0.012f, radius * 1.05f},
            {0.012f, radius}};
        Lathe(mesh, Frame{points[i], x, tangent, Cross(x, tangent)}, flange, 5, 14, flangeFinish);
    }
    for (int i = 0; i + 1 < count; ++i)
    {
        const Vec3 tangent = Normalized(points[i + 1] - points[i]);
        const Vec3 o = Normalized(out - tangent * Dot(out, tangent));
        const int brackets = static_cast<int>(Length(points[i + 1] - points[i]) / bracketSpacing);
        for (int b = 1; b <= brackets; ++b)
        {
            const Vec3 at = Lerp(points[i], points[i + 1], static_cast<float>(b) / (brackets + 1));
            Box(mesh,
                MakeFrame(at - o * (radius * 1.5f), tangent, o),
                {0.012f, radius * 1.2f, radius * 1.3f},
                0.003f,
                flangeFinish);
        }
    }
}

// A hand wheel (a valve's): a rim (a tube round a circle), four spokes and a hub, in the frame's x-y plane, its hub
// standing `standoff` out along z on a stem.
void ValveWheel(Mesh& mesh, const Frame& frame, float radius, float standoff, int rimFinish, int hubFinish)
{
    constexpr int STEPS = 20;
    Vec3 rim[STEPS + 1];
    for (int k = 0; k <= STEPS; ++k)
    {
        const float angle = 2 * PI * k / STEPS;
        rim[k] = frame.At(radius * cosf(angle), radius * sinf(angle), standoff);
    }
    Tube(mesh, rim, STEPS + 1, radius * 0.09f, 8, rimFinish, false);
    for (int spoke = 0; spoke < 4; ++spoke)
    {
        const float angle = PI * 0.25f + PI * 0.5f * spoke;
        const Vec3 bar[2] = {frame.At(0, 0, standoff), frame.At(radius * cosf(angle), radius * sinf(angle), standoff)};
        Tube(mesh, bar, 2, radius * 0.05f, 6, rimFinish, false);
    }
    const float hub[5][2] = {{0, 0.012f}, {standoff - 0.012f, 0.012f}, {standoff - 0.01f, radius * 0.2f}, {standoff + 0.012f, radius * 0.2f}, {standoff + 0.016f, 0}};
    Lathe(mesh, Upright(frame), hub, 5, 12, hubFinish);
}

// A rack of power cells on a wall (the frame's z out, y up): a back plate in a frame, `rows` x `columns` cells lying
// along x in cradles (a body with end caps and a lit band), a retaining bar across each row.
void CellRack(Mesh& mesh, const Frame& wall, int rows, int columns, const DetailLook& look)
{
    constexpr float CELL_LENGTH = 0.30f, CELL_RADIUS = 0.035f, PITCH_X = 0.36f, PITCH_Y = 0.10f;
    const float halfWidth = columns * PITCH_X * 0.5f + 0.03f, halfHeight = rows * PITCH_Y * 0.5f + 0.03f;
    Panel(mesh, wall, halfWidth, halfHeight, 0.025f, 0.012f, look.frame, look.dark, 0.12f, look.metal);
    for (int row = 0; row < rows; ++row)
    {
        const float y = -halfHeight + 0.03f + PITCH_Y * (row + 0.5f);
        for (int column = 0; column < columns; ++column)
        {
            const float x = -halfWidth + 0.03f + PITCH_X * (column + 0.5f);
            const Frame axis = {wall.At(x - CELL_LENGTH * 0.5f, y, CELL_RADIUS + 0.016f), wall.y, wall.x, -wall.z};
            const float body[8][2] = {
                {0, 0.0f},
                {0.002f, CELL_RADIUS * 0.8f},
                {0.012f, CELL_RADIUS},
                {CELL_LENGTH * 0.45f, CELL_RADIUS},
                {CELL_LENGTH * 0.47f, CELL_RADIUS * 0.92f},
                {CELL_LENGTH - 0.012f, CELL_RADIUS * 0.92f},
                {CELL_LENGTH - 0.002f, CELL_RADIUS * 0.7f},
                {CELL_LENGTH, 0.0f}};
            Lathe(mesh, axis, body, 8, 14, look.body);
            const float band[3][2] = {{CELL_LENGTH * 0.62f, CELL_RADIUS * 0.94f}, {CELL_LENGTH * 0.66f, CELL_RADIUS * 0.94f}, {CELL_LENGTH * 0.66f, CELL_RADIUS * 0.9f}};
            Lathe(mesh, axis, band, 3, 14, look.lamp);
            for (const float end : {0.04f, CELL_LENGTH - 0.04f})
            {
                Box(mesh, Offset(wall, x - CELL_LENGTH * 0.5f + end, y - CELL_RADIUS * 0.6f, 0.02f), {0.012f, 0.012f, 0.02f}, 0.003f, look.frame);
            }
        }
        const Vec3 bar[2] = {
            wall.At(-halfWidth + 0.02f, y + CELL_RADIUS * 0.3f, CELL_RADIUS * 2 + 0.03f),
            wall.At(halfWidth - 0.02f, y + CELL_RADIUS * 0.3f, CELL_RADIUS * 2 + 0.03f)};
        Tube(mesh, bar, 2, 0.007f, 8, look.metal);
    }
}

// A control panel on a wall (the frame's z out): a framed, riveted plate with a toggle bank above, a knob row and two
// gauges (bezels round dark faces, a lit needle) below.
void ControlPanel(Mesh& mesh, const Frame& wall, float halfWidth, float halfHeight, const DetailLook& look)
{
    Panel(mesh, wall, halfWidth, halfHeight, 0.02f, 0.01f, look.frame, look.body, 0.06f, look.metal);
    const int toggles = static_cast<int>((halfWidth * 2 - 0.06f) / 0.035f);
    ToggleBank(mesh, Offset(wall, 0, halfHeight * 0.45f, 0.002f), toggles > 1 ? toggles : 2, 0.035f, look);
    const int knobs = static_cast<int>((halfWidth - 0.04f) / 0.045f);
    KnobRow(mesh, Offset(wall, -halfWidth * 0.5f, -halfHeight * 0.4f, 0.002f), knobs > 1 ? knobs : 1, 0.045f, 0.012f, look);
    for (int g = 0; g < 2; ++g)
    {
        const Frame gauge = Offset(wall, halfWidth * (0.25f + 0.4f * g), -halfHeight * 0.35f, 0.002f);
        const float bezel[5][2] = {{0, 0.034f}, {0.006f, 0.034f}, {0.009f, 0.03f}, {0.009f, 0.026f}, {0.004f, 0.026f}};
        Lathe(mesh, Upright(gauge), bezel, 5, 20, look.metal);
        const float dial[2][2] = {{0.004f, 0.026f}, {0.004f, 0.0f}};
        Lathe(mesh, Upright(gauge), dial, 2, 20, look.dark);
        Box(mesh, Offset(gauge, 0.006f, 0.006f, 0.0045f), {0.012f, 0.0012f, 0.0005f}, 0, look.lamp);
    }
}

/// The driver's platform (or the MTT's gantry) reaches this far ahead of the pilot's eye, under the front console.
constexpr float PLATFORM_AHEAD = 0.95f;

/// How the front console looks: finishes of its body, deck top, metal trim, rivets, lamps, recesses and padding.
struct ConsoleLook
{
    int body, top, side, rivet, lamp, dark, pad;
    const StockScreenLook* screen = nullptr; // the side's stock screens for the HUD (PLAN 57 j), or the moulded ones
};

/// The front console (ShellDesigns.h, FrontConsole): its half width, the deck's near edge behind the eye's forward
/// point, the screens' middle from the eye and how far below eye level, the fascia's set-back behind the screens'
/// faces, the brow's reach forward.
constexpr float CONSOLE_HALF_WIDTH = 0.47f, DECK_NEAR = 0.20f, DECK_THICKNESS = 0.05f;
constexpr float SCREEN_DISTANCE = 0.80f, SCREEN_DROP_RADIANS = 26.0f * PI / 180.0f;
constexpr float FASCIA_SET_BACK = 0.06f, FASCIA_THICKNESS = 0.03f, BROW_REACH = 0.12f, BROW_THICKNESS = 0.025f;
/// A track's slot: the carriage's travel (both outer notches plus the play) and its own half length, with room.
constexpr float SLOT_HALF_LENGTH =
    LeverPlacement::NOTCH_SPACING_METERS + 0.012f + LeverPlacement::CARRIAGE_HALF_LENGTH_METERS + 0.012f;

// The driver's front console (PLAN 57 d / e, idren2023 2026-10-18: slide handles on the console ahead, the HUD's
// displays in its front around them, sitting closer to the window): before the seat, at the handles' height, a deck
// with their two tracks (a recessed bed, rails, end stops and three notch marks beside each, the neutral one lit) and a
// padded roll along its near edge; from its far part a fascia rises, leaning back toward the eye, with three screens in
// a shallow arc, each turned to the eye (the health bars over the score on the left, the minimap in the middle, the
// weapon's heat or ammunition over the target's health on the right) and the cockpit's own artwork round them; a brow
// over the fascia reaching forward; cheeks down to the floor and a kick panel under the deck, open beneath for the
// legs. Everything is placed from the pilot's eye (the tracks from LeverPlacement); the brow stays under the windows'
// lower edges as seen from it (about 13 deg below level in every stock cockpit). floorY: where the cheeks stand.
void FrontConsole(Mesh& mesh, float floorY, const ConsoleLook& console, const DashboardLook& look)
{
    const float k = look.scale, halfWidth = CONSOLE_HALF_WIDTH;
    const float deckY = _eye.y - LeverPlacement::PIVOT_DOWN_METERS - LeverPlacement::CARRIAGE_HEIGHT_METERS;
    const float neutralZ = _eye.z - LeverPlacement::PIVOT_AHEAD_METERS, nearZ = _eye.z - DECK_NEAR;
    // The screens: the map's middle SCREEN_DISTANCE from the eye, SCREEN_DROP_RADIANS below level; the fascia's front
    // face FASCIA_SET_BACK behind the screens' faces, in the plane facing the eye through the map's middle.
    const Vec3 middle = _eye + Vec3{0, -sinf(SCREEN_DROP_RADIANS), -cosf(SCREEN_DROP_RADIANS)} * SCREEN_DISTANCE;
    const Frame fascia = Offset(FacingEye(middle), 0, 0, -FASCIA_SET_BACK);
    // Where the fascia's face meets the deck's top (x 0): the deck runs on under it.
    const float meetZ = fascia.origin.z + (-(deckY - fascia.origin.y) * fascia.z.y) / fascia.z.z;
    const float farZ = meetZ - FASCIA_THICKNESS - 0.02f;
    // Side screens: a 0.20 x 0.135 m screen, margins 4.5 cm outboard (a column of artwork), 3 cm inboard, 3.5 cm over
    // (the hood), 6.5 cm under (a strip of artwork). The map's: 0.185 m square, 3 cm beside, 5.5 cm over, 3.5 cm under.
    const float sideHalfWidth = 0.1375f * k, sideHalfHeight = 0.1175f * k, sideScreenX = 0.0075f * k;
    const float sideScreenY = 0.015f * k, screenHalfWidth = 0.10f * k, screenHalfHeight = 0.0675f * k;
    float mapHalfWidth = 0.1225f * k, mapHalfHeight = 0.1375f * k;
    if (look.mapArt >= 0)
    {
        mapHalfWidth = look.mapArtWidth * 0.5f * k;
        mapHalfHeight = mapHalfWidth / look.mapArtAspect;
    }
    const float sideOffset = mapHalfWidth + 0.02f * k + sideHalfWidth;
    const float tallest = mapHalfHeight > sideHalfHeight ? mapHalfHeight : sideHalfHeight;
    // The fascia from the deck up to a little over the screens' tops (in its own frame).
    const float fasciaBottom = Dot(Vec3{0, deckY, meetZ} - fascia.origin, fascia.y);
    const float fasciaTop = tallest + 0.015f;
    const Vec3 topEdge = fascia.At(0, fasciaTop, 0);
    // Deck: a slab, its top at deckY, from the near edge to under the fascia; a padded roll along the near edge.
    Box(mesh,
        MakeFrame({0, deckY - DECK_THICKNESS * 0.5f, (nearZ + farZ) * 0.5f}, {1, 0, 0}, {0, 1, 0}),
        {halfWidth, DECK_THICKNESS * 0.5f, (nearZ - farZ) * 0.5f},
        0.01f,
        console.top);
    const Vec3 roll[2] = {{-halfWidth + 0.03f, deckY - 0.012f, nearZ}, {halfWidth - 0.03f, deckY - 0.012f, nearZ}};
    Tube(mesh, roll, 2, 0.026f, 12, console.pad);
    // The tracks: a recessed bed (riveted border), two rails the carriage runs between, end stops, notch marks
    // outboard (forward, neutral lit, reverse; forward is -z).
    for (int side = -1; side <= 1; side += 2)
    {
        const float x = side * LeverPlacement::HANDLE_SIDE_METERS;
        const Frame bed = MakeFrame({x, deckY + 0.001f, neutralZ}, {1, 0, 0}, {0, 0, -1});
        Panel(
            mesh,
            bed,
            0.062f,
            SLOT_HALF_LENGTH + 0.02f,
            0.014f,
            0.004f,
            console.side,
            console.dark,
            0.07f,
            console.rivet);
        for (int rail = -1; rail <= 1; rail += 2)
        {
            Box(mesh,
                MakeFrame(
                    {x + rail * (LeverPlacement::CARRIAGE_HALF_WIDTH_METERS + 0.008f), deckY + 0.005f, neutralZ},
                    {1, 0, 0},
                    {0, 1, 0}),
                {0.006f, 0.004f, SLOT_HALF_LENGTH},
                0.002f,
                console.side);
        }
        for (int end = -1; end <= 1; end += 2)
        {
            Box(mesh,
                MakeFrame({x, deckY + 0.007f, neutralZ + end * (SLOT_HALF_LENGTH + 0.006f)}, {1, 0, 0}, {0, 1, 0}),
                {0.05f, 0.006f, 0.006f},
                0.002f,
                console.side);
        }
        for (int notch = -1; notch <= 1; ++notch)
        {
            const float z = neutralZ - notch * LeverPlacement::NOTCH_SPACING_METERS;
            IndicatorLamp(
                mesh,
                MakeFrame({x + side * 0.088f, deckY, z}, {1, 0, 0}, {0, 1, 0}),
                notch == 0 ? 0.006f : 0.0045f,
                console.side,
                notch == 0 ? console.lamp : console.dark);
        }
    }
    // Between the tracks: a riveted plate with a row of keys, a small lamp beside each, one lit (as the old lever
    // consoles had; idren2023 2026-10-19: tidier lights).
    const Frame middlePlate = MakeFrame({0, deckY + 0.001f, neutralZ - 0.04f}, {1, 0, 0}, {0, 0, -1});
    Panel(mesh, middlePlate, 0.075f, 0.16f, 0.012f, 0.004f, console.side, console.dark, 0.06f, console.rivet);
    for (int key = 0; key < 4; ++key)
    {
        const Frame at = Offset(middlePlate, 0, -0.09f + 0.06f * key, 0.004f);
        Box(mesh, at, {0.022f, 0.012f, 0.005f}, 0.002f, console.top);
        IndicatorLamp(
            mesh,
            MakeFrame(middlePlate.At(0.04f, -0.09f + 0.06f * key, 0), {1, 0, 0}, {0, 1, 0}),
            0.005f,
            console.side,
            key == 2 ? console.lamp : console.dark);
    }
    // Fascia, brow, kick panel, cheeks.
    Box(mesh,
        Offset(fascia, 0, (fasciaBottom + fasciaTop) * 0.5f, -FASCIA_THICKNESS * 0.5f),
        {halfWidth, (fasciaTop - fasciaBottom) * 0.5f, FASCIA_THICKNESS * 0.5f},
        0.01f,
        console.body);
    const float browZ = topEdge.z - BROW_REACH * 0.5f;
    Box(mesh,
        MakeFrame({0, topEdge.y - BROW_THICKNESS * 0.5f, browZ}, {1, 0, 0}, {0, 1, 0}),
        {halfWidth + 0.01f, BROW_THICKNESS * 0.5f, BROW_REACH * 0.5f + 0.01f},
        0.008f,
        console.body);
    const Vec3 browLip[2] = {
        {-halfWidth - 0.01f, topEdge.y - 0.004f, topEdge.z + 0.008f},
        {halfWidth + 0.01f, topEdge.y - 0.004f, topEdge.z + 0.008f}};
    Tube(mesh, browLip, 2, 0.01f, 8, console.side);
    const float capZ = browZ - BROW_REACH * 0.5f;
    PlanarQuad(
        mesh,
        {-halfWidth, floorY, meetZ},
        {halfWidth, floorY, meetZ},
        {halfWidth, deckY - DECK_THICKNESS, meetZ},
        {-halfWidth, deckY - DECK_THICKNESS, meetZ},
        console.body);
    PlanarQuad(
        mesh,
        {-halfWidth, floorY, capZ},
        {halfWidth, floorY, capZ},
        {halfWidth, topEdge.y - BROW_THICKNESS, capZ},
        {-halfWidth, topEdge.y - BROW_THICKNESS, capZ},
        console.body);
    for (int side = -1; side <= 1; side += 2)
    {
        // Just inside the deck's, fascia's and brow's ends, which hide its upper edges.
        const float x = side * (halfWidth - 0.006f);
        const Vec3 under[4] = {
            {x, floorY, nearZ - 0.01f},
            {x, deckY - 0.01f, nearZ - 0.01f},
            {x, deckY - 0.01f, meetZ},
            {x, floorY, meetZ}};
        PlanarPolygon(mesh, under, 4, {0, 0, -1}, console.body);
        const Vec3 fasciaLow = fascia.At(x, fasciaBottom, -FASCIA_THICKNESS * 0.5f);
        const Vec3 fasciaHigh = fascia.At(x, fasciaTop - 0.01f, -FASCIA_THICKNESS * 0.5f);
        const Vec3 front[4] = {
            {x, floorY, meetZ},
            {x, fasciaLow.y, fasciaLow.z},
            {x, fasciaHigh.y, fasciaHigh.z},
            {x, floorY, fasciaHigh.z}};
        PlanarPolygon(mesh, front, 4, {0, 0, -1}, console.body);
        const Vec3 nose[4] = {
            {x, floorY, fasciaHigh.z},
            {x, fasciaHigh.y, fasciaHigh.z},
            {x, topEdge.y - BROW_THICKNESS, capZ},
            {x, floorY, capZ}};
        PlanarPolygon(mesh, nose, 4, {0, 0, -1}, console.body);
    }
    // The screens, proud of the fascia.
    for (int place = -1; place <= 1; ++place)
    {
        const Vec3 at =
            fascia.At(place * sideOffset, place ? (mapHalfHeight - sideHalfHeight) * -0.3f : 0.0f, FASCIA_SET_BACK);
        const Frame plate = FacingEye(at);
        const float depth = place ? FASCIA_SET_BACK + 0.05f : FASCIA_SET_BACK + 0.01f;
        if (place == 0 && look.mapArt >= 0)
        {
            // The cockpit's own gauge or display as the front, a ring round the map's glass for depth.
            Box(mesh,
                Offset(plate, 0, 0, -depth * 0.5f),
                {mapHalfWidth, mapHalfHeight, depth * 0.5f},
                0.012f,
                look.body);
            Picture(mesh, plate, mapHalfWidth, mapHalfHeight, look.mapArt);
            if (look.glassRight > look.glassLeft)
            {
                const auto corner = [&](float u, float v)
                {
                    return plate.At((u * 2 - 1) * mapHalfWidth, (1 - v * 2) * mapHalfHeight, 0.002f);
                };
                Quad(
                    mesh,
                    corner(look.glassLeft, look.glassTop),
                    corner(look.glassRight, look.glassTop),
                    corner(look.glassRight, look.glassBottom),
                    corner(look.glassLeft, look.glassBottom),
                    look.face);
            }
            const float mapX = look.mapArtX * 2 * mapHalfWidth, mapY = look.mapArtY * 2 * mapHalfWidth;
            const float mapRadius = look.mapArtSize * mapHalfWidth;
            const Frame hood =
                MakeFrame(plate.At(0, mapHalfHeight + 0.004f, 0.02f), plate.x, plate.y + plate.z * 0.35f);
            Box(mesh, hood, {mapHalfWidth + 0.006f, 0.005f, 0.045f}, 0.003f, look.bezel);
            AddDisplay(GROUP_MAP, plate, mapX, mapY, mapRadius * 2 * 0.94f, mapRadius * 2 * 0.94f);
            continue;
        }
        const float halfWidthHere = place ? sideHalfWidth : mapHalfWidth,
                    halfHeight = place ? sideHalfHeight : mapHalfHeight;
        const float screenX = place ? -place * sideScreenX : 0, screenY = place ? sideScreenY : -0.01f * k;
        const float screenW = place ? screenHalfWidth : 0.0925f * k, screenH = place ? screenHalfHeight : 0.0925f * k;
        ScreenFacet(
            mesh,
            plate,
            halfWidthHere,
            halfHeight,
            screenX,
            screenY,
            screenW,
            screenH,
            depth,
            look,
            console.screen);
        Decals(mesh, plate, place < 0 ? look.left : place > 0 ? look.right : look.map, place ? 4 : 3, k);
        // The HUD 2.5 mm before the plate (AddDisplay), within the screen, in the stock screen's readout colour.
        const Frame screen = Offset(plate, screenX, screenY, 0.003f);
        SetDisplayScreen(console.screen);
        if (place < 0)
        {
            AddDisplay(GROUP_HEALTH, screen, 0, 0.022f * k, 0.19f * k, 0.084f * k);
            AddDisplay(GROUP_SCORE, screen, 0, -0.045f * k, 0.19f * k, 0.036f * k);
        }
        else if (place > 0)
        {
            AddDisplay(GROUP_AMMUNITION, screen, 0, 0.02f * k, 0.19f * k, 0.072f * k);
            AddDisplay(GROUP_TARGET, screen, 0, -0.042f * k, 0.15f * k, 0.042f * k);
        }
        else
        {
            AddDisplay(GROUP_MAP, screen, 0, 0, 0.175f * k, 0.175f * k);
        }
        SetDisplayScreen(nullptr);
    }
}

// ---- AAT (cis_hover_aat): the Trade Federation tank ----
// The game's cockpit is a hood 4.5 m wide over the driver's place at its left: an arch from x -0.92 to 3.63, up to
// 3.76 m, its rear rim 0.1 to 0.35 m ahead of the eye; under the window a console whose lower lip sweeps from the
// sides (y -0.13) down to -0.77 at the front. Open: behind the rim, below the lip. The shell carries the arch back to
// a rear bulkhead with a hatch, ribbed, cable-laden in the CIS manner, a deck under it all, a raised driver's platform
// with the seat and lever consoles, a gunner's position across the cabin and machinery along the walls.
enum AatFinish
{
    AAT_PLATE,
    AAT_MACHINE_A,
    AAT_MACHINE_B,
    AAT_BAND,
    AAT_PANEL,
    AAT_DARK,
    AAT_SEAT,
    AAT_LAMP,
    AAT_PIPE,
    AAT_SCREEN,
    AAT_FRAME,
    AAT_STATUS,
    AAT_FACE,
    AAT_RADAR,
    AAT_STOCK_FRAME,
    AAT_STOCK_GLASS,
    AAT_STOCK_BACKING,
    AAT_GRIP,
    AAT_TRIGGER,
    AAT_PICTURE,
    AAT_FINISH_COUNT
};

// Patches of cis_1st_cockpit_AAT (512 x 512; texel rectangles in the comments) and cis_1st_cockpit_fx (128 x 128). The
// colours neutral grey, a touch green (idren2023 2026-10-19: the game's cockpit is a green-tinted grey; the warm colours
// until b307 turned it red-brown), at the same brightness.
const Finish AAT_FINISHES[AAT_FINISH_COUNT] = {
    {{0.332f, 0.479f, 0.684f, 0.615f}, 1.0f, 1, 0x757B77, 0.15f, 12, 0},  // plain plate (170..350, 245..315)
    {{0.078f, 0.156f, 0.742f, 0.342f}, 1.6f, 1, 0x797F7B, 0.10f, 10, 0},  // machinery, cables (40..380, 80..175)
    {{0.293f, 0.781f, 0.998f, 0.918f}, 1.7f, 1, 0x737975, 0.10f, 10, 0},  // cable runs (150..511, 400..470)
    {{0.016f, 0.951f, 0.984f, 0.976f}, 3.8f, 1, 0x8B918D, 0.30f, 20, 0},  // bevelled strip (8..504, 487..500)
    {{0.766f, 0.000f, 0.998f, 0.430f}, 0.45f, 1, 0x7B817D, 0.15f, 12, 0}, // framed panel (392..511, 0..220)
    {{0, 0, 1, 1}, 1, 0, 0x141815, 0.05f, 6, 0},                          // recesses, seams
    {{0.340f, 0.485f, 0.670f, 0.610f}, 0.7f, 1, 0x555B57, 0.14f, 9, 0},   // seat pads (plate, browner)
    {{0, 0, 1, 1}, 1, 0, 0xFFB45A, 0, 1, 1.0f},                           // amber lamps
    {{0.300f, 0.500f, 0.400f, 0.550f}, 0.25f, 1, 0x959B97, 0.45f, 30, 0}, // pipes, rails
    {{0.010f, 0.265f, 0.990f, 0.485f}, 0.30f, 2, 0xC0A060, 0, 1, 0.85f},  // readouts (fx scan bars, 0..128, 34..62)
    {{0.340f, 0.485f, 0.670f, 0.610f}, 1.0f, 1, 0x555B57, 0.20f, 14, 0},  // dark frames
    {{0.010f, 0.515f, 0.990f, 0.740f}, 0.20f, 2, 0xC0C0C0, 0, 1, 1.0f},   // status lights (fx, 0..128, 66..95)
    {{0, 0, 1, 1}, 1, 0, 0x1A140C, 0.70f, 40, 0.35f},                     // display glass (amber-black)
    {{0.0f, 0.0f, 1.0f, 0.25f}, 0.3f, 2, 0xB0B0B0, 0, 1, 0.75f},          // radar arcs (fx 0..128, 0..32)
    ScreenFrameFinish(),                                                  // stock screens (PLAN 57 j): frame
    ScreenGlassFinish(CIS_SCREEN_ART),                                    // glass
    ScreenBackingFinish(CIS_SCREEN_ART),                                  // behind the glass
    {{0, 0, 1, 1}, 1, 0, 0x1E1C1A, 0.20f, 10, 0},                         // grips (rubber)
    {{0, 0, 1, 1}, 1, 0, 0x8A5A20, 0.30f, 16, 0},                         // triggers
    {{0, 0.3f, 1, 0.7f}, 1, 6, 0xFFFFFF, 0, 1, 1.0f},                     // periscope picture (self-lit)
};

const StockScreenLook AAT_SCREENS = {&CIS_SCREEN_ART, AAT_STOCK_FRAME, AAT_STOCK_GLASS, AAT_STOCK_BACKING, AAT_DARK};

// A ladder rail's handhold: from the top of its straight run, an arc up to `height` over the ledge and down onto it at
// `landing` (idren2023 2026-10-19: a smooth arc, not a V).
void LadderArc(Mesh& mesh, Vec3 from, Vec3 landing, float height)
{
    constexpr int STEPS = 12;
    Vec3 arc[STEPS + 1];
    for (int step = 0; step <= STEPS; ++step)
    {
        const float t = static_cast<float>(step) / STEPS;
        arc[step] = Lerp(from, landing, t) + Vec3{0, height * sinf(PI * t), 0};
    }
    Tube(mesh, arc, STEPS + 1, 0.02f, 8, AAT_PIPE);
}

// A magazine of the AAT's energy shells (the DK cross-section's "energy shell magazines") on a wall, the frame's z
// toward the cabin: a back plate in a frame, two rows of five shells lying out of the wall in cradles, each a body
// with a glowing band and a dark nose.
void EnergyShellRack(Mesh& mesh, const Frame& wall)
{
    constexpr int ROWS = 2, COLUMNS = 5;
    constexpr float PITCH_X = 0.15f, PITCH_Y = 0.17f, LENGTH = 0.30f, RADIUS = 0.055f;
    const float halfWidth = PITCH_X * COLUMNS * 0.5f + 0.04f, halfHeight = PITCH_Y * ROWS * 0.5f + 0.04f;
    Panel(mesh, wall, halfWidth, halfHeight, 0.035f, 0.02f, AAT_FRAME, AAT_DARK, 0.12f, AAT_PIPE);
    for (int row = 0; row < ROWS; ++row)
    {
        const float y = (row - (ROWS - 1) * 0.5f) * PITCH_Y;
        Box(mesh, Offset(wall, 0, y - RADIUS - 0.012f, 0.16f), {halfWidth - 0.02f, 0.01f, 0.15f}, 0.004f, AAT_FRAME);
        for (int column = 0; column < COLUMNS; ++column)
        {
            const float x = (column - (COLUMNS - 1) * 0.5f) * PITCH_X;
            const Vec3 body[2] = {wall.At(x, y, 0.02f), wall.At(x, y, 0.02f + LENGTH * 0.72f)};
            Tube(mesh, body, 2, RADIUS, 10, AAT_PIPE);
            const Vec3 band[2] = {wall.At(x, y, 0.02f + LENGTH * 0.40f), wall.At(x, y, 0.02f + LENGTH * 0.50f)};
            Tube(mesh, band, 2, RADIUS + 0.004f, 10, AAT_LAMP);
            const float noseProfile[3][2] = {{0, RADIUS * 0.9f}, {LENGTH * 0.2f, RADIUS * 0.6f}, {LENGTH * 0.28f, 0}};
            Lathe(mesh, MakeFrame(wall.At(x, y, 0.02f + LENGTH * 0.72f), wall.x, wall.z), noseProfile, 3, 10, AAT_DARK);
        }
    }
}

/// The turret's race ring in the hood's crown over the commander (its inner radius); the hood's ribs and crown cables
/// stop this far (plus 0.12 m) from its axis.
constexpr float AAT_TURRET_RING_RADIUS = 0.80f;
/// The upper rear wall (behind the commander) and the lower one's foot: the lower rear wall (the ramp's) leans back
/// from the deck to the commander's loft by 20 deg (the hull model's rear face leans back 38 deg; less here, the cabin
/// is taller).
constexpr float AAT_UPPER_BACK = 5.20f, AAT_LOWER_BACK = 2.76f;
/// The lower rear wall's top: the loft's front edge (idren2023 2026-10-19: the ledge is the hatch wall's top edge).
constexpr float AAT_LOFT_FRONT = 3.72f;
/// How far the first wall band reaches forward over the game's hood (0.08 until PLAN 57 k: from the commander's
/// higher eye the outside showed past the hood's rear rim).
constexpr float AAT_HOOD_OVERLAP = 0.35f;

// The commander's loft (PLAN 57 k, idren2023 2026-10-19, after the DK cross-section and the hull model: the commander sits
// in the turret straight under its turning axis, behind and above the pilot, over the rear hatchway; a ledge without
// a railing whose edge is the hatch wall's top): part 0, a deck on the cabin's middle line, its top AAT_CABIN_EYE
// 1.25 m under the commander's eye, from the ramp wall's top (AAT_LOFT_FRONT) back to the upper rear wall, its front
// and sides edged with a moulding; the turret's race ring in the hood's crown straight over the commander (the turret
// turns about it), and inside it the round top hatch: two half doors split along a bevelled fore-and-aft seam, each
// hinged at its far side (they open up and outward), a pull handle on each beside the seam. crownY: the hood's crown
// over the middle line; floorY, backZ: the cabin's deck and upper rear wall.
void AatCommanderDeck(Mesh& mesh, float crownY, float floorY, float backZ)
{
    const Vec3 eye = {AAT_CABIN_EYE[0], AAT_CABIN_EYE[1], AAT_CABIN_EYE[2]}, up = {0, 1, 0};
    const float deckY = eye.y - 1.25f, front = AAT_LOFT_FRONT, back = backZ - 0.01f;
    // The loft's floor spans the cabin (BuildAat's rear walls); here its ledge: a bevelled moulding along the ramp
    // wall's top from side wall to side wall (run341: a deck slab 2 mm over that floor flickered, its side mouldings
    // stood in the middle of it).
    const float left = -0.90f, right = 3.62f;
    Strip(mesh, {left, deckY, front}, {right, deckY, front}, {0, 0, 1}, {0, 1, -1}, 0.07f, 0.02f, AAT_BAND);
    for (int seam = 0; seam < 3; ++seam)
    {
        const float z = front + (back - front) * (seam + 1) / 4;
        DeckSeam(mesh, {left + 0.1f, deckY, z}, {right - 0.1f, deckY, z}, 0.02f, AAT_DARK);
    }
    // The turntable's stepped ring on the ledge.
    const float ring[5][2] = {{0, 0.70f}, {0.02f, 0.70f}, {0.02f, 0.66f}, {0.035f, 0.66f}, {0.035f, 0.62f}};
    Lathe(mesh, MakeFrame({eye.x, deckY, eye.z}, {1, 0, 0}, up), ring, 5, 36, AAT_FRAME);
    // The turret's race ring in the crown, straight over the commander: a deep collar down from the crown, a bolted
    // flange under it.
    // Hung 0.25 m under the crown, where the arched hood is still wide enough for it (run337: at the crown the hood's
    // own surface covered the hatch).
    const Vec3 axis = {eye.x, crownY - 0.25f, eye.z};
    const float race[6][2] = {
        {0.08f, AAT_TURRET_RING_RADIUS + 0.10f},
        {-0.10f, AAT_TURRET_RING_RADIUS + 0.10f},
        {-0.10f, AAT_TURRET_RING_RADIUS + 0.02f},
        {-0.16f, AAT_TURRET_RING_RADIUS + 0.02f},
        {-0.16f, AAT_TURRET_RING_RADIUS - 0.04f},
        {0.08f, AAT_TURRET_RING_RADIUS - 0.04f}};
    Lathe(mesh, MakeFrame(axis, {1, 0, 0}, up), race, 6, 48, AAT_FRAME);
    for (int bolt = 0; bolt < 24; ++bolt)
    {
        const float angle = 2 * PI * bolt / 24;
        const Vec3 at = axis + Vec3{
                                   (AAT_TURRET_RING_RADIUS + 0.06f) * sinf(angle),
                                   -0.101f,
                                   -(AAT_TURRET_RING_RADIUS + 0.06f) * cosf(angle)};
        Rivet(mesh, MakeFrame(at, {1, 0, 0}, {0, 0, 1}), 0.012f, AAT_PIPE);
    }
    // Two lamps on the race ring's flange, before and behind the commander, so the turret's ceiling is lit.
    for (int end = -1; end <= 1; end += 2)
    {
        const Frame lamp =
            MakeFrame(axis + Vec3{0, -0.16f, end * (AAT_TURRET_RING_RADIUS - 0.01f)}, {1, 0, 0}, {0, 0, 1});
        Box(mesh, lamp, {0.10f, 0.02f, 0.02f}, 0.006f, AAT_FRAME);
        Box(mesh, Offset(lamp, 0, 0, 0.021f), {0.08f, 0.012f, 0.002f}, 0.002f, AAT_LAMP); // frame z points down
    }
    // The turret's own ceiling inside the ring: a flat plate round the hatch.
    {
        constexpr int STEPS = 32;
        Vec3 disc[STEPS];
        for (int index = 0; index < STEPS; ++index)
        {
            const float angle = 2 * PI * index / STEPS;
            disc[index] = axis + Vec3{
                                     (AAT_TURRET_RING_RADIUS - 0.04f) * sinf(angle),
                                     0.07f,
                                     -(AAT_TURRET_RING_RADIUS - 0.04f) * cosf(angle)};
        }
        PlanarPolygon(mesh, disc, STEPS, {1, 0, 0}, AAT_PLATE);
    }
    // The top hatch (a little behind the eye): a coaming, two half doors across the opening hinged at the far left and
    // far right, a bevelled seam along the middle where they meet, a pull handle on each beside it.
    constexpr float HATCH_RADIUS = 0.30f, RIM = HATCH_RADIUS + 0.05f;
    const Vec3 hatch = {eye.x, axis.y + 0.07f, eye.z + 0.15f};
    const float coaming[4][2] =
        {{-0.12f, RIM}, {-0.12f, HATCH_RADIUS + 0.02f}, {0.0f, HATCH_RADIUS + 0.02f}, {0.0f, HATCH_RADIUS}};
    Lathe(mesh, MakeFrame(hatch, {1, 0, 0}, up), coaming, 4, 24, AAT_FRAME);
    // The doors sit 3 cm down inside the coaming, under the turret's ceiling plate (run337: 1 cm above it, hidden).
    constexpr float SEAM_HALF = 0.012f, BEVEL = 0.015f, DOOR_Y = -0.03f;
    for (int side = -1; side <= 1; side += 2)
    {
        // A half door: the half disc beyond the seam's bevel, 1 cm into the coaming.
        constexpr int HALF_STEPS = 12;
        Vec3 door[HALF_STEPS + 1];
        for (int index = 0; index <= HALF_STEPS; ++index)
        {
            const float angle = -PI * 0.5f + PI * index / HALF_STEPS;
            const float x = HATCH_RADIUS * cosf(angle), z = HATCH_RADIUS * sinf(angle);
            door[index] = hatch + Vec3{side * (x > SEAM_HALF + BEVEL ? x : SEAM_HALF + BEVEL), DOOR_Y, z};
        }
        PlanarPolygon(mesh, door, HALF_STEPS + 1, {0, 0, 1}, AAT_FRAME);
        // The seam's bevel on this door's edge: a narrow face sloping up into the seam.
        const float edgeZ = sqrtf(HATCH_RADIUS * HATCH_RADIUS - (SEAM_HALF + BEVEL) * (SEAM_HALF + BEVEL));
        Quad(
            mesh,
            hatch + Vec3{side * (SEAM_HALF + BEVEL), DOOR_Y, -edgeZ},
            hatch + Vec3{side * (SEAM_HALF + BEVEL), DOOR_Y, edgeZ},
            hatch + Vec3{side * SEAM_HALF, DOOR_Y + 0.012f, edgeZ},
            hatch + Vec3{side * SEAM_HALF, DOOR_Y + 0.012f, -edgeZ},
            AAT_BAND,
            0,
            0,
            1,
            1);
        // Stiffening ribs across the door, the hinge barrel at its far side.
        for (int rib = -1; rib <= 1; rib += 2)
        {
            const Vec3 bar[2] = {
                hatch + Vec3{side * 0.06f, DOOR_Y - 0.01f, rib * 0.13f},
                hatch + Vec3{side * 0.25f, DOOR_Y - 0.01f, rib * 0.13f}};
            Tube(mesh, bar, 2, 0.012f, 6, AAT_PIPE);
        }
        const Vec3 hinge[2] = {
            hatch + Vec3{side * (HATCH_RADIUS - 0.05f), DOOR_Y - 0.005f, -0.10f},
            hatch + Vec3{side * (HATCH_RADIUS - 0.05f), DOOR_Y - 0.005f, 0.10f}};
        Tube(mesh, hinge, 2, 0.015f, 8, AAT_PIPE); // clear of the coaming (b303's went through it)
        // The pull handle beside the seam: a bar on two standoffs, along the seam.
        const Vec3 handle[4] = {
            hatch + Vec3{side * 0.07f, DOOR_Y - 0.005f, -0.08f},
            hatch + Vec3{side * 0.07f, DOOR_Y - 0.055f, -0.06f},
            hatch + Vec3{side * 0.07f, DOOR_Y - 0.055f, 0.06f},
            hatch + Vec3{side * 0.07f, DOOR_Y - 0.005f, 0.08f}};
        Tube(mesh, handle, 4, 0.011f, 8, AAT_PIPE);
    }
    // The seam's groove between the bevels.
    Quad(
        mesh,
        hatch + Vec3{-SEAM_HALF, DOOR_Y + 0.012f, -HATCH_RADIUS},
        hatch + Vec3{SEAM_HALF, DOOR_Y + 0.012f, -HATCH_RADIUS},
        hatch + Vec3{SEAM_HALF, DOOR_Y + 0.012f, HATCH_RADIUS},
        hatch + Vec3{-SEAM_HALF, DOOR_Y + 0.012f, HATCH_RADIUS},
        AAT_DARK);
}

/// The turning gunner seats' controls and periscope frame (idren2023 2026-10-19: the sticks on the chair's armrests,
/// not behind the viewscreen where the arms would meet it; a sturdier frame): each control grip's hand place from the
/// eye (x outward, y up, z back; the grip 0.6 of the way up its 15 cm) and its lean ahead at the top.
constexpr float ARMREST_GRIP_SIDE = 0.27f, ARMREST_GRIP_UP = -0.50f, ARMREST_GRIP_BACK = -0.27f;
constexpr float ARMREST_GRIP_LEAN_DEGREES = 9.5f;

/// The finishes of a turning gunner seat's armrests and frame.
struct SeatFrameLook
{
    int frame, metal, grip, trigger;
};

// Two armrests either side of a turning gunner seat (the eye over the seat, its pan seatHalfWidth wide), each a
// bevelled bar on a side plate down to the pan's edge, a control grip standing from its front end (leaning ahead, a
// trigger on its front, a small thumb button on top); and the periscope's frame: each rail a heavy curved box section
// about the eye from lowDegrees to highDegrees, railX to each side, on a strut down to the turntable (topY its top)
// footAhead before the eye (on the turntable, ahead of the legs' reach as far as it allows), a brace across the
// struts' feet and a gusset where each meets its rail.
void ArmrestsAndFrame(
    Mesh& mesh,
    Vec3 eye,
    float seatHalfWidth,
    float topY,
    float railX,
    float rail,
    float lowDegrees,
    float highDegrees,
    float footAhead,
    const SeatFrameLook& look)
{
    constexpr float RADIANS_PER_DEGREE = 0.01745329f;
    const Vec3 up = {0, 1, 0};
    const float lean = tanf(ARMREST_GRIP_LEAN_DEGREES * RADIANS_PER_DEGREE);
    const float seatY = eye.y - 0.80f;
    for (int side = -1; side <= 1; side += 2)
    {
        const float sideF = static_cast<float>(side);
        const float x = eye.x + sideF * ARMREST_GRIP_SIDE;
        const float restY = eye.y + ARMREST_GRIP_UP - 0.10f;
        // The armrest: a bar from behind the hip to the grip, a side plate down to the pan.
        Box(mesh, MakeFrame({x, restY, eye.z - 0.08f}, {1, 0, 0}, up), {0.045f, 0.025f, 0.22f}, 0.012f, look.frame);
        const Vec3 plate[4] = {
            {eye.x + sideF * (seatHalfWidth + 0.005f), seatY - 0.02f, eye.z + 0.10f},
            {eye.x + sideF * (seatHalfWidth + 0.005f), seatY - 0.02f, eye.z - 0.20f},
            {x - sideF * 0.03f, restY - 0.02f, eye.z - 0.24f},
            {x - sideF * 0.03f, restY - 0.02f, eye.z + 0.10f}};
        PlanarPolygon(mesh, plate, 4, {0, 0, 1}, look.frame);
        const Vec3 brace[2] = {
            {x, restY - 0.02f, eye.z - 0.22f},
            {eye.x + sideF * seatHalfWidth, seatY - 0.03f, eye.z - 0.24f}};
        Tube(mesh, brace, 2, 0.016f, 8, look.metal);
        // The grip on the armrest's front end.
        const float handY = eye.y + ARMREST_GRIP_UP, handZ = eye.z + ARMREST_GRIP_BACK;
        const Vec3 grip[2] = {{x, handY - 0.09f, handZ + 0.09f * lean}, {x, handY + 0.06f, handZ - 0.06f * lean}};
        Tube(mesh, grip, 2, 0.022f, 12, look.grip);
        const float collar[3][2] = {{0, 0.04f}, {0.02f, 0.04f}, {0.03f, 0.025f}};
        Lathe(mesh, MakeFrame({x, restY + 0.025f, handZ + 0.09f * lean}, {1, 0, 0}, up), collar, 3, 12, look.metal);
        Box(mesh,
            MakeFrame({x, handY + 0.015f, handZ - 0.024f}, {1, 0, 0}, up),
            {0.006f, 0.016f, 0.012f},
            0.002f,
            look.trigger);
        Box(mesh,
            MakeFrame({x, handY + 0.066f, handZ - 0.06f * lean}, {1, 0, 0}, up),
            {0.008f, 0.004f, 0.008f},
            0.002f,
            look.trigger);
    }
    // The periscope's rails as heavy box sections, each on a strut from the turntable.
    const float railProfile[5][2] =
        {{-0.022f, -0.012f}, {0.022f, -0.012f}, {0.022f, 0.012f}, {-0.022f, 0.012f}, {-0.022f, -0.012f}};
    constexpr int RAIL_POINTS = 14;
    Vec3 feet[2];
    for (int side = -1; side <= 1; side += 2)
    {
        Vec3 arc[RAIL_POINTS];
        for (int k = 0; k < RAIL_POINTS; ++k)
        {
            const float pitch = (lowDegrees + (highDegrees - lowDegrees) * k / (RAIL_POINTS - 1)) * RADIANS_PER_DEGREE;
            arc[k] = eye + Vec3{side * railX, sinf(pitch) * rail, -cosf(pitch) * rail};
        }
        Sweep(mesh, arc, RAIL_POINTS, railProfile, 5, false, false, {1, 0, 0}, look.frame, true);
        const Vec3 foot = {eye.x + side * railX, topY, eye.z - footAhead};
        const Vec3 strut[2] = {foot, arc[0] + Vec3{0, -0.01f, 0}};
        Tube(mesh, strut, 2, 0.022f, 10, look.metal);
        Box(mesh, MakeFrame(foot + Vec3{0, 0.02f, 0}, {1, 0, 0}, up), {0.05f, 0.02f, 0.05f}, 0.006f, look.frame);
        Box(mesh, MakeFrame(arc[0], {1, 0, 0}, up), {0.03f, 0.04f, 0.04f}, 0.008f, look.frame);
        feet[side < 0 ? 0 : 1] = foot + Vec3{0, 0.05f, 0};
    }
    Tube(mesh, feet, 2, 0.018f, 8, look.metal);
}

// Parts 1, 2 and 5 of the AAT's cabin (the combat speeder gunner's turning seat on the commander's deck): the turntable
// with the seat, the yoke on its column and two curved rails about the eye from 40 deg down to 50 deg up (the gun's
// -20..+40 and the readout strip under the display); the periscope display riding them (the stock CIS screen's frame
// round the picture, a dark case, sliders, the thin readout strip under it: player | heat | AAT, the target in the
// picture's top right); the picture behind the frame (SPEEDER_PERISCOPE_*).
void AatCommanderSeat(Mesh& mesh)
{
    constexpr float RAIL_LOW_DEGREES = -40.0f, RAIL_HIGH_DEGREES = 50.0f;
    constexpr float RADIANS_PER_DEGREE = 0.01745329f;
    const Vec3 eye = {AAT_CABIN_EYE[0], AAT_CABIN_EYE[1], AAT_CABIN_EYE[2]}, up = {0, 1, 0};
    const float deckY = eye.y - 1.25f;
    const float rail = ShellDesigns::SPEEDER_PERISCOPE_DISTANCE_METERS;
    const float halfWidth = ShellDesigns::SPEEDER_PERISCOPE_HALF_WIDTH_METERS;
    const float halfHeight = ShellDesigns::SPEEDER_PERISCOPE_HALF_HEIGHT_METERS;
    const float texelMeters = halfHeight * 2 / (CIS_SCREEN_ART.glassBottom - CIS_SCREEN_ART.glassTop);
    mesh.part = 1;
    // The turntable: its rim turned, its top flat and mapped by size (a turned top swirled into the middle).
    const float rim[2][2] = {{0, 0.60f}, {0.04f, 0.60f}};
    Lathe(mesh, MakeFrame({eye.x, deckY, eye.z}, {1, 0, 0}, up), rim, 2, 32, AAT_FRAME);
    {
        constexpr int STEPS = 32;
        Vec3 top[STEPS];
        for (int index = 0; index < STEPS; ++index)
        {
            const float angle = 2 * PI * index / STEPS;
            top[index] = {eye.x + 0.60f * sinf(angle), deckY + 0.04f, eye.z - 0.60f * cosf(angle)};
        }
        PlanarPolygon(mesh, top, STEPS, {1, 0, 0}, AAT_FRAME);
    }
    const SeatLook look = {0.21f, 0.55f, 0.14f, false, true, AAT_SEAT, AAT_FRAME, AAT_PIPE, AAT_BAND, AAT_DARK};
    Seat(mesh, eye, deckY + 0.04f, look);
    const auto fromEye = [&](float x, float y, float z)
    {
        return Vec3{eye.x + x, eye.y + y, eye.z + z};
    };
    const float bezelHalf = halfWidth + (CIS_SCREEN_ART.glassLeft - CIS_SCREEN_ART.outerLeft) * texelMeters;
    const float railX = bezelHalf + 0.03f;
    ArmrestsAndFrame(
        mesh,
        eye,
        0.19f,
        deckY + 0.04f,
        railX,
        rail,
        RAIL_LOW_DEGREES,
        RAIL_HIGH_DEGREES,
        0.55f,
        {AAT_FRAME, AAT_PIPE, AAT_GRIP, AAT_TRIGGER});
    mesh.part = 2;
    const Vec3 screen = {eye.x, eye.y, eye.z - rail};
    const Frame face = MakeFrame(screen, {1, 0, 0}, up);
    float bezelTop = 0, bezelBottom = 0;
    StockScreen(mesh, face, halfWidth, halfHeight, texelMeters, AAT_SCREENS, 0, &bezelTop, &bezelBottom, 0, false);
    Box(mesh,
        Offset(face, 0, (bezelTop + bezelBottom) * 0.5f, -0.024f),
        {bezelHalf + 0.004f, (bezelTop - bezelBottom) * 0.5f + 0.004f, 0.016f},
        0.004f,
        AAT_DARK);
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 arm[2] = {face.At(side * (bezelHalf - 0.01f), 0, -0.012f), face.At(side * railX, 0, 0)};
        Tube(mesh, arm, 2, 0.006f, 6, AAT_FRAME);
        Box(mesh, Offset(face, side * railX, 0, 0), {0.03f, 0.04f, 0.024f}, 0.005f, AAT_FRAME); // round the box rail
    }
    constexpr float PICTURE_Z = -0.004f;
    for (int side = -1; side <= 1; side += 2)
    {
        PlanarQuad(
            mesh,
            face.At(side * halfWidth, halfHeight, 0),
            face.At(side * halfWidth, -halfHeight, 0),
            face.At(side * halfWidth, -halfHeight, PICTURE_Z),
            face.At(side * halfWidth, halfHeight, PICTURE_Z),
            AAT_DARK);
    }
    SetDisplayScreen(&AAT_SCREENS);
    AddDisplay(GROUP_TARGET, face, halfWidth - 0.05f, halfHeight - 0.012f, 0.08f, 0.016f, 2);
    {
        constexpr float STRIP_HALF_WIDTH = 0.15f, STRIP_HALF_HEIGHT = 0.018f, STRIP_TEXEL_METERS = 0.00008f;
        const float stripY = bezelBottom - 0.006f -
                             (CIS_SCREEN_ART.glassTop - CIS_SCREEN_ART.outerTop) * STRIP_TEXEL_METERS -
                             STRIP_HALF_HEIGHT;
        const Frame strip = Offset(face, 0, stripY, 0);
        StockScreen(mesh, strip, STRIP_HALF_WIDTH, STRIP_HALF_HEIGHT, STRIP_TEXEL_METERS, AAT_SCREENS, 0.015f);
        AddDisplayRegion(GROUP_HEALTH, PLAYER_HEALTH, strip, -0.1f, 0, 0.09f, 0.03f, 2);
        AddDisplay(GROUP_AMMUNITION, strip, 0, 0, 0.09f, 0.03f, 2);
        AddDisplayRegion(GROUP_HEALTH, VEHICLE_HEALTH, strip, 0.1f, 0, 0.09f, 0.03f, 2);
    }
    SetDisplayScreen(nullptr);
    mesh.part = 5;
    Quad(
        mesh,
        face.At(-halfWidth, halfHeight, PICTURE_Z),
        face.At(halfWidth, halfHeight, PICTURE_Z),
        face.At(halfWidth, -halfHeight, PICTURE_Z),
        face.At(-halfWidth, -halfHeight, PICTURE_Z),
        AAT_PICTURE);
    mesh.part = 0;
}

// The power plant's housing along the right side (idren2023 2026-10-19: a quarter cylinder on its side from the wall
// down to the floor, front to back, built into the floor, not an afterthought; after the DK cross-section's
// generator): its axis in the corner of the right wall and the deck, radius HOUSING_RADIUS, from z HOUSING_FRONT to
// the ramp wall's foot. The curved face in sections: the cockpit's own machinery art (it suits curved surfaces) and
// plain access panels, bolted ribs between them, a thin lit strip along it, a sunk kick plate and a vent along its
// foot where it meets the deck, conduits along its top; quarter-disc end plates with a framed panel; a hinged access
// hatch and a small control box (one readout, three small lamps) toward the walkway. The walkway between it and the
// driver's platform (x 0.62..2.5) leads from the ramp to both seats.
constexpr float HOUSING_RADIUS = 1.10f, HOUSING_WALL = 3.62f, HOUSING_FRONT = 0.20f;

void PowerPlantHousing(Mesh& mesh, float floorY)
{
    constexpr int STEPS = 14;
    const float back = AAT_LOWER_BACK - 0.02f;
    const auto arc = [&](float angle, float z, float radius)
    {
        return Vec3{HOUSING_WALL - radius * cosf(angle), floorY + radius * sinf(angle), z};
    };
    const auto surfaceFrame = [&](float angle, float z, float radius)
    {
        const Vec3 out = Normalized(Vec3{-cosf(angle), sinf(angle), 0});
        // x along the housing, y up its curve, z out of it.
        return MakeFrame(arc(angle, z, radius), {0, 0, 1}, Cross(out, Vec3{0, 0, 1}) * -1.0f);
    };
    // Sections along z: plain plate, dark vented plate, plain plate, dark vented plate, plain plate (the cockpit's
    // cable art stretched along the curve, idren2023 2026-10-19: changed out).
    const float stations[6] = {HOUSING_FRONT, 0.62f, 1.05f, 1.55f, 2.10f, back};
    const int finishes[5] = {AAT_PLATE, AAT_FRAME, AAT_PLATE, AAT_FRAME, AAT_PLATE};
    for (int section = 0; section < 5; ++section)
    {
        Vec3 nearArc[STEPS + 1], farArc[STEPS + 1];
        for (int step = 0; step <= STEPS; ++step)
        {
            const float angle = 0.5f * PI * step / STEPS;
            nearArc[step] = arc(angle, stations[section], HOUSING_RADIUS);
            farArc[step] = arc(angle, stations[section + 1], HOUSING_RADIUS);
        }
        Loft(mesh, nearArc, farArc, STEPS + 1, 1, finishes[section]);
    }
    // Bolted ribs over the joints, the end ones framing it.
    const float ribProfile[4][2] = {{0.0f, -0.04f}, {0.03f, -0.03f}, {0.03f, 0.03f}, {0.0f, 0.04f}};
    for (int station = 0; station < 6; ++station)
    {
        Vec3 path[STEPS + 1];
        for (int step = 0; step <= STEPS; ++step)
        {
            path[step] = arc(0.5f * PI * step / STEPS, stations[station], HOUSING_RADIUS);
        }
        Sweep(mesh, path, STEPS + 1, ribProfile, 4, true, false, {0, 0, 1}, AAT_FRAME, true);
        for (int bolt = 1; bolt < 6; ++bolt)
        {
            Rivet(mesh, surfaceFrame(0.5f * PI * bolt / 6, stations[station], HOUSING_RADIUS + 0.03f), 0.01f, AAT_PIPE);
        }
    }
    // The dark sections vented: louvre slats along them, up the curve.
    for (int section = 1; section <= 3; section += 2)
    {
        for (int slat = 0; slat < 7; ++slat)
        {
            const float angle = 0.18f + slat * 0.15f;
            const Vec3 run[2] = {
                arc(angle, stations[section] + 0.06f, HOUSING_RADIUS + 0.012f),
                arc(angle, stations[section + 1] - 0.06f, HOUSING_RADIUS + 0.012f)};
            Tube(mesh, run, 2, 0.012f, 6, AAT_PIPE);
        }
    }
    // A thin lit strip along it, and conduits along its top.
    const Vec3 strip[2] = {
        arc(1.22f, stations[1] + 0.05f, HOUSING_RADIUS + 0.006f),
        arc(1.22f, stations[4] - 0.05f, HOUSING_RADIUS + 0.006f)};
    Tube(mesh, strip, 2, 0.008f, 6, AAT_LAMP);
    for (int pipe = 0; pipe < 3; ++pipe)
    {
        const float angle = 1.30f + pipe * 0.08f;
        const Vec3 run[2] = {
            arc(angle, HOUSING_FRONT + 0.05f, HOUSING_RADIUS + 0.045f),
            arc(angle, back - 0.05f, HOUSING_RADIUS + 0.045f)};
        Tube(mesh, run, 2, 0.03f - pipe * 0.006f, 8, pipe == 1 ? AAT_DARK : AAT_PIPE);
    }
    // Its foot in the deck: a sunk kick plate with a vent grille along the walkway side, a seam in the deck.
    const float footX = HOUSING_WALL - HOUSING_RADIUS;
    Box(mesh,
        MakeFrame({footX - 0.02f, floorY + 0.06f, (HOUSING_FRONT + back) * 0.5f}, {0, 0, 1}, {0, 1, 0}),
        {(back - HOUSING_FRONT) * 0.5f, 0.06f, 0.025f},
        0.008f,
        AAT_FRAME);
    Grille(
        mesh,
        MakeFrame({footX - 0.046f, floorY + 0.06f, 1.30f}, {0, 0, 1}, {0, 1, 0}),
        0.60f,
        0.04f,
        0.01f,
        12,
        0.008f,
        AAT_FRAME,
        AAT_DARK);
    DeckSeam(mesh, {footX - 0.12f, floorY, HOUSING_FRONT}, {footX - 0.12f, floorY, back}, 0.025f, AAT_DARK);
    // Quarter-disc end plates, each with a framed panel.
    for (int end = 0; end < 2; ++end)
    {
        const float z = end == 0 ? HOUSING_FRONT : back;
        Vec3 plate[STEPS + 2];
        plate[0] = {HOUSING_WALL, floorY, z};
        for (int step = 0; step <= STEPS; ++step)
        {
            plate[step + 1] = arc(0.5f * PI * step / STEPS, z, HOUSING_RADIUS);
        }
        PlanarPolygon(mesh, plate, STEPS + 2, {1, 0, 0}, AAT_PLATE);
        const Frame face = MakeFrame(
            {HOUSING_WALL - 0.40f, floorY + 0.38f, z + (end == 0 ? -0.006f : 0.006f)},
            {end == 0 ? -1.0f : 1.0f, 0, 0},
            {0, 1, 0});
        Panel(mesh, face, 0.22f, 0.22f, 0.025f, 0.012f, AAT_FRAME, AAT_DARK, 0.1f, AAT_PIPE);
    }
    // Toward the walkway: a hinged access hatch in the panel section, a small control box near the back.
    const Frame hatch = surfaceFrame(0.45f, (stations[2] + stations[3]) * 0.5f, HOUSING_RADIUS + 0.004f);
    Panel(mesh, hatch, 0.20f, 0.16f, 0.03f, 0.012f, AAT_FRAME, AAT_PLATE, 0.08f, AAT_PIPE);
    const Vec3 handle[2] = {hatch.At(-0.08f, -0.10f, 0.03f), hatch.At(0.08f, -0.10f, 0.03f)};
    Tube(mesh, handle, 2, 0.01f, 6, AAT_PIPE);
    const Frame box = surfaceFrame(0.95f, (stations[4] + back) * 0.5f, HOUSING_RADIUS + 0.06f);
    Box(mesh, Offset(box, 0, 0, -0.04f), {0.15f, 0.12f, 0.05f}, 0.01f, AAT_FRAME);
    Quad(
        mesh,
        box.At(-0.11f, 0.08f, 0.012f),
        box.At(0.02f, 0.08f, 0.012f),
        box.At(0.02f, -0.06f, 0.012f),
        box.At(-0.11f, -0.06f, 0.012f),
        AAT_SCREEN);
    for (int lamp = 0; lamp < 3; ++lamp)
    {
        IndicatorLamp(
            mesh,
            MakeFrame(box.At(0.07f, 0.06f - lamp * 0.05f, 0.01f), box.x, box.z),
            0.007f,
            AAT_FRAME,
            lamp == 0 ? AAT_LAMP : AAT_DARK);
    }
}

// A smaller machinery housing against a side wall (idren2023 2026-10-19: more machinery housings): a quarter cylinder
// with its axis in the corner of the wall (x wallX; side 1 the right wall, -1 the left) and the deck, from z front to
// z back, in plain and dark vented plate sections between bolted ribs, flat end plates, a kick plate at its foot.
void WallHousing(Mesh& mesh, float floorY, float wallX, float side, float radius, float front, float back, int sections)
{
    constexpr int STEPS = 10;
    const auto arc = [&](float angle, float z, float r)
    {
        return Vec3{wallX - side * r * cosf(angle), floorY + r * sinf(angle), z};
    };
    const float ribProfile[4][2] = {{0.0f, -0.035f}, {0.025f, -0.025f}, {0.025f, 0.025f}, {0.0f, 0.035f}};
    for (int section = 0; section <= sections; ++section)
    {
        const float z0 = front + (back - front) * section / sections;
        Vec3 rib[STEPS + 1];
        for (int step = 0; step <= STEPS; ++step)
        {
            rib[step] = arc(0.5f * PI * step / STEPS, z0, radius);
        }
        Sweep(mesh, rib, STEPS + 1, ribProfile, 4, true, false, {0, 0, 1}, AAT_FRAME, true);
        if (section == sections)
        {
            break;
        }
        const float z1 = front + (back - front) * (section + 1) / sections;
        const bool isVented = section % 2 == 1;
        Vec3 nearArc[STEPS + 1], farArc[STEPS + 1];
        for (int step = 0; step <= STEPS; ++step)
        {
            nearArc[step] = arc(0.5f * PI * step / STEPS, z0, radius);
            farArc[step] = arc(0.5f * PI * step / STEPS, z1, radius);
        }
        Loft(mesh, nearArc, farArc, STEPS + 1, 1, isVented ? AAT_FRAME : AAT_PLATE);
        if (isVented)
        {
            for (int slat = 0; slat < 5; ++slat)
            {
                const float angle = 0.22f + slat * 0.25f;
                const Vec3 run[2] = {arc(angle, z0 + 0.05f, radius + 0.01f), arc(angle, z1 - 0.05f, radius + 0.01f)};
                Tube(mesh, run, 2, 0.01f, 6, AAT_PIPE);
            }
        }
    }
    for (int end = 0; end < 2; ++end)
    {
        const float z = end == 0 ? front : back;
        Vec3 plate[STEPS + 2];
        plate[0] = {wallX, floorY, z};
        for (int step = 0; step <= STEPS; ++step)
        {
            plate[step + 1] = arc(0.5f * PI * step / STEPS, z, radius);
        }
        PlanarPolygon(mesh, plate, STEPS + 2, {1, 0, 0}, AAT_PLATE);
    }
    const float footX = wallX - side * radius;
    Box(mesh,
        MakeFrame({footX - side * 0.02f, floorY + 0.05f, (front + back) * 0.5f}, {0, 0, 1}, {0, 1, 0}),
        {(back - front) * 0.5f, 0.05f, 0.02f},
        0.006f,
        AAT_FRAME);
}

// The front right corner of the deck (idren2023 2026-10-19: filled with machinery): a capacitor bank between the console's
// skirt and the power plant's housing: a low plated cabinet with a sloped top toward the aisle (a framed access panel
// and a vent in its face), on it four long capacitor cylinders along x in clamps with bolted end caps, a bus bar
// across their ends, conduits from it into the power plant's front end.
void CapacitorBank(Mesh& mesh, float floorY)
{
    const Vec3 up = {0, 1, 0};
    constexpr float X0 = 1.15f, X1 = 3.55f, Z0 = -0.95f, Z1 = 0.12f, HEIGHT = 0.42f;
    const float middleX = (X0 + X1) * 0.5f, halfX = (X1 - X0) * 0.5f, middleZ = (Z0 + Z1) * 0.5f;
    Box(mesh,
        MakeFrame({middleX, floorY + HEIGHT * 0.5f, middleZ}, {1, 0, 0}, up),
        {halfX, HEIGHT * 0.5f, (Z1 - Z0) * 0.5f},
        0.02f,
        AAT_FRAME);
    // The face toward the aisle and the pilot (-x): a framed access panel and a vent.
    const Frame face = MakeFrame({X0 - 0.002f, floorY + HEIGHT * 0.5f, middleZ}, {0, 0, 1}, up);
    Panel(mesh, Offset(face, 0.18f, 0, 0), 0.26f, 0.15f, 0.025f, 0.012f, AAT_FRAME, AAT_PLATE, 0.09f, AAT_PIPE);
    Grille(mesh, Offset(face, -0.30f, 0, 0.004f), 0.16f, 0.13f, 0.015f, 6, 0.012f, AAT_FRAME, AAT_DARK);
    // Its top plate and the capacitors in clamps.
    TiledQuad(
        mesh,
        {X0 + 0.02f, floorY + HEIGHT + 0.002f, Z0 + 0.02f},
        {X1 - X0 - 0.04f, 0, 0},
        {0, 0, Z1 - Z0 - 0.04f},
        AAT_PLATE);
    for (int cell = 0; cell < 4; ++cell)
    {
        const float z = Z0 + 0.16f + cell * (Z1 - Z0 - 0.32f) / 3;
        const float y = floorY + HEIGHT + 0.11f;
        const Vec3 body[2] = {{X0 + 0.22f, y, z}, {X1 - 0.22f, y, z}};
        Tube(mesh, body, 2, 0.09f, 14, cell % 2 == 0 ? AAT_PLATE : AAT_FRAME);
        for (int end = -1; end <= 1; end += 2)
        {
            const float x = end < 0 ? X0 + 0.22f : X1 - 0.22f;
            const float cap[3][2] = {{0, 0.095f}, {0.03f, 0.095f}, {0.05f, 0.05f}};
            Lathe(mesh, MakeFrame({x, y, z}, {0, 1, 0}, {static_cast<float>(end), 0, 0}), cap, 3, 14, AAT_FRAME);
        }
        for (int clamp = 0; clamp < 3; ++clamp)
        {
            const float x = X0 + 0.55f + clamp * (X1 - X0 - 1.10f) / 2;
            const Vec3 band[2] = {{x - 0.025f, y, z}, {x + 0.025f, y, z}};
            Tube(mesh, band, 2, 0.1f, 14, AAT_PIPE);
            Box(mesh,
                MakeFrame({x, floorY + HEIGHT + 0.01f, z}, {1, 0, 0}, up),
                {0.03f, 0.01f, 0.07f},
                0.003f,
                AAT_FRAME);
        }
    }
    // The maintenance terminal at the walkway's front end (idren2023 2026-10-19: a reason for the walkway): on the bank's
    // back face, toward the ramp, a pedestal rising to a desk 1.0 m over the deck leaning toward the walker, a stock
    // CIS screen in its upper face, two rows of keys under it, side grab handles, a tread mat on the deck before it.
    {
        constexpr float TERMINAL_X = 1.75f, DESK = 1.00f;
        Box(mesh,
            MakeFrame({TERMINAL_X, floorY + DESK * 0.5f, Z1 + 0.08f}, {1, 0, 0}, up),
            {0.28f, DESK * 0.5f, 0.08f},
            0.015f,
            AAT_FRAME);
        const Vec3 tilt = Normalized(Vec3{0, 0.55f, 0.83f}); // the face leans back 34 deg toward the walker
        const Frame desk = MakeFrame(
            {TERMINAL_X, floorY + DESK + 0.12f, Z1 + 0.20f},
            {1, 0, 0},
            Cross(tilt, Vec3{1, 0, 0})); // z = tilt, toward the walker
        Box(mesh, Offset(desk, 0, 0, -0.04f), {0.34f, 0.24f, 0.04f}, 0.012f, AAT_FRAME);
        StockScreen(
            mesh,
            Offset(desk, 0, 0.06f, 0.008f),
            0.20f,
            0.10f,
            0.00012f,
            AAT_SCREENS); // backing clear of the desk
        for (int row = 0; row < 2; ++row)
        {
            for (int key = 0; key < 6; ++key)
            {
                Box(mesh,
                    Offset(desk, -0.15f + key * 0.06f, -0.12f - row * 0.05f, 0.008f),
                    {0.022f, 0.016f, 0.008f},
                    0.003f,
                    row == 0 && key == 4 ? AAT_LAMP : AAT_PLATE);
            }
        }
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 handle[4] = {
                desk.At(side * 0.36f, 0.18f, -0.03f),
                desk.At(side * 0.40f, 0.16f, 0.02f),
                desk.At(side * 0.40f, -0.16f, 0.02f),
                desk.At(side * 0.36f, -0.18f, -0.03f)};
            Tube(mesh, handle, 4, 0.013f, 6, AAT_PIPE);
        }
        Grille(
            mesh,
            MakeFrame({TERMINAL_X, floorY + 0.004f, Z1 + 0.55f}, {1, 0, 0}, {0, 0, -1}),
            0.36f,
            0.30f,
            0.025f,
            8,
            0.01f,
            AAT_FRAME,
            AAT_DARK);
    }
    const Vec3 bus[2] = {
        {X0 + 0.12f, floorY + HEIGHT + 0.11f, Z0 + 0.10f},
        {X0 + 0.12f, floorY + HEIGHT + 0.11f, Z1 - 0.10f}};
    Tube(mesh, bus, 2, 0.035f, 8, AAT_PIPE);
    for (int conduit = 0; conduit < 2; ++conduit)
    {
        const float z = Z1 - 0.25f - conduit * 0.25f;
        const Vec3 run[4] = {
            {X1 - 0.4f, floorY + HEIGHT + 0.02f, z},
            {X1 - 0.5f, floorY + HEIGHT + 0.30f, z + 0.1f},
            {X1 - 0.55f, floorY + HEIGHT + 0.45f, 0.10f},
            {X1 - 0.55f, floorY + HEIGHT + 0.45f, 0.25f}};
        Vec3 path[24];
        const int count = Smooth(run, 4, 5, path, 24);
        Tube(mesh, path, count, 0.035f, 8, conduit == 0 ? AAT_PIPE : AAT_DARK);
    }
}

void BuildAat(Mesh& mesh)
{
    // The game's rims (cis_1st_cockpit_AAT.msh, main node): the rear arch, left bottom to right bottom, and the
    // console's lower lip, right back corner round the front to the left back corner.
    const Vec3 arch[11] = {
        {-0.92f, -0.13f, 1.15f},
        {-0.92f, 0.63f, 1.15f},
        {-0.92f, 1.59f, 1.15f},
        {-0.71f, 2.48f, 1.17f},
        {-0.11f, 3.05f, 1.26f},
        {1.35f, 3.76f, 1.41f},
        {2.82f, 3.05f, 1.26f},
        {3.42f, 2.48f, 1.17f},
        {3.63f, 1.59f, 1.15f},
        {3.63f, 0.64f, 1.15f},
        {3.63f, -0.12f, 1.15f}};
    const Vec3 lip[11] = {
        {3.63f, -0.12f, 1.15f},
        {3.57f, -0.22f, 0.65f},
        {3.09f, -0.49f, -0.74f},
        {2.29f, -0.70f, -1.78f},
        {2.16f, -0.71f, -1.83f},
        {1.35f, -0.77f, -2.15f},
        {0.55f, -0.71f, -1.83f},
        {0.42f, -0.70f, -1.78f},
        {-0.38f, -0.50f, -0.74f},
        {-0.86f, -0.22f, 0.65f},
        {-0.92f, -0.13f, 1.15f}};
    constexpr float FLOOR = -0.90f, BACK = AAT_UPPER_BACK, PLATFORM = 0.25f; // BACK 2.65 until the commander (57 k)
    // Cross-section behind the rim: the arch, carried down both sides to the deck.
    Vec3 section[13];
    section[0] = {-0.94f, FLOOR, 1.15f};
    for (int i = 0; i < 11; ++i)
    {
        section[i + 1] = Pushed(arch[i], 1.03f);
    }
    section[12] = {3.66f, FLOOR, 1.15f};
    Vec3 curve[96];
    const int curveCount = Smooth(section, 13, 3, curve, 96);
    // Walls in bands between ribs, plain plating mapped by its real size (PLAN 57 k: the alternating machinery bands
    // read as smeared photographs; the detail is in the racks, panels and conduits); the first band starts under the
    // game's hood.
    const float stations[5] = {0, 1.62f, 2.80f, 3.95f, BACK};
    Vec3 a[96], b[96];
    for (int band = 0; band < 4; ++band)
    {
        for (int i = 0; i < curveCount; ++i)
        {
            // Under the hood only where the hood is (above the deck's edge): lower down the skirt meets it.
            const float tuck = band == 0 && curve[i].y > 0.0f ? -AAT_HOOD_OVERLAP : 0.0f;
            a[i] = band == 0 ? curve[i] + Vec3{0, 0, tuck} : Vec3{curve[i].x, curve[i].y, stations[band]};
        }
        AtDepth(curve, curveCount, stations[band + 1], b);
        Loft(mesh, a, b, curveCount, 2, AAT_PLATE);
    }
    // Ribs: a bevelled band 10 cm wide standing 9 cm into the cabin, at each station, down to the deck; lamps.
    const float ribProfile[4][2] = {{0.0f, -0.05f}, {0.09f, -0.04f}, {0.09f, 0.04f}, {0.0f, 0.05f}};
    for (int station = 1; station <= 3; ++station)
    {
        Vec3 path[96];
        AtDepth(curve, curveCount, stations[station], path);
        // Ribs over the commander (PLAN 57 k) stop at the turret's race ring: a rib within the ring's reach (and
        // 0.12 m) of the turret's axis is cut above the ring's height, and its crown lamp is left out.
        const float clearance = AAT_TURRET_RING_RADIUS + 0.12f;
        const bool crossesHatch = fabsf(stations[station] - AAT_CABIN_EYE[2]) < clearance;
        int cutFrom = curveCount, cutTo = curveCount;
        if (crossesHatch)
        {
            float top = path[0].y;
            for (int i = 1; i < curveCount; ++i)
            {
                top = path[i].y > top ? path[i].y : top;
            }
            for (int i = 0; i < curveCount; ++i)
            {
                // Above the ring's collar (0.30 m under the crown) the rib is left out: it ends on the walls beside
                // the ring (run-337 renders: cut by distance from the axis, its ends hung as stubs at the ring's edge).
                if (path[i].y > top - 0.30f)
                {
                    cutFrom = cutFrom == curveCount ? i : cutFrom;
                    cutTo = i + 1;
                }
            }
        }
        if (cutFrom < curveCount)
        {
            Sweep(mesh, path, cutFrom, ribProfile, 4, true, false, {0, 0, 1}, AAT_BAND, true);
            Sweep(mesh, path + cutTo, curveCount - cutTo, ribProfile, 4, true, false, {0, 0, 1}, AAT_BAND, true);
        }
        else
        {
            Sweep(mesh, path, curveCount, ribProfile, 4, true, false, {0, 0, 1}, AAT_BAND, true);
        }
        for (int k = crossesHatch ? 1 : 0; k < 3; ++k)
        {
            const int index = k == 0 ? curveCount / 2 : k == 1 ? curveCount / 4 : 3 * curveCount / 4;
            const Vec3 at = path[index];
            const Vec3 inward = Normalized(Vec3{1.35f, 1.4f, at.z} - at);
            Box(mesh, MakeFrame(at + inward * 0.1f, {0, 0, 1}, inward), {0.05f, 0.012f, 0.02f}, 0.006f, AAT_LAMP);
        }
    }
    // Deck: plating under everything, and the skirt from the console's lip (tucked up behind it) down to it.
    TiledQuad(mesh, {-0.94f, FLOOR, -2.3f}, {4.6f, 0, 0}, {0, 0, BACK + 2.3f}, AAT_FRAME);
    Vec3 pushedLip[11], lipCurve[96], lipFloor[96];
    for (int i = 0; i < 11; ++i)
    {
        pushedLip[i] = Pushed(lip[i], 1.04f) + Vec3{0, 0.06f, 0};
    }
    const int lipCount = Smooth(pushedLip, 11, 5, lipCurve, 96);
    for (int i = 0; i < lipCount; ++i)
    {
        lipFloor[i] = {lipCurve[i].x, FLOOR, lipCurve[i].z};
    }
    Loft(mesh, lipCurve, lipFloor, lipCount, 2, AAT_MACHINE_B);
    // Rear walls (PLAN 57 k, after the hull model: its rear face leans back from the hull's floor; idren2023 2026-10-19):
    // above the commander's loft the upper wall stands straight behind him (AAT_UPPER_BACK) with plated panels and a
    // vent; below it the lower wall leans back from the deck (AAT_LOWER_BACK) up to the loft's back edge, both flat
    // plates mapped by their real size, cut from the cabin's cross-section. In the lower wall the wide closed ramp.
    {
        const float loftY = AAT_CABIN_EYE[1] - 1.25f;
        const auto lowerZ = [&](float y)
        {
            return AAT_LOWER_BACK + (y - FLOOR) / (loftY - FLOOR) * (AAT_LOFT_FRONT - AAT_LOWER_BACK);
        };
        Vec3 lower[100], upper[100];
        int lowerCount = 0, upperCount = 0;
        for (int i = 0; i < curveCount; ++i)
        {
            const Vec3 point = curve[i];
            if (i > 0 && (curve[i - 1].y - loftY) * (point.y - loftY) < 0)
            {
                // The cross-section crosses the loft's height: a corner of both walls.
                const float t = (loftY - curve[i - 1].y) / (point.y - curve[i - 1].y);
                const float x = curve[i - 1].x + (point.x - curve[i - 1].x) * t;
                lower[lowerCount++] = {x, loftY, AAT_LOFT_FRONT};
                upper[upperCount++] = {x, loftY, AAT_UPPER_BACK};
            }
            if (point.y <= loftY)
            {
                lower[lowerCount++] = {point.x, point.y, lowerZ(point.y)};
            }
            else
            {
                upper[upperCount++] = {point.x, point.y, AAT_UPPER_BACK};
            }
        }
        PlanarPolygon(mesh, lower, lowerCount, {1, 0, 0}, AAT_PLATE);
        PlanarPolygon(mesh, upper, upperCount, {1, 0, 0}, AAT_PLATE);
        // The loft's floor reaches the side walls (the ledge between them, AatCommanderDeck), the upper wall stands on
        // it: a skirting along its foot.
        const Vec3 crease[2] = {
            {-0.9f, loftY + 0.02f, AAT_UPPER_BACK - 0.02f},
            {3.62f, loftY + 0.02f, AAT_UPPER_BACK - 0.02f}};
        Tube(mesh, crease, 2, 0.025f, 6, AAT_BAND);
        {
            // The loft's floor wider than the ledge: out to the side walls at its height.
            const Vec3 floorQuad[4] = {
                {-0.92f, loftY, AAT_LOFT_FRONT},
                {3.64f, loftY, AAT_LOFT_FRONT},
                {3.64f, loftY, AAT_UPPER_BACK},
                {-0.92f, loftY, AAT_UPPER_BACK}};
            PlanarPolygon(mesh, floorQuad, 4, {1, 0, 0}, AAT_PLATE);
        }
        // Behind the commander (idren2023 2026-10-19: the cockpit's cable art only works on curved surfaces): two plated
        // panels with plain insets either side of a louvred vent, a pair of conduits along the wall under them.
        const Frame upperWall = MakeFrame({1.35f, loftY + 0.85f, AAT_UPPER_BACK - 0.01f}, {-1, 0, 0}, {0, 1, 0});
        for (int side = -1; side <= 1; side += 2)
        {
            Panel(
                mesh,
                Offset(upperWall, side * 0.72f, 0, 0),
                0.40f,
                0.36f,
                0.05f,
                0.025f,
                AAT_FRAME,
                AAT_PLATE,
                0.16f,
                AAT_PIPE);
            // In each inset the commander's systems panel (idren2023 2026-10-19: more detail): switches, dials, gauges.
            const DetailLook look = {AAT_PLATE, AAT_FRAME, AAT_PIPE, AAT_DARK, AAT_LAMP, AAT_TRIGGER};
            ControlPanel(mesh, Offset(upperWall, side * 0.72f, 0, 0.004f), 0.30f, 0.24f, look);
        }
        Grille(mesh, Offset(upperWall, 0, 0.05f, 0.01f), 0.22f, 0.28f, 0.03f, 9, 0.025f, AAT_FRAME, AAT_DARK);
        for (int conduit = 0; conduit < 2; ++conduit)
        {
            const Vec3 run[2] = {
                upperWall.At(-1.55f, -0.50f - conduit * 0.08f, 0.05f),
                upperWall.At(1.55f, -0.50f - conduit * 0.08f, 0.05f)};
            Tube(mesh, run, 2, 0.028f - conduit * 0.006f, 8, AAT_PIPE);
        }
        // The ramp (after the hull model: its rear hatch takes about three quarters of the rear face's width and
        // nearly its height; the DK cross-section's exit ramp): a wide leaf in the leaning wall, closed, hinged
        // across its foot along the deck (it would fold down and out as a ramp), its inner face the ramp's walking
        // side: a raised border, cross ribs and anti-slip treads between them; a hydraulic arm each side from the
        // wall to the leaf, a lamp over it.
        const Vec3 slope = Normalized(Vec3{0, loftY - FLOOR, AAT_LOFT_FRONT - AAT_LOWER_BACK});
        constexpr float RAMP_HALF_WIDTH = 0.90f, RAMP_LENGTH = 2.30f;
        const Vec3 foot = {1.50f, FLOOR, AAT_LOWER_BACK};
        const Frame ramp =
            MakeFrame(foot + slope * (RAMP_LENGTH * 0.5f + 0.04f) + Vec3{0, 0, -0.012f}, {-1, 0, 0}, slope);
        Box(mesh, Offset(ramp, 0, 0, 0.01f), {RAMP_HALF_WIDTH, RAMP_LENGTH * 0.5f, 0.012f}, 0.006f, AAT_FRAME);
        Panel(
            mesh,
            Offset(ramp, 0, 0, 0.022f),
            RAMP_HALF_WIDTH - 0.02f,
            RAMP_LENGTH * 0.5f - 0.02f,
            0.06f,
            0.03f,
            AAT_FRAME,
            AAT_PLATE,
            0.25f,
            AAT_PIPE);
        for (int rib = 1; rib <= 4; ++rib)
        {
            const float y = -RAMP_LENGTH * 0.5f + RAMP_LENGTH * rib / 5;
            Box(mesh, Offset(ramp, 0, y, 0.045f), {RAMP_HALF_WIDTH - 0.08f, 0.025f, 0.02f}, 0.006f, AAT_FRAME);
        }
        for (int tread = 0; tread < 15; ++tread)
        {
            const float y = -RAMP_LENGTH * 0.5f + 0.10f + tread * (RAMP_LENGTH - 0.20f) / 14;
            if (tread % 3 == 0)
            {
                continue; // under the ribs
            }
            Box(mesh, Offset(ramp, 0, y, 0.03f), {RAMP_HALF_WIDTH - 0.16f, 0.008f, 0.006f}, 0.002f, AAT_PIPE);
        }
        const Vec3 hinge[2] = {
            foot + Vec3{-RAMP_HALF_WIDTH, 0.05f, -0.05f},
            foot + Vec3{RAMP_HALF_WIDTH, 0.05f, -0.05f}};
        Tube(mesh, hinge, 2, 0.045f, 12, AAT_PIPE);
        for (int knuckle = -2; knuckle <= 2; ++knuckle)
        {
            const Vec3 at = foot + Vec3{knuckle * 0.42f, 0.05f, -0.05f};
            const Vec3 band[2] = {at + Vec3{-0.06f, 0, 0}, at + Vec3{0.06f, 0, 0}};
            Tube(mesh, band, 2, 0.055f, 12, AAT_FRAME);
        }
        for (int side = -1; side <= 1; side += 2)
        {
            // A hydraulic arm from the ceiling down at about 45 deg to the leaf's top corner, ready to push it down to
            // the ground (idren2023 2026-10-19): a hanger from the hood's crown, a pivot block, the cylinder, the rod.
            const Vec3 end = ramp.At(-side * (RAMP_HALF_WIDTH - 0.10f), RAMP_LENGTH * 0.5f - 0.05f, 0.07f);
            const Vec3 base = end + Vec3{0, 0.95f, -0.95f};
            const Vec3 hanger[2] = {base, base + Vec3{0, 1.2f, 0}};
            Tube(mesh, hanger, 2, 0.035f, 8, AAT_FRAME);
            Box(mesh, MakeFrame(base, {1, 0, 0}, {0, 1, 0}), {0.07f, 0.07f, 0.07f}, 0.012f, AAT_FRAME);
            const Vec3 middle = Lerp(base, end, 0.55f);
            const Vec3 cylinder[2] = {base, middle};
            Tube(mesh, cylinder, 2, 0.05f, 12, AAT_FRAME);
            const Vec3 collar[2] = {middle - Normalized(end - base) * 0.03f, middle};
            Tube(mesh, collar, 2, 0.058f, 12, AAT_PIPE);
            const Vec3 rod[2] = {middle, end};
            Tube(mesh, rod, 2, 0.024f, 10, AAT_PIPE);
            Box(mesh, MakeFrame(end, {1, 0, 0}, slope), {0.05f, 0.05f, 0.04f}, 0.01f, AAT_FRAME);
        }
        // The ladder up the ramp's wall to the loft, left of the ramp, built into the wall (idren2023 2026-10-19): two
        // rails standing 9 cm off the leaning wall, rungs every 0.3 m along it, the rails running on over the ledge
        // as handholds and down onto the loft.
        {
            constexpr float LADDER_X = 0.02f, LADDER_HALF = 0.50f, OFF = 0.09f; // 1 m wide (idren2023)
            const Vec3 off =
                Normalized(Cross(slope, Vec3{1, 0, 0})) * OFF; // toward the cabin (run341: the other way, in the wall)
            const float length = Length(Vec3{0, loftY - FLOOR, AAT_LOFT_FRONT - AAT_LOWER_BACK});
            for (int side = -1; side <= 1; side += 2)
            {
                const float x = LADDER_X + side * LADDER_HALF;
                const Vec3 base = {x, FLOOR, AAT_LOWER_BACK};
                const Vec3 straight[3] = {
                    base + slope * 0.02f,
                    base + slope * 0.10f + off,
                    base + slope * length + off};
                Tube(mesh, straight, 3, 0.02f, 8, AAT_PIPE);
                LadderArc(mesh, straight[2], {x, loftY, AAT_LOFT_FRONT + 0.32f}, 0.55f);
                const Vec3 shoe[2] = {base + slope * 0.02f, base + slope * 0.02f + Vec3{0, 0, -0.08f}};
                Tube(mesh, shoe, 2, 0.03f, 8, AAT_FRAME);
            }
            for (float t = 0.30f; t < length - 0.1f; t += 0.30f)
            {
                const Vec3 a = Vec3{LADDER_X - LADDER_HALF, FLOOR, AAT_LOWER_BACK} + slope * t + off;
                const Vec3 rung[2] = {a, a + Vec3{2 * LADDER_HALF, 0, 0}};
                Tube(mesh, rung, 2, 0.016f, 8, AAT_FRAME);
            }
        }
        Box(mesh, Offset(ramp, 0, RAMP_LENGTH * 0.5f + 0.16f, 0.03f), {0.12f, 0.03f, 0.03f}, 0.008f, AAT_FRAME);
        Box(mesh, Offset(ramp, 0, RAMP_LENGTH * 0.5f + 0.16f, 0.062f), {0.09f, 0.015f, 0.003f}, 0.002f, AAT_LAMP);
    }
    // Driver's platform: raised deck under the seat and the front console, a tilted foot plate and a foot bar under
    // the console. Its back drops to the deck; a short ladder down it in the style of the ramp wall's (idren2023
    // 2026-10-19: keep the drop, add a ladder): two rails standing 9 cm off the face, rungs every 0.3 m, the rails bent
    // over onto the platform as handholds.
    // 0.45 m behind the seat's back to stand on and get into it (idren2023 2026-10-19; 2.54 until then).
    const float platformFront = _eye.z - PLATFORM_AHEAD, platformBack = 1.30f;
    {
        constexpr float LADDER_HALF = 0.25f, OFF = 0.09f;
        const float z = platformBack + OFF;
        for (int side = -1; side <= 1; side += 2)
        {
            const float x = side * LADDER_HALF;
            const Vec3 straight[2] = {{x, FLOOR, z}, {x, PLATFORM, z}};
            Tube(mesh, straight, 2, 0.02f, 8, AAT_PIPE);
            LadderArc(mesh, straight[1], {x, PLATFORM, platformBack - 0.25f}, 0.50f);
            const Vec3 shoe[2] = {{x, FLOOR + 0.02f, z}, {x, FLOOR + 0.02f, z + 0.08f}};
            Tube(mesh, shoe, 2, 0.03f, 8, AAT_FRAME);
        }
        for (float y = FLOOR + 0.3f; y < PLATFORM - 0.05f; y += 0.3f)
        {
            const Vec3 rung[2] = {{-LADDER_HALF, y, z}, {LADDER_HALF, y, z}};
            Tube(mesh, rung, 2, 0.016f, 8, AAT_FRAME);
        }
    }
    Box(mesh,
        MakeFrame({0, (FLOOR + PLATFORM) * 0.5f, (platformFront + platformBack) * 0.5f}, {1, 0, 0}, {0, 1, 0}),
        {0.62f, (PLATFORM - FLOOR) * 0.5f, (platformBack - platformFront) * 0.5f},
        0.03f,
        AAT_FRAME);
    Panel(
        mesh,
        MakeFrame({0, (FLOOR + PLATFORM) * 0.5f, platformFront - 0.01f}, {1, 0, 0}, {0, 1, 0}),
        0.58f,
        0.5f,
        0.05f,
        0.02f,
        AAT_FRAME,
        AAT_MACHINE_B,
        0.24f,
        AAT_PIPE);
    Box(mesh,
        MakeFrame({0, PLATFORM + 0.06f, _eye.z - 0.56f}, {1, 0, 0}, {0, 1, -0.6f}),
        {0.3f, 0.012f, 0.13f},
        0.006f,
        AAT_PANEL);
    const Vec3 footBar[2] = {{-0.32f, PLATFORM + 0.12f, _eye.z - 0.66f}, {0.32f, PLATFORM + 0.12f, _eye.z - 0.66f}};
    Tube(mesh, footBar, 2, 0.016f, 10, AAT_PIPE);
    // Seat and front console.
    const SeatLook seat = {0.22f, 0.62f, 0.16f, false, true, AAT_SEAT, AAT_FRAME, AAT_PIPE, AAT_BAND, AAT_DARK};
    Seat(mesh, _eye, PLATFORM, seat);
    const ConsoleLook console = {AAT_FRAME, AAT_PLATE, AAT_PIPE, AAT_PIPE, AAT_LAMP, AAT_DARK, AAT_SEAT, &AAT_SCREENS};
    FrontConsole(
        mesh,
        PLATFORM,
        console,
        {1.2f,
         AAT_PLATE,
         AAT_FRAME,
         AAT_FACE,
         AAT_PIPE,
         AAT_LAMP,
         {{AAT_PANEL, -0.115f, 0.015f, 0.02f, 0.037f},
          {AAT_RADAR, -0.025f, -0.085f, 0.05f, 0.0125f},
          {AAT_LAMP, 0.065f, -0.085f, 0.008f, 0.008f},
          {AAT_LAMP, 0.09f, -0.085f, 0.008f, 0.008f}},
         {{AAT_PANEL, 0.115f, 0.015f, 0.02f, 0.037f},
          {AAT_RADAR, 0.025f, -0.085f, 0.05f, 0.0125f},
          {AAT_LAMP, -0.065f, -0.085f, 0.008f, 0.008f},
          {AAT_LAMP, -0.09f, -0.085f, 0.008f, 0.008f}},
         {{AAT_RADAR, 0, 0.11f, 0.075f, 0.019f}}});
    // Left wall: a panel with two readouts, conduits running back.
    const Frame leftWall = MakeFrame({-0.89f, 0.75f, 2.05f}, {0, 0, -1}, {0, 1, 0});
    Panel(mesh, leftWall, 0.4f, 0.3f, 0.05f, 0.03f, AAT_FRAME, AAT_PLATE, 0.16f, AAT_PIPE);
    for (int screen = -1; screen <= 1; screen += 2)
    {
        Panel(
            mesh,
            Offset(leftWall, screen * 0.17f, 0.06f, 0.03f),
            0.12f,
            0.09f,
            0.022f,
            0.012f,
            AAT_FRAME,
            AAT_SCREEN,
            0,
            AAT_PIPE);
    }
    for (int k = 0; k < 3; ++k)
    {
        const float y = -0.2f + 0.12f * k;
        const Vec3 conduit[4] =
            {{-0.86f, y, 0.95f}, {-0.86f, y, 1.6f}, {-0.86f, y, 2.3f}, {-0.6f + 0.1f * k, y, BACK - 0.05f}};
        Vec3 path[24];
        const int n = Smooth(conduit, 4, 5, path, 24);
        Tube(mesh, path, n, 0.035f - 0.006f * k, 8, AAT_PIPE);
    }
    PowerPlantHousing(mesh, FLOOR);
    CapacitorBank(mesh, FLOOR);
    // Along the left wall behind the driver's platform, clear of the ladder's foot.
    WallHousing(mesh, FLOOR, -0.90f, -1.0f, 0.55f, 1.40f, 2.40f, 4);
    // Ceiling: cable bundles sagging between the ribs along the hood's crown.
    for (int cable = 0; cable < 7; ++cable)
    {
        const float x = 0.45f + 0.28f * cable;
        float crown = 0;
        for (int i = 0; i + 1 < curveCount; ++i)
        {
            if ((curve[i].x - x) * (curve[i + 1].x - x) <= 0 && curve[i].y > 1.5f)
            {
                const float t = (x - curve[i].x) / (curve[i + 1].x - curve[i].x);
                crown = curve[i].y + (curve[i + 1].y - curve[i].y) * t;
            }
        }
        if (crown < 2.0f)
        {
            continue;
        }
        const float y = crown - 0.16f - 0.02f * (cable % 3);
        // They run into the turret's race ring (PLAN 57 k) where they would pass under it.
        const float ringDx = x - AAT_CABIN_EYE[0], ringReach = AAT_TURRET_RING_RADIUS + 0.10f;
        const bool reachesRing = fabsf(ringDx) < ringReach;
        const float endZ =
            reachesRing ? AAT_CABIN_EYE[2] - sqrtf(ringReach * ringReach - ringDx * ringDx) : BACK - 0.02f;
        // Into the ring's collar (hung 0.25 m under the crown, AatCommanderDeck), not ending in the air.
        const Vec3 p0 = {x, y, 1.3f}, p3 = {x, reachesRing ? crown - 0.29f : y, endZ};
        Vec3 path[16];
        for (int i = 0; i < 16; ++i)
        {
            const float t = static_cast<float>(i) / 15.0f;
            path[i] = Lerp(p0, p3, t) + Vec3{0, -0.12f * sinf(PI * fmodf(t * 3.0f, 1.0f)), 0};
        }
        Tube(mesh, path, 16, 0.022f + 0.008f * (cable % 2), 6, cable % 3 == 0 ? AAT_DARK : AAT_PIPE);
    }
    // Trim: a bolted moulding over the seam round the hood's rear arch and a plain one along the console's lower lip;
    // skirting where the walls, the bulkhead and the skirt meet the deck; deck plate seams across the open deck and
    // two hatches in it; the driver's platform edged.
    SeamTrim(mesh, arch, 11, 3, 0.03f, 6, 0.8f, AAT_BAND, AAT_PIPE);
    SeamTrim(mesh, lip, 11, 2, 0.025f, 6, 0, AAT_BAND, AAT_PIPE);
    for (int side = 0; side < 2; ++side)
    {
        const float x = side == 0 ? -0.90f : 3.62f, inward = side == 0 ? 1.0f : -1.0f;
        const float from = side == 0 ? 1.05f : AAT_LOWER_BACK; // on the right the housing runs to the ramp wall
        Strip(mesh, {x, FLOOR, from}, {x, FLOOR, BACK - 0.02f}, {0, 1, 0}, {inward, 0, 0}, 0.1f, 0.04f, AAT_FRAME);
    }
    Strip(
        mesh,
        {-0.90f, FLOOR, AAT_LOWER_BACK - 0.04f},
        {3.62f, FLOOR, AAT_LOWER_BACK - 0.04f},
        {0, 1, 0},
        {0, 0, -1},
        0.1f,
        0.04f,
        AAT_FRAME);
    Vec3 skirting[24];
    int skirtingCount = 0;
    for (int i = 0; i < lipCount && skirtingCount < 24; i += 4)
    {
        const Vec3 inward = Normalized(Vec3{1.35f, FLOOR, 0.6f} - lipFloor[i]);
        skirting[skirtingCount++] = lipFloor[i] + inward * 0.03f + Vec3{0, 0.03f, 0};
    }
    Tube(mesh, skirting, skirtingCount, 0.035f, 4, AAT_FRAME);
    for (int seam = 0; seam < 4; ++seam)
    {
        const float z = -1.5f + 0.75f * seam;
        DeckSeam(mesh, {-0.6f, FLOOR, z}, {3.3f, FLOOR, z}, 0.03f, AAT_DARK);
    }
    FloorHatch(mesh, {0.2f, FLOOR, -0.35f}, 0.3f, 0.22f, AAT_FRAME, AAT_PANEL, AAT_PIPE);
    FloorHatch(mesh, {2.3f, FLOOR, -0.6f}, 0.25f, 0.25f, AAT_FRAME, AAT_MACHINE_B, AAT_PIPE);
    Strip(
        mesh,
        {-0.62f, PLATFORM, platformFront},
        {0.62f, PLATFORM, platformFront},
        {0, 0, 1},
        {0, 1, 0},
        0.05f,
        0.012f,
        AAT_BAND);
    for (int side = -1; side <= 1; side += 2)
    {
        Strip(
            mesh,
            {side * 0.62f, PLATFORM, platformFront + 0.02f},
            {side * 0.62f, PLATFORM, platformBack - 0.02f},
            {1, 0, 0},
            {0, 1, 0},
            0.05f,
            0.012f,
            AAT_BAND);
    }
    // Details (PLAN 57 k, idren2023 2026-10-19: more aesthetic detail, 60 000 triangles allowed; after the DK
    // cross-section's energy shell magazines): two magazine racks on the right wall over its equipment racks (low on
    // the left wall they hid behind the driver's platform), a grating down the middle of the deck, conduits along both
    // floor edges.
    for (int rack = 0; rack < 2; ++rack)
    {
        EnergyShellRack(mesh, MakeFrame({3.60f, 0.75f, 1.45f + rack * 0.80f}, {0, 0, 1}, {0, 1, 0}));
    }
    Grille(
        mesh,
        MakeFrame({1.35f, FLOOR + 0.006f, 1.80f}, {1, 0, 0}, {0, 0, -1}),
        0.32f,
        0.78f,
        0.03f,
        18,
        0.015f,
        AAT_FRAME,
        AAT_DARK);
    for (int side = 0; side < 1; ++side) // the right side is the power plant's housing
    {
        const float x = side == 0 ? -0.82f : 3.54f;
        for (int pipe = 0; pipe < 2; ++pipe)
        {
            const Vec3 run[2] = {
                {x + (side == 0 ? 1 : -1) * pipe * 0.07f, FLOOR + 0.07f, 0.95f},
                {x + (side == 0 ? 1 : -1) * pipe * 0.07f, FLOOR + 0.07f, AAT_LOWER_BACK - 0.06f}};
            Tube(mesh, run, 2, 0.032f - pipe * 0.008f, 8, pipe == 0 ? AAT_PIPE : AAT_DARK);
        }
    }
    // The commander's deck and turning seat over the rear hatchway (PLAN 57 k), under the hood's crown.
    float crown = 0;
    for (int i = 0; i + 1 < curveCount; ++i)
    {
        if ((curve[i].x - AAT_CABIN_EYE[0]) * (curve[i + 1].x - AAT_CABIN_EYE[0]) <= 0 && curve[i].y > 1.5f)
        {
            const float t = (AAT_CABIN_EYE[0] - curve[i].x) / (curve[i + 1].x - curve[i].x);
            crown = curve[i].y + (curve[i + 1].y - curve[i].y) * t;
        }
    }
    AatCommanderDeck(mesh, crown, FLOOR, BACK);
    AatCommanderSeat(mesh);
}

// ---- Fighter tanks (rep_hover_fightertank, imp_hover_fightertank) ----
// Both use one cockpit model (the Imperial copy 0.12 m nearer the eye, dz): a left hex wall at x -1.0 ending at
// z 1.10 + dz, a roof sloping down to the front whose rear edge runs across from (-1.08, 2.85, 0.70 + dz) to
// (1.97, 2.75, -0.58 + dz), and a front set at an angle (window, a dashboard reaching x 2.18, a lower front wall down
// to y -0.58). Open: the whole right side behind the front, the floor and the back. Each shell closes that into a
// cabin 3.3 m wide, its own way: the Republic's clean and squared with clone red, the Empire's dark and angular.

/// A U-shaped frame round the cabin's cross-section at depth z: up the right wall, across under the roof and, behind
/// the cockpit's left wall, down the left one; corners cut at `corner`. Profile: depth into the cabin, width along z.
void CabinRib(
    Mesh& mesh,
    float z,
    float left,
    float right,
    float floorY,
    float roofY,
    float corner,
    float depth,
    float width,
    bool hasLeftLeg,
    bool isChamfered,
    int finish)
{
    Vec3 path[8];
    int count = 0;
    path[count++] = {right, floorY, z};
    path[count++] = {right, roofY - corner, z};
    path[count++] = {right - corner, roofY, z};
    path[count++] = {left + corner, roofY, z};
    if (hasLeftLeg)
    {
        path[count++] = {left, roofY - corner, z};
        path[count++] = {left, floorY, z};
    }
    // Profile: x into the cabin is negative (the sweep's side points out of the walls here).
    const float square[4][2] = {{0, -width * 0.5f}, {-depth, -width * 0.5f}, {-depth, width * 0.5f}, {0, width * 0.5f}};
    const float cut[6][2] = {
        {0, -width * 0.5f},
        {-depth * 0.7f, -width * 0.5f},
        {-depth, -width * 0.25f},
        {-depth, width * 0.25f},
        {-depth * 0.7f, width * 0.5f},
        {0, width * 0.5f}};
    if (isChamfered)
    {
        Sweep(mesh, path, count, cut, 6, true, false, {0, 0, 1}, finish, true);
    }
    else
    {
        Sweep(mesh, path, count, square, 4, true, false, {0, 0, 1}, finish, true);
    }
}

/// A fighter tank's cabin (both tanks share the layout, BuildRepublicTank / BuildImperialTank) for its fittings.
struct TankCabin
{
    float left, right, floorY, roofY, front, back, wallEnd;
    float hatchX, hatchHalfWidth; // the rear hatch's middle and half width
    DetailLook look;
    int cable;
    bool hasRack;       // a power cell rack between the right wall's frames at z 0.13 (the Republic's frames leave room)
    float panelStandoff; // the left and right control panels this far off the wall (the Imperial's wall plates)
};

// The fighter tanks' fittings (idren2023 2026-10-19: about 50 000 triangles each, polished; after the exterior: a
// military hover tank, its crew compartment a working space): lockers and a control panel on the right wall before
// the driver's station, a power cell rack between the frames behind them, a control panel and an extinguisher on the
// left wall behind the cockpit's, a flanged coolant pipe and cable looms along the roof's edges, grab handles beside
// the hatch and over the walkway, coolant risers with hand wheels in the rear corners.
void FighterTankFittings(Mesh& mesh, const TankCabin& cabin)
{
    const DetailLook& look = cabin.look;
    const Vec3 up = {0, 1, 0};
    const Frame rightWall = MakeFrame({cabin.right - 0.004f, 0, 0}, {0, 0, 1}, up); // z: -x, into the cabin
    const Frame leftWall = MakeFrame({cabin.left + 0.004f, 0, 0}, {0, 0, -1}, up);  // z: +x
    const auto onWall = [](const Frame& wall, float z, float y)
    {
        Frame at = wall;
        at.origin = {wall.origin.x, y, z};
        return at;
    };
    // Right wall before the station: two lockers, a control panel over them, a power cell rack behind.
    for (int k = 0; k < 2; ++k)
    {
        Locker(mesh, onWall(rightWall, cabin.front + 0.42f + 0.40f * k, cabin.floorY + 0.62f), 0.17f, 0.55f, 0.2f, look);
    }
    // A control panel standing off the wall (over the Imperial's wall plates) sits on a block back to the wall.
    const auto standingPanel = [&](const Frame& wall, float halfWidth, float halfHeight)
    {
        if (cabin.panelStandoff > 0)
        {
            Box(mesh,
                Offset(wall, 0, 0, cabin.panelStandoff * 0.5f),
                {halfWidth, halfHeight, cabin.panelStandoff * 0.5f},
                0.01f,
                look.frame);
        }
        ControlPanel(mesh, Offset(wall, 0, 0, cabin.panelStandoff), halfWidth, halfHeight, look);
    };
    standingPanel(onWall(rightWall, cabin.front + 0.62f, 1.52f), 0.34f, 0.2f);
    if (cabin.hasRack)
    {
        CellRack(mesh, onWall(rightWall, 0.13f, 1.05f), 3, 1, look);
    }
    // Left wall behind the cockpit's: a control panel, an extinguisher.
    standingPanel(onWall(leftWall, cabin.wallEnd + 0.33f, 1.42f), 0.22f, 0.17f);
    Extinguisher(mesh, onWall(leftWall, cabin.back - 0.45f, 0.45f), look);
    // Roof edges: a flanged pipe on the left, a cable loom on the right, both the cabin's length.
    const float pipeX = cabin.left + 0.16f, loomX = cabin.right - 0.07f;
    const Vec3 pipe[3] = {
        {pipeX, cabin.roofY - 0.28f, cabin.wallEnd - 0.2f},
        {pipeX, cabin.roofY - 0.28f, (cabin.wallEnd + cabin.back) * 0.5f},
        {pipeX, cabin.roofY - 0.28f, cabin.back - 0.12f}};
    FlangedPipe(mesh, pipe, 3, 0.035f, {1, 0, 0}, 0.5f, look.metal, look.frame);
    const Vec3 loom[3] = {
        {loomX, 2.07f, cabin.front + 0.25f},
        {loomX, 2.09f, (cabin.front + cabin.back) * 0.5f},
        {loomX, 2.07f, cabin.back - 0.1f}};
    CableLoom(mesh, loom, 3, 4, 0.011f, up, cabin.cable, look.frame, 0.45f);
    // Coolant risers by the rear corners (clear of the corner posts, the lockers and the station), each with a hand
    // wheel at chest height.
    for (int side = -1; side <= 1; side += 2)
    {
        const float x = side < 0 ? cabin.left + 0.08f : cabin.right - 0.26f;
        const float z = side < 0 ? cabin.back - 0.25f : cabin.back - 0.12f;
        const Vec3 riser[2] = {{x, cabin.floorY + 0.05f, z}, {x, cabin.roofY - 0.05f, z}};
        FlangedPipe(mesh, riser, 2, 0.04f, {static_cast<float>(-side), 0, 0}, 0.9f, look.metal, look.frame);
        const float body[5][2] = {{-0.07f, 0.05f}, {-0.06f, 0.065f}, {0.06f, 0.065f}, {0.07f, 0.05f}, {0.07f, 0.04f}};
        Lathe(mesh, MakeFrame({x, 1.2f, z}, {1, 0, 0}, {0, 1, 0}), body, 5, 16, look.frame);
        // The wheel on the valve's inboard face (its z toward the cabin's middle).
        const Frame face = MakeFrame({x - side * 0.065f, 1.2f, z}, {0, 0, side * 1.0f}, up);
        ValveWheel(mesh, face, 0.09f, 0.05f, look.accent, look.metal);
    }
    // Grab handles: upright either side of the hatch, and along the roof over the walkway.
    for (int side = -1; side <= 1; side += 2)
    {
        const float x = cabin.hatchX + side * (cabin.hatchHalfWidth + 0.14f);
        GrabHandle(mesh, {x, 0.35f, cabin.back - 0.004f}, {x, 0.95f, cabin.back - 0.004f}, {0, 0, -1}, 0.06f, 0.014f, look.metal, look.frame);
    }
    // Over the hatch, a rack of spare power cells; along the roof's middle, a cable loom from the front to the rear wall.
    CellRack(mesh, MakeFrame({cabin.hatchX, cabin.roofY - 0.42f, cabin.back - 0.004f}, {-1, 0, 0}, up), 2, 3, look);
    const Vec3 roofLoom[3] = {
        {0.62f, cabin.roofY - 0.035f, cabin.wallEnd - 0.4f},
        {0.62f, cabin.roofY - 0.035f, (cabin.wallEnd + cabin.back) * 0.5f},
        {0.62f, cabin.roofY - 0.06f, cabin.back - 0.02f}};
    CableLoom(mesh, roofLoom, 3, 3, 0.012f, {1, 0, 0}, cabin.cable, look.frame, 0.4f);
    for (int k = 0; k < 3; ++k)
    {
        const float z = cabin.front + 1.4f + 0.8f * k;
        GrabHandle(mesh, {1.2f, cabin.roofY - 0.004f, z}, {1.2f, cabin.roofY - 0.004f, z + 0.35f}, {0, -1, 0}, 0.07f, 0.014f, look.metal, look.frame);
    }
}

enum RepFinish
{
    REP_PLATE,
    REP_HEX,
    REP_RAIL,
    REP_VENT,
    REP_MECH,
    REP_FINE,
    REP_CONSOLE_A,
    REP_CONSOLE_B,
    REP_CONSOLE_C,
    REP_SCREEN,
    REP_DARK,
    REP_SEAT,
    REP_RED,
    REP_LAMP,
    REP_PIPE,
    REP_FRAME,
    REP_STATUS,
    REP_LIGHT,
    REP_FACE,
    REP_LAMP_ROW,
    REP_TOGGLES,
    REP_BUTTONS,
    REP_AMBER,
    REP_KNOBS,
    REP_MINI,
    REP_STOCK_FRAME,
    REP_STOCK_GLASS,
    REP_STOCK_BACKING,
    REP_FINISH_COUNT
};

// Patches of rep_1st_cockpit_fightertank (512 x 512; texel rectangles in the comments) and rep_cockpit_fx (64 x 64).
const Finish REP_FINISHES[REP_FINISH_COUNT] = {
    {{0.809f, 0.617f, 0.965f, 0.820f}, 0.6f, 1, 0x8C8C90, 0.18f, 14, 0},         // grey plate (414..494, 316..420)
    {{0.008f, 0.191f, 0.523f, 0.371f}, 1.1f, 1, 0x8A8A8E, 0.12f, 12, 0},         // hex grille (4..268, 98..190)
    {{0.000f, 0.039f, 0.742f, 0.117f}, 1.6f, 1, 0x8A8A8E, 0.25f, 18, 0},         // rails (0..380, 20..60)
    {{0.820f, 0.020f, 0.977f, 0.166f}, 0.3f, 1, 0x8A8A8E, 0.15f, 12, 0},         // speaker vent (420..500, 10..85)
    {{0.772f, 0.186f, 0.998f, 0.478f}, 0.45f, 1, 0x86868A, 0.15f, 12, 0},        // machinery (395..511, 95..245)
    {{0.445f, 0.390f, 0.600f, 0.488f}, 0.3f, 1, 0x8A8A8E, 0.15f, 12, 0},         // fine grille (228..308, 200..250)
    {{0.008f, 0.859f, 0.332f, 0.992f}, 0.42f, 1, 0x909094, 0.1f, 10, 0.15f},     // switch panel A (4..170, 440..508)
    {{0.348f, 0.859f, 0.660f, 0.992f}, 0.42f, 1, 0x909094, 0.1f, 10, 0.15f},     // switch panel B (178..338, 440..508)
    {{0.676f, 0.859f, 0.988f, 0.992f}, 0.42f, 1, 0x909094, 0.1f, 10, 0.15f},     // switch panel C (346..506, 440..508)
    {{0.621f, 0.674f, 0.793f, 0.797f}, 0.30f, 1, 0xA0A0A0, 0, 1, 0.8f},          // green readout (318..406, 345..408)
    {{0, 0, 1, 1}, 1, 0, 0x16171A, 0.05f, 6, 0},                                 // recesses
    {{0.809f, 0.617f, 0.965f, 0.820f}, 0.5f, 1, 0x44464C, 0.12f, 9, 0},          // seat pads
    {{0, 0, 1, 1}, 1, 0, 0x9A1E18, 0.3f, 16, 0},                                 // clone red
    {{0, 0, 1, 1}, 1, 0, 0xD8E6FF, 0, 1, 1.0f},                                  // lamps (cool white)
    {{0.820f, 0.700f, 0.900f, 0.760f}, 0.25f, 1, 0xA6A6AA, 0.45f, 30, 0},        // pipes, rails (420..461, 358..389)
    {{0.809f, 0.617f, 0.965f, 0.820f}, 0.8f, 1, 0x5E6066, 0.2f, 14, 0},          // dark frames
    {{0.000f, 0.500f, 1.000f, 0.750f}, 0.15f, 2, 0xC0C0C0, 0, 1, 1.0f},          // status lights (fx 0..64, 32..48)
    {{0.547f, 0.195f, 0.713f, 0.336f}, 0.5f, 1, 0x9C9C9E, 0.2f, 14, 0},          // light plate (280..365, 100..172)
    {{0, 0, 1, 1}, 1, 0, 0x0C1418, 0.70f, 40, 0.35f},                            // display glass (blue-black)
    {{0.6172f, 0.6191f, 0.7969f, 0.6602f}, 0.2f, 1, 0x8C8C90, 0.1f, 10, 0.55f},  // lamp row (316..408, 317..338)
    {{0.2188f, 0.6914f, 0.3242f, 0.7480f}, 0.1f, 1, 0x8C8C90, 0.2f, 14, 0.1f},   // toggles (112..166, 354..383)
    {{0.3887f, 0.6328f, 0.4160f, 0.7012f}, 0.05f, 1, 0x8C8C90, 0.2f, 14, 0.35f}, // buttons (199..213, 324..359)
    {{0.4492f, 0.5977f, 0.5293f, 0.6270f}, 0.1f, 1, 0x8C8C90, 0.1f, 10, 0.45f},  // amber gauge (230..271, 306..321)
    {{0.4746f, 0.6367f, 0.5215f, 0.7500f}, 0.05f, 1, 0x8C8C90, 0.3f, 18, 0.0f},  // knobs (243..267, 326..384)
    {{0.3848f, 0.7051f, 0.4453f, 0.7422f}, 0.05f, 1, 0x8C8C90, 0.1f, 10, 0.5f},  // small readout (197..228, 361..380)
    ScreenFrameFinish(),                                                         // stock screens (PLAN 57 j): frame
    ScreenGlassFinish(REP_SCREEN_ART),                                           // glass
    ScreenBackingFinish(REP_SCREEN_ART),                                         // behind the glass
};

const StockScreenLook REP_SCREENS = {&REP_SCREEN_ART, REP_STOCK_FRAME, REP_STOCK_GLASS, REP_STOCK_BACKING, REP_DARK};

void BuildRepublicTank(Mesh& mesh)
{
    constexpr float FLOOR = -0.60f, ROOF = 2.92f, LEFT = -1.08f, RIGHT = 2.26f, FRONT = -1.92f, BACK = 2.45f;
    constexpr float PLATFORM = 0.25f, WALL_END = 1.10f;
    // Floor: grey plating, a grated walkway from the driver's platform to the hatch.
    TiledQuad(mesh, {LEFT, FLOOR, FRONT}, {RIGHT - LEFT, 0, 0}, {0, 0, BACK - FRONT}, REP_PLATE);
    TreadPlate(mesh, {0.72f, FLOOR, -0.4f}, {0.75f, 0, 0}, {0, 0, BACK - 0.05f + 0.4f}, 0.075f, REP_PLATE, REP_PIPE);
    for (int side = 0; side < 2; ++side)
    {
        const float x = side == 0 ? 0.70f : 1.49f;
        Box(mesh,
            MakeFrame({x, FLOOR + 0.008f, (BACK - 0.45f) * 0.5f}, {1, 0, 0}, {0, 1, 0}),
            {0.02f, 0.008f, (BACK + 0.4f) * 0.5f},
            0.004f,
            REP_FRAME);
    }
    // Right wall in bands: machinery low, hex grille at shoulder height, a rail, plain plate up to the roof.
    const float bands[5] = {FLOOR, 0.35f, 1.95f, 2.2f, ROOF};
    const int bandFinish[4] = {REP_MECH, REP_HEX, REP_RAIL, REP_PLATE};
    for (int band = 0; band < 4; ++band)
    {
        TiledQuad(
            mesh,
            {RIGHT, bands[band], FRONT},
            {0, 0, BACK - FRONT},
            {0, bands[band + 1] - bands[band], 0},
            bandFinish[band]);
        // Behind the cockpit's left wall the same bands, starting under its end.
        TiledQuad(
            mesh,
            {LEFT - 0.01f, bands[band], WALL_END - 0.5f},
            {0, 0, BACK - WALL_END + 0.5f},
            {0, bands[band + 1] - bands[band], 0},
            bandFinish[band]);
    }
    // Roof and rear wall.
    TiledQuad(mesh, {LEFT, ROOF, FRONT}, {RIGHT - LEFT, 0, 0}, {0, 0, BACK - FRONT}, REP_PLATE);
    TiledQuad(mesh, {LEFT, FLOOR, BACK}, {RIGHT - LEFT, 0, 0}, {0, ROOF - FLOOR, 0}, REP_PLATE);
    // A pillar closing the corner between the cockpit's front and the right wall.
    Box(mesh,
        MakeFrame({2.16f, (FLOOR + ROOF) * 0.5f, -1.93f}, {1, 0, 0}, {0, 1, 0}),
        {0.11f, (ROOF - FLOOR) * 0.5f, 0.08f},
        0.02f,
        REP_FRAME);
    // Frames round the cabin, squared, clone red where they meet the floor.
    const float ribs[4] = {-0.25f, 0.5f, 1.3f, 2.05f};
    for (const float z : ribs)
    {
        CabinRib(mesh, z, LEFT, RIGHT, FLOOR, ROOF, 0.22f, 0.07f, 0.12f, z > WALL_END, false, REP_FRAME);
        Box(mesh,
            MakeFrame({RIGHT - 0.04f, FLOOR + 0.12f, z}, {0, 0, 1}, {0, 1, 0}),
            {0.065f, 0.1f, 0.045f},
            0.01f,
            REP_RED);
    }
    // Ceiling: two light strips along the cabin and a conduit run on the right.
    for (int strip = 0; strip < 2; ++strip)
    {
        const float x = strip == 0 ? 0.1f : 1.45f;
        Box(mesh, MakeFrame({x, ROOF - 0.03f, 0.6f}, {1, 0, 0}, {0, 1, 0}), {0.07f, 0.025f, 1.6f}, 0.01f, REP_FRAME);
        Quad(
            mesh,
            {x - 0.045f, ROOF - 0.056f, -0.95f},
            {x + 0.045f, ROOF - 0.056f, -0.95f},
            {x + 0.045f, ROOF - 0.056f, 2.15f},
            {x - 0.045f, ROOF - 0.056f, 2.15f},
            REP_LAMP);
    }
    for (int k = 0; k < 3; ++k)
    {
        const Vec3 run[2] = {
            {1.85f + 0.07f * k, ROOF - 0.08f - 0.03f * (k % 2), -1.5f},
            {1.85f + 0.07f * k, ROOF - 0.08f - 0.03f * (k % 2), BACK - 0.02f}};
        Tube(mesh, run, 2, 0.025f, 8, k == 1 ? REP_RED : REP_PIPE);
    }
    // Driver's platform, seat, front console.
    const float platformFront = _eye.z - PLATFORM_AHEAD, platformBack = 2.54f;
    Box(mesh,
        MakeFrame({0, (FLOOR + PLATFORM) * 0.5f, (platformFront + platformBack) * 0.5f}, {1, 0, 0}, {0, 1, 0}),
        {0.6f, (PLATFORM - FLOOR) * 0.5f, (platformBack - platformFront) * 0.5f},
        0.025f,
        REP_FRAME);
    Panel(
        mesh,
        MakeFrame({0, (FLOOR + PLATFORM) * 0.5f, platformFront - 0.01f}, {1, 0, 0}, {0, 1, 0}),
        0.56f,
        0.36f,
        0.04f,
        0.02f,
        REP_FRAME,
        REP_VENT,
        0.22f,
        REP_PIPE);
    Quad(
        mesh,
        {-0.56f, PLATFORM + 0.001f, platformFront + 0.02f},
        {0.56f, PLATFORM + 0.001f, platformFront + 0.02f},
        {0.56f, PLATFORM + 0.001f, platformFront + 0.06f},
        {-0.56f, PLATFORM + 0.001f, platformFront + 0.06f},
        REP_RED);
    const SeatLook seat = {0.22f, 0.60f, 0.14f, false, false, REP_SEAT, REP_FRAME, REP_PIPE, REP_RED, REP_DARK};
    Seat(mesh, _eye, PLATFORM, seat);
    const ConsoleLook console = {REP_FRAME, REP_PLATE, REP_PIPE, REP_PIPE, REP_LAMP, REP_DARK, REP_SEAT, &REP_SCREENS};
    FrontConsole(
        mesh,
        PLATFORM,
        console,
        {1.0f,
         REP_PLATE,
         REP_FRAME,
         REP_FACE,
         REP_PIPE,
         REP_LAMP,
         {{REP_BUTTONS, -0.115f, 0.02f, 0.012f, 0.03f},
          {REP_TOGGLES, -0.04f, -0.085f, 0.045f, 0.024f},
          {REP_MINI, 0.06f, -0.085f, 0.026f, 0.016f}},
         {{REP_KNOBS, 0.115f, 0.02f, 0.012f, 0.029f},
          {REP_AMBER, -0.045f, -0.085f, 0.038f, 0.014f},
          {REP_MINI, 0.05f, -0.085f, 0.026f, 0.016f}},
         {{REP_LAMP_ROW, 0, 0.11f, 0.085f, 0.0194f}}});
    // Comms station along the right wall beside the driver (clear of the view ahead): a desk, a sloped panel of switch
    // boards and a readout, a stool.
    constexpr float STATION_Z = 1.5f;
    const Frame desk = MakeFrame({1.95f, 0.72f, STATION_Z}, {0, 0, -1}, {0, 1, 0});
    Box(mesh, desk, {0.75f, 0.03f, 0.3f}, 0.012f, REP_FRAME);
    Box(mesh,
        MakeFrame({2.0f, (FLOOR + 0.69f) * 0.5f, STATION_Z}, {0, 0, -1}, {0, 1, 0}),
        {0.72f, (0.69f - FLOOR) * 0.5f, 0.24f},
        0.015f,
        REP_PLATE);
    const Frame board = MakeFrame({2.18f, 1.18f, STATION_Z}, {0, 0, -1}, {-0.35f, 1, 0});
    Box(mesh, Offset(board, 0, 0, -0.03f), {0.74f, 0.36f, 0.03f}, 0.012f, REP_FRAME);
    const int boards[3] = {REP_CONSOLE_A, REP_CONSOLE_B, REP_CONSOLE_C};
    for (int k = 0; k < 3; ++k)
    {
        Panel(
            mesh,
            Offset(board, -0.48f + 0.48f * k, -0.14f, 0.004f),
            0.22f,
            0.12f,
            0.018f,
            0.008f,
            REP_FRAME,
            boards[k],
            0,
            REP_PIPE);
    }
    Panel(mesh, Offset(board, -0.25f, 0.17f, 0.004f), 0.2f, 0.13f, 0.02f, 0.01f, REP_FRAME, REP_SCREEN, 0, REP_PIPE);
    Panel(mesh, Offset(board, 0.3f, 0.17f, 0.004f), 0.22f, 0.13f, 0.02f, 0.01f, REP_FRAME, REP_VENT, 0, REP_PIPE);
    {
        const int before = mesh.count;
        const float stool[6][2] =
            {{0, 0.16f}, {0.03f, 0.16f}, {0.05f, 0.04f}, {0.42f, 0.035f}, {0.44f, 0.17f}, {0.5f, 0.17f}};
        Lathe(mesh, MakeFrame({0, FLOOR, 0}, {1, 0, 0}, {0, 1, 0}), stool, 6, 16, REP_SEAT);
        MoveSince(mesh, before, {1.35f, 0, STATION_Z});
    }
    // Rear wall: the hatch with a red-striped frame, lockers behind the driver, a fire extinguisher.
    const Frame hatch = MakeFrame({1.15f, FLOOR + 1.0f, BACK - 0.02f}, {-1, 0, 0}, {0, 1, 0});
    Panel(mesh, hatch, 0.58f, 1.0f, 0.08f, 0.05f, REP_FRAME, REP_PLATE, 0.25f, REP_PIPE);
    Panel(mesh, Offset(hatch, 0, -0.05f, 0.012f), 0.42f, 0.82f, 0.05f, 0.02f, REP_PLATE, REP_HEX, 0, REP_PIPE);
    for (int stripe = 0; stripe < 6; ++stripe)
    {
        const float x0 = -0.5f + 0.18f * stripe;
        Quad(
            mesh,
            hatch.At(x0, 1.04f, 0.052f),
            hatch.At(x0 + 0.09f, 1.04f, 0.052f),
            hatch.At(x0 + 0.15f, 0.94f, 0.052f),
            hatch.At(x0 + 0.06f, 0.94f, 0.052f),
            REP_RED);
    }
    const Vec3 handle[4] = {
        hatch.At(-0.33f, -0.1f, 0.02f),
        hatch.At(-0.33f, -0.1f, 0.09f),
        hatch.At(-0.33f, 0.15f, 0.09f),
        hatch.At(-0.33f, 0.15f, 0.02f)};
    Tube(mesh, handle, 4, 0.016f, 8, REP_PIPE);
    for (int locker = 0; locker < 3; ++locker)
    {
        const Frame door = MakeFrame({-0.8f + 0.3f * locker, FLOOR + 1.05f, BACK - 0.02f}, {-1, 0, 0}, {0, 1, 0});
        Grille(mesh, door, 0.13f, 0.7f, 0.025f, 9, 0.025f, REP_FRAME, REP_PLATE);
    }
    // Upper wall panels between the frames, vented, riveted.
    const float bays[5] = {-1.85f, -0.25f, 0.5f, 1.3f, 2.05f};
    for (int bay = 0; bay + 1 < 5; ++bay)
    {
        const float z0 = bays[bay] + 0.1f, z1 = bays[bay + 1] - 0.1f;
        const Frame panel = MakeFrame({RIGHT - 0.003f, 2.55f, (z0 + z1) * 0.5f}, {0, 0, -1}, {0, 1, 0});
        Panel(mesh, panel, (z1 - z0) * 0.5f, 0.27f, 0.045f, 0.02f, REP_FRAME, REP_LIGHT, 0.18f, REP_PIPE);
        Grille(
            mesh,
            Offset(panel, 0, 0, 0.02f),
            fminf(0.2f, (z1 - z0) * 0.3f),
            0.12f,
            0.02f,
            5,
            0.015f,
            REP_FRAME,
            REP_FINE);
    }
    // Ceiling vents between the light strips.
    for (int vent = 0; vent < 3; ++vent)
    {
        Grille(
            mesh,
            MakeFrame({0.78f, ROOF - 0.003f, -0.6f + 1.1f * vent}, {1, 0, 0}, {0, 0, -1}),
            0.32f,
            0.2f,
            0.03f,
            6,
            0.02f,
            REP_FRAME,
            REP_FINE);
    }
    // Medical kit on the left wall behind the cockpit's, white with a red cross.
    const Frame medkit = MakeFrame({LEFT + 0.07f, 1.45f, 1.85f}, {0, 0, -1}, {0, 1, 0});
    Box(mesh, medkit, {0.17f, 0.12f, 0.06f}, 0.02f, REP_LIGHT);
    Quad(
        mesh,
        medkit.At(-0.025f, -0.075f, 0.061f),
        medkit.At(0.025f, -0.075f, 0.061f),
        medkit.At(0.025f, 0.075f, 0.061f),
        medkit.At(-0.025f, 0.075f, 0.061f),
        REP_RED);
    Quad(
        mesh,
        medkit.At(-0.075f, -0.025f, 0.0615f),
        medkit.At(0.075f, -0.025f, 0.0615f),
        medkit.At(0.075f, 0.025f, 0.0615f),
        medkit.At(-0.075f, 0.025f, 0.0615f),
        REP_RED);
    // Tread plates before the hatch.
    for (int plate = 0; plate < 2; ++plate)
    {
        Panel(
            mesh,
            MakeFrame({1.1f, FLOOR + 0.006f, 1.95f + 0.25f * plate}, {1, 0, 0}, {0, 0, -1}),
            0.5f,
            0.11f,
            0.02f,
            0.008f,
            REP_FRAME,
            REP_VENT,
            0.2f,
            REP_PIPE);
    }
    // Grab handles on the roof over the station.
    for (int k = 0; k < 2; ++k)
    {
        const float z = 1.1f + 0.8f * k;
        const Vec3 grip[4] = {
            {1.6f, ROOF, z - 0.12f},
            {1.6f, ROOF - 0.08f, z - 0.1f},
            {1.6f, ROOF - 0.08f, z + 0.1f},
            {1.6f, ROOF, z + 0.12f}};
        Tube(mesh, grip, 4, 0.014f, 8, REP_RED);
    }
    // Trim: a bolted moulding round the game's cockpit's whole open edge (the roof's rear edge, the front's right and
    // bottom edges, the left wall's foot and rear edge); skirting and cornices along the walls, posts in the rear
    // corners, deck seams and hatches in the open floor before the platform, the platform edged.
    constexpr float DZ = 0.0f;
    const Vec3 rim[14] = {
        {-1.08f, 2.85f, 0.70f + DZ},
        {1.97f, 2.75f, -0.58f + DZ},
        {1.97f, 2.61f, -1.24f + DZ},
        {1.97f, 2.44f, -1.83f + DZ},
        {2.03f, 0.73f, -1.66f + DZ},
        {2.06f, 0.02f, -1.66f + DZ},
        {2.08f, -0.59f, -1.66f + DZ},
        {0.85f, -0.58f, -1.15f + DZ},
        {-0.98f, -0.56f, -0.38f + DZ},
        {-0.98f, -0.56f, 1.10f + DZ},
        {-1.00f, 0.01f, 1.10f + DZ},
        {-1.03f, 0.92f, 1.00f + DZ},
        {-1.03f, 1.01f, 0.99f + DZ},
        {-1.08f, 2.85f, 0.70f + DZ}};
    SeamTrim(mesh, rim, 14, 1, 0.028f, 6, 0.8f, REP_FRAME, REP_PIPE);
    for (int side = 0; side < 2; ++side)
    {
        const float x = side == 0 ? RIGHT : LEFT, inward = side == 0 ? -1.0f : 1.0f;
        const float z0 = side == 0 ? FRONT + 0.1f : WALL_END - 0.45f;
        Strip(mesh, {x, FLOOR, z0}, {x, FLOOR, BACK}, {0, 1, 0}, {inward, 0, 0}, 0.12f, 0.035f, REP_FRAME);
        Strip(mesh, {x, ROOF - 0.08f, z0}, {x, ROOF - 0.08f, BACK}, {0, 1, 0}, {inward, 0, 0}, 0.16f, 0.05f, REP_FRAME);
        Box(mesh,
            MakeFrame({x + inward * 0.06f, (FLOOR + ROOF) * 0.5f, BACK - 0.06f}, {1, 0, 0}, {0, 1, 0}),
            {0.06f, (ROOF - FLOOR) * 0.5f, 0.06f},
            0.02f,
            REP_FRAME);
    }
    Strip(mesh, {LEFT, FLOOR, BACK}, {RIGHT, FLOOR, BACK}, {0, 1, 0}, {0, 0, -1}, 0.12f, 0.035f, REP_FRAME);
    Strip(
        mesh,
        {LEFT, ROOF - 0.08f, BACK},
        {RIGHT, ROOF - 0.08f, BACK},
        {0, 1, 0},
        {0, 0, -1},
        0.16f,
        0.05f,
        REP_FRAME);
    for (int seam = 0; seam < 3; ++seam)
    {
        const float z = -1.0f + 0.55f * seam + DZ;
        DeckSeam(mesh, {-0.9f, FLOOR, z}, {RIGHT - 0.1f, FLOOR, z}, 0.03f, REP_DARK);
    }
    FloorHatch(mesh, {0.05f, FLOOR, 0.25f}, 0.32f, 0.2f, REP_FRAME, REP_VENT, REP_PIPE);
    FloorHatch(mesh, {1.25f, FLOOR, -0.75f + DZ}, 0.28f, 0.28f, REP_FRAME, REP_PLATE, REP_PIPE);
    Strip(
        mesh,
        {-0.6f, PLATFORM, platformFront + 0.01f},
        {0.6f, PLATFORM, platformFront + 0.01f},
        {0, 0, 1},
        {0, 1, 0},
        0.04f,
        0.01f,
        REP_RED);
    const TankCabin cabin = {
        LEFT, RIGHT, FLOOR, ROOF, FRONT, BACK, WALL_END, 1.15f, 0.58f, {REP_PLATE, REP_FRAME, REP_PIPE, REP_DARK, REP_LAMP, REP_RED}, REP_DARK, true, 0};
    FighterTankFittings(mesh, cabin);
}

enum ImpFinish
{
    IMP_PLATE,
    IMP_HEX,
    IMP_RAIL,
    IMP_VENT,
    IMP_MECH,
    IMP_FINE,
    IMP_CONSOLE_A,
    IMP_CONSOLE_C,
    IMP_SCREEN,
    IMP_DARK,
    IMP_SEAT,
    IMP_RED_LAMP,
    IMP_LAMP,
    IMP_PIPE,
    IMP_FRAME,
    IMP_STATUS,
    IMP_FACE,
    IMP_LAMP_ROW,
    IMP_TOGGLES,
    IMP_BUTTONS,
    IMP_AMBER,
    IMP_KNOBS,
    IMP_MINI,
    IMP_STOCK_FRAME,
    IMP_STOCK_GLASS,
    IMP_STOCK_BACKING,
    IMP_RED,
    IMP_FINISH_COUNT
};

// Patches of imp_1st_cockpit_fightertank (the same layout as the Republic's, darker) and imp_1st_cockpit_fx (64 x 64);
// the colours colder and darker.
const Finish IMP_FINISHES[IMP_FINISH_COUNT] = {
    {{0.809f, 0.617f, 0.965f, 0.820f}, 0.55f, 1, 0x6C6E74, 0.22f, 16, 0},        // dark grey plate (414..494, 316..420)
    {{0.008f, 0.191f, 0.523f, 0.371f}, 0.9f, 1, 0x606268, 0.12f, 12, 0},         // hex grille
    {{0.000f, 0.039f, 0.742f, 0.117f}, 1.4f, 1, 0x6A6C72, 0.25f, 18, 0},         // rails
    {{0.820f, 0.020f, 0.977f, 0.166f}, 0.26f, 1, 0x6A6C72, 0.15f, 12, 0},        // speaker vent
    {{0.772f, 0.186f, 0.998f, 0.478f}, 0.4f, 1, 0x66686E, 0.15f, 12, 0},         // machinery
    {{0.445f, 0.390f, 0.600f, 0.488f}, 0.25f, 1, 0x5E6066, 0.15f, 12, 0},        // fine grille (floor grating)
    {{0.008f, 0.859f, 0.332f, 0.992f}, 0.4f, 1, 0x7A7C80, 0.1f, 10, 0.12f},      // switch panel A
    {{0.676f, 0.859f, 0.988f, 0.992f}, 0.4f, 1, 0x7A7C80, 0.1f, 10, 0.12f},      // switch panel C
    {{0.621f, 0.674f, 0.793f, 0.797f}, 0.28f, 1, 0x9AA09A, 0, 1, 0.8f},          // green readout
    {{0, 0, 1, 1}, 1, 0, 0x0E0F11, 0.05f, 6, 0},                                 // recesses
    {{0.809f, 0.617f, 0.965f, 0.820f}, 0.45f, 1, 0x26272A, 0.18f, 10, 0},        // black seat pads
    {{0, 0, 1, 1}, 1, 0, 0xFF2A18, 0, 1, 1.0f},                                  // red lamps
    {{0, 0, 1, 1}, 1, 0, 0xC8D4E8, 0, 1, 0.9f},                                  // white lamps
    {{0.820f, 0.700f, 0.900f, 0.760f}, 0.25f, 1, 0x8A8C92, 0.5f, 34, 0},         // pipes, rails
    {{0.809f, 0.617f, 0.965f, 0.820f}, 0.7f, 1, 0x34363A, 0.25f, 16, 0},         // black frames
    {{0.000f, 0.500f, 1.000f, 0.750f}, 0.15f, 2, 0xC0C0C0, 0, 1, 1.0f},          // status lights (fx)
    {{0, 0, 1, 1}, 1, 0, 0x0A120C, 0.75f, 44, 0.35f},                            // display glass (green-black)
    {{0.6172f, 0.6191f, 0.7969f, 0.6602f}, 0.2f, 1, 0x74767A, 0.1f, 10, 0.55f},  // lamp row (316..408, 317..338)
    {{0.2188f, 0.6914f, 0.3242f, 0.7480f}, 0.1f, 1, 0x74767A, 0.2f, 14, 0.1f},   // toggles (112..166, 354..383)
    {{0.3887f, 0.6328f, 0.4160f, 0.7012f}, 0.05f, 1, 0x74767A, 0.2f, 14, 0.35f}, // buttons (199..213, 324..359)
    {{0.4492f, 0.5977f, 0.5293f, 0.6270f}, 0.1f, 1, 0x74767A, 0.1f, 10, 0.45f},  // amber gauge (230..271, 306..321)
    {{0.4746f, 0.6367f, 0.5215f, 0.7500f}, 0.05f, 1, 0x74767A, 0.3f, 18, 0.0f},  // knobs (243..267, 326..384)
    {{0.3848f, 0.7051f, 0.4453f, 0.7422f}, 0.05f, 1, 0x74767A, 0.1f, 10, 0.5f},  // small readout (197..228, 361..380)
    ScreenFrameFinish(),                                                         // stock screens (PLAN 57 j): frame
    ScreenGlassFinish(IMP_SCREEN_ART),                                           // glass
    ScreenBackingFinish(IMP_SCREEN_ART),                                         // behind the glass
    {{0, 0, 1, 1}, 1, 0, 0x7A1612, 0.40f, 26, 0},                                // extinguisher and hand-wheel red
};

const StockScreenLook IMP_SCREENS = {&IMP_SCREEN_ART, IMP_STOCK_FRAME, IMP_STOCK_GLASS, IMP_STOCK_BACKING, IMP_DARK};

void BuildImperialTank(Mesh& mesh)
{
    constexpr float DZ = 0.12f; // this cockpit sits 0.12 m nearer the eye than the Republic's
    constexpr float FLOOR = -0.60f, ROOF = 2.92f, LEFT = -1.08f, RIGHT = 2.26f, FRONT = -1.80f, BACK = 2.40f;
    constexpr float PLATFORM = 0.25f, WALL_END = 1.10f + DZ;
    // Floor: grating panels in black frames, a raised spine down the middle.
    for (int row = 0; row < 7; ++row)
    {
        const float z0 = FRONT + (BACK - FRONT) * row / 7.0f, z1 = FRONT + (BACK - FRONT) * (row + 1) / 7.0f;
        TiledQuad(mesh, {LEFT, FLOOR, z0}, {RIGHT - LEFT, 0, 0}, {0, 0, z1 - z0}, IMP_FINE);
        Box(mesh,
            MakeFrame({(LEFT + RIGHT) * 0.5f, FLOOR + 0.006f, z0}, {1, 0, 0}, {0, 1, 0}),
            {(RIGHT - LEFT) * 0.5f, 0.006f, 0.02f},
            0.003f,
            IMP_FRAME);
    }
    // Walls: angled kick panels low, large chamfered plates, a hex band high; the same behind the cockpit's left wall.
    for (int side = 0; side < 2; ++side)
    {
        const bool isRight = side == 0;
        const float x = isRight ? RIGHT : LEFT - 0.01f, z0 = isRight ? FRONT : WALL_END - 0.5f;
        const float inward = isRight ? -1.0f : 1.0f;
        // Kick panel leaning in at the bottom.
        PlanarQuad(
            mesh,
            {x, FLOOR + 0.45f, z0},
            {x, FLOOR + 0.45f, BACK},
            {x + inward * 0.18f, FLOOR, BACK},
            {x + inward * 0.18f, FLOOR, z0},
            IMP_VENT);
        TiledQuad(mesh, {x, FLOOR + 0.45f, z0}, {0, 0, BACK - z0}, {0, 2.0f - FLOOR - 0.45f, 0}, IMP_PLATE);
        TiledQuad(mesh, {x, 2.0f, z0}, {0, 0, BACK - z0}, {0, 0.5f, 0}, IMP_HEX);
        TiledQuad(mesh, {x, 2.5f, z0}, {0, 0, BACK - z0}, {0, ROOF - 2.5f, 0}, IMP_RAIL);
        // Chamfered wall plates between the frames.
        const float plates[4] = {-1.2f, -0.35f, 0.5f, 1.35f};
        for (const float zc : plates)
        {
            if (zc - 0.36f < z0 || zc + 0.36f > BACK)
            {
                continue;
            }
            const Frame plate =
                MakeFrame({x + inward * 0.002f, 1.2f, zc + 0.42f}, {0, 0, isRight ? -1.0f : 1.0f}, {0, 1, 0});
            Panel(mesh, plate, 0.32f, 0.62f, 0.05f, 0.025f, IMP_FRAME, IMP_PLATE, 0, IMP_PIPE);
        }
    }
    // Roof: a raised duct down the middle with red lamps; plates either side.
    TiledQuad(mesh, {LEFT, ROOF, FRONT}, {RIGHT - LEFT, 0, 0}, {0, 0, BACK - FRONT}, IMP_PLATE);
    // (It starts where the cockpit's sloping roof is high enough above it.)
    Box(mesh, MakeFrame({0.7f, ROOF - 0.08f, 1.3f}, {1, 0, 0}, {0, 1, 0}), {0.28f, 0.08f, 1.08f}, 0.04f, IMP_FRAME);
    for (int lamp = 0; lamp < 3; ++lamp)
    {
        const float z = 0.6f + 0.7f * lamp;
        for (int side = -1; side <= 1; side += 2)
        {
            Quad(
                mesh,
                {0.7f + side * 0.2f, ROOF - 0.162f, z - 0.12f},
                {0.7f + side * 0.12f, ROOF - 0.162f, z - 0.12f},
                {0.7f + side * 0.12f, ROOF - 0.162f, z + 0.12f},
                {0.7f + side * 0.2f, ROOF - 0.162f, z + 0.12f},
                IMP_RED_LAMP);
        }
    }
    // Rear wall: dark plating and a blast door split on the slant, a red bar above it.
    TiledQuad(mesh, {LEFT, FLOOR, BACK}, {RIGHT - LEFT, 0, 0}, {0, ROOF - FLOOR, 0}, IMP_MECH);
    const Frame door = MakeFrame({1.1f, FLOOR + 1.02f, BACK - 0.02f}, {-1, 0, 0}, {0, 1, 0});
    Panel(mesh, door, 0.66f, 1.02f, 0.1f, 0.06f, IMP_FRAME, IMP_DARK, 0, IMP_PIPE);
    for (int leaf = 0; leaf < 2; ++leaf)
    {
        // Two leaves meeting on a diagonal from top left to bottom right.
        const float s0 = leaf == 0 ? -1.0f : 1.0f;
        const Vec3 corners[4] = {
            door.At(s0 * 0.56f, 0.92f, 0.03f),
            door.At(leaf == 0 ? 0.12f : 0.18f, 0.92f, 0.03f),
            door.At(leaf == 0 ? -0.18f : -0.12f, -0.92f, 0.03f),
            door.At(s0 * 0.56f, -0.92f, 0.03f)};
        PlanarPolygon(mesh, corners, 4, door.y, IMP_PLATE);
    }
    Quad(
        mesh,
        door.At(-0.5f, 1.08f, 0.04f),
        door.At(0.5f, 1.08f, 0.04f),
        door.At(0.5f, 1.13f, 0.04f),
        door.At(-0.5f, 1.13f, 0.04f),
        IMP_RED_LAMP);
    // Angular frames round the cabin, closely spaced.
    const float ribs[5] = {-0.55f, 0.05f, 0.65f, 1.25f, 1.85f};
    for (const float z : ribs)
    {
        CabinRib(mesh, z, LEFT, RIGHT, FLOOR, ROOF, 0.4f, 0.09f, 0.1f, z > WALL_END, true, IMP_FRAME);
    }
    // A corner pillar to the cockpit's front.
    Box(mesh,
        MakeFrame({2.16f, (FLOOR + ROOF) * 0.5f, FRONT}, {1, 0, 0}, {0, 1, 0}),
        {0.11f, (ROOF - FLOOR) * 0.5f, 0.08f},
        0.03f,
        IMP_FRAME);
    // Driver's platform, winged seat, front console.
    const float platformFront = _eye.z - PLATFORM_AHEAD, platformBack = 2.5f;
    Box(mesh,
        MakeFrame({0, (FLOOR + PLATFORM) * 0.5f, (platformFront + platformBack) * 0.5f}, {1, 0, 0}, {0, 1, 0}),
        {0.58f, (PLATFORM - FLOOR) * 0.5f, (platformBack - platformFront) * 0.5f},
        0.05f,
        IMP_FRAME);
    Grille(
        mesh,
        MakeFrame({0, (FLOOR + PLATFORM) * 0.5f, platformFront - 0.01f}, {1, 0, 0}, {0, 1, 0}),
        0.5f,
        0.33f,
        0.04f,
        6,
        0.03f,
        IMP_FRAME,
        IMP_PLATE);
    const SeatLook seat = {0.23f, 0.66f, 0.12f, true, false, IMP_SEAT, IMP_FRAME, IMP_PIPE, IMP_SEAT, IMP_DARK};
    Seat(mesh, _eye, PLATFORM, seat);
    const ConsoleLook console =
        {IMP_FRAME, IMP_PLATE, IMP_PIPE, IMP_PIPE, IMP_RED_LAMP, IMP_DARK, IMP_SEAT, &IMP_SCREENS};
    FrontConsole(
        mesh,
        PLATFORM,
        console,
        {1.0f,
         IMP_PLATE,
         IMP_FRAME,
         IMP_FACE,
         IMP_PIPE,
         IMP_RED_LAMP,
         {{IMP_BUTTONS, -0.115f, 0.02f, 0.012f, 0.03f},
          {IMP_TOGGLES, -0.04f, -0.085f, 0.045f, 0.024f},
          {IMP_MINI, 0.06f, -0.085f, 0.026f, 0.016f}},
         {{IMP_KNOBS, 0.115f, 0.02f, 0.012f, 0.029f},
          {IMP_AMBER, -0.045f, -0.085f, 0.038f, 0.014f},
          {IMP_MINI, 0.05f, -0.085f, 0.026f, 0.016f}},
         {{IMP_LAMP_ROW, 0, 0.11f, 0.085f, 0.0194f}}});
    // Gunnery station on the right beside the driver (clear of the view ahead): a sloped console on a black plinth,
    // a readout and switch boards, a wall screen, a winged seat facing it.
    constexpr float STATION_Z = 1.45f;
    Box(mesh,
        MakeFrame({1.85f, (FLOOR + 0.55f) * 0.5f, STATION_Z}, {0, 0, -1}, {0, 1, 0}),
        {0.8f, (0.55f - FLOOR) * 0.5f, 0.3f},
        0.04f,
        IMP_FRAME);
    const Frame slope = MakeFrame({1.82f, 0.82f, STATION_Z}, {0, 0, -1}, {-0.7f, 1, 0});
    Box(mesh, Offset(slope, 0, 0, -0.04f), {0.8f, 0.3f, 0.04f}, 0.02f, IMP_FRAME);
    Panel(mesh, Offset(slope, -0.42f, 0.02f, 0.004f), 0.3f, 0.2f, 0.03f, 0.012f, IMP_FRAME, IMP_SCREEN, 0, IMP_PIPE);
    Panel(mesh, Offset(slope, 0.05f, 0.02f, 0.004f), 0.14f, 0.2f, 0.02f, 0.01f, IMP_FRAME, IMP_CONSOLE_A, 0, IMP_PIPE);
    Panel(mesh, Offset(slope, 0.45f, 0.02f, 0.004f), 0.24f, 0.2f, 0.02f, 0.01f, IMP_FRAME, IMP_CONSOLE_C, 0, IMP_PIPE);
    const Frame wallScreen = MakeFrame({RIGHT - 0.01f, 1.55f, STATION_Z}, {0, 0, -1}, {0, 1, 0});
    Panel(mesh, wallScreen, 0.42f, 0.22f, 0.04f, 0.02f, IMP_FRAME, IMP_SCREEN, 0, IMP_PIPE);
    {
        const int before = mesh.count;
        const SeatLook gunner = {0.2f, 0.5f, 0.1f, true, false, IMP_SEAT, IMP_FRAME, IMP_PIPE, IMP_SEAT, IMP_DARK};
        Seat(mesh, GAME_EYE, FLOOR + 0.6f, gunner);
        MoveSince(mesh, before, {0, -0.6f, 0});
        TurnSince(mesh, before, {0, 0, GAME_EYE.z}, -PI * 0.5f); // facing the right wall
        MoveSince(mesh, before, {1.25f, 0, STATION_Z - 1.5f});
    }
    // Wall racks behind the driver: two armoured cabinets with status strips.
    for (int k = 0; k < 2; ++k)
    {
        const Frame cabinet = MakeFrame({-0.85f + 0.42f * k, FLOOR + 0.75f, BACK - 0.16f}, {-1, 0, 0}, {0, 1, 0});
        Box(mesh, cabinet, {0.19f, 0.75f, 0.15f}, 0.03f, IMP_FRAME);
        Panel(mesh, Offset(cabinet, 0, 0.1f, 0.15f), 0.15f, 0.5f, 0.03f, 0.015f, IMP_FRAME, IMP_VENT, 0, IMP_PIPE);
        Quad(
            mesh,
            cabinet.At(-0.12f, 0.66f, 0.152f),
            cabinet.At(0.12f, 0.66f, 0.152f),
            cabinet.At(0.12f, 0.69f, 0.152f),
            cabinet.At(-0.12f, 0.69f, 0.152f),
            IMP_STATUS,
            0.05f + 0.3f * k,
            0.2f,
            0.3f + 0.3f * k,
            0.8f);
    }
    // Overhead conduits on the left, clamped to the frames.
    for (int k = 0; k < 4; ++k)
    {
        const Vec3 run[2] = {
            {-0.75f + 0.06f * k, ROOF - 0.06f, -1.0f},
            {-0.75f + 0.06f * k, ROOF - 0.06f, BACK - 0.02f}};
        Tube(mesh, run, 2, 0.022f, 6, k == 2 ? IMP_DARK : IMP_PIPE);
    }
    // Trim: a bolted moulding round the game's cockpit's whole open edge (the roof's rear edge, the front's right and
    // bottom edges, the left wall's foot and rear edge); skirting and cornices along the walls, posts in the rear
    // corners, deck seams and hatches in the open floor before the platform, the platform edged.
    const Vec3 rim[14] = {
        {-1.08f, 2.85f, 0.70f + DZ},
        {1.97f, 2.75f, -0.58f + DZ},
        {1.97f, 2.61f, -1.24f + DZ},
        {1.97f, 2.44f, -1.83f + DZ},
        {2.03f, 0.73f, -1.66f + DZ},
        {2.06f, 0.02f, -1.66f + DZ},
        {2.08f, -0.59f, -1.66f + DZ},
        {0.85f, -0.58f, -1.15f + DZ},
        {-0.98f, -0.56f, -0.38f + DZ},
        {-0.98f, -0.56f, 1.10f + DZ},
        {-1.00f, 0.01f, 1.10f + DZ},
        {-1.03f, 0.92f, 1.00f + DZ},
        {-1.03f, 1.01f, 0.99f + DZ},
        {-1.08f, 2.85f, 0.70f + DZ}};
    SeamTrim(mesh, rim, 14, 1, 0.028f, 6, 0.8f, IMP_FRAME, IMP_PIPE);
    for (int side = 0; side < 2; ++side)
    {
        const float x = side == 0 ? RIGHT : LEFT, inward = side == 0 ? -1.0f : 1.0f;
        const float z0 = side == 0 ? FRONT + 0.1f : WALL_END - 0.45f;
        Strip(mesh, {x, FLOOR, z0}, {x, FLOOR, BACK}, {0, 1, 0}, {inward, 0, 0}, 0.12f, 0.035f, IMP_FRAME);
        Strip(mesh, {x, ROOF - 0.08f, z0}, {x, ROOF - 0.08f, BACK}, {0, 1, 0}, {inward, 0, 0}, 0.16f, 0.05f, IMP_FRAME);
        Box(mesh,
            MakeFrame({x + inward * 0.06f, (FLOOR + ROOF) * 0.5f, BACK - 0.06f}, {1, 0, 0}, {0, 1, 0}),
            {0.06f, (ROOF - FLOOR) * 0.5f, 0.06f},
            0.02f,
            IMP_FRAME);
    }
    Strip(mesh, {LEFT, FLOOR, BACK}, {RIGHT, FLOOR, BACK}, {0, 1, 0}, {0, 0, -1}, 0.12f, 0.035f, IMP_FRAME);
    Strip(
        mesh,
        {LEFT, ROOF - 0.08f, BACK},
        {RIGHT, ROOF - 0.08f, BACK},
        {0, 1, 0},
        {0, 0, -1},
        0.16f,
        0.05f,
        IMP_FRAME);
    for (int seam = 0; seam < 3; ++seam)
    {
        const float z = -1.0f + 0.55f * seam + DZ;
        DeckSeam(mesh, {-0.9f, FLOOR, z}, {RIGHT - 0.1f, FLOOR, z}, 0.03f, IMP_DARK);
    }
    FloorHatch(mesh, {0.05f, FLOOR, 0.25f}, 0.32f, 0.2f, IMP_FRAME, IMP_VENT, IMP_PIPE);
    FloorHatch(mesh, {1.25f, FLOOR, -0.75f + DZ}, 0.28f, 0.28f, IMP_FRAME, IMP_PLATE, IMP_PIPE);
    const TankCabin cabin = {
        LEFT, RIGHT, FLOOR, ROOF, FRONT, BACK, WALL_END, 1.1f, 0.66f, {IMP_PLATE, IMP_FRAME, IMP_PIPE, IMP_DARK, IMP_LAMP, IMP_RED}, IMP_DARK, false, 0.055f};
    // A tread-plate walkway over the grating from the platform's side to the door; the gunnery console's switches and
    // dials modelled on its slope.
    TreadPlate(mesh, {0.72f, FLOOR + 0.012f, -0.4f}, {0.75f, 0, 0}, {0, 0, BACK - 0.05f + 0.4f}, 0.075f, IMP_PLATE, IMP_PIPE);
    ToggleBank(mesh, Offset(slope, 0.05f, 0.24f, 0.004f), 8, 0.035f, cabin.look);
    KnobRow(mesh, Offset(slope, 0.45f, 0.25f, 0.004f), 4, 0.05f, 0.013f, cabin.look);
    ControlPanel(mesh, MakeFrame({RIGHT - 0.004f, 2.3f, 0.35f}, {0, 0, 1}, {0, 1, 0}), 0.22f, 0.17f, cabin.look);
    FighterTankFittings(mesh, cabin);
}

// ---- MTT (cis_hover_mtt): the Trade Federation's troop transport ----
// The game's cockpit is a bridge 8 m wide: a front wall of olive lockers and control panels round a tall windscreen,
// a roof reaching back past the eye (2.15 to 2.62) with a glass skylight strip down its middle (x -0.85 to 1.27, to
// z 2.51), the left part wrapping round to a slanted edge. No floor: the front wall stops 3.6 to 4 m below the eye.
// The shell builds the bay under it: a main deck far below with droid storage cabinets along the walls, a rear
// bulkhead with the cargo door, roof pieces either side of the skylight (never over it: its glass is see-through),
// and the driver's command gantry at the usual height on pillars, with railings, a ladder, the seat and the consoles.
enum MttFinish
{
    MTT_LOCKERS,
    MTT_LOCKER_BAND,
    MTT_OLIVE,
    MTT_RAILS,
    MTT_SWITCHES,
    MTT_GRILLE,
    MTT_PORTHOLE,
    MTT_SIDE_PANEL,
    MTT_DARK,
    MTT_SEAT,
    MTT_LAMP,
    MTT_PIPE,
    MTT_FRAME,
    MTT_SCREEN,
    MTT_STATUS,
    MTT_FACE,
    MTT_LEDS,
    MTT_GAUGE,
    MTT_SWITCH_COLUMN,
    MTT_LED_PANEL,
    MTT_STOCK_FRAME,
    MTT_STOCK_GLASS,
    MTT_STOCK_BACKING,
    MTT_FINISH_COUNT
};

// Patches of cis_1st_cockpit_MTT (512 x 512; texel rectangles in the comments) and cis_1st_cockpit_fx (128 x 128).
const Finish MTT_FINISHES[MTT_FINISH_COUNT] = {
    {{0.453f, 0.570f, 0.998f, 0.992f}, 1.6f, 1, 0x80847A, 0.15f, 12, 0}, // locker doors (232..511, 292..508)
    {{0.453f, 0.449f, 0.998f, 0.566f}, 1.6f, 1, 0x80847A, 0.15f, 12, 0}, // ribbed band over them (232..511, 230..290)
    {{0.586f, 0.254f, 0.977f, 0.420f}, 1.0f, 1, 0x84867C, 0.15f, 12, 0}, // plain olive plate (300..500, 130..215)
    {{0.000f, 0.004f, 0.998f, 0.117f}, 2.2f, 1, 0x84867C, 0.25f, 18, 0}, // rail bands (0..511, 2..60)
    {{0.160f, 0.180f, 0.250f, 0.361f}, 0.18f, 1, 0x909088, 0.1f, 10, 0.2f}, // switch box (82..128, 92..185)
    {{0.270f, 0.195f, 0.381f, 0.322f}, 0.25f, 1, 0x84867C, 0.15f, 12, 0},   // mesh grille (138..195, 100..165)
    {{0.211f, 0.674f, 0.420f, 0.883f}, 0.55f, 1, 0x8A8C84, 0.3f, 20, 0},   // round gauge, porthole (108..215, 345..452)
    {{0.010f, 0.420f, 0.215f, 0.879f}, 0.5f, 1, 0x84867C, 0.15f, 12, 0},   // tall framed panel (5..110, 215..450)
    {{0, 0, 1, 1}, 1, 0, 0x14150F, 0.05f, 6, 0},                           // recesses
    {{0.586f, 0.254f, 0.977f, 0.420f}, 0.6f, 1, 0x4A4C40, 0.14f, 9, 0},    // seat pads (olive, dark)
    {{0, 0, 1, 1}, 1, 0, 0xFFB45A, 0, 1, 1.0f},                            // amber lamps
    {{0.600f, 0.300f, 0.700f, 0.340f}, 0.25f, 1, 0x9C9E94, 0.45f, 30, 0},  // pipes, rails, railings
    {{0.586f, 0.254f, 0.977f, 0.420f}, 1.0f, 1, 0x585A50, 0.2f, 14, 0},    // dark frames, gantry
    {{0.010f, 0.265f, 0.990f, 0.485f}, 0.30f, 2, 0xC0A060, 0, 1, 0.85f},   // readouts (fx scan bars)
    {{0.010f, 0.515f, 0.990f, 0.740f}, 0.20f, 2, 0xC0C0C0, 0, 1, 1.0f},    // status lights (fx)
    {{0, 0, 1, 1}, 1, 0, 0x1A140C, 0.70f, 40, 0.35f},                      // display glass (amber-black)
    {{0.270f, 0.186f, 0.527f, 0.342f}, 0.3f, 1, 0x909088, 0.1f, 10, 0.2f}, // grille and lamp block (138..270, 95..175)
    {{0.1953f, 0.6602f, 0.4395f, 0.8887f}, 0.3f, 1, 0x8A8C84, 0.3f, 20, 0.05f}, // round gauge with its lamps (100..225,
                                                                                // 338..455)
    {{0.1563f, 0.1719f, 0.2344f, 0.3672f}, 0.1f, 1, 0x8A8C84, 0.2f, 14, 0.15f}, // switch column (80..120, 88..188)
    {{0.3965f, 0.1914f, 0.5117f, 0.2969f}, 0.1f, 1, 0x8A8C84, 0.1f, 10, 0.4f},  // lamp panel (203..262, 98..152)
    ScreenFrameFinish(),                                                        // stock screens (PLAN 57 j): frame
    ScreenGlassFinish(CIS_SCREEN_ART),                                          // glass
    ScreenBackingFinish(CIS_SCREEN_ART),                                        // behind the glass
};

const StockScreenLook MTT_SCREENS = {&CIS_SCREEN_ART, MTT_STOCK_FRAME, MTT_STOCK_GLASS, MTT_STOCK_BACKING, MTT_DARK};

void BuildMtt(Mesh& mesh)
{
    constexpr float FLOOR = -2.55f, ROOF = 3.58f, LEFT = -3.72f, RIGHT = 4.48f, BACK = 2.80f;
    constexpr float DECK = 0.25f; // the command gantry's top
    // The roof is kept clear of every ray through the skylight: its glass edge sinks toward the front (y 2.7 at
    // x -0.85), so rays through it climb to the roof's height as far out as x -1.4. The skylight runs to the roof's
    // back edge; behind it a strip closes the gap, starting on that edge (pushed from the eye) and running down-back
    // to the rear wall's top, which seen from the eye lies beyond the edge: the strip covers exactly the directions
    // past the edge and none through the glass.
    constexpr float SKY_LEFT = -1.45f, SKY_RIGHT = 1.45f;
    // Main deck and the skirt from the front wall's bottom edge (tucked up behind it) down to it.
    TiledQuad(mesh, {LEFT, FLOOR, -3.5f}, {RIGHT - LEFT, 0, 0}, {0, 0, BACK + 3.5f}, MTT_OLIVE);
    const Vec3 bottom[6] = {
        {-3.61f, -2.22f, 0.20f},
        {-2.37f, -2.50f, -1.23f},
        {-0.16f, -2.24f, -2.14f},
        {0.83f, -2.12f, -2.55f},
        {2.15f, -1.96f, -3.10f},
        {3.88f, -2.12f, -2.52f}};
    Vec3 edge[6], skirtTop[48], skirtBottom[48];
    for (int i = 0; i < 6; ++i)
    {
        edge[i] = Pushed(bottom[i], 1.03f) + Vec3{0, 0.05f, 0};
    }
    const int edgeCount = Smooth(edge, 6, 4, skirtTop, 48);
    for (int i = 0; i < edgeCount; ++i)
    {
        skirtBottom[i] = {skirtTop[i].x, FLOOR, skirtTop[i].z};
    }
    Loft(mesh, skirtTop, skirtBottom, edgeCount, 1, MTT_LOCKER_BAND);
    // Side walls: lockers low, their ribbed band, plain plate, a rail band under the roof; pilasters between bays.
    for (int side = 0; side < 2; ++side)
    {
        const bool isLeft = side == 0;
        const float x = isLeft ? LEFT : RIGHT, z0 = isLeft ? -2.6f : -3.5f, length = BACK - z0;
        const float levels[5] = {FLOOR, FLOOR + 2.0f, FLOOR + 2.4f, ROOF - 0.32f, ROOF};
        const int finishes[4] = {MTT_LOCKERS, MTT_LOCKER_BAND, MTT_OLIVE, MTT_RAILS};
        for (int level = 0; level < 4; ++level)
        {
            TiledQuad(
                mesh,
                {x, levels[level], z0},
                {0, 0, length},
                {0, levels[level + 1] - levels[level], 0},
                finishes[level]);
        }
        const float inward = isLeft ? 1.0f : -1.0f;
        for (float z = z0 + 0.8f; z < BACK - 0.3f; z += 1.25f)
        {
            Box(mesh,
                MakeFrame({x + inward * 0.07f, (FLOOR + ROOF) * 0.5f, z}, {0, 0, 1}, {0, 1, 0}),
                {0.09f, (ROOF - FLOOR) * 0.5f, 0.07f},
                0.02f,
                MTT_FRAME);
            Box(mesh,
                MakeFrame({x + inward * 0.15f, FLOOR + 2.7f, z}, {0, 0, 1}, {0, 1, 0}),
                {0.05f, 0.015f, 0.05f},
                0.008f,
                MTT_LAMP);
        }
    }
    // Roof pieces either side of the skylight, and over its end; beams under them.
    TiledQuad(mesh, {LEFT, ROOF, -2.6f}, {SKY_LEFT - LEFT, 0, 0}, {0, 0, BACK + 2.6f}, MTT_OLIVE);
    TiledQuad(mesh, {SKY_RIGHT, ROOF, -3.5f}, {RIGHT - SKY_RIGHT, 0, 0}, {0, 0, BACK + 3.5f}, MTT_OLIVE);
    // The roof's back edge across the skylight (cis_1st_cockpit_MTT.msh: roof rim and the glass's back edge).
    const Vec3 roofEdge[6] = {
        {SKY_LEFT, 3.02f, 2.54f},
        {-1.02f, 3.16f, 2.48f},
        {-0.86f, 3.36f, 2.51f},
        {0.57f, 3.49f, 2.37f},
        {0.77f, 3.37f, 2.30f},
        {SKY_RIGHT, 3.45f, 2.23f}};
    Vec3 edgeLine[6], wallTop[6];
    for (int i = 0; i < 6; ++i)
    {
        edgeLine[i] = Pushed(roofEdge[i], 1.03f);
        wallTop[i] = {roofEdge[i].x, ROOF, BACK};
    }
    Loft(mesh, edgeLine, wallTop, 6, 1, MTT_RAILS);
    for (float z = -0.6f; z < BACK; z += 1.25f)
    {
        Box(mesh,
            MakeFrame({(LEFT + SKY_LEFT) * 0.5f, ROOF - 0.08f, z}, {1, 0, 0}, {0, 1, 0}),
            {(SKY_LEFT - LEFT) * 0.5f, 0.08f, 0.07f},
            0.02f,
            MTT_FRAME);
        Box(mesh,
            MakeFrame({(RIGHT + SKY_RIGHT) * 0.5f, ROOF - 0.08f, z}, {1, 0, 0}, {0, 1, 0}),
            {(RIGHT - SKY_RIGHT) * 0.5f, 0.08f, 0.07f},
            0.02f,
            MTT_FRAME);
    }
    // Rear bulkhead: the cargo door with a porthole, framed panels either side.
    TiledQuad(mesh, {LEFT, FLOOR, BACK}, {RIGHT - LEFT, 0, 0}, {0, 2.4f, 0}, MTT_LOCKERS);
    TiledQuad(mesh, {LEFT, FLOOR + 2.4f, BACK}, {RIGHT - LEFT, 0, 0}, {0, ROOF - FLOOR - 2.4f, 0}, MTT_OLIVE);
    const Frame door = MakeFrame({0.4f, FLOOR + 1.5f, BACK - 0.01f}, {-1, 0, 0}, {0, 1, 0});
    Panel(mesh, door, 1.25f, 1.5f, 0.14f, 0.07f, MTT_FRAME, MTT_LOCKER_BAND, 0.3f, MTT_PIPE);
    Panel(mesh, Offset(door, 0, 0.55f, 0.012f), 0.32f, 0.32f, 0.04f, 0.02f, MTT_FRAME, MTT_PORTHOLE, 0, MTT_PIPE);
    for (int k = -1; k <= 1; k += 2)
    {
        Panel(
            mesh,
            MakeFrame({0.4f + k * 2.2f, FLOOR + 1.6f, BACK - 0.01f}, {-1, 0, 0}, {0, 1, 0}),
            0.38f,
            1.0f,
            0.05f,
            0.025f,
            MTT_FRAME,
            MTT_SIDE_PANEL,
            0.25f,
            MTT_PIPE);
    }
    // Droid storage cabinets along both walls of the bay.
    for (int side = 0; side < 2; ++side)
    {
        const float x = side == 0 ? LEFT + 0.35f : RIGHT - 0.35f, face = side == 0 ? 1.0f : -1.0f;
        for (int k = 0; k < 3; ++k)
        {
            const float z = (side == 0 ? -0.6f : -2.0f) + 1.25f * k + 0.62f;
            const Frame cabinet = MakeFrame({x, FLOOR + 0.95f, z}, {0, 0, face}, {0, 1, 0});
            Box(mesh, cabinet, {0.5f, 0.95f, 0.3f}, 0.03f, MTT_FRAME);
            Panel(
                mesh,
                Offset(cabinet, 0, 0.05f, 0.3f),
                0.44f,
                0.82f,
                0.04f,
                0.02f,
                MTT_FRAME,
                MTT_LOCKERS,
                0,
                MTT_PIPE);
            Panel(
                mesh,
                Offset(cabinet, 0.3f, 0.75f, 0.32f),
                0.06f,
                0.1f,
                0.01f,
                0.006f,
                MTT_FRAME,
                MTT_SWITCHES,
                0,
                MTT_PIPE);
        }
    }
    // Crates on the deck.
    const float crates[4][4] =
        {{2.2f, 1.0f, 0.45f, 0.3f}, {2.9f, 1.6f, 0.4f, -0.2f}, {-2.3f, 1.9f, 0.5f, 0.15f}, {2.4f, -1.2f, 0.35f, 0.5f}};
    for (const auto& crate : crates)
    {
        const float yaw = crate[3];
        const Frame f = MakeFrame({crate[0], FLOOR + crate[2], crate[1]}, {cosf(yaw), 0, sinf(yaw)}, {0, 1, 0});
        Box(mesh, f, {crate[2], crate[2], crate[2] * 0.8f}, 0.03f, MTT_LOCKER_BAND);
    }
    // Command gantry: a deck on four pillars with trusses under it, railings on three sides, a ladder at the back.
    const float GX = 1.3f, GZ0 = _eye.z - PLATFORM_AHEAD, GZ1 = 2.55f;
    Box(mesh,
        MakeFrame({0, DECK - 0.06f, (GZ0 + GZ1) * 0.5f}, {1, 0, 0}, {0, 1, 0}),
        {GX, 0.06f, (GZ1 - GZ0) * 0.5f},
        0.02f,
        MTT_FRAME);
    Grille(
        mesh,
        MakeFrame({0, DECK + 0.001f, (GZ0 + GZ1) * 0.5f}, {1, 0, 0}, {0, 0, -1}),
        GX - 0.05f,
        (GZ1 - GZ0) * 0.5f - 0.05f,
        0.05f,
        10,
        0.01f,
        MTT_FRAME,
        MTT_GRILLE);
    for (int corner = 0; corner < 4; ++corner)
    {
        const float x = (corner & 1) ? GX - 0.1f : -GX + 0.1f, z = (corner & 2) ? GZ1 - 0.1f : GZ0 + 0.1f;
        Box(mesh,
            MakeFrame({x, (FLOOR + DECK - 0.12f) * 0.5f, z}, {1, 0, 0}, {0, 1, 0}),
            {0.08f, (DECK - 0.12f - FLOOR) * 0.5f, 0.08f},
            0.02f,
            MTT_FRAME);
    }
    for (int k = 0; k < 2; ++k)
    {
        const float z = k == 0 ? GZ0 + 0.1f : GZ1 - 0.1f;
        const Vec3 brace[2] = {{-GX + 0.1f, FLOOR + 0.3f, z}, {GX - 0.1f, DECK - 0.2f, z}};
        Tube(mesh, brace, 2, 0.035f, 8, MTT_PIPE);
        const Vec3 brace2[2] = {{GX - 0.1f, FLOOR + 0.3f, z}, {-GX + 0.1f, DECK - 0.2f, z}};
        Tube(mesh, brace2, 2, 0.035f, 8, MTT_PIPE);
    }
    // Railings: posts and two rails, low across the front so the view to the front panels stays clear.
    const Vec3 railCorners[4] = {
        {-GX + 0.04f, DECK, GZ1 - 0.05f},
        {-GX + 0.04f, DECK, GZ0 + 0.04f},
        {GX - 0.04f, DECK, GZ0 + 0.04f},
        {GX - 0.04f, DECK, GZ1 - 0.05f}};
    for (int run = 0; run < 3; ++run)
    {
        const float height = run == 1 ? 0.6f : 0.95f;
        for (int rail = 1; rail <= 2; ++rail)
        {
            const float y = height * rail / 2.0f;
            const Vec3 path[2] = {railCorners[run] + Vec3{0, y, 0}, railCorners[run + 1] + Vec3{0, y, 0}};
            Tube(mesh, path, 2, 0.02f, 8, MTT_PIPE, false);
        }
        const Vec3 a = railCorners[run], b = railCorners[run + 1];
        const int posts = static_cast<int>(Length(b - a) / 0.6f) + 1;
        for (int post = 0; post <= posts; ++post)
        {
            const Vec3 at = Lerp(a, b, static_cast<float>(post) / posts);
            const Vec3 path[2] = {at, at + Vec3{0, height, 0}};
            Tube(mesh, path, 2, 0.018f, 5, MTT_PIPE);
        }
    }
    for (int rung = 0; rung < 10; ++rung)
    {
        const float y = FLOOR + 0.3f + (DECK - FLOOR - 0.3f) * rung / 9.0f;
        const Vec3 path[2] = {{-0.25f, y, GZ1 + 0.06f}, {0.25f, y, GZ1 + 0.06f}};
        Tube(mesh, path, 2, 0.015f, 6, MTT_PIPE);
    }
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 rail[2] = {{side * 0.27f, FLOOR, GZ1 + 0.06f}, {side * 0.27f, DECK + 0.9f, GZ1 + 0.06f}};
        Tube(mesh, rail, 2, 0.022f, 8, MTT_FRAME);
    }
    // On the gantry: seat, front console, a side console with readouts and switches.
    const SeatLook seat = {0.22f, 0.62f, 0.15f, false, true, MTT_SEAT, MTT_FRAME, MTT_PIPE, MTT_LOCKER_BAND, MTT_DARK};
    Seat(mesh, _eye, DECK, seat);
    const ConsoleLook console = {MTT_FRAME, MTT_OLIVE, MTT_PIPE, MTT_PIPE, MTT_LAMP, MTT_DARK, MTT_SEAT, &MTT_SCREENS};
    FrontConsole(
        mesh,
        DECK,
        console,
        {1.2f,
         MTT_OLIVE,
         MTT_FRAME,
         MTT_FACE,
         MTT_PIPE,
         MTT_LAMP,
         {{MTT_SWITCH_COLUMN, -0.1125f, 0.015f, 0.02f, 0.05f},
          {MTT_GRILLE, -0.03f, -0.085f, 0.021f, 0.025f},
          {MTT_LED_PANEL, 0.05f, -0.085f, 0.027f, 0.025f}},
         {{MTT_SWITCH_COLUMN, 0.1125f, 0.015f, 0.02f, 0.05f},
          {MTT_LED_PANEL, -0.05f, -0.085f, 0.027f, 0.025f},
          {MTT_GRILLE, 0.03f, -0.085f, 0.021f, 0.025f}},
         {},
         MTT_GAUGE,
         0.275f,
         1.068f,
         0.02f,
         -0.052f,
         0.672f});
    const Frame side = MakeFrame({0.9f, DECK + 0.45f, 1.45f}, {0, 0, -1}, {0, 1, 0});
    Box(mesh, side, {0.45f, 0.45f, 0.18f}, 0.03f, MTT_FRAME);
    const Frame top = MakeFrame({0.9f, DECK + 0.93f, 1.45f}, {0, 0, -1}, {-0.6f, 1, 0});
    Panel(mesh, Offset(top, -0.18f, 0, 0), 0.2f, 0.13f, 0.02f, 0.01f, MTT_FRAME, MTT_SCREEN, 0, MTT_PIPE);
    Panel(mesh, Offset(top, 0.2f, 0, 0), 0.12f, 0.13f, 0.02f, 0.01f, MTT_FRAME, MTT_SWITCHES, 0, MTT_PIPE);
    // Conduits from the gantry down into the bay.
    for (int k = 0; k < 3; ++k)
    {
        const Vec3 run[4] = {
            {-1.15f + 0.08f * k, DECK - 0.1f, 2.4f},
            {-1.15f + 0.08f * k, -0.8f, 2.6f},
            {-1.6f - 0.1f * k, -1.8f, 2.7f},
            {-1.8f - 0.1f * k, FLOOR, 2.72f}};
        Vec3 path[20];
        const int n = Smooth(run, 4, 5, path, 20);
        Tube(mesh, path, n, 0.03f, 8, k == 1 ? MTT_DARK : MTT_PIPE);
    }
    // Trim: a bolted moulding over the seams round the game's bridge (its front wall's foot, the right side, the
    // roof's rear edge either side of the skylight, the left side); skirting along the bay's walls and bulkhead, deck
    // seams, the gantry's deck edged with kick plates.
    const Vec3 rimRight[11] = {
        {-0.16f, -2.24f, -2.14f},
        {0.83f, -2.12f, -2.55f},
        {2.15f, -1.96f, -3.10f},
        {3.88f, -2.12f, -2.52f},
        {3.90f, -0.33f, -2.86f},
        {3.91f, 0.16f, -2.96f},
        {4.00f, 1.74f, -2.50f},
        {4.09f, 2.71f, -1.30f},
        {4.34f, 3.34f, 2.32f},
        {2.15f, 3.53f, 2.15f},
        {0.77f, 3.37f, 2.30f}};
    const Vec3 rimLeft[8] = {
        {-1.02f, 3.16f, 2.48f},
        {-2.11f, 2.80f, 2.62f},
        {-1.90f, 2.14f, -0.89f},
        {-2.71f, 1.40f, -0.62f},
        {-3.12f, 0.66f, -0.35f},
        {-3.61f, -2.22f, 0.20f},
        {-2.37f, -2.50f, -1.23f},
        {-0.16f, -2.24f, -2.14f}};
    SeamTrim(mesh, rimRight, 11, 1, 0.035f, 5, 1.0f, MTT_FRAME, MTT_PIPE);
    SeamTrim(mesh, rimLeft, 8, 1, 0.035f, 5, 1.0f, MTT_FRAME, MTT_PIPE);
    for (int side = 0; side < 2; ++side)
    {
        const float x = side == 0 ? LEFT : RIGHT, inward = side == 0 ? 1.0f : -1.0f;
        Strip(mesh, {x, FLOOR, -2.4f}, {x, FLOOR, BACK}, {0, 1, 0}, {inward, 0, 0}, 0.16f, 0.05f, MTT_FRAME);
    }
    Strip(mesh, {LEFT, FLOOR, BACK}, {RIGHT, FLOOR, BACK}, {0, 1, 0}, {0, 0, -1}, 0.16f, 0.05f, MTT_FRAME);
    for (int seam = 0; seam < 4; ++seam)
    {
        const float z = -1.6f + 1.1f * seam;
        DeckSeam(mesh, {LEFT + 0.2f, FLOOR, z}, {RIGHT - 0.2f, FLOOR, z}, 0.05f, MTT_DARK);
    }
    Strip(mesh, {-GX, DECK, GZ0}, {GX, DECK, GZ0}, {0, 1, 0}, {0, 0, -1}, 0.1f, 0.02f, MTT_RAILS);
    for (int side = -1; side <= 1; side += 2)
    {
        Strip(
            mesh,
            {side * GX, DECK, GZ0},
            {side * GX, DECK, GZ1},
            {0, 1, 0},
            {side * 1.0f, 0, 0},
            0.1f,
            0.02f,
            MTT_RAILS);
    }
}

// ---- Combat speeder (all_hover_combatspeeder): the Alliance's tank ----
// The hull the game shows outside is all_hover_rebel_attack_speeder.msh (the ODF's GeometryName; the mod tools'
// all_hover_combatspeeder.msh is an older design the game does not load): a low tan hull with maroon bands, a long
// canopy (glass sides under an opaque roof) over the pilot and, behind him on the middle line, the gunner, who aims the
// nose turret by camera (his seat has no cockpit model). The first-person model is wider than that hull: a two-seat
// cabin, the pilot on the left of a dashboard centred 0.58 m to his right (SPD_CENTER), a windshield before him, an
// open frame before the right seat, the left hull deck and the nose; open above, to the right and behind. The shell
// keeps that width and builds the rest after the exterior: the long canopy (glass sides in the planes of the
// first-person frame, the passenger's windshield, a roof with an overhead console, frame ribs), the cabin (floor,
// walls, the pilot's and passenger's bucket seats bearing the exterior's own seat picture, a centre console, the
// gunner's raised seat behind them with his sight and yoke, a padded rear bulkhead) and, seen through the glass, the
// right hull deck (the game's left one mirrored), both decks carried back to the engine nacelles, and the gun pods.
enum SpeederFinish
{
    SPD_PLATE,
    SPD_FLOOR,
    SPD_HEADLINER,
    SPD_FRAME,
    SPD_HULL,
    SPD_STRIPE,
    SPD_SEAT,
    SPD_PLEATS,
    SPD_LEATHER,
    SPD_CONSOLE,
    SPD_DISPLAYS,
    SPD_VENT,
    SPD_MACHINE,
    SPD_METAL,
    SPD_TREAD,
    SPD_DARK,
    SPD_GLASS,
    SPD_STATUS,
    SPD_FACE,
    SPD_SWITCHES,
    SPD_READOUT,
    SPD_KNOBS,
    SPD_BUTTONS,
    SPD_DISPLAY,
    SPD_STOP_LAMP,
    SPD_LAMP_OFF,
    SPD_RETICLE,
    SPD_SCREEN_COLUMN,
    SPD_SCREEN_TOP,
    SPD_SCREEN_BOTTOM,
    SPD_SCREEN_GLASS,
    SPD_PICTURE,
    SPD_STOCK_FRAME,
    SPD_STOCK_GLASS,
    SPD_STOCK_BACKING,
    SPD_LAMP,
    SPD_RED,
    SPD_CABLE,
    SPD_STOCK,
    SPD_STOCK_FX,
    SPD_STOCK_UNDER,
    SPD_FINISH_COUNT
};

// Patches of all_1st_cockpit_combatspeeder (512 x 512; texel rectangles in the comments),
// all_hover_rebel_attack_speeder (the exterior, 512 x 512) and all_1st_cockpit_fx. Pictures (seat, console, displays,
// vent) go whole onto their faces (Picture); everything else is mapped by its real size.
const Finish SPD_FINISHES[SPD_FINISH_COUNT] = {
    {{0.215f, 0.512f, 0.430f, 0.645f}, 0.7f, 1, 0x86887E, 0.18f, 14, 0}, // plate, as the left deck (110..220, 262..330)
    {{0.215f, 0.512f, 0.430f, 0.645f}, 0.7f, 1, 0x62645C, 0.12f, 10, 0}, // floor: the plate, darker
    {{0.215f, 0.512f, 0.430f, 0.645f}, 0.7f, 1, 0x74766E, 0.10f, 10, 0}, // headliner
    {{0.215f, 0.512f, 0.430f, 0.645f}, 0.5f, 1, 0x4E5049, 0.30f, 20, 0}, // frames, ribs, seat shells
    {{0.820f, 0.293f, 0.986f, 0.645f}, 0.5f, 3, 0x7A776E, 0.15f, 12, 0}, // exterior tan (420..505, 150..330)
    {{0.428f, 0.962f, 0.760f, 0.986f}, 1.1f, 3, 0x808080, 0.20f, 14, 0}, // maroon band (219..389, 493..505)
    {{0.559f, 0.396f, 0.678f, 0.635f}, 0.4f, 3, 0x72767A, 0.20f, 12, 0}, // seat picture (286..347, 203..325)
    {{0.443f, 0.455f, 0.555f, 0.592f}, 0.35f, 3, 0x72767A, 0.20f, 12, 0},       // pleated leather (227..284, 233..303)
    {{0.682f, 0.449f, 0.781f, 0.617f}, 0.3f, 3, 0x72767A, 0.20f, 12, 0},        // plain leather (349..400, 230..316)
    {{0.760f, 0.840f, 0.885f, 0.996f}, 0.3f, 3, 0x6E7276, 0.15f, 12, 0.1f},     // button console (389..453, 430..510)
    {{0.674f, 0.367f, 0.775f, 0.441f}, 0.2f, 3, 0xA0A0A0, 0.10f, 10, 0.6f},     // two displays (345..397, 188..226)
    {{0.428f, 0.639f, 0.615f, 0.760f}, 0.4f, 3, 0x808080, 0.20f, 14, 0},        // oval vent (219..315, 327..389)
    {{0.666f, 0.121f, 0.793f, 0.293f}, 0.35f, 3, 0x808080, 0.20f, 14, 0},       // machinery, cables (341..406, 62..150)
    {{0.492f, 0.061f, 0.789f, 0.119f}, 0.6f, 3, 0x909090, 0.50f, 32, 0},        // metal (a pipe, 252..404, 31..61)
    {{0.482f, 0.230f, 0.721f, 0.322f}, 0.5f, 3, 0x808080, 0.30f, 18, 0},        // slotted plate (247..369, 118..165)
    {{0, 0, 1, 1}, 1, 0, 0x18171A, 0.05f, 6, 0},                                // recesses
    {{0, 0, 1, 1}, 1, 4, 0x808080, 0.60f, 40, 0, 0.99f},                        // glass: the window texture, its alpha
    {{0.000f, 0.500f, 1.000f, 0.750f}, 0.15f, 2, 0xC0C0C0, 0, 1, 1.0f},         // status lights (fx)
    {{0, 0, 1, 1}, 1, 0, 0x0A140E, 0.70f, 40, 0.35f},                           // display glass (green-black)
    {{0.609f, 0.445f, 0.719f, 0.512f}, 0.2f, 1, 0x8A8A86, 0.15f, 12, 0.1f},     // switch block (312..368, 228..262)
    {{0.596f, 0.553f, 0.742f, 0.742f}, 0.2f, 1, 0xA0A0A0, 0, 1, 0.7f},          // green display (305..380, 283..380)
    {{0.6250f, 0.3867f, 0.7090f, 0.4297f}, 0.1f, 1, 0x8A8A86, 0.3f, 18, 0.05f}, // lamp and knob (320..363, 198..220)
    {{0.6113f, 0.7852f, 0.7227f, 0.9180f}, 0.1f, 1, 0x8A8A86, 0.15f, 12, 0.1f}, // button block (313..370, 402..470)
    {{0.5898f, 0.5410f, 0.7461f, 0.7461f}, 0.2f, 1, 0x8A8A86, 0.2f, 14, 0.25f}, // framed display (302..382, 277..382)
    {{0, 0, 1, 1}, 1, 0, 0xFF2418, 0, 1, 1.0f},                                 // stop lamp, lit
    {{0, 0, 1, 1}, 1, 0, 0x1C1816, 0.40f, 24, 0},                               // stop lamp, dark
    {{0, 0, 1, 1}, 1, 0, 0x50FF70, 0, 1, 1.0f},                                 // sight reticle (self-lit green)
    // The periscope's display: all_1st_cockpit_securitycam (512 x 512, the left half of the stock screen, mirrored
    // about its right edge; texel rectangles in the comments), its glass with its own alpha (the reticle), and the
    // scope pass's picture (its square's middle band, the glass's 2.5 : 1).
    {{0.031f, 0.094f, 0.318f, 0.939f}, 1, 5, 0x808080, 0.25f, 16, 0},          // bezel column (16..163, 48..481)
    {{0.318f, 0.094f, 1.000f, 0.240f}, 1, 5, 0x808080, 0.25f, 16, 0},          // bezel top bar (163..512, 48..123)
    {{0.318f, 0.785f, 1.000f, 0.939f}, 1, 5, 0x808080, 0.25f, 16, 0},          // bezel bottom bar (163..512, 402..481)
    {{0.318f, 0.240f, 1.000f, 0.785f}, 1, 5, 0x808080, 0.5f, 40, 0.4f, 0.99f}, // glass (163..512, 123..402)
    {{0, 0.3f, 1, 0.7f}, 1, 6, 0xFFFFFF, 0, 1, 1.0f},                          // periscope picture (self-lit)
    ScreenFrameFinish(),                                                       // stock screens (PLAN 57 j): frame
    ScreenGlassFinish(ALL_SCREEN_ART),                                         // glass
    ScreenBackingFinish(ALL_SCREEN_ART),                                       // behind the glass
    {{0, 0, 1, 1}, 1, 0, 0xFFB050, 0, 1, 1.0f},                                // small lamps, pointers (amber)
    {{0, 0, 1, 1}, 1, 0, 0x8C1E18, 0.45f, 30, 0},                              // extinguisher red (the hull's maroon)
    {{0, 0, 1, 1}, 1, 0, 0x26272A, 0.25f, 14, 0},                              // cables, rubber
    {{0, 0, 1, 1}, 1, 1, 0x808080, 0, 1, 0},       // the game's left half, mirrored (SpeederStockMirror): lit by
                                                   // the map's own light as the game's half (CockpitShell)
    {{0, 0, 1, 1}, 1, 2, 0xC0C0C0, 0, 1, 1.0f},    // its lights (effects texture)
    {{0, 0, 1, 1}, 1, 1, 0x808080, 0, 1, 0},       // its faces turned down (the same since the map's light, b333)
};

const StockScreenLook SPD_SCREENS = {&ALL_SCREEN_ART, SPD_STOCK_FRAME, SPD_STOCK_GLASS, SPD_STOCK_BACKING, SPD_DARK};

/// The cabin's middle line (the dashboard's) and the mirror across it.
constexpr float SPD_CENTER = 0.58f;
/// The gunner's eye (GunnerSeat's cabin seat; his seat turns about the vertical through it) and the top of his raised
/// platform.
constexpr Vec3 SPD_GUNNER_EYE = {SPD_CENTER, 1.82f, 2.93f};
constexpr float SPD_PLATFORM_Y = 0.42f;

float SpeederMirror(float x)
{
    return 2 * SPD_CENTER - x;
}

// The canopy, the same all along the cabin: its sides in the planes of the first-person model's frame (the left side
// window's posts run from the rim at x -0.66 up to the roof rail at -0.21, the passenger's windshield post from 1.82
// up to 1.58), the roof between the rails, crowned. The cabin's walls below the rim, the floor, the rear bulkhead.
constexpr float SPD_RIM_Y = 1.16f, SPD_ROOF_Y = 2.085f, SPD_CROWN = 0.085f;
constexpr float SPD_LEFT_RIM = -0.675f, SPD_LEFT_TOP = -0.218f, SPD_RIGHT_RIM = 1.835f;
// The right side's top: the left's mirror (until b320 1.585, the game's panel edge: the right side stood steeper).
constexpr float SPD_RIGHT_TOP = 2 * 0.58f + 0.218f;
constexpr float SPD_LEFT_WALL = -0.74f, SPD_RIGHT_WALL = 1.88f, SPD_FLOOR_Y = 0.10f, SPD_BULKHEAD = 3.9f;
constexpr int SPD_ROOF_STEPS = 8;
// The side windows (both sides alike): the post line from z FRONT at the rim to TOP_FRONT at the rail (y TOP), the
// rear edge upright at BACK.
constexpr float SPD_WINDOW_FRONT = 0.90f, SPD_WINDOW_TOP_FRONT = 1.16f, SPD_WINDOW_BACK = 1.62f, SPD_WINDOW_TOP = 1.79f;

// x of the left (side -1) or right canopy side at height y.
float CanopySide(int side, float y)
{
    const float t = (y - SPD_RIM_Y) / (SPD_ROOF_Y - SPD_RIM_Y);
    return side < 0 ? SPD_LEFT_RIM + (SPD_LEFT_TOP - SPD_LEFT_RIM) * t
                    : SPD_RIGHT_RIM + (SPD_RIGHT_TOP - SPD_RIGHT_RIM) * t;
}

// The roof's height at x: a shallow arch between the rails.
float RoofY(float x)
{
    const float t = (x - SPD_LEFT_TOP) / (SPD_RIGHT_TOP - SPD_LEFT_TOP) * 2 - 1;
    return SPD_ROOF_Y + SPD_CROWN * (1 - t * t);
}

// The canopy's section at depth z, `inset` inside its skin, from the left rim over the roof to the right rim.
// Returns the count (SPD_ROOF_STEPS + 3).
int CanopySection(float z, float inset, Vec3* out)
{
    int n = 0;
    out[n++] = {CanopySide(-1, SPD_RIM_Y) + inset, SPD_RIM_Y, z};
    for (int k = 0; k <= SPD_ROOF_STEPS; ++k)
    {
        const float x = SPD_LEFT_TOP + (SPD_RIGHT_TOP - SPD_LEFT_TOP) * k / SPD_ROOF_STEPS;
        const float shift = k == 0 ? inset : k == SPD_ROOF_STEPS ? -inset : 0.0f;
        out[n++] = {x + shift, RoofY(x) - inset, z};
    }
    out[n++] = {CanopySide(1, SPD_RIM_Y) - inset, SPD_RIM_Y, z};
    return n;
}

// A point on the left (side -1) or right canopy side at height y and depth z, `out` beyond its skin (negative: inside).
Vec3 OnCanopySide(int side, float y, float z, float out)
{
    return {CanopySide(side, y) + side * out, y, z};
}

/// A bucket seat as on the exterior's texture: leather cushion, a backrest to the shoulders and a headrest above it on
/// two posts (so a look over the shoulder passes it), rolled bolsters, the seat picture on their faces (its top for
/// the headrest, its middle for the backrest, the rest for the cushion), a dark shell with a pleated pad behind, a
/// pedestal to the floor. headY: the headrest's middle (about the sitter's eye).
void SpeederSeat(Mesh& mesh, float x, float floorY, float top, float front, float back, float tilt, float headY)
{
    constexpr float W = 0.17f;
    const float midZ = (front + back) * 0.5f, halfDepth = (back - front) * 0.5f;
    RoundedBar(mesh, MakeFrame({x, top - 0.055f, midZ}, {1, 0, 0}, {0, 1, 0}), W, 0.055f, halfDepth, 0.04f, SPD_LEATHER);
    Quad(
        mesh,
        {x + W - 0.03f, top + 0.001f, back - 0.03f},
        {x - W + 0.03f, top + 0.001f, back - 0.03f},
        {x - W + 0.03f, top + 0.001f, front + 0.03f},
        {x + W - 0.03f, top + 0.001f, front + 0.03f},
        SPD_SEAT,
        0.06f,
        0.70f,
        0.94f,
        0.98f);
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 roll[2] = {
            {x + side * (W - 0.01f), top - 0.02f, back - 0.03f},
            {x + side * (W - 0.01f), top - 0.02f, front + 0.04f}};
        Tube(mesh, roll, 2, 0.035f, 10, SPD_LEATHER);
    }
    const float c = cosf(tilt), s = sinf(tilt), bh = 0.31f;
    const Frame rest = MakeFrame({x, top + bh * c, back + 0.05f + bh * s}, {1, 0, 0}, {0, c, s});
    RoundedBar(mesh, rest, W + 0.01f, bh, 0.05f, 0.045f, SPD_LEATHER);
    Quad(
        mesh,
        rest.At(W - 0.01f, bh - 0.01f, -0.051f),
        rest.At(-W + 0.01f, bh - 0.01f, -0.051f),
        rest.At(-W + 0.01f, -bh + 0.03f, -0.051f),
        rest.At(W - 0.01f, -bh + 0.03f, -0.051f),
        SPD_SEAT,
        0.03f,
        0.24f,
        0.97f,
        0.68f);
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 bolster[2] = {
            rest.At(side * (W + 0.005f), -bh + 0.06f, -0.035f),
            rest.At(side * (W + 0.005f), bh * 0.55f, -0.035f)};
        Tube(mesh, bolster, 2, 0.04f, 10, SPD_LEATHER);
    }
    Box(mesh, Offset(rest, 0, 0.01f, 0.065f), {W + 0.03f, bh + 0.02f, 0.02f}, 0.012f, SPD_FRAME);
    Panel(
        mesh,
        Offset(rest, 0, 0.0f, 0.086f),
        W - 0.01f,
        bh - 0.08f,
        0.02f,
        0.01f,
        SPD_FRAME,
        SPD_PLEATS,
        0,
        SPD_METAL);
    const float headLocal = (headY - top) / c - bh; // along the backrest's line
    const Frame head = Offset(rest, 0, headLocal, -0.005f);
    RoundedBar(mesh, head, 0.11f, 0.09f, 0.045f, 0.042f, SPD_LEATHER);
    Quad(
        mesh,
        head.At(0.1f, 0.08f, -0.046f),
        head.At(-0.1f, 0.08f, -0.046f),
        head.At(-0.1f, -0.08f, -0.046f),
        head.At(0.1f, -0.08f, -0.046f),
        SPD_SEAT,
        0.2f,
        0.01f,
        0.8f,
        0.22f);
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 post[2] = {rest.At(side * 0.065f, bh - 0.02f, 0.01f), head.At(side * 0.065f, -0.07f, 0.01f)};
        Tube(mesh, post, 2, 0.011f, 8, SPD_METAL);
    }
    // Pan under the cushion, pedestal, two floor rails.
    Box(mesh,
        MakeFrame({x, top - 0.125f, midZ}, {1, 0, 0}, {0, 1, 0}),
        {W + 0.01f, 0.015f, halfDepth + 0.01f},
        0.008f,
        SPD_FRAME);
    const float pedestalTop = top - 0.14f;
    Box(mesh,
        MakeFrame({x, (floorY + pedestalTop) * 0.5f, midZ + 0.03f}, {1, 0, 0}, {0, 1, 0}),
        {0.11f, (pedestalTop - floorY) * 0.5f, halfDepth - 0.06f},
        0.015f,
        SPD_FRAME);
    for (int side = -1; side <= 1; side += 2)
    {
        Box(mesh,
            MakeFrame({x + side * 0.13f, floorY + 0.015f, midZ}, {1, 0, 0}, {0, 1, 0}),
            {0.02f, 0.015f, halfDepth + 0.1f},
            0.005f,
            SPD_METAL);
    }
}

// One hull deck station (the right deck's x; the left deck is its mirror): the rim beside the cabin, the ledge where
// the deck steps down, the outer edge, and their heights. Measured on the game's left deck (z -1.36, -0.09, 1.32) and
// on the nose's right edge (z -2.58, -1.36), carried back after the exterior to its engine nacelles.
struct DeckStation
{
    float z, rim, ledge, outer, rimY, ledgeY, outerY;
};

const DeckStation SPD_DECK[] = {
    {-2.58f, 1.58f, 2.43f, 2.88f, 0.95f, 0.83f, 0.71f},
    {-1.36f, 1.82f, 2.52f, 3.23f, 1.09f, 0.92f, 0.71f},
    {-0.09f, 1.91f, 2.58f, 3.40f, 1.06f, 0.94f, 0.71f},
    {1.32f, 1.95f, 2.67f, 3.52f, 1.09f, 0.95f, 0.71f},
    {2.30f, 1.96f, 2.69f, 3.54f, 1.09f, 0.95f, 0.71f},
    {4.00f, 2.00f, 2.66f, 3.46f, 1.10f, 0.94f, 0.70f},
    {6.20f, 1.94f, 2.56f, 3.26f, 1.08f, 0.92f, 0.68f},
    {7.60f, 1.56f, 2.01f, 2.51f, 1.00f, 0.88f, 0.66f},
};

// The deck across one station, side -1 left (mirrored) or +1 right: rim, its lip, the maroon band's inner edge, the
// ledge's top and foot, the outer edge, the belly below it.
constexpr int SPD_DECK_POINTS = 7;

void DeckProfile(const DeckStation& station, int side, Vec3* out)
{
    const float lip = station.rim + 0.03f;
    const float band = lip + 0.12f;
    const float xs[SPD_DECK_POINTS] =
        {station.rim, lip, band, station.ledge, station.ledge, station.outer, station.outer - 0.12f};
    const float bandY =
        station.rimY - 0.04f + (station.ledgeY - station.rimY + 0.04f) * (band - lip) / (station.ledge - lip);
    const float ys[SPD_DECK_POINTS] =
        {station.rimY, station.rimY - 0.04f, bandY, station.ledgeY, station.ledgeY - 0.08f, station.outerY, 0.30f};
    for (int i = 0; i < SPD_DECK_POINTS; ++i)
    {
        out[i] = {side < 0 ? SpeederMirror(xs[i]) : xs[i], ys[i], station.z};
    }
}

/// The gunner's turning seat (PLAN 57 i, idren2023 2026-10-18: option 1, a turning seat with end stops; 2026-10-19: a
/// periscope showing the view from the gun's muzzle in the stock gunner screen): on the raised platform a fixed ring;
/// inside it a turntable (part 1) carrying the seat, the yoke's column and two curved rails, arcs about the gunner's
/// eye beside the periscope's display, held by a post from the yoke. The display (part 2) rides the rails: the stock
/// security camera screen's bezel and glass (its reticle) in a dark case, a slider on each rail, a dark lamp at each
/// lower corner; part 5 the scope pass's picture behind the glass (SPEEDER_PERISCOPE_*). Part 1 turns about the
/// vertical through the eye (SPD_GUNNER_EYE) by the turret's yaw relative to the hull, part 2 also about the horizontal
/// through it by the head's pitch within its limits (GunnerSeat, PLAN 58 d). Parts 3 and 4, drawn only at the right / left end stop (+-80 deg, the
/// turret's ODF YawLimits) with part 2's matrix: that side's lamp lit.
void SpeederGunnerSeat(Mesh& mesh)
{
    constexpr float DISC = 0.46f, RING_INNER = 0.465f, RING_OUTER = 0.50f, RING_TOP = 0.025f;
    // The rails reach past the display's pitch limits by its own half height and the strip under it.
    constexpr float RAIL_LOW_DEGREES = ShellDesigns::SPEEDER_PERISCOPE_LOW_DEGREES - 21.0f;
    constexpr float RAIL_HIGH_DEGREES = ShellDesigns::SPEEDER_PERISCOPE_HIGH_DEGREES + 8.0f;
    const Vec3 pivot = {SPD_GUNNER_EYE.x, SPD_PLATFORM_Y, SPD_GUNNER_EYE.z};
    const Vec3 up = {0, 1, 0};
    const float rail = ShellDesigns::SPEEDER_PERISCOPE_DISTANCE_METERS;
    const float halfWidth = ShellDesigns::SPEEDER_PERISCOPE_HALF_WIDTH_METERS;
    const float halfHeight = ShellDesigns::SPEEDER_PERISCOPE_HALF_HEIGHT_METERS;
    // The stock bezel in the texture's proportions: the column 147 texels beside the glass's 349 (its half), the bars
    // 75 and 79 over and under its 279.
    const float bezelHalf = halfWidth * (1.0f + 147.0f / 349.0f);
    const float barTop = halfHeight * 2 * 75.0f / 279.0f, barBottom = halfHeight * 2 * 79.0f / 279.0f;
    const float railX = bezelHalf + 0.03f; // the rails' sideways offset from the middle line
    mesh.part = 0;
    const float ring[3][2] = {{0, RING_OUTER}, {RING_TOP, RING_OUTER}, {RING_TOP, RING_INNER}};
    Lathe(mesh, MakeFrame(pivot, {1, 0, 0}, {0, 0, 1}), ring, 3, 32, SPD_FRAME);
    mesh.part = 1;
    const float disc[3][2] = {{0, DISC}, {0.02f, DISC}, {0.02f, 0}};
    Lathe(mesh, MakeFrame(pivot, {1, 0, 0}, {0, 0, 1}), disc, 3, 32, SPD_FRAME);
    const float deck = SPD_PLATFORM_Y + 0.02f;
    SpeederSeat(mesh, SPD_CENTER, deck, 1.02f, 2.65f, 3.12f, 0.21f, 1.84f);
    // The controls on the seat's armrests and the periscope's rails on struts (idren2023 2026-10-19).
    ArmrestsAndFrame(
        mesh,
        SPD_GUNNER_EYE,
        0.19f,
        deck,
        railX,
        rail,
        RAIL_LOW_DEGREES,
        RAIL_HIGH_DEGREES,
        0.42f,
        {SPD_FRAME, SPD_METAL, SPD_DARK, SPD_FRAME});
    // The display housing at the gun's pitch 0 (z toward the eye): the stock screen's bezel and glass, each half
    // mapped toward the middle as the stock frame mirrors them (as the AAT's window), a dark case behind, an arm and a
    // slider to each rail, a dark lamp at each lower corner.
    mesh.part = 2;
    const Vec3 screen = {SPD_GUNNER_EYE.x, SPD_GUNNER_EYE.y, SPD_GUNNER_EYE.z - rail};
    const Frame face = MakeFrame(screen, {1, 0, 0}, up);
    const auto at = [&](float x, float y, float z)
    {
        return screen + Vec3{x, y, z};
    };
    constexpr float BEZEL_Z = 0.003f, PICTURE_Z = -0.004f;
    const float bezelTop = halfHeight + barTop, bezelBottom = -halfHeight - barBottom;
    for (int side = -1; side <= 1; side += 2)
    {
        const float outer = side * bezelHalf, inner = side * halfWidth;
        const float sLeft = side < 0 ? 0.0f : 1.0f, sRight = side < 0 ? 1.0f : 0.0f;
        const float left = side < 0 ? outer : inner, right = side < 0 ? inner : outer;
        Quad(
            mesh,
            at(left, bezelTop, BEZEL_Z),
            at(right, bezelTop, BEZEL_Z),
            at(right, bezelBottom, BEZEL_Z),
            at(left, bezelBottom, BEZEL_Z),
            SPD_SCREEN_COLUMN,
            sLeft,
            0,
            sRight,
            1);
        const float barLeft = side < 0 ? inner : 0.0f, barRight = side < 0 ? 0.0f : inner;
        Quad(
            mesh,
            at(barLeft, bezelTop, BEZEL_Z),
            at(barRight, bezelTop, BEZEL_Z),
            at(barRight, halfHeight, BEZEL_Z),
            at(barLeft, halfHeight, BEZEL_Z),
            SPD_SCREEN_TOP,
            sLeft,
            0,
            sRight,
            1);
        Quad(
            mesh,
            at(barLeft, -halfHeight, BEZEL_Z),
            at(barRight, -halfHeight, BEZEL_Z),
            at(barRight, bezelBottom, BEZEL_Z),
            at(barLeft, bezelBottom, BEZEL_Z),
            SPD_SCREEN_BOTTOM,
            sLeft,
            0,
            sRight,
            1);
        Quad(
            mesh,
            at(barLeft, halfHeight, 0),
            at(barRight, halfHeight, 0),
            at(barRight, -halfHeight, 0),
            at(barLeft, -halfHeight, 0),
            SPD_SCREEN_GLASS,
            sLeft,
            0,
            sRight,
            1);
        const Vec3 arm[2] = {at(side * (bezelHalf - 0.01f), 0, -0.012f), at(side * railX, 0, 0)};
        Tube(mesh, arm, 2, 0.006f, 6, SPD_FRAME);
        Box(mesh, Offset(face, side * railX, 0, 0), {0.03f, 0.04f, 0.024f}, 0.005f, SPD_FRAME); // round the box rail
        Rivet(mesh, Offset(face, side * (bezelHalf - 0.03f), bezelBottom + 0.012f, BEZEL_Z), 0.006f, SPD_LAMP_OFF);
    }
    Box(mesh,
        Offset(face, 0, (bezelTop + bezelBottom) * 0.5f, -0.024f),
        {bezelHalf + 0.004f, (bezelTop - bezelBottom) * 0.5f + 0.004f, 0.016f},
        0.004f,
        SPD_DARK);
    // The glass's inner returns back to the picture (dark).
    for (int side = -1; side <= 1; side += 2)
    {
        PlanarQuad(
            mesh,
            at(side * halfWidth, halfHeight, BEZEL_Z),
            at(side * halfWidth, -halfHeight, BEZEL_Z),
            at(side * halfWidth, -halfHeight, PICTURE_Z),
            at(side * halfWidth, halfHeight, PICTURE_Z),
            SPD_DARK);
    }
    // The HUD (PLAN 57 j, idren2023 2026-10-19): the target's health in the picture's top right corner, and under the
    // display a long thin stock screen with the player's health, the gun's heat (in a gunner seat the HUD's
    // ammunition group) and the speeder's health in line. The map and the score stay on the head's panel.
    SetDisplayScreen(&SPD_SCREENS);
    AddDisplay(GROUP_TARGET, face, halfWidth - 0.05f, halfHeight - 0.012f, 0.08f, 0.016f, 2);
    {
        constexpr float STRIP_HALF_WIDTH = 0.15f, STRIP_HALF_HEIGHT = 0.018f, STRIP_TEXEL_METERS = 0.00008f;
        const float stripY = bezelBottom - 0.006f -
                             (ALL_SCREEN_ART.glassTop - ALL_SCREEN_ART.outerTop) * STRIP_TEXEL_METERS -
                             STRIP_HALF_HEIGHT;
        const Frame strip = Offset(face, 0, stripY, BEZEL_Z);
        StockScreen(mesh, strip, STRIP_HALF_WIDTH, STRIP_HALF_HEIGHT, STRIP_TEXEL_METERS, SPD_SCREENS, 0.015f);
        AddDisplayRegion(GROUP_HEALTH, PLAYER_HEALTH, strip, -0.1f, 0, 0.09f, 0.03f, 2);
        AddDisplay(GROUP_AMMUNITION, strip, 0, 0, 0.09f, 0.03f, 2);
        AddDisplayRegion(GROUP_HEALTH, VEHICLE_HEALTH, strip, 0.1f, 0, 0.09f, 0.03f, 2);
    }
    SetDisplayScreen(nullptr);
    // The lit lamps: part 3 the right stop's, part 4 the left's (over the dark ones).
    for (int side = 1; side >= -1; side -= 2)
    {
        mesh.part = side > 0 ? 3 : 4;
        Rivet(
            mesh,
            Offset(face, side * (bezelHalf - 0.03f), bezelBottom + 0.012f, BEZEL_Z + 0.002f),
            0.0075f,
            SPD_STOP_LAMP);
    }
    // Part 5: the periscope's picture behind the glass.
    mesh.part = 5;
    Quad(
        mesh,
        at(-halfWidth, halfHeight, PICTURE_Z),
        at(halfWidth, halfHeight, PICTURE_Z),
        at(halfWidth, -halfHeight, PICTURE_Z),
        at(-halfWidth, -halfHeight, PICTURE_Z),
        SPD_PICTURE);
    mesh.part = 0;
}

/// One vertex of a piece of the game's own models (StockTables: built at game start from the player's level files).
using StockTables::StockVertex;

/// A stock table's vertices and vertex count by its name (StockTables::Get); none when the game files lacked it.
const StockVertex* StockVertices(const char* name)
{
    return StockTables::Get(name).vertices;
}

int StockCount(const char* name)
{
    return StockTables::Get(name).count;
}

// The speeder's passenger half as the game's pilot half (idren2023 2026-10-19: framed and laid out as the left, with its
// displays): the game's window, canopy, dashboard, floorboard and lamps left of the middle line, mirrored, with their
// own texture coordinates; the game's right half hidden by the depth mask (5 mm before its faces).
void SpeederStockMirror(Mesh& mesh)
{
    const StockTables::Table mirror = StockTables::Get("SPEEDER_MIRROR");
    for (int i = 0; i + 2 < mirror.count; i += 3)
    {
        if (mesh.count + 3 > mesh.capacity)
        {
            mesh.isFull = true;
            break;
        }
        for (int k = 0; k < 3; ++k)
        {
            const StockVertex& stock = mirror.vertices[i + k];
            Vertex& vertex = mesh.vertices[mesh.count++];
            vertex.position = stock.position;
            vertex.normal = stock.normal;
            vertex.u = stock.u;
            vertex.v = stock.v;
            const bool isUnder = mirror.vertices[i].normal.y + mirror.vertices[i + 1].normal.y < -0.6f;
            vertex.finish = static_cast<uint8_t>(
                stock.texture == 4   ? SPD_GLASS
                : stock.texture == 2 ? SPD_STOCK_FX
                : isUnder            ? SPD_STOCK_UNDER
                                     : SPD_STOCK);
            vertex.part = mesh.part;
        }
    }
    int maskCount = 0;
    const Vec3* mask = StockTables::SpeederMask(maskCount);
    AddMask(mask, maskCount);
}

// The speeder's fittings (idren2023 2026-10-19: about 50 000 triangles, polished, after the exterior: a Rebel utility
// craft, cream and maroon outside, its cabin padded and practical): padded wall quilting and a control panel below
// the rims, cable looms under the sills and pipes along the floor, grab handles over the side windows and the gunner,
// dome lights and a cable tray under the roof, tread plates on the floor, the gunner's platform and the passenger's
// footwell, a step up to the platform, a locker and an extinguisher by the bulkhead.
void SpeederDetails(Mesh& mesh)
{
    constexpr float FLOOR = SPD_FLOOR_Y, BULKHEAD = SPD_BULKHEAD, LEFT = SPD_LEFT_WALL, RIGHT = SPD_RIGHT_WALL;
    const DetailLook look = {SPD_PLATE, SPD_FRAME, SPD_METAL, SPD_DARK, SPD_LAMP, SPD_RED};
    const Vec3 up = {0, 1, 0};
    const Frame leftWall = MakeFrame({LEFT + 0.002f, 0, 0}, {0, 0, -1}, up);  // z: +x, into the cabin
    const Frame rightWall = MakeFrame({RIGHT - 0.002f, 0, 0}, {0, 0, 1}, up); // z: -x
    const auto onWall = [](const Frame& wall, float z, float y)
    {
        Frame at = wall;
        at.origin = {wall.origin.x, y, z};
        return at;
    };
    // Walls below the rims, the same both sides: quilting behind the front seats, a control panel beside the gunner's
    // step, cable looms under the sills, a pipe along the floor, a locker by the bulkhead (no extinguisher: idren2023
    // 2026-10-19).
    for (const Frame* wall : {&leftWall, &rightWall})
    {
        const int side = wall == &leftWall ? -1 : 1;
        QuiltedPanel(mesh, onWall(*wall, 1.77f, 0.80f), 0.34f, 0.24f, 3, 5, 0.028f, 0.008f, SPD_LEATHER, SPD_DARK);
        ControlPanel(mesh, onWall(*wall, 2.31f, 0.80f), 0.18f, 0.13f, look);
        Locker(mesh, onWall(*wall, 3.58f, 0.62f), 0.16f, 0.28f, 0.16f, look);
        const float x = side < 0 ? LEFT + 0.028f : RIGHT - 0.028f, from = 1.40f;
        const Vec3 loom[3] = {{x, 1.10f, from}, {x, 1.10f, (from + BULKHEAD) * 0.5f}, {x, 1.10f, BULKHEAD - 0.06f}};
        CableLoom(mesh, loom, 3, 3, 0.009f, up, SPD_CABLE, SPD_FRAME, 0.32f);
        const Vec3 pipe[2] = {{x + side * 0.004f, 0.24f, from + 0.05f}, {x + side * 0.004f, 0.24f, BULKHEAD - 0.08f}};
        FlangedPipe(mesh, pipe, 2, 0.02f, {static_cast<float>(-side), 0, 0}, 0.55f, SPD_METAL, SPD_FRAME);
    }
    // Grab handles on the canopy's sides over the windows and over the gunner, standing off the side's slope.
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 low = OnCanopySide(side, SPD_RIM_Y, 0, 0), high = OnCanopySide(side, SPD_ROOF_Y, 0, 0);
        const Vec3 slope = Normalized(high - low);
        Vec3 inward = Cross(slope, {0, 0, 1});
        if (inward.x * side > 0)
        {
            inward = -inward;
        }
        for (const float z : {1.24f, 2.62f})
        {
            GrabHandle(
                mesh,
                OnCanopySide(side, 1.94f, z, -0.006f),
                OnCanopySide(side, 1.94f, z + 0.30f, -0.006f),
                inward,
                0.05f,
                0.011f,
                SPD_METAL,
                SPD_FRAME);
        }
    }
    // Roof: two dome lights and a cable tray from the overhead console back to the bulkhead.
    for (int light = 0; light < 4; ++light)
    {
        const float x = SPD_CENTER + (light % 2 ? 0.32f : -0.32f), z = light < 2 ? 1.80f : 2.60f;
        const Vec3 at = {x, RoofY(x) - 0.004f, z};
        const Frame down = {at, {1, 0, 0}, {0, -1, 0}, {0, 0, -1}};
        const float bezel[4][2] = {{0, 0.06f}, {0.008f, 0.06f}, {0.014f, 0.052f}, {0.014f, 0.046f}};
        Lathe(mesh, down, bezel, 4, 20, SPD_FRAME);
        const float lens[4][2] = {{0.010f, 0.046f}, {0.018f, 0.04f}, {0.024f, 0.025f}, {0.026f, 0.0f}};
        Lathe(mesh, down, lens, 4, 20, SPD_LAMP);
    }
    const Vec3 tray[2] = {{SPD_CENTER, RoofY(SPD_CENTER) - 0.025f, 1.62f}, {SPD_CENTER, RoofY(SPD_CENTER) - 0.025f, BULKHEAD - 0.05f}};
    CableLoom(mesh, tray, 2, 4, 0.008f, {1, 0, 0}, SPD_CABLE, SPD_FRAME, 0.28f);
    // Padding: quilted strips along the roof either side of the lights, between the ribs, and on the canopy's sides
    // behind the side windows, under the grab handles.
    for (int side = -1; side <= 1; side += 2)
    {
        for (const float from : {2.01f, 2.99f})
        {
            const float to = from < 2.5f ? 2.91f : BULKHEAD - 0.08f;
            for (int column = 0; column < 2; ++column)
            {
                const float x = SPD_CENTER + side * (0.52f + 0.17f * column);
                const float slope = (RoofY(x + 0.01f) - RoofY(x - 0.01f)) / 0.02f;
                const Frame roof = MakeFrame({x, RoofY(x) - 0.003f, (from + to) * 0.5f}, {1, slope, 0}, {0, 0, 1});
                QuiltedPanel(mesh, roof, 0.08f, (to - from) * 0.5f, 4, 1, 0.022f, 0.008f, SPD_LEATHER, SPD_DARK);
            }
            const Vec3 low = OnCanopySide(side, 1.24f, (from + to) * 0.5f, -0.004f);
            const Vec3 high = OnCanopySide(side, 1.80f, (from + to) * 0.5f, -0.004f);
            const Frame wall = MakeFrame((low + high) * 0.5f, {0, 0, static_cast<float>(side)}, high - low);
            QuiltedPanel(mesh, wall, (to - from) * 0.5f, Length(high - low) * 0.5f, 3, 4, 0.028f, 0.008f, SPD_LEATHER, SPD_DARK);
        }
    }
    // Floors: tread plates either side of the hatch behind the seats, in the passenger's footwell and on the gunner's
    // platform, a step up to it.
    TreadPlate(mesh, {-0.62f, FLOOR, 1.40f}, {0.90f, 0, 0}, {0, 0, 0.96f}, 0.08f, SPD_TREAD, SPD_METAL);
    TreadPlate(mesh, {0.88f, FLOOR, 1.40f}, {0.90f, 0, 0}, {0, 0, 0.96f}, 0.08f, SPD_TREAD, SPD_METAL);
    TreadPlate(mesh, {SPD_CENTER - 0.46f, SPD_PLATFORM_Y, 2.44f}, {0.92f, 0, 0}, {0, 0, 1.40f}, 0.08f, SPD_TREAD, SPD_METAL);
    Box(mesh, MakeFrame({SPD_CENTER, (FLOOR + 0.26f) * 0.5f, 2.32f}, {1, 0, 0}, up), {0.36f, (0.26f - FLOOR) * 0.5f, 0.08f}, 0.012f, SPD_FRAME);
    TreadPlate(mesh, {SPD_CENTER - 0.34f, 0.26f, 2.25f}, {0.68f, 0, 0}, {0, 0, 0.14f}, 0.07f, SPD_TREAD, SPD_METAL);
}

void BuildCombatSpeeder(Mesh& mesh)
{
    constexpr float RIM = SPD_RIM_Y, FLOOR = SPD_FLOOR_Y, BULKHEAD = SPD_BULKHEAD;
    constexpr float LEFT = SPD_LEFT_WALL, RIGHT = SPD_RIGHT_WALL;
    // Glass where the exterior has it, in the cockpit's own window texture (once over each pane, its edges grimy as
    // on the game's windshield), a centimetre beyond the frames: a side window each side in the first-person frame's
    // side opening (idren2023 2026-10-17: framed, smaller): from the post line behind the windshield (z 0.90 at the rim,
    // 1.16 at the rail, the game's left post) back to 1.62, from the sill up to the rail at 1.79 (SPD_WINDOW_*), the
    // passenger's windshield in the open frame before the right seat; a rear window comes with the bulkhead. The
    // canopy's sides are closed elsewhere: above the rail, before the right window back to the post line (on the left
    // the game's windshield and post reach that far), behind both windows.
    for (int side = -1; side <= 1; side += 2)
    {
        Quad(
            mesh,
            OnCanopySide(side, SPD_WINDOW_TOP + 0.03f, SPD_WINDOW_TOP_FRONT - 0.03f, 0.01f),
            OnCanopySide(side, SPD_WINDOW_TOP + 0.03f, SPD_WINDOW_BACK + 0.03f, 0.01f),
            OnCanopySide(side, RIM - 0.02f, SPD_WINDOW_BACK + 0.03f, 0.01f),
            OnCanopySide(side, RIM - 0.02f, SPD_WINDOW_FRONT - 0.04f, 0.01f),
            SPD_GLASS);
        const Vec3 above[4] = {
            OnCanopySide(side, SPD_WINDOW_TOP, SPD_WINDOW_TOP_FRONT - 0.02f, -0.005f),
            OnCanopySide(side, SPD_WINDOW_TOP, SPD_WINDOW_BACK, -0.005f),
            OnCanopySide(side, SPD_ROOF_Y, 1.95f, -0.005f),
            OnCanopySide(side, SPD_ROOF_Y, 1.12f, -0.005f)};
        PlanarPolygon(mesh, above, 4, {0, 0, 1}, SPD_PLATE);
        const Vec3 behind[5] = {
            OnCanopySide(side, RIM, SPD_WINDOW_BACK, -0.005f),
            OnCanopySide(side, RIM, BULKHEAD, -0.005f),
            OnCanopySide(side, SPD_ROOF_Y, BULKHEAD, -0.005f),
            OnCanopySide(side, SPD_ROOF_Y, 1.95f, -0.005f),
            OnCanopySide(side, SPD_WINDOW_TOP, SPD_WINDOW_BACK, -0.005f)};
        PlanarPolygon(mesh, behind, 5, {0, 0, 1}, SPD_PLATE);
    }
    // The passenger's front, the mirror of the game's own pilot front about the cabin's middle line (idren2023
    // 2026-10-19: the exterior's windscreen wraps round both halves of the canopy, the right window framed as the
    // left, the left dashboard's displays mirrored to the right; the game's first-person model closes the passenger's
    // half with a flat louvred panel, its dashboard and right side asymmetric): SpeederStockMirror (after the cabin)
    // puts the game's own left half mirrored there, with its textures, and hides the game's right half by depth.
    // Between the two windscreens one divider as wide as the exterior's (canopy_Glass of
    // all_hover_rebel_attack_speeder: no glass between x -0.20 and 0.20 at its top, -0.27 and 0.27 at its foot): a
    // framed member up the windscreens' slope on the middle line, its cabin face a raised border round a louvred inset,
    // bolts at its corners.
    {
        // The game's panel surface there (V-shaped about x 1.09; its left half's plane carried on beyond its edge).
        const auto panelAt = [](float x, float y)
        {
            const float t = (y - 1.16f) / 0.63f;
            const float left = 0.44f + 0.12f * t;
            const float edgeZ = -0.68f + 0.86f * t, middleZ = -0.76f + 0.88f * t;
            return Vec3{x, y, edgeZ + (middleZ - edgeZ) * (x - left) / (1.09f - left)};
        };
        constexpr float FOOT_HALF = 0.27f, HEAD_HALF = 0.20f, FOOT_Y = 1.13f, HEAD_Y = 1.86f, DEPTH = 0.035f;
        const Vec3 inward = Normalized({0, -0.81f, 0.59f});
        // Mirror-symmetric about the middle line: the left corners' z used on both sides (the panel slopes in x).
        const auto corner = [&](float side, float half, float y)
        {
            const Vec3 left = panelAt(SPD_CENTER - half, y);
            return Vec3{SPD_CENTER + side * half, y, left.z} + inward * 0.004f;
        };
        const Vec3 back[4] = {
            corner(-1, FOOT_HALF, FOOT_Y),
            corner(1, FOOT_HALF, FOOT_Y),
            corner(1, HEAD_HALF, HEAD_Y),
            corner(-1, HEAD_HALF, HEAD_Y)};
        Vec3 front[4];
        for (int k = 0; k < 4; ++k)
        {
            front[k] = back[k] + inward * DEPTH;
        }
        PlanarPolygon(mesh, front, 4, {1, 0, 0}, SPD_FRAME);
        for (int k = 0; k < 4; ++k)
        {
            PlanarQuad(mesh, back[k], back[(k + 1) % 4], front[(k + 1) % 4], front[k], SPD_FRAME);
        }
        // The raised border: a bevelled moulding round the face's edge; inside it the louvred inset.
        Vec3 loop[5];
        const Vec3 middle = (front[0] + front[1] + front[2] + front[3]) * 0.25f;
        for (int k = 0; k < 5; ++k)
        {
            loop[k] = front[k % 4] + Normalized(middle - front[k % 4]) * 0.022f;
        }
        const float moulding[6][2] = {
            {-0.016f, 0.0f}, {0.016f, 0.0f}, {0.016f, 0.008f}, {0.010f, 0.014f}, {-0.010f, 0.014f}, {-0.016f, 0.008f}};
        Sweep(mesh, loop, 5, moulding, 6, true, false, inward, SPD_FRAME, false);
        const Vec3 up = Normalized(front[3] - front[0] + (front[2] - front[1]));
        const Frame face = Frame{middle, {1, 0, 0}, up, inward};
        const float faceHalfHeight = Length((front[3] + front[2]) * 0.5f - (front[0] + front[1]) * 0.5f) * 0.5f;
        Grille(mesh, face, HEAD_HALF - 0.06f, faceHalfHeight - 0.07f, 0.014f, 14, 0.010f, SPD_FRAME, SPD_DARK);
        for (int k = 0; k < 4; ++k)
        {
            const Vec3 at = front[k] + Normalized(middle - front[k]) * 0.045f;
            Rivet(mesh, Frame{at, {1, 0, 0}, up, inward}, 0.011f, SPD_METAL);
        }
    }
    // Roof from behind the game's front roof band to the bulkhead, an overhead console over the middle.
    Vec3 roofFront[SPD_ROOF_STEPS + 1], roofBack[SPD_ROOF_STEPS + 1];
    for (int k = 0; k <= SPD_ROOF_STEPS; ++k)
    {
        const float x = SPD_LEFT_TOP + (SPD_RIGHT_TOP - SPD_LEFT_TOP) * k / SPD_ROOF_STEPS;
        roofFront[k] = {x, RoofY(x), 0.40f};
        roofBack[k] = {x, RoofY(x), BULKHEAD};
    }
    Loft(mesh, roofFront, roofBack, SPD_ROOF_STEPS + 1, 6, SPD_HEADLINER);
    const float consoleY = RoofY(SPD_CENTER) - 0.035f;
    Box(mesh,
        MakeFrame({SPD_CENTER, consoleY, 1.15f}, {1, 0, 0}, {0, 1, 0}),
        {0.17f, 0.035f, 0.42f},
        0.015f,
        SPD_FRAME);
    for (int k = 0; k < 2; ++k)
    {
        const Frame face = MakeFrame({SPD_CENTER, consoleY - 0.036f, 0.95f + 0.4f * k}, {1, 0, 0}, {0, 0, 1});
        Picture(mesh, face, 0.13f, 0.165f, SPD_CONSOLE);
    }
    for (int k = 0; k < 3; ++k)
    {
        Box(mesh,
            MakeFrame({SPD_CENTER - 0.06f + 0.06f * k, consoleY - 0.04f, 1.53f}, {1, 0, 0}, {0, 1, 0}),
            {0.012f, 0.006f, 0.012f},
            0.003f,
            k == 1 ? SPD_STATUS : SPD_DARK);
    }
    // Frame ribs: the right half of the game's arch behind the seats (leaning back as its left half does) and across
    // the roof to it, one over the gunner, one at the bulkhead; rails where the glass meets the roof.
    const float rib[4][2] = {{-0.022f, -0.025f}, {0.022f, -0.025f}, {0.022f, 0.025f}, {-0.022f, 0.025f}};
    Vec3 section[SPD_ROOF_STEPS + 6];
    for (int r = 0; r < 3; ++r)
    {
        const float z = r == 0 ? 1.97f : r == 1 ? 2.95f : BULKHEAD - 0.03f;
        int count = CanopySection(z, 0.025f, section);
        if (r == 0)
        {
            // From the game's left rail over the roof and down the right side to the window's rear corner.
            section[count - 1] = {CanopySide(1, 1.85f) - 0.025f, 1.85f, 1.70f};
            section[count] = {CanopySide(1, SPD_WINDOW_TOP) - 0.025f, SPD_WINDOW_TOP, SPD_WINDOW_BACK};
            count += 1;
            Sweep(mesh, section + 1, count - 1, rib, 4, true, false, {0, 0, 1}, SPD_FRAME, true);
        }
        else
        {
            Sweep(mesh, section, count, rib, 4, true, false, {0, 0, 1}, SPD_FRAME, true);
        }
    }
    // A frame round each side window (idren2023 2026-10-17: the panes had none): front post, rail, rear post and sill as
    // one bevelled member 2 cm inside the glass, a dark seal along its inner edge, a bolt at each corner; on the left
    // the arch from the window's rear corner up to the roof, as the right side's rib.
    const float frameProfile[6][2] =
        {{-0.026f, -0.03f}, {0.026f, -0.03f}, {0.026f, 0.012f}, {0.016f, 0.024f}, {-0.016f, 0.024f}, {-0.026f, 0.012f}};
    for (int side = -1; side <= 1; side += 2)
    {
        const float corners[5][2] = {
            {RIM, SPD_WINDOW_FRONT},
            {SPD_WINDOW_TOP, SPD_WINDOW_TOP_FRONT},
            {SPD_WINDOW_TOP, SPD_WINDOW_BACK},
            {RIM, SPD_WINDOW_BACK},
            {RIM, SPD_WINDOW_FRONT}};
        Vec3 loop[5], seal[5];
        for (int k = 0; k < 5; ++k)
        {
            loop[k] = OnCanopySide(side, corners[k][0], corners[k][1], -0.03f);
        }
        const Vec3 middle = (loop[0] + loop[1] + loop[2] + loop[3]) * 0.25f;
        for (int k = 0; k < 5; ++k)
        {
            seal[k] = loop[k] + Normalized(middle - loop[k]) * 0.03f + Vec3{side * 0.012f, 0, 0};
        }
        Sweep(mesh, loop, 5, frameProfile, 6, true, false, {static_cast<float>(-side), 0, 0}, SPD_FRAME, false);
        Tube(mesh, seal, 5, 0.006f, 4, SPD_DARK, false);
        for (int k = 0; k < 4; ++k)
        {
            Rivet(mesh, FacingEye(loop[k] + Vec3{side * -0.026f, 0, 0}), 0.012f, SPD_METAL);
        }
    }
    const Vec3 leftArch[3] = {
        OnCanopySide(-1, SPD_WINDOW_TOP, SPD_WINDOW_BACK, -0.025f),
        OnCanopySide(-1, 1.85f, 1.70f, -0.025f),
        OnCanopySide(-1, SPD_ROOF_Y - 0.025f, 1.97f, -0.025f)};
    Sweep(mesh, leftArch, 3, rib, 4, true, false, {0, 0, 1}, SPD_FRAME, true);
    const Vec3 leftRail[2] = {
        {SPD_LEFT_TOP + 0.02f, SPD_ROOF_Y - 0.02f, 1.97f},
        {SPD_LEFT_TOP + 0.02f, SPD_ROOF_Y - 0.02f, BULKHEAD}};
    Tube(mesh, leftRail, 2, 0.025f, 8, SPD_FRAME);
    const Vec3 rightRail[2] = {
        {SPD_RIGHT_TOP - 0.02f, SPD_ROOF_Y - 0.02f, 1.97f},
        {SPD_RIGHT_TOP - 0.02f, SPD_ROOF_Y - 0.02f, BULKHEAD}};
    Tube(mesh, rightRail, 2, 0.025f, 8, SPD_FRAME);
    // Cabin: floor, walls up to the rim (the left one behind the game's), sills over them, the passenger's raised
    // floor carried to the right wall with its front under the dashboard and its riser behind.
    TiledQuad(mesh, {LEFT, FLOOR, 1.0f}, {RIGHT - LEFT, 0, 0}, {0, 0, BULKHEAD - 1.0f}, SPD_FLOOR);
    TiledQuad(mesh, {LEFT, FLOOR, 0.6f}, {0, 0, 0.76f}, {0, 0.55f - FLOOR, 0}, SPD_PLATE);
    TiledQuad(mesh, {RIGHT, FLOOR, 0.6f}, {0, 0, 0.76f}, {0, 0.55f - FLOOR, 0}, SPD_PLATE);
    TiledQuad(mesh, {LEFT, FLOOR, 1.36f}, {0, 0, BULKHEAD - 1.36f}, {0, RIM + 0.005f - FLOOR, 0}, SPD_PLATE);
    TiledQuad(mesh, {RIGHT, FLOOR, -0.47f}, {0, 0, BULKHEAD + 0.47f}, {0, RIM + 0.005f - FLOOR, 0}, SPD_PLATE);
    const float leftSill = 2.10f, rightSill = 2.10f;
    Box(mesh,
        MakeFrame({(LEFT + SPD_LEFT_RIM) * 0.5f + 0.02f, RIM, (leftSill + BULKHEAD) * 0.5f}, {0, 0, 1}, {0, 1, 0}),
        {(BULKHEAD - leftSill) * 0.5f, 0.03f, 0.07f},
        0.015f,
        SPD_PLATE);
    Box(mesh,
        MakeFrame({(RIGHT + SPD_RIGHT_RIM) * 0.5f - 0.02f, RIM, (rightSill + BULKHEAD) * 0.5f}, {0, 0, 1}, {0, 1, 0}),
        {(BULKHEAD - rightSill) * 0.5f, 0.03f, 0.07f},
        0.015f,
        SPD_PLATE);
    // Wall panels: machinery behind the seats on both sides, a vent under the game's left wall, the passenger's
    // padded side and a vent beside his seat.
    for (int side = -1; side <= 1; side += 2)
    {
        const float x = side < 0 ? LEFT + 0.005f : RIGHT - 0.005f;
        const Vec3 along = {0, 0, side < 0 ? -1.0f : 1.0f};
        Panel(
            mesh,
            MakeFrame({x, 0.62f, 2.95f}, along, {0, 1, 0}),
            0.42f,
            0.3f,
            0.03f,
            0.015f,
            SPD_FRAME,
            SPD_MACHINE,
            0.14f,
            SPD_METAL);
        Picture(mesh, MakeFrame({x, 0.3f, 1.75f}, along, {0, 1, 0}), 0.19f, 0.12f, SPD_VENT);
    }
    // Rear bulkhead: up to the rim, then round the rear window (as the exterior's, over the gunner's head) up to the
    // canopy's section, the window framed; machinery and a vent either side below the rim. Outside behind it the rear
    // deck between the hull decks, sloping to the tail, a maroon band down its middle.
    TiledQuad(mesh, {LEFT, FLOOR, BULKHEAD}, {RIGHT - LEFT, 0, 0}, {0, RIM + 0.005f - FLOOR, 0}, SPD_PLATE);
    constexpr float WINDOW_LOW = 1.45f, WINDOW_HIGH = 1.95f, WINDOW_LOW_HALF = 0.8f, WINDOW_HIGH_HALF = 0.6f;
    const Vec3 below[4] = {
        OnCanopySide(-1, RIM, BULKHEAD, 0),
        OnCanopySide(1, RIM, BULKHEAD, 0),
        OnCanopySide(1, WINDOW_LOW, BULKHEAD, 0),
        OnCanopySide(-1, WINDOW_LOW, BULKHEAD, 0)};
    PlanarPolygon(mesh, below, 4, {1, 0, 0}, SPD_PLATE);
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 beside[4] = {
            OnCanopySide(side, WINDOW_LOW, BULKHEAD, 0),
            {SPD_CENTER + side * WINDOW_LOW_HALF, WINDOW_LOW, BULKHEAD},
            {SPD_CENTER + side * WINDOW_HIGH_HALF, WINDOW_HIGH, BULKHEAD},
            OnCanopySide(side, WINDOW_HIGH, BULKHEAD, 0)};
        PlanarPolygon(mesh, beside, 4, {1, 0, 0}, SPD_PLATE);
    }
    const int sectionCount = CanopySection(BULKHEAD, 0, section);
    section[0] = OnCanopySide(-1, WINDOW_HIGH, BULKHEAD, 0);
    section[sectionCount - 1] = OnCanopySide(1, WINDOW_HIGH, BULKHEAD, 0);
    PlanarPolygon(mesh, section, sectionCount, {1, 0, 0}, SPD_PLATE);
    const Vec3 rearWindow[4] = {
        {SPD_CENTER - WINDOW_HIGH_HALF, WINDOW_HIGH, BULKHEAD + 0.01f},
        {SPD_CENTER + WINDOW_HIGH_HALF, WINDOW_HIGH, BULKHEAD + 0.01f},
        {SPD_CENTER + WINDOW_LOW_HALF, WINDOW_LOW, BULKHEAD + 0.01f},
        {SPD_CENTER - WINDOW_LOW_HALF, WINDOW_LOW, BULKHEAD + 0.01f}};
    const Vec3 wider = {0.04f, 0, 0}, higher = {0, 0.04f, 0}; // the glass reaches under the plate round it
    Quad(
        mesh,
        rearWindow[0] - wider + higher,
        rearWindow[1] + wider + higher,
        rearWindow[2] + wider - higher,
        rearWindow[3] - wider - higher,
        SPD_GLASS);
    Vec3 windowFrame[5];
    for (int k = 0; k < 5; ++k)
    {
        windowFrame[k] = rearWindow[k % 4] + Vec3{0, 0, -0.03f};
    }
    Tube(mesh, windowFrame, 5, 0.022f, 6, SPD_FRAME, false);
    PlanarQuad(
        mesh,
        {-0.84f, 1.12f, BULKHEAD},
        {2.0f, 1.12f, BULKHEAD},
        {1.76f, 0.98f, 7.3f},
        {-0.6f, 0.98f, 7.3f},
        SPD_HULL);
    TiledQuad(mesh, {SPD_CENTER - 0.06f, 1.122f, BULKHEAD}, {0, -0.14f, 3.4f}, {0.12f, 0, 0}, SPD_STRIPE);
    const Vec3 toCabin = {-1, 0, 0};
    Panel(
        mesh,
        MakeFrame({-0.32f, 0.64f, BULKHEAD - 0.005f}, toCabin, {0, 1, 0}),
        0.34f,
        0.3f,
        0.03f,
        0.015f,
        SPD_FRAME,
        SPD_MACHINE,
        0.14f,
        SPD_METAL);
    Panel(
        mesh,
        MakeFrame({SpeederMirror(-0.32f), 0.64f, BULKHEAD - 0.005f}, toCabin, {0, 1, 0}),
        0.34f,
        0.3f,
        0.03f,
        0.015f,
        SPD_FRAME,
        SPD_MACHINE,
        0.14f,
        SPD_METAL);
    // Seats: the pilot's under the eye between the lever consoles, the passenger's beside it; behind them on the
    // middle line the gunner's raised platform (his turning seat: SpeederGunnerSeat, after the cabin).
    SpeederSeat(mesh, 0.0f, FLOOR, 0.70f, _eye.z - 0.30f, _eye.z + 0.19f, 0.21f, _eye.y + 0.02f);
    SpeederSeat(mesh, SpeederMirror(0.0f), FLOOR, 0.70f, _eye.z - 0.30f, _eye.z + 0.19f, 0.21f, _eye.y + 0.02f);
    constexpr float PLATFORM_FRONT = 2.40f;
    Box(mesh,
        MakeFrame(
            {SPD_CENTER, (FLOOR + SPD_PLATFORM_Y) * 0.5f, (PLATFORM_FRONT + BULKHEAD) * 0.5f},
            {1, 0, 0},
            {0, 1, 0}),
        {0.5f, (SPD_PLATFORM_Y - FLOOR) * 0.5f, (BULKHEAD - PLATFORM_FRONT) * 0.5f},
        0.02f,
        SPD_PLATE);
    // Centre console between the front seats, carrying the game's footwell partition back: two button panels on top.
    constexpr float CONSOLE_X = SPD_CENTER, CONSOLE_TOP = 0.78f, CONSOLE_FRONT = 1.04f, CONSOLE_BACK = 1.95f;
    Box(mesh,
        MakeFrame(
            {CONSOLE_X, (FLOOR + CONSOLE_TOP) * 0.5f, (CONSOLE_FRONT + CONSOLE_BACK) * 0.5f},
            {1, 0, 0},
            {0, 1, 0}),
        {0.16f, (CONSOLE_TOP - FLOOR) * 0.5f, (CONSOLE_BACK - CONSOLE_FRONT) * 0.5f},
        0.02f,
        SPD_PLATE);
    for (int k = 0; k < 2; ++k)
    {
        Picture(
            mesh,
            MakeFrame({CONSOLE_X, CONSOLE_TOP, 1.3f + 0.4f * k}, {1, 0, 0}, {0, 0, -1}),
            0.13f,
            0.165f,
            SPD_CONSOLE);
    }
    // The pilot's lever consoles.
    const ConsoleLook console =
        {SPD_PLATE, SPD_FRAME, SPD_METAL, SPD_METAL, SPD_STATUS, SPD_DARK, SPD_SEAT, &SPD_SCREENS};
    FrontConsole(
        mesh,
        FLOOR,
        console,
        {1.15f,
         SPD_PLATE,
         SPD_FRAME,
         SPD_FACE,
         SPD_METAL,
         SPD_STATUS,
         {{SPD_BUTTONS, -0.115f, 0.03f, 0.02f, 0.024f},
          {SPD_SWITCHES, -0.035f, -0.085f, 0.0414f, 0.024f},
          {SPD_KNOBS, 0.06f, -0.085f, 0.031f, 0.016f}},
         {{SPD_BUTTONS, 0.115f, 0.03f, 0.02f, 0.024f},
          {SPD_KNOBS, -0.06f, -0.085f, 0.031f, 0.016f},
          {SPD_SWITCHES, 0.035f, -0.085f, 0.0414f, 0.024f}},
         {}});
    // Outside, seen through the glass: the hull decks (the game draws the left one up to z 1.32), each with a maroon
    // band along its rim as the game's left deck has.
    const int stations = static_cast<int>(sizeof(SPD_DECK) / sizeof(SPD_DECK[0]));
    const int partFinish[SPD_DECK_POINTS - 1] = {SPD_PLATE, SPD_STRIPE, SPD_PLATE, SPD_FRAME, SPD_PLATE, SPD_PLATE};
    for (int side = -1; side <= 1; side += 2)
    {
        const int first = side < 0 ? 3 : 0; // the left deck from the game's end at z 1.32
        Vec3 profiles[8][SPD_DECK_POINTS];
        const int count = stations - first;
        for (int i = 0; i < count; ++i)
        {
            DeckProfile(SPD_DECK[first + i], side, profiles[i]);
        }
        for (int part = 0; part + 1 < SPD_DECK_POINTS; ++part)
        {
            Vec3 lineA[8], lineB[8];
            for (int i = 0; i < count; ++i)
            {
                lineA[i] = profiles[i][part];
                lineB[i] = profiles[i][part + 1];
            }
            Loft(mesh, lineA, lineB, count, part == 2 || part == 4 ? 2 : 1, partFinish[part]);
        }
        // Gun pod on the deck's outer part beside the cockpit, its barrel forward; an engine nacelle aft.
        const float podX = side < 0 ? -1.95f : SpeederMirror(-1.95f);
        const float pod[7][2] =
            {{0, 0}, {0.04f, 0.1f}, {0.15f, 0.15f}, {1.3f, 0.15f}, {1.42f, 0.12f}, {1.5f, 0.07f}, {1.5f, 0}};
        Lathe(mesh, MakeFrame({podX, 0.98f, 0.85f}, {1, 0, 0}, {0, 0, -1}), pod, 7, 14, SPD_TREAD);
        const Vec3 barrel[2] = {{podX, 0.98f, -0.6f}, {podX, 0.98f, -1.75f}};
        Tube(mesh, barrel, 2, 0.045f, 10, SPD_FRAME);
        const float muzzle[4][2] = {{0, 0.06f}, {0.1f, 0.06f}, {0.12f, 0.045f}, {0.12f, 0}};
        Lathe(mesh, MakeFrame({podX, 0.98f, -1.72f}, {1, 0, 0}, {0, 0, -1}), muzzle, 4, 10, SPD_DARK);
        Box(mesh, MakeFrame({podX, 0.83f, 0.1f}, {1, 0, 0}, {0, 1, 0}), {0.07f, 0.07f, 0.5f}, 0.01f, SPD_FRAME);
        const float nacelleX = side < 0 ? -1.9f : SpeederMirror(-1.9f);
        Box(mesh, MakeFrame({nacelleX, 0.8f, 6.1f}, {1, 0, 0}, {0, 1, 0}), {0.62f, 0.45f, 1.45f}, 0.12f, SPD_HULL);
        TiledQuad(mesh, {nacelleX - side * 0.5f, 1.251f, 4.75f}, {0, 0, 2.7f}, {side * 0.12f, 0, 0}, SPD_STRIPE);
        Grille(
            mesh,
            MakeFrame({nacelleX, 0.8f, 4.649f}, {-1, 0, 0}, {0, 1, 0}),
            0.45f,
            0.28f,
            0.04f,
            6,
            0.03f,
            SPD_FRAME,
            SPD_METAL);
    }
    // Trim: bolted mouldings over the seams with the game's canopy (the left frame's rear end down to the cabin wall) and along the dashboard's lower lip; skirting along the
    // cabin's walls and the bulkhead, posts in the rear corners, seams and a hatch in the floor behind the seats.
    // The roof's edge (idren2023, headset run 17: the trim must line up with what it trims): the game's roof band ends at
    // y 2.07 along this line (all_1st_cockpit_combatspeeder.msh, canopy node: x -0.21 z 1.97 to x 0.61 z 0.58; the
    // right half its mirror), and the shell's headliner runs above it at RoofY. A fascia closes the step between them
    // along the edge, and a half-round moulding hangs under the edge itself, centred on it, its flat top on the band's
    // plane: it covers the joint from below and nothing of it can reach into the roof. Bolts every 0.45 m.
    {
        constexpr float EDGE_Y = 2.07f;
        const Vec3 edge[7] = {
            {-0.210f, EDGE_Y, 1.970f},
            {-0.164f, EDGE_Y, 1.142f},
            {0.132f, EDGE_Y, 0.809f},
            {SPD_CENTER, EDGE_Y, 0.595f},
            {SpeederMirror(0.132f), EDGE_Y, 0.809f},
            {SpeederMirror(-0.164f), EDGE_Y, 1.142f},
            {SpeederMirror(-0.210f), EDGE_Y, 1.970f}};
        for (int k = 0; k + 1 < 7; ++k)
        {
            const Vec3 lowA = edge[k], lowB = edge[k + 1];
            const Vec3 highA = {lowA.x, RoofY(lowA.x) + 0.002f, lowA.z}, highB = {lowB.x, RoofY(lowB.x) + 0.002f, lowB.z};
            const Vec3 first[3] = {lowA, lowB, highB};
            const Vec3 second[3] = {lowA, highB, highA};
            PlanarPolygon(mesh, first, 3, lowB - lowA, SPD_FRAME);
            PlanarPolygon(mesh, second, 3, lowB - lowA, SPD_FRAME);
        }
        constexpr float RADIUS = 0.022f;
        constexpr int ROUND = 8;
        float halfRound[ROUND + 1][2];
        for (int k = 0; k <= ROUND; ++k)
        {
            const float angle = PI + PI * k / ROUND; // from the side, under the edge, to the other side
            halfRound[k][0] = RADIUS * cosf(angle);
            halfRound[k][1] = RADIUS * sinf(angle);
        }
        Vec3 path[7];
        for (int k = 0; k < 7; ++k)
        {
            path[k] = edge[k] + Vec3{0, 0.001f, 0};
        }
        Sweep(mesh, path, 7, halfRound, ROUND + 1, true, true, {0, 1, 0}, SPD_FRAME, true);
        float along = 0.2f;
        for (int k = 0; k + 1 < 7; ++k)
        {
            const float length = Length(edge[k + 1] - edge[k]);
            while (along < length)
            {
                const Vec3 at = Lerp(edge[k], edge[k + 1], along / length) - Vec3{0, RADIUS, 0};
                Rivet(mesh, Frame{at, Normalized(edge[k + 1] - edge[k]), Normalized(Cross({0, -1, 0}, edge[k + 1] - edge[k])), {0, -1, 0}}, 0.009f, SPD_METAL);
                along += 0.45f;
            }
            along -= length;
        }
    }
    const Vec3 leftFrameEnd[4] =
        {{-0.73f, 0.51f, 2.14f}, {-0.73f, 0.83f, 2.14f}, {-0.64f, 1.16f, 1.62f}, {-0.64f, 1.16f, 0.90f}};
    SeamTrim(mesh, leftFrameEnd, 4, 1, 0.022f, 6, 0.4f, SPD_FRAME, SPD_METAL);
    Vec3 rightFrameEnd[4];
    for (int k = 0; k < 4; ++k)
    {
        rightFrameEnd[k] = {SpeederMirror(leftFrameEnd[k].x), leftFrameEnd[k].y, leftFrameEnd[k].z};
    }
    SeamTrim(mesh, rightFrameEnd, 4, 1, 0.022f, 6, 0.4f, SPD_FRAME, SPD_METAL);
    const Vec3 dashLip[5] = {
        {-0.29f, 0.69f, -0.43f},
        {0.91f, 0.69f, -0.43f},
        {1.05f, 0.69f, -0.45f},
        {1.20f, 0.69f, -0.43f},
        {1.81f, 0.69f, -0.43f}};
    SeamTrim(mesh, dashLip, 5, 1, 0.018f, 6, 0.3f, SPD_FRAME, SPD_METAL);
    Strip(mesh, {LEFT, FLOOR, 1.36f}, {LEFT, FLOOR, BULKHEAD}, {0, 1, 0}, {1, 0, 0}, 0.1f, 0.03f, SPD_FRAME);
    Strip(mesh, {RIGHT, FLOOR, 1.05f}, {RIGHT, FLOOR, BULKHEAD}, {0, 1, 0}, {-1, 0, 0}, 0.1f, 0.03f, SPD_FRAME);
    Strip(mesh, {LEFT, FLOOR, BULKHEAD}, {RIGHT, FLOOR, BULKHEAD}, {0, 1, 0}, {0, 0, -1}, 0.1f, 0.03f, SPD_FRAME);
    for (int side = 0; side < 2; ++side)
    {
        const float x = side == 0 ? LEFT + 0.04f : RIGHT - 0.04f;
        Box(mesh,
            MakeFrame({x, (FLOOR + RIM) * 0.5f, BULKHEAD - 0.04f}, {1, 0, 0}, {0, 1, 0}),
            {0.04f, (RIM - FLOOR) * 0.5f, 0.04f},
            0.012f,
            SPD_FRAME);
    }
    for (int seam = 0; seam < 3; ++seam)
    {
        const float z = 1.75f + 0.62f * seam;
        DeckSeam(mesh, {LEFT, FLOOR, z}, {RIGHT, FLOOR, z}, 0.025f, SPD_DARK);
    }
    FloorHatch(mesh, {SPD_CENTER, FLOOR, 2.08f}, 0.28f, 0.2f, SPD_FRAME, SPD_TREAD, SPD_METAL);
    SpeederStockMirror(mesh);
    SpeederDetails(mesh);
    SpeederGunnerSeat(mesh);
}

// ---- The fighter tanks' gunner position (PLAN 56, idren2023 2026-10-17/18) ----
// Replaces the stock turret (a 0.9 m dome with a short barrel, hidden by GunnerSeat during the hull's draw) with an
// open gunner's station: the gunner stands in a hatch on the turret's yaw axis, torso out of the tank; a traverse ring
// on the deck's platform, the hatch rim at hip height, a curved shield before the chest, twin grips on a yoke (they do
// not aim: the head does), the gun on trunnions before the shield, an open hatch lid behind. Space: ShellDesigns.h
// (the hull model's turned half way round). Measures from the hull (rep_hover_fightertank.msh, the Imperial's is the
// same mesh): the deck platform's top at y 1.80 (x +-0.4..0.5, 0.46 ahead of the axis to 0.34 behind; the rear spine
// 1.86 from 0.54 behind; 1.66 at the sides and ahead), the yaw axis (turret_y) at z 2.858, the gun's pitch axis
// (turret_x) 2.069 up and 0.540 ahead, the barrel (turret_barrel1) at 2.036, the muzzle (hp_gun_1) 1.328 ahead.
// Each part is modelled where it sits at rest; GunnerSeat draws it with its bone's live matrix.

enum TurretFinish
{
    TUR_PLATE,
    TUR_INNER,
    TUR_RED,
    TUR_GUNMETAL,
    TUR_BARREL,
    TUR_MACHINE,
    TUR_GRILLE,
    TUR_WELL,
    TUR_FLOOR,
    TUR_GRIP,
    TUR_TRIGGER,
    TUR_LAMP,
    TUR_DARK,
    TUR_STOCK_FRAME,
    TUR_STOCK_GLASS,
    TUR_STOCK_BACKING,
    TUR_FINISH_COUNT
};

// Patches of the hull's own exterior texture (rep_hover_fightertank / imp_hover_fightertank: one layout, 512 x 512;
// texel rectangles in the comments), the same on both tanks; only the plain colours differ.
const Finish REP_TURRET_FINISHES[TUR_FINISH_COUNT] = {
    {{0.295f, 0.346f, 0.398f, 0.418f}, 0.45f, 3, 0x888888, 0.15f, 12, 0}, // riveted plate (151..204, 177..214)
    {{0.295f, 0.346f, 0.398f, 0.418f}, 0.45f, 3, 0x6E6E6E, 0.10f, 10, 0}, // the same, shaded (inside faces)
    {{0.020f, 0.318f, 0.121f, 0.342f}, 0.5f, 3, 0x808080, 0.15f, 12, 0},  // red band (10..62, 163..175)
    {{0.670f, 0.344f, 0.820f, 0.414f}, 0.5f, 3, 0x808080, 0.40f, 24, 0},  // dark cylinder (343..420, 176..212)
    {{0.629f, 0.289f, 0.836f, 0.342f}, 0.45f, 3, 0x888888, 0.45f, 28, 0}, // ribbed barrel (322..428, 148..175)
    {{0.842f, 0.270f, 0.998f, 0.396f}, 0.25f, 3, 0x808080, 0.20f, 14, 0}, // slots and gauges (431..511, 138..203)
    {{0.879f, 0.400f, 0.992f, 0.496f}, 0.25f, 3, 0x808080, 0.15f, 12, 0}, // plate with grille (450..508, 205..254)
    {{0.699f, 0.557f, 0.820f, 1.000f}, 0.30f, 3, 0x5C5C5C, 0.10f, 10, 0}, // grille column (358..420, 285..512)
    {{0.127f, 0.508f, 0.684f, 0.625f}, 1.2f, 3, 0x505050, 0.05f, 8, 0},   // dark plating (65..350, 260..320)
    {{0, 0, 1, 1}, 1, 0, 0x1E1E20, 0.20f, 10, 0},                         // grips (rubber)
    {{0, 0, 1, 1}, 1, 0, 0x9A1E18, 0.30f, 16, 0},                         // triggers (clone red)
    {{0, 0, 1, 1}, 1, 0, 0xFFB040, 0, 1, 1.0f},                           // lamp (amber)
    {{0, 0, 1, 1}, 1, 0, 0x141518, 0.05f, 6, 0},                          // dark
    ScreenFrameFinish(),                                                  // stock screens (PLAN 57 j): frame
    ScreenGlassFinish(REP_SCREEN_ART),                                    // glass
    ScreenBackingFinish(REP_SCREEN_ART),                                  // behind the glass
};

const Finish IMP_TURRET_FINISHES[TUR_FINISH_COUNT] = {
    {{0.295f, 0.346f, 0.398f, 0.418f}, 0.45f, 3, 0x888888, 0.15f, 12, 0},
    {{0.295f, 0.346f, 0.398f, 0.418f}, 0.45f, 3, 0x6E6E6E, 0.10f, 10, 0},
    {{0.020f, 0.318f, 0.121f, 0.342f}, 0.5f, 3, 0x808080, 0.15f, 12, 0},
    {{0.670f, 0.344f, 0.820f, 0.414f}, 0.5f, 3, 0x808080, 0.40f, 24, 0},
    {{0.629f, 0.289f, 0.836f, 0.342f}, 0.45f, 3, 0x888888, 0.45f, 28, 0},
    {{0.842f, 0.270f, 0.998f, 0.396f}, 0.25f, 3, 0x808080, 0.20f, 14, 0},
    {{0.879f, 0.400f, 0.992f, 0.496f}, 0.25f, 3, 0x808080, 0.15f, 12, 0},
    {{0.699f, 0.557f, 0.820f, 1.000f}, 0.30f, 3, 0x5C5C5C, 0.10f, 10, 0},
    {{0.127f, 0.508f, 0.684f, 0.625f}, 1.2f, 3, 0x505050, 0.05f, 8, 0},
    {{0, 0, 1, 1}, 1, 0, 0x1C1C1E, 0.20f, 10, 0},
    {{0, 0, 1, 1}, 1, 0, 0x3A3A40, 0.30f, 16, 0}, // triggers (dark steel)
    {{0, 0, 1, 1}, 1, 0, 0xFF3020, 0, 1, 1.0f},   // lamp (red)
    {{0, 0, 1, 1}, 1, 0, 0x141518, 0.05f, 6, 0},
    ScreenFrameFinish(),
    ScreenGlassFinish(IMP_SCREEN_ART),
    ScreenBackingFinish(IMP_SCREEN_ART),
};

constexpr float TURRET_AXIS_Z = 2.858f, TURRET_DECK = 1.80f;
constexpr float GUN_PIVOT_Y = 2.069f, GUN_PIVOT_AHEAD = 0.540f, BARREL_Y = 2.036f, MUZZLE_AHEAD = 1.328f;
/// The hatch: its rim (hip height), the collar's outer and the well's radius, the well's floor (the gunner's feet).
constexpr float HATCH_RIM = 2.02f, COLLAR_RADIUS = 0.455f, WELL_RADIUS = 0.415f, WELL_FLOOR = 1.10f;
const Vec3 GUNNER_EYE = {-FIGHTER_TANK_GUNNER_EYE[0], FIGHTER_TANK_GUNNER_EYE[1], -FIGHTER_TANK_GUNNER_EYE[2]};

// A point of the turret: `side` to the right of the yaw axis, at height y, `ahead` before it.
Vec3 TurretPoint(float side, float y, float ahead)
{
    return {side, y, TURRET_AXIS_Z - ahead};
}

// A point on a circle round the yaw axis: radius, height, angle from straight ahead (radians, right positive).
Vec3 AroundAxis(float radius, float y, float angle)
{
    return TurretPoint(radius * sinf(angle), y, radius * cosf(angle));
}

/// The edges of an ArcPlate: each rises from its height at the plate's ends to its height halfway (a sine).
struct ArcEdges
{
    float bottomAtEnds, bottomAtMiddle, topAtEnds, topAtMiddle;
};

// A curved plate round the yaw axis from angle `from` to `to` (radians from straight ahead, right positive), between
// two radii and between its bottom and top edges.
void ArcPlate(
    Mesh& mesh,
    float inner,
    float outer,
    float from,
    float to,
    const ArcEdges& edges,
    int outerFinish,
    int innerFinish,
    int edgeFinish)
{
    constexpr int STEPS = 18;
    Vec3 outerBottom[STEPS + 1], outerTop[STEPS + 1], innerBottom[STEPS + 1], innerTop[STEPS + 1];
    for (int step = 0; step <= STEPS; ++step)
    {
        const float t = static_cast<float>(step) / STEPS;
        const float angle = from + (to - from) * t;
        const float rise = sinf(PI * t);
        const float bottom = edges.bottomAtEnds + (edges.bottomAtMiddle - edges.bottomAtEnds) * rise;
        const float top = edges.topAtEnds + (edges.topAtMiddle - edges.topAtEnds) * rise;
        outerBottom[step] = AroundAxis(outer, bottom, angle);
        outerTop[step] = AroundAxis(outer, top, angle);
        innerBottom[step] = AroundAxis(inner, bottom, angle);
        innerTop[step] = AroundAxis(inner, top, angle);
    }
    Loft(mesh, outerBottom, outerTop, STEPS + 1, 2, outerFinish);
    Loft(mesh, innerBottom, innerTop, STEPS + 1, 2, innerFinish);
    Loft(mesh, innerTop, outerTop, STEPS + 1, 1, edgeFinish);
    PlanarQuad(mesh, outerBottom[0], innerBottom[0], innerTop[0], outerTop[0], edgeFinish);
    PlanarQuad(mesh, innerBottom[STEPS], outerBottom[STEPS], outerTop[STEPS], innerTop[STEPS], edgeFinish);
}

// A circle round the yaw axis as a closed path (count points, the last the first again).
int AxisCircle(float radius, float y, Vec3* path, int count)
{
    for (int point = 0; point < count; ++point)
    {
        path[point] = AroundAxis(radius, y, 2 * PI * point / (count - 1));
    }
    return count;
}

// Part 0, on turret_y: ring, collar and well, chest shield, trunnion cheeks, yoke and grips, side boxes, hatch lid.
void BuildTurretBase(Mesh& mesh)
{
    const Frame axis = MakeFrame(TurretPoint(0, TURRET_DECK, 0), {1, 0, 0}, {0, 1, 0});
    // Traverse ring on the platform, bolted round its top.
    const float ring[5][2] = {{-0.04f, 0.47f}, {0.0f, 0.52f}, {0.05f, 0.52f}, {0.07f, 0.50f}, {0.07f, 0.44f}};
    Lathe(mesh, axis, ring, 5, 64, TUR_GUNMETAL);
    for (int bolt = 0; bolt < 18; ++bolt)
    {
        const Vec3 at = AroundAxis(0.485f, TURRET_DECK + 0.07f, 2 * PI * bolt / 18);
        Rivet(mesh, MakeFrame(at, {1, 0, 0}, {0, 0, -1}), 0.011f, TUR_GUNMETAL);
    }
    // Collar up to the hatch rim, a rolled lip on it, and the well down to the gunner's footplate.
    const float collar[3][2] = {
        {0.07f, COLLAR_RADIUS},
        {HATCH_RIM - TURRET_DECK - 0.02f, COLLAR_RADIUS},
        {HATCH_RIM - TURRET_DECK, COLLAR_RADIUS - 0.015f}};
    Lathe(mesh, axis, collar, 3, 64, TUR_PLATE);
    Vec3 lip[37];
    const int lipCount = AxisCircle(WELL_RADIUS + 0.018f, HATCH_RIM, lip, 37);
    Tube(mesh, lip, lipCount, 0.022f, 8, TUR_GUNMETAL, false);
    const float well[3][2] = {
        {HATCH_RIM - TURRET_DECK, WELL_RADIUS},
        {WELL_FLOOR - TURRET_DECK, WELL_RADIUS},
        {WELL_FLOOR - TURRET_DECK, 0.0f}};
    Lathe(mesh, axis, well, 2, 48, TUR_WELL);
    Lathe(mesh, axis, well + 1, 2, 48, TUR_FLOOR);
    // Inside the well (idren2023 2026-10-19: every station polished): rungs up its rear wall to climb in by, a tread
    // plate on its floor, the station's systems panel on its front wall under the yoke, cables from the ring's boxes
    // up to the yoke's control box.
    const DetailLook look = {TUR_PLATE, TUR_GUNMETAL, TUR_BARREL, TUR_DARK, TUR_LAMP, TUR_RED};
    for (int rung = 0; rung < 3; ++rung)
    {
        const float y = WELL_FLOOR + 0.25f + 0.25f * rung;
        const Vec3 bar[4] = {
            AroundAxis(WELL_RADIUS - 0.005f, y, PI - 0.32f),
            AroundAxis(WELL_RADIUS - 0.07f, y, PI - 0.30f),
            AroundAxis(WELL_RADIUS - 0.07f, y, PI + 0.30f),
            AroundAxis(WELL_RADIUS - 0.005f, y, PI + 0.32f)};
        Tube(mesh, bar, 4, 0.013f, 10, TUR_GUNMETAL, false);
    }
    TreadPlate(mesh, TurretPoint(-0.24f, WELL_FLOOR, 0.24f), {0.48f, 0, 0}, {0, 0, 0.48f}, 0.07f, TUR_FLOOR, TUR_GUNMETAL);
    ControlPanel(mesh, MakeFrame(TurretPoint(0, 1.56f, WELL_RADIUS - 0.03f), {1, 0, 0}, {0, 1, 0}), 0.17f, 0.12f, look);
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 cable[4] = {
            TurretPoint(0.47f * side, 2.07f, 0.06f),
            TurretPoint(0.40f * side, 2.16f, 0.18f),
            TurretPoint(0.16f * side, 2.21f, 0.30f),
            TurretPoint(0.05f * side, 2.205f, 0.335f)};
        CableLoom(mesh, cable, 4, 2, 0.007f, {0, 1, 0}, TUR_DARK, TUR_GUNMETAL, 0.12f);
    }
    // Chest shield over the front arc: riveted plate, a red band along its inside top, bolts under the band.
    constexpr float SHIELD_HALF_ANGLE = 62.0f * PI / 180.0f, SHIELD_TOP = 2.36f, SHIELD_TOP_ENDS = 2.20f;
    ArcPlate(
        mesh,
        COLLAR_RADIUS,
        COLLAR_RADIUS + 0.035f,
        -SHIELD_HALF_ANGLE,
        SHIELD_HALF_ANGLE,
        {HATCH_RIM - 0.08f, HATCH_RIM - 0.08f, SHIELD_TOP_ENDS, SHIELD_TOP},
        TUR_PLATE,
        TUR_INNER,
        TUR_GUNMETAL);
    ArcPlate(
        mesh,
        COLLAR_RADIUS - 0.006f,
        COLLAR_RADIUS - 0.001f,
        -SHIELD_HALF_ANGLE + 0.04f,
        SHIELD_HALF_ANGLE - 0.04f,
        {SHIELD_TOP_ENDS - 0.075f, SHIELD_TOP - 0.075f, SHIELD_TOP_ENDS - 0.025f, SHIELD_TOP - 0.025f},
        TUR_RED,
        TUR_RED,
        TUR_RED);
    for (int bolt = 0; bolt < 11; ++bolt)
    {
        const float t = static_cast<float>(bolt) / 10;
        const float angle = -SHIELD_HALF_ANGLE + 0.1f + (2 * SHIELD_HALF_ANGLE - 0.2f) * t;
        const float top = SHIELD_TOP_ENDS + (SHIELD_TOP - SHIELD_TOP_ENDS) * sinf(PI * (0.06f + 0.88f * t));
        const Vec3 at = AroundAxis(COLLAR_RADIUS - 0.007f, top - 0.105f, angle);
        Rivet(mesh, MakeFrame(at, {cosf(angle), 0, sinf(angle)}, {0, 1, 0}), 0.009f, TUR_GUNMETAL);
    }
    // Trunnion cheeks before the shield, the gun's pitch axis through them.
    for (int side = -1; side <= 1; side += 2)
    {
        const Frame cheek = MakeFrame(TurretPoint(0.16f * side, 2.01f, 0.57f), {1, 0, 0}, {0, 1, 0});
        Box(mesh, cheek, {0.03f, 0.17f, 0.10f}, 0.012f, TUR_PLATE);
        Box(mesh,
            MakeFrame(TurretPoint(0.16f * side, GUN_PIVOT_Y, GUN_PIVOT_AHEAD), {1, 0, 0}, {0, 1, 0}),
            {0.034f, 0.045f, 0.045f},
            0.01f,
            TUR_GUNMETAL);
    }
    // Yoke: a column from the well's front wall, a bent crossbar, two grips leaning forward with a trigger each, a
    // small control box with a lamp between them.
    const Vec3 column[2] = {TurretPoint(0, 1.94f, 0.40f), TurretPoint(0, 2.17f, 0.335f)};
    Tube(mesh, column, 2, 0.026f, 10, TUR_GUNMETAL);
    const Vec3 bar[4] = {
        TurretPoint(-0.17f, 2.165f, 0.30f),
        TurretPoint(-0.12f, 2.18f, 0.335f),
        TurretPoint(0.12f, 2.18f, 0.335f),
        TurretPoint(0.17f, 2.165f, 0.30f)};
    Tube(mesh, bar, 4, 0.017f, 10, TUR_GUNMETAL);
    // The grips run through the hand's place (FIGHTER_TANK_GUNNER_GRIP, 0.6 of the way up) at its lean.
    const float gripSide = -FIGHTER_TANK_GUNNER_GRIP[0], gripHand = FIGHTER_TANK_GUNNER_GRIP[1];
    const float gripAhead = TURRET_AXIS_Z + FIGHTER_TANK_GUNNER_GRIP[2];
    const float gripLean = tanf(FIGHTER_TANK_GUNNER_GRIP_LEAN_DEGREES * PI / 180.0f);
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 grip[2] = {
            TurretPoint(gripSide * side, gripHand - 0.09f, gripAhead - 0.09f * gripLean),
            TurretPoint(gripSide * side, gripHand + 0.06f, gripAhead + 0.06f * gripLean)};
        Tube(mesh, grip, 2, 0.021f, 12, TUR_GRIP);
        Box(mesh,
            MakeFrame(TurretPoint(0.17f * side, 2.255f, 0.338f), {1, 0, 0}, {0, 1, 0}),
            {0.006f, 0.016f, 0.012f},
            0.002f,
            TUR_TRIGGER);
    }
    const Frame controls = MakeFrame(TurretPoint(0, 2.205f, 0.34f), {1, 0, 0}, {0, 1, 0});
    Box(mesh, controls, {0.055f, 0.032f, 0.028f}, 0.006f, TUR_MACHINE);
    Box(mesh, Offset(controls, 0, 0.034f, 0), {0.012f, 0.004f, 0.012f}, 0.002f, TUR_LAMP);
    // Boxes on the ring at each side (power cells): plate with a grille on top.
    for (int side = -1; side <= 1; side += 2)
    {
        const Frame box = MakeFrame(TurretPoint(0.47f * side, 1.955f, -0.02f), {1, 0, 0}, {0, 1, 0});
        Box(mesh, box, {0.05f, 0.11f, 0.16f}, 0.015f, TUR_PLATE);
        TiledQuad(mesh, box.At(-0.04f, 0.112f, 0.13f), {0.08f, 0, 0}, {0, 0, -0.26f}, TUR_GRILLE);
    }
    // The HUD on one curved stock monitor on the shield (PLAN 57 j, idren2023 2026-10-19): set upright against the
    // shield's inside, bent round the turret's yaw axis as the shield is (0.43 m out, +-32 deg across, 18..28 deg below
    // the eye), above the gun, so a glance down reads it while the turret follows the head's yaw: the player's health
    // left, the weapon's heat in the middle with the target's health under it, the tank's health right. The map is left
    // out of this seat; the score stays on the head's panel.
    {
        constexpr float GLASS_HALF_WIDTH = 0.225f, GLASS_HALF_HEIGHT = 0.05f, SCREEN_RADIUS = 0.43f;
        constexpr float SCREEN_Y = 2.34f, TEXEL_METERS = 0.00015f;
        const bool isImperial = mesh.finishes == IMP_TURRET_FINISHES;
        const StockScreenLook screens = {
            isImperial ? &IMP_SCREEN_ART : &REP_SCREEN_ART,
            TUR_STOCK_FRAME,
            TUR_STOCK_GLASS,
            TUR_STOCK_BACKING,
            TUR_DARK};
        const Frame face = MakeFrame(TurretPoint(0, SCREEN_Y, SCREEN_RADIUS), {1, 0, 0}, {0, 1, 0});
        StockScreen(
            mesh,
            face,
            GLASS_HALF_WIDTH,
            GLASS_HALF_HEIGHT,
            TEXEL_METERS,
            screens,
            0.01f,
            nullptr,
            nullptr,
            SCREEN_RADIUS);
        SetDisplayScreen(&screens);
        AddDisplayRegion(GROUP_HEALTH, PLAYER_HEALTH, CurvedFrame(face, -0.15f, SCREEN_RADIUS), 0, 0, 0.13f, 0.08f, 0);
        AddDisplay(GROUP_AMMUNITION, CurvedFrame(face, 0, SCREEN_RADIUS), 0, 0.02f, 0.13f, 0.045f);
        AddDisplay(GROUP_TARGET, CurvedFrame(face, 0, SCREEN_RADIUS), 0, -0.027f, 0.11f, 0.025f);
        AddDisplayRegion(GROUP_HEALTH, VEHICLE_HEALTH, CurvedFrame(face, 0.15f, SCREEN_RADIUS), 0, 0, 0.13f, 0.08f, 0);
        AddDisplay(GROUP_MAP, face, 0, 0, 0, 0);
        SetDisplayScreen(nullptr);
    }
    // Hatch lid, open behind the gunner (hinged on the rim, leaning back 20 degrees), on two knuckles.
    constexpr float LID_RADIUS = 0.40f, LID_LEAN = 20.0f * PI / 180.0f;
    const Vec3 hinge = TurretPoint(0, HATCH_RIM + 0.02f, -(WELL_RADIUS + 0.02f));
    const Vec3 upLid = {0, cosf(LID_LEAN), sinf(LID_LEAN)}, towardGunner = {0, sinf(LID_LEAN), -cosf(LID_LEAN)};
    const Frame lid = MakeFrame(hinge + upLid * (LID_RADIUS + 0.03f), {1, 0, 0}, towardGunner);
    const float disc[4][2] = {{0, 0}, {0, LID_RADIUS}, {-0.035f, LID_RADIUS}, {-0.035f, 0}};
    Lathe(mesh, lid, disc, 4, 28, TUR_PLATE);
    const float boss[3][2] = {{0.0f, 0.16f}, {0.02f, 0.14f}, {0.02f, 0.0f}};
    Lathe(mesh, lid, boss, 3, 20, TUR_GUNMETAL);
    const Vec3 knuckles[2] = {hinge + Vec3{-0.14f, 0, 0}, hinge + Vec3{0.14f, 0, 0}};
    Tube(mesh, knuckles, 2, 0.026f, 10, TUR_GUNMETAL);
}

// Part 1, on turret_x: the trunnion axle and the gun's cradle with a vented top and a recoil buffer under it.
void BuildTurretCradle(Mesh& mesh)
{
    const Vec3 axle[2] = {
        TurretPoint(-0.19f, GUN_PIVOT_Y, GUN_PIVOT_AHEAD),
        TurretPoint(0.19f, GUN_PIVOT_Y, GUN_PIVOT_AHEAD)};
    Tube(mesh, axle, 2, 0.03f, 12, TUR_GUNMETAL);
    // The cradle starts just before the shield's outer face (0.49 ahead) and ends where the barrel's jacket begins.
    const Frame cradle = MakeFrame(TurretPoint(0, 2.065f, 0.69f), {1, 0, 0}, {0, 1, 0});
    Box(mesh, cradle, {0.10f, 0.075f, 0.17f}, 0.02f, TUR_PLATE);
    TiledQuad(mesh, cradle.At(-0.075f, 0.077f, 0.13f), {0.15f, 0, 0}, {0, 0, -0.25f}, TUR_MACHINE);
    Box(mesh, Offset(cradle, 0, 0, 0.177f), {0.08f, 0.06f, 0.008f}, 0.004f, TUR_GUNMETAL);
    const Vec3 buffer[2] = {TurretPoint(0, 1.975f, 0.56f), TurretPoint(0, 1.975f, 0.96f)};
    Tube(mesh, buffer, 2, 0.026f, 16, TUR_BARREL);
    // Cooling fins along the cradle's sides, a feed box on its left with its cable.
    for (int side = -1; side <= 1; side += 2)
    {
        for (int fin = 0; fin < 7; ++fin)
        {
            Box(mesh,
                MakeFrame(TurretPoint(side * 0.108f, 2.065f, 0.57f + 0.04f * fin), {1, 0, 0}, {0, 1, 0}),
                {0.012f, 0.06f, 0.006f},
                0.002f,
                TUR_GUNMETAL);
        }
    }
    const Frame feed = MakeFrame(TurretPoint(-0.15f, 2.10f, 0.70f), {1, 0, 0}, {0, 1, 0});
    Box(mesh, feed, {0.035f, 0.045f, 0.09f}, 0.008f, TUR_MACHINE);
    const Vec3 feedCable[3] = {feed.At(0, -0.045f, 0.05f), TurretPoint(-0.13f, 1.98f, 0.60f), TurretPoint(-0.06f, 1.96f, 0.56f)};
    CableLoom(mesh, feedCable, 3, 1, 0.008f, {1, 0, 0}, TUR_DARK, TUR_GUNMETAL, 0);
}

// Part 2, on turret_barrel1: jacket, barrel with two rings, flash hider. Headset run 17 (PLAN 58 c, idren2023: bigger,
// longer): 1.35 times as thick and 0.32 m longer than b250's, whose muzzle sat on the game's hp_gun_1 (MUZZLE_AHEAD);
// the game's bolts and flash still start there, inside the barrel's last third.
void BuildTurretBarrel(Mesh& mesh)
{
    constexpr float K = 1.35f, END = MUZZLE_AHEAD + 0.32f, HIDER = END - 0.08f;
    const Vec3 jacket[2] = {TurretPoint(0, BARREL_Y, 0.86f), TurretPoint(0, BARREL_Y, 1.12f)};
    Tube(mesh, jacket, 2, 0.052f * K, 24, TUR_BARREL);
    for (int ring = 0; ring < 6; ++ring)
    {
        const float at = 0.88f + 0.04f * ring;
        const Vec3 band[2] = {TurretPoint(0, BARREL_Y, at), TurretPoint(0, BARREL_Y, at + 0.012f)};
        Tube(mesh, band, 2, 0.058f * K, 24, TUR_GUNMETAL);
    }
    const Vec3 barrel[2] = {TurretPoint(0, BARREL_Y, 1.12f), TurretPoint(0, BARREL_Y, HIDER)};
    Tube(mesh, barrel, 2, 0.033f * K, 20, TUR_GUNMETAL);
    for (int ring = 0; ring < 3; ++ring)
    {
        const float at = 1.20f + 0.14f * ring;
        const Vec3 band[2] = {TurretPoint(0, BARREL_Y, at), TurretPoint(0, BARREL_Y, at + 0.016f)};
        Tube(mesh, band, 2, 0.040f * K, 14, TUR_GUNMETAL);
    }
    const Vec3 hider[2] = {TurretPoint(0, BARREL_Y, HIDER), TurretPoint(0, BARREL_Y, END)};
    Tube(mesh, hider, 2, 0.045f * K, 20, TUR_DARK);
    for (int slot = 0; slot < 6; ++slot)
    {
        const float angle = 2 * PI * slot / 6;
        const float r = 0.046f * K;
        Box(mesh,
            MakeFrame(TurretPoint(r * sinf(angle), BARREL_Y + r * cosf(angle), END - 0.04f), {cosf(angle), -sinf(angle), 0}, {sinf(angle), cosf(angle), 0}),
            {0.008f, 0.003f, 0.025f},
            0.001f,
            TUR_GUNMETAL);
    }
}

void BuildFighterTankTurret(Mesh& mesh)
{
    mesh.part = 0;
    BuildTurretBase(mesh);
    mesh.part = 1;
    BuildTurretCradle(mesh);
    mesh.part = 2;
    BuildTurretBarrel(mesh);
}

/// Building turrets' viewscreens (PLAN 58 u, GunnerSeat BuildingTurret.screenDesign): the scope pass's picture on a
/// screen of the mod's (the dish and Geonosis: on a rail, part 1), its middle band (the picture is square), self-lit,
/// in a dark case. Design space is the turret model's turned half way round y (x right, y up, z toward the gunner).
const Finish BUILDING_SCREEN_FINISHES[] = {
    {{0, 0, 1, 1}, 1, 6, 0xFFFFFF, 0, 1, 1.0f},   // the gun's view
    {{0, 0, 1, 1}, 1, 0, 0x3C4045, 0.35f, 22, 0},       // handle bars (dark metal)
    {{0, 0, 1, 1}, 1, 0, 0x202124, 0.20f, 10, 0},       // grips (rubber)
};
constexpr int BUILDING_SCREEN_FINISH_COUNT = 3;
constexpr float SCREEN_LIFT_METERS = 0.002f; // in front of the stock face, toward the gunner

void ScreenPicture(Mesh& mesh, Vec3 centre, float halfWidth, float halfHeight)
{
    const float z = centre.z + SCREEN_LIFT_METERS;
    const float band = 0.5f * halfHeight / halfWidth;
    Quad(
        mesh,
        {centre.x - halfWidth, centre.y + halfHeight, z},
        {centre.x + halfWidth, centre.y + halfHeight, z},
        {centre.x + halfWidth, centre.y - halfHeight, z},
        {centre.x - halfWidth, centre.y - halfHeight, z},
        0,
        0,
        0.5f - band,
        1,
        0.5f + band);
}

// A rail screen (part 1): the picture in a thin dark case, its centre `rail` ahead of the eye at pitch 0.
void RailScreen(Mesh& mesh, Vec3 eye, float offsetX, float rail, float halfWidth, float halfHeight)
{
    mesh.part = 1;
    const Vec3 centre = {eye.x + offsetX, eye.y, eye.z - rail};
    ScreenPicture(mesh, centre, halfWidth, halfHeight);
    // The housing (headset run 19, idren2023: the screen floated): a deep dark case behind the picture, a bezel round it,
    // a visor over its top and cheeks at its sides reaching toward the eye (shading the picture, as a gun sight's
    // hood), and a bracket under it down to where its rail would carry it.
    constexpr float RIM = 0.018f, DEPTH = 0.07f, HOOD = 0.06f;
    Box(mesh,
        MakeFrame({centre.x, centre.y, centre.z - DEPTH * 0.5f}, {1, 0, 0}, {0, 1, 0}),
        {halfWidth + RIM, halfHeight + RIM, DEPTH * 0.5f},
        0.008f,
        1);
    const float z = centre.z + SCREEN_LIFT_METERS;
    for (int side = -1; side <= 1; side += 2)
    {
        Box(mesh, MakeFrame({centre.x, centre.y + side * (halfHeight + RIM * 0.5f), z + 0.004f}, {1, 0, 0}, {0, 1, 0}),
            {halfWidth + RIM, RIM * 0.5f, 0.006f}, 0.002f, 2); // bezel top and bottom
        Box(mesh, MakeFrame({centre.x + side * (halfWidth + RIM * 0.5f), centre.y, z + 0.004f}, {1, 0, 0}, {0, 1, 0}),
            {RIM * 0.5f, halfHeight, 0.006f}, 0.002f, 2); // bezel sides
        Box(mesh,
            MakeFrame({centre.x + side * (halfWidth + RIM - 0.004f), centre.y + 0.01f, z + HOOD * 0.5f}, {1, 0, 0},
                      {0, 1, 0}),
            {0.004f, halfHeight + RIM - 0.01f, HOOD * 0.5f}, 0.002f, 1); // cheeks
    }
    Box(mesh, MakeFrame({centre.x, centre.y + halfHeight + RIM - 0.004f, z + HOOD * 0.5f + 0.005f}, {1, 0, 0},
                        {0, 1, -0.25f}),
        {halfWidth + RIM, 0.004f, HOOD * 0.5f + 0.005f}, 0.002f, 1); // visor
    Box(mesh, MakeFrame({centre.x, centre.y - halfHeight - RIM - 0.04f, centre.z - DEPTH * 0.5f}, {1, 0, 0}, {0, 1, 0}),
        {0.03f, 0.04f, 0.02f}, 0.005f, 1); // bracket
    mesh.part = 0;
}

// hoth_bldg_dish_turret: the gunner behind the column (the model's own green screen in the hood stays as it is).
constexpr Vec3 DISH_GUNNER_EYE = {0, 1.75f, 1.30f};

void BuildDishScreen(Mesh& mesh)
{
    RailScreen(
        mesh, DISH_GUNNER_EYE, 0, DISH_SCREEN_RAIL_METERS, DISH_SCREEN_HALF_WIDTH_METERS, DISH_SCREEN_HALF_HEIGHT_METERS);
    // Two handles of the mod's (the model has nothing to hold): bars from the column's sloped back under the screen
    // (a face from y 1.43, z 0.51, x +-0.35 up to the hood at y 1.74, z 0.78; the bars began 0.3 m off it until b373,
    // idren2023: floating) on bolted plates, out to pistol grips leaning ahead 15 deg (GunnerSeat's grips: x +-0.26, y
    // 1.38, 1.02 behind the column's axis).
    const Vec3 slope = Normalized(Vec3{0, 0.31f, 0.27f});
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 foot = {side * 0.20f, 1.47f, 0.545f}; // on the face
        Box(mesh, MakeFrame(foot, {1, 0, 0}, slope), {0.045f, 0.05f, 0.008f}, 0.003f, 1);
        const Vec3 bar[4] = {
            {foot.x, foot.y, foot.z - 0.01f}, {side * 0.22f, 1.47f, 0.75f}, {side * 0.25f, 1.47f, 0.93f}, {side * 0.26f, 1.46f, 1.00f}};
        Tube(mesh, bar, 4, 0.016f, 8, 1);
        const Vec3 grip[2] = {{side * 0.26f, 1.30f, 1.04f}, {side * 0.26f, 1.46f, 1.00f}};
        Tube(mesh, grip, 2, 0.019f, 10, 2);
    }
}

// geo_bldg_geoturret: the gunner on the foot pads (the stock viewfinder box stays as it is).
constexpr Vec3 GEO_GUNNER_EYE = {0, 1.86f, 1.40f};

void BuildGeoScreen(Mesh& mesh)
{
    RailScreen(
        mesh,
        GEO_GUNNER_EYE,
        -GEO_SCREEN_OFFSET_X_METERS,
        GEO_SCREEN_RAIL_METERS,
        GEO_SCREEN_HALF_WIDTH_METERS,
        GEO_SCREEN_HALF_HEIGHT_METERS);
}

// ---- The Hoth tower's cabin (PLAN 58 u; idren2023 2026-10-20: the white Hoth turret as the AAT gunner's cabin with a
// lore-accurate interior; 2026-10-21: professional interior and texture work, 40 000 triangles at most, every texel
// the same size and none stretched). The tower's head is a 16-sided drum 3.1 m across and only a metre tall (y
// 3.14..4.14; wall r 1.56 from 3.37 to 3.80, roof slope to r 1.02 at 3.93, a raised hatch ring r 0.36..0.70 at
// 4.08), the gun in its front (pivot 0 3.58 1.46 of the model). The gunner sits low in the tower's top, legs in its
// well, the eye in the drum under the hatch: a Rebel gun cab in Echo Base's own materials (the command centre's
// consoles, the bunkers' white panelling, the tower's own plates), all at the shells' one texel density (160 per
// metre; every texture here 256 texels, so a whole texture covers 1.6 m): flat faces tile their patch by real size,
// console art goes whole onto faces cut to its own size. Design space: the model turned half way round y (x right,
// y up, z back). Part 0 turns with the head, part 1 also pitches with the gun.
//
// Textures: 1 turret256 (the tower; the shell takes the map's light from its draws), 2 HOTH_bldg_bunker, 3
// HOTH_bldg_command_center_console, 4 the stock viewfinder art com_1st_cockpit_hothviewfinder (blended: its window's
// alpha), 5 the Rebel stock gunner screen all_1st_cockpit_securitycam (the snowspeeder's rear gunner's, loaded on
// Hoth: the HUD screens' frames, glass and scan lines, as the tanks' dashboards), 6 the gun's view.
enum HothTowerFinish
{
    HTW_PANELLING, // bunker: plain white panelling, tiled
    HTW_ROOF,     // bunker: big framed panels with rivets, tiled
    HTW_ROOF_FLAT, // turret256: plates with seams, tiled
    HTW_RIVET_BAND, // bunker: a band with a row of rivets
    HTW_SKIRT,    // bunker: the band of two pipes, tiled along the skirt
    HTW_DECK,     // console: dark framed floor plate, tiled
    HTW_PLATE,    // console: dark stained plate (desk, racks), tiled
    HTW_HATCH_ART, // turret256: the round hatch, whole
    HTW_VENT,     // bunker: the louvred vent, whole
    HTW_KEYBOARD, // console art, whole: keyboard strip
    HTW_KEYSIDE,  //   its end panel
    HTW_KNOBS_L,  //   knob panels
    HTW_KNOBS_R,
    HTW_SWITCHES, //   switch strip
    HTW_BUTTONS,  //   button grid
    HTW_SCOPE_A,  //   the three green scopes (self-lit)
    HTW_SCOPE_B,
    HTW_SCOPE_C,
    HTW_UNIT,     //   side unit with switches
    HTW_KEYPAD,   //   button block
    HTW_METAL,
    HTW_DARK,
    HTW_FRAME,
    HTW_RUBBER,
    HTW_LAMP,
    HTW_GREEN,
    HTW_PAD,
    HTW_ACCENT,
    HTW_YELLOW,
    HTW_SCREEN_ART,
    HTW_PICTURE,
    HTW_STOCK_FRAME,
    HTW_STOCK_GLASS,
    HTW_STOCK_BACKING,
    HTW_POLISHED,
    HTW_BUTTON,
    HTW_BODY_LIGHT,
    HTW_FINISH_COUNT
};

constexpr float HTW_TEXELS = 256.0f;

// A finish on one of the cabin's 256-texel textures at the shells' one density, from its texel rectangle.
constexpr Finish HtwTexture(
    int texture, float x0, float y0, float x1, float y1, uint32_t color, float specular, float power, float glow = 0)
{
    Finish finish = {{x0 / HTW_TEXELS, y0 / HTW_TEXELS, x1 / HTW_TEXELS, y1 / HTW_TEXELS}, 1, texture, color, specular, power, glow};
    finish.textureTexels = HTW_TEXELS;
    finish.isDense = true;
    return finish;
}

const Finish HTW_FINISHES[HTW_FINISH_COUNT] = {
    HtwTexture(2, 2, 178, 126, 254, 0x8E9296, 0.12f, 10),        // white panelling (bunker)
    HtwTexture(2, 130, 14, 254, 124, 0x868A8E, 0.15f, 12),       // roof: big framed panels (bunker)
    HtwTexture(1, 0, 64, 64, 96, 0x8A8E92, 0.15f, 12),           // hatch ring: plates with seams (turret256)
    HtwTexture(2, 0, 56, 128, 82, 0x868A8E, 0.15f, 12),          // rivet band (bunker)
    HtwTexture(2, 0, 132, 128, 174, 0x8A8E92, 0.20f, 14),        // skirt: two pipes in a band (bunker)
    HtwTexture(3, 200, 194, 254, 254, 0x8C8C8C, 0.25f, 16),      // deck plate (console)
    HtwTexture(3, 146, 34, 188, 160, 0x888888, 0.20f, 14),       // dark plate (console)
    HtwTexture(1, 12, 138, 118, 244, 0x8A8E92, 0.20f, 14),       // round hatch (turret256)
    HtwTexture(2, 94, 2, 124, 38, 0x8A8E92, 0.15f, 12),          // vent (bunker)
    HtwTexture(3, 2, 166, 142, 192, 0x909090, 0.20f, 14, 0.15f), // keyboard
    HtwTexture(3, 144, 166, 190, 192, 0x909090, 0.20f, 14, 0.25f),
    HtwTexture(3, 4, 60, 72, 112, 0x909090, 0.25f, 16, 0.10f),   // knob panels
    HtwTexture(3, 74, 60, 142, 112, 0x909090, 0.25f, 16, 0.10f),
    HtwTexture(3, 4, 36, 72, 58, 0x909090, 0.20f, 14, 0.20f),    // switch strip
    HtwTexture(3, 74, 36, 142, 58, 0x909090, 0.20f, 14, 0.25f),  // button grid
    HtwTexture(3, 6, 114, 46, 156, 0x9A9A9A, 0.40f, 30, 0.60f),  // scopes
    HtwTexture(3, 52, 114, 98, 156, 0x9A9A9A, 0.40f, 30, 0.60f),
    HtwTexture(3, 100, 114, 142, 156, 0x9A9A9A, 0.40f, 30, 0.60f),
    HtwTexture(3, 190, 28, 216, 76, 0x909090, 0.20f, 14, 0.15f), // side unit
    HtwTexture(3, 190, 78, 214, 112, 0x909090, 0.20f, 14, 0.20f), // button block
    {{0, 0, 1, 1}, 1, 0, 0x5C6064, 0.40f, 28, 0},   // bright metal
    {{0, 0, 1, 1}, 1, 0, 0x18191C, 0.05f, 6, 0},    // recesses, seams
    {{0, 0, 1, 1}, 1, 0, 0x6A6E73, 0.25f, 16, 0},   // frames, mouldings, ribs
    {{0, 0, 1, 1}, 1, 0, 0x222326, 0.20f, 10, 0},   // grips, cables
    {{0, 0, 1, 1}, 1, 0, 0xFFB050, 0, 1, 1.0f},     // amber lamps
    {{0, 0, 1, 1}, 1, 0, 0x50FF70, 0, 1, 1.0f},     // green lamps
    {{0, 0, 1, 1}, 1, 0, 0x5E5446, 0.15f, 10, 0},   // seat padding (worn tan)
    {{0, 0, 1, 1}, 1, 0, 0xB85A1E, 0.20f, 14, 0},   // Rebel orange
    {{0, 0, 1, 1}, 1, 0, 0xC8A020, 0.20f, 14, 0},   // hazard yellow
    {{0, 0, 1, 1}, 1, 4, 0x808080, 0.30f, 20, 0.6f, 0.99f}, // the viewfinder art, its own alpha
    {{0, 0.191f, 1, 0.809f}, 1, 6, 0xFFFFFF, 0, 1, 1.0f},   // the gun's view (its window 1.62 : 1)
    ScreenFrameFinish(),                                    // stock screens: frame
    ScreenGlassFinish(ALL_SCREEN_ART),                      //   glass
    ScreenBackingFinish(ALL_SCREEN_ART),                    //   behind the glass
    {{0, 0, 1, 1}, 1, 0, 0xA4A8AC, 0.60f, 40, 0},           // polished metal (collars)
    {{0, 0, 1, 1}, 1, 0, 0xB01E16, 0.40f, 24, 0.15f},       // red buttons
    {{0, 0, 1, 1}, 1, 0, 0x9EA2A6, 0.30f, 20, 0},           // the CRT's bezel (Rebel white-grey)
};

const StockScreenLook HTW_SCREENS = {&ALL_SCREEN_ART, HTW_STOCK_FRAME, HTW_STOCK_GLASS, HTW_STOCK_BACKING, HTW_DARK};

/// The gunner's eye (GunnerSeat's BuildingTurret: model 0 3.75 -0.10).
constexpr Vec3 HOTH_TOWER_EYE = {0, 3.75f, 0.10f};
constexpr float HTW_FLOOR_Y = 2.45f, HTW_DECK_Y = 3.22f;
/// The drum's inside as rings of the 16-gon (corner radius, height): well rim, deck edge, wall foot, wall top, roof
/// edge, hatch ring foot, its top, the hatch (turret256's round hatch, 102 texels: 0.64 m across).
constexpr float HTW_WELL = 0.98f, HTW_DECK_OUT = 1.38f, HTW_WALL = 1.50f, HTW_WALL_FOOT = 3.40f, HTW_WALL_TOP = 3.78f;
constexpr float HTW_ROOF_IN = 1.02f, HTW_ROOF_Y = 3.90f, HTW_RING = 0.72f, HTW_RING_TOP = 4.03f, HTW_HATCH = 0.32f;
/// The viewscreen: the art's square (half HTW_ART_HALF) and the picture behind its window (texels 28..484 x
/// 120..402 of 512: half 0.205 x 0.127 m, its centre 0.0045 m under the square's).
constexpr float HTW_ART_HALF = 0.23f, HTW_SCREEN_Y = 3.66f, HTW_SCREEN_Z = -0.43f;
constexpr int HTW_SIDES = 16;
/// The turntable (idren2023 2026-10-21: the whole interior visibly turns): the deck to this corner radius, then a bearing
/// race with rollers in a dark gap, then the drum's ledge; a bracing cage of posts and rings stands on it.
constexpr float HTW_TURNTABLE = 1.30f, HTW_CAGE = 1.27f, HTW_CAGE_TOP = 3.80f;

// A corner of the 16-gon (index k, corner k at k * 22.5 deg from straight ahead, to the right) at radius r, height y.
Vec3 HtwCorner(int k, float r, float y)
{
    const float a = static_cast<float>(k) * 2 * PI / HTW_SIDES;
    return {r * sinf(a), y, -r * cosf(a)};
}

// A frame on facet k (between corners k and k+1) at radius r (of the corners), height y: x along the facet (to the
// right as seen from the axis), y up, z toward the axis.
Frame HtwFacet(int k, float r, float y)
{
    const Vec3 a = HtwCorner(k, r, y), b = HtwCorner(k + 1, r, y);
    const Vec3 middle = (a + b) * 0.5f;
    return MakeFrame(middle, b - a, {0, 1, 0});
}

// A picture of a whole finish (console art) at the shells' density: the face cut to the patch's own size.
void HtwPicture(Mesh& mesh, const Frame& frame, int finish)
{
    const float halfWidth = MetersU(mesh, finish) * 0.5f, halfHeight = MetersV(mesh, finish) * 0.5f;
    Picture(mesh, frame, halfWidth, halfHeight, finish);
}

// One band of the drum between two rings, every facet a flat quad tiled by its real size.
void HtwBand(Mesh& mesh, float r0, float y0, float r1, float y1, int finish)
{
    for (int k = 0; k < HTW_SIDES; ++k)
    {
        PlanarQuad(mesh, HtwCorner(k, r0, y0), HtwCorner(k + 1, r0, y0), HtwCorner(k + 1, r1, y1), HtwCorner(k, r1, y1), finish);
    }
}

// An equipment rack standing on the deck against facet k: a dark plate body from the deck to under the roof, its
// face leaning back 8 deg; `screen` puts a HUD screen (group) in its upper half, else a side unit and button block.
void HtwRack(Mesh& mesh, int k, int screenGroup, int lower)
{
    const Frame foot = HtwFacet(k, HTW_WALL * cosf(PI / HTW_SIDES), HTW_DECK_Y); // on the facet's plane
    constexpr float HALF_WIDTH = 0.25f, DEPTH = 0.20f, TOP = HTW_WALL_TOP - 0.02f;
    const float height = TOP - HTW_DECK_Y;
    // Body: two flanks, the face plate (leaning), a top.
    const Vec3 lowFront = foot.At(0, 0, DEPTH), highFront = foot.At(0, height, DEPTH - 0.06f);
    const Vec3 x = foot.x;
    PlanarQuad(mesh, lowFront - x * HALF_WIDTH, lowFront + x * HALF_WIDTH, highFront + x * HALF_WIDTH, highFront - x * HALF_WIDTH, HTW_PLATE);
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 s = x * (side * HALF_WIDTH);
        PlanarQuad(mesh, foot.At(0, 0, 0) + s, lowFront + s, highFront + s, foot.At(0, height, 0) + s, HTW_PLATE);
        const Vec3 edge[2] = {lowFront + s, highFront + s};
        Tube(mesh, edge, 2, 0.012f, 6, HTW_FRAME);
    }
    PlanarQuad(mesh, foot.At(-HALF_WIDTH, height, 0), foot.At(HALF_WIDTH, height, 0), highFront + x * HALF_WIDTH, highFront - x * HALF_WIDTH, HTW_FRAME);
    const Vec3 lip[2] = {highFront - x * HALF_WIDTH, highFront + x * HALF_WIDTH};
    Tube(mesh, lip, 2, 0.012f, 6, HTW_FRAME);
    // The face's own frame (leaning): pictures on it, 3 mm proud.
    const Vec3 up = Normalized(highFront - lowFront);
    const Frame face = {lowFront, x, up, Cross(x, up)};
    if (screenGroup >= 0)
    {
        // A stock Rebel screen (as the tanks' dashboards) in a dark case, the HUD group on its glass in the
        // screen's readout colour.
        const Frame screen = Offset(face, 0, height * 0.70f, 0.012f);
        Box(mesh, Offset(screen, 0, 0, -0.006f), {0.205f, 0.105f, 0.006f}, 0.004f, HTW_DARK);
        StockScreen(mesh, screen, 0.15f, 0.07f, SCREEN_TEXEL_METERS * 0.07f / 0.0675f, HTW_SCREENS);
        SetDisplayScreen(&HTW_SCREENS);
        AddDisplay(screenGroup, Offset(screen, 0, 0, 0.003f), 0, 0, 0.27f, 0.12f);
        SetDisplayScreen(nullptr);
    }
    else
    {
        HtwPicture(mesh, Offset(face, -0.09f, height * 0.66f, 0.003f), HTW_UNIT);
        HtwPicture(mesh, Offset(face, 0.10f, height * 0.70f, 0.003f), HTW_KEYPAD);
    }
    HtwPicture(mesh, Offset(face, 0, height * 0.30f, 0.003f), lower);
    for (int lamp = -2; lamp <= 2; ++lamp)
    {
        IndicatorLamp(mesh, Upright(Offset(face, lamp * 0.06f, 0.035f, 0.003f)), 0.007f, HTW_METAL, lamp % 2 ? HTW_GREEN : HTW_LAMP);
    }
}

// A grip as the tank levers' (LeverModel, scaled 1.2: a gloved fist round a gun's control): a ribbed rubber boot, a
// polished collar, an oval rubber grip with three finger grooves swelling under the palm, a metal dome cap with a red
// thumb button toward the gunner, a curved trigger blade in front; on an oval carriage at its foot. `base`: the
// grip's foot, its axis leaning ahead by leanRadians.
void HtwGrip(Mesh& mesh, Vec3 base, float leanRadians)
{
    constexpr float S = 1.2f, BOOT = 0.030f * S, SHAFT = 0.034f * S, BOTTOM = 0.046f * S, TOP = 0.154f * S;
    constexpr float CAP = 0.174f * S, OVAL = 1.18f;
    constexpr int SIDES = 16;
    const Frame frame = MakeFrame(base, {1, 0, 0}, {0, cosf(leanRadians), -sinf(leanRadians)}); // y up it, z back
    const float carriage[5][2] = {{-0.026f, 0.034f}, {-0.007f, 0.034f}, {-0.002f, 0.030f}, {0, 0.024f}, {0, 0}};
    Lathe(mesh, frame, carriage, 5, SIDES, HTW_FRAME, 1.4f);
    float boot[13][2];
    for (int ring = 0; ring < 13; ++ring)
    {
        const float t = ring / 12.0f, rib = 0.5f + 0.5f * cosf(2 * PI * 3 * t);
        boot[ring][0] = t * BOOT;
        boot[ring][1] = S * (0.019f + t * (0.013f - 0.019f) + 0.0028f * rib * (1 - 0.4f * t));
    }
    Lathe(mesh, frame, boot, 13, SIDES, HTW_RUBBER);
    const float collar[6][2] = {{BOOT - 0.002f, 0.0125f * S}, {SHAFT, 0.0145f * S}, {SHAFT + 0.002f, 0.0165f * S},
                                {BOTTOM - 0.003f, 0.0165f * S}, {BOTTOM - 0.001f, 0.015f * S}, {BOTTOM, 0.015f * S}};
    Lathe(mesh, frame, collar, 6, SIDES, HTW_POLISHED);
    float grip[25][2];
    for (int ring = 0; ring < 25; ++ring)
    {
        const float t = ring / 24.0f;
        const float grooves = t < 0.62f ? 0.0021f * sinf(PI * 3 * t / 0.62f) : 0.0f;
        grip[ring][0] = BOTTOM + t * (TOP - BOTTOM);
        grip[ring][1] = S * (0.0168f + 0.0028f * sinf(PI * t) - fabsf(grooves));
    }
    Lathe(mesh, frame, grip, 25, SIDES, HTW_RUBBER, OVAL);
    float cap[9][2];
    for (int ring = 0; ring < 9; ++ring)
    {
        const float angle = 0.5f * PI * ring / 8;
        cap[ring][0] = TOP + (CAP - TOP) * sinf(angle);
        cap[ring][1] = S * 0.0185f * cosf(angle) + (ring == 8 ? 0.0f : 0.0004f);
    }
    Lathe(mesh, frame, cap, 9, SIDES, HTW_METAL, OVAL);
    const float button[5][2] = {{0, 0}, {0, 0.0058f * S}, {0.006f * S, 0.0058f * S}, {0.007f * S, 0.0045f * S}, {0.007f * S, 0}};
    Lathe(mesh, Offset(frame, 0, CAP - 0.007f * S, 0.009f * S), button, 5, 12, HTW_BUTTON);
    // The trigger: a blade 1.4 cm wide curving forward and down from its hinge under the fingers, its tip curled.
    constexpr int STEPS = 8;
    constexpr float HALF = 0.007f, THICK = 0.0048f, LENGTH = 0.031f, HINGE_Y = BOTTOM + 0.090f * S, HINGE_Z = -0.0205f * S;
    Vec3 rear[STEPS + 1], front[STEPS + 1];
    for (int step = 0; step <= STEPS; ++step)
    {
        const float t = static_cast<float>(step) / STEPS;
        const float y = HINGE_Y - LENGTH * t, z = HINGE_Z - 0.0126f * sinf(PI * 0.85f * t) - 0.0048f * t * t;
        rear[step] = frame.At(0, y, z);
        front[step] = frame.At(0, y, z - THICK);
    }
    const Vec3 half = frame.x * HALF;
    for (int step = 0; step < STEPS; ++step)
    {
        Quad(mesh, rear[step] - half, rear[step] + half, rear[step + 1] + half, rear[step + 1] - half, HTW_METAL);
        Quad(mesh, front[step] + half, front[step] - half, front[step + 1] - half, front[step + 1] + half, HTW_METAL);
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 s = half * static_cast<float>(side);
            Quad(mesh, rear[step] + s, rear[step + 1] + s, front[step + 1] + s, front[step] + s, HTW_METAL);
        }
    }
    Quad(mesh, rear[STEPS] - half, rear[STEPS] + half, front[STEPS] + half, front[STEPS] - half, HTW_METAL);
}

// The minimap's CRT on the desk (idren2023 2026-10-21: a small Star Wars CRT at a 40 deg angle facing the gunner): a
// chunky housing with a deep bezel round a stock Rebel screen (the map on its glass), its back tapering into a tube
// housing with a vent grille, two knobs and a lamp under the screen; it hangs in a yoke (trunnions either side) on a
// base plate on the desk's slope at (x, along) of the desk top's frame, tilted back 40 deg and turned toward the eye.
void HtwCrt(Mesh& mesh, const Frame& desk, float x, float along, Vec3 eye)
{
    constexpr float HALF_W = 0.125f, HALF_H = 0.105f, BEZEL = 0.035f, BACK = 0.17f, TILT = 40.0f * PI / 180;
    constexpr float RISE = 0.17f; // the pivot over the desk
    const Vec3 base = desk.At(x, along, 0);
    const Vec3 pivot = base + Vec3{0, RISE, 0};
    Vec3 toEye = eye - pivot;
    toEye.y = 0;
    toEye = Normalized(toEye);
    // Face normal: toward the eye's heading, tilted up by TILT; x level.
    const Vec3 normal = toEye * cosf(TILT) + Vec3{0, sinf(TILT), 0};
    const Vec3 xAxis = Normalized(Cross({0, 1, 0}, normal));
    const Vec3 yAxis = Cross(normal, xAxis);
    const Frame face = {pivot + normal * 0.02f, xAxis, yAxis, normal}; // the screen's plane
    // Housing: the bezel block, the tapering back (four flat sides and a cap), a vent grille on its top.
    Box(mesh, Offset(face, 0, 0, -BEZEL * 0.5f), {HALF_W, HALF_H, BEZEL * 0.5f}, 0.018f, HTW_BODY_LIGHT);
    const float backW = HALF_W * 0.62f, backH = HALF_H * 0.62f;
    const Vec3 front[4] = {face.At(-HALF_W + 0.01f, HALF_H - 0.01f, -BEZEL), face.At(HALF_W - 0.01f, HALF_H - 0.01f, -BEZEL),
                           face.At(HALF_W - 0.01f, -HALF_H + 0.01f, -BEZEL), face.At(-HALF_W + 0.01f, -HALF_H + 0.01f, -BEZEL)};
    const Vec3 back[4] = {face.At(-backW, backH, -BACK), face.At(backW, backH, -BACK), face.At(backW, -backH, -BACK), face.At(-backW, -backH, -BACK)};
    for (int edge = 0; edge < 4; ++edge)
    {
        const int next = (edge + 1) % 4;
        PlanarQuad(mesh, front[next], front[edge], back[edge], back[next], HTW_PLATE);
    }
    PlanarQuad(mesh, back[0], back[1], back[2], back[3], HTW_PLATE);
    Grille(mesh, MakeFrame((front[0] + front[1] + back[0] + back[1]) * 0.25f + yAxis * 0.004f, xAxis, Normalized(back[0] - front[0])),
           0.06f, 0.035f, 0.006f, 5, 0.004f, HTW_FRAME, HTW_DARK);
    // The screen: a stock Rebel screen in the bezel, the map on it.
    const Frame screen = Offset(face, 0, 0.012f, 0.002f);
    StockScreen(mesh, screen, 0.075f, 0.065f, SCREEN_TEXEL_METERS * 0.065f / 0.0675f, HTW_SCREENS);
    SetDisplayScreen(&HTW_SCREENS);
    AddDisplay(GROUP_MAP, Offset(screen, 0, 0, 0.003f), 0, 0, 0.125f, 0.125f);
    AddDisplay(GROUP_SCORE, screen, 0, 0, 0, 0);
    SetDisplayScreen(nullptr);
    // Under the screen: two knobs and a lamp on the bezel's bottom rail.
    for (int knob = -1; knob <= 1; knob += 2)
    {
        const float ring[4][2] = {{0, 0.011f}, {0.008f, 0.011f}, {0.011f, 0.009f}, {0.011f, 0}};
        Lathe(mesh, Upright(Offset(face, knob * 0.085f, -HALF_H + 0.016f, 0)), ring, 4, 12, HTW_METAL);
    }
    IndicatorLamp(mesh, Upright(Offset(face, 0, -HALF_H + 0.016f, 0)), 0.006f, HTW_METAL, HTW_GREEN);
    // The yoke: trunnions in the housing's flanks, arms down to a base plate on the desk.
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 trunnion = pivot + xAxis * (side * (HALF_W * 0.85f + 0.012f)) - normal * 0.05f;
        const Frame hub = MakeFrame(trunnion, normal, xAxis);
        const float boss[3][2] = {{-0.012f, 0.022f}, {0.012f, 0.022f}, {0.016f, 0.016f}};
        Lathe(mesh, hub, boss, 3, 12, HTW_POLISHED);
        const Vec3 arm[3] = {trunnion + xAxis * (side * 0.014f), base + xAxis * (side * (HALF_W * 0.85f + 0.026f)) + desk.z * 0.02f, base + xAxis * (side * 0.05f) + desk.z * 0.012f};
        Tube(mesh, arm, 3, 0.011f, 8, HTW_FRAME);
    }
    Box(mesh, Offset(desk, x, along, 0.008f), {0.13f, 0.07f, 0.008f}, 0.004f, HTW_FRAME);
}

void BuildHothTowerCabin(Mesh& mesh)
{
    const Vec3 eye = HOTH_TOWER_EYE, up = {0, 1, 0};
    const DetailLook detail = {HTW_PLATE, HTW_FRAME, HTW_METAL, HTW_DARK, HTW_LAMP, HTW_ACCENT};
    const float facetRadius = cosf(PI / HTW_SIDES); // a facet's middle at this times the corner radius
    mesh.part = 0;
    SetDisplayScreen(nullptr);
    // ---- The drum: deck, skirt, wall, roof slope, roof flat, hatch ring, hatch ----
    HtwBand(mesh, HTW_WELL, HTW_DECK_Y, HTW_TURNTABLE, HTW_DECK_Y, HTW_DECK);
    // Headset run 19 (idren2023: the walls turned with the head): the drum round the turntable stays still (part 2, the
    // tower's own frame); the turntable, seat, desk and racks turn (part 0), the breech pitches (part 1).
    mesh.part = 2;
    HtwBand(mesh, HTW_TURNTABLE, HTW_DECK_Y - 0.006f, HTW_TURNTABLE + 0.035f, HTW_DECK_Y - 0.006f, HTW_DARK);
    HtwBand(mesh, HTW_TURNTABLE + 0.035f, HTW_DECK_Y, HTW_DECK_OUT, HTW_DECK_Y, HTW_FRAME);
    HtwBand(mesh, HTW_DECK_OUT, HTW_DECK_Y, HTW_WALL, HTW_WALL_FOOT, HTW_SKIRT);
    HtwBand(mesh, HTW_WALL, HTW_WALL_FOOT, HTW_WALL, HTW_WALL_TOP, HTW_PANELLING);
    HtwBand(mesh, HTW_WALL, HTW_WALL_TOP, HTW_ROOF_IN, HTW_ROOF_Y, HTW_ROOF);
    HtwBand(mesh, HTW_ROOF_IN, HTW_ROOF_Y, HTW_RING, HTW_ROOF_Y, HTW_ROOF);
    HtwBand(mesh, HTW_RING, HTW_ROOF_Y, HTW_RING, HTW_RING_TOP, HTW_RIVET_BAND);
    HtwBand(mesh, HTW_RING, HTW_RING_TOP, HTW_HATCH, HTW_RING_TOP, HTW_ROOF_FLAT);
    {
        const Frame hatch = {{0, HTW_RING_TOP + 0.004f, 0}, {1, 0, 0}, {0, 0, -1}, {0, -1, 0}};
        HtwPicture(mesh, hatch, HTW_HATCH_ART);
        GrabHandle(mesh, {-0.11f, HTW_RING_TOP + 0.004f, 0}, {0.11f, HTW_RING_TOP + 0.004f, 0}, {0, -1, 0}, 0.05f, 0.011f, HTW_ACCENT, HTW_POLISHED);
    }
    mesh.part = 0;
    {
        // Bearing race and rollers, gear teeth under the well's rim, radial braces on the plate.
        constexpr int RACE_STEPS = 33;
        const float raceRadius = (HTW_TURNTABLE + 0.0175f) * facetRadius;
        Vec3 race[RACE_STEPS];
        for (int step = 0; step < RACE_STEPS; ++step)
        {
            const float a = 2 * PI * step / (RACE_STEPS - 1);
            race[step] = {raceRadius * sinf(a), HTW_DECK_Y + 0.004f, -raceRadius * cosf(a)};
        }
        Tube(mesh, race, RACE_STEPS, 0.006f, 6, HTW_POLISHED, false);
        for (int roller = 0; roller < 32; ++roller)
        {
            const float a = 2 * PI * (roller + 0.5f) / 32;
            const Frame at = MakeFrame({raceRadius * sinf(a), HTW_DECK_Y - 0.006f, -raceRadius * cosf(a)}, {cosf(a), 0, sinf(a)}, up);
            const float puck[4][2] = {{0, 0.011f}, {0.010f, 0.011f}, {0.012f, 0.008f}, {0.012f, 0}};
            Lathe(mesh, at, puck, 4, 8, HTW_METAL);
        }
        for (int tooth = 0; tooth < 64; ++tooth)
        {
            const float a = 2 * PI * (tooth + 0.5f) / 64, r = HTW_WELL * facetRadius - 0.012f;
            const Frame at = MakeFrame({r * sinf(a), HTW_DECK_Y - 0.035f, -r * cosf(a)}, {cosf(a), 0, sinf(a)}, up);
            Box(mesh, at, {0.010f, 0.022f, 0.012f}, 0, HTW_FRAME);
        }
        for (int brace = 0; brace < 8; ++brace)
        {
            const float a = 2 * PI * (brace + 0.5f) / 8, r = (HTW_WELL + HTW_TURNTABLE) * 0.5f * facetRadius;
            const Vec3 radial = {sinf(a), 0, -cosf(a)};
            const Frame at = MakeFrame(radial * r + Vec3{0, HTW_DECK_Y + 0.006f, 0}, Cross(up, radial), up);
            Box(mesh, at, {0.022f, 0.006f, (HTW_TURNTABLE - HTW_WELL) * 0.5f * facetRadius - 0.01f}, 0.003f, HTW_FRAME);
            for (int bolt = -1; bolt <= 1; bolt += 2)
            {
                const Frame bolted = {at.At(0, 0.006f, bolt * 0.12f), at.x, at.z * -1.0f, up}; // z up
                Rivet(mesh, bolted, 0.006f, HTW_METAL);
            }
        }
        // The cage: posts between the racks, a foot ring and a head ring under the roof, knee braces.
        constexpr int RING_STEPS = 33;
        Vec3 head[RING_STEPS], foot[RING_STEPS];
        for (int step = 0; step < RING_STEPS; ++step)
        {
            const float a = 2 * PI * step / (RING_STEPS - 1);
            head[step] = {HTW_CAGE * sinf(a), HTW_CAGE_TOP, -HTW_CAGE * cosf(a)};
            foot[step] = {HTW_CAGE * sinf(a), HTW_DECK_Y + 0.016f, -HTW_CAGE * cosf(a)};
        }
        Tube(mesh, head, RING_STEPS, 0.018f, 8, HTW_FRAME, false);
        Tube(mesh, foot, RING_STEPS, 0.014f, 8, HTW_FRAME, false);
        const float posts[7] = {-123.75f, -78.75f, -33.75f, 33.75f, 78.75f, 123.75f, 180.0f};
        for (float degrees : posts)
        {
            const float a = degrees * PI / 180;
            const Vec3 radial = {sinf(a), 0, -cosf(a)};
            const Frame post = MakeFrame(radial * HTW_CAGE + Vec3{0, (HTW_DECK_Y + HTW_CAGE_TOP) * 0.5f, 0}, Cross(up, radial), up);
            Box(mesh, post, {0.022f, (HTW_CAGE_TOP - HTW_DECK_Y) * 0.5f, 0.022f}, 0.005f, HTW_FRAME);
            for (int side = -1; side <= 1; side += 2)
            {
                const float b = a + side * 9.0f * PI / 180;
                const Vec3 knee[2] = {radial * HTW_CAGE + Vec3{0, HTW_CAGE_TOP - 0.17f, 0}, {HTW_CAGE * sinf(b), HTW_CAGE_TOP - 0.01f, -HTW_CAGE * cosf(b)}};
                Tube(mesh, knee, 2, 0.009f, 6, HTW_FRAME);
            }
            Box(mesh, Offset(post, 0, -(HTW_CAGE_TOP - HTW_DECK_Y) * 0.5f + 0.02f, 0), {0.045f, 0.02f, 0.045f}, 0.006f, HTW_FRAME); // shoe
        }
    }
    mesh.part = 2; // the drum's structure and the well stay still with the walls
    // ---- Structure: a rib at every corner (wall and roof slope), mouldings along the wall's top and foot with
    // rivets, a lamp strip under every other roof facet ----
    for (int k = 0; k < HTW_SIDES; ++k)
    {
        const Vec3 foot = HtwCorner(k, HTW_WALL - 0.012f, HTW_WALL_FOOT), top = HtwCorner(k, HTW_WALL - 0.012f, HTW_WALL_TOP);
        const Vec3 inward = Normalized(Vec3{-foot.x, 0, -foot.z});
        const Frame rib = MakeFrame((foot + top) * 0.5f, Cross(up, inward), up);
        Box(mesh, rib, {0.028f, (HTW_WALL_TOP - HTW_WALL_FOOT) * 0.5f, 0.014f}, 0.006f, HTW_FRAME);
        const Vec3 roof[2] = {HtwCorner(k, HTW_WALL - 0.02f, HTW_WALL_TOP + 0.005f), HtwCorner(k, HTW_ROOF_IN, HTW_ROOF_Y - 0.012f)};
        Tube(mesh, roof, 2, 0.016f, 6, HTW_FRAME);
        for (int level = 0; level < 2; ++level)
        {
            const float y = level ? HTW_WALL_TOP - 0.018f : HTW_WALL_FOOT + 0.018f;
            const Frame belt = Offset(HtwFacet(k, HTW_WALL, y), 0, 0, 0.012f);
            const float halfLength = HTW_WALL * sinf(PI / HTW_SIDES);
            Box(mesh, belt, {halfLength, 0.018f, 0.012f}, 0.005f, HTW_FRAME);
            for (int rivet = -1; rivet <= 1; rivet += 2)
            {
                Rivet(mesh, Offset(belt, rivet * halfLength * 0.6f, 0, 0.012f), 0.006f, HTW_METAL);
            }
        }
        if (k % 2 == 1)
        {
            // Lamp strip: on the roof slope's facet, a housing and its lamps.
            const Vec3 a = HtwCorner(k, 1.30f, 3.83f), b = HtwCorner(k + 1, 1.30f, 3.83f);
            const Vec3 slopeDown = Normalized(HtwCorner(k, HTW_WALL, HTW_WALL_TOP) - HtwCorner(k, HTW_ROOF_IN, HTW_ROOF_Y));
            const Frame strip = {(a + b) * 0.5f, Normalized(b - a), slopeDown, Cross(Normalized(b - a), slopeDown)};
            Box(mesh, Offset(strip, 0, 0, -0.012f), {0.20f, 0.03f, 0.012f}, 0.005f, HTW_FRAME);
            Box(mesh, Offset(strip, 0, 0, -0.025f), {0.17f, 0.012f, 0.002f}, 0, k % 4 == 1 ? HTW_LAMP : HTW_GREEN);
        }
    }
    // Vents on the wall behind and the rear quarters.
    for (int k : {5, 10})
    {
        HtwPicture(mesh, Offset(HtwFacet(k, HTW_WALL, (HTW_WALL_FOOT + HTW_WALL_TOP) * 0.5f), 0, 0, 0.004f), HTW_VENT);
    }
    // Conduits up the wall at four corners, on brackets.
    for (int k : {2, 5, 11, 14})
    {
        const Vec3 run[3] = {HtwCorner(k, 1.40f, HTW_DECK_Y + 0.05f), HtwCorner(k, HTW_WALL - 0.06f, HTW_WALL_FOOT), HtwCorner(k, HTW_WALL - 0.06f, HTW_WALL_TOP)};
        const Vec3 outward = Normalized(Vec3{run[1].x, 0, run[1].z});
        FlangedPipe(mesh, run, 3, 0.022f, outward, 0.18f, HTW_METAL, HTW_FRAME);
    }
    // ---- The well: its wall (16 flat faces), ribs, a floor plate, the ladder ----
    HtwBand(mesh, HTW_WELL, HTW_FLOOR_Y, HTW_WELL, HTW_DECK_Y, HTW_PANELLING);
    {
        Vec3 floorPlate[HTW_SIDES];
        for (int k = 0; k < HTW_SIDES; ++k)
        {
            floorPlate[k] = HtwCorner(k, HTW_WELL, HTW_FLOOR_Y);
        }
        PlanarPolygon(mesh, floorPlate, HTW_SIDES, {1, 0, 0}, HTW_DECK);
    }
    for (int k = 0; k < HTW_SIDES; ++k)
    {
        if (k % 2 == 0)
        {
            const Vec3 rib[2] = {HtwCorner(k, HTW_WELL - 0.015f, HTW_FLOOR_Y), HtwCorner(k, HTW_WELL - 0.015f, HTW_DECK_Y)};
            Tube(mesh, rib, 2, 0.014f, 6, HTW_FRAME);
        }
    }
    {
        // The well's rim: a rolled lip, an orange rail on stanchions round the back half.
        constexpr int STEPS = 33;
        Vec3 lip[STEPS];
        for (int step = 0; step < STEPS; ++step)
        {
            const float a = 2 * PI * step / (STEPS - 1);
            lip[step] = {HTW_WELL * facetRadius * sinf(a), HTW_DECK_Y + 0.008f, -HTW_WELL * facetRadius * cosf(a)};
        }
        Tube(mesh, lip, STEPS, 0.014f, 8, HTW_METAL, false);
        constexpr int RAIL_STEPS = 13;
        Vec3 rail[RAIL_STEPS];
        for (int step = 0; step < RAIL_STEPS; ++step)
        {
            const float a = PI * 0.5f + PI * step / (RAIL_STEPS - 1); // from the right, round the back, to the left
            rail[step] = {1.05f * sinf(a), HTW_DECK_Y + 0.30f, -1.05f * cosf(a)};
            if (step % 3 == 0)
            {
                const Vec3 post[2] = {{rail[step].x, HTW_DECK_Y, rail[step].z}, rail[step]};
                Tube(mesh, post, 2, 0.012f, 6, HTW_METAL);
            }
        }
        Tube(mesh, rail, RAIL_STEPS, 0.018f, 8, HTW_ACCENT);
    }
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 stile[2] = {{side * 0.20f, HTW_FLOOR_Y, 0.92f}, {side * 0.20f, HTW_DECK_Y + 0.35f, 0.92f}};
        Tube(mesh, stile, 2, 0.016f, 8, HTW_METAL);
    }
    for (int rung = 0; rung < 4; ++rung)
    {
        const float y = HTW_FLOOR_Y + 0.20f + rung * 0.19f;
        const Vec3 bar[2] = {{-0.20f, y, 0.92f}, {0.20f, y, 0.92f}};
        Tube(mesh, bar, 2, 0.012f, 8, HTW_METAL, false);
    }
    mesh.part = 0;
    // ---- The seat on its pedestal ----
    const SeatLook seat = {0.22f, 0.50f, 0.16f, false, false, HTW_PAD, HTW_FRAME, HTW_METAL, HTW_ACCENT, HTW_DARK};
    Seat(mesh, eye, HTW_FLOOR_Y, seat);
    // ---- The gunnery desk ahead: a sloping top over the well's front from 3.24 (near edge, z -0.40) to 3.38 (far,
    // z -0.92), 1.48 m wide; on it the keyboard and its end panels along the near edge, the two knob panels at the
    // wings; under the near edge an apron with the switch strip and button grid; two grips on arms from the apron
    // (GunnerSeat's grips: x +-0.22, y 3.33, z -0.32, leaning ahead 15 deg); the viewscreen on a neck and two scopes ----
    {
        constexpr float HALF = 0.74f, NEAR_Z = -0.40f, FAR_Z = -0.92f, NEAR_Y = 3.24f, FAR_Y = 3.38f, APRON = 0.15f;
        const Vec3 nearLeft = {-HALF, NEAR_Y, NEAR_Z}, nearRight = {HALF, NEAR_Y, NEAR_Z};
        const Vec3 farLeft = {-HALF, FAR_Y, FAR_Z}, farRight = {HALF, FAR_Y, FAR_Z};
        PlanarQuad(mesh, nearLeft, nearRight, farRight, farLeft, HTW_PLATE);
        const Vec3 drop = {0, -APRON, 0};
        PlanarQuad(mesh, nearLeft + drop, nearRight + drop, nearRight, nearLeft, HTW_PLATE);
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 n = side < 0 ? nearLeft : nearRight, f = side < 0 ? farLeft : farRight;
            PlanarQuad(mesh, n + drop, n, f, Vec3{f.x, HTW_DECK_Y, f.z}, HTW_PLATE);
        }
        const Vec3 nose[2] = {nearLeft, nearRight};
        Tube(mesh, nose, 2, 0.016f, 8, HTW_FRAME);
        const Vec3 back[2] = {farLeft, farRight};
        Tube(mesh, back, 2, 0.012f, 6, HTW_FRAME);
        // The top's own frame (x right, y up the slope, z out of it).
        const Vec3 slope = Normalized(farLeft - nearLeft);
        const Frame top = {(nearLeft + nearRight) * 0.5f, {1, 0, 0}, slope, Cross({1, 0, 0}, slope)};
        const float keyboardHalf = MetersV(mesh, HTW_KEYBOARD) * 0.5f;
        HtwPicture(mesh, Offset(top, 0, 0.012f + keyboardHalf, 0.003f), HTW_KEYBOARD);
        for (int side = -1; side <= 1; side += 2)
        {
            HtwPicture(mesh, Offset(top, side * (MetersU(mesh, HTW_KEYBOARD) * 0.5f + 0.155f), 0.012f + keyboardHalf, 0.003f), HTW_KEYSIDE);
            if (side > 0)
            {
                HtwPicture(mesh, Offset(top, side * 0.50f, 0.012f + 2 * keyboardHalf + 0.01f + MetersV(mesh, HTW_KNOBS_R) * 0.5f, 0.003f), HTW_KNOBS_R);
            }
        }
        const Frame apron = {{0, NEAR_Y - APRON * 0.5f, NEAR_Z}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        HtwPicture(mesh, Offset(apron, -0.25f, 0, 0.003f), HTW_SWITCHES);
        HtwPicture(mesh, Offset(apron, 0.25f, 0, 0.003f), HTW_BUTTONS);
        // Hazard stripes on the apron's ends.
        for (int side = -1; side <= 1; side += 2)
        {
            for (int stripe = 0; stripe < 3; ++stripe)
            {
                const float x = side * (0.56f + stripe * 0.05f);
                const Vec3 a = {x - 0.012f, NEAR_Y - 0.01f, NEAR_Z + 0.003f}, b = {x + 0.012f, NEAR_Y - 0.01f, NEAR_Z + 0.003f};
                const Vec3 c = {x - 0.018f, NEAR_Y - APRON + 0.01f, NEAR_Z + 0.003f}, d = {x - 0.042f, NEAR_Y - APRON + 0.01f, NEAR_Z + 0.003f};
                PlanarQuad(mesh, d, c, b, a, HTW_YELLOW);
            }
        }
        // Grips on arms from the apron.
        for (int side = -1; side <= 1; side += 2)
        {
            // The grip's middle (0.144 m up its axis) on GunnerSeat's grip point.
            const float lean = 15.0f * PI / 180.0f;
            const Vec3 axis = {0, cosf(lean), -sinf(lean)};
            const Vec3 base = Vec3{side * 0.22f, 3.33f, -0.32f} - axis * 0.144f;
            const Vec3 arm[3] = {{side * 0.22f, NEAR_Y - 0.07f, NEAR_Z + 0.005f}, {side * 0.22f, base.y - 0.06f, -0.36f}, base - axis * 0.026f};
            Tube(mesh, arm, 3, 0.016f, 10, HTW_FRAME);
            HtwGrip(mesh, base, lean);
            IndicatorLamp(mesh, Upright(Offset(apron, side * 0.50f, 0.045f, 0.003f)), 0.008f, HTW_METAL, side < 0 ? HTW_GREEN : HTW_LAMP);
        }
        // The minimap (idren2023 2026-10-21: a small Star Wars CRT on the desk, at 40 deg, facing the gunner): on the
        // left wing, HtwCrt; the score is left out of this seat.
        HtwCrt(mesh, top, -0.50f, 0.012f + 2 * keyboardHalf + 0.01f + MetersV(mesh, HTW_KNOBS_R) * 0.5f, eye);
        // A scope on the desk's back edge right of the viewscreen, in a hood leaning back toward the eye.
        {
            constexpr int side = 1;
            const Vec3 at = {side * 0.47f, 3.62f, -0.84f};
            const Frame scope = MakeFrame(at, {1, 0, 0}, Normalized(Vec3{0, 1, 0.35f}));
            Box(mesh, Offset(scope, 0, 0, -0.05f), {0.16f, 0.15f, 0.05f}, 0.012f, HTW_PLATE);
            Box(mesh, Offset(scope, 0, 0.15f, 0.01f), {0.16f, 0.006f, 0.06f}, 0.003f, HTW_FRAME); // hood
            HtwPicture(mesh, Offset(scope, 0, 0, 0.002f), HTW_SCOPE_C);
            const Vec3 leg[2] = {{at.x, FAR_Y - 0.01f, FAR_Z + 0.03f}, {at.x, at.y - 0.14f, at.z - 0.04f}};
            Tube(mesh, leg, 2, 0.02f, 8, HTW_FRAME);
        }
        // The viewscreen: its housing on a neck from the desk, the viewfinder art over the gun's view.
        const Frame face = MakeFrame({0, HTW_SCREEN_Y, HTW_SCREEN_Z}, {1, 0, 0}, up);
        Box(mesh, Offset(face, 0, 0, -0.045f), {HTW_ART_HALF + 0.025f, HTW_ART_HALF + 0.025f, 0.045f}, 0.012f, HTW_PLATE);
        const Vec3 neck[3] = {{0, FAR_Y - 0.01f, -0.88f}, {0, 3.45f, -0.70f}, {0, 3.55f, -0.52f}};
        Tube(mesh, neck, 3, 0.035f, 10, HTW_FRAME);
        Quad(mesh,
             face.At(-0.205f, 0.127f - 0.0045f, 0.002f),
             face.At(0.205f, 0.127f - 0.0045f, 0.002f),
             face.At(0.205f, -0.127f - 0.0045f, 0.002f),
             face.At(-0.205f, -0.127f - 0.0045f, 0.002f),
             HTW_PICTURE);
        Picture(mesh, Offset(face, 0, 0, 0.003f), HTW_ART_HALF, HTW_ART_HALF, HTW_SCREEN_ART);
    }
    // ---- Racks on the deck: the two ahead carry the HUD (health left, ammunition right, as the AAT cabin's
    // screens), abeam the knobs and the third scope; lockers and power cells behind ----
    HtwRack(mesh, 13, GROUP_HEALTH, HTW_SWITCHES);
    HtwRack(mesh, 2, GROUP_AMMUNITION, HTW_BUTTONS);
    HtwRack(mesh, 11, -1, HTW_KNOBS_L);
    HtwRack(mesh, 4, -1, HTW_KNOBS_R);
    for (int k : {7, 8})
    {
        const Frame wall = Offset(HtwFacet(k, HTW_WALL * facetRadius, HTW_DECK_Y + 0.27f), 0, 0, 0.01f);
        Locker(mesh, wall, 0.22f, 0.24f, 0.13f, detail);
    }
    for (int k : {9, 6})
    {
        CellRack(mesh, Offset(HtwFacet(k, HTW_WALL * facetRadius, HTW_DECK_Y + 0.30f), 0, 0, 0.01f), 2, 2, detail);
    }
    // ---- The gun's breech through the front wall (part 1: pitches with the gun about 0 3.58 -1.46): its housing in
    // the bunker panelling, two recoil cylinders, a band of hazard yellow, a feed cable to the wall ----
    mesh.part = 1;
    {
        const Frame breech = MakeFrame({0, 3.58f, -1.20f}, {1, 0, 0}, up);
        Box(mesh, breech, {0.20f, 0.15f, 0.26f}, 0.03f, HTW_PANELLING);
        Box(mesh, Offset(breech, 0, 0, 0.27f), {0.14f, 0.10f, 0.02f}, 0.01f, HTW_FRAME);
        Box(mesh, Offset(breech, 0, 0, 0.10f), {0.205f, 0.155f, 0.03f}, 0.008f, HTW_YELLOW);
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 recoil[2] = {{side * 0.13f, 3.72f, -1.45f}, {side * 0.13f, 3.72f, -0.98f}};
            Tube(mesh, recoil, 2, 0.035f, 12, HTW_METAL);
        }
    }
    mesh.part = 0;
    {
        const Vec3 cable[4] = {{0.20f, 3.50f, -1.10f}, {0.45f, 3.42f, -1.08f}, {0.80f, 3.45f, -1.10f}, {1.05f, 3.55f, -1.02f}};
        CableLoom(mesh, cable, 4, 3, 0.012f, {0, 1, 0}, HTW_RUBBER, HTW_FRAME, 0.25f);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// X-wing (PLAN starfighters, idren2023 2026-10-27: full cockpit interiors as in the films, the HUD in the dashboard as if
// it had always been there). Our own cockpit at the size of the real one (docs/research/starfighter-cockpits-canon.md:
// a narrow tub, the dash about 0.6 m ahead of the eyes under a glare hood, the canopy's front pane and side panes on a
// grey frame, the nose deck ahead, the astromech's dome behind the head); it replaces the game's first-person model
// (all_1st_cockpit_xwing, a flat-screen model twice the size, its dash 1.5 m ahead), whose texture it uses. The stick,
// throttle and take-off lever (Flight) stand on its pedestal and left console. Space: the cockpit model's, the eye at
// the game's (0, 1.5, 1.5).

enum XwgFinish
{
    XWG_DASH,      // all_1st_cockpit_xwing: the main dash panel (lamps, switches, the targeting screen), whole
    XWG_SIDE,      //   the left side panel (switch banks), whole
    XWG_WIRING,    //   the open wiring bay, whole
    XWG_DIALS,     //   the twin dials unit, whole
    XWG_VENTS,     //   ribbed wall (vents), by size
    XWG_PLATE,     //   worn floor plate, by size
    XWG_RAIL,      //   the cable rail band, by size
    XWG_GRILLE,    //   grille band, by size
    XWG_HULL,      // all_fly_xwing: the exterior (nose deck), by size
    XWG_GLASS,     // all_cockpit_xwingwin: the canopy's glass
    XWG_FRAME,     // canopy frame and struts (grey)
    XWG_DARK,      // recesses, seams, screen backs
    XWG_METAL,     // bright metal
    XWG_RUBBER,    // boots, grips
    XWG_PAD,       // seat padding
    XWG_ORANGE,    // Rebel flight-suit orange accents
    XWG_DOME,      // the astromech's dome (white)
    XWG_DOME_BLUE, // its blue panels
    XWG_LAMP,      // amber lamps
    XWG_RED,       // red lamps
    XWG_SCREEN,    // a screen's dark glass under the HUD
    XWG_STOCK,     // the ship's own exterior (StockExterior), at its model's texture positions
    XWG_PLATE_GREY, // kitbash plates (mid grey)
    XWG_GREEN,     // green lamps
    XWG_WHITE,     // white lamps
    XWG_HOSE,      // the corrugated ducts (dull silver)
    XWG_COCKPIT,   // the game's first-person cockpit pieces (StockPieces), their own mapping
    XWG_FX,        //   their lamps (effects texture)
    XWG_BLACK,     // black inset plates, the glare deck, the columns, the viewer
    XWG_RED_LINE,  // the red outline boxes
    XWG_CREAM,     // white/cream push buttons and pin lines
    XWG_BLUE,      // blue buttons
    XWG_PALE,      // the viewer's pale readout strip
    XWG_BRASS,     // the column's brass tip
    XWG_LIGHT_GREY, // the machinery boxes behind the seat
    XWG_WARM_GREY, // the Y-wing's dark warm grey walls
    XWG_YELLOW,    // the Y-wing's yellow lens
    XWG_RED_SCREEN, // the Y-wing's red targeting screen
    XWG_PICTURE,    // the Y-wing gunner's display: the scope pass's picture (self-lit)
    XWG_SCREEN_FRAME,   // the HUD screens' stock frame (texture 5, all_1st_cockpit_securitycam: the tanks' look)
    XWG_SCREEN_GLASS,   //   their glass (scan lines)
    XWG_SCREEN_BACKING, //   behind the glass
    XWG_FINISH_COUNT
};

// ---- Starfighter kit (idren2023 2026-10-27: every starfighter cockpit to about 50 000 triangles, true to the films and to
// the ship's own exterior; docs/research/starfighter-cockpits-film.md) ----

/// The finishes a starfighter's kitbashed panels are made in (the original trilogy's cockpits were built from aircraft
/// surplus: rows of toggles, coloured push buttons, small dials and screens on grey plates).
struct FighterLook
{
    int plate, frame, metal, dark, screen;
    int lamps[4]; // the push buttons' and lamps' colours
};

// A small fixed random sequence: a panel's layout is the same in every build.
struct Kit
{
    uint32_t state;
    float Next()
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
    }
    int Pick(int count)
    {
        const int k = static_cast<int>(Next() * static_cast<float>(count));
        return k < count ? k : count - 1;
    }
};

DetailLook AsDetail(const FighterLook& look, int lamp)
{
    return {look.plate, look.frame, look.metal, look.dark, lamp, look.plate};
}

// One module of a kitbashed panel in its cell (the frame's x-y plane, z out of the panel; half sizes), its kind and
// colours from the sequence: a toggle bank, a grid of push buttons, knobs, a dial, a small screen, slot lamps, a
// louvred vent or a screwed cover plate.
void KitModule(Mesh& mesh, const Frame& cell, float halfWidth, float halfHeight, Kit& kit, const FighterLook& look)
{
    const int lamp = look.lamps[kit.Pick(4)];
    const float least = halfWidth < halfHeight ? halfWidth : halfHeight;
    switch (kit.Pick(9))
    {
    case 0:
    {
        // toggles: a guard strip, a bat lever leaning up from each boss, a lamp over every other one
        const int count = static_cast<int>(halfWidth * 2 / 0.022f);
        if (count >= 2 && halfHeight > 0.016f)
        {
            Box(mesh, Offset(cell, 0, 0, 0.002f), {halfWidth - 0.002f, halfHeight - 0.002f, 0.002f}, 0, look.frame);
            for (int k = 0; k < count && k < 8; ++k)
            {
                const float x = (k - (count - 1) * 0.5f) * 0.022f;
                Box(mesh, Offset(cell, x, -0.004f, 0.006f), {0.005f, 0.007f, 0.002f}, 0, look.dark);
                const Vec3 bat[2] = {cell.At(x, -0.004f, 0.007f), cell.At(x, 0.002f, 0.019f)};
                Tube(mesh, bat, 2, 0.0018f, 4, look.metal);
                if (k % 2 == 0 && halfHeight > 0.02f)
                {
                    Box(mesh, Offset(cell, x, halfHeight - 0.006f, 0.0045f), {0.003f, 0.003f, 0.0015f}, 0, lamp);
                }
            }
            return;
        }
        break;
    }
    case 1:
    case 2:
    {
        // a grid of square push buttons, most of them lit
        int columns = static_cast<int>(halfWidth * 2 / 0.021f), rows = static_cast<int>(halfHeight * 2 / 0.021f);
        columns = columns < 1 ? 1 : columns > 6 ? 6 : columns;
        rows = rows < 1 ? 1 : rows > 3 ? 3 : rows;
        Box(mesh, Offset(cell, 0, 0, 0.002f), {halfWidth - 0.002f, halfHeight - 0.002f, 0.002f}, 0, look.dark);
        for (int r = 0; r < rows; ++r)
        {
            for (int c = 0; c < columns; ++c)
            {
                const float x = (c - (columns - 1) * 0.5f) * 0.021f, y = (r - (rows - 1) * 0.5f) * 0.021f;
                const int finish = kit.Next() < 0.3f ? look.frame : look.lamps[kit.Pick(4)];
                Box(mesh, Offset(cell, x, y, 0.0065f), {0.0075f, 0.0075f, 0.0035f}, 0, finish);
            }
        }
        return;
    }
    case 3:
    {
        // rotary knobs: a stepped cap with a lit pointer
        const float radius = least * 0.55f > 0.011f ? 0.011f : least * 0.55f;
        const int count = static_cast<int>(halfWidth * 2 / (radius * 3.0f));
        if (count >= 1 && radius > 0.004f)
        {
            for (int k = 0; k < count && k < 5; ++k)
            {
                const Frame at = Offset(cell, (k - (count - 1) * 0.5f) * radius * 3.0f, 0, 0);
                const float knob[4][2] = {{0, radius * 1.2f}, {0.003f, radius}, {0.012f, radius * 0.85f}, {0.014f, 0}};
                Lathe(mesh, Upright(at), knob, 4, 8, look.frame);
                Box(mesh, Offset(at, 0, radius * 0.45f, 0.0142f), {0.0012f, radius * 0.4f, 0.0005f}, 0, lamp);
            }
            return;
        }
        break;
    }
    case 4:
    {
        // a round dial: a bright bezel, a dark face, lit ticks and a needle
        const float radius = least - 0.004f;
        if (radius > 0.01f)
        {
            const Frame dial = Upright(cell);
            const float bezel[4][2] = {{0, radius}, {0.005f, radius}, {0.006f, radius * 0.82f}, {0.003f, radius * 0.8f}};
            Lathe(mesh, dial, bezel, 4, 12, look.metal);
            const float face[2][2] = {{0.003f, radius * 0.8f}, {0.003f, 0}};
            Lathe(mesh, dial, face, 2, 12, look.dark);
            for (int k = 0; k < 5; ++k)
            {
                const float a = -2.0f + k * 1.0f;
                Box(mesh, Offset(cell, sinf(a) * radius * 0.65f, cosf(a) * radius * 0.65f, 0.0034f), {0.0009f, 0.0009f, 0.0003f}, 0, lamp);
            }
            const float a = -1.5f + kit.Next() * 3.0f;
            const Vec3 needle[2] = {cell.At(0, 0, 0.0036f), cell.At(sinf(a) * radius * 0.6f, cosf(a) * radius * 0.6f, 0.0036f)};
            Tube(mesh, needle, 2, 0.0008f, 3, lamp);
            return;
        }
        break;
    }
    case 5:
    {
        // a small screen: a dark bezel, the glass with a few lit lines
        if (halfWidth > 0.02f && halfHeight > 0.014f)
        {
            Box(mesh, Offset(cell, 0, 0, 0.004f), {halfWidth - 0.002f, halfHeight - 0.002f, 0.004f}, 0.002f, look.dark);
            const Frame glass = Offset(cell, 0, 0, 0.0082f);
            const float w = halfWidth - 0.007f, h = halfHeight - 0.007f;
            Quad(mesh, glass.At(-w, h, 0), glass.At(w, h, 0), glass.At(w, -h, 0), glass.At(-w, -h, 0), look.screen);
            const int lines = 2 + kit.Pick(3);
            for (int k = 0; k < lines; ++k)
            {
                const float y = -h + (k + 1) * (2 * h / (lines + 1));
                const float length = w * (0.3f + 0.6f * kit.Next());
                Box(mesh, Offset(glass, -w + length + 0.004f, y, 0.0004f), {length, 0.0007f, 0.0002f}, 0, lamp);
            }
            return;
        }
        break;
    }
    case 6:
    {
        // slot lamps in a dark strip
        Box(mesh, Offset(cell, 0, 0, 0.002f), {halfWidth - 0.002f, halfHeight - 0.002f, 0.002f}, 0, look.dark);
        const int count = static_cast<int>(halfWidth * 2 / 0.014f);
        for (int k = 0; k < count && k < 10; ++k)
        {
            const float x = (k - (count - 1) * 0.5f) * 0.014f;
            const int finish = kit.Next() < 0.4f ? look.frame : look.lamps[kit.Pick(4)];
            Box(mesh, Offset(cell, x, 0, 0.0048f), {0.0035f, halfHeight * 0.6f, 0.0012f}, 0, finish);
        }
        return;
    }
    case 7:
    {
        // a louvred vent
        const int count = static_cast<int>(halfHeight * 2 / 0.009f);
        Box(mesh, Offset(cell, 0, 0, 0.0015f), {halfWidth - 0.002f, halfHeight - 0.002f, 0.0015f}, 0, look.dark);
        for (int k = 0; k < count && k < 10; ++k)
        {
            const float y = (k - (count - 1) * 0.5f) * 0.009f;
            Box(mesh, Offset(cell, 0, y, 0.0045f), {halfWidth - 0.005f, 0.002f, 0.0015f}, 0, look.frame);
        }
        return;
    }
    default:
        break;
    }
    // a screwed cover plate
    Box(mesh, Offset(cell, 0, 0, 0.004f), {halfWidth - 0.002f, halfHeight - 0.002f, 0.004f}, 0.0025f, look.frame);
    if (halfWidth > 0.012f && halfHeight > 0.012f)
    {
        for (int k = 0; k < 4; ++k)
        {
            const float x = (k & 1 ? 1 : -1) * (halfWidth - 0.008f), y = (k & 2 ? 1 : -1) * (halfHeight - 0.008f);
            Box(mesh, Offset(cell, x, y, 0.0085f), {0.002f, 0.002f, 0.0006f}, 0, look.metal);
        }
    }
}

// A kitbashed panel on a face (the frame's x-y plane, z out): a plate with a raised border, filled row by row with
// modules of random widths (the same in every build for a seed).
void KitPanel(Mesh& mesh, const Frame& face, float halfWidth, float halfHeight, uint32_t seed, const FighterLook& look)
{
    Panel(mesh, face, halfWidth, halfHeight, 0.008f, 0.004f, look.frame, look.plate, 0, look.metal);
    Kit kit{seed * 2654435761u + 12345u};
    constexpr float INSET = 0.011f;
    float top = halfHeight - INSET;
    while (top > -halfHeight + INSET + 0.018f)
    {
        float rowHeight = 0.028f + kit.Next() * 0.04f;
        if (top - rowHeight < -halfHeight + INSET + 0.018f)
        {
            rowHeight = top + halfHeight - INSET;
        }
        float left = -halfWidth + INSET;
        while (left < halfWidth - INSET - 0.018f)
        {
            float width = 0.035f + kit.Next() * 0.09f;
            if (left + width > halfWidth - INSET - 0.03f)
            {
                width = halfWidth - INSET - left;
            }
            // 1.5 mm off the plate: the modules' backs on its face would fight it for depth (b417 speckle)
            KitModule(
                mesh, Offset(face, left + width * 0.5f, top - rowHeight * 0.5f, 0.0015f), width * 0.5f - 0.002f, rowHeight * 0.5f - 0.002f, kit, look);
            left += width;
        }
        top -= rowHeight;
    }
}

// A kitbashed panel between four corners (a, b along its top, then c, d along its bottom; the face turned to the
// side the corners go round clockwise from): its frame at the middle, sized to the shorter of the opposite edges.
void KitPanelBetween(Mesh& mesh, Vec3 a, Vec3 b, Vec3 c, Vec3 d, uint32_t seed, const FighterLook& look)
{
    const Vec3 middle = (a + b + c + d) * 0.25f;
    const Vec3 across = Normalized((b - a) + (c - d));
    const Vec3 up = (a + b) * 0.5f - (c + d) * 0.5f;
    const float halfWidth = 0.5f * (Length(b - a) < Length(c - d) ? Length(b - a) : Length(c - d));
    KitPanel(mesh, MakeFrame(middle, across, up), halfWidth, 0.5f * Length(up), seed, look);
}

// A corrugated hose along the points (smoothed): a tube with a raised ring every 3 cm.
void CorrugatedHose(Mesh& mesh, const Vec3* points, int count, float radius, int hoseFinish, int ringFinish)
{
    Vec3 smooth[96];
    const int n = Smooth(points, count, 8, smooth, 96);
    Tube(mesh, smooth, n, radius, 8, hoseFinish, false);
    float next = 0.015f, along = 0;
    for (int i = 0; i + 1 < n; ++i)
    {
        const Vec3 step = smooth[i + 1] - smooth[i];
        const float length = Length(step);
        if (length < 1e-5f)
        {
            continue;
        }
        const Vec3 t = step * (1.0f / length);
        while (next < along + length)
        {
            const Vec3 p = smooth[i] + t * (next - along);
            const Vec3 ring[2] = {p - t * 0.005f, p + t * 0.005f};
            Tube(mesh, ring, 2, radius * 1.16f, 8, ringFinish, false);
            next += 0.03f;
        }
        along += length;
    }
}

// A finish on one of the X-wing's 512-texel textures at the shells' one density, from its texel rectangle.
constexpr Finish XwgTexture(
    int texture, float x0, float y0, float x1, float y1, uint32_t color, float specular, float power, float glow = 0)
{
    Finish finish = {{x0 / 512.0f, y0 / 512.0f, x1 / 512.0f, y1 / 512.0f}, 1, texture, color, specular, power, glow};
    finish.isDense = true;
    return finish;
}

const Finish XWG_FINISHES[XWG_FINISH_COUNT] = {
    XwgTexture(1, 0, 0, 512, 150, 0x8E9296, 0.20f, 14, 0.10f),     // main dash
    XwgTexture(1, 0, 150, 146, 360, 0x8E9296, 0.20f, 14, 0.08f),   // side panel
    XwgTexture(1, 150, 168, 316, 360, 0x8A8E92, 0.15f, 12),        // wiring bay
    XwgTexture(1, 318, 168, 388, 270, 0x8E9296, 0.25f, 16, 0.05f), // dials
    XwgTexture(1, 388, 160, 512, 360, 0x8A8E92, 0.15f, 12),        // vents
    XwgTexture(1, 0, 362, 512, 420, 0x868A8E, 0.15f, 12),          // plate
    XwgTexture(1, 0, 420, 512, 476, 0x8A8E92, 0.20f, 14),          // cable rail
    XwgTexture(1, 0, 478, 512, 512, 0x8A8E92, 0.20f, 14),          // grille
    XwgTexture(3, 0, 0, 256, 256, 0x9A9EA2, 0.25f, 16),            // hull
    {{0, 0, 1, 1}, 1, 4, 0x98B0C4, 0.60f, 40, 0, 0.99f},           // glass: the window texture, its alpha
    XwgTexture(1, 0, 362, 512, 420, 0xAEB7BC, 0.30f, 18), // frame: the stock art, tinted
    XwgTexture(1, 0, 362, 512, 420, 0x23262A, 0.05f, 6), // dark: the stock art, tinted
    {{0, 0, 1, 1}, 1, 0, 0x9CA0A4, 0.60f, 40, 0},                  // bright metal
    XwgTexture(1, 0, 362, 512, 420, 0x33363A, 0.20f, 10), // rubber: the stock art, tinted
    XwgTexture(1, 0, 362, 512, 420, 0x303133, 0.20f, 12), // seat padding (black): the stock art, tinted
    {{0, 0, 1, 1}, 1, 0, 0xC0581C, 0.20f, 12, 0},                  // orange
    {{0, 0, 1, 1}, 1, 0, 0xD6DADE, 0.45f, 30, 0},                  // dome white
    {{0, 0, 1, 1}, 1, 0, 0x2A4C9C, 0.45f, 30, 0},                  // dome blue
    {{0, 0, 1, 1}, 1, 0, 0xFFB050, 0, 1, 1.0f},                    // amber lamp
    {{0, 0, 1, 1}, 1, 0, 0xFF3020, 0, 1, 1.0f},                    // red lamp
    {{0, 0, 1, 1}, 1, 0, 0x0C1410, 0.50f, 40, 0.05f},              // screen glass
    {{0, 0, 1, 1}, 1, 3, 0x808080, 0.20f, 14, 0},                  // the exterior, its own mapping
    XwgTexture(1, 0, 362, 512, 420, 0xA4A8AE, 0.25f, 16),          // kitbash plate: the stock's grimy plate art (idren2023: no flat colours)
    {{0, 0, 1, 1}, 1, 0, 0x60FF70, 0, 1, 1.0f},                    // green lamp
    {{0, 0, 1, 1}, 1, 0, 0xE8F0FF, 0, 1, 1.0f},                    // white lamp
    XwgTexture(1, 0, 420, 512, 476, 0xADB7C3, 0.35f, 24), // hose: the stock art, tinted
    {{0, 0, 1, 1}, 1, 1, 0x808080, 0.25f, 16, 0},                  // stock cockpit pieces
    {{0, 0, 1, 1}, 1, 2, 0xC0C0C0, 0, 1, 1.0f},                    // their lamps
    XwgTexture(1, 0, 362, 512, 420, 0x222222, 0.25f, 16), // black: the stock art, tinted
    {{0, 0, 1, 1}, 1, 0, 0xB3261E, 0.20f, 12, 0},                  // red outline
    {{0, 0, 1, 1}, 1, 0, 0xE8E4D8, 0.30f, 18, 0.15f},              // cream buttons, pin lines
    {{0, 0, 1, 1}, 1, 0, 0x3060E0, 0, 1, 1.0f},                    // blue button
    {{0, 0, 1, 1}, 1, 0, 0xD8F0A0, 0, 1, 0.8f},                    // pale readout
    {{0, 0, 1, 1}, 1, 0, 0xB08A4A, 0.60f, 36, 0},                  // brass
    XwgTexture(1, 0, 362, 512, 420, 0xFFFFFF, 0.30f, 18), // light grey: the stock art, tinted
    XwgTexture(1, 0, 362, 512, 420, 0x8C8678, 0.20f, 12),          // warm grey: the grimy plate art, warmer
    {{0, 0, 1, 1}, 1, 0, 0xFFE040, 0, 1, 1.0f},                    // yellow lens
    {{0, 0, 1, 1}, 1, 0, 0xFF4060, 0, 1, 0.8f},                    // red screen
    {{0, 0.3f, 1, 0.7f}, 1, 6, 0xFFFFFF, 0, 1, 1.0f},              // the gunner's display picture
    ScreenFrameFinish(),                                                    // stock screen frame
    ScreenGlassFinish(ALL_SCREEN_ART),                                      // its glass
    ScreenBackingFinish(ALL_SCREEN_ART),                                    // behind the glass
};

// The HUD screens in the tanks' look (idren2023 2026-11-09: every vehicle's HUD as the tanks').
const StockScreenLook XWG_SCREENS = {&ALL_SCREEN_ART, XWG_SCREEN_FRAME, XWG_SCREEN_GLASS, XWG_SCREEN_BACKING, XWG_DARK};

constexpr Vec3 XWG_EYE = {0.0f, 1.5f, 1.5f};
/// The tub: floor, the canopy's sill, the walls' half width, the rear bulkhead; the consoles' tops and inner edges
/// (Flight's throttle and lever stand on the left one, rest-space (-0.24, -0.62) and (-0.36, -0.62) from the eye).
constexpr float XWG_FLOOR = 0.62f, XWG_SILL = 1.26f, XWG_HALF = 0.46f, XWG_BACK = 2.08f;
constexpr float XWG_CONSOLE_Y = 0.88f, XWG_CONSOLE_IN = 0.19f, XWG_CONSOLE_FRONT = 1.04f;
/// The dash's centre panel (the DK cutaway and the studio replica: a flat centre panel, wing panels angled back to the
/// canopy's A-pillars either side): its half width, its top and bottom edges (y, z).
constexpr float XWG_DASH_HALF = 0.18f, XWG_DASH_TOP_Y = 1.36f, XWG_DASH_TOP_Z = 0.88f, XWG_DASH_LOW_Y = 1.06f, XWG_DASH_LOW_Z = 0.94f;
/// The flight stick's pivot (Flight's STICK_PIVOT, rest-space (0, -0.86, -0.34): idren2023 2026-10-27, a centre stick
/// between the legs on its own floor plate, LeverModel::JetStick); the cockpits with a lower floor raise a pedestal to it.
constexpr Vec3 XWG_STICK_BASE = {0.0f, 0.64f, 1.16f};

// A strut of a canopy's frame between two points.
void XwgStrutOf(Mesh& mesh, Vec3 a, Vec3 b, float radius, int finish)
{
    const Vec3 path[2] = {a, b};
    Tube(mesh, path, 2, radius, 8, finish);
}

void XwgStrut(Mesh& mesh, Vec3 a, Vec3 b, float radius)
{
    XwgStrutOf(mesh, a, b, radius, XWG_FRAME);
}

// A glass pane (three or four corners in order round it), the window texture spread over it once and evenly (both
// triangles of a four-cornered pane share its corners' places in the texture, so no seam shows).
void XwgPaneOf(Mesh& mesh, const Vec3* points, int count, int finish)
{
    const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (int i = 1; i + 1 < count; ++i)
    {
        const float st[3][2] = {
            {corners[0][0], corners[0][1]},
            {corners[i][0], corners[i][1]},
            {corners[i + 1][0], corners[i + 1][1]}};
        Triangle(mesh, points[0], points[i], points[i + 1], st, finish);
    }
}

void XwgPane(Mesh& mesh, const Vec3* points, int count)
{
    XwgPaneOf(mesh, points, count, XWG_GLASS);
}

// A screen let into a panel (x, y on the face, metres): a dark bezel, the glass, and the HUD group shown on it.
void XwgScreen(Mesh& mesh, const Frame& face, float x, float y, float halfWidth, float halfHeight, int group)
{
    // the tanks' stock screen (idren2023 2026-11-09), in a case let into the panel
    const Frame glass = Offset(face, x, y, 0.004f);
    StockScreen(mesh, glass, halfWidth, halfHeight, SCREEN_TEXEL_METERS * halfHeight / 0.0675f, XWG_SCREENS, 0.012f);
    SetDisplayScreen(&XWG_SCREENS);
    AddDisplay(group, glass, 0, 0, halfWidth * 1.9f, halfHeight * 1.9f);
    SetDisplayScreen(nullptr);
}


// A starfighter's own exterior round its cockpit (tools/stockexterior.py: the ship's model from the mod tools, moved
// so the eye sits in its canopy; its own cockpit inside left out): the hull on the exterior's texture (3) at the
// model's own texture positions, its glass as the design's glass. Without the mod tools at build time: nothing.
void StockExterior(Mesh& mesh, const StockVertex* table, int count, int hullFinish, int glassFinish, int capFinish = -1,
                   int liningFinish = -1, float liningReach = 0, float liningAbove = 1e9f)
{
    for (int i = 0; i + 2 < count; i += 3)
    {
        if (liningFinish >= 0 && table[i].texture != 4 && table[i].texture != 5)
        {
            // a lining under the hull seen from inside (2026-11-09 texturing pass, idren2023: the snowspeeder's canopy
            // frames showed the hull's outer paint, its squadron letters, from the seats; then "the canopy needs UV
            // fixing": the frames' sides kept the hull's streaked mapping): within reach of the eye, a hull face turned
            // away from it, or any above liningAbove (the canopy's frame over the sill), gets a copy 4 mm toward the
            // eye in the lining's finish, mapped by its real size
            const Vec3 p0 = table[i].position, p1 = table[i + 1].position, p2 = table[i + 2].position;
            const Vec3 centre = (p0 + p1 + p2) * (1.0f / 3.0f);
            const Vec3 normal = table[i].normal + table[i + 1].normal + table[i + 2].normal;
            const Vec3 toEye = mesh.eye - centre;
            if ((Dot(normal, toEye) < 0 || centre.y > liningAbove) && Length(toEye) < liningReach &&
                mesh.count + 3 <= mesh.capacity)
            {
                const Vec3 in = Normalized(toEye) * 0.004f;
                const Vec3 lining[3] = {p0 + in, p1 + in, p2 + in};
                // one direction for all of it (along the ship, or across where a face runs along it), so the plating
                // runs on across neighbouring faces instead of turning with each triangle's own edge
                const Vec3 n = Normalized(Cross(p1 - p0, p2 - p0));
                PlanarPolygon(mesh, lining, 3, fabsf(n.z) > 0.7f ? Vec3{1, 0, 0} : Vec3{0, 0, 1}, liningFinish);
            }
        }
        if (mesh.count + 3 > mesh.capacity)
        {
            mesh.isFull = true;
            break;
        }
        if (table[i].texture == 5 && capFinish >= 0)
        {
            // the cap where a cut piece sat: mapped by its real size (it has no mapping of its own; textured since the
            // 2026-11-09 texturing pass)
            const Vec3 cap[3] = {table[i].position, table[i + 1].position, table[i + 2].position};
            // one direction for all (along the ship, or across where a face runs along it), as the lining
            const Vec3 n = Normalized(Cross(cap[1] - cap[0], cap[2] - cap[0]));
            PlanarPolygon(mesh, cap, 3, fabsf(n.z) > 0.7f ? Vec3{1, 0, 0} : Vec3{0, 0, 1}, capFinish);
            continue;
        }
        for (int k = 0; k < 3; ++k)
        {
            const StockVertex& stock = table[i + k];
            Vertex& vertex = mesh.vertices[mesh.count++];
            vertex.position = stock.position;
            vertex.normal = stock.normal;
            vertex.u = stock.u;
            vertex.v = stock.v;
            vertex.finish = static_cast<uint8_t>(
                stock.texture == 4 ? glassFinish : stock.texture == 5 && capFinish >= 0 ? capFinish : hullFinish);
            vertex.part = mesh.part;
        }
        if (table[i].texture == 4 && glassFinish < 0)
        {
            mesh.count -= 3; // the design uses the stock cockpit's glass instead (StockPieces)
        }
    }
}


// Pieces of the game's own first-person cockpit fitted into ours (tools/stockcockpit.py: idren2023 2026-10-27, the stock
// canopies and instruments at their better texture detail, scaled to the player): on the cockpit's texture (1), its
// effects texture (2) and its glass (4), at the model's own texture positions.
void StockPieces(Mesh& mesh, const StockVertex* table, int count, int cockpitFinish, int effectsFinish, int glassFinish)
{
    for (int i = 0; i + 2 < count; i += 3)
    {
        if (mesh.count + 3 > mesh.capacity)
        {
            mesh.isFull = true;
            break;
        }
        for (int k = 0; k < 3; ++k)
        {
            const StockVertex& stock = table[i + k];
            Vertex& vertex = mesh.vertices[mesh.count++];
            vertex.position = stock.position;
            vertex.normal = stock.normal;
            vertex.u = stock.u;
            vertex.v = stock.v;
            vertex.finish = static_cast<uint8_t>(
                stock.texture == 4 ? glassFinish : stock.texture == 2 ? effectsFinish : cockpitFinish);
            vertex.part = mesh.part;
        }
    }
}

// The X-wing's cockpit inside the given exterior (the Y-wing's pilot sits in the same kind of Rebel tub).
void XwingCockpit(Mesh& mesh, const StockVertex* exterior, int exteriorCount, const StockVertex* stock, int stockCount)
{
    const FighterLook look = {XWG_PLATE_GREY, XWG_FRAME, XWG_METAL, XWG_DARK, XWG_SCREEN, {XWG_LAMP, XWG_RED, XWG_GREEN, XWG_WHITE}};
    const DetailLook detail = AsDetail(look, XWG_LAMP);
    // The ship's own hull round the cockpit: nose, canopy frame and glass, S-foils, engines, the astromech.
    StockExterior(mesh, exterior, exteriorCount, XWG_STOCK, XWG_GLASS); // the hull's own glass in its own frames
    // The game's own gauges on the dash (its canopy no more: the hull's own frame and glass, 2026-11-09).
    StockPieces(mesh, stock, stockCount, XWG_COCKPIT, XWG_FX, XWG_GLASS);
    // The tub: the floor plate, the walls up to the canopy's sill with a padded coaming, the rear bulkhead.
    TiledQuad(mesh, {-XWG_HALF, XWG_FLOOR, 0.80f}, {2 * XWG_HALF, 0, 0}, {0, 0, XWG_BACK - 0.80f}, XWG_PLATE);
    FloorHatch(mesh, {0, XWG_FLOOR + 0.002f, 1.55f}, 0.12f, 0.16f, XWG_FRAME, XWG_PLATE, XWG_METAL);
    for (int side = -1; side <= 1; side += 2)
    {
        const float x = side * XWG_HALF;
        TiledQuad(mesh, {x, XWG_FLOOR, 0.80f}, {0, 0, XWG_BACK - 0.80f}, {0, XWG_SILL - XWG_FLOOR, 0}, XWG_VENTS);
        RoundedBar(mesh, MakeFrame({x - side * 0.015f, XWG_SILL, 1.60f}, {0, 0, 1}, {0, 1, 0}), 0.50f, 0.025f, 0.035f, 0.02f, XWG_PAD);
        // the wall over the console: two kitbashed banks between ribs, facing the cockpit (2026-11-09: the frame's z
        // had pointed into the wall, the banks hidden in it; idren2023: the switches belong on the walls, not the seat)
        const Frame wall = MakeFrame({x - side * 0.004f, 1.07f, 1.52f}, {0, 0, static_cast<float>(side)}, {0, 1, 0});
        KitPanel(mesh, Offset(wall, -0.20f, 0, 0), 0.19f, 0.15f, 11 + side, look);
        KitPanel(mesh, Offset(wall, 0.21f, 0, 0), 0.19f, 0.15f, 21 + side, look);
        for (int k = 0; k < 3; ++k)
        {
            Box(mesh, Offset(wall, -0.405f + k * 0.405f, 0, 0.012f), {0.012f, 0.17f, 0.012f}, 0.004f, XWG_FRAME);
        }
        // the console: a box from the floor, its top with the stock's side art, its leg side kitbashed
        const float inner = side * XWG_CONSOLE_IN, mid = (x + inner) * 0.5f, halfX = fabsf(x - inner) * 0.5f;
        const float halfZ = (XWG_BACK - 0.20f - XWG_CONSOLE_FRONT) * 0.5f, back = XWG_CONSOLE_FRONT + 2 * halfZ;
        Box(mesh,
            MakeFrame({mid, (XWG_CONSOLE_Y + XWG_FLOOR) * 0.5f, XWG_CONSOLE_FRONT + halfZ}, {1, 0, 0}, {0, 1, 0}),
            {halfX, (XWG_CONSOLE_Y - XWG_FLOOR) * 0.5f, halfZ},
            0.01f,
            XWG_DARK);
        const float top = XWG_CONSOLE_Y + 0.001f;
        const Frame deck = MakeFrame({mid, top, XWG_CONSOLE_FRONT + halfZ}, {1, 0, 0}, {0, 0, -1}); // up (side's x: the left one faced down)
        KitPanel(mesh, Offset(deck, 0, -0.17f, 0), halfX - 0.01f, halfZ - 0.18f, 31 + side, look);
        // its leg side plain plating (the switch strips that stood here, beside the seat, moved to the walls)
        TiledQuad(mesh, {inner, XWG_FLOOR, XWG_CONSOLE_FRONT}, {0, 0, 2 * halfZ}, {0, XWG_CONSOLE_Y - XWG_FLOOR, 0},
                  XWG_PLATE);
        // cable looms along the wall under the console's lip
        const Vec3 loom[3] = {{x - side * 0.03f, XWG_CONSOLE_Y + 0.03f, 1.05f}, {x - side * 0.03f, XWG_CONSOLE_Y + 0.025f, 1.50f}, {x - side * 0.03f, XWG_CONSOLE_Y + 0.03f, 1.95f}};
        CableLoom(mesh, loom, 3, 3, 0.007f, {0, 1, 0}, XWG_RUBBER, XWG_FRAME, 0.18f);
    }
    // The left console's front: the throttle's track and the take-off lever's gate (Flight); the right console's front:
    // a keypad.
    Box(mesh, MakeFrame({-0.24f, XWG_CONSOLE_Y + 0.004f, 1.22f}, {1, 0, 0}, {0, 0, -1}), {0.022f, 0.15f, 0.004f}, 0.002f, XWG_FRAME);
    Box(mesh, MakeFrame({-0.24f, XWG_CONSOLE_Y + 0.006f, 1.22f}, {1, 0, 0}, {0, 0, -1}), {0.010f, 0.14f, 0.004f}, 0.002f, XWG_DARK);
    Box(mesh, MakeFrame({-0.36f, XWG_CONSOLE_Y + 0.004f, 1.40f}, {1, 0, 0}, {0, 0, -1}), {0.02f, 0.11f, 0.004f}, 0.002f, XWG_FRAME);
    Box(mesh, MakeFrame({-0.36f, XWG_CONSOLE_Y + 0.006f, 1.40f}, {1, 0, 0}, {0, 0, -1}), {0.009f, 0.10f, 0.004f}, 0.002f, XWG_DARK);
    {
        const Frame pad = MakeFrame({0.31f, XWG_CONSOLE_Y + 0.004f, 1.18f}, {1, 0, 0}, {0, 0.5f, -1});
        Box(mesh, Offset(pad, 0, 0, 0.006f), {0.10f, 0.07f, 0.008f}, 0.003f, XWG_FRAME);
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 5; ++c)
            {
                Box(mesh, Offset(pad, -0.072f + c * 0.036f, 0.045f - r * 0.03f, 0.016f), {0.013f, 0.010f, 0.004f}, 0.002f, (r + c) % 4 == 0 ? XWG_LAMP : XWG_PLATE_GREY);
            }
        }
    }
    // The dash: the centre panel (the stock art strip over the HUD's screens), the wing panels angled back to the
    // walls with their button columns, the column down to the floor between the knees, the glare hood over it all.
    const Vec3 topIn = {XWG_DASH_HALF, XWG_DASH_TOP_Y, XWG_DASH_TOP_Z}, lowIn = {XWG_DASH_HALF, XWG_DASH_LOW_Y, XWG_DASH_LOW_Z};
    const Vec3 topOut = {0.40f, 1.40f, 1.05f}, lowOut = {XWG_HALF - 0.01f, 1.00f, 1.16f};
    {
        const auto mirror = [](Vec3 p) { return Vec3{-p.x, p.y, p.z}; };
        const Frame centre = MakeFrame((topIn + mirror(lowIn)) * 0.5f, {1, 0, 0}, topIn - lowIn);
        const float halfHeight = 0.5f * Length(topIn - lowIn);
        Box(mesh, Offset(centre, 0, 0, -0.03f), {XWG_DASH_HALF, halfHeight, 0.03f}, 0.008f, XWG_FRAME);
        // the art strip along its top
        Quad(mesh, centre.At(-XWG_DASH_HALF + 0.01f, halfHeight - 0.01f, 0.001f), centre.At(XWG_DASH_HALF - 0.01f, halfHeight - 0.01f, 0.001f),
             centre.At(XWG_DASH_HALF - 0.01f, halfHeight - 0.115f, 0.001f), centre.At(-XWG_DASH_HALF + 0.01f, halfHeight - 0.115f, 0.001f), XWG_DASH);
        // (the research: two small CRTs side by side, the small targeting screen under them with a red button and a
        // row of three dials beside it; a row of tiny red lamps along the top edge)
        XwgScreen(mesh, centre, -0.058f, -0.01f, 0.045f, 0.04f, GROUP_MAP);
        XwgScreen(mesh, centre, 0.058f, -0.01f, 0.045f, 0.04f, GROUP_AMMUNITION);
        XwgScreen(mesh, centre, 0.0f, -0.10f, 0.035f, 0.035f, GROUP_TARGET);
        IndicatorLamp(mesh, Upright(Offset(centre, 0.07f, -0.10f, 0.004f)), 0.012f, XWG_METAL, XWG_RED);
        KnobRow(mesh, Offset(centre, -0.095f, -0.10f, 0.002f), 3, 0.025f, 0.008f, detail);
        for (int k = 0; k < 13; ++k)
        {
            IndicatorLamp(mesh, Upright(Offset(centre, -XWG_DASH_HALF + 0.03f + k * 0.0267f, halfHeight - 0.004f, 0.012f)), 0.0028f, XWG_METAL, XWG_RED);
        }
        for (int side = -1; side <= 1; side += 2)
        {
            const auto at = [side](Vec3 p) { return Vec3{p.x * side, p.y, p.z}; };
            const Vec3 a = at(topIn), b = at(topOut), c = at(lowOut), d = at(lowIn);
            // the wing panel's body behind its face, the face: a screen bay at its top, the button columns below
            if (side < 0)
            {
                Quad(mesh, b + Vec3{0, 0, 0.03f}, a + Vec3{0, 0, -0.03f}, d + Vec3{0, 0, -0.03f}, c + Vec3{0, 0, 0.03f}, XWG_FRAME);
            }
            else
            {
                Quad(mesh, a + Vec3{0, 0, -0.03f}, b + Vec3{0, 0, 0.03f}, c + Vec3{0, 0, 0.03f}, d + Vec3{0, 0, -0.03f}, XWG_FRAME);
            }
            const Vec3 aMid = Lerp(a, d, 0.33f), bMid = Lerp(b, c, 0.33f);
            const Frame bay = MakeFrame((a + b + aMid + bMid) * 0.25f, side < 0 ? a - b : b - a, (a + b) * 0.5f - (aMid + bMid) * 0.5f);
            Box(mesh, Offset(bay, 0, 0, 0.004f), {0.5f * Length(b - a) - 0.01f, 0.5f * Length(a - aMid) - 0.006f, 0.004f}, 0.002f, XWG_PLATE_GREY);
            XwgScreen(mesh, Offset(bay, 0, 0, 0.008f), 0, 0, 0.07f, 0.04f, side < 0 ? GROUP_HEALTH : GROUP_SCORE);
            {
                // the lower face (the set: left a chrome perforated dome, two slider slots and a knob in a black inset,
                // a red-outlined grid of white push buttons, chrome rings down the inboard edge; right two big chrome
                // knobs, a column of amber lamps, a red-outlined button grid)
                const Vec3 across = side < 0 ? aMid - bMid : bMid - aMid;
                const Frame face = MakeFrame((aMid + bMid + c + d) * 0.25f, Normalized(across), (aMid + bMid) - (c + d));
                const Vec3 out = face.z * 0.004f;
                const float halfWidth = 0.5f * Length(across), halfHeight2 = 0.25f * Length((aMid + bMid) - (c + d));
                Box(mesh, Offset(face, 0, 0, 0.002f), {halfWidth - 0.02f, halfHeight2 - 0.01f, 0.002f}, 0, XWG_PLATE_GREY);
                const float s = static_cast<float>(side);
                // the red-outlined grid of white buttons
                const Frame grid = Offset(face, -s * 0.05f, -0.02f, 0.004f);
                Box(mesh, grid, {0.055f, 0.045f, 0.002f}, 0, XWG_RED_LINE);
                Box(mesh, Offset(grid, 0, 0, 0.001f), {0.05f, 0.04f, 0.002f}, 0, XWG_BLACK);
                const int columns = side < 0 ? 3 : 2;
                for (int k = 0; k < columns * 3; ++k)
                {
                    Box(mesh, Offset(grid, (k % columns - (columns - 1) * 0.5f) * 0.024f, (k / columns - 1) * 0.024f, 0.006f), {0.0075f, 0.0075f, 0.003f}, 0.0012f, XWG_CREAM);
                }
                // chrome rings down the inboard edge
                for (int k = 0; k < 4; ++k)
                {
                    const float ring[3][2] = {{0, 0.008f}, {0.006f, 0.008f}, {0.006f, 0.004f}};
                    Lathe(mesh, Upright(Offset(face, s * (halfWidth - 0.04f), 0.06f - k * 0.03f, 0.004f)), ring, 3, 10, XWG_METAL);
                }
                if (side < 0)
                {
                    const Frame inset = Offset(face, s * -0.11f + 0.0f, 0.05f, 0.004f);
                    Box(mesh, inset, {0.035f, 0.05f, 0.002f}, 0, XWG_BLACK);
                    for (int k = -1; k <= 1; k += 2)
                    {
                        Box(mesh, Offset(inset, k * 0.012f, 0.01f, 0.002f), {0.004f, 0.03f, 0.0015f}, 0, XWG_CREAM);
                    }
                    const float knob[4][2] = {{0, 0.015f}, {0.012f, 0.015f}, {0.018f, 0.011f}, {0.02f, 0}};
                    Lathe(mesh, Upright(Offset(inset, 0, -0.035f, 0.002f)), knob, 4, 12, XWG_METAL);
                    // the perforated dome on the panel's top corner (the "salt-shaker")
                    const Vec3 corner = b + (a - b) * 0.18f + (d - a) * 0.10f + out * 2;
                    const float dome[6][2] = {{0, 0.022f}, {0.012f, 0.022f}, {0.016f, 0.03f}, {0.03f, 0.028f}, {0.042f, 0.018f}, {0.048f, 0}};
                    Lathe(mesh, MakeFrame(corner, {1, 0, 0}, {0, 1, 0}), dome, 6, 16, XWG_METAL);
                    for (int k = 0; k < 10; ++k)
                    {
                        const float a2 = k * 0.628f;
                        Box(mesh, MakeFrame(corner + Vec3{0.026f * cosf(a2), 0.034f, 0.026f * sinf(a2)}, {1, 0, 0}, {0, 1, 0}), {0.003f, 0.003f, 0.003f}, 0, XWG_BLACK);
                    }
                }
                else
                {
                    for (int k = 0; k < 2; ++k)
                    {
                        const float knob[5][2] = {{0, 0.022f}, {0.012f, 0.022f}, {0.02f, 0.02f}, {0.025f, 0.014f}, {0.026f, 0}};
                        Lathe(mesh, Upright(Offset(face, s * -0.11f, 0.07f - k * 0.055f, 0.004f)), knob, 5, 16, XWG_METAL);
                    }
                    for (int k = 0; k < 4; ++k)
                    {
                        Box(mesh, Offset(face, s * 0.03f, 0.075f - k * 0.026f, 0.006f), {0.01f, 0.01f, 0.004f}, 0.002f, XWG_LAMP);
                    }
                }
                ToggleBank(mesh, Offset(face, 0, -halfHeight2 + 0.03f, 0.004f), 3, 0.03f, detail);
            }
            // under the wing panel down to the floor: the knee well's side, wiring and a loom
            const Vec3 floorIn = {side * 0.13f, XWG_FLOOR, 0.98f}, floorOut = {side * XWG_HALF, XWG_FLOOR, 1.10f};
            if (side < 0)
            {
                Quad(mesh, c, d, floorIn, floorOut, XWG_WIRING);
            }
            else
            {
                Quad(mesh, d, c, floorOut, floorIn, XWG_WIRING);
            }
            const Vec3 loom[3] = {Lerp(d, floorIn, 0.2f) + Vec3{0, 0, 0.03f}, Lerp(c, floorOut, 0.5f) + Vec3{0, 0, 0.02f}, floorOut + Vec3{-side * 0.04f, 0.03f, 0.10f}};
            CableLoom(mesh, loom, 3, 4, 0.006f, {0, 0, 1}, XWG_RUBBER, XWG_FRAME, 0.12f);
            // the A-pillar's foot: a bracket where the wing panel meets the sill
            Box(mesh, MakeFrame(b + Vec3{-side * 0.02f, 0.01f, 0}, {1, 0, 0}, {0, 1, 0}), {0.03f, 0.03f, 0.04f}, 0.008f, XWG_FRAME);
        }
        // the column between the knees: kitbashed, a lamp strip, down to the floor
        const Vec3 cTop = {0, XWG_DASH_LOW_Y - 0.01f, XWG_DASH_LOW_Z + 0.01f}, cLow = {0, XWG_FLOOR + 0.04f, 1.00f};
        const Frame column = MakeFrame((cTop + cLow) * 0.5f, {1, 0, 0}, cTop - cLow);
        Box(mesh, Offset(column, 0, 0, -0.06f), {0.10f, 0.5f * Length(cTop - cLow), 0.06f}, 0.008f, XWG_DARK);
        KitPanel(mesh, column, 0.095f, 0.5f * Length(cTop - cLow) - 0.01f, 61, look);
        // the glare hood: from the dash's top edges forward under the windscreen, a rolled lip along its back edge
        const Vec3 hoodFront = {0.30f, 1.365f, 0.22f};
        const Vec3 hood[6] = {mirror(topOut) + Vec3{0, 0.02f, 0}, mirror(topIn), topIn, topOut + Vec3{0, 0.02f, 0}, hoodFront, mirror(hoodFront)};
        PlanarPolygon(mesh, hood, 6, {1, 0, 0}, XWG_BLACK);
        const Vec3 lip[4] = {mirror(topOut) + Vec3{0, 0.025f, 0.005f}, mirror(topIn) + Vec3{0, 0.005f, 0.005f}, topIn + Vec3{0, 0.005f, 0.005f}, topOut + Vec3{0, 0.025f, 0.005f}};
        Tube(mesh, lip, 4, 0.012f, 8, XWG_PAD);
        for (int k = 0; k < 6; ++k)
        {
            // pinstripe rectangles (circuit-like line graphics)
            const float z = 0.36f + k * 0.085f, y = 1.365f - (z - 0.22f) * 0.0f + 0.003f, w = 0.04f + (k % 3) * 0.05f;
            const Frame line = MakeFrame({(k % 2 ? 0.08f : -0.10f), y, z}, {1, 0, 0}, {0, 0, -1});
            for (int e = -1; e <= 1; e += 2)
            {
                Box(mesh, Offset(line, 0, e * 0.025f, 0), {w, 0.0015f, 0.0005f}, 0, XWG_CREAM);
                Box(mesh, Offset(line, e * w, 0, 0), {0.0015f, 0.025f, 0.0005f}, 0, XWG_CREAM);
            }
        }
        const Vec3 hose[3] = {{-0.27f, 1.375f, 0.26f}, {0, 1.38f, 0.24f}, {0.27f, 1.375f, 0.26f}};
        CorrugatedHose(mesh, hose, 3, 0.0125f, XWG_HOSE, XWG_METAL);
        Box(mesh, MakeFrame({0, 1.39f, 0.52f}, {1, 0, 0}, {0, 1, 0}), {0.06f, 0.025f, 0.05f}, 0.006f, XWG_FRAME);
        const int buttons[3] = {XWG_RED, XWG_BLUE, XWG_BLUE};
        for (int k = 0; k < 3; ++k)
        {
            IndicatorLamp(mesh, MakeFrame({0.14f + k * 0.03f, 1.366f, 0.50f}, {1, 0, 0}, {0, 1, 0}) , 0.008f, XWG_METAL, buttons[k]);
        }
    }
    // The targeting viewer (the research: a black boxy viewer with a lens ring and a pale readout strip on its side, on
    // a long thin black arm from a pivot behind and right of the head; stowed, it stands up beside the right of the
    // head, clear of the canopy).
    {
        const Vec3 pivot = {0.22f, 1.50f, 1.86f}, viewer = {0.20f, 1.64f, 1.62f};
        const Vec3 arm[3] = {pivot, pivot + Vec3{0, 0.10f, -0.06f}, viewer + Vec3{0, 0, 0.05f}};
        Tube(mesh, arm, 3, 0.01f, 6, XWG_BLACK);
        const float hub[3][2] = {{-0.03f, 0.025f}, {0.03f, 0.025f}, {0.035f, 0.018f}};
        Lathe(mesh, MakeFrame(pivot, {0, 1, 0}, {1, 0, 0}), hub, 3, 12, XWG_FRAME);
        const Frame box = MakeFrame(viewer, {1, 0, 0}, {0, 0.5f, 1});
        Box(mesh, box, {0.07f, 0.045f, 0.05f}, 0.008f, XWG_BLACK);
        const float lens[4][2] = {{0, 0.022f}, {0.012f, 0.022f}, {0.015f, 0.016f}, {0.012f, 0.012f}};
        Lathe(mesh, MakeFrame(box.At(0, -0.045f, 0), box.x, -box.y), lens, 4, 14, XWG_METAL);
        Box(mesh, Offset(box, -0.071f, 0, 0), {0.002f, 0.025f, 0.03f}, 0, XWG_PALE);
    }
    // The flight stick stands on its own floor plate (LeverModel::JetStick): a low round socket under it.
    {
        const float h = XWG_STICK_BASE.y - XWG_FLOOR;
        const float rings[4][2] = {{0, 0.092f}, {h * 0.5f, 0.092f}, {h, 0.084f}, {h, 0.0f}};
        Lathe(mesh, MakeFrame({XWG_STICK_BASE.x, XWG_FLOOR, XWG_STICK_BASE.z}, {1, 0, 0}, {0, 1, 0}), rings, 4, 20, XWG_FRAME);
    }
    // Rudder pedals ahead in the footwell.
    for (int side = -1; side <= 1; side += 2)
    {
        const Frame pedal = MakeFrame({side * 0.12f, XWG_FLOOR + 0.10f, 0.86f}, {1, 0, 0}, {0, 1, 0.5f});
        Box(mesh, pedal, {0.045f, 0.08f, 0.012f}, 0.006f, XWG_FRAME);
        for (int k = 0; k < 4; ++k)
        {
            Box(mesh, Offset(pedal, 0, -0.06f + k * 0.04f, 0.013f), {0.04f, 0.004f, 0.003f}, 0.001f, XWG_RUBBER);
        }
        const Vec3 rod[2] = {pedal.At(0, -0.07f, -0.01f), {side * 0.12f, XWG_FLOOR + 0.01f, 0.80f}};
        Tube(mesh, rod, 2, 0.012f, 8, XWG_METAL);
    }
    // The rear bulkhead (the research, from the ESB and ANH front stills): two black columns either side of the
    // headrest with small boxes and red lamps, the right one brass-tipped; corrugated hoses, two a side, looping from
    // behind the headrest down to the walls at elbow height; light grey machinery boxes low behind the seat.
    TiledQuad(mesh, {-XWG_HALF, XWG_FLOOR, XWG_BACK}, {2 * XWG_HALF, 0, 0}, {0, 1.46f - XWG_FLOOR, 0}, XWG_PLATE);
    for (int side = -1; side <= 1; side += 2)
    {
        const float x = side * 0.24f;
        Box(mesh, MakeFrame({x, 1.45f, 1.84f}, {1, 0, 0}, {0, 1, 0}), {0.03f, 0.27f, 0.03f}, 0.006f, XWG_BLACK);
        Box(mesh, MakeFrame({x - side * 0.035f, 1.45f, 1.83f}, {1, 0, 0}, {0, 1, 0}), {0.02f, 0.03f, 0.025f}, 0.004f, XWG_FRAME);
        IndicatorLamp(mesh, Upright(MakeFrame({x - side * 0.035f, 1.45f, 1.804f}, {1, 0, 0}, {0, 1, 0})), 0.008f, XWG_FRAME, XWG_RED);
        if (side > 0)
        {
            const float tip[4][2] = {{0, 0.02f}, {0.03f, 0.018f}, {0.05f, 0.01f}, {0.06f, 0}};
            Lathe(mesh, MakeFrame({x, 1.72f, 1.84f}, {1, 0, 0}, {0, 1, 0}), tip, 4, 10, XWG_BRASS);
        }
        for (int h = 0; h < 2; ++h)
        {
            const float o = h * 0.07f;
            const Vec3 duct[5] = {{side * (0.14f + o * 0.5f), 1.52f, 1.90f}, {side * (0.30f + o), 1.40f, 1.86f}, {side * (XWG_HALF - 0.06f), 1.16f, 1.72f - o}, {side * (XWG_HALF - 0.05f), 1.00f, 1.50f - o}, {side * (XWG_HALF - 0.04f), XWG_CONSOLE_Y + 0.05f, 1.30f - o}};
            CorrugatedHose(mesh, duct, 5, 0.04f - h * 0.006f, XWG_HOSE, XWG_FRAME);
        }
        const Frame box = MakeFrame({side * 0.25f, 0.95f, XWG_BACK - 0.10f}, {1, 0, 0}, {0, 1, 0});
        Box(mesh, box, {0.12f, 0.15f, 0.09f}, 0.012f, XWG_LIGHT_GREY);
        for (int k = 0; k < 5; ++k)
        {
            Box(mesh, Offset(box, 0, -0.09f + k * 0.04f, -0.092f), {0.10f, 0.006f, 0.004f}, 0, XWG_DARK);
        }
        KitPanel(mesh, MakeFrame({side * 0.36f, 0.80f, XWG_BACK - 0.004f}, {-1, 0, 0}, {0, 1, 0}), 0.085f, 0.12f, 71 + side, look);
    }
    const SeatLook seat = {0.23f, 0.72f, 0.26f, false, false, XWG_PAD, XWG_BLACK, XWG_METAL, XWG_DARK, XWG_DARK};
    Seat(mesh, XWG_EYE, XWG_FLOOR, seat);
}

void BuildXwing(Mesh& mesh)
{
    XwingCockpit(mesh, StockVertices("EXTERIOR_XWING"), StockCount("EXTERIOR_XWING"), StockVertices("STOCK_XWING"), StockCount("STOCK_XWING"));
}

/// The Y-wing's tub (docs/research/cockpits/xwing-ywing.md section 2): floor, the side walls' lower edge (half width,
/// height) and their top at the sill, the tub's front and back, the centre pod's face.
constexpr float YWG_FLOOR = 0.42f, YWG_WALL_LOW = 0.34f, YWG_WALL_LOW_Y = 0.60f, YWG_WALL_HIGH = 0.47f, YWG_SILL = 1.38f;
constexpr float YWG_FRONT = 0.90f, YWG_BACK = 2.05f, YWG_POD_Z = 1.00f;
/// The tandem cockpit's rear (idren2023 2026-11-02: the gunner behind the pilot under the one canopy, as the two-seat
/// Y-wings): the tub runs back to the gunner's back wall; between the seats only a low divider (its top).
constexpr float YWG_REAR = 2.86f, YWG_DIVIDER_TOP = 1.00f;
/// The Y-wing's pilot sits this much higher than the X-wing's (headset run 18: the eye too low to see out of the
/// windows): its eye, and the stick's pedestal and the left shelf under the handles (Flight places them from the eye).
constexpr float YWG_LIFT = 0.20f; // headset run 19: 8 cm higher again (was 0.12)
constexpr Vec3 YWG_EYE = {0.0f, 1.5f + YWG_LIFT, 1.5f};

// The Y-wing's tub wall at a height: the side walls lean out from the floor to the sill (docs/research/cockpits/
// xwing-ywing.md 2.1: a deep V-section tub, the pilot sitting deep).
float YwgWallX(float y)
{
    const float t = (y - YWG_WALL_LOW_Y) / (YWG_SILL - YWG_WALL_LOW_Y);
    return YWG_WALL_LOW + (YWG_WALL_HIGH - YWG_WALL_LOW) * (t < 0 ? 0 : t > 1 ? 1 : t);
}

// A large round coloured lens (the Y-wing's signature lamps) in a chrome bezel on a face.
void YwgLens(Mesh& mesh, const Frame& face, float x, float y, float radius, int lens)
{
    const Frame at = Upright(Offset(face, x, y, 0));
    const float bezel[4][2] = {{0, radius + 0.008f}, {0.008f, radius + 0.008f}, {0.012f, radius + 0.002f}, {0.008f, radius}};
    Lathe(mesh, at, bezel, 4, 16, XWG_METAL);
    const float dome[3][2] = {{0.008f, radius}, {0.016f, radius * 0.7f}, {0.02f, 0}};
    Lathe(mesh, at, dome, 3, 16, lens);
}

// The Y-wing's pilot cockpit (ANH Gold Squadron; a separate, more cramped set than the X-wing's): inside the ship's
// own hull and canopy (the trapezoid front hoop, the side and rear panes).
// The Y-wing gunner's station (headset run 18; laid out as the TIE bomber's bombardier, SPEEDER_CABIN_PARTS), in the
// back of the pilot's tub (idren2023 2026-11-02: one tandem cockpit under the ship's long canopy, the gunner seeing the
// pilot and out of the side panes; the tub's walls and floor are BuildYwing's): part 0 the back wall, 1 the turning
// chair with its column, grips and the display's rails, 2 the display housing, 3 and 4 its stop lamps (never lit: the
// turret has no stops), 5 the picture.
void YwingGunnerStation(Mesh& mesh, const FighterLook& look)
{
    const Vec3 eye = {YWING_GUNNER_EYE[0], YWING_GUNNER_EYE[1], YWING_GUNNER_EYE[2]};
    constexpr float BACK = YWG_REAR, WALL_TOP = 1.72f; // under the hull's roof where the canopy ends
    mesh.part = 0;
    {
        // the tread under the chair; the back wall from wall to wall of the tub, under the canopy's end
        TreadPlate(mesh, {-0.26f, YWG_FLOOR + 0.002f, eye.z - 0.40f}, {0.52f, 0, 0}, {0, 0, 0.55f}, 0.05f, XWG_RUBBER, XWG_DARK);
        const float low = YwgWallX(YWG_FLOOR), high = YwgWallX(WALL_TOP);
        Quad(mesh, {high, WALL_TOP, BACK}, {-high, WALL_TOP, BACK}, {-low, YWG_FLOOR, BACK}, {low, YWG_FLOOR, BACK}, XWG_WARM_GREY);
        KitPanel(mesh, MakeFrame({0, 1.15f, BACK - 0.005f}, {-1, 0, 0}, {0, 1, 0}), 0.34f, 0.26f, 615, look);
        for (int side = -1; side <= 1; side += 2)
        {
            // hoses down the back corners
            const float s = static_cast<float>(side);
            const Vec3 duct[3] = {{s * (YwgWallX(1.60f) - 0.05f), 1.60f, BACK - 0.04f}, {s * (YwgWallX(1.10f) - 0.07f), 1.10f, BACK - 0.08f}, {s * (YwgWallX(YWG_FLOOR) - 0.05f), YWG_FLOOR + 0.05f, BACK - 0.05f}};
            CorrugatedHose(mesh, duct, 3, 0.022f, XWG_HOSE, XWG_DARK);
        }
        for (int k = 0; k < 4; ++k)
        {
            IndicatorLamp(mesh, Upright(MakeFrame({-0.27f + k * 0.18f, 1.62f, BACK - 0.01f}, {-1, 0, 0}, {0, 1, 0})), 0.01f, XWG_FRAME, k % 2 ? XWG_LAMP : XWG_GREEN);
        }
    }

    // Part 1: the turning chair on its turntable, the column with the twin grips, the display's rails.
    mesh.part = 1;
    // (the sight close and small, under the canon canopy's roof: YWING_GUNNER_SIGHT_*)
    const float rail = YWING_GUNNER_SIGHT_DISTANCE;
    const float halfWidth = YWING_GUNNER_SIGHT_HALF_WIDTH, halfHeight = YWING_GUNNER_SIGHT_HALF_HEIGHT;
    {
        const float disc[3][2] = {{0, 0.30f}, {0.03f, 0.30f}, {0.035f, 0.28f}};
        Lathe(mesh, MakeFrame({eye.x, YWG_FLOOR, eye.z}, {1, 0, 0}, {0, 1, 0}), disc, 3, 32, XWG_FRAME);
        // narrow and upright: it turns all round in the canon canopy's 0.77 m between the pilot's seat and the end plate
        const SeatLook seat = {0.17f, 0.50f, 0.05f, false, false, XWG_PAD, XWG_BLACK, XWG_METAL, XWG_DARK, XWG_DARK};
        Seat(mesh, eye, YWG_FLOOR + 0.03f, seat);
        const Vec3 column[2] = {{eye.x, YWG_FLOOR + 0.03f, eye.z - 0.30f}, {eye.x, eye.y - 0.40f, eye.z - 0.30f}};
        Tube(mesh, column, 2, 0.03f, 12, XWG_METAL);
        const Frame head = MakeFrame({eye.x, eye.y - 0.38f, eye.z - 0.30f}, {1, 0, 0}, {0, 1, 0});
        Box(mesh, head, {0.10f, 0.05f, 0.05f}, 0.01f, XWG_FRAME);
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 arm[2] = {head.At(side * 0.10f, 0, 0), head.At(side * 0.20f, 0.02f, 0.03f)};
            Tube(mesh, arm, 2, 0.014f, 8, XWG_METAL);
            const Vec3 grip[2] = {head.At(side * 0.20f, 0.02f, 0.03f), head.At(side * 0.20f, 0.14f, 0.03f)};
            Tube(mesh, grip, 2, 0.0175f, 10, XWG_RUBBER);
            const float button[3][2] = {{0, 0.0075f}, {0.006f, 0.0075f}, {0.009f, 0}};
            Lathe(mesh, MakeFrame(grip[1], {1, 0, 0}, {0, 1, 0}), button, 3, 10, XWG_RED);
            // the rail: an arc about the eye beside the display, from below its lowest pitch to its highest (under the
            // canopy's roof)
            Vec3 arc[15];
            for (int k = 0; k <= 14; ++k)
            {
                const float a = (-50.0f + k * (50.0f + YWING_GUNNER_SIGHT_HIGH_DEGREES) / 14.0f) * 0.0174533f;
                arc[k] = {eye.x + side * (halfWidth + 0.05f), eye.y + rail * sinf(a), eye.z - rail * cosf(a)};
            }
            Tube(mesh, arc, 15, 0.01f, 6, XWG_METAL, false);
            const Vec3 post[2] = {arc[0], {eye.x + side * (halfWidth + 0.05f), YWG_FLOOR + 0.03f, arc[0].z}};
            Tube(mesh, post, 2, 0.012f, 6, XWG_FRAME);
        }
    }

    // Part 2: the display on the rails: a dark case, the bezel, a ring reticle, the HUD's target and ammunition.
    mesh.part = 2;
    const Vec3 screen = {eye.x, eye.y, eye.z - rail};
    const Frame face = MakeFrame(screen, {1, 0, 0}, {0, 1, 0});
    {
        Box(mesh, Offset(face, 0, 0, -0.03f), {halfWidth + 0.035f, halfHeight + 0.035f, 0.02f}, 0.006f, XWG_FRAME);
        for (int side = -1; side <= 1; side += 2)
        {
            Box(mesh, Offset(face, side * (halfWidth + 0.02f), 0, -0.004f), {0.015f, halfHeight + 0.035f, 0.006f}, 0.003f, XWG_DARK);
            Box(mesh, Offset(face, 0, side * (halfHeight + 0.02f), -0.004f), {halfWidth + 0.035f, 0.015f, 0.006f}, 0.003f, XWG_DARK);
            Box(mesh, Offset(face, side * (halfWidth + 0.05f), 0, -0.02f), {0.02f, 0.03f, 0.02f}, 0.004f, XWG_METAL);
        }
        SetDisplayScreen(&XWG_SCREENS); // the tanks' look (idren2023 2026-11-09)
        AddDisplay(GROUP_TARGET, face, -halfWidth + 0.05f, halfHeight - 0.012f, 0.08f, 0.016f, 2);
        StockScreen(mesh, Offset(face, 0, -halfHeight - 0.045f, 0.0f), 0.065f, 0.016f, 0.00008f, XWG_SCREENS, 0.006f);
        AddDisplay(GROUP_AMMUNITION, Offset(face, 0, -halfHeight - 0.045f, 0.0f), 0, 0, 0.10f, 0.022f, 2);
        SetDisplayScreen(nullptr);
    }

    // Part 5: the turret camera's picture (parts 3 and 4, the stop lamps, stay empty: no end stops).
    mesh.part = 5;
    Quad(mesh, face.At(-halfWidth, halfHeight, -0.002f), face.At(halfWidth, halfHeight, -0.002f), face.At(halfWidth, -halfHeight, -0.002f),
         face.At(-halfWidth, -halfHeight, -0.002f), XWG_PICTURE);
    mesh.part = 0;
}

void BuildYwing(Mesh& mesh)
{
    const FighterLook look = {XWG_WARM_GREY, XWG_FRAME, XWG_METAL, XWG_BLACK, XWG_SCREEN, {XWG_RED, XWG_LAMP, XWG_RED, XWG_GREEN}};
    const DetailLook detail = AsDetail(look, XWG_RED);
    // the hull's inside over the sill lined in the walls' warm grey (2026-11-09 texturing pass: its own mapping showed
    // light and streaked overhead)
    StockExterior(mesh, StockVertices("EXTERIOR_YWING"), StockCount("EXTERIOR_YWING"), XWG_STOCK, XWG_GLASS, XWG_WARM_GREY, XWG_WARM_GREY, 3.0f,
                  YWG_SILL); // the canopy's own inside (stockexterior KEPT_NODES) in the walls' plating
    // The tub: tall side walls leaning out, densely panelled; the floor; the sill; back to the gunner's wall (tandem).
    const float front = YWG_FRONT, back = YWG_REAR;
    TiledQuad(mesh, {-YWG_WALL_LOW, YWG_FLOOR, front}, {2 * YWG_WALL_LOW, 0, 0}, {0, 0, back - front}, XWG_PLATE);
    for (int side = -1; side <= 1; side += 2)
    {
        const float s = static_cast<float>(side);
        const Vec3 lowF = {s * YWG_WALL_LOW, YWG_WALL_LOW_Y, front}, lowB = {s * YWG_WALL_LOW, YWG_WALL_LOW_Y, back};
        const Vec3 highF = {s * YWG_WALL_HIGH, YWG_SILL, front}, highB = {s * YWG_WALL_HIGH, YWG_SILL, back};
        const Vec3 footF = {s * YWG_WALL_LOW, YWG_FLOOR, front}, footB = {s * YWG_WALL_LOW, YWG_FLOOR, back};
        if (side < 0)
        {
            Quad(mesh, highB, highF, lowF, lowB, XWG_WARM_GREY);
            Quad(mesh, lowB, lowF, footF, footB, XWG_WARM_GREY);
        }
        else
        {
            Quad(mesh, highF, highB, lowB, lowF, XWG_WARM_GREY);
            Quad(mesh, lowF, lowB, footB, footF, XWG_WARM_GREY);
        }
        // the leaning wall's panels, a sill rail
        const Vec3 up = Normalized(highF - lowF);
        for (int k = 0; k < 4; ++k)
        {
            const float z = 1.30f + k * 0.48f;
            const Vec3 middle = Lerp(lowF, highF, 0.55f) + Vec3{-s * 0.004f, 0, z - front};
            KitPanel(mesh, MakeFrame(middle, {0, 0, s}, up), 0.21f, 0.20f, 401 + k * 2 + side, look); // facing in (were in the wall)
        }
        RoundedBar(mesh, MakeFrame({s * (YWG_WALL_HIGH - 0.01f), YWG_SILL, (front + back) * 0.5f}, {0, 0, 1}, {0, 1, 0}), (back - front) * 0.5f, 0.02f, 0.03f, 0.015f, XWG_PAD);
        // the left shelf for the throttle and the take-off lever (Flight), a small one on the right
        const float shelfIn = s * 0.19f, shelfOut = s * YwgWallX(0.88f);
        Box(mesh, MakeFrame({(shelfIn + shelfOut) * 0.5f, 0.86f + YWG_LIFT, 1.38f}, {1, 0, 0}, {0, 1, 0}), {fabsf(shelfOut - shelfIn) * 0.5f, 0.02f, 0.30f}, 0.006f, XWG_FRAME);
        // the white boxes with red push buttons behind the shoulders, the black hoses running down, chrome pipes
        const Frame box = MakeFrame({s * 0.25f, YWG_DIVIDER_TOP + 0.03f, YWG_BACK - 0.02f}, {-1, 0, 0}, {0, 1, 0});
        Box(mesh, Offset(box, 0, 0, 0.03f), {0.04f, 0.03f, 0.03f}, 0.005f, XWG_LIGHT_GREY);
        IndicatorLamp(mesh, Upright(Offset(box, 0, 0, 0.061f)), 0.012f, XWG_METAL, XWG_RED);
        for (int h = 0; h < 2; ++h)
        {
            const Vec3 hose[4] = {{s * (0.34f + h * 0.05f), YWG_SILL + 0.10f, YWG_BACK - 0.05f}, {s * (0.36f + h * 0.05f), 1.20f, YWG_BACK - 0.10f}, {s * (0.34f + h * 0.03f), 0.90f, YWG_BACK - 0.08f}, {s * (0.30f + h * 0.03f), YWG_FLOOR + 0.05f, YWG_BACK - 0.05f}};
            CorrugatedHose(mesh, hose, 4, 0.035f, XWG_BLACK, XWG_DARK);
        }
    }
    // the front bulkhead behind the pod, closing the tub ahead up to the glare line
    Quad(mesh, {-YWG_WALL_HIGH, YWG_SILL + 0.02f, front}, {YWG_WALL_HIGH, YWG_SILL + 0.02f, front}, {YWG_WALL_LOW, YWG_FLOOR, front}, {-YWG_WALL_LOW, YWG_FLOOR, front}, XWG_WARM_GREY);
    // the low divider between the seats (the pilot's back bulkhead before the tandem cockpit), plated on both faces,
    // chrome pipes along its top
    const float dividerX = YwgWallX(YWG_DIVIDER_TOP);
    TiledQuad(mesh, {-dividerX, YWG_FLOOR, YWG_BACK}, {2 * dividerX, 0, 0}, {0, YWG_DIVIDER_TOP - YWG_FLOOR, 0}, XWG_PLATE);
    TiledQuad(mesh, {dividerX, YWG_FLOOR, YWG_BACK + 0.03f}, {-2 * dividerX, 0, 0}, {0, YWG_DIVIDER_TOP - YWG_FLOOR, 0}, XWG_PLATE);
    Box(mesh, MakeFrame({0, YWG_DIVIDER_TOP, YWG_BACK + 0.015f}, {1, 0, 0}, {0, 1, 0}), {dividerX, 0.012f, 0.02f}, 0.005f, XWG_FRAME);
    for (int k = 0; k < 2; ++k)
    {
        const Vec3 pipe[2] = {{-0.36f, YWG_DIVIDER_TOP - 0.10f + k * 0.05f, YWG_BACK - 0.04f}, {0.36f, YWG_DIVIDER_TOP - 0.10f + k * 0.05f, YWG_BACK - 0.04f}};
        FlangedPipe(mesh, pipe, 2, 0.012f, {0, 0, -1}, 0.25f, XWG_METAL, XWG_FRAME);
    }
    // The dash: the tiny centre pod (two square instruments with the minimap and the target, a chrome cup, a lower box
    // with three dials and toggles), the front wall panels angled in either side with their big round lenses, the grey
    // corrugated hose arching over the pod (the Y-wing dash's signature).
    {
        const Frame pod = MakeFrame({0, 1.18f, YWG_POD_Z}, {1, 0, 0}, {0, 1, 0.25f});
        Box(mesh, Offset(pod, 0, 0, -0.10f), {0.14f, 0.12f, 0.10f}, 0.012f, XWG_FRAME);
        for (int side = -1; side <= 1; side += 2)
        {
            const Frame face = Offset(pod, side * 0.06f, 0.045f, 0.002f);
            Box(mesh, Offset(face, 0, 0, 0.004f), {0.048f, 0.048f, 0.006f}, 0.004f, XWG_METAL);
            XwgScreen(mesh, Offset(face, 0, 0, 0.011f), 0, 0, 0.036f, 0.036f, side < 0 ? GROUP_MAP : GROUP_TARGET);
        }
        const float cup[5][2] = {{0, 0.02f}, {0.02f, 0.025f}, {0.05f, 0.035f}, {0.06f, 0.036f}, {0.06f, 0.03f}};
        Lathe(mesh, Upright(Offset(pod, 0.12f, 0.10f, 0)), cup, 5, 16, XWG_METAL);
        const Frame lower = Offset(pod, 0, -0.07f, 0.002f);
        Box(mesh, Offset(lower, 0, 0, 0.004f), {0.10f, 0.035f, 0.008f}, 0.004f, XWG_BLACK);
        KnobRow(mesh, Offset(lower, 0, 0.005f, 0.012f), 3, 0.05f, 0.014f, detail);
        ToggleBank(mesh, Offset(pod, 0, -0.105f, 0.004f), 4, 0.03f, detail);
        XwgScreen(mesh, Offset(pod, 0, -0.16f, 0.0f), 0, 0, 0.05f, 0.022f, GROUP_SCORE);
        const Vec3 arch[5] = {{-0.18f, 1.05f, YWG_POD_Z - 0.02f}, {-0.14f, 1.33f, YWG_POD_Z - 0.06f}, {0.02f, 1.38f, YWG_POD_Z - 0.08f}, {0.16f, 1.28f, YWG_POD_Z - 0.05f}, {0.18f, YWG_FLOOR + 0.10f, YWG_POD_Z + 0.08f}};
        CorrugatedHose(mesh, arch, 5, 0.025f, XWG_HOSE, XWG_METAL);
        const Vec3 drop[3] = {{-0.18f, 1.05f, YWG_POD_Z - 0.02f}, {-0.20f, 0.80f, YWG_POD_Z + 0.05f}, {-0.18f, YWG_FLOOR + 0.05f, YWG_POD_Z + 0.12f}};
        CorrugatedHose(mesh, drop, 3, 0.022f, XWG_HOSE, XWG_METAL);
        for (int side = -1; side <= 1; side += 2)
        {
            const float s = static_cast<float>(side);
            const Vec3 inner = {s * 0.17f, 1.18f, YWG_POD_Z - 0.02f}, outer = {s * 0.36f, 1.20f, YWG_POD_Z + 0.20f};
            const Vec3 across = side > 0 ? outer - inner : inner - outer;
            const Frame panel = MakeFrame((inner + outer) * 0.5f, Normalized(across), {0, 1, 0.2f});
            Box(mesh, Offset(panel, 0, 0, -0.03f), {0.5f * Length(across) + 0.01f, 0.17f, 0.03f}, 0.008f, XWG_FRAME);
            Box(mesh, Offset(panel, 0, 0, 0.002f), {0.5f * Length(across) - 0.01f, 0.15f, 0.003f}, 0, XWG_BLACK);
            for (int k = 0; k < 8; ++k)
            {
                Box(mesh, Offset(panel, -0.10f + k * 0.028f, 0.13f, 0.006f), {0.004f, 0.004f, 0.002f}, 0, XWG_RED);
            }
            if (side < 0)
            {
                YwgLens(mesh, panel, -0.05f, 0.06f, 0.02f, XWG_LAMP);
                YwgLens(mesh, panel, 0.05f, 0.06f, 0.02f, XWG_RED);
                const float gauge[4][2] = {{0, 0.035f}, {0.008f, 0.035f}, {0.01f, 0.03f}, {0.006f, 0.028f}};
                Lathe(mesh, Upright(Offset(panel, 0.02f, -0.06f, 0.004f)), gauge, 4, 18, XWG_METAL);
                XwgScreen(mesh, Offset(panel, 0.02f, -0.06f, 0.01f), 0, 0, 0.026f, 0.026f, GROUP_HEALTH);
                ToggleBank(mesh, Offset(panel, -0.09f, -0.02f, 0.004f), 2, 0.025f, detail);
            }
            else
            {
                const int lenses[4] = {XWG_RED, XWG_LAMP, XWG_GREEN, XWG_YELLOW};
                for (int k = 0; k < 4; ++k)
                {
                    YwgLens(mesh, panel, -0.09f + k * 0.06f, -0.08f, 0.018f, lenses[k]);
                }
                XwgScreen(mesh, Offset(panel, 0, 0.04f, 0.004f), 0, 0, 0.06f, 0.035f, GROUP_AMMUNITION);
                IndicatorLamp(mesh, Upright(Offset(panel, 0.10f, -0.02f, 0.003f)), 0.01f, XWG_METAL, XWG_RED);
            }
            // a short chrome rod from the wall's top edge, seen against the window
            const Vec3 rod[2] = {outer + Vec3{0, 0.16f, 0}, outer + Vec3{-s * 0.04f, 0.26f, -0.04f}};
            Tube(mesh, rod, 2, 0.006f, 6, XWG_METAL);
        }
        // the finned heat-sink block on the centre line under the dash
        const Frame sink = MakeFrame({0, YWG_FLOOR + 0.16f, YWG_POD_Z + 0.05f}, {1, 0, 0}, {0, 1, 0});
        Box(mesh, Offset(sink, 0, 0, -0.03f), {0.11f, 0.14f, 0.03f}, 0.005f, XWG_DARK);
        for (int k = 0; k < 9; ++k)
        {
            Box(mesh, Offset(sink, -0.096f + k * 0.024f, 0, 0.02f), {0.004f, 0.13f, 0.04f}, 0, XWG_METAL);
        }
    }
    // The targeting box (a flat screen box with a red lens face, not the X-wing's viewer) on a two-link arm from a
    // chrome post at the right front wall; stowed up and out.
    {
        const Vec3 postLow = {0.30f, 1.20f, 1.18f}, postTop = {0.30f, 1.52f, 1.18f};
        const Vec3 post[2] = {postLow, postTop};
        Tube(mesh, post, 2, 0.012f, 8, XWG_METAL);
        const Vec3 link[3] = {postTop, postTop + Vec3{-0.02f, 0.08f, 0.04f}, {0.26f, 1.66f, 1.22f}};
        Tube(mesh, link, 3, 0.01f, 6, XWG_BLACK);
        const Frame box = MakeFrame({0.24f, 1.64f, 1.20f}, {1, 0, 0}, {0, -0.6f, -1});
        Box(mesh, box, {0.075f, 0.05f, 0.03f}, 0.006f, XWG_BLACK);
        Quad(mesh, box.At(-0.06f, 0.038f, 0.031f), box.At(0.06f, 0.038f, 0.031f), box.At(0.06f, -0.038f, 0.031f), box.At(-0.06f, -0.038f, 0.031f), XWG_RED_SCREEN);
    }
    // The stick's pedestal (Flight), the throttle's and lever's slots on the left shelf, pedals; the seat.
    Box(mesh, MakeFrame({-0.24f, 0.884f + YWG_LIFT, 1.22f}, {1, 0, 0}, {0, 0, -1}), {0.022f, 0.15f, 0.004f}, 0.002f, XWG_FRAME);
    Box(mesh, MakeFrame({-0.24f, 0.886f + YWG_LIFT, 1.22f}, {1, 0, 0}, {0, 0, -1}), {0.010f, 0.14f, 0.004f}, 0.002f, XWG_DARK);
    Box(mesh, MakeFrame({-0.33f, 0.884f + YWG_LIFT, 1.40f}, {1, 0, 0}, {0, 0, -1}), {0.02f, 0.11f, 0.004f}, 0.002f, XWG_FRAME);
    {
        const float h = XWG_STICK_BASE.y + YWG_LIFT - YWG_FLOOR;
        const float rings[7][2] = {{0, 0.10f}, {0.015f, 0.10f}, {0.02f, 0.085f}, {0.05f, 0.075f}, {h - 0.04f, 0.075f}, {h - 0.02f, 0.088f}, {h, 0.088f}};
        Lathe(mesh, MakeFrame({XWG_STICK_BASE.x, YWG_FLOOR, XWG_STICK_BASE.z}, {1, 0, 0}, {0, 1, 0}), rings, 7, 18, XWG_FRAME);
    }
    for (int side = -1; side <= 1; side += 2)
    {
        const Frame pedal = MakeFrame({side * 0.12f, YWG_FLOOR + 0.08f, 0.82f}, {1, 0, 0}, {0, 1, 0.5f});
        Box(mesh, pedal, {0.04f, 0.08f, 0.012f}, 0.006f, XWG_BLACK);
    }
    const SeatLook seat = {0.22f, 0.72f, 0.24f, false, false, XWG_PAD, XWG_BLACK, XWG_METAL, XWG_DARK, XWG_DARK};
    Seat(mesh, YWG_EYE, YWG_FLOOR, seat);
    YwingGunnerStation(mesh, look);
}


// ---------------------------------------------------------------------------------------------------------------------
// TIE fighter (PLAN starfighters): our own cockpit pod at the real size in place of the game's first-person model
// (imp_1st_cockpit_tiefighter, whose textures it uses): the ball's inside in dark ribbed metal, the big round front
// window with its octagonal middle pane and eight struts (docs/research/starfighter-cockpits-canon.md), the console
// under it with the targeting display, the overhead hatch; the stick, throttle and take-off lever (Flight) as in every
// starfighter. Space: the cockpit model's, the eye at the game's (0, 1.5, 1.5).

enum TieFinish
{
    TIE_RIBS,     // imp_1st_cockpit_tiefighter (512 x 256): the ribbed trim band, by size
    TIE_LEFT,     //   the left panel (radar), whole
    TIE_TARGET,   //   the targeting panel, whole
    TIE_DIAGONAL, //   the diagonal ribs panel, whole
    TIE_RIGHT,    //   the right panel (display, wiring), whole
    TIE_GLASS,    // imp_cockpit_win: the window's glass
    TIE_FRAME,    // window frame and struts (dark grey)
    TIE_DARK,
    TIE_METAL,
    TIE_RUBBER,
    TIE_PAD,
    TIE_LAMP,     // green lamps
    TIE_RED,
    TIE_SCREEN,
    TIE_STOCK,    // the ship's own exterior (StockExterior)
    TIE_PLATE,    // kitbash plates (the ANH set's grey)
    TIE_WHITE,
    TIE_AMBER,
    TIE_COCKPIT,  // the game's first-person cockpit pieces (StockPieces), their own mapping
    TIE_FX,       //   their lamps
    TIE_SEAM,     // the facets' raised seam strips (one step lighter)
    TIE_ORANGE,   // the ANH set's red-orange light graphics
    TIE_FAN,      // the column's perforated fan (worn aluminium)
    TIE_WEAR,     // worn bright steel (the hatch's wheel)
    TIE_ROD,      // the translucent red rods overhead
    TIE_PICTURE,  // the bomber's bomb sight: the scope pass's picture (self-lit)
    TIE_SCREEN_FRAME,   // the HUD screens' stock frame (texture 5, imp_1st_cockpit_securitycam: the tanks' look)
    TIE_SCREEN_GLASS,   //   their glass (scan lines)
    TIE_SCREEN_BACKING, //   behind the glass
    TIE_FINISH_COUNT
};

// A finish on the TIE cockpit's 512 x 256 texture, from its texel rectangle.
constexpr Finish TieTexture(float x0, float y0, float x1, float y1, uint32_t color, float specular, float power, float glow = 0)
{
    Finish finish = {{x0 / 512.0f, y0 / 256.0f, x1 / 512.0f, y1 / 256.0f}, 1, 1, color, specular, power, glow};
    finish.isDense = true;
    return finish;
}

const Finish TIE_FINISHES[TIE_FINISH_COUNT] = {
    // the ball's wall: the stock's brushed grey panel art, inside its bevel lines (2026-11-11: the whole patch
    // (110, 175, 220, 250) tiled its dark and bright edges into blotches on the walls)
    TieTexture(118, 180, 208, 240, 0x8A8E94, 0.25f, 16),
    TieTexture(0, 56, 100, 256, 0x868A90, 0.20f, 14, 0.08f),  // left panel
    TieTexture(100, 56, 226, 256, 0x868A90, 0.20f, 14, 0.10f), // targeting panel
    TieTexture(226, 56, 290, 256, 0x868A90, 0.20f, 14),       // diagonal ribs
    TieTexture(290, 56, 512, 256, 0x868A90, 0.20f, 14, 0.08f), // right panel
    {{0, 0, 1, 1}, 1, 4, 0x98B0C4, 0.60f, 40, 0, 0.99f},       // glass
    TieTexture(118, 180, 208, 240, 0x6C7585, 0.35f, 22), // frame (gunmetal): the stock art, tinted
    TieTexture(118, 180, 208, 240, 0x3A3E46, 0.05f, 6), // dark: the stock art, tinted
    {{0, 0, 1, 1}, 1, 0, 0x8C9096, 0.60f, 40, 0},              // metal
    TieTexture(118, 180, 208, 240, 0x464853, 0.20f, 10), // rubber: the stock art, tinted
    TieTexture(118, 180, 208, 240, 0x696B78, 0.15f, 10), // seat padding (black): the stock art, tinted
    {{0, 0, 1, 1}, 1, 0, 0x60FF80, 0, 1, 1.0f},                // green lamp
    {{0, 0, 1, 1}, 1, 0, 0xFF3020, 0, 1, 1.0f},                // red lamp
    {{0, 0, 1, 1}, 1, 0, 0x0A100C, 0.50f, 40, 0.05f},          // screen glass
    {{0, 0, 1, 1}, 1, 3, 0x808080, 0.20f, 14, 0},             // the exterior, its own mapping
    TieTexture(118, 180, 208, 240, 0x8A8E94, 0.25f, 16),     // plates: the brushed panel art
    {{0, 0, 1, 1}, 1, 0, 0xE8F0FF, 0, 1, 1.0f},               // white lamp
    {{0, 0, 1, 1}, 1, 0, 0xFFB050, 0, 1, 1.0f},               // amber lamp
    {{0, 0, 1, 1}, 1, 1, 0x808080, 0.25f, 16, 0},             // stock cockpit pieces
    {{0, 0, 1, 1}, 1, 2, 0xC0C0C0, 0, 1, 1.0f},               // their lamps
    TieTexture(226, 56, 290, 256, 0xB0B4B8, 0.35f, 22), // seams: the stock art, tinted
    {{0, 0, 1, 1}, 1, 0, 0xFF4A1C, 0, 1, 1.0f},               // red-orange light graphics
    {{0, 0, 1, 1}, 1, 0, 0xB9BCBE, 0.55f, 34, 0},             // fan
    {{0, 0, 1, 1}, 1, 0, 0x9AA3AB, 0.70f, 44, 0},             // worn steel
    {{0, 0, 1, 1}, 1, 0, 0xFF2A1A, 0.50f, 40, 0.6f, 0.75f},   // red rods (translucent, glowing)
    {{0, 0.3f, 1, 0.7f}, 1, 6, 0xFFFFFF, 0, 1, 1.0f},          // the bomb sight's picture
    ScreenFrameFinish(),                                                    // stock screen frame
    ScreenGlassFinish(IMP_SCREEN_ART),                                      // its glass
    ScreenBackingFinish(IMP_SCREEN_ART),                                    // behind the glass
};

// The HUD screens in the tanks' look (idren2023 2026-11-09: every vehicle's HUD as the tanks').
const StockScreenLook TIE_SCREENS = {&IMP_SCREEN_ART, TIE_SCREEN_FRAME, TIE_SCREEN_GLASS, TIE_SCREEN_BACKING, TIE_DARK};

constexpr Vec3 TIE_EYE = {0.0f, 1.5f, 1.5f};
/// The ball: its centre and radius; the window's plane (its middle on the ball's axis through the eye's height); the
/// floor's chord; the octagonal middle pane's corner radius.
constexpr Vec3 TIE_BALL = {0, 1.42f, 1.60f};
constexpr float TIE_RADIUS = 1.25f, TIE_WINDOW_Z = 0.60f, TIE_FLOOR = 0.38f, TIE_OCTAGON = 0.303f;
/// The TIE bomber's tube behind the ball (the pilot pod's cylinder): its radius, axis height, front (where it meets
/// the ball) and back, its floor; the bombardier's eye and grips are ShellDesigns.h's TIE_BOMBER_GUNNER_*.
constexpr float TIE_BOMBER_TUBE_RADIUS = 0.95f, TIE_BOMBER_TUBE_Y = 1.44f, TIE_BOMBER_TUBE_FRONT = 2.40f;
constexpr float TIE_BOMBER_TUBE_BACK = 5.80f, TIE_BOMBER_TUBE_FLOOR = 0.58f;

// The ball's radius at a distance behind the window's plane.
float TieRing(float behind)
{
    const float axial = behind - (TIE_BALL.z - TIE_WINDOW_Z);
    const float squared = TIE_RADIUS * TIE_RADIUS - axial * axial;
    return squared > 0 ? sqrtf(squared) : 0.0f;
}

// A screen let into the console (as XwgScreen).
void TieScreen(Mesh& mesh, const Frame& face, float x, float y, float halfWidth, float halfHeight, int group)
{
    // the tanks' stock screen (idren2023 2026-11-09), in a case let into the panel
    const Frame glass = Offset(face, x, y, 0.004f);
    StockScreen(mesh, glass, halfWidth, halfHeight, SCREEN_TEXEL_METERS * halfHeight / 0.0675f, TIE_SCREENS, 0.012f);
    SetDisplayScreen(&TIE_SCREENS);
    AddDisplay(group, glass, 0, 0, halfWidth * 1.9f, halfHeight * 1.9f);
    SetDisplayScreen(nullptr);
}

// A point on the ball's inside: its centre, a direction and a distance from it.
Vec3 TieAt(Vec3 direction, float radius)
{
    return TIE_BALL + direction * radius;
}

// The direction from the ball's centre at a yaw (right positive) and pitch (up positive) from straight ahead.
Vec3 TieDirection(float yaw, float pitch)
{
    return {sinf(yaw) * cosf(pitch), sinf(pitch), -cosf(yaw) * cosf(pitch)};
}

/// The overhead hatch's axis (up and a little back, docs/research/cockpits/tie.md section 3) and the rear hatch's.
const Vec3 TIE_HATCH_AXIS = {0, 0.966f, 0.259f};
constexpr Vec3 TIE_REAR_AXIS = {0, 0, 1};

// Whether a direction from the ball's centre is in one of its openings (the window, the overhead hatch's slots, the
// rear hatch's window): the wall is left open there and the opening's own frame covers the edge.
/// The ball's back is open into the bomber's tube (TieBomberStation) while the bomber's shell is built.
bool _isTieRearOpen = false;

bool TieOpening(Vec3 d)
{
    return -d.z > 0.80f || Dot(d, TIE_HATCH_AXIS) > 0.925f || Dot(d, TIE_REAR_AXIS) > (_isTieRearOpen ? 0.64f : 0.988f);
}

// A frame on the wall facing the ball's centre, in a direction, at a distance (x across, y up as seen from inside).
Frame TieFacing(Vec3 direction, float radius)
{
    const Vec3 up = fabsf(direction.y) > 0.9f ? Vec3{0, 0, -1} : Vec3{0, 1, 0};
    return MakeFrame(TieAt(direction, radius), Normalized(Cross(up, -direction)), up);
}

// A wall facet's dark triangular inset panel (recessed, inset from the facet's edges), half of them carrying the
// red-orange slit graphics of the ANH set (rows of thin slits, short bars).
void TieInsetPanel(Mesh& mesh, Vec3 a, Vec3 b, Vec3 c, int index)
{
    const Vec3 centre = (a + b + c) * (1.0f / 3);
    const Vec3 toward = Normalized(TIE_BALL - centre) * 0.012f;
    const Vec3 p[3] = {Lerp(centre, a, 0.78f) + toward, Lerp(centre, b, 0.78f) + toward, Lerp(centre, c, 0.78f) + toward};
    const float st[3][2] = {{0, 0}, {1, 0}, {0, 1}};
    Triangle(mesh, p[0], p[1], p[2], st, TIE_DARK);
    if (index % 2)
    {
        return;
    }
    const Frame face = MakeFrame(centre + toward * 1.4f, Normalized(b - a), Normalized(c - (a + b) * 0.5f));
    const int rows = 2 + index % 3;
    for (int r = 0; r < rows; ++r)
    {
        for (int k = 0; k < 4 - r; ++k)
        {
            const float x = (k - (3 - r) * 0.5f) * 0.026f, y = -0.04f + r * 0.035f;
            Box(mesh, Offset(face, x, y, 0), {0.004f, index % 4 == 0 ? 0.022f : 0.012f, 0.002f}, 0, (k + r + index) % 5 ? TIE_ORANGE : TIE_WHITE);
        }
    }
}

// The ball's wall: a smooth inside (left open at the window and the hatches) overlaid with the ANH set's faceting:
// raised seam strips along a coarse grid of flat triangles, bolt heads at their corners, one facet in three with a
// dark inset panel.
void TieWall(Mesh& mesh)
{
    constexpr int AROUND = 64, DOWN = 32;
    const auto at = [](int i, int j) {
        const float theta = j * (3.1415926f / DOWN), phi = i * (6.2831853f / AROUND);
        return Vec3{sinf(theta) * cosf(phi), cosf(theta), sinf(theta) * sinf(phi)};
    };
    for (int j = 0; j < DOWN; ++j)
    {
        for (int i = 0; i < AROUND; ++i)
        {
            const Vec3 d = Normalized(at(i, j) + at(i + 1, j + 1));
            if (TieOpening(d))
            {
                continue;
            }
            // by real size round the ball (2026-11-09 texturing pass: each cell had the whole patch, squeezed toward
            // the poles)
            PlanarQuad(mesh, TieAt(at(i, j), TIE_RADIUS), TieAt(at(i, j + 1), TIE_RADIUS),
                       TieAt(at(i + 1, j + 1), TIE_RADIUS), TieAt(at(i + 1, j), TIE_RADIUS), TIE_RIBS);
        }
    }
    // the facets: a coarse grid (12 round, 8 down), each cell two triangles
    constexpr int CELLS_AROUND = 12, CELLS_DOWN = 8;
    const auto corner = [](int i, int j) {
        const float theta = j * (3.1415926f / CELLS_DOWN), phi = i * (6.2831853f / CELLS_AROUND) + (j % 2) * (3.1415926f / CELLS_AROUND);
        return Vec3{sinf(theta) * cosf(phi), cosf(theta), sinf(theta) * sinf(phi)};
    };
    const float seam = TIE_RADIUS - 0.012f;
    int index = 0;
    for (int j = 1; j < CELLS_DOWN; ++j)
    {
        for (int i = 0; i < CELLS_AROUND; ++i)
        {
            const Vec3 a = corner(i, j), b = corner(i + 1, j), c = corner(i, j + 1), d = corner(i + 1, j + 1);
            const Vec3 edges[3][2] = {{a, b}, {a, c}, {b, c}};
            for (const auto& edge : edges)
            {
                Vec3 path[4];
                bool isOpen = false;
                for (int k = 0; k < 4; ++k)
                {
                    const Vec3 dir = Normalized(Lerp(edge[0], edge[1], k / 3.0f));
                    isOpen = isOpen || TieOpening(dir) || Dot(dir, {1, 0, 0}) > 0.80f || Dot(dir, {-1, 0, 0}) > 0.80f ||
                             Dot(dir, TIE_REAR_AXIS) > 0.93f; // not across the rear hatch's plate
                    path[k] = TieAt(dir, seam);
                }
                if (!isOpen)
                {
                    Tube(mesh, path, 4, 0.008f, 4, TIE_SEAM, false);
                }
            }
            const Vec3 bolt = TieAt(a, seam - 0.006f);
            if (!TieOpening(a) && fabsf(a.x) < 0.80f)
            {
                Box(mesh, MakeFrame(bolt, {1, 0, 0}, {0, 1, 0}), {0.009f, 0.009f, 0.009f}, 0.004f, TIE_METAL);
            }
            const Vec3 middle = Normalized(a + c + d);
            // (none round the rear hatch: they cut across its ring, headset run 18 "unfinished")
            if (++index % 3 == 0 && !TieOpening(middle) && fabsf(middle.x) < 0.75f && middle.y > -0.6f &&
                Dot(Normalized(a), TIE_REAR_AXIS) < 0.80f && Dot(Normalized(c), TIE_REAR_AXIS) < 0.80f &&
                Dot(Normalized(d), TIE_REAR_AXIS) < 0.80f)
            {
                TieInsetPanel(mesh, TieAt(a, seam - 0.004f), TieAt(c, seam - 0.004f), TieAt(d, seam - 0.004f), index);
            }
        }
    }
    // the side walls: a shallow eight-sided pyramid of triangles converging on a boss at the wing pylon's axis
    for (int side = -1; side <= 1; side += 2)
    {
        const Vec3 axis = {static_cast<float>(side), 0, 0};
        const Vec3 hub = TieAt(axis, TIE_RADIUS - 0.08f);
        const Vec3 u = {0, 1, 0}, v = {0, 0, 1};
        Vec3 rim[9];
        for (int k = 0; k <= 8; ++k)
        {
            const float a = k * 0.785398f;
            rim[k] = TieAt(Normalized(axis * 0.8f + (u * cosf(a) + v * sinf(a)) * 0.6f), TIE_RADIUS - 0.004f);
        }
        for (int k = 0; k < 8; ++k)
        {
            // by real size (the whole patch had been squeezed onto each long thin triangle)
            const Vec3 facet[3] = {hub, side > 0 ? rim[k] : rim[k + 1], side > 0 ? rim[k + 1] : rim[k]};
            PlanarPolygon(mesh, facet, 3, rim[k + 1] - rim[k], TIE_PLATE);
            const Vec3 spoke[2] = {hub, rim[k]};
            Tube(mesh, spoke, 2, 0.009f, 4, TIE_SEAM);
            const Vec3 edge[2] = {rim[k], rim[k + 1]};
            Tube(mesh, edge, 2, 0.009f, 4, TIE_SEAM);
            if (k % 2 == 0)
            {
                TieInsetPanel(mesh, hub, side > 0 ? rim[k] : rim[k + 1], side > 0 ? rim[k + 1] : rim[k], k + (side > 0 ? 0 : 1));
            }
        }
        const float boss[4][2] = {{0, 0.045f}, {0.02f, 0.045f}, {0.03f, 0.03f}, {0.035f, 0}};
        Lathe(mesh, MakeFrame(hub, {0, 0, 1}, -axis), boss, 4, 14, TIE_METAL);
    }
}

// The TIE's cockpit inside the given exterior (the bomber's pilot pod is the same set: the films had one TIE cockpit).
void TieBomberStation(Mesh& mesh, const FighterLook& look);

void TieCockpit(Mesh& mesh, const StockVertex* exterior, int exteriorCount, const StockVertex* stock, int stockCount, bool isBomber = false)
{
    const FighterLook look = {TIE_PLATE, TIE_FRAME, TIE_METAL, TIE_DARK, TIE_SCREEN, {TIE_ORANGE, TIE_RED, TIE_WHITE, TIE_ORANGE}};
    const DetailLook detail = AsDetail(look, TIE_ORANGE);
    // The ship's own pylons and wings round the ball (the ball, its window and frame are ours).
    StockExterior(mesh, exterior, exteriorCount, TIE_STOCK, TIE_GLASS);
    _isTieRearOpen = isBomber;
    TieWall(mesh);

    // The floor: a flat pan across the ball's bottom, a raised strip under the seat and the column, ribbed footwells.
    {
        const float r = sqrtf(TIE_RADIUS * TIE_RADIUS - (TIE_BALL.y - TIE_FLOOR) * (TIE_BALL.y - TIE_FLOOR)) - 0.01f;
        Vec3 disc[32];
        for (int i = 0; i < 32; ++i)
        {
            const float a = i * (6.2831853f / 32);
            disc[i] = {r * cosf(a), TIE_FLOOR, TIE_BALL.z + r * sinf(a)};
        }
        PlanarPolygon(mesh, disc, 32, {1, 0, 0}, TIE_DARK);
        Box(mesh, MakeFrame({0, TIE_FLOOR + 0.02f, TIE_BALL.z - 0.10f}, {1, 0, 0}, {0, 1, 0}), {0.175f, 0.02f, 0.55f}, 0.008f, TIE_PLATE);
        for (int side = -1; side <= 1; side += 2)
        {
            TreadPlate(mesh, {side > 0 ? 0.19f : -0.64f, TIE_FLOOR + 0.002f, 0.95f}, {0.45f, 0, 0}, {0, 0, 0.55f}, 0.05f, TIE_RUBBER, TIE_DARK);
        }
    }
    // The window: the ring's deep collar and tunnel, the flat-topped octagon and its eight struts from the corners,
    // bolts round the collar, glass.
    {
        const float rim = TieRing(0);
        const float tunnel[5][2] = {{0, rim + 0.10f}, {0, rim + 0.005f}, {0.012f, rim - 0.005f}, {0.12f, rim - 0.005f}, {0.13f, rim + 0.09f}};
        Lathe(mesh, MakeFrame({0, TIE_BALL.y, TIE_WINDOW_Z - 0.005f}, {1, 0, 0}, {0, 0, 1}), tunnel, 5, 48, TIE_FRAME);
        for (int i = 0; i < 20; ++i)
        {
            const float a = (i + 0.5f) * (6.2831853f / 20);
            Box(mesh, MakeFrame({(rim + 0.045f) * cosf(a), TIE_BALL.y + (rim + 0.045f) * sinf(a), TIE_WINDOW_Z + 0.128f}, {1, 0, 0}, {0, 1, 0}), {0.0075f, 0.0075f, 0.005f}, 0.002f, TIE_METAL);
        }
        Vec3 octagon[9];
        for (int i = 0; i <= 8; ++i)
        {
            const float a = i * (6.2831853f / 8) + 0.39270f;
            octagon[i] = {TIE_OCTAGON * cosf(a), TIE_BALL.y + TIE_OCTAGON * sinf(a), TIE_WINDOW_Z};
        }
        for (int i = 0; i < 8; ++i)
        {
            const Vec3 side[2] = {octagon[i], octagon[i + 1]};
            const Vec3 mid = (side[0] + side[1]) * 0.5f;
            const Frame bar = MakeFrame(mid + Vec3{0, 0, 0.04f}, Normalized(side[1] - side[0]), Normalized(Vec3{mid.x, mid.y - TIE_BALL.y, 0}));
            Box(mesh, bar, {0.5f * Length(side[1] - side[0]) + 0.022f, 0.0225f, 0.04f}, 0.006f, TIE_FRAME);
            const float a0 = i * 0.785398f + 0.39270f, a1 = (i + 1) * 0.785398f + 0.39270f;
            const Vec3 strutOut = {rim * cosf(a0), TIE_BALL.y + rim * sinf(a0), TIE_WINDOW_Z};
            const Vec3 along = Normalized(strutOut - octagon[i]);
            const Frame strut = MakeFrame((octagon[i] + strutOut) * 0.5f + Vec3{0, 0, 0.04f}, along, Cross({0, 0, 1}, along));
            Box(mesh, strut, {0.5f * Length(strutOut - octagon[i]), 0.0225f, 0.04f}, 0.006f, TIE_FRAME);
            // the pane out to the ring itself (its outer edge follows the ring's curve, not a chord between the struts:
            // a gap of open air showed along it, idren2023 2026-10-27)
            constexpr int ARC = 8;
            Vec3 pane[ARC + 3];
            pane[0] = octagon[i];
            for (int k = 0; k <= ARC; ++k)
            {
                const float a = a0 + (a1 - a0) * k / ARC;
                pane[1 + k] = {(rim + 0.01f) * cosf(a), TIE_BALL.y + (rim + 0.01f) * sinf(a), TIE_WINDOW_Z};
            }
            pane[ARC + 2] = octagon[i + 1];
            PlanarPolygon(mesh, pane, ARC + 3, {1, 0, 0}, TIE_GLASS);
        }
        const Vec3 centre = {0, TIE_BALL.y, TIE_WINDOW_Z};
        for (int i = 0; i < 8; ++i)
        {
            const float st[3][2] = {{0.5f, 0.5f}, {0, 0}, {1, 0}};
            Triangle(mesh, centre, octagon[i], octagon[i + 1], st, TIE_GLASS);
        }
    }
    // The crescent cluster in the ring's lower arc (Bill George's replica): boxes, knobs and small tanks along it, the
    // game's own gauge cluster (its screens and dials) in the middle.
    {
        const float rim = TieRing(0);
        for (int k = 0; k < 9; ++k)
        {
            const float a = 3.1415926f * (1.22f + k * 0.07f);
            if (k == 4)
            {
                continue;
            }
            const Vec3 at = {(rim - 0.06f) * cosf(a), TIE_BALL.y + (rim - 0.06f) * sinf(a), TIE_WINDOW_Z + 0.09f};
            const Frame box = MakeFrame(at, {-sinf(a), cosf(a), 0}, {cosf(a), sinf(a), 0});
            Box(mesh, Offset(box, 0, 0, 0), {0.045f, 0.035f, 0.06f}, 0.006f, k % 3 ? TIE_PLATE : TIE_FRAME);
            if (k % 2)
            {
                const float tank[4][2] = {{0, 0.022f}, {0.004f, 0.026f}, {0.07f, 0.026f}, {0.075f, 0}};
                Lathe(mesh, MakeFrame(at + Vec3{0, 0.04f, 0.0f}, {1, 0, 0}, {0, 0, 1}), tank, 4, 10, TIE_METAL);
            }
            else
            {
                KnobRow(mesh, MakeFrame(at + Vec3{0, 0.036f, 0.0f}, {1, 0, 0}, {0, 0.3f, 1}), 2, 0.03f, 0.008f, detail);
            }
        }
        StockPieces(mesh, stock, stockCount, TIE_COCKPIT, TIE_FX, TIE_GLASS);
    }
    // No control column of the set's (headset run 18, idren2023: the yoke doubled the flight stick; only the stick stays):
    // its box head with toggles and the targeting screen sits on the wall below the window instead.
    {
        const Frame unit = TieFacing(Normalized(Vec3{0, -0.80f, -0.60f}), TIE_RADIUS - 0.08f);
        Box(mesh, Offset(unit, 0, 0, -0.03f), {0.11f, 0.07f, 0.05f}, 0.01f, TIE_FRAME);
        ToggleBank(mesh, MakeFrame(unit.At(0, 0.07f, -0.03f), unit.x, unit.z * -1.0f), 4, 0.04f, detail);
        TieScreen(mesh, Offset(unit, 0, 0, 0.022f), 0, 0, 0.06f, 0.045f, GROUP_TARGET);
    }
    // The walls' modules: left, a switch box, a round gauge (health) and a dark niche with the score; right, a vertical
    // lamp strip, a keypad and the round green scope (the minimap); the small screen over the window (weapons); the red rod
    // clusters fanning down from above at the left and right.
    for (int side = -1; side <= 1; side += 2)
    {
        const float s = static_cast<float>(side);
        const Frame upper = TieFacing(Normalized(Vec3{s * 0.85f, 0.10f, 0.30f}), TIE_RADIUS - 0.05f);
        KitPanel(mesh, upper, 0.16f, 0.12f, 301 + side, look);
        const Frame scope = TieFacing(Normalized(Vec3{s * 0.80f, -0.20f, -0.45f}), TIE_RADIUS - 0.07f);
        Box(mesh, Offset(scope, 0, 0, -0.03f), {0.11f, 0.11f, 0.03f}, 0.01f, TIE_FRAME);
        // the tanks' stock screen in the housing (idren2023 2026-11-09; was a round glass in a turned bezel)
        StockScreen(mesh, Offset(scope, 0, 0, 0.002f), 0.07f, 0.07f, SCREEN_TEXEL_METERS * 0.07f / 0.0675f, TIE_SCREENS);
        SetDisplayScreen(&TIE_SCREENS);
        AddDisplay(side < 0 ? GROUP_HEALTH : GROUP_MAP, Offset(scope, 0, 0, 0.002f), 0, 0, 0.13f, 0.13f);
        SetDisplayScreen(nullptr);
        if (side < 0)
        {
            ToggleBank(mesh, TieFacing(Normalized(Vec3{-0.75f, -0.35f, 0.10f}), TIE_RADIUS - 0.04f), 6, 0.03f, detail);
            const Frame niche = TieFacing(Normalized(Vec3{-0.80f, 0.20f, -0.30f}), TIE_RADIUS - 0.04f);
            Box(mesh, Offset(niche, 0, 0, -0.01f), {0.10f, 0.13f, 0.02f}, 0.008f, TIE_DARK);
            TieScreen(mesh, niche, 0, 0.04f, 0.07f, 0.04f, GROUP_SCORE);
        }
        else
        {
            const Frame strip = TieFacing(Normalized(Vec3{0.72f, 0.05f, -0.55f}), TIE_RADIUS - 0.03f);
            Box(mesh, strip, {0.02f, 0.15f, 0.012f}, 0.005f, TIE_FRAME);
            for (int k = 0; k < 8; ++k)
            {
                Box(mesh, Offset(strip, 0, -0.13f + k * 0.037f, 0.013f), {0.012f, 0.012f, 0.003f}, 0.002f, k % 3 ? TIE_ORANGE : TIE_WHITE);
            }
            const Frame keypad = TieFacing(Normalized(Vec3{0.78f, -0.40f, 0.05f}), TIE_RADIUS - 0.04f);
            Box(mesh, Offset(keypad, 0, 0, -0.01f), {0.05f, 0.04f, 0.012f}, 0.004f, TIE_FRAME);
            for (int k = 0; k < 12; ++k)
            {
                Box(mesh, Offset(keypad, -0.033f + (k % 4) * 0.022f, -0.025f + (k / 4) * 0.025f, 0.004f), {0.008f, 0.008f, 0.004f}, 0.002f, k == 5 ? TIE_RED : TIE_PLATE);
            }
        }
        const Vec3 base = {s * 0.55f, 2.35f, 1.80f};
        Box(mesh, MakeFrame(base, {1, 0, 0}, {0, 1, 0}), {0.07f, 0.04f, 0.06f}, 0.01f, TIE_FRAME);
        for (int k = 0; k < 5; ++k)
        {
            const Vec3 dir = Normalized(Vec3{-s * (0.15f + k * 0.08f), -0.8f, -0.55f + k * 0.06f});
            const float length = 0.30f + (k % 3) * 0.05f;
            const Vec3 rod[2] = {base + Vec3{(k - 2) * 0.02f, -0.03f, 0}, base + Vec3{(k - 2) * 0.02f, -0.03f, 0} + dir * length};
            Tube(mesh, rod, 2, 0.02f, 8, TIE_ROD);
        }
    }
    {
        const Frame top = TieFacing(Normalized(Vec3{0, 0.55f, -0.75f}), TIE_RADIUS - 0.05f);
        Box(mesh, Offset(top, 0, 0, -0.02f), {0.13f, 0.08f, 0.025f}, 0.008f, TIE_FRAME);
        TieScreen(mesh, top, 0, 0, 0.09f, 0.05f, GROUP_AMMUNITION);
    }
    // The overhead hatch (the set: a slotted plate with a cast locking wheel, above and behind the head): the ring
    // band with its dark recesses and bolts, the plate with a central spine and four glazed slots fore-and-aft, the
    // wheel on the spine, a hinge at the rear, two grab handles.
    {
        const Vec3 axis = TIE_HATCH_AXIS;
        const Frame plate = MakeFrame(TieAt(axis, 1.10f), {1, 0, 0}, Cross(-axis, Vec3{1, 0, 0})); // z toward the centre
        const float ring[6][2] = {{-0.09f, 0.53f}, {0, 0.53f}, {0.004f, 0.51f}, {0.004f, 0.42f}, {0, 0.40f}, {-0.02f, 0.40f}};
        Lathe(mesh, Upright(plate), ring, 6, 40, TIE_FRAME);
        for (int k = 0; k < 16; ++k)
        {
            const float a = k * (6.2831853f / 16);
            Box(mesh, Offset(plate, 0.495f * cosf(a), 0.495f * sinf(a), 0.008f), {0.008f, 0.008f, 0.004f}, 0.002f, TIE_METAL);
            if (k % 2 == 0)
            {
                Box(mesh, Offset(plate, 0.465f * cosf(a + 0.2f), 0.465f * sinf(a + 0.2f), 0.005f), {0.025f, 0.025f, 0.003f}, 0, TIE_DARK);
            }
        }
        // the plate: solid between the slots; slots fore-and-aft (along the plate's y), two either side of the spine
        const float slotX[4] = {-0.27f, -0.14f, 0.14f, 0.27f}, slotHalf[4] = {0.17f, 0.26f, 0.26f, 0.17f};
        for (int k = 0; k < 4; ++k)
        {
            const float x0 = slotX[k] - 0.05f, x1 = slotX[k] + 0.05f, h = slotHalf[k];
            const Vec3 pane[4] = {plate.At(x0, h, -0.01f), plate.At(x1, h, -0.01f), plate.At(x1, -h, -0.01f), plate.At(x0, -h, -0.01f)};
            XwgPaneOf(mesh, pane, 4, TIE_GLASS);
            // the plate beyond the slot's ends, out to the ring
            const float reach = sqrtf(0.40f * 0.40f - (fabsf(slotX[k]) + 0.05f) * (fabsf(slotX[k]) + 0.05f));
            for (int e = -1; e <= 1; e += 2)
            {
                Box(mesh, Offset(plate, slotX[k], e * (h + reach) * 0.5f, 0.006f), {0.05f, (reach - h) * 0.5f + 0.004f, 0.008f}, 0.002f, TIE_PLATE);
            }
        }
        // the solid parts between the slots: the spine, the bars, the outer edges
        const float solids[4][2] = {{-0.09f, 0.09f}, {0.19f, 0.22f}, {-0.22f, -0.19f}, {0.32f, 0.40f}};
        for (int k = 0; k < 5; ++k)
        {
            const float x0 = k < 4 ? solids[k][0] : -0.40f, x1 = k < 4 ? solids[k][1] : -0.32f;
            const float h = sqrtf(0.40f * 0.40f - (fabsf(x0) > fabsf(x1) ? x0 * x0 : x1 * x1));
            Box(mesh, Offset(plate, (x0 + x1) * 0.5f, 0, 0.006f), {(x1 - x0) * 0.5f, h, 0.008f}, 0.003f, TIE_PLATE);
        }
        // the locking wheel: a ring of knobs, a disc with six holes, a boss, a short stem down
        const Frame wheel = Offset(plate, 0, 0, 0.015f);
        const float rim[4][2] = {{0, 0.12f}, {0.03f, 0.12f}, {0.04f, 0.10f}, {0.04f, 0.08f}};
        Lathe(mesh, Upright(wheel), rim, 4, 28, TIE_METAL);
        for (int k = 0; k < 10; ++k)
        {
            const float a = k * 0.628f;
            Box(mesh, Offset(wheel, 0.125f * cosf(a), 0.125f * sinf(a), 0.02f), {0.0125f, 0.0125f, 0.02f}, 0.004f, TIE_WEAR);
        }
        const float disc[2][2] = {{0.035f, 0.08f}, {0.035f, 0}};
        Lathe(mesh, Upright(wheel), disc, 2, 28, TIE_FRAME);
        for (int k = 0; k < 6; ++k)
        {
            const float a = k * 1.047f;
            const float hole[2][2] = {{0.036f, 0.012f}, {0.036f, 0}};
            Lathe(mesh, Upright(Offset(wheel, 0.05f * cosf(a), 0.05f * sinf(a), 0)), hole, 2, 10, TIE_DARK);
        }
        const float bossShape[4][2] = {{0.035f, 0.025f}, {0.06f, 0.025f}, {0.07f, 0.015f}, {0.135f, 0.012f}};
        Lathe(mesh, Upright(wheel), bossShape, 4, 14, TIE_WEAR);
        const Vec3 hingeA = plate.At(-0.15f, 0.53f, -0.02f), hingeB = plate.At(0.15f, 0.53f, -0.02f);
        const Vec3 hinge[2] = {hingeA, hingeB};
        Tube(mesh, hinge, 2, 0.03f, 12, TIE_FRAME);
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 handle[4] = {plate.At(side * 0.05f, side * 0.15f + 0.07f, 0.01f), plate.At(side * 0.05f, side * 0.15f + 0.07f, 0.06f),
                                    plate.At(side * 0.05f, side * 0.15f - 0.07f, 0.06f), plate.At(side * 0.05f, side * 0.15f - 0.07f, 0.01f)};
            Tube(mesh, handle, 4, 0.0125f, 8, TIE_METAL);
        }
    }
    // The rear hatch: a round plate on the ball's back with a small hexagonal window, a bolt ring, red lamps round it
    // (the bomber's ball opens into its tube instead: the bombardier's station).
    if (isBomber)
    {
        TieBomberStation(mesh, look);
    }
    else
    {
        const Frame plate = TieFacing(TIE_REAR_AXIS, 1.17f);
        Vec3 outline[33];
        for (int k = 0; k <= 32; ++k)
        {
            outline[k] = plate.At(0.40f * cosf(k * 0.19635f), 0.40f * sinf(k * 0.19635f), 0);
        }
        Tube(mesh, outline, 33, 0.025f, 8, TIE_FRAME, false);
        for (int k = 0; k < 6; ++k)
        {
            const float a0 = k * 1.0472f, a1 = (k + 1) * 1.0472f;
            const Vec3 inner0 = plate.At(0.18f * cosf(a0), 0.18f * sinf(a0), 0), inner1 = plate.At(0.18f * cosf(a1), 0.18f * sinf(a1), 0);
            const Vec3 outer0 = plate.At(0.40f * cosf(a0), 0.40f * sinf(a0), 0), outer1 = plate.At(0.40f * cosf(a1), 0.40f * sinf(a1), 0);
            Quad(mesh, inner0, outer0, outer1, inner1, TIE_PLATE);
            const Vec3 frame[2] = {inner0, inner1};
            Tube(mesh, frame, 2, 0.02f, 6, TIE_FRAME);
            Box(mesh, Offset(plate, 0.23f * cosf(a0 + 0.52f), 0.23f * sinf(a0 + 0.52f), 0.01f), {0.008f, 0.008f, 0.005f}, 0.002f, TIE_METAL);
            IndicatorLamp(mesh, Upright(Offset(plate, 0.31f * cosf(a0), 0.31f * sinf(a0), 0.005f)), 0.01f, TIE_FRAME, TIE_RED);
            const Vec3 pane[3] = {plate.At(0, 0, -0.005f), inner1 + plate.z * -0.005f, inner0 + plate.z * -0.005f};
            const float st[3][2] = {{0.5f, 0.5f}, {1, 0}, {0, 0}};
            Triangle(mesh, pane[0], pane[1], pane[2], st, TIE_GLASS);
        }
    }
    // Side consoles (the throttle's track and the lever's gate on the left), the stick's pedestal, the seat.
    for (int side = -1; side <= 1; side += 2)
    {
        const float inner = side * 0.19f, outer = side * 0.56f, mid = (inner + outer) * 0.5f;
        Box(mesh, MakeFrame({mid, (0.88f + TIE_FLOOR) * 0.5f, 1.42f}, {1, 0, 0}, {0, 1, 0}), {fabsf(outer - inner) * 0.5f, (0.88f - TIE_FLOOR) * 0.5f, 0.34f}, 0.012f, TIE_FRAME);
        KitPanel(mesh, MakeFrame({mid + side * 0.06f, 0.881f, 1.50f}, {1, 0, 0}, {0, 0, -1}), // up (the left one faced down)
                  fabsf(outer - inner) * 0.5f - 0.07f, 0.20f, 111 + side, look);
    }
    Box(mesh, MakeFrame({-0.24f, 0.886f, 1.22f}, {1, 0, 0}, {0, 0, -1}), {0.022f, 0.15f, 0.004f}, 0.002f, TIE_FRAME);
    Box(mesh, MakeFrame({-0.24f, 0.888f, 1.22f}, {1, 0, 0}, {0, 0, -1}), {0.010f, 0.14f, 0.004f}, 0.002f, TIE_DARK);
    Box(mesh, MakeFrame({-0.36f, 0.886f, 1.40f}, {1, 0, 0}, {0, 0, -1}), {0.02f, 0.11f, 0.004f}, 0.002f, TIE_FRAME);
    Box(mesh, MakeFrame({-0.36f, 0.888f, 1.40f}, {1, 0, 0}, {0, 0, -1}), {0.009f, 0.10f, 0.004f}, 0.002f, TIE_DARK);
    {
        const float h = XWG_STICK_BASE.y - TIE_FLOOR;
        const float rings[7][2] = {{0, 0.10f}, {0.015f, 0.10f}, {0.02f, 0.085f}, {0.05f, 0.075f}, {h - 0.04f, 0.075f}, {h - 0.02f, 0.088f}, {h, 0.088f}};
        Lathe(mesh, MakeFrame({XWG_STICK_BASE.x, TIE_FLOOR, XWG_STICK_BASE.z}, {1, 0, 0}, {0, 1, 0}), rings, 7, 18, TIE_FRAME);
    }
    const SeatLook seat = {0.22f, 0.62f, 0.26f, true, false, TIE_PAD, TIE_FRAME, TIE_METAL, TIE_DARK, TIE_DARK};
    Seat(mesh, TIE_EYE, TIE_FLOOR, seat);
}

void BuildTieFighter(Mesh& mesh)
{
    TieCockpit(mesh, StockVertices("EXTERIOR_TIE"), StockCount("EXTERIOR_TIE"), StockVertices("STOCK_TIE"), StockCount("STOCK_TIE"));
}

void BuildTieBomber(Mesh& mesh)
{
    TieCockpit(mesh, StockVertices("EXTERIOR_TIEBOMBER"), StockCount("EXTERIOR_TIEBOMBER"), StockVertices("STOCK_TIEBOMBER"), StockCount("STOCK_TIEBOMBER"), true);
    _isTieRearOpen = false;
}

// The TIE bomber's bombardier station (idren2023 2026-10-27: the bomber's second seat, ODF TURRET1 "bomber", gets a
// turret-style chair at the back of the pilot pod's tube that turns within its aim limits, with a viewscreen for
// where the bombs will fall). Laid out as the combat speeder gunner's cabin (SPEEDER_CABIN_PARTS): part 0 the tube
// behind the pilot's ball (the ball's back open into it), part 1 the turning chair with its yoke and the display's
// curved rails, part 2 the bomb-sight display on the rails (pitched with the aim), parts 3 and 4 its right and left
// end-stop lamps lit, part 5 the bomb sight's picture behind the display's glass (the scope pass: a camera along the
// bombs' line). GunnerSeat turns part 1 about the vertical through the bombardier's eye by the turret's yaw and
// part 2 also by its pitch. In the pilot seat parts 0 to 2 are drawn at rest.
void TieBomberStation(Mesh& mesh, const FighterLook& look)
{
    const DetailLook detail = AsDetail(look, TIE_ORANGE);
    const Vec3 eye = {TIE_BOMBER_GUNNER_EYE[0], TIE_BOMBER_GUNNER_EYE[1], TIE_BOMBER_GUNNER_EYE[2]};
    constexpr float RADIUS = TIE_BOMBER_TUBE_RADIUS, AXIS_Y = TIE_BOMBER_TUBE_Y, FRONT = TIE_BOMBER_TUBE_FRONT, BACK = TIE_BOMBER_TUBE_BACK;

    mesh.part = 0;

    // The tube: the pod's cylinder in the ball's faceted grey, ribs round it, a flat floor with a tread plate, the

    // back closed by a bulkhead with racks (the cutaway's avionics and life support), a hatch overhead.
    {
        const float wall[4][2] = {{0, RADIUS}, {(BACK - FRONT) * 0.5f, RADIUS}, {BACK - FRONT + 0.03f, RADIUS}, {BACK - FRONT + 0.031f, 0.0f}};
        Lathe(mesh, MakeFrame({0, AXIS_Y, FRONT}, {1, 0, 0}, {0, 0, 1}), wall, 4, 40, TIE_RIBS);
        for (int k = 0; k < 5; ++k)
        {
            Vec3 ring[25];
            const float z = FRONT + 0.35f + k * (BACK - FRONT - 0.5f) / 4;
            for (int i = 0; i <= 24; ++i)
            {
                const float a = i * (6.2831853f / 24);
                ring[i] = {(RADIUS - 0.02f) * cosf(a), AXIS_Y + (RADIUS - 0.02f) * sinf(a), z};
            }
            Tube(mesh, ring, 25, 0.022f, 4, TIE_SEAM, false);
        }
        const float floorHalf = sqrtf(RADIUS * RADIUS - (AXIS_Y - TIE_BOMBER_TUBE_FLOOR) * (AXIS_Y - TIE_BOMBER_TUBE_FLOOR)) - 0.01f;
        TiledQuad(mesh, {-floorHalf, TIE_BOMBER_TUBE_FLOOR, FRONT + 0.20f}, {2 * floorHalf, 0, 0}, {0, 0, BACK - FRONT - 0.2f}, TIE_DARK);
        TreadPlate(mesh, {-0.30f, TIE_BOMBER_TUBE_FLOOR + 0.002f, eye.z - 0.70f}, {0.60f, 0, 0}, {0, 0, 0.95f}, 0.05f, TIE_RUBBER, TIE_DARK);
        Vec3 disc[32];
        for (int i = 0; i < 32; ++i)
        {
            const float a = i * (6.2831853f / 32);
            disc[i] = {RADIUS * cosf(a), AXIS_Y + RADIUS * sinf(a), BACK};
        }
        PlanarPolygon(mesh, disc, 32, {1, 0, 0}, TIE_PLATE);
        for (int side = -1; side <= 1; side += 2)
        {
            KitPanel(mesh, MakeFrame({side * 0.36f, AXIS_Y + 0.10f, BACK - 0.01f}, {-1, 0, 0}, {0, 1, 0}), 0.16f, 0.18f, 501 + side, look);
            // racks along the walls behind the chair
            const Vec3 wallAt = {side * (RADIUS - 0.10f), AXIS_Y - 0.15f, BACK - 0.55f};
            KitPanel(mesh, MakeFrame(wallAt, {0, 0, static_cast<float>(side)}, {0, 1, 0}), 0.28f, 0.14f, 505 + side, look);
            for (int k = 0; k < 4; ++k)
            {
                IndicatorLamp(mesh, Upright(MakeFrame(wallAt + Vec3{-side * 0.01f, 0.33f, -0.30f + k * 0.2f}, {0, 0, static_cast<float>(side)}, {0, 1, 0})), 0.01f, TIE_FRAME, k % 2 ? TIE_ORANGE : TIE_AMBER);
            }
        }
        // the overhead hatch of the bomber's pod (the cutaway: pilot ingress on top)
        const Frame hatch = MakeFrame({0, AXIS_Y + RADIUS - 0.03f, eye.z - 0.25f}, {1, 0, 0}, {0, 0, -1});
        Vec3 rim[25];
        for (int i = 0; i <= 24; ++i)
        {
            const float a = i * (6.2831853f / 24);
            rim[i] = hatch.At(0.30f * cosf(a), 0.30f * sinf(a), 0);
        }
        Tube(mesh, rim, 25, 0.03f, 8, TIE_FRAME, false);
        ValveWheel(mesh, hatch, 0.13f, 0.03f, TIE_WEAR, TIE_DARK);
    }

    // Part 1: the turning chair on a turntable ring, its column and the twin grips, the display's rails.

    mesh.part = 1;
    const float rail = TIE_BOMBER_SIGHT_DISTANCE;
    const float halfWidth = TIE_BOMBER_SIGHT_HALF_WIDTH, halfHeight = TIE_BOMBER_SIGHT_HALF_HEIGHT;
    {
        const float disc[3][2] = {{0, 0.42f}, {0.03f, 0.42f}, {0.035f, 0.40f}};
        Lathe(mesh, MakeFrame({eye.x, TIE_BOMBER_TUBE_FLOOR, eye.z - 0.05f}, {1, 0, 0}, {0, 1, 0}), disc, 3, 32, TIE_FRAME);
        const SeatLook seat = {0.22f, 0.62f, 0.22f, true, false, TIE_PAD, TIE_FRAME, TIE_METAL, TIE_DARK, TIE_DARK};
        Seat(mesh, eye, TIE_BOMBER_TUBE_FLOOR + 0.03f, seat);
        const Vec3 column[2] = {{eye.x, TIE_BOMBER_TUBE_FLOOR + 0.03f, eye.z - 0.42f}, {eye.x, eye.y - 0.42f, eye.z - 0.40f}};
        Tube(mesh, column, 2, 0.03f, 12, TIE_METAL);
        const Frame head = MakeFrame({eye.x, eye.y - 0.40f, eye.z - 0.38f}, {1, 0, 0}, {0, 1, 0});
        Box(mesh, head, {0.10f, 0.05f, 0.05f}, 0.01f, TIE_FRAME);
        ToggleBank(mesh, MakeFrame(head.At(0, 0.05f, 0), {1, 0, 0}, {0, 0, -1}), 4, 0.04f, detail);
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 arm[2] = {head.At(side * 0.10f, 0, 0), head.At(side * 0.20f, 0.02f, 0.05f)};
            Tube(mesh, arm, 2, 0.014f, 8, TIE_METAL);
            const Vec3 grip[2] = {head.At(side * 0.20f, 0.02f, 0.05f), head.At(side * 0.20f, 0.14f, 0.07f)};
            Tube(mesh, grip, 2, 0.0175f, 10, TIE_RUBBER);
            const float button[3][2] = {{0, 0.0075f}, {0.006f, 0.0075f}, {0.009f, 0}};
            Lathe(mesh, MakeFrame(grip[1], {1, 0, 0}, {0, 1, 0}), button, 3, 10, TIE_RED);
            // the rail: an arc about the eye beside the display, from below its lowest pitch to above its highest
            Vec3 arc[13];
            for (int k = 0; k <= 12; ++k)
            {
                const float a = (-55.0f + k * 9.0f) * 0.0174533f;
                arc[k] = {eye.x + side * (halfWidth + 0.05f), eye.y + rail * sinf(a), eye.z - rail * cosf(a)};
            }
            Tube(mesh, arc, 13, 0.01f, 6, TIE_METAL, false);
            const Vec3 post[2] = {arc[0], {eye.x + side * (halfWidth + 0.05f), TIE_BOMBER_TUBE_FLOOR + 0.03f, arc[0].z}};
            Tube(mesh, post, 2, 0.012f, 6, TIE_FRAME);
        }
    }

    // Part 2: the bomb-sight display on the rails: a dark case, the bezel, a bombing reticle (a ring, a cross, range

    // ticks) and the HUD's target over the picture, sliders on the rails, a dark lamp at each lower corner.

    mesh.part = 2;
    const Vec3 screen = {eye.x, eye.y, eye.z - rail};
    const Frame face = MakeFrame(screen, {1, 0, 0}, {0, 1, 0});
    {
        Box(mesh, Offset(face, 0, 0, -0.03f), {halfWidth + 0.035f, halfHeight + 0.035f, 0.02f}, 0.006f, TIE_FRAME);
        for (int side = -1; side <= 1; side += 2)
        {
            Box(mesh, Offset(face, side * (halfWidth + 0.02f), 0, -0.004f), {0.015f, halfHeight + 0.035f, 0.006f}, 0.003f, TIE_DARK);
            Box(mesh, Offset(face, 0, side * (halfHeight + 0.02f), -0.004f), {halfWidth + 0.035f, 0.015f, 0.006f}, 0.003f, TIE_DARK);
            Box(mesh, Offset(face, side * (halfWidth + 0.05f), 0, -0.02f), {0.02f, 0.03f, 0.02f}, 0.004f, TIE_METAL);
            Rivet(mesh, Offset(face, side * (halfWidth - 0.01f), -halfHeight - 0.02f, 0.003f), 0.006f, TIE_DARK);
        }
        Vec3 ring[25];
        for (int k = 0; k <= 24; ++k)
        {
            const float a = k * (6.2831853f / 24);
            ring[k] = face.At(0.03f * cosf(a), 0.03f * sinf(a), 0.001f);
        }
        Tube(mesh, ring, 25, 0.0012f, 4, TIE_ORANGE, false);
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 bar[2] = {face.At(side * 0.012f, 0, 0.001f), face.At(side * 0.06f, 0, 0.001f)};
            Tube(mesh, bar, 2, 0.0012f, 4, TIE_ORANGE);
            const Vec3 bar2[2] = {face.At(0, side * 0.012f, 0.001f), face.At(0, side * 0.045f, 0.001f)};
            Tube(mesh, bar2, 2, 0.0012f, 4, TIE_ORANGE);
        }
        for (int k = 0; k < 5; ++k)
        {
            Box(mesh, Offset(face, halfWidth - 0.012f, -halfHeight + 0.02f + k * 0.022f, 0.001f), {0.006f, 0.0012f, 0.0004f}, 0, TIE_ORANGE);
        }
        SetDisplayScreen(&TIE_SCREENS); // the tanks' look (idren2023 2026-11-09)
        AddDisplay(GROUP_TARGET, face, -halfWidth + 0.05f, halfHeight - 0.014f, 0.08f, 0.018f, 2);
        StockScreen(mesh, Offset(face, 0, -halfHeight - 0.045f, 0.0f), 0.065f, 0.016f, 0.00008f, TIE_SCREENS, 0.006f);
        AddDisplay(GROUP_AMMUNITION, Offset(face, 0, -halfHeight - 0.045f, 0.0f), 0, 0, 0.10f, 0.022f, 2);
        SetDisplayScreen(nullptr);
    }

    // Parts 3 and 4: the right and left end stops' lamps lit.
    for (int side = 1; side >= -1; side -= 2)
    {
        mesh.part = side > 0 ? 3 : 4;
        Rivet(mesh, Offset(face, side * (halfWidth - 0.01f), -halfHeight - 0.02f, 0.005f), 0.0075f, TIE_RED);
    }

    // Part 5: the bomb sight's picture.

    mesh.part = 5;
    Quad(mesh, face.At(-halfWidth, halfHeight, -0.002f), face.At(halfWidth, halfHeight, -0.002f), face.At(halfWidth, -halfHeight, -0.002f),
         face.At(-halfWidth, -halfHeight, -0.002f), TIE_PICTURE);

    mesh.part = 0;

}



// ---------------------------------------------------------------------------------------------------------------------
// Jedi starfighter (Delta-7, PLAN starfighters): our own cockpit at the real size in place of the game's first-person
// model (rep_1st_cockpit_jedistarfighter, whose textures it uses): a narrow tub under a low faceted canopy, the dash
// with the holo display on its stand in the middle (the minimap), the wedge hull spreading flat to both sides and the
// astromech's dome in its socket on the left wing root (docs/research/starfighter-cockpits-canon.md). Stick, throttle
// and take-off lever (Flight) as in every starfighter. Space: the cockpit model's, the eye at the game's (0, 1.5, 1.5).

enum JedFinish
{
    JED_PANEL,  // rep_1st_cockpit_jedistarfighter: the left panel (lamps), whole
    JED_PILLAR, //   the middle pillar, whole
    JED_HOLO,   //   the holo display (its round frame), whole
    JED_BANDS,  //   ribbed bands, by size
    JED_LOWER,  //   lower panels, by size
    JED_HULL,   // REP_fly_fighter: the hull, by size
    JED_GLASS,  // rep_cockpit_win
    JED_FRAME,
    JED_DARK,
    JED_METAL,
    JED_RUBBER,
    JED_PAD,
    JED_RED,    // Republic red
    JED_DOME,   // the astromech's dome (silver)
    JED_LAMP,
    JED_SCREEN,
    JED_STOCK,  // the ship's own exterior (StockExterior)
    JED_PLATE,  // kitbash plates (charcoal)
    JED_ORANGE, // orange lamps
    JED_CREAM,  // the hull's cream paint (the canopy's rim)
    JED_COCKPIT, // the game's first-person cockpit pieces (StockPieces), their own mapping
    JED_FX,      //   their lamps
    JED_AMBER,   // amber lamps
    JED_REDLAMP, // red lamps
    JED_GREEN,   // green lamps
    JED_YELLOW,  // yellow keys
    JED_BROWN,   // brown rubber foot pads
    JED_PANEL_TONE, // the consoles' gunmetal
    JED_GREY,    // the roll-bar unit and the machinery
    JED_BLACK,   // the horseshoe headrest's black leather
    JED_SCREEN_FRAME,   // the HUD screens' stock frame (texture 5, rep_1st_cockpit_securitycam: the tanks' look)
    JED_SCREEN_GLASS,   //   their glass (scan lines)
    JED_SCREEN_BACKING, //   behind the glass
    JED_FINISH_COUNT
};

const Finish JED_FINISHES[JED_FINISH_COUNT] = {
    XwgTexture(1, 4, 70, 195, 360, 0x8E9296, 0.20f, 14, 0.08f), // panel
    XwgTexture(1, 196, 0, 285, 360, 0x8E9296, 0.25f, 16),       // pillar
    XwgTexture(1, 288, 4, 508, 250, 0x8E9296, 0.30f, 20, 0.35f), // holo
    XwgTexture(1, 0, 362, 512, 440, 0x8A8E92, 0.20f, 14),       // bands
    XwgTexture(1, 0, 442, 512, 512, 0x8A8E92, 0.20f, 14),       // lower panels
    XwgTexture(3, 0, 0, 256, 256, 0xA0A4A8, 0.25f, 16),         // hull
    {{0, 0, 1, 1}, 1, 4, 0x98B0C4, 0.60f, 40, 0, 0.99f},        // glass
    XwgTexture(1, 10, 10, 190, 75, 0xB7BDCC, 0.30f, 18), // frame: the stock art, tinted
    XwgTexture(1, 10, 10, 190, 75, 0xC0C2C8, 0.15f, 10),       // dark (charcoal): the stock's grey metal art, darkened
    {{0, 0, 1, 1}, 1, 0, 0xA0A4A8, 0.60f, 40, 0},               // metal
    XwgTexture(1, 10, 10, 190, 75, 0x41434A, 0.20f, 10), // rubber: the stock art, tinted
    XwgTexture(1, 10, 10, 190, 75, 0x966A4E, 0.25f, 14), // seat padding (brown leather): the stock art, tinted
    {{0, 0, 1, 1}, 1, 0, 0x8A2A24, 0.40f, 24, 0},               // dark red (the astromech's dome)
    {{0, 0, 1, 1}, 1, 0, 0xB8BCC2, 0.60f, 40, 0},               // dome silver
    {{0, 0, 1, 1}, 1, 0, 0x2A8CFF, 0, 1, 1.0f},                 // blue lamp
    {{0, 0, 1, 1}, 1, 0, 0x08121A, 0.50f, 40, 0.05f},           // screen glass
    {{0, 0, 1, 1}, 1, 3, 0x808080, 0.20f, 14, 0},              // the exterior, its own mapping
    XwgTexture(1, 10, 10, 190, 75, 0xB0B4BA, 0.25f, 16),       // kitbash plate: the grey metal art
    {{0, 0, 1, 1}, 1, 0, 0xFF9A30, 0, 1, 1.0f},                // orange lamp
    {{0, 0, 1, 1}, 1, 0, 0xD8CCB0, 0.30f, 18, 0},              // cream
    {{0, 0, 1, 1}, 1, 1, 0x808080, 0.25f, 16, 0},              // stock cockpit pieces
    {{0, 0, 1, 1}, 1, 2, 0xC0C0C0, 0, 1, 1.0f},                // their lamps
    {{0, 0, 1, 1}, 1, 0, 0xFFA020, 0, 1, 1.0f},                // amber lamp
    {{0, 0, 1, 1}, 1, 0, 0xFF3020, 0, 1, 1.0f},                // red lamp
    {{0, 0, 1, 1}, 1, 0, 0x40FF60, 0, 1, 1.0f},                // green lamp
    {{0, 0, 1, 1}, 1, 0, 0xFFD040, 0, 1, 0.8f},                // yellow key
    {{0, 0, 1, 1}, 1, 0, 0x5A3A26, 0.15f, 8, 0},               // brown rubber
    XwgTexture(1, 0, 442, 512, 512, 0xA0A4AA, 0.35f, 20),      // console gunmetal: the stock's lower panel art
    XwgTexture(1, 0, 442, 512, 512, 0xD0DDEA, 0.35f, 20), // machinery grey: the stock art, tinted
    XwgTexture(1, 10, 10, 190, 75, 0x292929, 0.30f, 16), // black leather: the stock art, tinted
    ScreenFrameFinish(),                                                    // stock screen frame
    ScreenGlassFinish(REP_SCREEN_ART),                                      // its glass
    ScreenBackingFinish(REP_SCREEN_ART),                                    // behind the glass
};

// The HUD screens in the tanks' look (idren2023 2026-11-09: every vehicle's HUD as the tanks').
const StockScreenLook JED_SCREENS = {&REP_SCREEN_ART, JED_SCREEN_FRAME, JED_SCREEN_GLASS, JED_SCREEN_BACKING, JED_DARK};

constexpr Vec3 JED_EYE = {0.0f, 1.5f, 1.5f};
/// The tub: a long pointed oval under the ship's own canopy (docs/research/cockpits/jedi.md): floor, sill (elbow
/// height), half width, the middle of its length, its length ahead of and behind that; the centre column's face,
/// the wing panels' span round the wall, the shelf's top behind the seat, the astromech dome's base.
constexpr float JED_FLOOR = 0.50f, JED_SILL = 1.10f, JED_HALF = 0.52f, JED_OVAL_Z = 1.60f, JED_OVAL_FRONT = 0.92f, JED_OVAL_BACK = 0.80f;
constexpr float JED_COLUMN_Z = 0.82f, JED_WING_0 = 0.30f, JED_WING_1 = 0.95f, JED_SHELF_Y = 0.95f;
/// The side consoles' tops: the right one at the sill's height less 12 cm; the left one (the throttle's and the
/// take-off lever's deck) just under the throttle's hinge (Flight: the eye + (-0.25, -0.64, -0.27)) and the lever's
/// carriage (the eye + (-0.36, -0.62, -0.10), 2.6 cm tall): headset run 19, at 0.98 it hid both but their knobs.
constexpr float JED_CONSOLE_TOP = 0.98f, JED_LEVER_DECK = 0.854f;

void JedScreen(Mesh& mesh, const Frame& face, float x, float y, float halfWidth, float halfHeight, int group)
{
    // the tanks' stock screen (idren2023 2026-11-09), in a case let into the panel
    const Frame glass = Offset(face, x, y, 0.004f);
    StockScreen(mesh, glass, halfWidth, halfHeight, SCREEN_TEXEL_METERS * halfHeight / 0.0675f, JED_SCREENS, 0.012f);
    SetDisplayScreen(&JED_SCREENS);
    AddDisplay(group, glass, 0, 0, halfWidth * 1.9f, halfHeight * 1.9f);
    SetDisplayScreen(nullptr);
}

// A point on the tub's wall at an angle round it (0 straight ahead, positive to the right) and a height: a long
// pointed oval, narrow at the dash and widest at the seat (docs/research/cockpits/jedi.md), moved inward by `inset`.
Vec3 JedOval(float angle, float y, float inset = 0)
{
    const float c = cosf(angle);
    const float half = (JED_HALF - inset) * (c > 0 ? 1.0f - 0.45f * c * c : 1.0f);
    const float depth = (c > 0 ? JED_OVAL_FRONT : JED_OVAL_BACK) - inset;
    return {half * sinf(angle), y, JED_OVAL_Z - depth * c};
}

// A kitbashed panel on the tub's wall: a chord across the angles a0..a1 between two heights, facing inward.
void JedWallPanel(Mesh& mesh, float a0, float a1, float y0, float y1, uint32_t seed, const FighterLook& look)
{
    const Vec3 p0 = a0 < a1 ? JedOval(a0, 0, 0.012f) : JedOval(a1, 0, 0.012f);
    const Vec3 p1 = a0 < a1 ? JedOval(a1, 0, 0.012f) : JedOval(a0, 0, 0.012f);
    const Vec3 middle = (p0 + p1) * 0.5f + Vec3{0, (y0 + y1) * 0.5f, 0};
    KitPanel(mesh, MakeFrame(middle, Normalized(p1 - p0), {0, 1, 0}), 0.5f * Length(p1 - p0) - 0.01f, (y1 - y0) * 0.5f, seed, look);
}

// A frame on the wall at an angle and height, facing into the tub.
Frame JedWallFrame(float angle, float y, float inset)
{
    const Vec3 a = JedOval(angle - 0.05f, y, inset), b = JedOval(angle + 0.05f, y, inset);
    return MakeFrame((a + b) * 0.5f, Normalized(b - a), {0, 1, 0});
}

void BuildJediFighter(Mesh& mesh)
{
    const FighterLook look = {JED_PLATE, JED_FRAME, JED_METAL, JED_DARK, JED_SCREEN, {JED_AMBER, JED_REDLAMP, JED_AMBER, JED_LAMP}};
    // The ship's own hull: the cream arrowhead with its maroon chevrons, the canopy's frame, the engines.
    StockExterior(mesh, StockVertices("EXTERIOR_JEDI"), StockCount("EXTERIOR_JEDI"), JED_STOCK, JED_GLASS);
    // The game's own arched holo display on the centre column (the main screen); the canopy's glass is the hull's own.
    StockPieces(mesh, StockVertices("STOCK_JEDI"), StockCount("STOCK_JEDI"), JED_COCKPIT, JED_FX, JED_GLASS);
    AddDisplay(GROUP_MAP, MakeFrame({0, 1.19f, 0.888f}, {1, 0, 0}, {0, 1, 0}), 0, 0, 0.16f, 0.16f);
    // The tub (the film's cockpit is dark: charcoal satin metal): the floor grating with ribbed panels, the walls up to
    // the sill at elbow height, the sill's bevelled cream lip with a dark seal.
    {
        constexpr int STEPS = 48;
        Vec3 floorRing[STEPS], seal[STEPS + 1];
        for (int i = 0; i <= STEPS; ++i)
        {
            const float a = i * (6.2831853f / STEPS);
            seal[i] = JedOval(a, JED_SILL - 0.012f, 0.012f);
            if (i == STEPS)
            {
                break;
            }
            floorRing[i] = JedOval(a, JED_FLOOR);
            const float a1 = (i + 1) * (6.2831853f / STEPS);
            Quad(mesh, JedOval(a1, JED_SILL), JedOval(a, JED_SILL), JedOval(a, JED_FLOOR), JedOval(a1, JED_FLOOR), JED_DARK);
            Quad(mesh, JedOval(a1, JED_SILL + 0.012f, -0.04f), JedOval(a, JED_SILL + 0.012f, -0.04f), JedOval(a, JED_SILL, 0.0f),
                 JedOval(a1, JED_SILL, 0.0f), JED_CREAM);
        }
        PlanarPolygon(mesh, floorRing, STEPS, {1, 0, 0}, JED_DARK);
        Tube(mesh, seal, STEPS + 1, 0.012f, 6, JED_RUBBER, false);
        TreadPlate(mesh, {-0.18f, JED_FLOOR + 0.002f, 0.95f}, {0.36f, 0, 0}, {0, 0, 0.55f}, 0.04f, JED_PLATE, JED_METAL);
        for (int side = -1; side <= 1; side += 2)
        {
            Box(mesh, MakeFrame({side * 0.15f, JED_FLOOR + 0.008f, 0.88f}, {1, 0, 0}, {0, 0, -1}), {0.09f, 0.125f, 0.008f}, 0.004f, JED_BROWN);
        }
    }
    // The walls: kitbashed consoles, the side consoles (the throttle's slot on the left with its three lamps, the blue
    // keypad on the right), the red-over-green lamp pairs at shoulder height (the brightest things in the film's dark
    // cockpit).
    for (int side = -1; side <= 1; side += 2)
    {
        const float s = static_cast<float>(side);
        JedWallPanel(mesh, s * 1.10f, s * 1.75f, JED_FLOOR + 0.06f, JED_SILL - 0.30f, 121 + side, look);
        JedWallPanel(mesh, s * 2.45f, s * 3.05f, JED_FLOOR + 0.04f, 0.90f, 129 + side, look);
        for (int k = 0; k < 2; ++k)
        {
            // on the wall's face (2026-11-11: 4 cm in from it they floated)
            const Frame lamp = JedWallFrame(s * (1.55f + k * 0.10f), JED_SILL - 0.04f, 0.014f);
            IndicatorLamp(mesh, Upright(Offset(lamp, 0, 0.0f, 0)), 0.007f, JED_FRAME, JED_REDLAMP);
            IndicatorLamp(mesh, Upright(Offset(lamp, 0, -0.03f, 0)), 0.007f, JED_FRAME, JED_GREEN);
        }
        const float inner = s * 0.20f, outer = s * 0.48f, mid = (inner + outer) * 0.5f;
        const float topY = side < 0 ? JED_LEVER_DECK : JED_CONSOLE_TOP;
        Box(mesh, MakeFrame({mid, (topY + JED_FLOOR) * 0.5f, 1.28f}, {1, 0, 0}, {0, 1, 0}), {fabsf(outer - inner) * 0.5f, (topY - JED_FLOOR) * 0.5f, 0.24f}, 0.015f, JED_PANEL_TONE);
        if (side < 0)
        {
            // the lever deck (flat, z up, y ahead): the throttle's slot, the take-off lever's slot ahead of its hinge (it
            // lies ahead when flying), three lamps outboard of them, a low kit strip at the front
            const Frame top = MakeFrame({mid, topY + 0.001f, 1.28f}, {1, 0, 0}, {0, 0, -1});
            Box(mesh, MakeFrame({-0.25f, topY + 0.003f, 1.23f}, {1, 0, 0}, {0, 0, -1}), {0.022f, 0.13f, 0.004f}, 0.002f, JED_FRAME);
            Box(mesh, MakeFrame({-0.36f, topY + 0.003f, 1.34f}, {1, 0, 0}, {0, 0, -1}), {0.02f, 0.11f, 0.004f}, 0.002f, JED_FRAME);
            const int colours[3] = {JED_REDLAMP, JED_AMBER, JED_AMBER};
            for (int k = 0; k < 3; ++k)
            {
                IndicatorLamp(mesh, Upright(Offset(top, -0.085f, -0.06f + k * 0.04f, 0.002f)), 0.007f, JED_FRAME, colours[k]);
            }
            KitPanel(mesh, Offset(top, 0, 0.205f, 0.002f), 0.12f, 0.028f, 133, look);
        }
        else
        {
            const Frame top = MakeFrame({mid, topY + 0.001f, 1.28f}, {s, 0, 0}, {0, 0.6f, -1});
            Box(mesh, Offset(top, 0, 0, 0.004f), {0.08f, 0.06f, 0.004f}, 0.003f, JED_DARK);
            Quad(mesh, top.At(-0.07f, 0.05f, 0.009f), top.At(0.07f, 0.05f, 0.009f), top.At(0.07f, -0.05f, 0.009f), top.At(-0.07f, -0.05f, 0.009f), JED_SCREEN);
            const int keys[3] = {JED_REDLAMP, JED_YELLOW, JED_LAMP};
            for (int k = 0; k < 20; ++k)
            {
                Box(mesh, Offset(top, -0.054f + (k % 5) * 0.027f, -0.036f + (k / 5) * 0.024f, 0.011f), {0.009f, 0.007f, 0.003f}, 0.0015f, keys[(k * 7) % 3]);
            }
            for (int k = 0; k < 3; ++k)
            {
                Box(mesh, Offset(top, 0.10f, -0.03f + k * 0.03f, 0.008f), {0.008f, 0.01f, 0.006f}, 0.002f, JED_ORANGE);
            }
        }
        KitPanel(mesh, MakeFrame({inner, (topY + JED_FLOOR) * 0.5f, 1.28f}, {0, 0, s}, {0, 1, 0}), 0.20f, (topY - JED_FLOOR) * 0.5f - 0.03f, 135 + side, look);
    }
    {
        const float h = XWG_STICK_BASE.y - JED_FLOOR;
        const float rings[7][2] = {{0, 0.10f}, {0.015f, 0.10f}, {0.02f, 0.085f}, {0.05f, 0.075f}, {h - 0.04f, 0.075f}, {h - 0.02f, 0.088f}, {h, 0.088f}};
        Lathe(mesh, MakeFrame({XWG_STICK_BASE.x, JED_FLOOR, XWG_STICK_BASE.z}, {1, 0, 0}, {0, 1, 0}), rings, 7, 18, JED_FRAME);
    }
    // The forward console: the narrow centre column (its arched blue main screen is the game's holo display), amber
    // lamp columns either side, a row of amber lamps and the red triangular button under it; the pedal housings with
    // their orange rings at its foot; the angled wing panels along both walls with their wide blue screens.
    {
        const float faceZ = JED_COLUMN_Z;
        Box(mesh, MakeFrame({0, (JED_FLOOR + 1.32f) * 0.5f, faceZ - 0.10f}, {1, 0, 0}, {0, 1, 0.15f}), {0.11f, (1.32f - JED_FLOOR) * 0.5f, 0.10f}, 0.015f, JED_PANEL_TONE);
        const Frame face = MakeFrame({0, 1.00f, faceZ + 0.005f}, {1, 0, 0}, {0, 1, 0.15f});
        for (int k = 0; k < 6; ++k)
        {
            Box(mesh, Offset(face, -0.0625f + k * 0.025f, -0.03f, 0), {0.0075f, 0.0075f, 0.003f}, 0, JED_AMBER);
        }
        const Vec3 tri[3] = {face.At(-0.015f, -0.06f, 0.004f), face.At(0.015f, -0.06f, 0.004f), face.At(0, -0.085f, 0.004f)};
        const float st[3][2] = {{0, 0}, {1, 0}, {0.5f, 1}};
        Triangle(mesh, tri[0], tri[1], tri[2], st, JED_REDLAMP);
        // the column's head widened behind the main screen to carry the lamp columns (idren2023 2026-11-11: they floated
        // beside the 22 cm column); its face flush with theirs
        Box(mesh, Offset(face, 0, 0.16f, -0.02f), {0.17f, 0.10f, 0.02f}, 0.008f, JED_PANEL_TONE);
        for (int side = -1; side <= 1; side += 2)
        {
            for (int k = 0; k < 4; ++k)
            {
                Box(mesh, Offset(face, side * 0.14f, 0.10f + k * 0.04f, 0.0f), {0.01f, 0.012f, 0.004f}, 0.002f, k % 2 ? JED_REDLAMP : JED_AMBER);
            }
            const Vec3 pedal = {side * 0.12f, JED_FLOOR, faceZ + 0.04f};
            const float housing[4][2] = {{0, 0.045f}, {0.17f, 0.045f}, {0.19f, 0.035f}, {0.20f, 0}};
            Lathe(mesh, MakeFrame(pedal, {1, 0, 0}, {0, 1, 0}), housing, 4, 16, JED_FRAME);
            for (int k = 0; k < 3; ++k)
            {
                const float ring[2][2] = {{0.04f + k * 0.045f, 0.047f}, {0.055f + k * 0.045f, 0.047f}};
                Lathe(mesh, MakeFrame(pedal, {1, 0, 0}, {0, 1, 0}), ring, 2, 16, JED_ORANGE);
            }
            // the wing panel: angled back along the wall, a wide blue screen and a smaller one behind it
            const float a0 = JED_WING_0 * side, a1 = JED_WING_1 * side;
            const Vec3 topA = JedOval(a0, JED_SILL - 0.02f, 0.02f), topB = JedOval(a1, JED_SILL - 0.02f, 0.02f);
            const Vec3 lowA = JedOval(a0, JED_SILL - 0.22f, 0.12f), lowB = JedOval(a1, JED_SILL - 0.22f, 0.12f);
            if (side > 0)
            {
                Quad(mesh, topA, topB, lowB, lowA, JED_PANEL_TONE);
            }
            else
            {
                Quad(mesh, topB, topA, lowA, lowB, JED_PANEL_TONE);
            }
            // its screens and lamp row let into the panel itself, in its plane (idren2023 2026-11-11: standing on it,
            // turned to the eye, housing, screens and lamps floated off the panel and cut into each other)
            const Vec3 centre = (topA + topB + lowA + lowB) * 0.25f;
            Vec3 across = Normalized((topB + lowB) - (topA + lowA));
            const Vec3 rise = (topA + topB) - (lowA + lowB);
            const Vec3 up = Normalized(rise - across * Dot(rise, across));
            if (Dot(Cross(across, up), JED_EYE - centre) < 0)
            {
                across = across * -1.0f;
            }
            const Frame wing = MakeFrame(centre, across, up);
            const float panelHalf = 0.5f * Length((topB + lowB) - (topA + lowA)) * 0.5f;
            const float sideOut = Dot(JedOval(a1, JED_SILL - 0.12f, 0.07f) - centre, across) > 0 ? 1.0f : -1.0f;
            JedScreen(mesh, wing, -sideOut * panelHalf * 0.30f, 0.025f, 0.11f, 0.035f, side < 0 ? GROUP_HEALTH : GROUP_AMMUNITION);
            JedScreen(mesh, wing, sideOut * panelHalf * 0.55f, 0.025f, 0.05f, 0.028f, side < 0 ? GROUP_TARGET : GROUP_SCORE);
            for (int k = 0; k < 10; ++k)
            {
                Box(mesh, Offset(wing, -0.17f + k * 0.035f, -0.045f, 0.002f), {0.006f, 0.005f, 0.002f}, 0, k % 3 ? JED_AMBER : JED_REDLAMP);
            }
        }
    }
    // Behind the seat: the grey roll-bar unit with its U-loop of black hoses and an upright bracket (the film's
    // Geonosis frame), machinery blocks either side with red buttons and orange ring lights, the raised shelf.
    {
        const float back = JED_OVAL_Z + JED_OVAL_BACK - 0.05f;
        const Frame bar = MakeFrame({0, 1.43f, 1.88f}, {1, 0, 0}, {0, 1, 0});
        Box(mesh, bar, {0.15f, 0.125f, 0.10f}, 0.02f, JED_GREY);
        for (int k = 0; k < 2; ++k)
        {
            Vec3 loop[9];
            for (int i = 0; i <= 8; ++i)
            {
                const float a = i * (3.1415926f / 8);
                loop[i] = bar.At(-0.10f * cosf(a), 0.125f + 0.12f * sinf(a), -0.03f + k * 0.05f);
            }
            Tube(mesh, loop, 9, 0.0175f, 8, JED_RUBBER);
        }
        const Vec3 bracket[5] = {bar.At(-0.05f, 0.125f, 0.06f), bar.At(-0.05f, 0.275f, 0.06f), bar.At(0.05f, 0.275f, 0.06f), bar.At(0.05f, 0.125f, 0.06f), bar.At(-0.05f, 0.125f, 0.06f)};
        Tube(mesh, bracket, 5, 0.01f, 6, JED_GREY, false);
        for (int side = -1; side <= 1; side += 2)
        {
            const Frame block = MakeFrame({side * 0.33f, 1.28f, 1.98f}, {1, 0, 0}, {0, 1, 0});
            Box(mesh, block, {0.10f, 0.16f, 0.12f}, 0.015f, JED_GREY);
            for (int k = 0; k < 4; ++k)
            {
                Box(mesh, Offset(block, -0.06f + k * 0.04f, 0.10f, -0.122f), {0.012f, 0.004f, 0.002f}, 0, JED_DARK);
                IndicatorLamp(mesh, Upright(MakeFrame(block.At(-0.05f + (k % 2) * 0.10f, -0.02f - (k / 2) * 0.05f, -0.123f), {1, 0, 0}, {0, 1, 0})), 0.009f, JED_FRAME, k == 1 ? JED_AMBER : JED_REDLAMP);
            }
            const float ringLight[2][2] = {{0, 0.11f}, {0.012f, 0.11f}};
            Lathe(mesh, MakeFrame(block.At(0, -0.16f, 0), {1, 0, 0}, {0, 1, 0}), ringLight, 2, 18, JED_ORANGE);
        }
        Box(mesh, MakeFrame({0, (JED_FLOOR + JED_SHELF_Y) * 0.5f, (1.95f + back) * 0.5f + 0.05f}, {1, 0, 0}, {0, 1, 0}), {0.36f, (JED_SHELF_Y - JED_FLOOR) * 0.5f, (back - 1.95f) * 0.5f}, 0.015f, JED_GREY);
        KitPanel(mesh, MakeFrame({0, JED_SHELF_Y + 0.001f, (1.95f + back) * 0.5f + 0.05f}, {1, 0, 0}, {0, 0, -1}), 0.32f, (back - 1.95f) * 0.5f - 0.02f, 151, look);
    }
    // The seat: brown quilted leather, the black padded horseshoe headrest wrapping the head (the clearest object in
    // every flying shot).
    {
        const SeatLook seat = {0.24f, 0.62f, 0.31f, false, false, JED_PAD, JED_FRAME, JED_METAL, JED_PAD, JED_DARK};
        Seat(mesh, JED_EYE, JED_FLOOR, seat);
        Vec3 horseshoe[13];
        for (int k = 0; k <= 12; ++k)
        {
            const float a = -0.35f + k * ((3.1415926f + 0.70f) / 12);
            horseshoe[k] = {0.145f * cosf(a), JED_EYE.y - 0.02f, JED_EYE.z + 0.08f + 0.12f * sinf(a)};
        }
        Vec3 smooth[64];
        const float pad[8][2] = {{-0.03f, 0.10f}, {0.03f, 0.10f}, {0.045f, 0.08f}, {0.045f, -0.08f}, {0.03f, -0.10f}, {-0.03f, -0.10f}, {-0.045f, -0.08f}, {-0.045f, 0.08f}};
        Sweep(mesh, smooth, Smooth(horseshoe, 13, 4, smooth, 64), pad, 8, true, true, {0, 1, 0}, JED_BLACK, true);
    }
    // No astromech of our own (headset run 18): the stock exterior's R4 in its socket shows.
}

// ---------------------------------------------------------------------------------------------------------------------
// Droid starfighter (PLAN starfighters): the vulture droid has no cockpit in the films (it is its own pilot,
// docs/research/starfighter-cockpits-canon.md), so the player rides in its sensor chamber: a cramped angular pod in the
// droid's green-grey metal behind a wide slit window (the head's sensor eye), the HUD on red visor screens of the
// stock art in a low console, round access ports on the walls; the stick, throttle and take-off lever (Flight) as in
// every starfighter. Replaces the game's first-person model (cis_1st_cockpit_starfighter, whose texture it uses).

enum DrdFinish
{
    DRD_VISOR, // cis_1st_cockpit_starfighter: the red visor screen with its frame, whole
    DRD_PORT,  //   the round port, whole
    DRD_PLATE, //   plain plate, by size
    DRD_BAND,  //   the band, by size
    DRD_EDGE,  //   the left column's panel lines, by size
    DRD_GLASS, // the window: the visor's red glass
    DRD_FRAME,
    DRD_DARK,
    DRD_METAL,
    DRD_RUBBER,
    DRD_PAD,
    DRD_LAMP,
    DRD_SCREEN,
    DRD_STOCK, // the droid's own exterior (StockExterior)
    DRD_KIT,   // kitbash plates (the droid's grey-green)
    DRD_AMBER,
    DRD_BLUE,  // the vulture's blue markings
    DRD_COCKPIT, // the game's first-person cockpit pieces (StockPieces), their own mapping
    DRD_FX,      //   their lamps
    DRD_GUNMETAL, // structure, bezels, consoles (the Separatist bridges' dark metal)
    DRD_LIME,     // lime light bars and glyphs
    DRD_BRASS,    // the antenna dome, cables, capacitors
    DRD_COPPER,   // the dome's ribs
    DRD_WHITE,    // the magnetic-imaging pods
    DRD_SHELL,    // the head shell's pale blue-green ribs
    DRD_FINISH_COUNT
};

const Finish DRD_FINISHES[DRD_FINISH_COUNT] = {
    XwgTexture(1, 60, 40, 262, 440, 0x8E9296, 0.30f, 20, 0.25f), // visor
    XwgTexture(1, 296, 18, 482, 190, 0x8E9296, 0.30f, 20),       // port
    XwgTexture(1, 280, 210, 508, 466, 0x8A8E8A, 0.20f, 14),      // plate
    XwgTexture(1, 0, 478, 512, 500, 0x8A8E8A, 0.25f, 16),        // band
    XwgTexture(1, 0, 60, 58, 460, 0x8A8E8A, 0.20f, 14),          // edge
    {{0, 0, 1, 1}, 1, 0, 0x501010, 0.70f, 40, 0.10f, 0.22f},     // window glass: a faint red tint
    {{0, 0, 1, 1}, 1, 0, 0x5A6058, 0.35f, 22, 0},                // frame
    {{0, 0, 1, 1}, 1, 0, 0x141614, 0.05f, 6, 0},                 // dark
    {{0, 0, 1, 1}, 1, 0, 0x9CA09C, 0.60f, 40, 0},                // metal
    {{0, 0, 1, 1}, 1, 0, 0x202220, 0.20f, 10, 0},                // rubber
    {{0, 0, 1, 1}, 1, 0, 0x4A4E48, 0.15f, 10, 0},                // seat
    {{0, 0, 1, 1}, 1, 0, 0xFF4030, 0, 1, 1.0f},                  // red lamp
    {{0, 0, 1, 1}, 1, 0, 0x1A0606, 0.50f, 40, 0.05f},            // screen glass
    {{0, 0, 1, 1}, 1, 3, 0x808080, 0.20f, 14, 0},               // the exterior, its own mapping
    {{0, 0, 1, 1}, 1, 0, 0x5E645C, 0.25f, 16, 0},               // kitbash plate
    {{0, 0, 1, 1}, 1, 0, 0xFFB040, 0, 1, 1.0f},                 // amber lamp
    {{0, 0, 1, 1}, 1, 0, 0x5080FF, 0, 1, 1.0f},                 // blue lamp
    {{0, 0, 1, 1}, 1, 1, 0x808080, 0.25f, 16, 0},               // stock cockpit pieces
    {{0, 0, 1, 1}, 1, 2, 0xC0C0C0, 0, 1, 1.0f},                 // their lamps
    {{0, 0, 1, 1}, 1, 0, 0x2E3336, 0.40f, 24, 0},               // gunmetal
    {{0, 0, 1, 1}, 1, 0, 0xA8FF3C, 0, 1, 1.0f},                 // lime
    {{0, 0, 1, 1}, 1, 0, 0xB08A4A, 0.60f, 36, 0},               // brass
    {{0, 0, 1, 1}, 1, 0, 0xA0623A, 0.55f, 30, 0},               // copper
    {{0, 0, 1, 1}, 1, 0, 0xE6E8EC, 0.45f, 30, 0},               // white
    {{0, 0, 1, 1}, 1, 0, 0x9DB8B4, 0.30f, 20, 0},               // pale blue-green
};

constexpr Vec3 DRD_EYE = {0.0f, 1.5f, 1.5f};
/// The sensor pod let into the head's deck: floor, rim (the deck's height round it), half width, front and back; the
/// console's top and foot.
constexpr float DRD_FLOOR = 0.64f, DRD_ROOF = 1.97f, DRD_HALF = 0.95f, DRD_BACK = 3.10f;
/// The open arch into the machine room behind the seat (the brain racks, the antenna dome).
constexpr float DRD_ARCH_Z = 2.20f;
/// The chamber's floor, roof and walls reach forward past the stock shell's front to close it in.
constexpr float DRD_SHELL_FRONT = -0.25f;
/// The console under the window: its top edge (height, place) and foot.
constexpr float DRD_DASH_TOP_Y = 1.00f, DRD_DASH_TOP_Z = 0.78f, DRD_DASH_LOW_Y = 0.76f;

void DroidScreen(Mesh& mesh, const Frame& face, float x, float y, float halfWidth, float halfHeight, int group)
{
    // the stock visor art as the screen's frame round it, the HUD on its red glass
    Quad(
        mesh,
        face.At(-halfWidth * 1.35f, halfHeight * 1.25f, 0.002f),
        face.At(halfWidth * 1.35f, halfHeight * 1.25f, 0.002f),
        face.At(halfWidth * 1.35f, -halfHeight * 1.25f, 0.002f),
        face.At(-halfWidth * 1.35f, -halfHeight * 1.25f, 0.002f),
        DRD_VISOR);
    (void)x;
    (void)y;
    AddDisplay(group, Offset(face, 0, 0, 0.004f), 0, 0, halfWidth * 1.8f, halfHeight * 1.8f);
}

void BuildDroidFighter(Mesh& mesh)
{
    // idren2023 2026-10-27, the detached head: the droid is the ship and its head rises off its base, as in the films'
    // walking pose; the pilot's eye is the head itself, so there is no cockpit: the ship's own body (stockexterior.py:
    // the head, its own texture island, left out; a flat gunmetal cap where it sat), a red viewscreen over the view and
    // the controllers' holograms (ControllerHolo), and the HUD in red in the agreed layout (concept H): the target's
    // bar high over the middle, health and weapons either side of the reticle where the eyes stand, the minimap upper
    // left, the score upper right, all on displays a metre ahead of the eye.
    StockExterior(mesh, StockVertices("EXTERIOR_DROID"), StockCount("EXTERIOR_DROID"), DRD_STOCK, DRD_GLASS, DRD_GUNMETAL);
    SetDisplayTint(0xFF3C28 | TINT_KEEPS_RELATIONS);
    // Headset run 18 (idren2023: hard to read, too close): 1.6 m ahead, each place and size 1.1 times the old angle
    // (ControllerHolo HUD_DEPTH_SCALE, HUD_GROWTH alike).
    constexpr float D = 1.6f, G = 1.6f * 1.1f;
    const Frame ahead = MakeFrame({DRD_EYE.x, DRD_EYE.y, DRD_EYE.z - D}, {1, 0, 0}, {0, 1, 0});
    // On the head (HEAD_PART, idren2023 2026-10-27: the HUD turns with the head; run497 had it fixed to the hull).
    // the target's bar low between the controllers' screens (idren2023 2026-10-27), its TGT: tag and the hexagon round
    // its type icon drawn by ControllerHolo (TARGET_* there)
    AddDisplay(GROUP_TARGET, ahead, 0, -0.42f * G, 0.30f * G, 0.06f * G, HEAD_PART);
    // The health and ammunition readouts left out: the eye gauges show the hull and the heat (ControllerHolo; idren2023
    // 2026-10-27: no player health).
    AddDisplay(GROUP_HEALTH, ahead, 0, 0, 0, 0, HEAD_PART);
    AddDisplay(GROUP_AMMUNITION, ahead, 0, 0, 0, 0, HEAD_PART);
    AddDisplay(GROUP_MAP, ahead, -0.62f * G, 0.38f * G, 0.26f * G, 0.26f * G, HEAD_PART);
    AddDisplay(GROUP_SCORE, ahead, 0.60f * G, 0.42f * G, 0.30f * G, 0.07f * G, HEAD_PART);
    SetDisplayTint(0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Geonosian fighter (Nantex, PLAN starfighters): the stock game has no cockpit for it (it shows the gunner seats'
// security-camera frame), so ours is built from the films' ship and the cross-sections' cutaway
// (docs/research/cockpits/geonosian.md): the pilot's head up in the ship's own frameless bubble, his body sunk in a
// narrow well of dark gunmetal laminasteel ribs; a bone crescent dash under the forward glass carries the readouts
// (the minimap in the middle, health over ammunition left, target over score right), turned to the eye; the stick in a
// ribbed bellows boot between the knees (Flight), the throttle and the take-off lever on a moulded pod grown from the
// left wall; a segmented saddle and spine back for a winged pilot. Headset run 19 (idren2023: the weakest cockpit): one
// dash instead of the column and the panels on stands, no cowl on the glass, the well gunmetal (its brown read as
// wooden staves), hoses that end in the floor, the scent mask on the right wall away from the levers. Textures: the
// ship's own (cis_fly_fighter, its canopy cis_fly_fighter_canopy).

enum GeoFinish
{
    GEO_STOCK,   // the ship's own exterior (StockExterior)
    GEO_GLASS,   // cis_fly_fighter_canopy: the bubble
    GEO_SHELL,   // the well's inner skin: gunmetal (the stock art's riveted grey plate)
    GEO_RIB,     // laminasteel ribs (bronze: the hull's art)
    GEO_DARK,    // recesses, slots, the floor
    GEO_METAL,   // bright bronze: the rim, bezels, collars
    GEO_RUBBER,  // ribbed hoses, the bellows
    GEO_PAD,     // seat pads and the scent mask (bone)
    GEO_BONE,    // the dash's and the lever pod's moulded shell
    GEO_SCREEN,  // screen glass
    GEO_AMBER,   // the gold gauge
    GEO_GREEN,   // jewel lamps
    GEO_YELLOW,
    GEO_CYAN,
    GEO_MAGENTA,
    GEO_SCREEN_FRAME,   // the HUD screens' stock frame (texture 5, cis_1st_cockpit_securitycam: the tanks' look)
    GEO_SCREEN_GLASS,   //   their glass (scan lines)
    GEO_SCREEN_BACKING, //   behind the glass
    GEO_FINISH_COUNT
};

const Finish GEO_FINISHES[GEO_FINISH_COUNT] = {
    {{0, 0, 1, 1}, 1, 3, 0x808080, 0.20f, 14, 0},            // the exterior, its own mapping
    {{0, 0, 1, 1}, 1, 4, 0x98B0C4, 0.60f, 40, 0, 0.40f},     // bubble glass (the stock canopy texture is near opaque)
    XwgTexture(3, 345, 470, 410, 508, 0x9A948C, 0.30f, 18), // inner skin: the stock art's riveted grey plate
    XwgTexture(3, 110, 150, 170, 330, 0xABABAB, 0.40f, 24), // ribs (laminasteel): the hull's art
    {{0, 0, 1, 1}, 1, 0, 0x16130F, 0.05f, 6, 0},             // dark
    XwgTexture(3, 40, 190, 100, 320, 0xD9D9D9, 0.55f, 36),  // bright bronze: the hull's art, lighter
    {{0, 0, 1, 1}, 1, 0, 0x2A2420, 0.20f, 10, 0},            // rubber: near-black brown
    XwgTexture(3, 410, 200, 480, 250, 0x7C7764, 0.20f, 14), // seat pads (bone): the stock art, tinted
    XwgTexture(3, 410, 200, 480, 250, 0x7C7764, 0.30f, 20), // moulded bone: the stock art, tinted
    {{0, 0, 1, 1}, 1, 0, 0x140C06, 0.50f, 40, 0.05f},        // screen glass
    {{0, 0, 1, 1}, 1, 0, 0xFFA040, 0, 1, 1.0f},              // amber
    {{0, 0, 1, 1}, 1, 0, 0x6CFF4A, 0, 1, 1.0f},              // green jewel
    {{0, 0, 1, 1}, 1, 0, 0xFFD23A, 0, 1, 1.0f},              // yellow jewel
    {{0, 0, 1, 1}, 1, 0, 0x3AC8FF, 0, 1, 1.0f},              // cyan jewel
    {{0, 0, 1, 1}, 1, 0, 0xFF4FD8, 0, 1, 1.0f},              // magenta jewel
    ScreenFrameFinish(),                                                    // stock screen frame
    ScreenGlassFinish(CIS_SCREEN_ART),                                      // its glass
    ScreenBackingFinish(CIS_SCREEN_ART),                                    // behind the glass
};

// The HUD screens in the tanks' look (idren2023 2026-11-09: every vehicle's HUD as the tanks').
const StockScreenLook GEO_SCREENS = {&CIS_SCREEN_ART, GEO_SCREEN_FRAME, GEO_SCREEN_GLASS, GEO_SCREEN_BACKING, GEO_DARK};

constexpr Vec3 GEO_EYE = {0.0f, 1.5f, 1.5f};
/// The well under the ship's own bubble: an oval the canopy's length; floor, sill, half width, the middle of its
/// length and half its length. The ship sits 15 cm lower under the eye than the exterior's own camera (idren2023
/// 2026-10-27: the head up in the bubble, the prongs seen over the nose; tools/stockexterior.py).
constexpr float GEO_FLOOR = 0.43f, GEO_SILL = 1.22f, GEO_HALF = 0.54f, GEO_OVAL_Z = 1.57f, GEO_OVAL_DEPTH = 0.95f;
/// The bubble (the exterior's glass, nearly an ellipsoid on the sill): half width, height, half length.
constexpr float GEO_BUBBLE_HALF = 0.55f, GEO_BUBBLE_HIGH = 0.46f, GEO_BUBBLE_DEPTH = 0.99f;
/// The dash's crescent: its half span round the well (radians), the share of it at full section (beyond, it sinks
/// into the wall), its section's points.
constexpr float GEO_DASH_SPAN = 1.0f, GEO_DASH_FULL = 0.70f;
constexpr int GEO_DASH_POINTS = 6;
/// The lever pod on the left wall (Flight: the throttle's hinge at the eye + (-0.25, -0.64, -0.27), the take-off
/// lever's at the eye + (-0.36, -0.62, -0.10)): its top just under both pivots, its inner side clear of the stick.
constexpr float GEO_POD_TOP = 0.854f, GEO_POD_INNER = -0.17f, GEO_POD_FRONT = 1.06f, GEO_POD_BACK = 1.54f;

// A screen set in the dash: a dark recess, the glass in it, a raised bronze bezel round its edges (a frame of four
// bars, not a solid block behind the glass: that read as flicker through the glass).
void GeoScreen(Mesh& mesh, const Frame& face, float halfWidth, float halfHeight, int group)
{
    // the tanks' stock screen (idren2023 2026-11-09; was a dark recess, glass and a bronze bezel)
    StockScreen(mesh, face, halfWidth, halfHeight, SCREEN_TEXEL_METERS * halfHeight / 0.0675f, GEO_SCREENS, 0.014f);
    SetDisplayScreen(&GEO_SCREENS);
    AddDisplay(group, Offset(face, 0, 0, 0.0015f), 0, 0, halfWidth * 1.9f, halfHeight * 1.9f);
    SetDisplayScreen(nullptr);
}

// A point on the well's wall at an angle round it (0 straight ahead, positive to the right) and a height: an oval
// that narrows from the rim to the pilot's hips (docs/research/cockpits/geonosian.md: the head up in the bubble,
// the body sunk in the hull), moved inward by `inset`.
Vec3 GeoOval(float angle, float y, float inset = 0)
{
    const float t = (GEO_SILL - y) / (GEO_SILL - GEO_FLOOR); // 0 at the rim, 1 at the floor
    const float half = GEO_HALF - 0.22f * (t < 0 ? 0 : t > 1 ? 1 : t);
    const float depth = GEO_OVAL_DEPTH - 0.30f * (t < 0 ? 0 : t > 1 ? 1 : t);
    return {(half - inset) * sinf(angle), y, GEO_OVAL_Z - (depth - inset) * cosf(angle)};
}

// A point on the bubble at an angle round it (0 straight ahead, positive to the right) and an elevation (0 at the sill,
// pi/2 at its crown), moved inward by `inset`.
Vec3 GeoBubble(float angle, float elevation, float inset = 0)
{
    const float flat = cosf(elevation);
    return {(GEO_BUBBLE_HALF - inset) * flat * sinf(angle), GEO_SILL + (GEO_BUBBLE_HIGH - inset) * sinf(elevation),
            GEO_OVAL_Z - (GEO_BUBBLE_DEPTH - inset) * flat * cosf(angle)};
}

// A round jewelled lamp on a wall (bronze bezel, domed lens), facing `out`.
void GeoJewel(Mesh& mesh, Vec3 at, Vec3 out, float radius, int lens)
{
    const Frame face = MakeFrame(at, Normalized(Cross({0, 1, 0}, out)), {0, 1, 0});
    const float bezel[4][2] = {{-0.01f, radius + 0.012f}, {0.004f, radius + 0.012f}, {0.008f, radius + 0.004f}, {0.006f, radius}};
    Lathe(mesh, Upright(face), bezel, 4, 16, GEO_METAL);
    const float dome[4][2] = {{0.006f, radius}, {0.012f, radius * 0.85f}, {0.016f, radius * 0.5f}, {0.018f, 0}};
    Lathe(mesh, Upright(face), dome, 4, 16, lens);
}

// The dash's section at an angle round the well (the research, section 5: a bone crescent under the forward glass):
// from under the rim in over a short brow, down a face turned to the eye (the screens), back under to the wall. The
// face runs on a gentle arc in plan (the well's front is too pointed for flat screens), met by the ray from the well's
// middle through the wall; toward the crescent's ends the section shrinks into the wall (no end caps).
void GeoDashSection(float angle, Vec3* out)
{
    constexpr float ARC_RADIUS = 0.55f;
    const float t = fabsf(angle) / GEO_DASH_SPAN;
    const float k = t <= GEO_DASH_FULL ? 1.0f : t >= 1 ? 0.0f : 0.5f + 0.5f * cosf((t - GEO_DASH_FULL) / (1 - GEO_DASH_FULL) * 3.1415926f);
    // each point's depth under the sill and distance in from the well's front: rim, brow front, the face's top (its
    // bronze lip), the face's foot, the rounded underside, back at the wall
    const float profile[GEO_DASH_POINTS][2] = {{0.004f, -0.012f}, {0.025f, 0.120f}, {0.045f, 0.150f},
                                               {0.215f, 0.250f}, {0.255f, 0.225f}, {0.340f, 0.0f}};
    for (int i = 0; i < GEO_DASH_POINTS; ++i)
    {
        const float y = GEO_SILL - (i == 0 ? profile[i][0] : 0.10f + (profile[i][0] - 0.10f) * k);
        const Vec3 wall = GeoOval(angle, y, i == 0 ? profile[i][1] : 0);
        if (i == 0 || i == GEO_DASH_POINTS - 1)
        {
            out[i] = wall;
            continue;
        }
        // s along the ray from the middle (0) to the wall (1) where it meets z = z0 + x^2 / 2R
        const float dx = wall.x, dz = wall.z - GEO_OVAL_Z;
        const float a = dx * dx / (2 * ARC_RADIUS), c = GeoOval(0, y).z + profile[i][1] - GEO_OVAL_Z;
        const float s = fminf(1.0f, 2 * c / (dz - sqrtf(dz * dz - 4 * a * c)));
        const Vec3 arc = {s * dx, y, GEO_OVAL_Z + s * dz};
        out[i] = wall + (arc - wall) * k;
    }
}

// A frame on the dash's face at an angle round the well, `along` from its top (0) to its foot (1): x along the
// crescent, y up the face, z out of it toward the eye.
Frame GeoDashFace(float angle, float along, float out = 0)
{
    Vec3 here[GEO_DASH_POINTS], before[GEO_DASH_POINTS], after[GEO_DASH_POINTS];
    GeoDashSection(angle, here);
    GeoDashSection(angle - 0.01f, before);
    GeoDashSection(angle + 0.01f, after);
    const Frame face = MakeFrame(Lerp(here[2], here[3], along), Lerp(after[2], after[3], along) - Lerp(before[2], before[3], along),
                                 here[2] - here[3]);
    return Offset(face, 0, 0, out);
}

// A frame on the dash's brow at an angle round the well, `along` from the rim (0) to its front (1): z up.
Frame GeoDashBrow(float angle, float along)
{
    Vec3 here[GEO_DASH_POINTS], before[GEO_DASH_POINTS], after[GEO_DASH_POINTS];
    GeoDashSection(angle, here);
    GeoDashSection(angle - 0.01f, before);
    GeoDashSection(angle + 0.01f, after);
    return MakeFrame(Lerp(here[0], here[1], along), Lerp(after[0], after[1], along) - Lerp(before[0], before[1], along),
                     here[0] - here[1]);
}

// A laminasteel rib up the wall at an angle, from the floor to `top`, bulging in by `bulge` at mid height.
void GeoRib(Mesh& mesh, float angle, float top, float bulge, float radius)
{
    Vec3 rib[5];
    for (int i = 0; i < 5; ++i)
    {
        const float f = i / 4.0f;
        rib[i] = GeoOval(angle, GEO_FLOOR + 0.02f + (top - GEO_FLOOR - 0.04f) * f, 0.010f + bulge * sinf(f * 3.1415926f));
    }
    Vec3 smooth[24];
    Tube(mesh, smooth, Smooth(rib, 5, 4, smooth, 24), radius, 6, GEO_RIB);
}

void BuildGeoFighter(Mesh& mesh)
{
    // The ship's own needle hull and its frameless bubble in the bronze rim (the research: no ribs on the glass).
    StockExterior(mesh, StockVertices("EXTERIOR_GEO"), StockCount("EXTERIOR_GEO"), GEO_STOCK, GEO_GLASS);
    // The well (the cross-sections' cutaway and the reference models): dark gunmetal, curved, no flat panels; arched
    // laminasteel ribs every 26 cm round the wall (under the dash they stop at its underside); a worn bronze rim.
    {
        constexpr int STEPS = 48;
        constexpr int BANDS = 4;
        Vec3 rings[BANDS + 1][STEPS + 1];
        for (int b = 0; b <= BANDS; ++b)
        {
            for (int i = 0; i <= STEPS; ++i)
            {
                rings[b][i] = GeoOval(i * (6.2831853f / STEPS), GEO_FLOOR + (GEO_SILL - GEO_FLOOR) * b / BANDS);
            }
        }
        for (int b = 0; b < BANDS; ++b)
        {
            Loft(mesh, rings[b], rings[b + 1], STEPS + 1, 1, GEO_SHELL);
        }
        Vec3 floorRing[STEPS], rim[STEPS + 1];
        for (int i = 0; i <= STEPS; ++i)
        {
            const float a = i * (6.2831853f / STEPS);
            rim[i] = GeoOval(a, GEO_SILL + 0.01f, -0.02f);
            if (i < STEPS)
            {
                floorRing[i] = GeoOval(a, GEO_FLOOR);
            }
        }
        PlanarPolygon(mesh, floorRing, STEPS, {1, 0, 0}, GEO_DARK);
        Tube(mesh, rim, STEPS + 1, 0.028f, 10, GEO_METAL, false);
        for (int k = 0; k < 24; ++k)
        {
            const float a = remainderf((k + 0.5f) * (6.2831853f / 24), 6.2831853f);
            if (fabsf(a) > 3.1415926f - 0.55f)
            {
                continue; // the ribcage fills the back
            }
            if (a < -0.72f && a > -1.62f)
            {
                continue; // the lever pod
            }
            Vec3 dash[GEO_DASH_POINTS];
            GeoDashSection(a, dash);
            const bool isUnderDash = fabsf(a) < GEO_DASH_SPAN;
            GeoRib(mesh, a, isUnderDash ? dash[GEO_DASH_POINTS - 1].y - 0.01f : GEO_SILL, isUnderDash ? 0.012f : 0.03f, 0.013f);
        }
    }
    // The back wall under the rear of the dome: a ribcage of arched ribs closing to a spine, a round ribbed fan disc
    // low on the left.
    {
        const Vec3 spineTop = GeoOval(3.1415926f, GEO_SILL - 0.04f, 0.10f);
        for (int k = -3; k <= 3; ++k)
        {
            const float a = 3.1415926f + k * 0.16f;
            const Vec3 rib[4] = {GeoOval(a, GEO_FLOOR + 0.05f, 0.05f), GeoOval(a, 0.80f, 0.10f), GeoOval(a, 1.03f, 0.11f), spineTop};
            Vec3 smooth[24];
            Tube(mesh, smooth, Smooth(rib, 4, 5, smooth, 24), 0.018f, 6, GEO_RIB);
        }
        const Vec3 fan = GeoOval(3.1415926f - 0.55f, 0.67f, 0.06f);
        const Frame disc = MakeFrame(fan, Normalized(Cross({0, 1, 0}, Vec3{0, 0, -1})), {0, 1, 0});
        const float ring[6][2] = {{-0.03f, 0.07f}, {0, 0.075f}, {0.01f, 0.07f}, {0.012f, 0.055f}, {0.006f, 0.05f}, {0.004f, 0}};
        Lathe(mesh, Upright(disc), ring, 6, 20, GEO_METAL);
        for (int k = 0; k < 8; ++k)
        {
            const float a = k * 0.785f;
            const Vec3 blade[2] = {disc.At(0.012f * cosf(a), 0.012f * sinf(a), 0.008f), disc.At(0.048f * cosf(a + 0.4f), 0.048f * sinf(a + 0.4f), 0.008f)};
            Tube(mesh, blade, 2, 0.004f, 4, GEO_DARK);
        }
    }
    // Ribbed hoses along the rear side walls between the ribs, each rising out of the floor and going back into it.
    for (int side = -1; side <= 1; side += 2)
    {
        for (int h = 0; h < 2; ++h)
        {
            const float s = static_cast<float>(side), y = 0.72f + h * 0.09f;
            const float a0 = s * (1.80f + h * 0.06f), a1 = s * (2.62f - h * 0.05f);
            const Vec3 path[6] = {GeoOval(a0 - s * 0.05f, GEO_FLOOR + 0.01f, 0.045f), GeoOval(a0, y - 0.10f, 0.05f), GeoOval(a0 + s * 0.12f, y, 0.05f),
                                  GeoOval(a1 - s * 0.12f, y - 0.015f, 0.05f), GeoOval(a1, y - 0.10f, 0.05f), GeoOval(a1 + s * 0.05f, GEO_FLOOR + 0.01f, 0.045f)};
            Vec3 smooth[64];
            CorrugatedHose(mesh, smooth, Smooth(path, 6, 10, smooth, 64), 0.017f - h * 0.003f, GEO_RUBBER, GEO_DARK);
        }
    }
    // The dash (the research: a bone crescent under the forward glass, its face tilted to the eye, pale screens in a
    // gentle arc with round dials between, a row of toggles and the yellow and cyan lamps on its brow, the bronze lip).
    {
        constexpr int STEPS = 36;
        Vec3 lines[GEO_DASH_POINTS][STEPS + 1];
        for (int i = 0; i <= STEPS; ++i)
        {
            Vec3 section[GEO_DASH_POINTS];
            GeoDashSection(-GEO_DASH_SPAN + i * (2 * GEO_DASH_SPAN / STEPS), section);
            for (int p = 0; p < GEO_DASH_POINTS; ++p)
            {
                lines[p][i] = section[p];
            }
        }
        for (int p = 0; p + 1 < GEO_DASH_POINTS; ++p)
        {
            Loft(mesh, lines[p], lines[p + 1], STEPS + 1, 1, GEO_BONE);
        }
        Vec3 lip[STEPS - 5];
        for (int i = 0; i < STEPS - 5; ++i)
        {
            Vec3 section[GEO_DASH_POINTS];
            GeoDashSection(-GEO_DASH_SPAN * 0.86f + i * (2 * GEO_DASH_SPAN * 0.86f / (STEPS - 6)), section);
            lip[i] = section[2];
        }
        Tube(mesh, lip, STEPS - 5, 0.009f, 8, GEO_METAL);
        // the readouts: the minimap in the middle, health over ammunition left, target over score right
        GeoScreen(mesh, GeoDashFace(0, 0.5f, 0.010f), 0.070f, 0.070f, GROUP_MAP);
        for (int side = -1; side <= 1; side += 2)
        {
            const float a = side * 0.50f;
            GeoScreen(mesh, GeoDashFace(a, 0.25f, 0.010f), 0.070f, 0.032f, side < 0 ? GROUP_HEALTH : GROUP_TARGET);
            GeoScreen(mesh, GeoDashFace(a, 0.75f, 0.010f), 0.070f, 0.032f, side < 0 ? GROUP_AMMUNITION : GROUP_SCORE);
            // a round dial beyond them (dark face, one pale needle) and the brow's toggles and lamps
            const Frame dial = GeoDashFace(side * 0.80f, 0.5f, 0.004f);
            const float bezel[4][2] = {{-0.01f, 0.036f}, {0.006f, 0.036f}, {0.010f, 0.030f}, {0.004f, 0.027f}};
            Lathe(mesh, Upright(dial), bezel, 4, 18, GEO_METAL);
            const float faceRings[2][2] = {{0.004f, 0.027f}, {0.0045f, 0}};
            Lathe(mesh, Upright(dial), faceRings, 2, 18, GEO_SCREEN);
            const Vec3 needle[2] = {dial.At(0, 0, 0.007f), dial.At(side * 0.012f, 0.018f, 0.007f)};
            Tube(mesh, needle, 2, 0.0022f, 4, GEO_CYAN);
            for (int k = 0; k < 5; ++k)
            {
                const Frame toggle = GeoDashBrow(side * (0.32f + k * 0.06f), 0.55f);
                Box(mesh, Offset(toggle, 0, 0, 0.008f), {0.006f, 0.010f, 0.008f}, 0.002f, k == 2 ? GEO_METAL : GEO_DARK);
            }
            const Frame lamp = GeoDashBrow(side * 0.14f, 0.5f);
            GeoJewel(mesh, lamp.At(0, 0, 0.002f), lamp.z, 0.014f, side < 0 ? GEO_YELLOW : GEO_CYAN);
        }
    }
    // The canopy from inside: a bronze collar where the bubble meets the rim.
    {
        constexpr int STEPS = 40;
        Vec3 collar[STEPS + 1];
        for (int i = 0; i <= STEPS; ++i)
        {
            collar[i] = GeoBubble(i * (6.2831853f / STEPS), 0.06f, 0.012f);
        }
        Tube(mesh, collar, STEPS + 1, 0.016f, 8, GEO_METAL, false);
    }
    // The jewelled lamps on the side walls (magenta over green, mirrored), the small gold gauge behind the right hand.
    for (int side = -1; side <= 1; side += 2)
    {
        const float a = side * 1.57f;
        const Vec3 at = GeoOval(a, GEO_SILL - 0.13f, 0.05f), lower = GeoOval(a, GEO_SILL - 0.20f, 0.05f);
        const Vec3 in = Normalized(Vec3{-at.x, 0, GEO_OVAL_Z - at.z});
        GeoJewel(mesh, at, in, 0.022f, GEO_MAGENTA);
        GeoJewel(mesh, lower, in, 0.022f, GEO_GREEN);
    }
    {
        const Vec3 at = GeoOval(1.95f, 0.90f, 0.06f);
        const Vec3 in = Normalized(Vec3{-at.x, 0, GEO_OVAL_Z - at.z});
        GeoJewel(mesh, at, in, 0.028f, GEO_AMBER);
    }
    // The parked scent mask (the pilot's flight feedback by smell) on the right wall, its corrugated hose to the floor.
    {
        const Vec3 mask = GeoOval(1.12f, GEO_SILL - 0.30f, 0.08f);
        const Vec3 maskOut = Normalized(Vec3{-mask.x, 0.3f, GEO_OVAL_Z - mask.z});
        const Vec3 cradle[2] = {GeoOval(1.12f, GEO_SILL - 0.30f, 0.0f), mask};
        Tube(mesh, cradle, 2, 0.012f, 8, GEO_METAL);
        const float maskCup[6][2] = {{0, 0.05f}, {0.02f, 0.055f}, {0.05f, 0.045f}, {0.07f, 0.03f}, {0.08f, 0.012f}, {0.082f, 0}};
        Lathe(mesh, MakeFrame(mask, Cross({0, 1, 0}, maskOut), maskOut), maskCup, 6, 14, GEO_PAD);
        const Vec3 hose[4] = {mask + Vec3{0, -0.03f, 0}, GeoOval(1.10f, GEO_SILL - 0.45f, 0.06f), GeoOval(1.05f, GEO_FLOOR + 0.12f, 0.06f),
                              GeoOval(1.03f, GEO_FLOOR + 0.01f, 0.05f)};
        Vec3 smooth[32];
        CorrugatedHose(mesh, smooth, Smooth(hose, 4, 8, smooth, 32), 0.014f, GEO_RUBBER, GEO_DARK);
    }
    // The stick's gimbal: a ribbed bellows boot up to its floor plate (LeverModel::JetStick) with a bronze collar.
    {
        const Vec3 mount = {XWG_STICK_BASE.x, GEO_FLOOR, XWG_STICK_BASE.z};
        const float h = XWG_STICK_BASE.y - GEO_FLOOR;
        float bellows[12][2];
        for (int i = 0; i < 12; ++i)
        {
            bellows[i][0] = i * (h / 11);
            bellows[i][1] = (i % 2 ? 0.060f : 0.050f) - i * 0.0012f;
        }
        Lathe(mesh, MakeFrame(mount, {1, 0, 0}, {0, 1, 0}), bellows, 12, 14, GEO_RUBBER);
        const float collar[4][2] = {{h - 0.03f, 0.066f}, {h - 0.025f, 0.072f}, {h - 0.005f, 0.072f}, {h, 0.062f}};
        Lathe(mesh, MakeFrame(mount, {1, 0, 0}, {0, 1, 0}), collar, 4, 16, GEO_METAL);
    }
    // The lever pod on the left wall: a moulded gunmetal body grown out of the wall, a bone top just under the throttle's
    // hub and the take-off lever's carriage with a slot for each, bronze notch marks beside the throttle's, a yellow and
    // a green jewel at its front (the lever's two states).
    {
        const float outer = -0.52f, middleX = (outer + GEO_POD_INNER) * 0.5f, middleZ = (GEO_POD_FRONT + GEO_POD_BACK) * 0.5f;
        const float halfX = (GEO_POD_INNER - outer) * 0.5f, halfZ = (GEO_POD_BACK - GEO_POD_FRONT) * 0.5f;
        Box(mesh, MakeFrame({middleX, (GEO_FLOOR + GEO_POD_TOP - 0.012f) * 0.5f, middleZ}, {1, 0, 0}, {0, 1, 0}),
            {halfX, (GEO_POD_TOP - 0.012f - GEO_FLOOR) * 0.5f, halfZ}, 0.045f, GEO_SHELL);
        Box(mesh, MakeFrame({middleX, GEO_POD_TOP - 0.010f, middleZ}, {1, 0, 0}, {0, 1, 0}), {halfX + 0.006f, 0.010f, halfZ + 0.006f},
            0.009f, GEO_BONE);
        const Frame top = MakeFrame({0, GEO_POD_TOP + 0.0008f, 0}, {1, 0, 0}, {0, 0, -1}); // z up, y ahead
        const Vec3 throttle = {GEO_EYE.x - 0.25f, 0, GEO_EYE.z - 0.27f}, lever = {GEO_EYE.x - 0.36f, 0, GEO_EYE.z - 0.10f};
        Box(mesh, Offset(top, throttle.x, -throttle.z, 0), {0.016f, 0.095f, 0.0015f}, 0.001f, GEO_DARK);
        Box(mesh, Offset(top, lever.x, -(lever.z - 0.06f), 0), {0.014f, 0.095f, 0.0015f}, 0.001f, GEO_DARK);
        for (int notch = -1; notch <= 1; ++notch)
        {
            Box(mesh, Offset(top, throttle.x + 0.032f, -throttle.z + notch * 0.055f, 0.001f), {0.010f, 0.0025f, 0.0015f}, 0.0008f, GEO_METAL);
        }
        for (int k = 0; k < 2; ++k)
        {
            const Vec3 at = {-0.385f + k * 0.05f, GEO_POD_TOP + 0.002f, 1.15f};
            GeoJewel(mesh, at, Normalized(GEO_EYE - at), 0.010f, k ? GEO_GREEN : GEO_YELLOW);
        }
    }
    // The seat for a winged Geonosian: a segmented saddle, knee rests sloping into the tunnel, a spine back of padded
    // segments (as an insect's abdomen) on a gunmetal spine, a round-topped crest, a wing channel either side edged by
    // thin ribs.
    {
        const float cushion = GEO_EYE.y - 0.80f;
        for (int k = 0; k < 4; ++k)
        {
            RoundedBar(mesh, MakeFrame({0, cushion - 0.03f, GEO_EYE.z - 0.12f + k * 0.11f}, {1, 0, 0}, {0, 1, 0}), 0.13f, 0.03f, 0.05f, 0.025f, GEO_PAD);
        }
        for (int side = -1; side <= 1; side += 2)
        {
            const Frame knee = MakeFrame({side * 0.16f, cushion - 0.06f, GEO_EYE.z - 0.32f}, {1, 0, 0}, {0, 1, -0.6f});
            RoundedBar(mesh, knee, 0.045f, 0.02f, 0.09f, 0.018f, GEO_PAD);
            const Vec3 channel[3] = {{side * 0.11f, cushion + 0.05f, GEO_EYE.z + 0.31f}, {side * 0.12f, cushion + 0.36f, GEO_EYE.z + 0.355f}, {side * 0.09f, cushion + 0.64f, GEO_EYE.z + 0.39f}};
            Vec3 smooth[16];
            Tube(mesh, smooth, Smooth(channel, 3, 6, smooth, 16), 0.009f, 6, GEO_RIB);
        }
        // the spine: a gunmetal bar leaning back 8 deg from behind the saddle, six pads on it narrowing upward
        const Vec3 spineFoot = {0, cushion - 0.02f, GEO_EYE.z + 0.34f}, lean = Normalized(Vec3{0, 1, 0.14f});
        const Vec3 spine[2] = {spineFoot, spineFoot + lean * 0.66f};
        Tube(mesh, spine, 2, 0.022f, 10, GEO_SHELL);
        for (int k = 0; k < 6; ++k)
        {
            const Vec3 at = spineFoot + lean * (0.10f + k * 0.098f) + Vec3{0, 0, -0.03f};
            RoundedBar(mesh, MakeFrame(at, {1, 0, 0}, lean), 0.075f - k * 0.005f, 0.042f, 0.026f, 0.020f, GEO_PAD);
        }
        const float crest[5][2] = {{0, 0.062f}, {0.025f, 0.062f}, {0.05f, 0.052f}, {0.068f, 0.032f}, {0.076f, 0}};
        Lathe(mesh, MakeFrame(spineFoot + lean * 0.66f + Vec3{0, 0, -0.03f}, {1, 0, 0}, lean), crest, 5, 14, GEO_PAD, 0.5f);
        const Vec3 post[2] = {{0, GEO_FLOOR, GEO_EYE.z + 0.05f}, {0, cushion - 0.05f, GEO_EYE.z + 0.05f}};
        Tube(mesh, post, 2, 0.04f, 10, GEO_SHELL);
    }
}


// ---------------------------------------------------------------------------------------------------------------- AT-ST
// The AT-ST's head (the vehicle proposal 2026-11-04, docs/research/vehicle-controls-proposal.md; idren2023 2026-11-11: the
// co-pilot on the pilot's right, his area the pilot's mirrored, one mesh): the game's first-person cockpit
// (imp_1st_cockpit_ATST: the pilot's window and dash, the console to his left, the vent box over the window) and its
// mirror about the cab's middle (tools/stockcockpit.py ATST), closed into the whole head round both crew, a top hatch
// with its hand wheel, a rear hatch. Cockpit model space (x right, y up, z back); the pilot's eye nearer the window
// than the game's (0, 1.5, 1.5); the throttle lever's console under the right hand (WalkerDrive's pivot: eye + (0.25,
// -0.62, -0.30)). Finishes from the cockpit's own texture (imp_cockpit_ATST.tga, 512 px): the patches below (u, v from
// the top left).
enum AtstFinish
{
    ATST_PLATE,   // grimy grey plate (below the long beam)
    ATST_RIVETS,  // the riveted side panel
    ATST_CONSOLE, // the switch-and-screen console
    ATST_BUTTONS, // the button strip
    ATST_VENT,    // the slatted vent panel
    ATST_BEAM,    // the long beam with its round sockets
    ATST_GRID,    // the diamond mesh (the stock right wall's)
    ATST_LAMPS,   // the red / green lamp panel
    ATST_READOUT, // the red readout
    ATST_RIBBED,  // the ribbed strip
    ATST_FRAME,   // dark frames (the plate, darker)
    ATST_DARK,    // recesses
    ATST_SEAT,    // seat pads
    ATST_PIPE,    // pipes, rails
    ATST_STOCK,   // the game's pilot model and its mirror (StockPieces), its own mapping
    ATST_STOCK_FX, //   its lamps (effects texture)
    ATST_SCREEN_FRAME,   // the HUD screens' stock frame (texture 5, imp_1st_cockpit_securitycam: the tanks' look)
    ATST_SCREEN_GLASS,   //   their glass
    ATST_SCREEN_BACKING, //   behind the glass
    ATST_FINISH_COUNT
};

const Finish ATST_FINISHES[ATST_FINISH_COUNT] = {
    {{0.004f, 0.590f, 0.540f, 0.775f}, 1.1f, 1, 0x8A8C90, 0.18f, 14, 0},      // plate (2..276, 302..397)
    {{0.006f, 0.141f, 0.160f, 0.430f}, 0.5f, 1, 0x8A8C90, 0.20f, 14, 0},      // riveted panel (3..82, 72..220)
    {{0.188f, 0.170f, 0.500f, 0.380f}, 0.62f, 1, 0x9A9C9E, 0.15f, 12, 0.18f}, // console (96..256, 87..195)
    {{0.190f, 0.006f, 0.578f, 0.118f}, 0.8f, 1, 0x9A9C9E, 0.15f, 12, 0.12f},  // button strip (97..296, 3..60)
    {{0.523f, 0.141f, 0.660f, 0.441f}, 0.4f, 1, 0x86888C, 0.15f, 12, 0},      // vent (268..338, 72..226)
    {{0.002f, 0.441f, 0.700f, 0.580f}, 1.6f, 1, 0x8C8E92, 0.22f, 16, 0},      // beam (1..358, 226..297)
    {{0.002f, 0.820f, 0.428f, 0.998f}, 0.8f, 1, 0x7A7C80, 0.25f, 18, 0},      // diamond mesh (1..219, 420..511)
    {{0.740f, 0.277f, 0.965f, 0.400f}, 0.36f, 1, 0x9A9C9E, 0.10f, 10, 0.5f},  // lamp panel (379..494, 142..205)
    {{0.700f, 0.445f, 0.980f, 0.560f}, 0.42f, 1, 0x9A9C9E, 0.10f, 10, 0.55f}, // red readout (358..502, 228..287)
    {{0.002f, 0.779f, 0.428f, 0.818f}, 0.6f, 1, 0x8A8C90, 0.30f, 20, 0},      // ribbed strip (1..219, 399..419)
    {{0.004f, 0.590f, 0.540f, 0.775f}, 0.9f, 1, 0x4A4C50, 0.22f, 16, 0},      // dark frames
    {{0, 0, 1, 1}, 1, 0, 0x101113, 0.05f, 6, 0},                             // recesses
    {{0.004f, 0.590f, 0.540f, 0.775f}, 0.5f, 1, 0x5A5650, 0.18f, 10, 0},      // seat pads (worn leather grey)
    {{0, 0, 1, 1}, 1, 0, 0x55585C, 0.25f, 20, 0},                            // pipes (darker: read as white, review)
    {{0, 0, 1, 1}, 1, 1, 0x808080, 0.18f, 14, 0},                            // the stock model
    {{0, 0, 1, 1}, 1, 2, 0xC0C0C0, 0, 1, 1.0f},                              // its lamps
    ScreenFrameFinish(),                                                     // stock screen frame
    ScreenGlassFinish(IMP_SCREEN_ART),                                       // its glass
    ScreenBackingFinish(IMP_SCREEN_ART),                                     // behind the glass
};

constexpr Vec3 ATST_EYE = {0.0f, 1.42f, 0.25f};
constexpr float ATST_MIRROR_X = 1.30f;              // the cab's middle: the co-pilot's side is the pilot's mirrored
constexpr float ATST_GUNNER_X = 2.0f * ATST_MIRROR_X; // the co-pilot's eye (tools/stockcockpit.py ATST)

// A wall plane (z = const) from x0..x1, y0..y1 with the hole hx0..hx1, hy0..hy1 left open: four tiled quads.
void AtstWallWithHole(Mesh& mesh, float z, float x0, float x1, float y0, float y1, float hx0, float hx1, float hy0,
                      float hy1, int finish)
{
    TiledQuad(mesh, {x0, y0, z}, {hx0 - x0, 0, 0}, {0, y1 - y0, 0}, finish);
    TiledQuad(mesh, {hx1, y0, z}, {x1 - hx1, 0, 0}, {0, y1 - y0, 0}, finish);
    TiledQuad(mesh, {hx0, y0, z}, {hx1 - hx0, 0, 0}, {0, hy0 - y0, 0}, finish);
    TiledQuad(mesh, {hx0, hy1, z}, {hx1 - hx0, 0, 0}, {0, y1 - hy1, 0}, finish);
}

// The walkers' HUD screens (idren2023 2026-11-11: the AT-AT and AT-ST had none, unlike the other vehicles): three stock
// screens on posts from the dash, turned to the eye, below the window's line: left health and score, middle the map,
// right ammunition and target. `readout` lights behind each glass (the game draws few readouts for a walker's pilot);
// displays only for the seat whose HUD they carry (the other seat's screens are its mirror, readouts only).
void WalkerScreens(Mesh& mesh, const Vec3& eye, const Vec3 (&at)[3], const float (&half)[3][2], float postFootY,
                   const StockScreenLook& look, int readout, int postFinish, bool hasDisplays)
{
    const Vec3 worldUp = {0, 1, 0};
    for (int i = 0; i < 3; ++i)
    {
        const Vec3 normal = Normalized(eye - at[i]);
        const Vec3 up = Normalized(worldUp - normal * Dot(worldUp, normal));
        const Frame face = MakeFrame(at[i], Cross(up, normal), up);
        const float w = half[i][0], h = half[i][1];
        StockScreen(mesh, face, w, h, SCREEN_TEXEL_METERS * h / 0.0675f, look, 0.03f);
        Quad(mesh, face.At(-w, h, -0.002f), face.At(w, h, -0.002f), face.At(w, -h, -0.002f), face.At(-w, -h, -0.002f),
             readout);
        const Vec3 post[2] = {face.At(0, -h, -0.03f), {at[i].x, postFootY, at[i].z + normal.z * -0.03f}};
        Tube(mesh, post, 2, 0.018f, 10, postFinish);
        if (!hasDisplays)
        {
            continue;
        }
        SetDisplayScreen(&look);
        if (i == 0)
        {
            AddDisplay(GROUP_HEALTH, face, 0, h * 0.28f, w * 1.8f, h * 1.15f);
            AddDisplay(GROUP_SCORE, face, 0, -h * 0.58f, w * 1.6f, h * 0.45f);
        }
        else if (i == 1)
        {
            AddDisplay(GROUP_MAP, face, 0, 0, w * 1.85f, h * 1.85f);
        }
        else
        {
            AddDisplay(GROUP_AMMUNITION, face, 0, h * 0.28f, w * 1.8f, h * 0.95f);
            AddDisplay(GROUP_TARGET, face, 0, -h * 0.58f, w * 1.4f, h * 0.5f);
        }
        SetDisplayScreen(nullptr);
    }
}

// Trims along a box cabin's inside corners (2026-11-11: the quads' shared edges let the sky through as bright lines
// in the AT-AT's and AT-ST's cabs): the roof's long edges and back edge, the back's upright corners, the floor's side
// and back edges; `size` the trim's half thickness.
void CabinCornerTrims(Mesh& mesh, float xl, float xr, float floorY, float roof, float front, float back, float size,
                      int finish)
{
    const float midZ = (front + back) * 0.5f, halfZ = (back - front) * 0.5f, midX = (xl + xr) * 0.5f;
    const float halfX = (xr - xl) * 0.5f, midY = (floorY + roof) * 0.5f, halfY = (roof - floorY) * 0.5f;
    for (const float x : {xl, xr})
    {
        Box(mesh, MakeFrame({x, roof, midZ}, {1, 0, 0}, {0, 1, 0}), {size, size, halfZ}, size * 0.4f, finish);
        Box(mesh, MakeFrame({x, floorY, midZ}, {1, 0, 0}, {0, 1, 0}), {size, size, halfZ}, size * 0.4f, finish);
        Box(mesh, MakeFrame({x, midY, back}, {1, 0, 0}, {0, 1, 0}), {size, halfY, size}, size * 0.4f, finish);
    }
    Box(mesh, MakeFrame({midX, roof, back}, {1, 0, 0}, {0, 1, 0}), {halfX, size, size}, size * 0.4f, finish);
    Box(mesh, MakeFrame({midX, floorY, back}, {1, 0, 0}, {0, 1, 0}), {halfX, size, size}, size * 0.4f, finish);
}

void BuildAtst(Mesh& mesh)
{
    // idren2023 2026-11-11: the co-pilot on the pilot's right with the pilot's area mirrored, the cab one mesh. Both
    // fronts are the game's pilot model, cut at the cab's middle (ATST_MIRROR_X) and mirrored (its mesh wall at the
    // pilot's right left out); the game's own model is not drawn (isWholeCockpit). The walls just outside the stock
    // consoles (x -1.62 and its mirror).
    constexpr float M = ATST_MIRROR_X;
    constexpr float XL = -1.75f, XR = 2.0f * M - XL, FLOOR = 0.04f, ROOF = 2.72f, FRONT = -0.70f, BACK = 1.35f;
    // The windows: the stock pilot window's opening (x -0.91..0.98, up to y 2.33) and its mirror.
    constexpr float WX0 = -0.91f, WX1 = 0.98f, WTOP = 2.33f;
    StockPieces(mesh, StockVertices("STOCK_ATST"), StockCount("STOCK_ATST"), ATST_STOCK, ATST_STOCK_FX, ATST_STOCK);
    // Floor: plates, a ribbed tread down each seat's well.
    TiledQuad(mesh, {XL, FLOOR, FRONT}, {XR - XL, 0, 0}, {0, 0, BACK - FRONT}, ATST_PLATE);
    for (const float x : {0.0f, ATST_GUNNER_X})
    {
        Box(mesh, MakeFrame({x, FLOOR + 0.008f, 0.30f}, {1, 0, 0}, {0, 1, 0}), {0.30f, 0.008f, 0.55f}, 0.004f,
            ATST_RIBBED);
    }
    // Front wall round the two stock window frames: the outer strips, the pillar between them, the band above.
    TiledQuad(mesh, {XL, FLOOR, FRONT}, {WX0 - XL, 0, 0}, {0, ROOF - FLOOR, 0}, ATST_PLATE);
    TiledQuad(mesh, {WX1, FLOOR, FRONT}, {2.0f * M - 2.0f * WX1, 0, 0}, {0, ROOF - FLOOR, 0}, ATST_PLATE);
    TiledQuad(mesh, {2.0f * M - WX0, FLOOR, FRONT}, {XR - (2.0f * M - WX0), 0, 0}, {0, ROOF - FLOOR, 0}, ATST_PLATE);
    TiledQuad(mesh, {WX0, WTOP, FRONT}, {2.0f * M - 2.0f * WX0, 0, 0}, {0, ROOF - WTOP, 0}, ATST_PLATE);
    // under each window, down to the floor (2026-11-11: the stock frame stops at y 0.50; snow showed through below it)
    TiledQuad(mesh, {WX0, FLOOR, FRONT}, {WX1 - WX0, 0, 0}, {0, 0.52f - FLOOR, 0}, ATST_PLATE);
    TiledQuad(mesh, {2.0f * M - WX1, FLOOR, FRONT}, {WX1 - WX0, 0, 0}, {0, 0.52f - FLOOR, 0}, ATST_PLATE);
    // Side walls, each the other's mirror: plates, a riveted panel and a vent beside each seat.
    for (int side = -1; side <= 1; side += 2)
    {
        const float x = side < 0 ? XL : XR, inward = static_cast<float>(-side);
        TiledQuad(mesh, {x, FLOOR, FRONT}, {0, 0, BACK - FRONT}, {0, ROOF - FLOOR, 0}, ATST_PLATE);
        Panel(mesh, MakeFrame({x + inward * 0.01f, 1.25f, 0.30f}, {0, 0, -inward}, {0, 1, 0}), 0.45f, 0.55f, 0.05f,
              0.03f, ATST_FRAME, ATST_RIVETS, 0.12f, ATST_PIPE);
        Panel(mesh, MakeFrame({x + inward * 0.01f, 1.25f, 1.05f}, {0, 0, -inward}, {0, 1, 0}), 0.22f, 0.55f, 0.05f,
              0.03f, ATST_FRAME, ATST_VENT, 0, ATST_PIPE);
    }
    // Roof: plates, the long beam across, the top hatch over the cab's middle with its hand wheel.
    TiledQuad(mesh, {XL, ROOF, FRONT}, {XR - XL, 0, 0}, {0, 0, BACK - FRONT}, ATST_PLATE);
    Box(mesh, MakeFrame({M, ROOF - 0.07f, 0.95f}, {1, 0, 0}, {0, 1, 0}), {(XR - XL) * 0.5f, 0.07f, 0.09f}, 0.02f,
        ATST_BEAM);
    {
        // (rings: height along the frame's y, radius) the rim hanging from the roof round the hatch's lid
        const float rim[6][2] = {{0.00f, 0.40f}, {-0.05f, 0.40f}, {-0.07f, 0.34f}, {-0.07f, 0.30f}, {-0.02f, 0.30f},
                                 {-0.02f, 0.01f}};
        Lathe(mesh, MakeFrame({M, ROOF - 0.005f, 0.45f}, {1, 0, 0}, {0, 1, 0}), rim, 6, 24, ATST_FRAME);
        ValveWheel(mesh, MakeFrame({M, ROOF - 0.08f, 0.45f}, {1, 0, 0}, {0, 0, 1}), 0.16f, 0.05f, ATST_PIPE, ATST_FRAME);
    }
    // Back wall: the rear hatch in the middle, the readout over it, lamp boards behind the seats.
    TiledQuad(mesh, {XL, FLOOR, BACK}, {XR - XL, 0, 0}, {0, ROOF - FLOOR, 0}, ATST_PLATE);
    Panel(mesh, MakeFrame({M, 1.05f, BACK - 0.01f}, {-1, 0, 0}, {0, 1, 0}), 0.42f, 0.85f, 0.07f, 0.04f, ATST_FRAME,
          ATST_PLATE, 0.14f, ATST_PIPE);
    Quad(mesh,
         {M + 0.30f, 1.95f, BACK - 0.02f},
         {M - 0.30f, 1.95f, BACK - 0.02f},
         {M - 0.30f, 2.12f, BACK - 0.02f},
         {M + 0.30f, 2.12f, BACK - 0.02f}, ATST_READOUT);
    for (const float x : {-0.55f, ATST_GUNNER_X + 0.55f})
    {
        Panel(mesh, MakeFrame({x, 1.30f, BACK - 0.01f}, {-1, 0, 0}, {0, 1, 0}), 0.30f, 0.40f, 0.04f, 0.03f, ATST_FRAME,
              ATST_LAMPS, 0, ATST_PIPE);
    }
    // Frames round the head: two ribs across, pipes along the roof's edges.
    for (const float z : {0.62f, 1.30f})
    {
        CabinRib(mesh, z, XL, XR, FLOOR, ROOF, 0.25f, 0.07f, 0.08f, true, true, ATST_FRAME);
    }
    for (const float x : {XL + 0.10f, XR - 0.10f})
    {
        const Vec3 pipe[2] = {{x, ROOF - 0.10f, FRONT}, {x, ROOF - 0.10f, BACK}};
        Tube(mesh, pipe, 2, 0.03f, 8, ATST_PIPE);
    }
    // Seats: the pilot's under his eye, the co-pilot's under his (Imperial seats with side wings).
    const SeatLook look = {0.22f, 0.58f, 0.12f, true, false, ATST_SEAT, ATST_FRAME, ATST_PIPE, ATST_SEAT, ATST_DARK};
    Seat(mesh, ATST_EYE, FLOOR, look);
    Seat(mesh, {ATST_GUNNER_X, ATST_EYE.y, ATST_EYE.z}, FLOOR, look);
    CabinCornerTrims(mesh, XL, XR, FLOOR, ROOF, FRONT, BACK, 0.04f, ATST_FRAME);
    {
        const StockScreenLook screens = {&IMP_SCREEN_ART, ATST_SCREEN_FRAME, ATST_SCREEN_GLASS, ATST_SCREEN_BACKING,
                                         ATST_FRAME};
        const float half[3][2] = {{0.10f, 0.06f}, {0.07f, 0.06f}, {0.10f, 0.06f}};
        for (int seat = 0; seat < 2; ++seat)
        {
            const float x = seat ? ATST_GUNNER_X : ATST_EYE.x, mirror = seat ? -1.0f : 1.0f;
            const Vec3 eye = {x, ATST_EYE.y, ATST_EYE.z};
            const Vec3 at[3] = {{x - mirror * 0.28f, 1.02f, -0.30f}, {x, 1.02f, -0.30f}, {x + mirror * 0.28f, 1.02f, -0.30f}};
            WalkerScreens(mesh, eye, at, half, 0.66f, screens, ATST_CONSOLE, ATST_PIPE, seat == 0);
        }
    }
    // The throttle lever's console under the pilot's right hand (WalkerDrive's pivot).
    const Vec3 lever = {ATST_EYE.x + 0.25f, ATST_EYE.y - 0.62f, ATST_EYE.z - 0.30f};
    Box(mesh, MakeFrame({lever.x + 0.02f, (FLOOR + lever.y) * 0.5f, lever.z + 0.05f}, {1, 0, 0}, {0, 1, 0}),
        {0.11f, (lever.y - FLOOR) * 0.5f, 0.26f}, 0.02f, ATST_FRAME);
    Quad(mesh,
         {lever.x - 0.08f, lever.y + 0.002f, lever.z - 0.18f},
         {lever.x + 0.12f, lever.y + 0.002f, lever.z - 0.18f},
         {lever.x + 0.12f, lever.y + 0.002f, lever.z + 0.28f},
         {lever.x - 0.08f, lever.y + 0.002f, lever.z + 0.28f}, ATST_BUTTONS);
    Box(mesh, MakeFrame({lever.x, lever.y + 0.004f, lever.z}, {1, 0, 0}, {0, 1, 0}), {0.018f, 0.004f, 0.12f}, 0.002f,
        ATST_DARK);
    // The co-pilot's twin grips on his dash (GunnerSeat's AT-ST cabin puts his hands here: x +-0.20 about his eye,
    // y 0.88, z 0.0, leaning back 18 deg).
    for (const float dx : {-0.20f, 0.20f})
    {
        const Vec3 base = {ATST_GUNNER_X + dx, 0.74f, -0.02f};
        Box(mesh, MakeFrame(base, {1, 0, 0}, {0, 1, 0}), {0.03f, 0.03f, 0.04f}, 0.01f, ATST_FRAME);
        const Vec3 grip[2] = {base + Vec3{0, 0.03f, 0}, base + Vec3{0, 0.20f, 0.05f}};
        Tube(mesh, grip, 2, 0.022f, 10, ATST_SEAT);
    }
}

// ------------------------------------------------------------------------------------------------------- spider walker
// The homing spider droid's core (the vehicle proposal 2026-11-04, idren2023: a cabin with a viewscreen; his AT-ST
// verdict: the larger, stock-like cabin): the game's first-person cockpit (cis_1st_cockpit_spiderwalker: a flat brown
// plate 7 m wide, 3.5 m before the game's eye, its round port the droid's photoreceptor) kept as the front wall of an
// octagonal room round the pilot, the port glazed red as a viewscreen; a droid seat, a dash under the port, the
// throttle lever's console under the right hand (WalkerDrive's pivot: eye + (0.25, -0.62, -0.30)). The film droid has
// no crew: the room is ours. Cockpit model space (x right, y up, z back); the pilot's eye 1.5 m nearer the port than
// the game's (0, 1.5, 1.5), so the port spans about 40 x 75 degrees. Finishes from the plate's own texture
// (cis_1st_cockpit_spiderwalker.tga, 512 px).
enum SpiderFinish
{
    SPIDER_PLATE, // grimy brown plate (the texture's plain top)
    SPIDER_SEAM,  // plate with the radial seam lines
    SPIDER_GRATE, // the hex grate beside the port
    SPIDER_BAND,  // the dark ribbed band along the plate's foot
    SPIDER_FRAME, // dark frames (the plate, darker)
    SPIDER_DARK,  // recesses
    SPIDER_METAL, // pipes, rails, the seat
    SPIDER_LAMP,  // red lamps (lit)
    SPIDER_GLASS, // the viewscreen: the port glazed red, see-through
    SPIDER_PORT,  // the whole first-person texture laid over the sphere's front (idren2023: its port modelled)
    SPIDER_FINISH_COUNT
};

const Finish SPIDER_FINISHES[SPIDER_FINISH_COUNT] = {
    {{0.020f, 0.020f, 0.300f, 0.180f}, 2.0f, 1, 0x9A9288, 0.12f, 12, 0},  // plate (10..154, 10..92)
    {{0.600f, 0.020f, 0.980f, 0.300f}, 1.6f, 1, 0x9A9288, 0.15f, 12, 0},  // seams (307..502, 10..154)
    {{0.270f, 0.270f, 0.330f, 0.400f}, 0.25f, 1, 0x8A8680, 0.25f, 18, 0}, // hex grate (138..169, 138..205)
    {{0.000f, 0.930f, 1.000f, 0.990f}, 2.0f, 1, 0x8A8680, 0.25f, 18, 0},  // foot band (0..512, 476..507)
    {{0.020f, 0.020f, 0.300f, 0.180f}, 0.9f, 1, 0x4C4640, 0.20f, 16, 0},  // dark frames
    {{0, 0, 1, 1}, 1, 0, 0x100E0C, 0.05f, 6, 0},                         // recesses
    {{0, 0, 1, 1}, 1, 0, 0x6E6A66, 0.45f, 30, 0},                        // metal
    {{0, 0, 1, 1}, 1, 0, 0xFF3020, 0, 1, 0.9f},                          // red lamps
    {{0, 0, 1, 1}, 1, 0, 0xC03020, 0.6f, 60, 0.05f, 0.12f},              // the viewscreen's red glass
    {{0, 0, 1, 1}, 1, 1, 0x9A9288, 0.15f, 12, 0, 1.0f, false, true},       // the texture, its own mapping
};

constexpr Vec3 SPIDER_EYE = {0.0f, 1.5f, 0.0f};

/// A droid's pod round the pilot (the spider's and the hailfire's cabins, idren2023 2026-11-04: a seat inside the hull's
/// sphere; an enclosed cabin with a viewscreen): an ellipsoid room (radii x, y, z about its middle) cut flat by the
/// floor, a viewscreen window in its front (longitude 0 straight ahead, -z: +-windowLongitude across, windowLow..
/// windowHigh up from the middle), glazed; frame rings and ribs (none across the window), a band round the floor's edge.
struct PodLook
{
    int plate, seam, frame, band, glass;
};

void DroidPod(Mesh& mesh, Vec3 centre, Vec3 radii, float floorY, float windowLongitude, float windowLow,
              float windowHigh, const PodLook& look)
{
    constexpr float PI_F = 3.14159265f, DEGREE = PI_F / 180.0f;
    const float floorLatitude = asinf((floorY - centre.y) / radii.y);
    const auto at = [&](float latitude, float longitude) {
        return centre + Vec3{radii.x * cosf(latitude) * sinf(longitude), radii.y * sinf(latitude),
                             -radii.z * cosf(latitude) * cosf(longitude)};
    };
    constexpr int LONGITUDES = 36; // 10 deg
    // Rings: the floor, the window's edges, every 15 deg up to the pole (sorted, the ones under the floor left out).
    float latitudes[12];
    int RINGS = 0;
    for (const float latitude : {floorLatitude, windowLow, windowHigh, 0.0f, 15.0f * DEGREE, 30.0f * DEGREE,
                                 45.0f * DEGREE, 60.0f * DEGREE, 75.0f * DEGREE, 89.9f * DEGREE})
    {
        if (latitude < floorLatitude - 0.001f)
        {
            continue;
        }
        int at = RINGS++;
        for (; at > 0 && latitudes[at - 1] > latitude; --at)
        {
            latitudes[at] = latitudes[at - 1];
        }
        latitudes[at] = latitude;
    }
    // the patch's real size each way (2026-11-09 texturing pass: one size for both had stretched the plate's wide
    // strip 60 % of the wall)
    const float metres = MetersU(mesh, look.plate), metresV = MetersV(mesh, look.plate);
    const float meanRadius = (radii.x + radii.z) * 0.5f;
    const auto longitudeOf = [&](int k) { return (k - LONGITUDES / 2) * 2 * PI_F / LONGITUDES; };
    const auto inWindow = [&](float lo0, float lo1, float la0, float la1) {
        return la0 >= windowLow - 0.001f && la1 <= windowHigh + 0.001f && lo0 >= -windowLongitude - 0.001f &&
               lo1 <= windowLongitude + 0.001f;
    };
    for (int ring = 0; ring + 1 < RINGS; ++ring)
    {
        const float la0 = latitudes[ring], la1 = latitudes[ring + 1];
        if (la1 <= la0)
        {
            continue;
        }
        for (int k = 0; k < LONGITUDES; ++k)
        {
            const float lo0 = longitudeOf(k), lo1 = longitudeOf(k + 1);
            const int finish = inWindow(lo0, lo1, la0, la1) ? look.glass : (ring == 0 ? look.seam : look.plate);
            if (finish < 0)
            {
                continue; // the window left open (PortWall fills it)
            }
            const float width = meanRadius * cosf((la0 + la1) * 0.5f);
            const Vec3 corners[4] = {at(la0, lo0), at(la0, lo1), at(la1, lo1), at(la1, lo0)};
            const float st[4][2] = {{lo0 * width / metres, la0 * radii.y / metresV},
                                    {lo1 * width / metres, la0 * radii.y / metresV},
                                    {lo1 * width / metres, la1 * radii.y / metresV},
                                    {lo0 * width / metres, la1 * radii.y / metresV}};
            MapPoint points[4];
            for (int c = 0; c < 4; ++c)
            {
                points[c] = {corners[c], Normalized(centre - corners[c]), st[c][0], st[c][1]};
            }
            MappedPolygon(mesh, points, 4, finish, false);
        }
    }
    const auto bar = [&](Vec3 a, Vec3 b, float half, int finish) {
        const Vec3 mid = (a + b) * 0.5f;
        const Vec3 inward = Normalized(centre - mid);
        Box(mesh, MakeFrame(mid + inward * half, Normalized(b - a), inward), {Length(b - a) * 0.5f, half, half},
            half * 0.4f, finish);
    };
    for (const float latitude : {windowLow, windowHigh, 45.0f * DEGREE, 60.0f * DEGREE})
    {
        for (int k = 0; k < LONGITUDES; ++k)
        {
            if (look.glass < 0 && inWindow(longitudeOf(k), longitudeOf(k + 1), latitude, latitude))
            {
                continue;
            }
            bar(at(latitude, longitudeOf(k)), at(latitude, longitudeOf(k + 1)), 0.05f, look.frame);
        }
    }
    for (int k = 0; k < 12; ++k)
    {
        const float longitude = (k - 6) * 30.0f * DEGREE;
        for (int step = 0; step < 6; ++step)
        {
            const float la0 = floorLatitude + (90.0f * DEGREE - floorLatitude) * step / 6.0f;
            const float la1 = floorLatitude + (90.0f * DEGREE - floorLatitude) * (step + 1) / 6.0f;
            if (fabsf(longitude) < windowLongitude && la1 > windowLow && la0 < windowHigh)
            {
                continue; // nothing across the window (its middle is the aim line)
            }
            bar(at(la0, longitude), at(la1, longitude), 0.04f, look.frame);
        }
    }
    for (const float longitude : {-windowLongitude, windowLongitude})
    {
        bar(at(windowLow, longitude), at(windowHigh, longitude), 0.035f, look.frame);
    }
    // The floor: wedges of plates (a polygon takes at most 24 points), a dark band round its edge.
    Vec3 disc[LONGITUDES];
    for (int k = 0; k < LONGITUDES; ++k)
    {
        disc[k] = at(floorLatitude, longitudeOf(k));
        disc[k].y = floorY;
    }
    for (int k = 0; k < LONGITUDES; ++k)
    {
        const Vec3 wedge[3] = {{centre.x, floorY, centre.z}, disc[k], disc[(k + 1) % LONGITUDES]};
        PlanarPolygon(mesh, wedge, 3, {1, 0, 0}, look.plate);
        const Vec3 a = disc[k] + (Vec3{centre.x, floorY, centre.z} - disc[k]) * 0.03f;
        const Vec3 b = disc[(k + 1) % LONGITUDES] + (Vec3{centre.x, floorY, centre.z} - disc[(k + 1) % LONGITUDES]) * 0.03f;
        Box(mesh, MakeFrame((a + b) * 0.5f + Vec3{0, 0.04f, 0}, Normalized(b - a), {0, 1, 0}),
            {Length(b - a) * 0.5f, 0.04f, 0.07f}, 0.01f, look.band);
    }
}

/// The walker throttle lever's console under the pilot's right hand (WalkerDrive's pivot: eye + (0.25, -0.62,
/// -0.30)), from the floor (or a dais) up, with the slot and the three notches' lamps (reverse, neutral, forward).
void ThrottleConsole(Mesh& mesh, Vec3 eye, float floorY, int frame, int dark, int lamp)
{
    const Vec3 lever = {eye.x + 0.25f, eye.y - 0.62f, eye.z - 0.30f};
    Box(mesh, MakeFrame({lever.x + 0.02f, (floorY + lever.y) * 0.5f, lever.z + 0.05f}, {1, 0, 0}, {0, 1, 0}),
        {0.11f, (lever.y - floorY) * 0.5f, 0.26f}, 0.02f, frame);
    Box(mesh, MakeFrame({lever.x, lever.y + 0.004f, lever.z}, {1, 0, 0}, {0, 1, 0}), {0.018f, 0.004f, 0.12f}, 0.002f,
        dark);
    for (const float dz : {-0.14f, 0.0f, 0.14f})
    {
        Box(mesh, MakeFrame({lever.x + 0.07f, lever.y + 0.006f, lever.z + dz}, {1, 0, 0}, {0, 1, 0}),
            {0.012f, 0.004f, 0.012f}, 0.002f, lamp);
    }
}

/// A droid's first-person frame made solid on its pod's front (idren2023 2026-11-04: the viewport a 3D modelled version
/// of the first-person texture as part of the cabin): over the pod's front (longitude +-regionLongitude, from the floor
/// up to regionHigh) the whole frame texture is laid at `scale` radians a texture width, the port's middle (portU,
/// portV) at the latitude `portLatitude`; cells inside the port (`isPort`) are glazed, the rest the texture; a lip
/// stands in round the port's edge, and the caller's own relief (rings) goes on with PortPoint.
struct PortWall
{
    Vec3 centre, radii;
    float floorY, regionLongitude, regionHigh;
    float portU, portV, portLatitude, scale, scaleUp; // radians a texture width across, and up (the frames are
                                                    // 1.22 times wider than tall: scaleUp = scale / 1.22)
    bool (*isPort)(float u, float v);
    int texture, glass, lip;

    float FloorLatitude() const { return asinf((floorY - centre.y) / radii.y); }
    Vec3 At(float latitude, float longitude, float inset = 0) const
    {
        const Vec3 p = centre + Vec3{radii.x * cosf(latitude) * sinf(longitude), radii.y * sinf(latitude),
                                     -radii.z * cosf(latitude) * cosf(longitude)};
        return p + Normalized(centre - p) * inset;
    }
    /// The point of the texture (u, v) on the wall, `inset` metres toward the pod's middle.
    Vec3 PortPoint(float u, float v, float inset = 0) const
    {
        return At(portLatitude - (v - portV) * scaleUp, (u - portU) * scale, inset);
    }
};

void BuildPortWall(Mesh& mesh, const PortWall& wall)
{
    constexpr float STEP = 2.0f * 3.14159265f / 180.0f;
    const float low = wall.FloorLatitude();
    const int across = static_cast<int>(2 * wall.regionLongitude / STEP + 0.5f);
    const int up = static_cast<int>((wall.regionHigh - low) / STEP + 0.999f);
    for (int j = 0; j < up; ++j)
    {
        const float la0 = low + (wall.regionHigh - low) * j / up, la1 = low + (wall.regionHigh - low) * (j + 1) / up;
        for (int i = 0; i < across; ++i)
        {
            const float lo0 = -wall.regionLongitude + STEP * i, lo1 = lo0 + STEP;
            const float u0 = wall.portU + lo0 / wall.scale, u1 = wall.portU + lo1 / wall.scale;
            const float v0 = wall.portV - (la0 - wall.portLatitude) / wall.scaleUp;
            const float v1 = wall.portV - (la1 - wall.portLatitude) / wall.scaleUp;
            const bool isPort = wall.isPort((u0 + u1) * 0.5f, (v0 + v1) * 0.5f);
            const Vec3 corners[4] = {wall.At(la0, lo0), wall.At(la0, lo1), wall.At(la1, lo1), wall.At(la1, lo0)};
            const float st[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
            MapPoint points[4];
            for (int c = 0; c < 4; ++c)
            {
                points[c] = {corners[c], Normalized(wall.centre - corners[c]), fminf(fmaxf(st[c][0], 0.0f), 0.999f),
                             fminf(fmaxf(st[c][1], 0.0f), 0.999f)};
            }
            MappedPolygon(mesh, points, 4, isPort ? wall.glass : wall.texture, false);
        }
    }
    // The lip: round the port's edge (found along rays from its middle), standing 0.09 m in.
    constexpr int EDGE = 120;
    Vec3 edge[EDGE];
    for (int k = 0; k < EDGE; ++k)
    {
        const float angle = 2 * 3.14159265f * k / EDGE;
        float inside = 0, outside = 0.6f;
        for (int n = 0; n < 24; ++n)
        {
            const float mid = (inside + outside) * 0.5f;
            (wall.isPort(wall.portU + mid * cosf(angle), wall.portV + mid * sinf(angle)) ? inside : outside) = mid;
        }
        edge[k] = wall.PortPoint(wall.portU + inside * cosf(angle), wall.portV + inside * sinf(angle));
    }
    for (int k = 0; k < EDGE; ++k)
    {
        const Vec3 a = edge[k], b = edge[(k + 1) % EDGE];
        const Vec3 mid = (a + b) * 0.5f;
        const Vec3 inward = Normalized(wall.centre - mid);
        Box(mesh, MakeFrame(mid + inward * 0.045f, Normalized(b - a), inward), {Length(b - a) * 0.5f + 0.012f, 0.035f, 0.045f},
            0.012f, wall.lip);
    }
}

// The spider's port (cis_1st_cockpit_spiderwalker.tga, measured): a round middle (radius 0.17 about (0.508, 0.53))
// with a wing either side (u 0.195..0.822, v 0.455..0.605, rounded ends).
bool IsSpiderPort(float u, float v)
{
    const float du = u - 0.508f, dv = v - 0.53f;
    if (du * du + dv * dv < 0.17f * 0.17f)
    {
        return true;
    }
    constexpr float HALF = 0.075f, END = 0.313f - 0.075f; // the wings' half height; their ends' round middles
    if (fabsf(dv) > HALF)
    {
        return false;
    }
    const float along = fabsf(du);
    return along < END || (along - END) * (along - END) + dv * dv < HALF * HALF;
}

// The hailfire's port (cis_1st_cockpit_hailfiredroid.tga, measured): a cross, the bar across u 0.125..0.877 v
// 0.37..0.63, the bar up u 0.40..0.60 v 0.18..0.81, stepped shoulders between them.
bool IsHailfirePort(float u, float v)
{
    const float du = fabsf(u - 0.5f), dv = fabsf(v - 0.495f);
    return (du < 0.376f && dv < 0.13f) || (du < 0.10f && v > 0.18f && v < 0.81f) ||
           (du < 0.19f && dv < 0.20f && du + dv < 0.33f);
}

void BuildSpider(Mesh& mesh)
{
    // The sphere (idren2023 2026-11-04: a seat inside the hull's sphere): the hull's core (cis_walk_spider.msh: radius
    // 2.5 about the hull's (0, 6, 0), the game's eye at its top, (0, 8, 0)), 0.2 m of shell inside it; the eye 0.45 m
    // above its middle (the world is still seen from the game's eye; the sphere is drawn round ours). The floor a flat
    // disc 0.9 m under the middle; the seat on a dais, the eye 1.2 m over its top. The viewscreen +-50 deg across,
    // -8..+30 deg up from the middle (from the eye about -20..+22 deg), glazed red.
    constexpr float DEGREE = 3.14159265f / 180.0f;
    const Vec3 centre = {SPIDER_EYE.x, SPIDER_EYE.y - 0.45f, SPIDER_EYE.z + 0.30f};
    const float floorY = centre.y - 0.90f;
    // The front (+-70 deg, up to 75 deg) is the first-person frame made solid (idren2023 2026-11-04): its port at the
    // eye's height, the wings +-48 deg across, the round middle 26 deg in radius; a bezel ring round the middle as the
    // texture's (radius 0.30..0.36 of its width), clamps on it at the four points.
    const float eyeLatitude = asinf((SPIDER_EYE.y - centre.y) / 2.30f);
    const PortWall wall = {centre, {2.30f, 2.30f, 2.30f}, floorY, 70.0f * DEGREE, 75.0f * DEGREE, 0.508f, 0.53f,
                           eyeLatitude, 170.0f * DEGREE, 140.0f * DEGREE, IsSpiderPort, SPIDER_PORT, SPIDER_GLASS, SPIDER_FRAME};
    DroidPod(mesh, centre, {2.30f, 2.30f, 2.30f}, floorY, 70.0f * DEGREE, wall.FloorLatitude(), 75.0f * DEGREE,
             {SPIDER_PLATE, SPIDER_SEAM, SPIDER_FRAME, SPIDER_BAND, -1});
    BuildPortWall(mesh, wall);
    {
        constexpr int SEGMENTS = 96;
        for (int k = 0; k < SEGMENTS; ++k)
        {
            const float a0 = 6.2831853f * k / SEGMENTS, a1 = 6.2831853f * (k + 1) / SEGMENTS;
            const Vec3 a = wall.PortPoint(0.508f + 0.33f * cosf(a0), 0.53f + 0.33f * sinf(a0), 0.05f);
            const Vec3 b = wall.PortPoint(0.508f + 0.33f * cosf(a1), 0.53f + 0.33f * sinf(a1), 0.05f);
            Box(mesh, MakeFrame((a + b) * 0.5f, Normalized(b - a), Normalized(centre - (a + b) * 0.5f)),
                {Length(b - a) * 0.5f + 0.01f, 0.07f, 0.05f}, 0.02f, SPIDER_FRAME);
        }
        for (int k = 0; k < 4; ++k)
        {
            const float angle = 1.5707963f * k + 0.7853982f;
            const Vec3 p = wall.PortPoint(0.508f + 0.33f * cosf(angle), 0.53f + 0.33f * sinf(angle), 0.09f);
            const Vec3 inward = Normalized(centre - p);
            const Vec3 along = Normalized(Cross(inward, Vec3{0, 1, 0}));
            Box(mesh, MakeFrame(p, along, Cross(along, inward) * -1.0f), {0.10f, 0.08f, 0.05f}, 0.02f, SPIDER_METAL);
        }
    }
    const float seatFloor = SPIDER_EYE.y - 1.20f; // the dais's top
    {
        const float dais[4][2] = {{0.0f, 0.75f}, {seatFloor - floorY - 0.03f, 0.75f}, {seatFloor - floorY, 0.72f},
                                  {seatFloor - floorY, 0.0f}};
        Lathe(mesh, MakeFrame({SPIDER_EYE.x, floorY, SPIDER_EYE.z + 0.15f}, {1, 0, 0}, {0, 1, 0}), dais, 4, 24,
              SPIDER_FRAME);
    }
    // The dash under the window (its top under the line from the eye to the window's foot), grates and red lamps.
    {
        const float top = floorY + 0.17f; // review: low, the port's foot clear
        Box(mesh, MakeFrame({0, (floorY + top) * 0.5f, -1.55f}, {1, 0, 0}, {0, 1, 0}),
            {0.75f, (top - floorY) * 0.5f, 0.25f}, 0.04f, SPIDER_FRAME);
        const Frame face = MakeFrame({0, top, -1.42f}, {1, 0, 0}, {0, 0.45f, 1.0f});
        for (const float x : {-0.55f, 0.55f})
        {
            Panel(mesh, Offset(face, x, 0, 0.01f), 0.28f, 0.11f, 0.03f, 0.02f, SPIDER_FRAME, SPIDER_GRATE, 0,
                  SPIDER_METAL);
        }
        for (int k = -2; k <= 2; ++k)
        {
            Box(mesh, Offset(face, k * 0.08f, 0, 0.02f), {0.025f, 0.015f, 0.01f}, 0.004f, SPIDER_LAMP);
        }
    }
    // Side consoles beside the seat, turned toward it: grates over red lamp rows.
    for (int side = -1; side <= 1; side += 2)
    {
        const float s = static_cast<float>(side);
        const Frame panel = MakeFrame({s * 1.75f, 1.45f, -0.10f}, {0.35f * s, 0, s}, {0, 1, 0});
        Panel(mesh, panel, 0.38f, 0.30f, 0.05f, 0.03f, SPIDER_FRAME, SPIDER_GRATE, 0.10f, SPIDER_METAL);
        for (int k = -2; k <= 2; ++k)
        {
            Box(mesh, Offset(panel, k * 0.13f, -0.40f, 0.03f), {0.03f, 0.02f, 0.01f}, 0.004f, SPIDER_LAMP);
        }
    }
    // The back: a power-cell drum behind the seat (lamp rings round it), conduits from it up into the dome.
    {
        const Frame base = MakeFrame({0, floorY, SPIDER_EYE.z + 1.35f}, {1, 0, 0}, {0, 1, 0});
        const float drum[6][2] = {{0.0f, 0.50f}, {0.08f, 0.50f}, {0.10f, 0.44f}, {1.70f, 0.44f}, {1.72f, 0.50f},
                                  {1.80f, 0.50f}};
        Lathe(mesh, base, drum, 6, 16, SPIDER_FRAME);
        for (const float y : {0.80f, 1.30f})
        {
            const float ring[4][2] = {{y, 0.45f}, {y, 0.465f}, {y + 0.04f, 0.465f}, {y + 0.04f, 0.45f}};
            Lathe(mesh, base, ring, 4, 16, SPIDER_LAMP);
        }
        for (const float x : {-0.30f, 0.30f})
        {
            const Vec3 pipe[3] = {{x, floorY + 1.80f, SPIDER_EYE.z + 1.35f}, {x, centre.y + 1.90f, SPIDER_EYE.z + 0.95f},
                                  {x, centre.y + 2.10f, centre.z}};
            Tube(mesh, pipe, 3, 0.05f, 10, SPIDER_METAL);
        }
    }
    // The droid seat on the dais; the throttle lever's console under the right hand.
    const SeatLook look = {0.22f, 0.30f, 0.12f, false, true, SPIDER_METAL, SPIDER_FRAME, SPIDER_METAL, SPIDER_METAL,
                           SPIDER_DARK};
    Seat(mesh, SPIDER_EYE, seatFloor, look);
    ThrottleConsole(mesh, SPIDER_EYE, seatFloor, SPIDER_FRAME, SPIDER_DARK, SPIDER_LAMP);
}

// ------------------------------------------------------------------------------------------------------ hailfire droid
// The hailfire droid's body (the vehicle proposal 2026-11-04, idren2023: an enclosed cabin with a viewscreen, the head
// turns and aims, a throttle handle): a pod inside the egg-shaped core between the hoop wheels (cis_tread_hailfire.msh:
// x +-1.5, y 1.2..4.0, z -1.0..1.8 about the hull's (0, 2.6, 0.4); the photoreceptor low at the front, the missile pods
// either side above), its viewscreen ahead where the photoreceptor looks out, glazed red as its eye. The game's eye
// (EyePointOffset 0 3 -5) is behind and above the droid: the world is seen from there, the pod drawn round ours. The
// stock first-person frame (cis_1st_cockpit_hailfiredroid: a flat plate with a cross-shaped port) is not drawn.
// Finishes from that frame's texture (512 px).
enum HailfireFinish
{
    HAIL_PLATE,  // grey plate (the texture's plain left)
    HAIL_SEAM,   // plate with panel seams (lower left)
    HAIL_GRILLE, // the vent grille (top right)
    HAIL_RUNGS,  // the rung strip (top middle)
    HAIL_SENSOR, // the round red sensor (lit)
    HAIL_INSET,  // the machinery inset (upper left)
    HAIL_BAND,   // the pipe band along the foot
    HAIL_FRAME,  // dark frames
    HAIL_DARK,   // recesses
    HAIL_METAL,  // pipes, the seat
    HAIL_LAMP,   // red lamps (lit)
    HAIL_GREEN,  // green lamps (lit)
    HAIL_GLASS,  // the viewscreen, glazed red
    HAIL_PORT,   // the whole first-person texture laid over the pod's front
    HAIL_FINISH_COUNT
};

const Finish HAIL_FINISHES[HAIL_FINISH_COUNT] = {
    {{0.004f, 0.004f, 0.120f, 0.170f}, 1.4f, 1, 0x9A9A98, 0.12f, 12, 0},  // plate (2..61, 2..87)
    {{0.004f, 0.600f, 0.190f, 0.880f}, 1.0f, 1, 0x9A9A98, 0.15f, 12, 0},  // seams (2..97, 307..450)
    {{0.640f, 0.004f, 0.996f, 0.250f}, 0.6f, 1, 0x8A8A88, 0.25f, 18, 0},  // grille (328..510, 2..128)
    {{0.420f, 0.004f, 0.580f, 0.150f}, 0.25f, 1, 0x8A8A88, 0.25f, 18, 0}, // rungs (215..297, 2..77)
    {{0.620f, 0.150f, 0.800f, 0.320f}, 0.20f, 1, 0xB0B0B0, 0.30f, 20, 0.6f}, // sensor (317..410, 77..164)
    {{0.004f, 0.190f, 0.300f, 0.290f}, 0.6f, 1, 0x9A9A98, 0.20f, 14, 0},  // machinery inset (2..154, 97..148)
    {{0, 0, 1, 1}, 1, 0, 0x2A2A2A, 0.30f, 20, 0},                        // the floor's edge band (plain dark)
    {{0.004f, 0.004f, 0.120f, 0.170f}, 0.6f, 1, 0x4C4C4A, 0.20f, 16, 0},  // dark frames
    {{0, 0, 1, 1}, 1, 0, 0x0E0E0E, 0.05f, 6, 0},                         // recesses
    {{0, 0, 1, 1}, 1, 0, 0x707274, 0.45f, 30, 0},                        // metal
    {{0, 0, 1, 1}, 1, 0, 0xFF3020, 0, 1, 0.9f},                          // red lamps
    {{0, 0, 1, 1}, 1, 0, 0x30FF50, 0, 1, 0.9f},                          // green lamps
    {{0, 0, 1, 1}, 1, 0, 0xC03020, 0.6f, 60, 0.05f, 0.12f},              // the viewscreen's red glass
    {{0, 0, 1, 1}, 1, 1, 0x9A9A98, 0.15f, 12, 0, 1.0f, false, true},       // the texture, its own mapping
};

constexpr Vec3 HAIL_EYE = {0.0f, 1.5f, 0.0f};

void BuildHailfire(Mesh& mesh)
{
    // The pod: 0.15 m inside the core (radii 1.35, 1.25, 1.45); the eye 0.35 m above its middle, the floor 0.85 m
    // under it (the eye 1.2 m over the floor). The viewscreen +-55 deg across, -12..+28 deg up from the middle.
    constexpr float DEGREE = 3.14159265f / 180.0f;
    const Vec3 centre = {HAIL_EYE.x, HAIL_EYE.y - 0.35f, HAIL_EYE.z + 0.15f};
    const float floorY = centre.y - 0.95f; // review: the eye 1.3 m over the floor, room for the head
    // The front (+-70 deg, up to 75 deg) is the first-person frame made solid (as the spider's): its cross-shaped
    // port at the eye's height, the bar across +-55 deg.
    const float eyeLatitude = asinf((HAIL_EYE.y - centre.y) / 1.25f);
    const PortWall wall = {centre, {1.35f, 1.25f, 1.45f}, floorY, 70.0f * DEGREE, 75.0f * DEGREE, 0.5f, 0.495f,
                           eyeLatitude, 146.0f * DEGREE, 120.0f * DEGREE, IsHailfirePort, HAIL_PORT, HAIL_GLASS, HAIL_FRAME};
    DroidPod(mesh, centre, {1.35f, 1.25f, 1.45f}, floorY, 70.0f * DEGREE, wall.FloorLatitude(), 75.0f * DEGREE,
             {HAIL_PLATE, HAIL_SEAM, HAIL_FRAME, HAIL_BAND, -1});
    BuildPortWall(mesh, wall);
    // The dash under the viewscreen: a low shelf along the front curve, the sensor dial and lamps on it.
    {
        const float top = floorY + 0.28f;
        Box(mesh, MakeFrame({0, (floorY + top) * 0.5f, centre.z - 1.05f}, {1, 0, 0}, {0, 1, 0}),
            {0.55f, (top - floorY) * 0.5f, 0.18f}, 0.03f, HAIL_FRAME);
        const Frame face = MakeFrame({0, top, centre.z - 0.95f}, {1, 0, 0}, {0, 0.45f, 1.0f});
        Panel(mesh, Offset(face, -0.30f, 0, 0.01f), 0.20f, 0.08f, 0.02f, 0.02f, HAIL_FRAME, HAIL_INSET, 0, HAIL_METAL);
        Quad(mesh, face.At(0.18f, -0.07f, 0.012f), face.At(0.32f, -0.07f, 0.012f), face.At(0.32f, 0.07f, 0.012f),
             face.At(0.18f, 0.07f, 0.012f), HAIL_SENSOR);
        Box(mesh, Offset(face, 0.0f, 0.02f, 0.015f), {0.02f, 0.02f, 0.01f}, 0.004f, HAIL_GREEN);
        Box(mesh, Offset(face, 0.0f, -0.04f, 0.015f), {0.02f, 0.02f, 0.01f}, 0.004f, HAIL_LAMP);
    }
    // The missile pods' feeds either side (the pods are outside above): grilles and rung strips on the side walls,
    // facing the seat, a lamp row under each.
    for (int side = -1; side <= 1; side += 2)
    {
        const float s = static_cast<float>(side);
        const Frame panel = MakeFrame({s * 1.20f, centre.y + 0.30f, centre.z - 0.15f}, {0.45f * s, 0, s}, {0, 1, 0});
        Panel(mesh, panel, 0.30f, 0.22f, 0.04f, 0.02f, HAIL_FRAME, HAIL_GRILLE, 0.06f, HAIL_METAL);
        Panel(mesh, Offset(panel, 0, -0.42f, 0), 0.30f, 0.10f, 0.03f, 0.02f, HAIL_FRAME, HAIL_RUNGS, 0, HAIL_METAL);
        for (int k = -2; k <= 2; ++k)
        {
            Box(mesh, Offset(panel, k * 0.10f, -0.28f, 0.03f), {0.025f, 0.015f, 0.01f}, 0.004f, HAIL_LAMP);
        }
    }
    // Behind the seat: the droid's processor stack, a ribbed column with a lamp ring, cables up into the dome.
    {
        const Frame base = MakeFrame({0, floorY, centre.z + 0.95f}, {1, 0, 0}, {0, 1, 0});
        const float column[6][2] = {{0.0f, 0.30f}, {0.06f, 0.30f}, {0.08f, 0.26f}, {1.30f, 0.26f}, {1.32f, 0.30f},
                                    {1.38f, 0.30f}};
        Lathe(mesh, base, column, 6, 16, HAIL_FRAME);
        const float ring[4][2] = {{0.95f, 0.265f}, {0.95f, 0.28f}, {0.99f, 0.28f}, {0.99f, 0.265f}};
        Lathe(mesh, base, ring, 4, 16, HAIL_LAMP);
        for (const float x : {-0.18f, 0.18f})
        {
            const Vec3 cable[3] = {{x, floorY + 1.38f, centre.z + 0.95f}, {x, centre.y + 1.05f, centre.z + 0.60f},
                                   {x, centre.y + 1.22f, centre.z}};
            Tube(mesh, cable, 3, 0.035f, 8, HAIL_METAL);
        }
    }
    // The droid seat; the throttle lever's console under the right hand.
    const SeatLook look = {0.22f, 0.30f, 0.12f, false, true, HAIL_METAL, HAIL_FRAME, HAIL_METAL, HAIL_METAL, HAIL_DARK};
    Seat(mesh, HAIL_EYE, floorY, look);
    ThrottleConsole(mesh, HAIL_EYE, floorY, HAIL_FRAME, HAIL_DARK, HAIL_LAMP);
}

// ---------------------------------------------------------------------------------------------------------------- AT-AT
// The AT-AT's head (the vehicle proposal 2026-11-04; idren2023's AT-ST verdict: the larger, stock-like cabin with the big
// windows): the game's first-person cockpit (imp_1st_cockpit_atat: the pilot's wide window and its frame, the dash
// stringer, the shared centre console to his right, the left wall and the roof visor) closed into the whole cab as The
// Empire Strikes Back's: two crew side by side at the long viewport (the stock gunner model imp_1st_cockpit_ATATgnr is
// the pilot's mirrored about the centre console, so the gunner sits 1.82 m to the right, his half of the window the
// pilot's mirrored), room behind them for the commander standing, the neck's passage aft. Cockpit model space (x right,
// y up, z back); the pilot's eye 1.7 m nearer the window than the game's (0, 1.5, 1.5); the throttle lever's console
// under his right hand. Finishes from the cockpit's own texture (imp_1st_cockpit_ATAT.tga, 512 px).
enum AtatFinish
{
    ATAT_PLATE,   // grey plating with panel lines (top right)
    ATAT_GRILLE,  // the mesh vent (top left)
    ATAT_SLATS,   // the slatted vent
    ATAT_CONSOLE, // the long instrument strip
    ATAT_RIVETS,  // the riveted rail under it
    ATAT_TARGET,  // the green targeting screen (lit)
    ATAT_RADAR,   // the orange radar (lit)
    ATAT_SWITCH,  // the switch and slider column
    ATAT_SCOPE,   // the green scope screen (lit)
    ATAT_FRAME,   // dark frames (the plating, darker)
    ATAT_DARK,    // recesses, the neck's passage
    ATAT_SEAT,    // seat pads
    ATAT_PIPE,    // pipes, rails
    ATAT_LAMP,    // warm lamps (lit)
    ATAT_SCREEN,  // the gunner's periscope screen (lit)
    ATAT_RED,     // the end-stop lamps (lit)
    ATAT_PICTURE, // the gunner's display: the scope pass's picture (self-lit)
    ATAT_STOCK,   // the game's pilot model and its mirror (StockPieces), its own mapping
    ATAT_STOCK_FX, //   its lamps (effects texture)
    ATAT_GLASS,   //   the long window, one pane (imp_cockpit_win)
    ATAT_SCREEN_FRAME,   // the HUD screens' stock frame (texture 5, imp_1st_cockpit_securitycam: the tanks' look)
    ATAT_SCREEN_GLASS,   //   their glass
    ATAT_SCREEN_BACKING, //   behind the glass
    ATAT_FINISH_COUNT
};

const Finish ATAT_FINISHES[ATAT_FINISH_COUNT] = {
    {{0.420f, 0.010f, 0.980f, 0.240f}, 1.4f, 1, 0x9A9C9E, 0.18f, 14, 0},      // plating (215..502, 5..123)
    {{0.050f, 0.060f, 0.200f, 0.210f}, 0.30f, 1, 0x9A9C9E, 0.20f, 14, 0},     // mesh vent (26..102, 31..107)
    {{0.180f, 0.060f, 0.370f, 0.220f}, 0.36f, 1, 0x9A9C9E, 0.20f, 14, 0},     // slatted vent (92..189, 31..112)
    {{0.050f, 0.270f, 0.970f, 0.580f}, 1.6f, 1, 0xA0A2A4, 0.15f, 12, 0.18f},  // instruments (26..497, 138..297)
    {{0.000f, 0.590f, 1.000f, 0.660f}, 2.0f, 1, 0x9A9C9E, 0.22f, 16, 0},      // riveted rail (0..512, 302..338)
    {{0.020f, 0.710f, 0.210f, 0.980f}, 0.20f, 1, 0xB0B0B0, 0.20f, 20, 0.6f},  // target screen (10..107, 363..502)
    {{0.230f, 0.700f, 0.390f, 0.980f}, 0.17f, 1, 0xB0B0B0, 0.20f, 20, 0.6f},  // radar (118..200, 358..502)
    {{0.400f, 0.700f, 0.620f, 0.990f}, 0.24f, 1, 0xA0A2A4, 0.20f, 16, 0.15f}, // switch column (205..317, 358..507)
    {{0.660f, 0.780f, 0.810f, 0.960f}, 0.16f, 1, 0xB0B0B0, 0.20f, 20, 0.6f},  // scope (338..415, 399..492)
    {{0.420f, 0.010f, 0.980f, 0.240f}, 1.0f, 1, 0x505254, 0.22f, 16, 0},      // dark frames
    {{0, 0, 1, 1}, 1, 0, 0x0E0F10, 0.05f, 6, 0},                             // recesses
    {{0.420f, 0.010f, 0.980f, 0.240f}, 0.5f, 1, 0x4A4846, 0.18f, 10, 0},      // seat pads (dark grey)
    {{0.420f, 0.010f, 0.980f, 0.240f}, 0.4f, 1, 0x6A6C6E, 0.25f, 20, 0},      // pipes: the plating's art, darker
                                                                             // (run624: plain grey read as white)
    {{0, 0, 1, 1}, 1, 0, 0xE8D8B0, 0, 1, 0.4f},                              // warm lamps
    {{0, 0, 1, 1}, 1, 0, 0x1A3A26, 0.6f, 40, 0.55f},                         // periscope screen (green-black, lit)
    {{0, 0, 1, 1}, 1, 0, 0xFF3020, 0, 1, 0.9f},                              // red lamps
    {{0, 0.3f, 1, 0.7f}, 1, 6, 0xFFFFFF, 0, 1, 1.0f},                        // the display's picture
    {{0, 0, 1, 1}, 1, 1, 0x808080, 0.18f, 14, 0},                            // the gunner model
    {{0, 0, 1, 1}, 1, 2, 0xC0C0C0, 0, 1, 1.0f},                              // its lamps
    {{0, 0, 1, 1}, 1, 4, 0xFFFFFF, 0.25f, 40, 0, 0.99f},                     // the window glass (one pane)
    ScreenFrameFinish(),                                                     // stock screen frame
    ScreenGlassFinish(IMP_SCREEN_ART),                                       // its glass
    ScreenBackingFinish(IMP_SCREEN_ART),                                     // behind the glass
};

constexpr Vec3 ATAT_EYE = {-0.10f, 1.45f, -0.20f};
constexpr float ATAT_CONSOLE_X = 0.91f;                       // the shared centre console's middle (x 0.59..1.23)
constexpr float ATAT_GUNNER_X = 2.0f * ATAT_CONSOLE_X - ATAT_EYE.x; // the gunner's eye, mirrored about it

// The AT-AT gunner's swivel station (idren2023 2026-11-04: a viewscreen swivel seat, so he reaches his whole aiming area,
// the turret's yaw -65..65; laid out as the Y-wing gunner's, SPEEDER_CABIN_PARTS): part 1 the turning chair on its
// turntable with the column, the twin grips and the display's rails, 2 the display housing (pitched with the gun), 3
// and 4 its end-stop lamps, 5 the turret camera's picture. Part 0 (the cab) is BuildAtat's.
void AtatGunnerStation(Mesh& mesh)
{
    // Headset run 19 (idren2023): a plain co-pilot chair, no swivel or display: the big window shows the gun's whole aim
    // range, the head aims (GunnerSeat Cabin isPlainSeat) and the aiming line shows where the shots go. The chair as
    // the pilot's; in front of it a column with the twin grips (GunnerSeat's grip place, mirrored about the eye).
    const Vec3 eye = {ATAT_GUNNER_X, ATAT_EYE.y, ATAT_EYE.z};
    constexpr float FLOOR = -0.07f;
    mesh.part = 0;
    const SeatLook seat = {0.22f, 0.58f, 0.12f, true, false, ATAT_SEAT, ATAT_FRAME, ATAT_PIPE, ATAT_SEAT, ATAT_DARK};
    Seat(mesh, eye, FLOOR, seat);
    // the grips low, under the dash's lip (review: the column hid the co-pilot's scope), 30 cm out from the chest
    const float gripZ = eye.z - 0.30f, gripY = eye.y - 0.55f;
    const Vec3 column[2] = {{eye.x, FLOOR, gripZ - 0.10f}, {eye.x, gripY - 0.12f, gripZ - 0.10f}};
    Tube(mesh, column, 2, 0.04f, 12, ATAT_PIPE);
    Box(mesh, MakeFrame({eye.x, FLOOR + 0.03f, gripZ - 0.10f}, {1, 0, 0}, {0, 1, 0}), {0.12f, 0.03f, 0.12f}, 0.01f,
        ATAT_FRAME);
    const Frame head = MakeFrame({eye.x, gripY - 0.10f, gripZ - 0.04f}, {1, 0, 0}, {0, 1, 0});
    Box(mesh, head, {0.11f, 0.045f, 0.06f}, 0.012f, ATAT_FRAME);
    Quad(mesh, head.At(-0.08f, 0.046f, -0.04f), head.At(0.08f, 0.046f, -0.04f), head.At(0.08f, 0.046f, 0.04f),
         head.At(-0.08f, 0.046f, 0.04f), ATAT_SWITCH);
    for (int side = -1; side <= 1; side += 2)
    {
        const float x = eye.x + side * 0.20f;
        const Vec3 arm[2] = {head.At(side * 0.11f, 0, 0), {x, gripY - 0.10f, gripZ}};
        Tube(mesh, arm, 2, 0.016f, 8, ATAT_PIPE);
        const Vec3 grip[2] = {{x, gripY - 0.10f, gripZ}, {x, gripY + 0.07f, gripZ - 0.02f}};
        Tube(mesh, grip, 2, 0.019f, 10, ATAT_SEAT);
        Box(mesh, MakeFrame({x, gripY + 0.075f, gripZ - 0.035f}, {1, 0, 0}, {0, 1, 0}), {0.008f, 0.006f, 0.008f}, 0.002f,
            ATAT_RED); // the trigger button under the thumb
    }
}

void BuildAtat(Mesh& mesh)
{
    // The cab: the stock left wall (x -1.81) mirrored for the right (3.63); the floor at the stock shell's foot; the
    // roof at its top; the front at the window frame's plane; the back 1.9 m behind the crew (the stock shell ends at
    // z 0.91).
    constexpr float XL = -1.81f, XR = 2.0f * ATAT_CONSOLE_X + 1.81f, FLOOR = -0.07f, ROOF = 3.25f;
    constexpr float FRONT = -2.65f, BACK = 1.70f; // the front at the stock nose's tip (2026-11-11: at the window's
                                                   // plane the walls left a gap beside it)
    // Floor: plating, a ribbed tread in each seat's well.
    TiledQuad(mesh, {XL, FLOOR, FRONT}, {XR - XL, 0, 0}, {0, 0, BACK - FRONT}, ATAT_PLATE);
    for (const float x : {ATAT_EYE.x, ATAT_GUNNER_X})
    {
        Box(mesh, MakeFrame({x, FLOOR + 0.008f, ATAT_EYE.z - 0.30f}, {1, 0, 0}, {0, 1, 0}), {0.30f, 0.008f, 0.50f},
            0.004f, ATAT_RIVETS);
    }
    // The cab's front, both seats: the game's pilot model cut at the centre console and mirrored for the gunner, one
    // mesh drawn by us with one light (tools/stockcockpit.py ATAT; idren2023 2026-11-11: the two halves one complete mesh,
    // the long window one glass panel; the game's own model is not drawn: isWholeCockpit).
    StockPieces(mesh, StockVertices("STOCK_ATAT"), StockCount("STOCK_ATAT"), ATAT_STOCK, ATAT_STOCK_FX, ATAT_GLASS);
    // Side walls: the right one whole; the left just outside the stock shell's slanted wall (closing its gaps).
    TiledQuad(mesh, {XR, FLOOR, FRONT}, {0, 0, BACK - FRONT}, {0, ROOF - FLOOR, 0}, ATAT_PLATE);
    TiledQuad(mesh, {XL - 0.04f, FLOOR, FRONT}, {0, 0, BACK - FRONT}, {0, ROOF - FLOOR, 0}, ATAT_PLATE);
    for (int side = -1; side <= 1; side += 2)
    {
        const float x = side > 0 ? XR - 0.01f : XL + 0.01f, facing = side > 0 ? -1.0f : 1.0f;
        Panel(mesh, MakeFrame({x, 1.55f, 1.20f}, {0, 0, facing}, {0, 1, 0}), 0.30f, 0.45f, 0.05f, 0.03f, ATAT_FRAME,
              ATAT_SLATS, 0, ATAT_PIPE);
    }
    Panel(mesh, MakeFrame({XR - 0.01f, 1.55f, 0.15f}, {0, 0, -1}, {0, 1, 0}), 0.55f, 0.40f, 0.05f, 0.03f, ATAT_FRAME,
          ATAT_CONSOLE, 0.10f, ATAT_PIPE);
    // The right wall broken up as the stock left one is (review of run624: a bare slab): a side console at the
    // gunner's right knee with the switch column on its sloped top and the radar over it, a lamp strip along the wall.
    {
        const float x = XR - 0.25f, z = 0.10f;
        Box(mesh, MakeFrame({x, (FLOOR + 0.95f) * 0.5f, z}, {1, 0, 0}, {0, 1, 0}), {0.24f, (0.95f - FLOOR) * 0.5f, 0.45f},
            0.03f, ATAT_FRAME);
        const Frame top = MakeFrame({x - 0.02f, 0.97f, z}, {0, 0, 1}, {-0.5f, 1, 0});
        Quad(mesh, top.At(-0.40f, -0.16f, 0.005f), top.At(0.40f, -0.16f, 0.005f), top.At(0.40f, 0.16f, 0.005f),
             top.At(-0.40f, 0.16f, 0.005f), ATAT_SWITCH);
        Panel(mesh, MakeFrame({XR - 0.01f, 1.95f, -0.55f}, {0, 0, -1}, {0, 1, 0}), 0.20f, 0.20f, 0.03f, 0.02f, ATAT_FRAME,
              ATAT_RADAR, 0, ATAT_PIPE);
        Quad(mesh, {XR - 0.02f, 2.55f, -1.2f}, {XR - 0.02f, 2.55f, 1.4f}, {XR - 0.02f, 2.60f, 1.4f},
             {XR - 0.02f, 2.60f, -1.2f}, ATAT_LAMP);
    }
    // Roof: plating, a lamp strip over each seat, ribs across.
    TiledQuad(mesh, {XL, ROOF, FRONT}, {XR - XL, 0, 0}, {0, 0, BACK - FRONT}, ATAT_PLATE);
    for (float z = -0.6f; z < BACK; z += 0.9f)
    {
        Box(mesh, MakeFrame({(XL + XR) * 0.5f, ROOF - 0.06f, z}, {1, 0, 0}, {0, 1, 0}), {(XR - XL) * 0.5f, 0.06f, 0.06f},
            0.02f, ATAT_FRAME);
    }
    for (const float x : {ATAT_EYE.x, ATAT_GUNNER_X})
    {
        Quad(mesh, {x - 0.25f, ROOF - 0.13f, 0.30f}, {x + 0.25f, ROOF - 0.13f, 0.30f}, {x + 0.25f, ROOF - 0.13f, 0.40f},
             {x - 0.25f, ROOF - 0.13f, 0.40f}, ATAT_LAMP);
    }
    // Back: the neck's passage aft in the middle (a dark doorway in a heavy frame), consoles either side of it.
    {
        const float half = 0.55f, top = 2.20f, mid = ATAT_CONSOLE_X;
        TiledQuad(mesh, {XL, FLOOR, BACK}, {mid - half - XL, 0, 0}, {0, ROOF - FLOOR, 0}, ATAT_PLATE);
        TiledQuad(mesh, {mid + half, FLOOR, BACK}, {XR - mid - half, 0, 0}, {0, ROOF - FLOOR, 0}, ATAT_PLATE);
        TiledQuad(mesh, {mid - half, top, BACK}, {2 * half, 0, 0}, {0, ROOF - top, 0}, ATAT_PLATE);
        Quad(mesh, {mid - half, FLOOR, BACK + 0.6f}, {mid + half, FLOOR, BACK + 0.6f}, {mid + half, top, BACK + 0.6f},
             {mid - half, top, BACK + 0.6f}, ATAT_DARK);
        TiledQuad(mesh, {mid - half, FLOOR, BACK}, {2 * half, 0, 0}, {0, 0, 0.6f}, ATAT_FRAME); // the passage's floor
        for (const float x : {mid - half, mid + half})
        {
            TiledQuad(mesh, {x, FLOOR, BACK}, {0, 0, 0.6f}, {0, top - FLOOR, 0}, ATAT_FRAME);
            Box(mesh, MakeFrame({x, (FLOOR + top) * 0.5f, BACK - 0.05f}, {1, 0, 0}, {0, 1, 0}),
                {0.08f, (top - FLOOR) * 0.5f, 0.06f}, 0.03f, ATAT_FRAME);
        }
        Box(mesh, MakeFrame({mid, top + 0.05f, BACK - 0.05f}, {1, 0, 0}, {0, 1, 0}), {half + 0.08f, 0.08f, 0.06f}, 0.03f,
            ATAT_FRAME);
        TiledQuad(mesh, {mid - half, top, BACK}, {2 * half, 0, 0}, {0, 0, 0.6f}, ATAT_FRAME);
        for (int side = -1; side <= 1; side += 2)
        {
            const float x = mid + static_cast<float>(side) * (half + 0.75f);
            Box(mesh, MakeFrame({x, 0.55f, BACK - 0.25f}, {1, 0, 0}, {0, 1, 0}), {0.55f, 0.60f, 0.25f}, 0.03f,
                ATAT_FRAME);
            const Frame face = MakeFrame({x, 1.20f, BACK - 0.45f}, {-1, 0, 0}, {0, 0.6f, -1.0f});
            Quad(mesh, face.At(-0.45f, -0.14f, 0.01f), face.At(0.45f, -0.14f, 0.01f), face.At(0.45f, 0.14f, 0.01f),
                 face.At(-0.45f, 0.14f, 0.01f), side < 0 ? ATAT_SWITCH : ATAT_CONSOLE);
            Panel(mesh, MakeFrame({x, 1.75f, BACK - 0.01f}, {-1, 0, 0}, {0, 1, 0}), 0.20f, 0.20f, 0.03f, 0.02f,
                  ATAT_FRAME, side < 0 ? ATAT_RADAR : ATAT_TARGET, 0, ATAT_PIPE);
        }
    }
    // Seats side by side (Imperial, winged).
    const SeatLook look = {0.22f, 0.58f, 0.12f, true, false, ATAT_SEAT, ATAT_FRAME, ATAT_PIPE, ATAT_SEAT, ATAT_DARK};
    Seat(mesh, ATAT_EYE, FLOOR, look);
    // The pilot's throttle console under his right hand.
    {
        const Vec3 lever = {ATAT_EYE.x + 0.25f, ATAT_EYE.y - 0.62f, ATAT_EYE.z - 0.30f};
        Box(mesh, MakeFrame({lever.x + 0.02f, (FLOOR + lever.y) * 0.5f, lever.z + 0.05f}, {1, 0, 0}, {0, 1, 0}),
            {0.11f, (lever.y - FLOOR) * 0.5f, 0.26f}, 0.02f, ATAT_FRAME);
        Box(mesh, MakeFrame({lever.x, lever.y + 0.004f, lever.z}, {1, 0, 0}, {0, 1, 0}), {0.018f, 0.004f, 0.12f},
            0.002f, ATAT_DARK);
        for (const float dz : {-0.14f, 0.0f, 0.14f})
        {
            Box(mesh, MakeFrame({lever.x + 0.07f, lever.y + 0.006f, lever.z + dz}, {1, 0, 0}, {0, 1, 0}),
                {0.012f, 0.004f, 0.012f}, 0.002f, ATAT_LAMP);
        }
    }
    CabinCornerTrims(mesh, XL - 0.04f, XR, FLOOR, ROOF, FRONT, BACK, 0.04f, ATAT_FRAME);
    // The HUD on the stock dash's own screens (idren2023 2026-11-11: no added screens; the map in the centre console's
    // screen). Each display 3 mm off its screen's face toward the eye (no depth fight): the map on the centre
    // console's green screen (0.36 x 0.30 m, shared by both seats); the pilot's two orange screens (panel9) health
    // above ammunition; the score on the readout strip at his top left (panel7); the target on the lamp panel under
    // the heartbeat readout (panel2).
    {
        const Vec3 slope = Normalized(Vec3{0, 0.295f, -0.127f}); // the dash's face rising away from the eye
        const Frame dash9 = MakeFrame({-0.391f, 0.6395f, -1.1005f}, {1, 0, 0}, slope);
        const Frame dash7 = MakeFrame({-0.946f, 0.7245f, -1.1535f}, {1, 0, 0}, slope);
        const Frame dash2 = MakeFrame({0.0555f, 0.5415f, -1.077f}, {1, 0, 0}, slope);
        const Frame console = MakeFrame({0.9025f, 0.937f, -0.8865f}, {1, 0, 0}, Normalized(Vec3{0, 0.12f, -0.271f}));
        AddDisplay(GROUP_MAP, Offset(console, 0, 0, 0.003f), 0, 0, 0.27f, 0.27f);
        AddDisplay(GROUP_HEALTH, Offset(dash9, 0, 0, 0.003f), 0, 0.085f, 0.44f, 0.13f);
        AddDisplay(GROUP_AMMUNITION, Offset(dash9, 0, 0, 0.003f), 0, -0.09f, 0.44f, 0.13f);
        AddDisplay(GROUP_SCORE, Offset(dash7, 0, 0, 0.003f), 0, 0, 0.48f, 0.12f);
        AddDisplay(GROUP_TARGET, Offset(dash2, 0, 0, 0.003f), 0, 0, 0.29f, 0.18f);
    }
    // Behind the centre console, between the two dashes (run 799: a slit of snow where the dash met the console).
    TiledQuad(mesh, {ATAT_CONSOLE_X - 0.40f, 0.05f, -1.30f}, {0.80f, 0, 0}, {0, 0.90f, 0}, ATAT_FRAME);
    AtatGunnerStation(mesh); // his co-pilot chair and grips
}

// ----------------------------------------------------------------------------------------------------------------
// AT-TE The AT-TE's cab (the vehicle proposal 2026-11-04: the pilot and two gunner stations, idren2023: gunners as crew
// chairs in the pilot's cabin): the game's first-person cockpit (rep_1st_cockpit_ATTE: the narrow pod with its
// windshield and angled side windows, its panels either side) opened at its back into a crew compartment: the rear
// gunner (TURRET2, the turret aft: YawLimits 120..240) facing the back wall's window (design rep_walk_atte_rear; the
// pilot has his cab in the nose since b534, the main cannon's gunner his open-air station since b530). Cockpit model
// space (x right, y up, z back); the pilot 1 m nearer the windshield than the game's eye (0, 1.5, 1.5); the throttle
// lever's console under his right hand. Finishes from the cockpit's own texture (rep_1st_cockpit_ATTE.tga, 512 px).
enum AtteFinish
{
    ATTE_PLATE,   // dark grey plate (the panel's plain middle)
    ATTE_RIVETS,  // the riveted edge rail (turned: it runs along the faces)
    ATTE_LAMPS,   // the red / green lamp column
    ATTE_CONSOLE, // the slanted instrument panel
    ATTE_SLOT,    // the slotted band along the panel's foot
    ATTE_FRAME,   // dark frames
    ATTE_DARK,    // recesses
    ATTE_SEAT,    // seat pads
    ATTE_PIPE,    // pipes, rails
    ATTE_LAMP,    // warm lamps (lit)
    ATTE_SCREEN,  // the gunners' targeting screens (lit)
    ATTE_STOCK,   // the walker's own exterior (texture 3: the open-air cannon's, the cab's)
    ATTE_GLASS,   // the cab's amber window (see-through)
    ATTE_SCREEN_FRAME,   // the stock Republic gunner screen's frame (texture 5; the cannon's HUD screens)
    ATTE_SCREEN_GLASS,   //   its glass
    ATTE_SCREEN_BACKING, //   behind the glass
    ATTE_PANEL,          // a whole stock panel (the texture entire: rivets, lamp column, instrument plate)
    ATTE_ACCENT,         // blue-lit strips (lit)
    ATTE_RED,            // red lamps and placards (lit)
    ATTE_PICTURE,        // a viewscreen: the scope pass's picture (the cannon's gun camera)
    ATTE_ARMOR,          // the walker's plain weathered plating (exterior texture: the cannon station's armour)
    ATTE_MAROON,         // the Republic's maroon band
    ATTE_READOUT,        // a lit readout behind a screen's glass (the instruments' picture, green)
    ATTE_WINDOW,         // the pilot cab's glass: the stock windshield's texture (rep_cockpit_win), its grime and alpha
    ATTE_WHITE,          // white strip lights and wall lamps (lit; the walkthrough video's)
    ATTE_HAZARD,         // the yellow of the troop bay floor's chevrons
    ATTE_FINISH_COUNT
};

const Finish ATTE_FINISHES[ATTE_FINISH_COUNT] = {
    {{0.120f, 0.500f, 0.900f, 0.800f}, 1.6f, 1, 0x9A9C9E, 0.15f, 12, 0},          // plate (61..461, 256..410)
    {{0.000f, 0.050f, 0.060f, 0.950f}, 1.6f, 1, 0x9A9C9E, 0.25f, 18, 0, 1, true}, // rivet rail (0..31, 26..486)
    {{0.080f, 0.130f, 0.160f, 0.500f}, 0.20f, 1, 0xA0A0A0, 0.20f, 16, 0.45f},     // lamp column (41..82, 67..256)
    {{0.180f, 0.120f, 0.950f, 0.460f}, 0.60f, 1, 0xA0A2A4, 0.15f, 12, 0.15f},     // instruments (92..486, 61..236)
    {{0.050f, 0.820f, 0.950f, 0.920f}, 1.4f, 1, 0x9A9C9E, 0.20f, 14, 0},          // slot band (26..486, 420..471)
    {{0.120f, 0.500f, 0.900f, 0.800f}, 1.0f, 1, 0x505254, 0.22f, 16, 0},          // dark frames
    {{0, 0, 1, 1}, 1, 0, 0x0E0F10, 0.05f, 6, 0},                                  // recesses
    {{0.120f, 0.500f, 0.900f, 0.800f}, 0.5f, 1, 0x58524A, 0.18f, 10, 0},          // seat pads (worn brown)
    {{0, 0, 1, 1}, 1, 0, 0x55585C, 0.25f, 20, 0},                            // pipes (darker: read as white, review)
    {{0, 0, 1, 1}, 1, 0, 0xE8D8B0, 0, 1, 0.4f},                              // warm lamps
    {{0, 0, 1, 1}, 1, 0, 0x1A3A4A, 0.6f, 40, 0.55f},                         // screens (blue-black, lit)
    {{0, 0, 1, 1}, 1, 3, 0x808080, 0.20f, 14, 0},                            // the exterior
    {{0, 0, 1, 1}, 1, 0, 0xD8A040, 0.6f, 60, 0.05f, 0.22f},                  // amber glass
    ScreenFrameFinish(),                                                     // stock screen frame
    ScreenGlassFinish(REP_SCREEN_ART),                                       // its glass
    ScreenBackingFinish(REP_SCREEN_ART),                                     // behind the glass
    {{0, 0, 1, 1}, 1.0f, 1, 0xB4B6B8, 0.15f, 12, 0.10f, 1, false, true},     // a whole stock panel
    {{0, 0, 1, 1}, 1, 0, 0x3C8CFF, 0, 1, 0.9f},                              // blue-lit strips
    {{0, 0, 1, 1}, 1, 0, 0xFF3020, 0, 1, 0.9f},                              // red lamps
    {{0, 0.3f, 1, 0.7f}, 1, 6, 0xFFFFFF, 0, 1, 1.0f},                        // the gun camera's picture
    {{0.860f, 0.030f, 0.950f, 0.120f}, 0.8f, 3, 0xA0A0A0, 0.20f, 14, 0.04f}, // armour (440..486, 15..61)
    {{0, 0, 1, 1}, 1, 0, 0x6A1E24, 0.15f, 10, 0},                            // maroon band
    {{0.180f, 0.120f, 0.950f, 0.460f}, 1, 1, 0x70E890, 0, 1, 0.75f},         // lit readout (instruments, green)
    {{0, 0, 1, 1}, 1, 4, 0x98B0C4, 0.60f, 40, 0, 0.99f},                     // pilot glass: the window texture
    {{0, 0, 1, 1}, 1, 0, 0xEEF2FF, 0, 1, 0.95f},                              // white lamps
    {{0, 0, 1, 1}, 1, 0, 0xB8901E, 0.15f, 10, 0},                             // hazard yellow
};

constexpr Vec3 ATTE_EYE = {0.0f, 1.45f, 0.50f};
constexpr Vec3 ATTE_REAR_EYE = {0.85f, 1.45f, 3.55f};    // the rear gunner, facing aft (+z)

// A gunner's twin grips on a post, his hands' place (x +-0.20 about `eye`): `ahead` is the direction he faces.
void AtteGrips(Mesh& mesh, Vec3 eye, float ahead, float floorY)
{
    for (const float dx : {-0.20f, 0.20f})
    {
        const Vec3 base = {eye.x + dx, eye.y - 0.68f, eye.z + ahead * 0.30f};
        Box(mesh, MakeFrame({base.x, (floorY + base.y) * 0.5f, base.z}, {1, 0, 0}, {0, 1, 0}),
            {0.025f, (base.y - floorY) * 0.5f, 0.025f}, 0.01f, ATTE_PIPE);
        const Vec3 grip[2] = {base, base + Vec3{0, 0.18f, -ahead * 0.05f}};
        Tube(mesh, grip, 2, 0.022f, 10, ATTE_SEAT);
    }
}

void BuildAtte(Mesh& mesh)
{
    // The stock pod: x +-0.73, floor -0.17, top 2.39, z -1.04..1.76 (open at the back). The compartment behind it.
    constexpr float FLOOR = -0.17f, POD_TOP = 2.39f, POD_HALF = 0.73f, POD_BACK = 1.76f;
    constexpr float XL = -1.70f, XR = 1.70f, ROOF = 3.45f, BACK = 4.20f;
    // Bulkhead at the pod's back: beside the pod down to the floor, whole above the pod's top (2026-11-07: the cannon's
    // gunner has his own open-air station since b530; his slit here was left open, the "open side" of the review).
    TiledQuad(mesh, {XL, FLOOR, POD_BACK}, {XL * -1.0f - POD_HALF, 0, 0}, {0, POD_TOP - FLOOR, 0}, ATTE_PLATE);
    TiledQuad(mesh, {POD_HALF, FLOOR, POD_BACK}, {XR - POD_HALF, 0, 0}, {0, POD_TOP - FLOOR, 0}, ATTE_PLATE);
    TiledQuad(mesh, {XL, POD_TOP, POD_BACK}, {XR - XL, 0, 0}, {0, ROOF - POD_TOP, 0}, ATTE_PLATE);
    Box(mesh,
        MakeFrame({0, POD_TOP + 0.04f, POD_BACK + 0.04f}, {1, 0, 0}, {0, 1, 0}),
        {POD_HALF + 0.08f, 0.05f, 0.04f},
        0.015f,
        ATTE_FRAME);
    for (const float x : {-POD_HALF - 0.04f, POD_HALF + 0.04f})
    {
        Box(mesh,
            MakeFrame({x, (FLOOR + POD_TOP) * 0.5f, POD_BACK + 0.04f}, {1, 0, 0}, {0, 1, 0}),
            {0.05f, (POD_TOP - FLOOR) * 0.5f, 0.04f},
            0.015f,
            ATTE_FRAME);
    }
    // The compartment: floor, side walls, roof, back wall with the rear gunner's window (x 0.30..1.45, y 1.05..1.95).
    TiledQuad(mesh, {XL, FLOOR, POD_BACK}, {XR - XL, 0, 0}, {0, 0, BACK - POD_BACK}, ATTE_PLATE);
    for (const float x : {XL, XR})
    {
        TiledQuad(mesh, {x, FLOOR, POD_BACK}, {0, 0, BACK - POD_BACK}, {0, ROOF - FLOOR, 0}, ATTE_PLATE);
        Box(mesh, MakeFrame({x * 0.99f, 1.10f, (POD_BACK + BACK) * 0.5f}, {0, 0, 1}, {0, 1, 0}),
            {(BACK - POD_BACK) * 0.5f, 0.05f, 0.03f}, 0.01f, ATTE_SLOT);
    }
    TiledQuad(mesh, {XL, ROOF, POD_BACK}, {XR - XL, 0, 0}, {0, 0, BACK - POD_BACK}, ATTE_PLATE);
    {
        constexpr float WX0 = 0.30f, WX1 = 1.45f, WY0 = 1.05f, WY1 = 1.95f;
        TiledQuad(mesh, {XL, FLOOR, BACK}, {WX0 - XL, 0, 0}, {0, ROOF - FLOOR, 0}, ATTE_PLATE);
        TiledQuad(mesh, {WX1, FLOOR, BACK}, {XR - WX1, 0, 0}, {0, ROOF - FLOOR, 0}, ATTE_PLATE);
        TiledQuad(mesh, {WX0, FLOOR, BACK}, {WX1 - WX0, 0, 0}, {0, WY0 - FLOOR, 0}, ATTE_PLATE);
        TiledQuad(mesh, {WX0, WY1, BACK}, {WX1 - WX0, 0, 0}, {0, ROOF - WY1, 0}, ATTE_PLATE);
        for (const float y : {WY0 - 0.05f, WY1 + 0.05f})
        {
            Box(mesh, MakeFrame({(WX0 + WX1) * 0.5f, y, BACK - 0.06f}, {1, 0, 0}, {0, 1, 0}),
                {(WX1 - WX0) * 0.5f + 0.08f, 0.05f, 0.08f}, 0.02f, ATTE_FRAME);
        }
        for (const float x : {WX0 - 0.05f, WX1 + 0.05f})
        {
            Box(mesh, MakeFrame({x, (WY0 + WY1) * 0.5f, BACK - 0.06f}, {1, 0, 0}, {0, 1, 0}),
                {0.05f, (WY1 - WY0) * 0.5f + 0.08f, 0.08f}, 0.02f, ATTE_FRAME);
        }
    }
    // Ribs across the compartment, a lamp on the roof.
    for (float z = POD_BACK + 0.6f; z < BACK; z += 0.8f)
    {
        Box(mesh, MakeFrame({0, ROOF - 0.06f, z}, {1, 0, 0}, {0, 1, 0}), {XR, 0.06f, 0.05f}, 0.02f, ATTE_FRAME);
        for (const float x : {XL + 0.05f, XR - 0.05f})
        {
            Box(mesh, MakeFrame({x, (FLOOR + ROOF) * 0.5f, z}, {0, 0, 1}, {0, 1, 0}), {0.05f, (ROOF - FLOOR) * 0.5f, 0.05f},
                0.02f, ATTE_FRAME);
        }
    }
    Quad(mesh, {-0.3f, ROOF - 0.13f, 2.8f}, {0.3f, ROOF - 0.13f, 2.8f}, {0.3f, ROOF - 0.13f, 2.9f},
         {-0.3f, ROOF - 0.13f, 2.9f}, ATTE_LAMP);
    // The pilot: his seat in the pod, the throttle console under his right hand.
    const SeatLook look = {0.22f, 0.55f, 0.12f, false, false, ATTE_SEAT, ATTE_FRAME, ATTE_PIPE, ATTE_SEAT, ATTE_DARK};
    Seat(mesh, ATTE_EYE, FLOOR, look);
    ThrottleConsole(mesh, ATTE_EYE, FLOOR, ATTE_FRAME, ATTE_DARK, ATTE_LAMP);
    // Where the cannon gunner's platform stood: the walls lined with the stock panels (as the pilot's cab).
    for (int side = -1; side <= 1; side += 2)
    {
        const float inner = side * (XR - 0.006f);
        for (const float z : {POD_BACK + 0.10f, POD_BACK + 1.15f})
        {
            const float zLeft = side < 0 ? z + 0.95f : z, zRight = side < 0 ? z : z + 0.95f;
            Quad(
                mesh,
                {inner, 2.45f, zLeft},
                {inner, 2.45f, zRight},
                {inner, 1.50f, zRight},
                {inner, 1.50f, zLeft},
                ATTE_PANEL);
        }
    }
    // The rear gunner: a stool facing aft, his screen under the window, his grips.
    {
        const float stool[4][2] = {{0.0f, 0.22f}, {0.70f, 0.22f}, {0.75f, 0.20f}, {0.75f, 0.0f}};
        Lathe(mesh, MakeFrame({ATTE_REAR_EYE.x, FLOOR, ATTE_REAR_EYE.z - 0.10f}, {1, 0, 0}, {0, 1, 0}), stool, 4, 16,
              ATTE_SEAT);
        const Frame screen = MakeFrame({ATTE_REAR_EYE.x, ATTE_REAR_EYE.y - 0.45f, ATTE_REAR_EYE.z + 0.45f}, {-1, 0, 0},
                                       {0, 0.88f, -0.47f});
        Panel(mesh, screen, 0.18f, 0.12f, 0.03f, 0.04f, ATTE_FRAME, ATTE_SCREEN, 0, ATTE_PIPE);
        AtteGrips(mesh, ATTE_REAR_EYE, 1.0f, FLOOR);
    }
}

// ------------------------------------------------------------------------------------------------------- snowspeeder
// The T-47 snowspeeder (the vehicle proposal 2026-11-04; docs/research/vehicle-interiors-research.md): the ship's own
// exterior and canopy round its cockpit (tools/stockexterior.py), the pilot ahead in a narrow tub with the dash under
// the canopy's front (the stock first-person texture's instrument strip), the flight stick and the throttle where
// Flight puts them; the tail gunner (FLYERSECTION TURRET, YawLimits 80..280: he faces aft) back to back with him on a
// swivel station laid out as the Y-wing gunner's (SPEEDER_CABIN_PARTS: part 1 the chair, column, grips and rails, 2
// the display, 3 and 4 its end-stop lamps, 5 the turret camera's picture; GunnerSeat's cabin turns them aft). Cockpit
// model space (x right, y up, z back), the pilot's eye at the game's (0, 1.5, 1.5). Finishes from the cockpit's own
// texture (all_1st_cockpit_snowspeeder.tga, 512 px) and the ship's.
enum SnowFinish
{
    SNOW_PLATE,   // plain brushed metal (the band under the panels)
    SNOW_DASH,    // the top instrument strip (radar, readouts)
    SNOW_SLIDERS, // the slider panels
    SNOW_KNOBS,   // the knob and switch row
    SNOW_DIAL,    // the round dial
    SNOW_BOX,     // the gun box
    SNOW_TALL,    // the tall side panel
    SNOW_RAIL,    // the cable rod along the foot
    SNOW_FRAME,   // dark frames
    SNOW_DARK,    // recesses
    SNOW_PAD,     // seat pads (cream, as the film's)
    SNOW_METAL,   // pipes, rails
    SNOW_LAMP,    // red lamps (lit)
    SNOW_STOCK,   // the ship's own exterior (texture 3)
    SNOW_GLASS,   // its canopy glass (texture 4)
    SNOW_PICTURE, // the gunner's display: the scope pass's picture
    SNOW_COCKPIT, // the game's first-person dash (StockPieces), its own mapping
    SNOW_FX,      //   its lamps (effects texture)
    SNOW_SCREEN,  // the HUD screens' dark glass
    SNOW_LABEL,   // the light grey label strip along the dash's top (the film's)
    SNOW_AMBER,   // amber lamps (lit)
    SNOW_CHARCOAL, // the dash's and frames' charcoal (the film's)
    SNOW_SCREEN_FRAME,   // the HUD screens' stock frame (texture 5, all_1st_cockpit_securitycam: the tanks' look)
    SNOW_SCREEN_GLASS,   //   their glass (scan lines)
    SNOW_SCREEN_BACKING, //   behind the glass
    SNOW_FINISH_COUNT
};

const Finish SNOW_FINISHES[SNOW_FINISH_COUNT] = {
    {{0.000f, 0.805f, 1.000f, 0.900f}, 1.6f, 1, 0xA8AAAC, 0.25f, 18, 0},     // plate (0..512, 412..460: the band and its seam)
    {{0.000f, 0.000f, 1.000f, 0.340f}, 1.0f, 1, 0xB0B2B4, 0.20f, 16, 0.15f}, // dash strip (0..512, 0..174)
    {{0.020f, 0.460f, 0.340f, 0.640f}, 0.36f, 1, 0xB0B2B4, 0.20f, 16, 0.1f}, // sliders (10..174, 236..328)
    {{0.020f, 0.660f, 0.260f, 0.800f}, 0.26f, 1, 0xB0B2B4, 0.20f, 16, 0.1f}, // knobs (10..133, 338..410)
    {{0.270f, 0.660f, 0.410f, 0.800f}, 0.14f, 1, 0xB0B2B4, 0.20f, 16, 0},    // dial (138..210, 338..410)
    {{0.430f, 0.680f, 0.730f, 0.790f}, 0.30f, 1, 0xB0B2B4, 0.20f, 16, 0},    // gun box (220..374, 348..404)
    {{0.830f, 0.420f, 0.980f, 0.800f}, 0.16f, 1, 0xB0B2B4, 0.20f, 16, 0.1f}, // tall panel (425..502, 215..410)
    {{0.000f, 0.930f, 1.000f, 0.980f}, 1.4f, 1, 0xA0A2A4, 0.30f, 20, 0},     // cable rod (0..512, 476..502)
    {{0.000f, 0.805f, 1.000f, 0.900f}, 1.6f, 1, 0x505456, 0.22f, 16, 0},     // dark frames
    {{0, 0, 1, 1}, 1, 0, 0x0E0F10, 0.05f, 6, 0},                            // recesses
    {{0.060f, 0.470f, 0.290f, 0.700f}, 0.40f, 3, 0x807A69, 0.10f, 8, 0},    // cream pads: the hull's weathered panel
                                                                            // (all_fly_speeder 30..150, 240..360), tinted
    {{0, 0, 1, 1}, 1, 0, 0x55585C, 0.25f, 20, 0},                           // metal (darker: plain grey read as white, review)
    {{0, 0, 1, 1}, 1, 0, 0xFF3020, 0, 1, 0.9f},                             // red lamps
    {{0, 0, 1, 1}, 1, 3, 0x808080, 0.20f, 14, 0},                           // the exterior, its own mapping
    {{0, 0, 1, 1}, 1, 4, 0xFFFFFF, 0.6f, 60, 0, 0.35f},                     // canopy glass (idren2023: the windows back)
    {{0, 0.3f, 1, 0.7f}, 1, 6, 0xFFFFFF, 0, 1, 1.0f},                       // the display's picture
    {{0, 0, 1, 1}, 1, 1, 0x808080, 0.25f, 16, 0},                           // stock dash pieces
    {{0, 0, 1, 1}, 1, 2, 0xC0C0C0, 0, 1, 1.0f},                             // their lamps
    {{0, 0, 1, 1}, 1, 0, 0x0A1012, 0.5f, 40, 0.05f},                        // screen glass
    {{0, 0, 1, 1}, 1, 0, 0xB8B6AE, 0.10f, 8, 0},                            // label strip
    {{0, 0, 1, 1}, 1, 0, 0xFFA020, 0, 1, 0.9f},                             // amber lamps
    {{0.000f, 0.805f, 1.000f, 0.900f}, 1.6f, 1, 0x3A3D40, 0.20f, 14, 0},     // charcoal plate
    ScreenFrameFinish(),                                                    // stock screen frame
    ScreenGlassFinish(ALL_SCREEN_ART),                                      // its glass
    ScreenBackingFinish(ALL_SCREEN_ART),                                    // behind the glass
};

const StockScreenLook SNOW_SCREENS = {&ALL_SCREEN_ART, SNOW_SCREEN_FRAME, SNOW_SCREEN_GLASS, SNOW_SCREEN_BACKING,
                                      SNOW_CHARCOAL};

constexpr Vec3 SNOW_EYE = {0.0f, 1.5f, 1.5f};
constexpr Vec3 SNOW_GUNNER_EYE = {0.0f, 1.58f, 2.70f}; // hp_gunner (0, 0.38, -0.89) and his head, in cockpit space

void SnowGunnerStation(Mesh& mesh)
{
    const Vec3 eye = SNOW_GUNNER_EYE;
    constexpr float FLOOR = 0.55f;
    mesh.part = 1;
    {
        const float disc[3][2] = {{0, 0.28f}, {0.03f, 0.28f}, {0.035f, 0.26f}};
        Lathe(mesh, MakeFrame({eye.x, FLOOR, eye.z}, {1, 0, 0}, {0, 1, 0}), disc, 3, 32, SNOW_FRAME);
        const SeatLook seat = {0.20f, 0.55f, 0.20f, false, false, SNOW_PAD, SNOW_FRAME, SNOW_METAL, SNOW_DARK, SNOW_DARK};
        Seat(mesh, eye, FLOOR + 0.03f, seat);
        const Vec3 column[2] = {{eye.x, FLOOR + 0.03f, eye.z - 0.30f}, {eye.x, eye.y - 0.40f, eye.z - 0.30f}};
        Tube(mesh, column, 2, 0.03f, 12, SNOW_METAL);
        const Frame head = MakeFrame({eye.x, eye.y - 0.38f, eye.z - 0.30f}, {1, 0, 0}, {0, 1, 0});
        Box(mesh, head, {0.10f, 0.05f, 0.05f}, 0.01f, SNOW_FRAME);
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 arm[2] = {head.At(side * 0.10f, 0, 0), head.At(side * 0.20f, 0.02f, 0.03f)};
            Tube(mesh, arm, 2, 0.014f, 8, SNOW_METAL);
            const Vec3 grip[2] = {head.At(side * 0.20f, 0.02f, 0.03f), head.At(side * 0.20f, 0.14f, 0.03f)};
            Tube(mesh, grip, 2, 0.0175f, 10, SNOW_DARK);
        }
    }
    mesh.part = 0; // headset run 19 (idren2023): no display; the head aims and the aiming line shows the shots
}

// A panel of the stock art: a frame round a dark inset, the whole picture on it at its own proportion (width over
// height `aspect`), not tiled (a tiled inset showed cut pieces of it).
void SnowArtPanel(Mesh& mesh, const Frame& frame, float halfWidth, int art, float aspect)
{
    constexpr float BORDER = 0.012f;
    const float halfHeight = halfWidth / aspect;
    // the frame's z may point into the wall it sits on: the border stands, and the picture lies, toward the eye
    const float out = Dot(frame.z, mesh.eye - frame.origin) < 0 ? -1.0f : 1.0f;
    Panel(mesh, frame, halfWidth + BORDER, halfHeight + BORDER, BORDER, out * BORDER, SNOW_FRAME, SNOW_DARK, 0,
          SNOW_METAL);
    const Frame face = Offset(frame, 0, 0, out * 0.001f);
    Quad(mesh, face.At(-halfWidth, -halfHeight, 0), face.At(halfWidth, -halfHeight, 0), face.At(halfWidth, halfHeight, 0),
         face.At(-halfWidth, halfHeight, 0), art, 0, 1, 1, 0);
}

void BuildSnowspeeder(Mesh& mesh)
{
    // The tub (the canopy's glass from x +-0.58, its sill 0.12 under the eye, its front 0.74 ahead): walls leaning out
    // a little, the floor 1.0 under the eye (the pilot reclined), from the dash back past the gunner's station.
    constexpr float FLOOR = 0.55f, SILL = 1.38f, FRONT = 0.80f, BACK = 3.30f, LOW = 0.48f, HIGH = 0.56f;
    StockExterior(mesh, StockVertices("EXTERIOR_SNOWSPEEDER"), StockCount("EXTERIOR_SNOWSPEEDER"), SNOW_STOCK, SNOW_GLASS, -1, SNOW_CHARCOAL, 3.0f, 1.40f);
    TiledQuad(mesh, {-LOW, FLOOR, FRONT}, {2 * LOW, 0, 0}, {0, 0, BACK - FRONT}, SNOW_CHARCOAL); // the plating, dark
    for (int side = -1; side <= 1; side += 2)
    {
        const float s = static_cast<float>(side);
        PlanarQuad(mesh, {s * LOW, FLOOR, FRONT}, {s * LOW, FLOOR, BACK}, {s * HIGH, SILL, BACK},
                   {s * HIGH, SILL, FRONT}, SNOW_PLATE);
        // the sill: a padded rail along the canopy's foot; the cable rod low along the wall
        Box(mesh, MakeFrame({s * (HIGH - 0.03f), SILL - 0.02f, (FRONT + BACK) * 0.5f}, {0, 0, 1}, {0, 1, 0}),
            {(BACK - FRONT) * 0.5f, 0.03f, 0.04f}, 0.012f, SNOW_FRAME);
        Box(mesh, MakeFrame({s * (LOW + 0.005f - 0.02f), FLOOR + 0.18f, (FRONT + BACK) * 0.5f}, {0, 0, 1}, {0, 1, 0}),
            {(BACK - FRONT) * 0.5f, 0.02f, 0.015f}, 0.005f, SNOW_RAIL);
        // the side consoles (the film, ESB frames 0684 / 0734: dark grey angled panels from the dash back beside the
        // pilot at shoulder height, dense with switches; small amber lamps on the right): a wedge on each wall from
        // the dash back past the seat's front, its sloped face (from 0.42 m out at 1.02 m up to the wall under the
        // sill) carrying the stock art's panels; the left one stays above the throttle's shelf
        {
            constexpr float Z0 = 0.84f, Z1 = 1.46f, IN_X = 0.42f, IN_Y = 1.02f, OUT_Y = SILL - 0.04f;
            const float outX = LOW + (HIGH - LOW) * (OUT_Y - FLOOR) / (SILL - FLOOR) - 0.005f;
            const float wallX = LOW + (HIGH - LOW) * (IN_Y - FLOOR) / (SILL - FLOOR) - 0.005f;
            const Vec3 a0 = {s * IN_X, IN_Y, Z0}, a1 = {s * IN_X, IN_Y, Z1};
            const Vec3 b0 = {s * outX, OUT_Y, Z0}, b1 = {s * outX, OUT_Y, Z1};
            const Vec3 c0 = {s * wallX, IN_Y, Z0}, c1 = {s * wallX, IN_Y, Z1};
            if (side < 0)
            {
                PlanarQuad(mesh, a0, a1, b1, b0, SNOW_CHARCOAL);
                PlanarQuad(mesh, a0, b0, c0, c0, SNOW_FRAME); // the front end (a triangle)
                PlanarQuad(mesh, a1, c1, b1, b1, SNOW_FRAME); // the back end
                Quad(mesh, c0, c1, a1, a0, SNOW_DARK);  // the underside
            }
            else
            {
                PlanarQuad(mesh, a0, b0, b1, a1, SNOW_CHARCOAL);
                PlanarQuad(mesh, a0, c0, b0, b0, SNOW_FRAME);
                PlanarQuad(mesh, a1, b1, c1, c1, SNOW_FRAME);
                Quad(mesh, c0, a0, a1, c1, SNOW_DARK);
            }
            // a frame bar along the sloped face's inner edge
            Box(mesh, MakeFrame({s * (IN_X + 0.004f), IN_Y + 0.01f, (Z0 + Z1) * 0.5f}, {0, 0, 1}, {0, 1, 0}),
                {(Z1 - Z0) * 0.5f, 0.012f, 0.012f}, 0.004f, SNOW_FRAME);
            const Vec3 slopeMid = (a0 + b0) * 0.5f;
            const auto face = [&](float z)
            {
                return MakeFrame({slopeMid.x - s * 0.004f, slopeMid.y + 0.004f, z}, {0, 0, -s}, {s * (outX - IN_X), OUT_Y - IN_Y, 0});
            };
            if (side < 0)
            {
                SnowArtPanel(mesh, face(0.98f), 0.10f, SNOW_SLIDERS, 164.0f / 92.0f);
                SnowArtPanel(mesh, face(1.20f), 0.065f, SNOW_DIAL, 1.0f);
                SnowArtPanel(mesh, face(1.38f), 0.09f, SNOW_KNOBS, 123.0f / 72.0f);
            }
            else
            {
                SnowArtPanel(mesh, face(0.98f), 0.09f, SNOW_KNOBS, 123.0f / 72.0f);
                SnowArtPanel(mesh, face(1.20f), 0.10f, SNOW_BOX, 154.0f / 56.0f);
                SnowArtPanel(mesh, face(1.38f), 0.10f, SNOW_SLIDERS, 164.0f / 92.0f);
            }
        }
        SnowArtPanel(mesh, MakeFrame({s * (LOW - 0.02f), 1.00f, 2.75f}, {0, 0, -s}, {s * 0.3f, 1, 0}), 0.07f, SNOW_TALL,
                     77.0f / 195.0f);
    }
    // The dash (idren2023 2026-11-08: the pilot view rebuilt from the film and the game; the HUD's screens on it): the
    // game's own three-part dash at the player's scale (tools/stockcockpit.py SNOW: its faces 0.66 m ahead, 15-46 cm
    // under the eye; the film's low dark dash with the vents, the round screen and the key grid in its middle, ESB
    // laserdisc frames 0663 / 0666), a charcoal cowl over it reaching to the glass with the film's light label strip
    // along its near edge, the body behind it and a recessed kick panel under it for the legs. The HUD: the minimap on
    // its radar screen (left), the target on the round screen (middle); the health over the score and the weapon's
    // ammunition on two pods at the front pillars' feet, turned to the eye (out of the front pane, a glance away).
    {
        constexpr float DASH_TOP = 1.35f, DASH_BOTTOM = 1.038f, DASH_NEAR = 0.84f, DASH_FAR = 0.744f, DASH_HALF = 0.492f;
        StockPieces(mesh, StockVertices("STOCK_SNOW"), StockCount("STOCK_SNOW"), SNOW_COCKPIT, SNOW_FX, SNOW_GLASS);
        // the body behind the stock faces, from the floor to the cowl, as wide as the dash
        Box(mesh, MakeFrame({0, (DASH_BOTTOM + DASH_TOP) * 0.5f, DASH_FAR - 0.07f}, {1, 0, 0}, {0, 1, 0}),
            {DASH_HALF, (DASH_TOP - DASH_BOTTOM) * 0.5f, 0.07f}, 0.01f, SNOW_FRAME);
        // the cowl: a board over the dash, its near edge a little proud of the faces, sloping down to the glass
        const Vec3 nearLeft = {-DASH_HALF - 0.01f, DASH_TOP + 0.012f, DASH_NEAR + 0.02f};
        const Vec3 nearRight = {DASH_HALF + 0.01f, DASH_TOP + 0.012f, DASH_NEAR + 0.02f};
        const Vec3 farRight = {DASH_HALF + 0.01f, DASH_TOP - 0.01f, DASH_FAR - 0.16f};
        const Vec3 farLeft = {-DASH_HALF - 0.01f, DASH_TOP - 0.01f, DASH_FAR - 0.16f};
        PlanarQuad(mesh, nearLeft, nearRight, farRight, farLeft, SNOW_CHARCOAL);
        const Vec3 down = {0, -0.02f, 0};
        PlanarQuad(mesh, nearLeft + down, nearRight + down, nearRight, nearLeft, SNOW_FRAME);
        // the label strip along the cowl's near edge, with dark legend ticks
        const Frame strip = MakeFrame({0, DASH_TOP + 0.0125f, DASH_NEAR + 0.005f}, {1, 0, 0}, {0, 0.22f, -1.0f});
        Quad(mesh, strip.At(-0.40f, -0.012f, 0.001f), strip.At(0.40f, -0.012f, 0.001f), strip.At(0.40f, 0.012f, 0.001f),
             strip.At(-0.40f, 0.012f, 0.001f), SNOW_LABEL);
        for (int k = 0; k < 14; ++k)
        {
            const float x = -0.36f + k * 0.055f;
            Quad(mesh, strip.At(x - 0.012f, -0.003f, 0.002f), strip.At(x + 0.012f, -0.003f, 0.002f),
                 strip.At(x + 0.012f, 0.003f, 0.002f), strip.At(x - 0.012f, 0.003f, 0.002f), SNOW_DARK);
        }
        // the kick panel: recessed under the dash (the legs reach under it), a dark lip along the dash's foot
        Box(mesh, MakeFrame({0, DASH_BOTTOM - 0.012f, DASH_NEAR - 0.02f}, {1, 0, 0}, {0, 1, 0}),
            {DASH_HALF, 0.012f, 0.03f}, 0.006f, SNOW_FRAME);
        const float kickZ = FRONT - 0.005f; // where the tub's walls start (further ahead the ship's nose shows)
        const float kickHalf = LOW + (HIGH - LOW) * (DASH_BOTTOM - FLOOR) / (SILL - FLOOR);
        PlanarQuad(mesh, {-LOW, FLOOR, kickZ}, {LOW, FLOOR, kickZ}, {kickHalf, DASH_BOTTOM - 0.024f, kickZ},
             {-kickHalf, DASH_BOTTOM - 0.024f, kickZ}, SNOW_FRAME);
        // between the dash's foot and the kick panel's top (its underside)
        Quad(mesh, {-DASH_HALF, DASH_BOTTOM - 0.024f, DASH_NEAR - 0.05f}, {DASH_HALF, DASH_BOTTOM - 0.024f, DASH_NEAR - 0.05f},
             {kickHalf, DASH_BOTTOM - 0.024f, kickZ}, {-kickHalf, DASH_BOTTOM - 0.024f, kickZ}, SNOW_DARK);
        // the HUD in the tanks' stock screens (idren2023 2026-11-09: every vehicle's HUD in that look; the map on the
        // radar screen, where it had been a dim blue blob on the stock art): one over the radar CRT (left panel),
        // one over the round screen (middle), the readouts in the screens' colour
        SetDisplayScreen(&SNOW_SCREENS);
        {
            const auto screen = [&](Vec3 at, float halfWidth, float halfHeight)
            {
                const Frame face = MakeFrame(at, {1, 0, 0}, {0, 1, 0});
                StockScreen(mesh, face, halfWidth, halfHeight, SCREEN_TEXEL_METERS * halfHeight / 0.0675f, SNOW_SCREENS,
                            0.01f);
                return face;
            };
            AddDisplay(GROUP_MAP, screen({-0.290f, 1.200f, 0.834f}, 0.056f, 0.052f), 0, 0, 0.108f, 0.100f);
            AddDisplay(GROUP_TARGET, screen({0.043f, 1.262f, 0.848f}, 0.050f, 0.026f), 0, 0, 0.096f, 0.048f);
        }
        // the pods: a charcoal housing on the cowl just inside each front pillar (the canopy's pillar feet hid them at
        // 0.435 m out once the frame was back, 2026-11-09), its face turned to the eye, a
        // screen in it; the right one with the film's red dome lamp beside it
        for (int side = -1; side <= 1; side += 2)
        {
            const float s = static_cast<float>(side);
            // (2026-11-09: wholly under the canopy's lower frame band, which the hooded pods reached into: no hood,
            // a shallow housing, lower and nearer the eye)
            const Frame pod = FacingEye({s * 0.30f, DASH_TOP + 0.062f, DASH_NEAR + 0.13f});
            const float halfWidth = 0.062f, halfHeight = 0.044f;
            StockScreen(mesh, pod, halfWidth, halfHeight, SCREEN_TEXEL_METERS * halfHeight / 0.0675f, SNOW_SCREENS,
                        0.03f);
            const Frame glass = pod;
            if (side < 0)
            {
                AddDisplay(GROUP_HEALTH, glass, 0, 0.012f, halfWidth * 1.9f, halfHeight * 1.15f);
                AddDisplay(GROUP_SCORE, glass, 0, -0.030f, halfWidth * 1.9f, halfHeight * 0.55f);
            }
            else
            {
                AddDisplay(GROUP_AMMUNITION, glass, 0, 0, halfWidth * 1.9f, halfHeight * 1.6f);
                IndicatorLamp(mesh, Upright(MakeFrame(pod.At(0.105f, -0.03f, -0.03f), {1, 0, 0}, {0, 1, 0})), 0.016f,
                              SNOW_FRAME, SNOW_LAMP);
            }
            // two small lamps under the screen
            for (int k = -1; k <= 1; k += 2)
            {
                IndicatorLamp(mesh, MakeFrame(pod.At(k * 0.03f, -halfHeight - 0.010f, 0.0f), pod.x, pod.z), 0.004f,
                              SNOW_FRAME, side < 0 ? SNOW_LAMP : SNOW_AMBER);
            }
        }
        SetDisplayScreen(nullptr);
        // amber lamps along the right side console's top (the film's)
        for (int k = 0; k < 5; ++k)
        {
            IndicatorLamp(mesh, Upright(MakeFrame({HIGH - 0.06f, SILL - 0.03f, 1.00f + k * 0.07f}, {0, 0, 1}, {0, 1, 0})),
                          0.006f, SNOW_FRAME, SNOW_AMBER);
        }
    }
    // The flight stick's pedestal (Flight: the eye + (0, -0.86, -0.34)), the throttle's slot on the left shelf (the
    // eye + (-0.25, -0.64, -0.27)); the pilot's seat (cream pads, reclined).
    {
        const Vec3 base = {SNOW_EYE.x, SNOW_EYE.y - 0.86f, SNOW_EYE.z - 0.34f};
        const float h = base.y - FLOOR;
        const float rings[4][2] = {{0, 0.09f}, {0.02f, 0.09f}, {0.03f, 0.07f}, {h, 0.07f}};
        Lathe(mesh, MakeFrame({base.x, FLOOR, base.z}, {1, 0, 0}, {0, 1, 0}), rings, 4, 18, SNOW_FRAME);
        // the shelf from the wall to 6 cm inboard of the throttle, from ahead of it back past the take-off lever's hinge
        // (the eye + (-0.36, -0.62, -0.10)), its top under the throttle's hub and the lever's carriage (headset run 19: its
        // half width came out negative, the box inside out: both handles floated)
        const Vec3 throttle = {SNOW_EYE.x - 0.25f, SNOW_EYE.y - 0.64f, SNOW_EYE.z - 0.27f};
        const float shelfTop = throttle.y - 0.006f, shelfInner = throttle.x + 0.06f, shelfFront = throttle.z - 0.14f;
        const float shelfBack = SNOW_EYE.z - 0.10f + 0.10f;
        const float shelfOuter = -(HIGH + 0.01f); // into the leaning wall
        Box(mesh, MakeFrame({(shelfInner + shelfOuter) * 0.5f, (FLOOR + shelfTop) * 0.5f, (shelfFront + shelfBack) * 0.5f}, {1, 0, 0}, {0, 1, 0}),
            {(shelfInner - shelfOuter) * 0.5f, (shelfTop - FLOOR) * 0.5f, (shelfBack - shelfFront) * 0.5f}, 0.02f, SNOW_FRAME);
        Box(mesh, MakeFrame({throttle.x, shelfTop + 0.002f, throttle.z}, {1, 0, 0}, {0, 1, 0}), {0.012f, 0.002f, 0.10f},
            0.001f, SNOW_DARK);
        Box(mesh, MakeFrame({SNOW_EYE.x - 0.36f, shelfTop + 0.002f, SNOW_EYE.z - 0.16f}, {1, 0, 0}, {0, 1, 0}),
            {0.012f, 0.002f, 0.09f}, 0.001f, SNOW_DARK);
        const SeatLook seat = {0.21f, 0.62f, 0.30f, false, false, SNOW_PAD, SNOW_FRAME, SNOW_METAL, SNOW_DARK, SNOW_DARK};
        Seat(mesh, SNOW_EYE, FLOOR, seat);
    }
    // Between the two seats: a low bulkhead, the gunner's fixed turntable ring behind it.
    Box(mesh, MakeFrame({0, FLOOR + 0.35f, 2.12f}, {1, 0, 0}, {0, 1, 0}), {LOW, 0.35f, 0.04f}, 0.01f, SNOW_FRAME);
    {
        const float ring[3][2] = {{0, 0.34f}, {0.03f, 0.34f}, {0.035f, 0.30f}};
        Lathe(mesh, MakeFrame({SNOW_GUNNER_EYE.x, FLOOR, SNOW_GUNNER_EYE.z}, {1, 0, 0}, {0, 1, 0}), ring, 3, 32,
              SNOW_METAL);
    }
    TiledQuad(mesh, {-LOW, FLOOR, BACK}, {2 * LOW, 0, 0}, {0, SILL - FLOOR, 0}, SNOW_PLATE);
    SnowGunnerStation(mesh);
}

// ------------------------------------------------------------------------------------------------------------ gunship
// The LAAT/i gunship (the vehicle proposal 2026-11-04; docs/research/vehicle-interiors-research.md: pilot and co-pilot
// gunner in tandem bubbles, the gunner lower in the nose, the pilot higher behind; ball turrets on arms at the bay's
// rear corners). The pilot: the game's first-person cockpit (rep_1st_cockpit_gunship: the glass dome, its frame and
// the dash) closed into his cab (floor, sills, the cab behind the dome up to the bulkhead with the troop bay's hatch),
// the helicopter stick and the collective on the left (Flight). The co-pilot (TURRET1, the top gun's turret, YawLimits
// -89..89): ahead and below in his own glass bubble, on a swivel station as the AT-AT gunner's (SPEEDER_CABIN_PARTS:
// 1 the chair, column, grips and rails, 2 the display, 3 and 4 its stop lamps, 5 the picture). Cockpit model space (x
// right, y up, z back); the pilot's eye under the dome's back (the game's (0, 1.5, 1.5) is behind it). Finishes from
// the cockpit's own texture (rep_1st_cockpit_gunship.tga, 512 px).
enum GunshipFinish
{
    GUN_PLATE,   // brushed light grey plate
    GUN_RIVETS,  // the riveted strip along the texture's foot
    GUN_MAROON,  // the maroon stripe (the Republic's)
    GUN_CLUSTER, // the instrument cluster (lit screens)
    GUN_RADAR,   // the green radar shield (lit)
    GUN_WEDGE,   // the wedge panel with two green lamps
    GUN_GRILLE,  // the grille band
    GUN_FRAME,   // dark frames
    GUN_DARK,    // recesses, the bay's hatchway
    GUN_SEAT,    // seat pads (dark grey)
    GUN_METAL,   // pipes, rails
    GUN_LAMP,    // red lamps (lit)
    GUN_GLASS,   // the co-pilot's bubble (the cockpit's window texture, see-through)
    GUN_PICTURE, // the co-pilot's display: the scope pass's picture
    GUN_STOCK,   // the ship's own exterior (texture 3: the ball turrets')
    GUN_COCKPIT, // the game's pilot cockpit (StockPieces), its own mapping
    GUN_FX,      //   its lamps and screens (effects texture)
    GUN_FINISH_COUNT
};

const Finish GUN_FINISHES[GUN_FINISH_COUNT] = {
    {{0.640f, 0.050f, 0.880f, 0.220f}, 0.8f, 1, 0xB4B6B8, 0.25f, 18, 0},     // plate (328..450, 26..113)
    {{0.000f, 0.880f, 1.000f, 0.995f}, 1.6f, 1, 0xA8AAAC, 0.25f, 18, 0},     // rivets (0..512, 451..509)
    {{0.000f, 0.830f, 1.000f, 0.870f}, 1.6f, 1, 0xA06070, 0.20f, 14, 0},     // maroon stripe (0..512, 425..445)
    {{0.020f, 0.060f, 0.430f, 0.400f}, 0.36f, 1, 0xB0B2B4, 0.20f, 16, 0.3f}, // cluster (10..220, 31..205)
    {{0.460f, 0.050f, 0.630f, 0.270f}, 0.16f, 1, 0xB0B2B4, 0.20f, 16, 0.5f}, // radar (236..323, 26..138)
    {{0.000f, 0.400f, 0.380f, 0.800f}, 0.30f, 1, 0xB0B2B4, 0.20f, 16, 0.2f}, // wedge (0..195, 205..410)
    {{0.400f, 0.700f, 1.000f, 0.800f}, 0.9f, 1, 0xA0A2A4, 0.25f, 18, 0},     // grille (205..512, 358..410)
    {{0.640f, 0.050f, 0.880f, 0.220f}, 0.7f, 1, 0x585A5C, 0.22f, 16, 0},     // dark frames
    {{0, 0, 1, 1}, 1, 0, 0x0E0F10, 0.05f, 6, 0},                            // recesses
    {{0.640f, 0.050f, 0.880f, 0.220f}, 0.5f, 1, 0x4C4E50, 0.18f, 10, 0},     // seat pads
    {{0, 0, 1, 1}, 1, 0, 0x55585C, 0.25f, 20, 0},                           // metal (darker: plain grey read as white, review)
    {{0, 0, 1, 1}, 1, 0, 0xFF3020, 0, 1, 0.9f},                             // red lamps
    {{0, 0, 1, 1}, 1, 4, 0xFFFFFF, 0.6f, 60, 0, 0.35f},                     // bubble glass
    {{0, 0.3f, 1, 0.7f}, 1, 6, 0xFFFFFF, 0, 1, 1.0f},                       // the display's picture
    {{0, 0, 1, 1}, 1, 3, 0x808080, 0.20f, 14, 0},                           // the exterior, its own mapping
    {{0, 0, 1, 1}, 1, 1, 0x808080, 0.20f, 14, 0},                           // the stock cockpit
    {{0, 0, 1, 1}, 1, 2, 0xC0C0C0, 0, 1, 1.0f},                             // its lamps and screens
};

constexpr Vec3 GUN_EYE = {0.0f, 1.52f, 0.55f}; // headset run 19: 12 cm higher (the dome's frame at 1.27 cut the side view)
constexpr Vec3 GUN_BALL_EYE = {0.0f, 1.5f, 1.5f};
constexpr Vec3 GUN_COPILOT_EYE = {0.0f, 0.42f, -1.55f}; // ahead and 1 m below, in the nose's bubble

// The co-pilot's swivel station (as AtatGunnerStation; his floor `floorY`).
void GunshipCopilotStation(Mesh& mesh, float floorY)
{
    const Vec3 eye = GUN_COPILOT_EYE;
    mesh.part = 1;
    {
        const float disc[3][2] = {{0, 0.32f}, {0.04f, 0.32f}, {0.045f, 0.30f}};
        Lathe(mesh, MakeFrame({eye.x, floorY, eye.z}, {1, 0, 0}, {0, 1, 0}), disc, 3, 32, GUN_FRAME);
        const SeatLook seat = {0.21f, 0.55f, 0.15f, false, false, GUN_SEAT, GUN_FRAME, GUN_METAL, GUN_SEAT, GUN_DARK};
        Seat(mesh, eye, floorY + 0.04f, seat);
        const Vec3 column[2] = {{eye.x, floorY + 0.04f, eye.z - 0.30f}, {eye.x, eye.y - 0.40f, eye.z - 0.30f}};
        Tube(mesh, column, 2, 0.03f, 12, GUN_METAL);
        const Frame head = MakeFrame({eye.x, eye.y - 0.38f, eye.z - 0.30f}, {1, 0, 0}, {0, 1, 0});
        Box(mesh, head, {0.10f, 0.05f, 0.05f}, 0.01f, GUN_FRAME);
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 arm[2] = {head.At(side * 0.10f, 0, 0), head.At(side * 0.20f, 0.02f, 0.03f)};
            Tube(mesh, arm, 2, 0.014f, 8, GUN_METAL);
            const Vec3 grip[2] = {head.At(side * 0.20f, 0.02f, 0.03f), head.At(side * 0.20f, 0.14f, 0.03f)};
            Tube(mesh, grip, 2, 0.0175f, 10, GUN_DARK);
        }
    }
    mesh.part = 0; // headset run 19 (idren2023: aiming lines, no reticles; review: an empty frame floated ahead): no display
}

void BuildGunship(Mesh& mesh)
{
    // The stock dome: glass x +-0.93, y 0.77..2.00, z -0.59..0.77; its frame x +-0.98, y 0.37..1.27, z -0.96..0.49.
    constexpr float FLOOR = 0.37f, HALF = 0.98f, SILL = 1.27f, TOP = 2.00f, FRONT = -0.96f, BACK = 1.70f;
    // The game's pilot cockpit drawn by us (isWholeCockpit; tools/stockcockpit.py GUNSHIP): its frame, dome glass and
    // floor at their size, its dash 1.12x so it fills the sill (idren2023 2026-11-11: squashed, not fitting the dash).
    // The HUD on the dash's middle screen (scaled: 0.67 x 0.40 m, its face at z 0.09): health over score, the map,
    // ammunition over target.
    StockPieces(mesh, StockVertices("STOCK_GUNSHIP"), StockCount("STOCK_GUNSHIP"), GUN_COCKPIT, GUN_FX, GUN_GLASS);
    {
        const Frame screen = MakeFrame({0, 0.952f, 0.092f}, {1, 0, 0}, {0, 1, 0});
        AddDisplay(GROUP_HEALTH, screen, -0.22f, 0.06f, 0.20f, 0.10f);
        AddDisplay(GROUP_SCORE, screen, -0.22f, -0.09f, 0.20f, 0.05f);
        AddDisplay(GROUP_MAP, screen, 0, 0, 0.18f, 0.18f);
        AddDisplay(GROUP_AMMUNITION, screen, 0.22f, 0.06f, 0.20f, 0.10f);
        AddDisplay(GROUP_TARGET, screen, 0.22f, -0.09f, 0.18f, 0.05f);
    }
    // Floor, the walls under the sill from the dash back, the cab behind the dome (walls up to its top, the roof).
    TiledQuad(mesh, {-HALF, FLOOR, FRONT}, {2 * HALF, 0, 0}, {0, 0, BACK - FRONT}, GUN_PLATE);
    for (int side = -1; side <= 1; side += 2)
    {
        const float x = side * HALF;
        TiledQuad(mesh, {x, FLOOR, FRONT}, {0, 0, BACK - FRONT}, {0, SILL - FLOOR, 0}, GUN_PLATE);
        TiledQuad(mesh, {x, SILL, 0.80f}, {0, 0, BACK - 0.80f}, {0, TOP - SILL, 0}, GUN_PLATE); // behind the glass (review)
        Box(mesh, MakeFrame({x * 0.99f, SILL - 0.05f, (FRONT + BACK) * 0.5f}, {0, 0, 1}, {0, 1, 0}),
            {(BACK - FRONT) * 0.5f, 0.02f, 0.02f}, 0.005f, GUN_MAROON);
        Panel(mesh, MakeFrame({x - side * 0.01f, 0.95f, 1.20f}, {0, 0, -static_cast<float>(side)}, {0, 1, 0}), 0.22f,
              0.20f, 0.03f, 0.02f, GUN_FRAME, side < 0 ? GUN_WEDGE : GUN_CLUSTER, 0, GUN_METAL);
    }
    TiledQuad(mesh, {-HALF, TOP, 0.77f}, {2 * HALF, 0, 0}, {0, 0, BACK - 0.77f}, GUN_PLATE);
    // The dome's back edge to the roof: a frame across.
    Box(mesh, MakeFrame({0, (SILL + TOP) * 0.5f + 0.35f, 0.79f}, {1, 0, 0}, {0, 1, 0}), {HALF, 0.06f, 0.05f}, 0.02f,
        GUN_FRAME);
    // The bulkhead to the troop bay with its hatchway (dark), the rivet strip across.
    {
        constexpr float DOOR = 0.40f, DOOR_TOP = 1.80f;
        TiledQuad(mesh, {-HALF, FLOOR, BACK}, {HALF - DOOR, 0, 0}, {0, TOP - FLOOR, 0}, GUN_PLATE);
        TiledQuad(mesh, {DOOR, FLOOR, BACK}, {HALF - DOOR, 0, 0}, {0, TOP - FLOOR, 0}, GUN_PLATE);
        TiledQuad(mesh, {-DOOR, DOOR_TOP, BACK}, {2 * DOOR, 0, 0}, {0, TOP - DOOR_TOP, 0}, GUN_PLATE);
        Quad(mesh, {-DOOR, FLOOR, BACK + 0.3f}, {DOOR, FLOOR, BACK + 0.3f}, {DOOR, DOOR_TOP, BACK + 0.3f},
             {-DOOR, DOOR_TOP, BACK + 0.3f}, GUN_DARK);
        for (const float x : {-DOOR, DOOR})
        {
            TiledQuad(mesh, {x, FLOOR, BACK}, {0, 0, 0.3f}, {0, DOOR_TOP - FLOOR, 0}, GUN_FRAME);
        }
        TiledQuad(mesh, {-DOOR, DOOR_TOP, BACK}, {2 * DOOR, 0, 0}, {0, 0, 0.3f}, GUN_FRAME);
        TiledQuad(mesh, {-DOOR, FLOOR, BACK}, {2 * DOOR, 0, 0}, {0, 0, 0.3f}, GUN_FRAME);
        Box(mesh, MakeFrame({0, 1.95f - 0.05f, BACK - 0.02f}, {1, 0, 0}, {0, 1, 0}), {HALF, 0.05f, 0.02f}, 0.005f,
            GUN_RIVETS);
    }
    // The pilot: his seat; the stick's pedestal (Flight: the eye + (0, -0.86, -0.34)); the collective's console on his
    // left (its pivot the eye + (-0.27, -0.70, -0.05)) with its slot; the radar on the dash's left wing.
    const SeatLook look = {0.22f, 0.62f, 0.15f, false, false, GUN_SEAT, GUN_FRAME, GUN_METAL, GUN_SEAT, GUN_DARK};
    Seat(mesh, GUN_EYE, FLOOR, look);
    {
        const Vec3 base = {GUN_EYE.x, GUN_EYE.y - 0.86f, GUN_EYE.z - 0.34f};
        const float h = base.y - FLOOR;
        const float rings[4][2] = {{0, 0.09f}, {0.02f, 0.09f}, {0.03f, 0.07f}, {h, 0.07f}};
        Lathe(mesh, MakeFrame({base.x, FLOOR, base.z}, {1, 0, 0}, {0, 1, 0}), rings, 4, 18, GUN_FRAME);
        const Vec3 pivot = {GUN_EYE.x - 0.27f, GUN_EYE.y - 0.70f, GUN_EYE.z - 0.05f};
        Box(mesh, MakeFrame({(pivot.x - HALF) * 0.5f, (FLOOR + pivot.y) * 0.5f, pivot.z}, {1, 0, 0}, {0, 1, 0}),
            {(HALF + pivot.x) * 0.5f + 0.05f, (pivot.y - FLOOR) * 0.5f, 0.16f}, 0.02f, GUN_FRAME);
        Box(mesh, MakeFrame({pivot.x, pivot.y + 0.004f, pivot.z}, {1, 0, 0}, {0, 1, 0}), {0.014f, 0.004f, 0.12f}, 0.002f,
            GUN_DARK);
        Quad(mesh, {pivot.x - 0.22f, pivot.y + 0.003f, pivot.z - 0.14f}, {pivot.x - 0.06f, pivot.y + 0.003f, pivot.z - 0.14f},
             {pivot.x - 0.06f, pivot.y + 0.003f, pivot.z + 0.12f}, {pivot.x - 0.22f, pivot.y + 0.003f, pivot.z + 0.12f},
             GUN_WEDGE);
    }
    // The co-pilot's bubble ahead and below: a glass half dome over his floor (1 m under the pilot's), the step down
    // under the pilot's dash, the bubble's frame rings; his swivel station.
    {
        const float copilotFloor = GUN_COPILOT_EYE.y - 1.10f;
        const Vec3 middle = {GUN_COPILOT_EYE.x, GUN_COPILOT_EYE.y - 0.15f, GUN_COPILOT_EYE.z};
        constexpr float R = 0.95f;
        // the dome: rings of glass from the floor's level up, forward half and sides (behind him the step)
        float profile[8][2];
        for (int k = 0; k < 8; ++k)
        {
            const float a = (-20.0f + k * 15.0f) * 0.0174533f; // -20..85 deg
            profile[k][0] = R * sinf(a);
            profile[k][1] = R * cosf(a);
        }
        Lathe(mesh, MakeFrame(middle, {1, 0, 0}, {0, 1, 0}), profile, 8, 24, GUN_GLASS);
        for (const float a : {-20.0f, 20.0f, 55.0f})
        {
            const float y = R * sinf(a * 0.0174533f), r = R * cosf(a * 0.0174533f);
            const float ring[4][2] = {{y - 0.02f, r - 0.045f}, {y - 0.02f, r - 0.012f}, {y + 0.02f, r - 0.012f},
                                      {y + 0.02f, r - 0.045f}};
            Lathe(mesh, MakeFrame(middle, {1, 0, 0}, {0, 1, 0}), ring, 4, 24, GUN_FRAME);
        }
        const float lower = middle.y + R * sinf(-20.0f * 0.0174533f);
        const float skirt[3][2] = {{copilotFloor - middle.y, R * 0.94f}, {lower - middle.y, R * 0.94f},
                                   {lower - middle.y + 0.01f, R * 0.95f}};
        Lathe(mesh, MakeFrame(middle, {1, 0, 0}, {0, 1, 0}), skirt, 3, 24, GUN_PLATE);
        const float disc[2][2] = {{copilotFloor - middle.y, 0.0f}, {copilotFloor - middle.y, R * 0.94f}};
        Lathe(mesh, MakeFrame(middle, {1, 0, 0}, {0, 1, 0}), disc, 2, 24, GUN_PLATE);
        // the bubble's frame (idren2023 2026-11-11, canon: faceted panes in a dark frame): a spine and two ribs either
        // side running up the dome from its foot ring to the top
        for (const float deg : {0.0f, -40.0f, 40.0f, -80.0f, 80.0f})
        {
            const float t = deg * 0.0174533f;
            Vec3 rib[8];
            for (int k = 0; k < 8; ++k)
            {
                const float e = (-20.0f + k * 15.0f) * 0.0174533f, r = (R - 0.02f) * cosf(e);
                rib[k] = middle + Vec3{r * sinf(t), (R - 0.02f) * sinf(e), -r * cosf(t)};
            }
            Tube(mesh, rib, 8, 0.018f, 8, GUN_FRAME);
        }
        // his console: a low desk ahead of the chair under the bubble's foot, its sloped face the instrument cluster
        {
            const float top = GUN_COPILOT_EYE.y - 0.42f, z = GUN_COPILOT_EYE.z - 0.56f;
            Box(mesh, MakeFrame({0, (copilotFloor + top) * 0.5f, z}, {1, 0, 0}, {0, 1, 0}),
                {0.42f, (top - copilotFloor) * 0.5f, 0.10f}, 0.02f, GUN_FRAME);
            const Frame face = MakeFrame({0, top + 0.03f, z + 0.12f}, {1, 0, 0}, Normalized(Vec3{0, 0.8f, -0.6f}));
            Box(mesh, Offset(face, 0, 0, -0.03f), {0.42f, 0.07f, 0.03f}, 0.01f, GUN_FRAME);
            Quad(mesh, face.At(-0.30f, -0.055f, 0.002f), face.At(0.30f, -0.055f, 0.002f), face.At(0.30f, 0.055f, 0.002f),
                 face.At(-0.30f, 0.055f, 0.002f), GUN_CLUSTER);
        }
        // the step up to the pilot's floor behind him, under the dash
        TiledQuad(mesh, {-HALF, copilotFloor, FRONT}, {2 * HALF, 0, 0}, {0, FLOOR - copilotFloor, 0}, GUN_GRILLE);
        GunshipCopilotStation(mesh, copilotFloor);
    }
}

// The gunship's ball turrets (idren2023 2026-11-04: the player inside the ball, head aiming, the ship he hangs from seen
// round him): the ship's own exterior (tools/stockexterior.py, its eye at the ball's gunner: hp_gunner2 / 3 and
// TURRET2 / 3's EyePointOffset (0 0.3 0.25)) fixed (part 0); his seat on the ball's yaw ring (part 1, turned with the
// turret's yaw); the glazed ball round him with its frame and the twin cannons outside it (part 2, also pitched with
// the guns); the end-stop lamps (3 and 4). Cockpit model space, the gunner's eye at (0, 1.5, 1.5).
void BuildGunshipBall(Mesh& mesh, const StockVertex* exterior, int exteriorCount)
{
    const Vec3 eye = {0.0f, 1.5f, 1.5f};
    constexpr float R = 0.90f;
    mesh.part = 0;
    StockExterior(mesh, exterior, exteriorCount, GUN_STOCK, -1);
    mesh.part = 1;
    {
        const float floorY = eye.y - 0.80f;
        const float ring[4][2] = {{-0.03f, 0.40f}, {0.0f, 0.40f}, {0.0f, 0.10f}, {-0.03f, 0.10f}};
        Lathe(mesh, MakeFrame({eye.x, floorY, eye.z}, {1, 0, 0}, {0, 1, 0}), ring, 4, 24, GUN_FRAME);
        const SeatLook seat = {0.20f, 0.50f, 0.15f, false, false, GUN_SEAT, GUN_FRAME, GUN_METAL, GUN_SEAT, GUN_DARK};
        Seat(mesh, eye, floorY, seat);
        // the twin grips on a yoke before him (his hands' place: x +-0.20, 0.30 ahead, 0.30 under the eye)
        const Frame yoke = MakeFrame({eye.x, eye.y - 0.38f, eye.z - 0.32f}, {1, 0, 0}, {0, 1, 0});
        Box(mesh, yoke, {0.20f, 0.025f, 0.03f}, 0.01f, GUN_FRAME);
        const Vec3 column[2] = {{eye.x, floorY, eye.z - 0.30f}, yoke.origin};
        Tube(mesh, column, 2, 0.03f, 10, GUN_METAL);
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 grip[2] = {yoke.At(side * 0.20f, 0.02f, 0.02f), yoke.At(side * 0.20f, 0.16f, 0.02f)};
            Tube(mesh, grip, 2, 0.0175f, 10, GUN_DARK);
        }
    }
    mesh.part = 2;
    {
        // the ball: glass from below his feet to over his head, the frame's rings across and round it, a ring sight
        float glass[13][2];
        for (int k = 0; k <= 12; ++k)
        {
            const float a = (-80.0f + k * 13.33f) * 0.0174533f;
            glass[k][0] = R * sinf(a);
            glass[k][1] = R * cosf(a) + 0.001f;
        }
        Lathe(mesh, MakeFrame(eye, {1, 0, 0}, {0, 1, 0}), glass, 13, 28, GUN_GLASS);
        for (const float a : {-50.0f, -15.0f, 30.0f, 62.0f})
        {
            const float y = R * sinf(a * 0.0174533f), r = R * cosf(a * 0.0174533f);
            // inside the glass, clear of it (idren2023 2026-11-04: the rings on the glass z-fought)
            const float ring[4][2] = {{y - 0.015f, r - 0.045f}, {y - 0.015f, r - 0.012f}, {y + 0.015f, r - 0.012f},
                                      {y + 0.015f, r - 0.045f}};
            Lathe(mesh, MakeFrame(eye, {1, 0, 0}, {0, 1, 0}), ring, 4, 28, GUN_FRAME);
        }
        {
            // a great ring round the ball side to side (none fore-aft: it would cross the aim)
            const float ring[4][2] = {{-0.012f, R - 0.035f}, {-0.012f, R - 0.012f}, {0.012f, R - 0.012f}, {0.012f, R - 0.035f}};
            Lathe(mesh, MakeFrame(eye, {1, 0, 0}, {0, 0, 1}), ring, 4, 28, GUN_FRAME);
        }
        // the cannons either side outside the glass, along his aim
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 barrel[2] = {{eye.x + side * 0.62f, eye.y - 0.20f, eye.z - 0.93f},
                                    {eye.x + side * 0.62f, eye.y - 0.20f, eye.z - 1.85f}};
            Tube(mesh, barrel, 2, 0.06f, 12, GUN_METAL);
            const Vec3 shroud[2] = {{eye.x + side * 0.62f, eye.y - 0.20f, eye.z - 0.95f},
                                    {eye.x + side * 0.62f, eye.y - 0.20f, eye.z - 1.35f}};
            Tube(mesh, shroud, 2, 0.075f, 12, GUN_FRAME); // (review: the shroud filled a third of a side view)
            // the mount from the ball's frame to the shroud (review, headset run 19: the cannons seemed to float)
            Box(mesh, MakeFrame({eye.x + side * 0.56f, eye.y - 0.20f, eye.z - 0.86f}, {1, 0, 0}, {0, 1, 0}),
                {0.09f, 0.055f, 0.10f}, 0.015f, GUN_FRAME);
        }
        // (no ring sight since headset run 19, idren2023: aiming lines in every cockpit, no reticles)
    }
    for (int side = 1; side >= -1; side -= 2)
    {
        mesh.part = static_cast<uint8_t>(side > 0 ? 3 : 4);
        Box(mesh, MakeFrame({eye.x + side * 0.35f, eye.y + 0.55f, eye.z - 0.60f}, {1, 0, 0}, {0, 1, 0}),
            {0.015f, 0.015f, 0.01f}, 0.004f, GUN_LAMP);
    }
    mesh.part = 0;
}

void BuildGunshipBall2(Mesh& mesh)
{
    BuildGunshipBall(mesh, StockVertices("EXTERIOR_GUNSHIP_BALL2"), StockCount("EXTERIOR_GUNSHIP_BALL2"));
}

void BuildGunshipBall3(Mesh& mesh)
{
    BuildGunshipBall(mesh, StockVertices("EXTERIOR_GUNSHIP_BALL3"), StockCount("EXTERIOR_GUNSHIP_BALL3"));
}

// The gunship's door passenger (TURRET4: no weapon, third person only in stock; the vehicle proposal 2026-11-04,
// idren2023: first person at the open side door, a rail for the hand, firing his own weapon (research)). He stands in the
// troop bay at hp_passenger1, beside the starboard door (the shell's +x), facing ahead: the bay is the ship's own
// exterior model, which has it (floor, ribbed ceiling 1.64 m over his eyes, both side doors open; tools/stockexterior.py
// GUNSHIP_DOOR, his eye there). Ours: a hand rail across the bay under the ceiling just ahead of him, two hand straps
// hanging from it with upright handles at his face's height (the grips: x +-0.22, 5 cm under and 30 cm ahead of the
// eye), and a grab bar up the door's front edge. Cockpit model space (x right, y up, z back), his eye at (0, 1.5, 1.5);
// the floor at y -0.15, the ceiling at 3.14; the door's opening from x 0.63 out, z -0.5 (its front edge) to 3.8.
constexpr float GUNSHIP_DOOR_GRIP[3] = {0.22f, 1.45f, 1.20f};

void BuildGunshipDoor(Mesh& mesh)
{
    const Vec3 eye = {0.0f, 1.5f, 1.5f};
    constexpr float CEILING = 3.14f, RAIL_Y = 3.00f, RAIL_Z = 1.20f;
    mesh.part = 0;
    StockExterior(mesh, StockVertices("EXTERIOR_GUNSHIP_DOOR"), StockCount("EXTERIOR_GUNSHIP_DOOR"), GUN_STOCK, -1);
    // The bay's ceiling of our own (run649: the exterior's roof is not drawn from inside, the sky showed through it):
    // plate under the hull's roof across the bay (its middle line at x -0.97, the doors' openings z -0.5..3.8, the bay
    // a little longer), ribs across it at the doors' frames.
    {
        constexpr float X0 = -2.55f, X1 = 0.62f, Z0 = -1.10f, Z1 = 4.40f;
        TiledQuad(mesh, {X0, CEILING - 0.005f, Z0}, {X1 - X0, 0, 0}, {0, 0, Z1 - Z0}, GUN_PLATE);
        for (float z = Z0 + 0.35f; z < Z1; z += 0.85f)
        {
            Box(mesh, MakeFrame({(X0 + X1) * 0.5f, CEILING - 0.05f, z}, {1, 0, 0}, {0, 1, 0}),
                {(X1 - X0) * 0.5f, 0.045f, 0.04f}, 0.012f, GUN_FRAME);
        }
        Quad(mesh, {-1.07f, CEILING - 0.10f, 0.4f}, {-0.87f, CEILING - 0.10f, 0.4f}, {-0.87f, CEILING - 0.10f, 2.8f},
             {-1.07f, CEILING - 0.10f, 2.8f}, GUN_LAMP);
    }
    // the rail across, its two hangers up to the ceiling
    const Vec3 rail[2] = {{-0.90f, RAIL_Y, RAIL_Z}, {0.58f, RAIL_Y, RAIL_Z}};
    Tube(mesh, rail, 2, 0.02f, 10, GUN_METAL);
    for (const float x : {-0.80f, 0.48f})
    {
        const Vec3 hanger[2] = {{x, RAIL_Y, RAIL_Z}, {x, CEILING + 0.02f, RAIL_Z}};
        Tube(mesh, hanger, 2, 0.015f, 8, GUN_FRAME);
        Box(mesh, MakeFrame({x, CEILING - 0.01f, RAIL_Z}, {1, 0, 0}, {0, 1, 0}), {0.05f, 0.012f, 0.05f}, 0.004f,
            GUN_FRAME);
    }
    // the straps: a flat band from the rail down to each handle, the handle upright in the fist (its middle the grip)
    for (int side = -1; side <= 1; side += 2)
    {
        const float x = side * GUNSHIP_DOOR_GRIP[0];
        const float top = GUNSHIP_DOOR_GRIP[1] + 0.09f;
        Box(mesh, MakeFrame({x, (RAIL_Y + top) * 0.5f, RAIL_Z}, {1, 0, 0}, {0, 1, 0}),
            {0.018f, (RAIL_Y - top) * 0.5f, 0.004f}, 0.002f, GUN_SEAT);
        Box(mesh, MakeFrame({x, top, RAIL_Z}, {1, 0, 0}, {0, 1, 0}), {0.045f, 0.012f, 0.012f}, 0.004f, GUN_DARK);
        const Vec3 handle[2] = {{x, GUNSHIP_DOOR_GRIP[1] - 0.08f, RAIL_Z}, {x, top - 0.01f, RAIL_Z}};
        Tube(mesh, handle, 2, 0.017f, 10, GUN_DARK);
        Box(mesh, MakeFrame({x, GUNSHIP_DOOR_GRIP[1] - 0.085f, RAIL_Z}, {1, 0, 0}, {0, 1, 0}), {0.03f, 0.008f, 0.012f},
            0.003f, GUN_DARK);
    }
    // the grab bar up the door's front edge (just inside the opening, 0.56 right and 1.9 m ahead of his eye, stood off the frame)
    const Vec3 bar[2] = {{0.56f, 0.55f, -0.40f}, {0.56f, 2.55f, -0.40f}};
    Tube(mesh, bar, 2, 0.018f, 10, GUN_METAL);
    for (const float y : {0.55f, 2.55f})
    {
        const Vec3 stand[2] = {{0.56f, y, -0.40f}, {0.64f, y, -0.40f}};
        Tube(mesh, stand, 2, 0.015f, 8, GUN_FRAME);
    }
    (void)eye;
}

// ------------------------------------------------------------------------------------------------------------- MAF
// The Separatist MAF (the vehicle proposal 2026-11-04: Forefront-style helicopter controls; the pilot and two
// stations): the game's first-person cockpit (cis_1st_cockpit_MAF: a large curved room, the oval window ahead, the
// grated floor, machinery along the left wall) closed into the whole room (right wall, roof, back), the pilot on a
// dais before the window with the helicopter stick and the collective, the two gunners (TURRET1 the top camera gun,
// TURRET2 the rear cannon) on swivel stations either side behind him (idren2023: gunners as crew chairs in the pilot's
// cabin). One design per seat: in the pilot's every station stands still (part 0); in a gunner's, his own is parts
// 1 to 5 (SPEEDER_CABIN_PARTS, turned and pitched with his turret) and the other stands still. Cockpit model space
// (x right, y up, z back); the pilot's eye 1.5 m nearer the window than the game's (0, 1.5, 1.5). Finishes from the
// cockpit's own texture (cis_1st_cockpit_MAF.tga, 512 px).
enum MafFinish
{
    MAF_PLATE,   // grey plate (the texture's plain top left)
    MAF_GRATE,   // the big grille
    MAF_MACHINE, // the machinery block
    MAF_SCOPE,   // the green scope (lit)
    MAF_PIPES,   // the pipe band
    MAF_PANEL,   // the panelled strip (bottom left)
    MAF_FRAME,   // dark frames
    MAF_DARK,    // recesses
    MAF_METAL,   // pipes, rails, droid seats
    MAF_LAMP,    // red lamps (lit)
    MAF_PICTURE, // a gunner's display: the scope pass's picture
    MAF_SCREEN_FRAME,   // the HUD screens' stock frame (texture 5, cis_1st_cockpit_securitycam: the tanks' look)
    MAF_SCREEN_GLASS,   //   their glass (scan lines)
    MAF_SCREEN_BACKING, //   behind the glass
    MAF_FINISH_COUNT
};

const Finish MAF_FINISHES[MAF_FINISH_COUNT] = {
    {{0.000f, 0.000f, 0.450f, 0.100f}, 1.6f, 1, 0xA8A49C, 0.12f, 12, 0.04f}, // plate (0..230, 0..51; as the stock)
    {{0.700f, 0.300f, 0.990f, 0.680f}, 0.7f, 1, 0xC8C4BC, 0.25f, 18, 0},     // grille (358..507, 154..348)
    {{0.100f, 0.400f, 0.450f, 0.720f}, 0.9f, 1, 0xD0CCC4, 0.20f, 14, 0.1f},  // machinery (51..230, 205..369)
    {{0.460f, 0.660f, 0.560f, 0.760f}, 0.14f, 1, 0xB0B0B0, 0.20f, 18, 0.5f}, // scope (236..287, 338..389)
    {{0.350f, 0.800f, 1.000f, 0.900f}, 1.6f, 1, 0xD0CCC4, 0.25f, 18, 0},     // pipes (179..512, 410..461)
    {{0.000f, 0.830f, 0.350f, 0.990f}, 0.7f, 1, 0xD0CCC4, 0.20f, 14, 0},     // panels (0..179, 425..507)
    {{0.000f, 0.000f, 0.450f, 0.100f}, 1.2f, 1, 0x86827C, 0.20f, 16, 0},     // dark frames
    {{0, 0, 1, 1}, 1, 0, 0x0E0E0E, 0.05f, 6, 0},                             // recesses
    {{0, 0, 1, 1}, 1, 0, 0xA8A4A0, 0.45f, 30, 0},                            // metal
    {{0, 0, 1, 1}, 1, 0, 0xFF3020, 0, 1, 0.9f},                              // red lamps
    {{0, 0.3f, 1, 0.7f}, 1, 6, 0xFFFFFF, 0, 1, 1.0f},                        // the display's picture
    ScreenFrameFinish(),                                                    // stock screen frame
    ScreenGlassFinish(CIS_SCREEN_ART),                                      // its glass
    ScreenBackingFinish(CIS_SCREEN_ART),                                    // behind the glass
};

// The HUD screens in the tanks' look (idren2023 2026-11-09: every vehicle's HUD as the tanks').
const StockScreenLook MAF_SCREENS = {&CIS_SCREEN_ART, MAF_SCREEN_FRAME, MAF_SCREEN_GLASS, MAF_SCREEN_BACKING, MAF_DARK};

constexpr Vec3 MAF_EYE = {0.0f, 1.5f, 0.0f};
constexpr Vec3 MAF_TOP_EYE = {2.10f, 1.5f, 0.60f}; // TURRET1's gunner, right behind the pilot
// TURRET2's, behind the pilot to his right (2026-11-07: the stock window wall runs diagonally across the room, x -2.10
// z 1.80 to x 4.90 z -4.30 at its back face; at (-1.25, 0.90) the eye sat 0.12 m inside it and the wall cut the
// display: the "stray plane"); his display then clears the wall all the way round.
constexpr Vec3 MAF_REAR_EYE = {0.70f, 1.5f, 1.20f};
// The window wall's back face (the stock model's, plan x / z): from its left end, the room on the +x / +z side.
constexpr float MAF_WALL_X0 = -2.10f, MAF_WALL_Z0 = 1.80f, MAF_WALL_X1 = 4.90f, MAF_WALL_Z1 = -4.30f;
// The display on its rails this far out (idren2023, headset run 19: the screens too close), its glass grown with it so
// that it spans the same angle the periscope's picture is made for (GunnerSeat: SPEEDER_PERISCOPE_* at 0.52 m).
constexpr float MAF_DISPLAY_DISTANCE = 0.68f;

// A point on the window wall's back face `t` metres from its left end, `y` up, `out` metres further toward the window
// (into the wall: our closing pieces sit just inside it).
Vec3 MafWallPoint(float t, float y, float out = 0.0f)
{
    const float dx = MAF_WALL_X1 - MAF_WALL_X0, dz = MAF_WALL_Z1 - MAF_WALL_Z0;
    const float length = sqrtf(dx * dx + dz * dz), ux = dx / length, uz = dz / length;
    return {MAF_WALL_X0 + ux * t + uz * out, y, MAF_WALL_Z0 + uz * t - ux * out};
}

// Where the window wall's back face crosses a line of constant z (plan x).
float MafWallX(float z)
{
    return MAF_WALL_X0 + (z - MAF_WALL_Z0) * (MAF_WALL_X1 - MAF_WALL_X0) / (MAF_WALL_Z1 - MAF_WALL_Z0);
}

// A droid gunner's swivel station at `eye` over `floorY`: when `isMoving`, parts 1 (chair, column, grips, rails), 2
// (display), 3 and 4 (stop lamps), 5 (picture), else all part 0 at rest (facing ahead).
void MafStation(Mesh& mesh, Vec3 eye, float floorY, bool isMoving)
{
    const float rail = MAF_DISPLAY_DISTANCE, grow = MAF_DISPLAY_DISTANCE / SPEEDER_PERISCOPE_DISTANCE_METERS;
    const float halfWidth = SPEEDER_PERISCOPE_HALF_WIDTH_METERS * grow;
    const float halfHeight = SPEEDER_PERISCOPE_HALF_HEIGHT_METERS * grow;
    const auto part = [&](int number) { mesh.part = static_cast<uint8_t>(isMoving ? number : 0); };
    mesh.part = 0;
    {
        const float base[3][2] = {{0, 0.42f}, {0.05f, 0.42f}, {0.06f, 0.38f}};
        Lathe(mesh, MakeFrame({eye.x, floorY, eye.z}, {1, 0, 0}, {0, 1, 0}), base, 3, 28, MAF_FRAME);
    }
    part(1);
    {
        const SeatLook seat = {0.22f, 0.30f, 0.12f, false, true, MAF_METAL, MAF_FRAME, MAF_METAL, MAF_METAL, MAF_DARK};
        Seat(mesh, eye, floorY + 0.06f, seat);
        const Vec3 column[2] = {{eye.x, floorY + 0.06f, eye.z - 0.30f}, {eye.x, eye.y - 0.40f, eye.z - 0.30f}};
        Tube(mesh, column, 2, 0.035f, 12, MAF_METAL);
        const Frame head = MakeFrame({eye.x, eye.y - 0.38f, eye.z - 0.30f}, {1, 0, 0}, {0, 1, 0});
        Box(mesh, head, {0.10f, 0.05f, 0.05f}, 0.01f, MAF_FRAME);
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 arm[2] = {head.At(side * 0.10f, 0, 0), head.At(side * 0.20f, 0.02f, 0.03f)};
            Tube(mesh, arm, 2, 0.014f, 8, MAF_METAL);
            const Vec3 grip[2] = {head.At(side * 0.20f, 0.02f, 0.03f), head.At(side * 0.20f, 0.14f, 0.03f)};
            Tube(mesh, grip, 2, 0.0175f, 10, MAF_DARK);
            Vec3 arc[11];
            for (int k = 0; k <= 10; ++k)
            {
                const float a = (-45.0f + k * 9.0f) * 0.0174533f;
                arc[k] = {eye.x + side * (halfWidth + 0.05f), eye.y + rail * sinf(a), eye.z - rail * cosf(a)};
            }
            Tube(mesh, arc, 11, 0.01f, 6, MAF_FRAME, false);
        }
    }
    part(2);
    const Frame face = MakeFrame({eye.x, eye.y, eye.z - rail}, {1, 0, 0}, {0, 1, 0});
    {
        Box(mesh, Offset(face, 0, 0, -0.03f), {halfWidth + 0.035f, halfHeight + 0.035f, 0.02f}, 0.006f, MAF_FRAME);
        // the ammunition on a stock strip under the display, the readouts in the screens' colour (idren2023 2026-11-09)
        StockScreen(mesh, Offset(face, 0, -halfHeight - 0.045f * grow, 0.0f), 0.065f * grow, 0.016f * grow,
                    0.00008f * grow, MAF_SCREENS, 0.006f);
        SetDisplayScreen(&MAF_SCREENS);
        if (isMoving)
        {
            AddDisplay(
                GROUP_TARGET,
                face,
                -halfWidth + 0.05f * grow,
                halfHeight - 0.012f * grow,
                0.08f * grow,
                0.016f * grow,
                2);
            AddDisplay(
                GROUP_AMMUNITION,
                Offset(face, 0, -halfHeight - 0.045f * grow, 0.0f),
                0,
                0,
                0.10f * grow,
                0.022f * grow,
                2);
        }
        SetDisplayScreen(nullptr);
    }
    for (int side = 1; side >= -1; side -= 2)
    {
        part(side > 0 ? 3 : 4);
        Box(mesh, Offset(face, side * (halfWidth + 0.05f), halfHeight + 0.02f, -0.01f), {0.012f, 0.012f, 0.008f}, 0.003f,
            MAF_LAMP);
    }
    part(5);
    Quad(mesh, face.At(-halfWidth, halfHeight, -0.002f), face.At(halfWidth, halfHeight, -0.002f),
         face.At(halfWidth, -halfHeight, -0.002f), face.At(-halfWidth, -halfHeight, -0.002f),
         isMoving ? MAF_PICTURE : MAF_DARK);
    mesh.part = 0;
}

// `seat`: 0 the pilot's design, 1 TURRET1's, 2 TURRET2's.
void BuildMafSeat(Mesh& mesh, int seat)
{
    // The stock room (tools/shelltour.py, 2026-11-07): an alcove only, set diagonally in our space: the oval window in
    // its far wall, grated floor, ceiling and sloped sides round it, 1.83 m deep, its open mouth (a hexagon 9.3 m
    // across, y -0.49..3.47) on the window wall's back face (MafWallPoint). Our room is the rest: the triangle between
    // that face, the right wall and the back wall, closed over and round the mouth (the stray planes and the gaps at
    // the right and over the top came from a box that ignored the diagonal: its left wall, roof and deck stood outside
    // the window, and the corners above the mouth's slanted sides were open).
    constexpr float XR = 4.95f, FLOOR = -0.50f, ROOF = 3.50f, BACK = 1.85f;
    constexpr float DECK = 0.30f; // the crew's deck over the grated floor: eyes 1.2 m over it
    const float backLeft = MafWallX(BACK); // where the back wall meets the window wall
    const float rightFront =
        MAF_WALL_Z0 + (XR - MAF_WALL_X0) * (MAF_WALL_Z1 - MAF_WALL_Z0) / (MAF_WALL_X1 - MAF_WALL_X0);
    const float tBack = -0.06f, tRight = 9.35f; // the mouth's face from the back wall to the right wall
    // the mouth's outline on the face (t along it, y): over the top, and under it down to the deck either side
    constexpr float TOP[6][2] =
        {{0.0f, 0.90f}, {2.17f, 3.24f}, {4.64f, 3.47f}, {7.10f, 3.24f}, {8.19f, 2.07f}, {9.28f, 0.90f}};
    constexpr float LOW_LEFT[3][2] = {{0.0f, 0.90f}, {0.72f, 0.49f}, {1.05f, DECK}};
    constexpr float LOW_RIGHT[3][2] = {{8.24f, DECK}, {8.53f, 0.47f}, {9.28f, 0.90f}};
    const Vec3 along = Normalized(Vec3{MAF_WALL_X1 - MAF_WALL_X0, 0, MAF_WALL_Z1 - MAF_WALL_Z0});
    // Walls: right, back, the roof over the triangle, the face round the mouth.
    TiledQuad(mesh, {XR, FLOOR, rightFront}, {0, 0, BACK - rightFront}, {0, ROOF - FLOOR, 0}, MAF_PLATE);
    TiledQuad(mesh, {backLeft, FLOOR, BACK}, {XR - backLeft, 0, 0}, {0, ROOF - FLOOR, 0}, MAF_PLATE);
    {
        const Vec3 roof[3] = {{backLeft, ROOF, BACK}, {XR, ROOF, rightFront}, {XR, ROOF, BACK}};
        PlanarPolygon(mesh, roof, 3, {1, 0, 0}, MAF_PLATE);
    }
    {
        const auto fill = [&](float ta, float ya, float tb, float yb, float edge)
        {
            const Vec3 quad[4] = {
                MafWallPoint(ta, ya, -0.01f),
                MafWallPoint(tb, yb, -0.01f),
                MafWallPoint(tb, edge, -0.01f),
                MafWallPoint(ta, edge, -0.01f)};
            PlanarPolygon(mesh, quad, 4, along, MAF_FRAME);
        };
        fill(tBack, 0.90f, 0.0f, 0.90f, ROOF);
        fill(tBack, DECK - 0.02f, 0.0f, DECK - 0.02f, 0.90f);
        for (int k = 0; k + 1 < 6; ++k)
        {
            fill(TOP[k][0], TOP[k][1], TOP[k + 1][0], TOP[k + 1][1], ROOF);
        }
        fill(9.28f, 0.90f, tRight, 0.90f, ROOF);
        fill(9.28f, DECK - 0.02f, tRight, DECK - 0.02f, 0.90f);
        for (int k = 0; k + 1 < 3; ++k)
        {
            fill(LOW_LEFT[k][0], LOW_LEFT[k][1], LOW_LEFT[k + 1][0], LOW_LEFT[k + 1][1], DECK - 0.02f);
            fill(LOW_RIGHT[k][0], LOW_RIGHT[k][1], LOW_RIGHT[k + 1][0], LOW_RIGHT[k + 1][1], DECK - 0.02f);
        }
        // a heavy moulding round the mouth, standing into the room
        Vec3 rim[9];
        int rimCount = 0;
        rim[rimCount++] = MafWallPoint(LOW_LEFT[2][0], DECK + 0.03f, -0.04f);
        rim[rimCount++] = MafWallPoint(LOW_LEFT[1][0], LOW_LEFT[1][1], -0.04f);
        for (int k = 0; k < 6; ++k)
        {
            rim[rimCount++] = MafWallPoint(TOP[k][0], TOP[k][1], -0.04f);
        }
        rim[rimCount++] = MafWallPoint(LOW_RIGHT[1][0], LOW_RIGHT[1][1], -0.04f);
        Tube(mesh, rim, rimCount, 0.045f, 10, MAF_FRAME);
        // ribs on the face from the mouth up to the roof
        for (const float t : {0.9f, 3.4f, 5.9f, 8.6f})
        {
            int k = 0;
            while (k + 2 < 6 && TOP[k + 1][0] < t)
            {
                ++k;
            }
            const float y = TOP[k][1] + (TOP[k + 1][1] - TOP[k][1]) * (t - TOP[k][0]) / (TOP[k + 1][0] - TOP[k][0]);
            if (y < ROOF - 0.15f)
            {
                Box(mesh,
                    MakeFrame(MafWallPoint(t, (y + ROOF) * 0.5f, -0.05f), along, {0, 1, 0}),
                    {0.06f, (ROOF - y) * 0.5f, 0.04f},
                    0.015f,
                    MAF_FRAME);
            }
        }
    }
    // Ribs across the roof and up the right and back walls.
    for (float z = -3.6f; z < BACK - 0.3f; z += 1.2f)
    {
        const float x0 = MafWallX(z) + 0.05f;
        Box(mesh,
            MakeFrame({(x0 + XR) * 0.5f, ROOF - 0.07f, z}, {1, 0, 0}, {0, 1, 0}),
            {(XR - x0) * 0.5f, 0.07f, 0.06f},
            0.02f,
            MAF_FRAME);
        Box(mesh,
            MakeFrame({XR - 0.05f, (FLOOR + ROOF) * 0.5f, z}, {0, 0, 1}, {0, 1, 0}),
            {0.06f, (ROOF - FLOOR) * 0.5f, 0.05f},
            0.02f,
            MAF_FRAME);
    }
    for (float x = -1.2f; x < XR - 0.3f; x += 1.2f)
    {
        Box(mesh,
            MakeFrame({x, (FLOOR + ROOF) * 0.5f, BACK - 0.05f}, {1, 0, 0}, {0, 1, 0}),
            {0.06f, (ROOF - FLOOR) * 0.5f, 0.05f},
            0.02f,
            MAF_FRAME);
    }
    // The deck over the room and a ledge into the alcove under the pilot (his stick and collective stand on it), its
    // front a pipe band down to the grating.
    {
        const Vec3 room[3] = {{backLeft, DECK, BACK}, {XR, DECK, rightFront}, {XR, DECK, BACK}};
        PlanarPolygon(mesh, room, 3, {1, 0, 0}, MAF_PANEL);
        const Vec3 ledge[4] = {
            MafWallPoint(LOW_LEFT[2][0], DECK, -0.01f),
            MafWallPoint(LOW_LEFT[2][0], DECK, 0.45f),
            MafWallPoint(LOW_RIGHT[0][0], DECK, 0.45f),
            MafWallPoint(LOW_RIGHT[0][0], DECK, -0.01f)};
        PlanarPolygon(mesh, ledge, 4, {1, 0, 0}, MAF_PANEL);
        const Vec3 front[4] = {
            MafWallPoint(LOW_LEFT[2][0], DECK, 0.45f),
            MafWallPoint(LOW_RIGHT[0][0], DECK, 0.45f),
            MafWallPoint(6.87f, FLOOR + 0.01f, 0.45f),
            MafWallPoint(2.42f, FLOOR + 0.01f, 0.45f)};
        PlanarPolygon(mesh, front, 4, along, MAF_PIPES);
        const Vec3 edge[2] = {
            MafWallPoint(LOW_LEFT[2][0] + 0.05f, DECK + 0.015f, 0.45f),
            MafWallPoint(LOW_RIGHT[0][0] - 0.05f, DECK + 0.015f, 0.45f)};
        Tube(mesh, edge, 2, 0.02f, 8, MAF_FRAME);
    }
    // The back wall: the grille in the middle, machinery either side, a lamp row, a cable run along its top.
    Panel(
        mesh,
        MakeFrame({1.40f, 1.70f, BACK - 0.02f}, {-1, 0, 0}, {0, 1, 0}),
        0.80f,
        0.70f,
        0.05f,
        0.03f,
        MAF_FRAME,
        MAF_GRATE,
        0.10f,
        MAF_METAL);
    for (const float x : {-0.75f, 3.60f})
    {
        Panel(
            mesh,
            MakeFrame({x, 1.40f, BACK - 0.02f}, {-1, 0, 0}, {0, 1, 0}),
            0.55f,
            0.45f,
            0.05f,
            0.03f,
            MAF_FRAME,
            MAF_MACHINE,
            0,
            MAF_METAL);
    }
    for (int k = -4; k <= 4; ++k)
    {
        Box(mesh,
            MakeFrame({1.40f + k * 0.30f, 2.75f, BACK - 0.03f}, {1, 0, 0}, {0, 1, 0}),
            {0.04f, 0.02f, 0.01f},
            0.004f,
            MAF_LAMP);
    }
    {
        const Vec3 cable[3] = {
            {backLeft + 0.3f, 3.10f, BACK - 0.10f},
            {2.0f, 3.12f, BACK - 0.10f},
            {XR - 0.15f, 3.10f, BACK - 0.10f}};
        CableLoom(mesh, cable, 3, 3, 0.018f, {0, 1, 0}, MAF_DARK, MAF_FRAME, 0.6f);
    }
    // The right wall: machinery, the scope, pipe runs along it.
    Panel(mesh, MakeFrame({XR - 0.02f, 1.50f, -1.50f}, {0, 0, -1}, {0, 1, 0}), 1.10f, 0.55f, 0.05f, 0.03f, MAF_FRAME,
          MAF_MACHINE, 0.12f, MAF_METAL);
    Panel(mesh, MakeFrame({XR - 0.02f, 1.60f, 0.40f}, {0, 0, -1}, {0, 1, 0}), 0.16f, 0.16f, 0.03f, 0.02f, MAF_FRAME,
          MAF_SCOPE, 0, MAF_METAL);
    {
        const Vec3 pipe[2] = {{XR - 0.14f, 2.85f, rightFront + 0.30f}, {XR - 0.14f, 2.85f, BACK - 0.15f}};
        FlangedPipe(mesh, pipe, 2, 0.05f, {1, 0, 0}, 0.9f, MAF_PIPES, MAF_FRAME);
        const Vec3 low[2] = {{XR - 0.12f, 0.55f, rightFront + 0.30f}, {XR - 0.12f, 0.55f, BACK - 0.15f}};
        Tube(mesh, low, 2, 0.03f, 10, MAF_PIPES);
    }
    // The pilot: a droid seat on the deck; the stick's pedestal (Flight: the eye + (0, -0.86, -0.34)); the collective's
    // console on his left (its pivot the eye + (-0.27, -0.70, -0.05)), its slot.
    const SeatLook look = {0.22f, 0.30f, 0.12f, false, true, MAF_METAL, MAF_FRAME, MAF_METAL, MAF_METAL, MAF_DARK};
    Seat(mesh, MAF_EYE, DECK, look);
    {
        const Vec3 base = {MAF_EYE.x, MAF_EYE.y - 0.86f, MAF_EYE.z - 0.34f};
        const float h = base.y - DECK;
        const float rings[4][2] = {{0, 0.09f}, {0.02f, 0.09f}, {0.03f, 0.07f}, {h, 0.07f}};
        Lathe(mesh, MakeFrame({base.x, DECK, base.z}, {1, 0, 0}, {0, 1, 0}), rings, 4, 18, MAF_FRAME);
        const Vec3 pivot = {MAF_EYE.x - 0.27f, MAF_EYE.y - 0.70f, MAF_EYE.z - 0.05f};
        Box(mesh, MakeFrame({pivot.x - 0.08f, (DECK + pivot.y) * 0.5f, pivot.z}, {1, 0, 0}, {0, 1, 0}),
            {0.14f, (pivot.y - DECK) * 0.5f, 0.18f}, 0.02f, MAF_FRAME);
        Box(mesh, MakeFrame({pivot.x, pivot.y + 0.004f, pivot.z}, {1, 0, 0}, {0, 1, 0}), {0.014f, 0.004f, 0.12f}, 0.002f,
            MAF_DARK);
    }
    MafStation(mesh, MAF_TOP_EYE, DECK, seat == 1);
    MafStation(mesh, MAF_REAR_EYE, DECK, seat == 2);
}

void BuildMaf(Mesh& mesh)
{
    BuildMafSeat(mesh, 0);
}

void BuildMafTop(Mesh& mesh)
{
    BuildMafSeat(mesh, 1);
}

void BuildMafRear(Mesh& mesh)
{
    BuildMafSeat(mesh, 2);
}

// The AT-TE pilot's cab in the walker's own nose (idren2023 2026-11-04: the stock pod had no floor and no window; the
// exterior shows the cab: a narrow cab between the two front laser domes, its viewport the sloped amber window down
// the middle of the nose, x +-0.42 from y 5.2 / z 7.45 at its foot to y 6.4 / z 6.56 at its top, the grille under it):
// the walker's exterior round the pilot (tools/stockexterior.py ATTE_CAB: his eye at the exterior's (0, 5.6, 6.0), the
// cab's inside cut out), amber glass in the window, our tub (floor, walls, roof), a hatch aft to the troop bay.
// Cockpit model space, his eye at (0, 1.5, 1.5).
// 2026-11-07 (idren2023: too dark and plain, the rear open at a side): a Republic cockpit: the walls lined with the stock
// cockpit's own riveted panels (the whole texture: its lamp column and instrument plate, as the stock pod's walls), a
// lit roof strip; the dash a sloped console with three of the Republic's stock screens (the HUD: health and score
// left, the map in the middle, the weapon's heat and the target right), switch banks and a blue-lit edge under them;
// an overhead switch panel over the window, a side console at the left knee; aft lockers, a cell rack, an extinguisher
// and grab handles by the hatch. The throttle console stays where WalkerDrive's lever is.
void BuildAtteCab(Mesh& mesh)
{
    // (the exterior's (x, y, z) is the cockpit's (-x, y - 4.1, 8.8 - z): the window across the nose at z 0.5)
    constexpr float HALF = 0.52f, FLOOR = 0.15f, ROOF = 2.60f, FRONT = 0.50f, BACK = 4.30f;
    // idren2023 2026-11-08 (his sketch over P8): the front window from the roof to below his feet, a footwell behind its
    // foot (the floor ends WELL_Z from the window), a grille across its lowest part
    constexpr float WX = 0.34f, WY0 = -0.40f, WY1 = 2.20f, WELL = -0.45f, WELL_Z = 0.85f;
    const Vec3 eye = {0.0f, 1.5f, 1.5f};
    const DetailLook look = {ATTE_PLATE, ATTE_FRAME, ATTE_PIPE, ATTE_DARK, ATTE_LAMP, ATTE_RED};
    StockExterior(mesh, StockVertices("EXTERIOR_ATTE_CAB"), StockCount("EXTERIOR_ATTE_CAB"), ATTE_STOCK, -1);
    // The window (amber, see-through) in the front wall, its heavy frame.
    Quad(mesh, {-WX, WY0, FRONT}, {WX, WY0, FRONT}, {WX, WY1, FRONT}, {-WX, WY1, FRONT}, ATTE_WINDOW);
    AtstWallWithHole(mesh, FRONT, -HALF, HALF, WELL, ROOF, -WX, WX, WY0, WY1, ATTE_PLATE);
    for (int bar = 0; bar < 7; ++bar)
    {
        const float x = -WX + 2 * WX * static_cast<float>(bar + 1) / 8;
        Box(mesh, MakeFrame({x, WY0 + 0.25f, FRONT + 0.03f}, {1, 0, 0}, {0, 1, 0}), {0.012f, 0.25f, 0.015f}, 0.004f,
            ATTE_FRAME);
    }
    Box(mesh, MakeFrame({0, WY0 + 0.51f, FRONT + 0.03f}, {1, 0, 0}, {0, 1, 0}), {WX, 0.015f, 0.02f}, 0.004f, ATTE_FRAME);
    // the footwell: its floor, the floor's front edge (a riser with a lit lip), its side walls
    TiledQuad(mesh, {-HALF, WELL, FRONT}, {2 * HALF, 0, 0}, {0, 0, WELL_Z - FRONT}, ATTE_PLATE);
    TiledQuad(mesh, {-HALF, WELL, WELL_Z}, {2 * HALF, 0, 0}, {0, FLOOR - WELL, 0}, ATTE_FRAME);
    Quad(mesh, {-HALF, FLOOR - 0.012f, WELL_Z - 0.002f}, {HALF, FLOOR - 0.012f, WELL_Z - 0.002f},
         {HALF, FLOOR - 0.002f, WELL_Z - 0.002f}, {-HALF, FLOOR - 0.002f, WELL_Z - 0.002f}, ATTE_ACCENT);
    for (const float side : {-1.0f, 1.0f})
    {
        TiledQuad(mesh, {side * HALF, WELL, FRONT}, {0, 0, WELL_Z - FRONT}, {0, FLOOR - WELL, 0}, ATTE_PLATE);
    }
    for (const float y : {WY0 - 0.04f, WY1 + 0.04f})
    {
        Box(mesh, MakeFrame({0, y, FRONT + 0.05f}, {1, 0, 0}, {0, 1, 0}), {WX + 0.06f, 0.04f, 0.05f}, 0.012f, ATTE_FRAME);
    }
    for (const float x : {-WX - 0.04f, WX + 0.04f})
    {
        Box(mesh, MakeFrame({x, (WY0 + WY1) * 0.5f, FRONT + 0.05f}, {1, 0, 0}, {0, 1, 0}),
            {0.04f, (WY1 - WY0) * 0.5f + 0.06f, 0.05f}, 0.012f, ATTE_FRAME);
    }
    for (int bolt = 0; bolt < 9; ++bolt)
    {
        const float y = WY0 + (WY1 - WY0) * static_cast<float>(bolt) / 8;
        for (const float x : {-WX - 0.04f, WX + 0.04f})
        {
            Rivet(mesh, MakeFrame({x, y, FRONT + 0.101f}, {1, 0, 0}, {0, 1, 0}), 0.008f, ATTE_PIPE);
        }
    }
    // The tub: floor (a tread plate down the walkway aft), roof with ribs and a lit strip.
    TiledQuad(mesh, {-HALF, FLOOR, WELL_Z}, {2 * HALF, 0, 0}, {0, 0, BACK - WELL_Z}, ATTE_PLATE);
    TreadPlate(mesh, {-0.30f, FLOOR + 0.004f, 2.05f}, {0.60f, 0, 0}, {0, 0, 2.10f}, 0.08f, ATTE_FRAME, ATTE_PIPE);
    TiledQuad(mesh, {-HALF, ROOF, FRONT}, {2 * HALF, 0, 0}, {0, 0, BACK - FRONT}, ATTE_PLATE);
    for (float z = 1.00f; z < BACK; z += 0.95f)
    {
        Box(mesh, MakeFrame({0, ROOF - 0.05f, z}, {1, 0, 0}, {0, 1, 0}), {HALF, 0.05f, 0.04f}, 0.015f, ATTE_FRAME);
    }
    Box(mesh, MakeFrame({0, ROOF - 0.03f, 3.00f}, {1, 0, 0}, {0, 1, 0}), {0.09f, 0.03f, 1.05f}, 0.01f, ATTE_FRAME);
    Quad(
        mesh,
        {-0.06f, ROOF - 0.062f, 2.00f},
        {0.06f, ROOF - 0.062f, 2.00f},
        {0.06f, ROOF - 0.062f, 4.00f},
        {-0.06f, ROOF - 0.062f, 4.00f},
        ATTE_LAMP);
    // The side walls: plate behind, lined with the stock panels (two rows, three bays) between ribs, a slotted kick
    // strip under them, the riveted rail along the top.
    // 2026-11-08 (idren2023; the "Inside the AT-TE" walkthrough, re/reference/atte-video P1-P10): a triangular window in
    // each side wall beside the front one (vertical at the front, level at the top), lamp panels under them; the
    // panelled bays start behind the windows.
    constexpr float TRI_Z0 = FRONT + 0.05f, TRI_Z1 = 2.45f, TRI_Y0 = 0.30f, TRI_Y1 = 2.35f; // its slant past the headrest
    constexpr float BAYS[4] = {2.50f, 3.08f, 3.66f, 4.26f};
    constexpr float ROWS[3] = {0.55f, 1.50f, 2.45f};
    for (int side = -1; side <= 1; side += 2)
    {
        const float s = static_cast<float>(side), x = s * HALF, inner = s * (HALF - 0.006f);
        TiledQuad(mesh, {x, FLOOR, TRI_Z1}, {0, 0, BACK - TRI_Z1}, {0, ROOF - FLOOR, 0}, ATTE_PLATE);
        TiledQuad(mesh, {x, FLOOR, FRONT}, {0, 0, TRI_Z1 - FRONT}, {0, TRI_Y0 - FLOOR, 0}, ATTE_PLATE);
        TiledQuad(mesh, {x, TRI_Y1, FRONT}, {0, 0, TRI_Z1 - FRONT}, {0, ROOF - TRI_Y1, 0}, ATTE_PLATE);
        TiledQuad(mesh, {x, TRI_Y0, FRONT}, {0, 0, TRI_Z0 - FRONT}, {0, TRI_Y1 - TRI_Y0, 0}, ATTE_PLATE);
        {
            const Vec3 plate[3] = {{x, TRI_Y0, TRI_Z0}, {x, TRI_Y0, TRI_Z1}, {x, TRI_Y1, TRI_Z1}};
            PlanarPolygon(mesh, plate, 3, {0, 0, 1}, ATTE_PLATE); // by real size (the whole patch had been stretched)
            const float glassSt[3][2] = {{0, 1}, {1, 0}, {0, 0}};
            Triangle(mesh, {x, TRI_Y0, TRI_Z0}, {x, TRI_Y1, TRI_Z1}, {x, TRI_Y1, TRI_Z0}, glassSt, ATTE_WINDOW);
            const float in = x - s * 0.03f;
            const auto bar = [&](const Vec3& from, const Vec3& to)
            {
                const Vec3 along = to - from;
                Box(mesh, MakeFrame((from + to) * 0.5f, Normalized(along), {1, 0, 0}),
                    {Length(along) * 0.5f + 0.03f, 0.035f, 0.03f}, 0.01f, ATTE_FRAME);
            };
            bar({in, TRI_Y0, TRI_Z0}, {in, TRI_Y1, TRI_Z0});
            bar({in, TRI_Y1, TRI_Z0}, {in, TRI_Y1, TRI_Z1});
            bar({in, TRI_Y0, TRI_Z0}, {in, TRI_Y1, TRI_Z1});
            // the lamp panel under the window: two rows of red and blue lamps on a dark plate (P5, P6)
            const Frame panel = MakeFrame({s * (HALF - 0.012f), 0.98f, 2.05f}, {0, 0, s}, {0, 1, 0});
            Box(mesh, Offset(panel, 0, 0, -0.006f), {0.44f, 0.11f, 0.006f}, 0.004f, ATTE_DARK);
            for (int row = 0; row < 2; ++row)
            {
                for (int lamp = 0; lamp < 9; ++lamp)
                {
                    const bool isRed = (lamp + row) % 3 == 0;
                    Box(mesh, Offset(panel, -0.38f + 0.095f * static_cast<float>(lamp), 0.035f - 0.07f * row, 0.004f),
                        {0.014f, 0.010f, 0.004f}, 0.002f, isRed ? ATTE_RED : ATTE_ACCENT);
                }
            }
            // a red lamp high in the front corner (P2)
            Box(mesh, MakeFrame({s * (HALF - 0.05f), ROOF - 0.12f, FRONT + 0.08f}, {1, 0, 0}, {0, 1, 0}),
                {0.03f, 0.03f, 0.03f}, 0.006f, ATTE_RED);
        }
        for (int bay = 0; bay < 3; ++bay)
        {
            // seen from inside: the texture's left edge toward the back on the left wall, toward the front on the right
            const float zLeft = side < 0 ? BAYS[bay + 1] : BAYS[bay], zRight = side < 0 ? BAYS[bay] : BAYS[bay + 1];
            for (int row = 0; row < 2; ++row)
            {
                Quad(
                    mesh,
                    {inner, ROWS[row + 1], zLeft},
                    {inner, ROWS[row + 1], zRight},
                    {inner, ROWS[row], zRight},
                    {inner, ROWS[row], zLeft},
                    ATTE_PANEL);
            }
        }
        for (const float z : BAYS)
        {
            Box(mesh,
                MakeFrame({s * (HALF - 0.02f), (FLOOR + ROOF) * 0.5f, z}, {0, 0, 1}, {0, 1, 0}),
                {0.03f, (ROOF - FLOOR) * 0.5f, 0.025f},
                0.01f,
                ATTE_FRAME);
        }
        TiledQuad(
            mesh,
            {s * (HALF - 0.008f), FLOOR, BAYS[0]},
            {0, 0, BAYS[3] - BAYS[0]},
            {0, ROWS[0] - FLOOR, 0},
            ATTE_SLOT);
        Box(mesh,
            MakeFrame({s * (HALF - 0.03f), ROOF - 0.10f, (TRI_Z1 + BACK) * 0.5f}, {0, 0, 1}, {0, 1, 0}),
            {(BACK - TRI_Z1) * 0.5f, 0.03f, 0.03f},
            0.01f,
            ATTE_RIVETS);
        // a blue-lit strip along the wall at the panels' foot
        Quad(
            mesh,
            {s * (HALF - 0.012f), ROWS[0] + 0.012f, BAYS[0]},
            {s * (HALF - 0.012f), ROWS[0] + 0.012f, BAYS[3]},
            {s * (HALF - 0.012f), ROWS[0] + 0.024f, BAYS[3]},
            {s * (HALF - 0.012f), ROWS[0] + 0.024f, BAYS[0]},
            ATTE_ACCENT);
    }
    // No dash across the window (it reaches down to his knees: his legs are seen through it, P3 / P8): two pods in
    // the front corners beside it carry the stock screens turned to the eye (left: health and score, the map over
    // it on an arm; right: ammunition and target), a blue-lit edge and switches on the right pod.
    {
        for (const float side : {-1.0f, 1.0f})
        {
            Box(mesh, MakeFrame({side * 0.42f, (WELL + 0.90f) * 0.5f, FRONT + 0.22f}, {1, 0, 0}, {0, 1, 0}),
                {0.10f, (0.90f - WELL) * 0.5f, 0.22f}, 0.015f, ATTE_FRAME);
            Quad(mesh, {side * 0.32f, 0.86f, FRONT + 0.441f}, {side * 0.52f, 0.86f, FRONT + 0.441f},
                 {side * 0.52f, 0.875f, FRONT + 0.441f}, {side * 0.32f, 0.875f, FRONT + 0.441f}, ATTE_ACCENT);
        }
        Box(mesh, MakeFrame({-0.45f, 1.10f, FRONT + 0.12f}, {1, 0, 0}, {0, 1, 0}), {0.02f, 0.12f, 0.02f}, 0.005f,
            ATTE_PIPE);
        const Vec3 worldUp = {0, 1, 0};
        const StockScreenLook screens =
            {&REP_SCREEN_ART, ATTE_SCREEN_FRAME, ATTE_SCREEN_GLASS, ATTE_SCREEN_BACKING, ATTE_FRAME};
        for (int place = -1; place <= 1; ++place)
        {
            const float halfWidth = place ? 0.105f : 0.072f, halfHeight = place ? 0.068f : 0.072f;
            const Vec3 at = place < 0   ? Vec3{-0.40f, 0.98f, 0.80f}
                            : place > 0 ? Vec3{0.40f, 0.98f, 0.80f}
                                        : Vec3{-0.40f, 1.22f, 0.72f};
            const Vec3 normal = Normalized(eye - at);
            const Vec3 up = Normalized(worldUp - normal * Dot(worldUp, normal));
            const Frame face = MakeFrame(at, Cross(up, normal), up);
            StockScreen(mesh, face, halfWidth, halfHeight, SCREEN_TEXEL_METERS * halfHeight / 0.0675f, screens, 0.02f);
            if (place)
            {
                // A lit readout behind the glass (run656: the game draws no health, score, ammunition or target
                // readouts for the walker's pilot, only the map, so these two screens stayed dark).
                Quad(
                    mesh,
                    face.At(-halfWidth, halfHeight, -0.002f),
                    face.At(halfWidth, halfHeight, -0.002f),
                    face.At(halfWidth, -halfHeight, -0.002f),
                    face.At(-halfWidth, -halfHeight, -0.002f),
                    ATTE_READOUT);
            }
            SetDisplayScreen(&screens);
            if (place < 0)
            {
                AddDisplay(GROUP_HEALTH, face, 0, 0.020f, 0.19f, 0.080f);
                AddDisplay(GROUP_SCORE, face, 0, -0.042f, 0.17f, 0.030f);
            }
            else if (place > 0)
            {
                AddDisplay(GROUP_AMMUNITION, face, 0, 0.020f, 0.19f, 0.065f);
                AddDisplay(GROUP_TARGET, face, 0, -0.042f, 0.15f, 0.036f);
            }
            else
            {
                AddDisplay(GROUP_MAP, face, 0, 0, 0.135f, 0.135f);
            }
            SetDisplayScreen(nullptr);
        }
        const Frame desk = MakeFrame({0.42f, 0.902f, FRONT + 0.36f}, {1, 0, 0}, {0, 0, -1});
        ToggleBank(mesh, desk, 4, 0.035f, look);
    }
    // The overhead switch panel over the window, turned down to the eye; the side console at the left knee.
    {
        const Frame overhead = MakeFrame({0, 2.42f, 0.64f}, {1, 0, 0}, {0, 0.69f, 0.72f});
        Box(mesh, Offset(overhead, 0, 0, -0.04f), {0.34f, 0.12f, 0.04f}, 0.012f, ATTE_FRAME);
        ControlPanel(mesh, overhead, 0.31f, 0.10f, look);
        const Frame knee = MakeFrame({-0.40f, 0.86f, 1.60f}, {0, 0, -1}, {-0.45f, 1, 0});
        Box(mesh,
            MakeFrame({-0.40f, (FLOOR + 0.84f) * 0.5f, 1.60f}, {1, 0, 0}, {0, 1, 0}),
            {0.11f, (0.84f - FLOOR) * 0.5f, 0.24f},
            0.015f,
            ATTE_FRAME);
        Quad(
            mesh,
            knee.At(-0.22f, 0.10f, 0.003f),
            knee.At(0.22f, 0.10f, 0.003f),
            knee.At(0.22f, -0.10f, 0.003f),
            knee.At(-0.22f, -0.10f, 0.003f),
            ATTE_CONSOLE);
        ToggleBank(mesh, Offset(knee, 0, -0.06f, 0.004f), 5, 0.035f, look);
    }
    // Aft: a locker on the left, the cell rack and an extinguisher on the right, grab handles either side of the hatch.
    const DetailLook lockerLook = {ATTE_PLATE, ATTE_FRAME, ATTE_PIPE, ATTE_DARK, ATTE_LAMP, ATTE_SLOT}; // a plain placard
    Locker(mesh, MakeFrame({-HALF + 0.03f, 1.30f, 3.33f}, {0, 0, -1}, {0, 1, 0}), 0.36f, 0.62f, 0.10f, lockerLook);
    CellRack(mesh, MakeFrame({HALF - 0.03f, 1.70f, 3.33f}, {0, 0, 1}, {0, 1, 0}), 2, 3, look);
    Extinguisher(mesh, MakeFrame({HALF - 0.03f, 0.75f, 3.10f}, {0, 0, 1}, {0, 1, 0}), look);
    // The bulkhead aft with the hatch to the troop bay (dark), a lamp over it.
    {
        constexpr float DOOR = 0.30f, DOOR_TOP = 1.95f;
        TiledQuad(mesh, {-HALF, FLOOR, BACK}, {HALF - DOOR, 0, 0}, {0, ROOF - FLOOR, 0}, ATTE_PLATE);
        TiledQuad(mesh, {DOOR, FLOOR, BACK}, {HALF - DOOR, 0, 0}, {0, ROOF - FLOOR, 0}, ATTE_PLATE);
        TiledQuad(mesh, {-DOOR, DOOR_TOP, BACK}, {2 * DOOR, 0, 0}, {0, ROOF - DOOR_TOP, 0}, ATTE_PLATE);
        Quad(mesh, {-DOOR, FLOOR, BACK + 0.25f}, {DOOR, FLOOR, BACK + 0.25f}, {DOOR, DOOR_TOP, BACK + 0.25f},
             {-DOOR, DOOR_TOP, BACK + 0.25f}, ATTE_DARK);
        for (const float x : {-DOOR, DOOR})
        {
            TiledQuad(mesh, {x, FLOOR, BACK}, {0, 0, 0.25f}, {0, DOOR_TOP - FLOOR, 0}, ATTE_FRAME);
            Box(mesh,
                MakeFrame({x, (FLOOR + DOOR_TOP) * 0.5f, BACK - 0.03f}, {1, 0, 0}, {0, 1, 0}),
                {0.05f, (DOOR_TOP - FLOOR) * 0.5f + 0.05f, 0.03f},
                0.012f,
                ATTE_FRAME);
            GrabHandle(
                mesh,
                {x * 1.45f, 1.05f, BACK - 0.01f},
                {x * 1.45f, 1.55f, BACK - 0.01f},
                {0, 0, -1},
                0.05f,
                0.012f,
                ATTE_PIPE,
                ATTE_FRAME);
        }
        Box(mesh,
            MakeFrame({0, DOOR_TOP + 0.03f, BACK - 0.03f}, {1, 0, 0}, {0, 1, 0}),
            {DOOR + 0.05f, 0.05f, 0.03f},
            0.012f,
            ATTE_FRAME);
        TiledQuad(mesh, {-DOOR, DOOR_TOP, BACK}, {2 * DOOR, 0, 0}, {0, 0, 0.25f}, ATTE_FRAME);
        TiledQuad(mesh, {-DOOR, FLOOR, BACK}, {2 * DOOR, 0, 0}, {0, 0, 0.25f}, ATTE_FRAME);
        Quad(
            mesh,
            {-0.2f, 2.12f, BACK - 0.02f},
            {0.2f, 2.12f, BACK - 0.02f},
            {0.2f, 2.18f, BACK - 0.02f},
            {-0.2f, 2.18f, BACK - 0.02f},
            ATTE_LAMP);
    }
    // The roof hatch over the pilot (P2, P4): a framed square, its dark lid inset, a handle across it.
    {
        constexpr float HZ = 1.55f, HH = 0.30f;
        Quad(mesh, {-HH, ROOF - 0.004f, HZ - HH}, {HH, ROOF - 0.004f, HZ - HH}, {HH, ROOF - 0.004f, HZ + HH},
             {-HH, ROOF - 0.004f, HZ + HH}, ATTE_DARK);
        for (const float side : {-1.0f, 1.0f})
        {
            Box(mesh, MakeFrame({side * HH, ROOF - 0.03f, HZ}, {1, 0, 0}, {0, 1, 0}), {0.03f, 0.03f, HH + 0.03f}, 0.008f,
                ATTE_FRAME);
            Box(mesh, MakeFrame({0, ROOF - 0.03f, HZ + side * HH}, {1, 0, 0}, {0, 1, 0}), {HH + 0.03f, 0.03f, 0.03f},
                0.008f, ATTE_FRAME);
        }
        GrabHandle(mesh, {-0.15f, ROOF - 0.01f, HZ}, {0.15f, ROOF - 0.01f, HZ}, {0, -1, 0}, 0.05f, 0.012f, ATTE_PIPE,
                   ATTE_FRAME);
    }
    // The spotter's seat above and behind the pilot (P10): a raised platform aft with its seat, the rangefinder's
    // eyepiece block before his face, its tube up through the roof.
    {
        constexpr float TOP = 1.30f, Z0 = 2.35f, Z1 = 3.15f;
        Box(mesh, MakeFrame({0, (FLOOR + TOP) * 0.5f, (Z0 + Z1) * 0.5f}, {1, 0, 0}, {0, 1, 0}),
            {0.34f, (TOP - FLOOR) * 0.5f, (Z1 - Z0) * 0.5f}, 0.015f, ATTE_FRAME);
        TreadPlate(mesh, {-0.32f, TOP + 0.003f, Z0 + 0.02f}, {0.64f, 0, 0}, {0, 0, Z1 - Z0 - 0.04f}, 0.08f, ATTE_FRAME,
                   ATTE_PIPE);
        const Vec3 spotterEye = {0, TOP + 1.05f, (Z0 + Z1) * 0.5f + 0.05f};
        const SeatLook spotterSeat = {0.19f, 0.45f, 0.10f, false, false, ATTE_SEAT, ATTE_FRAME, ATTE_PIPE, ATTE_SEAT,
                                      ATTE_DARK};
        Seat(mesh, spotterEye, TOP, spotterSeat);
        Box(mesh, MakeFrame({0, spotterEye.y, spotterEye.z - 0.32f}, {1, 0, 0}, {0, 1, 0}), {0.13f, 0.06f, 0.07f}, 0.01f,
            ATTE_FRAME);
        for (const float side : {-0.05f, 0.05f})
        {
            Box(mesh, MakeFrame({side, spotterEye.y, spotterEye.z - 0.245f}, {1, 0, 0}, {0, 1, 0}),
                {0.022f, 0.022f, 0.01f}, 0.004f, ATTE_RED);
        }
        Box(mesh, MakeFrame({0, (spotterEye.y + ROOF) * 0.5f + 0.03f, spotterEye.z - 0.36f}, {1, 0, 0}, {0, 1, 0}),
            {0.05f, (ROOF - spotterEye.y) * 0.5f + 0.03f, 0.05f}, 0.01f, ATTE_PIPE);
    }
    // The pilot's seat and the throttle console under his right hand.
    const SeatLook seat = {0.21f, 0.55f, 0.12f, false, false, ATTE_SEAT, ATTE_FRAME, ATTE_PIPE, ATTE_SEAT, ATTE_DARK};
    Seat(mesh, eye, FLOOR, seat);
    ThrottleConsole(mesh, eye, FLOOR, ATTE_FRAME, ATTE_DARK, ATTE_ACCENT); // (cream lamps read as unfinished blocks)
}

constexpr Vec3 ATTE_CAB_EYE = {0.0f, 1.5f, 1.5f};

// The AT-TE's main cannon (idren2023 2026-11-04: the open-air turret on its back, head aimed, the view kept as the
// fighter tanks' gunner's while the walker moves): the gunner at hp_main_gunner (0, 7.1, 1.72) behind the cannon, the
// walker's own exterior round him (tools/stockexterior.py ATTE_CANNON, part 0). Cockpit model space, his eye at
// (0, 1.5, 1.5).
// 2026-11-07 (idren2023: the barrel hidden under the turret's hump, the curved screens not liked; a station as the
// fighter tanks' turrets got): our own gunner station in place of the stock turret (ATTE_GUN no longer drawn), turning
// with the turret's yaw about his eye (part 1: he stays where he is, the gun always before him): an armoured tub open
// above, its front low (the coaming 0.95 m under his eye) so the mantlet and the whole barrel show over it; inside it
// his seat, a yoke with two grips (GunnerSeat's grip places), and before it a sloped console with three Republic stock
// screens: the gun camera's picture in the middle (GunnerSeat Cabin viewscreen: the periscope pass along the shot),
// the player's and the walker's health left, the cannon's heat and the target right. Part 2: the barrel
// (tools/stockexterior.py ATTE_BARREL) in a round mantlet on its trunnion, pitching about it (GunnerSeat pitchPivot);
// the aiming line from its muzzle (GunnerSeat::GetDrawnMuzzle).
void BuildAtteCannon(Mesh& mesh)
{
    const Vec3 eye = {0.0f, 1.5f, 1.5f};
    const Vec3 trunnion = {ATTE_CANNON_TRUNNION[0], ATTE_CANNON_TRUNNION[1], ATTE_CANNON_TRUNNION[2]};
    const DetailLook look = {ATTE_PLATE, ATTE_FRAME, ATTE_PIPE, ATTE_DARK, ATTE_LAMP, ATTE_RED};
    mesh.part = 0;
    StockExterior(mesh, StockVertices("EXTERIOR_ATTE_CANNON"), StockCount("EXTERIOR_ATTE_CANNON"), ATTE_STOCK, -1);
    mesh.part = 1;
    // The tub: eight armoured sides in plan from the skirt (down into the hull's deck) up to the coaming, the outside
    // the walker's plating, the inside lined, a rolled cap between; low at the front, rising toward his back.
    constexpr float SKIRT = -0.60f, FLOOR = 0.20f, WALL = 0.06f;
    constexpr int SIDES = 8;
    const float plan[SIDES][2] = {
        {-0.45f, -0.72f},
        {0.45f, -0.72f},
        {0.70f, -0.35f},
        {0.70f, 1.95f},
        {0.45f, 2.35f},
        {-0.45f, 2.35f},
        {-0.70f, 1.95f},
        {-0.70f, -0.35f}};
    const float top[SIDES] = {0.55f, 0.55f, 0.88f, 1.12f, 1.22f, 1.22f, 1.12f, 0.88f};
    const auto inward = [&](int k)
    {
        // the corner moved WALL toward the tub's middle along its own bisector (the middle at x 0, z 0.80)
        const Vec3 corner = {plan[k][0], 0, plan[k][1]};
        const Vec3 toMiddle = Normalized(Vec3{-plan[k][0], 0, 0.80f - plan[k][1]});
        return corner + toMiddle * WALL;
    };
    for (int k = 0; k < SIDES; ++k)
    {
        const int n = (k + 1) % SIDES;
        const Vec3 a = {plan[k][0], 0, plan[k][1]}, b = {plan[n][0], 0, plan[n][1]};
        const Vec3 ia = inward(k), ib = inward(n);
        const Vec3 outer[4] = {{a.x, SKIRT, a.z}, {b.x, SKIRT, b.z}, {b.x, top[n], b.z}, {a.x, top[k], a.z}};
        PlanarPolygon(mesh, outer, 4, b - a, ATTE_ARMOR);
        const Vec3 inner[4] = {{ia.x, FLOOR, ia.z}, {ib.x, FLOOR, ib.z}, {ib.x, top[n], ib.z}, {ia.x, top[k], ia.z}};
        PlanarPolygon(mesh, inner, 4, b - a, ATTE_PLATE);
        const Vec3 cap[4] = {{a.x, top[k], a.z}, {b.x, top[n], b.z}, {ib.x, top[n], ib.z}, {ia.x, top[k], ia.z}};
        PlanarPolygon(mesh, cap, 4, b - a, ATTE_FRAME);
        const Vec3 rim[2] = {
            {(a.x + ia.x) * 0.5f, top[k] + 0.012f, (a.z + ia.z) * 0.5f},
            {(b.x + ib.x) * 0.5f, top[n] + 0.012f, (b.z + ib.z) * 0.5f}};
        Tube(mesh, rim, 2, 0.022f, 8, ATTE_FRAME);
        // bolts along the outside under the coaming, a maroon band round it (the Republic's)
        const int bolts = static_cast<int>(Length(b - a) / 0.16f);
        const Vec3 outward = Normalized(Cross({0, 1, 0}, b - a)) * -1.0f;
        for (int bolt = 1; bolt < bolts; ++bolt)
        {
            const float t = static_cast<float>(bolt) / bolts;
            const Vec3 at = Lerp(a, b, t) + Vec3{0, top[k] + (top[n] - top[k]) * t - 0.07f, 0} + outward * 0.002f;
            Rivet(mesh, MakeFrame(at, b - a, {0, 1, 0}), 0.011f, ATTE_PIPE);
        }
        const Vec3 band[4] = {
            Vec3{a.x, top[k] - 0.20f, a.z} + outward * 0.003f,
            Vec3{b.x, top[n] - 0.20f, b.z} + outward * 0.003f,
            Vec3{b.x, top[n] - 0.14f, b.z} + outward * 0.003f,
            Vec3{a.x, top[k] - 0.14f, a.z} + outward * 0.003f};
        PlanarPolygon(mesh, band, 4, b - a, ATTE_MAROON);
    }
    {
        // the floor inside, a tread plate under his feet
        Vec3 floor[SIDES];
        for (int k = 0; k < SIDES; ++k)
        {
            floor[k] = inward(k) + Vec3{0, FLOOR, 0};
        }
        PlanarPolygon(mesh, floor, SIDES, {1, 0, 0}, ATTE_FRAME);
        TreadPlate(mesh, {-0.30f, FLOOR + 0.004f, 0.55f}, {0.60f, 0, 0}, {0, 0, 0.70f}, 0.08f, ATTE_FRAME, ATTE_PIPE);
    }
    // The inside: the stock panels on both long walls, a cell rack on the left, cables along the right's foot, grab handles on
    // the coaming either side, a lamp under the rear coaming.
    for (int side = -1; side <= 1; side += 2)
    {
        const float x = side * (0.70f - WALL - 0.005f);
        const float zLeft = side < 0 ? 1.85f : 0.95f, zRight = side < 0 ? 0.95f : 1.85f;
        Quad(mesh, {x, 1.02f, zLeft}, {x, 1.02f, zRight}, {x, 0.24f, zRight}, {x, 0.24f, zLeft}, ATTE_PANEL);
        GrabHandle(
            mesh,
            {side * 0.67f, 0.95f, 0.20f},
            {side * 0.67f, 1.00f, 0.70f},
            {0, 1, 0},
            0.05f,
            0.012f,
            ATTE_PIPE,
            ATTE_FRAME);
    }
    CellRack(mesh, MakeFrame({-0.70f + WALL + 0.005f, 0.62f, 0.25f}, {0, 0, -1}, {0, 1, 0}), 1, 3, look);
    {
        const Vec3 cable[4] =
            {{0.58f, 0.27f, 1.90f}, {0.58f, 0.27f, 0.90f}, {0.56f, 0.27f, 0.0f}, {0.30f, 0.27f, -0.58f}};
        CableLoom(mesh, cable, 4, 2, 0.012f, {0, 1, 0}, ATTE_DARK, ATTE_PIPE, 0.35f);
    }
    Quad(mesh, {-0.20f, 1.12f, 2.22f}, {0.20f, 1.12f, 2.22f}, {0.20f, 1.16f, 2.22f}, {-0.20f, 1.16f, 2.22f}, ATTE_LAMP);
    // The trunnion's cheeks before the front wall, the axle through them, the base under the mantlet.
    for (int side = -1; side <= 1; side += 2)
    {
        Box(mesh,
            MakeFrame({side * 0.43f, trunnion.y - 0.02f, -0.97f}, {1, 0, 0}, {0, 1, 0}),
            {0.05f, 0.24f, 0.27f},
            0.02f,
            ATTE_ARMOR);
        const float cap[3][2] = {{0, 0.10f}, {0.03f, 0.10f}, {0.045f, 0.07f}};
        Lathe(
            mesh,
            MakeFrame({side * 0.48f, trunnion.y, trunnion.z}, {0, 0, -1}, {static_cast<float>(side), 0, 0}),
            cap,
            3,
            16,
            ATTE_FRAME);
    }
    Box(mesh,
        MakeFrame({0, trunnion.y - 0.58f, trunnion.z + 0.02f}, {1, 0, 0}, {0, 1, 0}),
        {0.48f, 0.24f, 0.42f},
        0.03f,
        ATTE_ARMOR);
    // The seat.
    const SeatLook seat = {0.22f, 0.55f, 0.12f, false, false, ATTE_SEAT, ATTE_FRAME, ATTE_PIPE, ATTE_SEAT, ATTE_DARK};
    Seat(mesh, eye, FLOOR, seat);
    // The console before the yoke, turned to his eye: the gun camera's picture in the middle (a stock frame round it),
    // a stock screen either side (toed in), a blue-lit edge under them, the pedestal down to the floor.
    {
        const Vec3 centre = {ATTE_CANNON_VIEWSCREEN[0], ATTE_CANNON_VIEWSCREEN[1], ATTE_CANNON_VIEWSCREEN[2]};
        const Vec3 toEye = Normalized(eye - centre);
        const Frame plate = MakeFrame(centre, {1, 0, 0}, Cross(toEye, {1, 0, 0}));
        Box(mesh, Offset(plate, 0, 0, -0.035f), {0.46f, 0.115f, 0.035f}, 0.015f, ATTE_PLATE);
        Box(mesh,
            MakeFrame({0, (FLOOR + centre.y - 0.10f) * 0.5f, centre.z - 0.02f}, {1, 0, 0}, {0, 1, 0}),
            {0.14f, (centre.y - 0.10f - FLOOR) * 0.5f, 0.10f},
            0.02f,
            ATTE_FRAME);
        Quad(
            mesh,
            plate.At(-0.43f, -0.108f, 0.002f),
            plate.At(0.43f, -0.108f, 0.002f),
            plate.At(0.43f, -0.097f, 0.002f),
            plate.At(-0.43f, -0.097f, 0.002f),
            ATTE_ACCENT);
        const StockScreenLook screens = {&REP_SCREEN_ART, ATTE_SCREEN_FRAME, ATTE_SCREEN_GLASS, ATTE_SCREEN_BACKING,
                                         ATTE_FRAME};
        const float viewHalfWidth = ATTE_CANNON_VIEWSCREEN[3];
        const float viewHalfHeight =
            viewHalfWidth * SPEEDER_PERISCOPE_HALF_HEIGHT_METERS / SPEEDER_PERISCOPE_HALF_WIDTH_METERS;
        const Frame view = Offset(plate, 0, 0, 0.004f);
        StockScreen(
            mesh,
            view,
            viewHalfWidth,
            viewHalfHeight,
            SCREEN_TEXEL_METERS * viewHalfHeight / 0.0675f,
            screens,
            0.02f,
            nullptr,
            nullptr,
            0,
            false);
        Quad(
            mesh,
            view.At(-viewHalfWidth, viewHalfHeight, -0.002f),
            view.At(viewHalfWidth, viewHalfHeight, -0.002f),
            view.At(viewHalfWidth, -viewHalfHeight, -0.002f),
            view.At(-viewHalfWidth, -viewHalfHeight, -0.002f),
            ATTE_PICTURE);
        for (int side = -1; side <= 1; side += 2)
        {
            constexpr float HALF_WIDTH = 0.085f, HALF_HEIGHT = 0.060f;
            const Vec3 at = plate.At(side * 0.335f, 0, 0.006f);
            const Vec3 normal = Normalized(eye - at);
            const Frame face = MakeFrame(at, Cross(plate.y, normal), plate.y);
            StockScreen(
                mesh,
                face,
                HALF_WIDTH,
                HALF_HEIGHT,
                SCREEN_TEXEL_METERS * HALF_HEIGHT / 0.0675f,
                screens,
                0.02f);
            SetDisplayScreen(&screens);
            if (side < 0)
            {
                AddDisplayRegion(GROUP_HEALTH, PLAYER_HEALTH, face, 0, 0.026f, 0.15f, 0.045f, 0);
                AddDisplayRegion(GROUP_HEALTH, VEHICLE_HEALTH, face, 0, -0.022f, 0.15f, 0.06f, 0);
            }
            else
            {
                AddDisplay(GROUP_AMMUNITION, face, 0, 0.020f, 0.15f, 0.052f);
                AddDisplay(GROUP_TARGET, face, 0, -0.033f, 0.12f, 0.030f);
            }
            SetDisplayScreen(nullptr);
        }
        AddDisplay(GROUP_MAP, plate, 0, 0, 0, 0); // the map stays off this seat (the head's panel keeps the score)
        // The yoke on an arm from the console's foot (his hands' place: x +-0.20, 0.30 ahead, 0.22 under the eye):
        // a crossbar, two grips with a trigger each, a control box with a lamp between them.
        const Frame yoke = MakeFrame({eye.x, eye.y - 0.38f, eye.z - 0.32f}, {1, 0, 0}, {0, 1, 0});
        const Vec3 arm[3] = {plate.At(0, -0.11f, 0.02f), {0, eye.y - 0.50f, eye.z - 0.45f}, yoke.origin};
        Tube(mesh, arm, 3, 0.03f, 10, ATTE_FRAME);
        const Vec3 bar[4] =
            {yoke.At(-0.21f, 0.01f, 0.02f), yoke.At(-0.12f, 0, 0), yoke.At(0.12f, 0, 0), yoke.At(0.21f, 0.01f, 0.02f)};
        Tube(mesh, bar, 4, 0.018f, 10, ATTE_PIPE);
        for (int side = -1; side <= 1; side += 2)
        {
            const Vec3 grip[2] = {yoke.At(side * 0.20f, 0.02f, 0.02f), yoke.At(side * 0.20f, 0.16f, 0.02f)};
            Tube(mesh, grip, 2, 0.019f, 12, ATTE_DARK);
            Box(mesh, Offset(yoke, side * 0.20f, 0.13f, -0.005f), {0.006f, 0.016f, 0.012f}, 0.002f, ATTE_RED);
        }
        Box(mesh, Offset(yoke, 0, 0.035f, 0), {0.06f, 0.03f, 0.03f}, 0.006f, ATTE_FRAME);
        Box(mesh, Offset(yoke, 0, 0.067f, 0), {0.012f, 0.004f, 0.012f}, 0.002f, ATTE_RED);
    }
    // Part 2: the barrel in its mantlet (a drum round the trunnion's axle, so it never meets the front wall as it
    // pitches), a collar where the barrel leaves it.
    mesh.part = 2;
    StockExterior(mesh, StockVertices("EXTERIOR_ATTE_BARREL"), StockCount("EXTERIOR_ATTE_BARREL"), ATTE_STOCK, -1);
    {
        const float drum[6][2] =
            {{-0.37f, 0.0f}, {-0.37f, 0.29f}, {-0.34f, 0.32f}, {0.34f, 0.32f}, {0.37f, 0.29f}, {0.37f, 0.0f}};
        Lathe(mesh, MakeFrame(trunnion, {0, 1, 0}, {1, 0, 0}), drum, 6, 24, ATTE_ARMOR);
        const Vec3 muzzle = {ATTE_CANNON_MUZZLE[0], ATTE_CANNON_MUZZLE[1], ATTE_CANNON_MUZZLE[2]};
        const Vec3 along = Normalized(muzzle - trunnion);
        const Vec3 collar[2] = {trunnion + along * 0.28f, trunnion + along * 0.62f};
        Tube(mesh, collar, 2, 0.15f, 16, ATTE_FRAME);
        const Vec3 ring[2] = {trunnion + along * 0.58f, trunnion + along * 0.66f};
        Tube(mesh, ring, 2, 0.17f, 16, ATTE_PIPE);
    }
    mesh.part = 0;
}

constexpr Vec3 ATTE_CANNON_DESIGN_EYE = {0.0f, 1.5f, 1.5f};

#if __has_include("SeatedTrooper.inc")
#include "SeatedTrooper.inc" // src/data: the clone trooper posed seated (tools/seated_trooper.py), game parts made at start
#else
constexpr int SEATED_TROOPER_COUNT = 0;
constexpr StockVertex SEATED_TROOPER[1] = {};
#endif

// A seated clone trooper (SEATED_TROOPER: his seat point at the origin, facing -z) put at `seat`, facing `facing`
// (level), in the finish of the design's exterior slot (the bay's carries rep_inf_trooper).
void SeatedTrooper(Mesh& mesh, Vec3 seat, Vec3 facing, int finish)
{
    const Vec3 up = {0, 1, 0};
    const Vec3 ahead = Normalized(facing), right = Cross(ahead, up);
    for (int i = 0; i + 2 < SEATED_TROOPER_COUNT; i += 3)
    {
        if (mesh.count + 3 > mesh.capacity)
        {
            mesh.isFull = true;
            return;
        }
        for (int k = 0; k < 3; ++k)
        {
            const StockVertex& stock = SEATED_TROOPER[i + k];
            const Vec3 p = stock.position, n = stock.normal;
            Vertex& vertex = mesh.vertices[mesh.count++];
            vertex.position = seat + right * p.x + up * p.y - ahead * p.z;
            vertex.normal = right * n.x + up * n.y - ahead * n.z;
            vertex.u = stock.u;
            vertex.v = stock.v;
            vertex.finish = static_cast<uint8_t>(finish);
            vertex.part = mesh.part;
        }
    }
}

// The AT-TE's rear gunner (idren2023 2026-11-08): in the middle of the troop bay's back end on a turning seat with
// the gun camera's viewscreen (as the cannon's), facing aft (+z; TURRET2's YawLimits 120..240); behind him the troop
// bay of the walkthrough video (re/reference/atte-video H, I; his marked frame for the console's lit strip): seats
// along both walls with clone troopers waiting in them, facing the aisle, a chevron floor down the aisle, grab rails
// and pipes overhead, a white strip light, wall lamps, the forward bulkhead's round passage past the generator (G).
// Walker space (x right, y up, z back), his eye at ATTE_BAY_EYE.
constexpr Vec3 ATTE_BAY_EYE = {0.0f, 1.45f, 3.55f};

void BuildAtteBay(Mesh& mesh)
{
    constexpr float HALF = 1.25f, FLOOR = 0.15f, ROOF = 2.40f, FORE = -0.95f, AFT = 4.45f;
    const Vec3 eye = ATTE_BAY_EYE;
    const DetailLook look = {ATTE_PLATE, ATTE_FRAME, ATTE_PIPE, ATTE_DARK, ATTE_LAMP, ATTE_RED};
    mesh.part = 0;
    // The box: floor, roof, side walls, the bulkheads fore and aft.
    TiledQuad(mesh, {-HALF, FLOOR, FORE}, {2 * HALF, 0, 0}, {0, 0, AFT - FORE}, ATTE_PLATE);
    TiledQuad(mesh, {-HALF, ROOF, FORE}, {2 * HALF, 0, 0}, {0, 0, AFT - FORE}, ATTE_PLATE);
    for (const float side : {-1.0f, 1.0f})
    {
        TiledQuad(mesh, {side * HALF, FLOOR, FORE}, {0, 0, AFT - FORE}, {0, ROOF - FLOOR, 0}, ATTE_PLATE);
    }
    TiledQuad(mesh, {-HALF, FLOOR, FORE}, {2 * HALF, 0, 0}, {0, ROOF - FLOOR, 0}, ATTE_PLATE);
    TiledQuad(mesh, {-HALF, FLOOR, AFT}, {2 * HALF, 0, 0}, {0, ROOF - FLOOR, 0}, ATTE_PLATE);
    // The aisles' chevron floor along both walls (yellow bands on black, pointing forward), a tread plate under the
    // gunner's station.
    {
        constexpr float X0 = 0.58f, X1 = HALF - 0.04f, Z0 = FORE + 0.15f, Z1 = 2.75f, STEP = 0.30f;
        for (const float side : {-1.0f, 1.0f})
        {
            const float inner = side * X0, outer = side * X1, middle = (inner + outer) * 0.5f;
            TiledQuad(mesh, {side < 0 ? outer : inner, FLOOR + 0.002f, Z0}, {X1 - X0, 0, 0}, {0, 0, Z1 - Z0}, ATTE_DARK);
            for (float z = Z0; z + 0.20f + STEP * 0.45f < Z1; z += STEP)
            {
                for (const float edge : {inner, outer})
                {
                    const Vec3 band[4] = {{middle, FLOOR + 0.004f, z},
                                          {edge, FLOOR + 0.004f, z + 0.20f},
                                          {edge, FLOOR + 0.004f, z + 0.20f + STEP * 0.45f},
                                          {middle, FLOOR + 0.004f, z + STEP * 0.45f}};
                    PlanarPolygon(mesh, band, 4, {1, 0, 0}, ATTE_HAZARD);
                }
            }
        }
        TreadPlate(mesh, {-0.55f, FLOOR + 0.003f, 2.95f}, {1.10f, 0, 0}, {0, 0, 1.40f}, 0.08f, ATTE_FRAME, ATTE_PIPE);
    }
    // Overhead: the white strip light down the middle, pipes along the roof's edges, a grab rail over each seat row.
    Box(mesh, MakeFrame({0, ROOF - 0.03f, 1.0f}, {1, 0, 0}, {0, 1, 0}), {0.10f, 0.03f, 1.90f}, 0.01f, ATTE_FRAME);
    Quad(mesh, {-0.06f, ROOF - 0.062f, -0.85f}, {0.06f, ROOF - 0.062f, -0.85f}, {0.06f, ROOF - 0.062f, 2.85f},
         {-0.06f, ROOF - 0.062f, 2.85f}, ATTE_WHITE);
    for (const float side : {-1.0f, 1.0f})
    {
        for (const float offset : {0.0f, 0.09f})
        {
            const Vec3 pipe[2] = {{side * (HALF - 0.06f - offset), ROOF - 0.06f, FORE},
                                  {side * (HALF - 0.06f - offset), ROOF - 0.06f, AFT}};
            Tube(mesh, pipe, 2, 0.035f, 10, ATTE_PIPE);
        }
        const Vec3 rail[2] = {{side * 0.90f, 2.02f, FORE + 0.25f}, {side * 0.90f, 2.02f, 2.70f}};
        Tube(mesh, rail, 2, 0.018f, 10, ATTE_PIPE);
        for (float z = FORE + 0.35f; z < 2.75f; z += 0.85f)
        {
            const Vec3 hanger[2] = {{side * 0.90f, 2.02f, z}, {side * 0.90f, ROOF, z}};
            Tube(mesh, hanger, 2, 0.012f, 8, ATTE_FRAME);
        }
    }
    // The seats back to back down the middle (idren2023 2026-11-08, the video's forward bay: four a side), each with a
    // trooper waiting in it facing the wall, the divider between their backs with a rail along its top; the stock
    // panels high on the walls, a wall lamp between each pair.
    constexpr float SEAT_TOP = 0.62f, SEAT_DEPTH = 0.42f, SPINE = 0.06f, SPINE_TOP = 1.75f;
    const float rows[4] = {-0.45f, 0.30f, 1.05f, 1.80f};
    constexpr float ROW_Z0 = -0.80f, ROW_Z1 = 2.15f;
    Box(mesh, MakeFrame({0, (FLOOR + SPINE_TOP) * 0.5f, (ROW_Z0 + ROW_Z1) * 0.5f}, {1, 0, 0}, {0, 1, 0}),
        {SPINE, (SPINE_TOP - FLOOR) * 0.5f, (ROW_Z1 - ROW_Z0) * 0.5f}, 0.02f, ATTE_FRAME);
    for (const float side : {-1.0f, 1.0f})
    {
        const Vec3 rail[2] = {{side * 0.04f, SPINE_TOP + 0.05f, ROW_Z0}, {side * 0.04f, SPINE_TOP + 0.05f, ROW_Z1}};
        Tube(mesh, rail, 2, 0.016f, 10, ATTE_PIPE);
        for (const float z : {ROW_Z0 + 0.05f, (ROW_Z0 + ROW_Z1) * 0.5f, ROW_Z1 - 0.05f})
        {
            const Vec3 post[2] = {{side * 0.04f, SPINE_TOP, z}, {side * 0.04f, SPINE_TOP + 0.05f, z}};
            Tube(mesh, post, 2, 0.012f, 8, ATTE_FRAME);
        }
        const float back = side * SPINE, face = side * (SPINE + SEAT_DEPTH);
        for (const float z : rows)
        {
            Box(mesh, MakeFrame({(back + face) * 0.5f, SEAT_TOP - 0.05f, z}, {1, 0, 0}, {0, 1, 0}),
                {SEAT_DEPTH * 0.5f, 0.05f, 0.27f}, 0.02f, ATTE_SEAT);
            Box(mesh, MakeFrame({(back + face) * 0.5f, (FLOOR + SEAT_TOP - 0.10f) * 0.5f, z}, {1, 0, 0}, {0, 1, 0}),
                {SEAT_DEPTH * 0.5f - 0.04f, (SEAT_TOP - 0.10f - FLOOR) * 0.5f, 0.24f}, 0.015f, ATTE_FRAME);
            Box(mesh, MakeFrame({back + side * 0.05f, SEAT_TOP + 0.42f, z}, {1, 0, 0}, {0, 1, 0}), {0.05f, 0.40f, 0.25f},
                0.03f, ATTE_SEAT);
            Box(mesh, MakeFrame({back + side * 0.06f, SEAT_TOP + 0.98f, z}, {1, 0, 0}, {0, 1, 0}), {0.05f, 0.12f, 0.14f},
                0.03f, ATTE_SEAT);
            SeatedTrooper(mesh, {back + side * 0.27f, SEAT_TOP, z}, {side, 0, 0}, ATTE_STOCK);
            const Vec3 bar[3] = {{face, SEAT_TOP + 0.05f, z - 0.27f}, {face, SEAT_TOP + 0.55f, z - 0.27f},
                                 {back + side * 0.12f, SEAT_TOP + 0.75f, z - 0.27f}};
            Tube(mesh, bar, 3, 0.015f, 8, ATTE_PIPE);
        }
        const float wall = side * HALF;
        for (int bay = 0; bay < 3; ++bay)
        {
            const float z0 = rows[bay] + 0.10f, z1 = rows[bay + 1] - 0.10f, x = wall - side * 0.006f;
            const float zLeft = side < 0 ? z1 : z0, zRight = side < 0 ? z0 : z1;
            const float lampZ = (rows[bay] + rows[bay + 1]) * 0.5f;
            const float lamp[4][2] = {{0, 0.045f}, {0.10f, 0.045f}, {0.10f, 0.0f}, {0.0f, 0.0f}};
            Lathe(mesh, MakeFrame({x - side * 0.05f, 1.72f, lampZ}, {0, 0, 1}, {0, 1, 0}), lamp, 4, 12, ATTE_WHITE);
            Quad(mesh, {x, 2.25f, zLeft}, {x, 2.25f, zRight}, {x, 1.85f, zRight}, {x, 1.85f, zLeft}, ATTE_PANEL);
        }
        // the wall opposite each row: a bench of stowed gear (cell racks) at the troopers' knees
        CellRack(mesh, MakeFrame({wall - side * 0.03f, 1.20f, 0.68f}, {0, 0, side}, {0, 1, 0}), 3, 3, look);
    }
    // The forward bulkhead: the round passage past the generator (a ribbed ring, its dark tunnel, red lamps over it).
    {
        const Vec3 centre = {0, 1.15f, FORE + 0.004f};
        const float profile[4][2] = {{0, 0.62f}, {0.06f, 0.62f}, {0.08f, 0.52f}, {0.0f, 0.50f}};
        Lathe(mesh, MakeFrame(centre, {1, 0, 0}, {0, 0, 1}), profile, 4, 28, ATTE_FRAME);
        const float tunnel[2][2] = {{0, 0.50f}, {-0.60f, 0.50f}};
        Lathe(mesh, MakeFrame(centre, {1, 0, 0}, {0, 0, 1}), tunnel, 2, 28, ATTE_DARK);
        const Frame ring = MakeFrame(centre, {1, 0, 0}, {0, 1, 0});
        Quad(mesh, ring.At(-0.5f, -0.5f, -0.60f), ring.At(0.5f, -0.5f, -0.60f), ring.At(0.5f, 0.5f, -0.60f),
             ring.At(-0.5f, 0.5f, -0.60f), ATTE_DARK);
        for (int lamp = -1; lamp <= 1; ++lamp)
        {
            Box(mesh, Offset(ring, lamp * 0.18f, 0.80f, 0.02f), {0.05f, 0.015f, 0.02f}, 0.004f, ATTE_RED);
        }
        Locker(mesh, MakeFrame({-0.95f, 1.30f, FORE + 0.03f}, {1, 0, 0}, {0, 1, 0}), 0.36f, 0.70f, 0.10f, look);
        CellRack(mesh, MakeFrame({0.95f, 1.40f, FORE + 0.03f}, {1, 0, 0}, {0, 1, 0}), 2, 3, look);
    }
    // The aft bulkhead behind the gunner's console: the stock panels, the guns' breech housings low either side.
    for (const float side : {-1.0f, 1.0f})
    {
        Quad(mesh, {side * 1.15f, 2.20f, AFT - 0.006f}, {side * 0.45f, 2.20f, AFT - 0.006f},
             {side * 0.45f, 1.40f, AFT - 0.006f}, {side * 1.15f, 1.40f, AFT - 0.006f}, ATTE_PANEL);
        Box(mesh, MakeFrame({side * 0.83f, 0.75f, AFT - 0.18f}, {1, 0, 0}, {0, 1, 0}), {0.22f, 0.25f, 0.18f}, 0.03f,
            ATTE_FRAME);
    }
    // Part 1: the gunner's turning station: a pedestal seat, his twin grips, the console before him with the gun
    // camera's picture and the stock screens, a white-lit strip under it (idren2023's marked frame), knobs on its face.
    mesh.part = 1;
    {
        const float pedestal[4][2] = {{0, 0.20f}, {0.06f, 0.20f}, {0.10f, 0.07f}, {0.55f, 0.07f}};
        Lathe(mesh, MakeFrame({eye.x, FLOOR, eye.z - 0.05f}, {1, 0, 0}, {0, 1, 0}), pedestal, 4, 16, ATTE_FRAME);
        const float cushionY = eye.y - 0.80f;
        Box(mesh, MakeFrame({eye.x, cushionY - 0.05f, eye.z - 0.05f}, {1, 0, 0}, {0, 1, 0}), {0.22f, 0.05f, 0.24f}, 0.03f,
            ATTE_SEAT);
        // a low back only (to the small of his back): looking over his shoulder shows the bay, not the seat
        Box(mesh, MakeFrame({eye.x, cushionY + 0.17f, eye.z - 0.30f}, {1, 0, 0}, {0, 1, 0}), {0.20f, 0.15f, 0.04f}, 0.03f,
            ATTE_SEAT);
        AtteGrips(mesh, eye, 1.0f, FLOOR);
        const Vec3 centre = {ATTE_BAY_VIEWSCREEN[0], ATTE_BAY_VIEWSCREEN[1], ATTE_BAY_VIEWSCREEN[2]};
        const Vec3 toEye = Normalized(eye - centre);
        const Frame plate = MakeFrame(centre, {-1, 0, 0}, Cross(toEye, {-1, 0, 0}));
        Box(mesh, Offset(plate, 0, 0, -0.035f), {0.46f, 0.115f, 0.035f}, 0.015f, ATTE_PLATE);
        Box(mesh, MakeFrame({0, (FLOOR + centre.y - 0.10f) * 0.5f, centre.z + 0.02f}, {1, 0, 0}, {0, 1, 0}),
            {0.14f, (centre.y - 0.10f - FLOOR) * 0.5f, 0.10f}, 0.02f, ATTE_FRAME);
        Quad(mesh, plate.At(-0.43f, -0.108f, 0.002f), plate.At(0.43f, -0.108f, 0.002f), plate.At(0.43f, -0.097f, 0.002f),
             plate.At(-0.43f, -0.097f, 0.002f), ATTE_WHITE);
        KnobRow(mesh, Offset(plate, -0.36f, -0.075f, 0.004f), 4, 0.04f, 0.010f, look);
        const StockScreenLook screens = {&REP_SCREEN_ART, ATTE_SCREEN_FRAME, ATTE_SCREEN_GLASS, ATTE_SCREEN_BACKING,
                                         ATTE_FRAME};
        const float viewHalfWidth = ATTE_BAY_VIEWSCREEN[3];
        const float viewHalfHeight =
            viewHalfWidth * SPEEDER_PERISCOPE_HALF_HEIGHT_METERS / SPEEDER_PERISCOPE_HALF_WIDTH_METERS;
        const Frame view = Offset(plate, 0, 0, 0.004f);
        StockScreen(mesh, view, viewHalfWidth, viewHalfHeight, SCREEN_TEXEL_METERS * viewHalfHeight / 0.0675f, screens,
                    0.02f, nullptr, nullptr, 0, false);
        Quad(mesh, view.At(-viewHalfWidth, viewHalfHeight, -0.002f), view.At(viewHalfWidth, viewHalfHeight, -0.002f),
             view.At(viewHalfWidth, -viewHalfHeight, -0.002f), view.At(-viewHalfWidth, -viewHalfHeight, -0.002f),
             ATTE_PICTURE);
        for (int side = -1; side <= 1; side += 2)
        {
            constexpr float HALF_WIDTH = 0.085f, HALF_HEIGHT = 0.060f;
            const Vec3 at = plate.At(side * 0.335f, 0, 0.006f);
            const Vec3 normal = Normalized(eye - at);
            const Frame face = MakeFrame(at, Cross(plate.y, normal), plate.y);
            StockScreen(mesh, face, HALF_WIDTH, HALF_HEIGHT, SCREEN_TEXEL_METERS * HALF_HEIGHT / 0.0675f, screens, 0.02f);
            SetDisplayScreen(&screens);
            if (side < 0)
            {
                AddDisplayRegion(GROUP_HEALTH, PLAYER_HEALTH, face, 0, 0.026f, 0.15f, 0.045f, 0);
                AddDisplayRegion(GROUP_HEALTH, VEHICLE_HEALTH, face, 0, -0.022f, 0.15f, 0.06f, 0);
            }
            else
            {
                AddDisplay(GROUP_AMMUNITION, face, 0, 0.020f, 0.15f, 0.052f);
                AddDisplay(GROUP_TARGET, face, 0, -0.033f, 0.12f, 0.030f);
            }
            SetDisplayScreen(nullptr);
        }
        AddDisplay(GROUP_MAP, plate, 0, 0, 0, 0);
    }
    mesh.part = 0;
}


// The shells: which vehicle, its builder, its textures and finishes.
struct Design
{
    const char* odfName;
    void (*build)(Mesh& mesh);
    const char* textureName;  // the first-person cockpit's
    const char* effectsName;  // its effects texture
    const char* exteriorName; // the vehicle's own exterior texture, where a design uses it
    const char* glassName;    // the cockpit's window texture, where a design has glass
    const Finish* finishes;
    int finishCount;
    int partCount = 1;                 // moving parts (a gunner's turret: 3)
    const Vec3* eye = &TANK_PILOT_EYE; // the pilot's eye (cockpits), or the eye the flat faces turn their normals to
    int restPartCount = 0; // a cockpit with moving parts: the parts CockpitShell::Draw draws at rest (0: all)
    const char* screenName = nullptr; // a stock targeting frame's texture, where a design uses it
    bool isWholeCockpit = false;      // the shell is the whole cockpit: the game's first-person model is not drawn
    bool isDisplayOnTop = false; // a gunner station's display (parts 2 to 5) drawn over everything (headset run 19: the
                                 // Y-wing's display went through the hull when the chair turned back)
    int plateFinish = -1; // the finish whose patch the plain-colour finishes take (Textured); -1: the first plain
                          // textured one of the cockpit's texture
};

const Design DESIGNS[] = {
    {"imp_walk_atst",
     BuildAtst,
     "imp_cockpit_ATST",
     "imp_1st_cockpit_fx",
     nullptr,
     nullptr,
     ATST_FINISHES,
     ATST_FINISH_COUNT,
     1,
     &ATST_EYE,
     0,
     "imp_1st_cockpit_securitycam", // the HUD screens' stock frame
     true}, // the whole cab is ours (the stock pilot model mirrored, 2026-11-11)
    {"imp_walk_atst_snow",
     BuildAtst,
     "imp_cockpit_ATST",
     "imp_1st_cockpit_fx",
     nullptr,
     nullptr,
     ATST_FINISHES,
     ATST_FINISH_COUNT,
     1,
     &ATST_EYE,
     0,
     "imp_1st_cockpit_securitycam", // the HUD screens' stock frame
     true}, // the whole cab is ours (the stock pilot model mirrored, 2026-11-11)
    {"imp_walk_atst_jungle",
     BuildAtst,
     "imp_cockpit_ATST",
     "imp_1st_cockpit_fx",
     nullptr,
     nullptr,
     ATST_FINISHES,
     ATST_FINISH_COUNT,
     1,
     &ATST_EYE,
     0,
     "imp_1st_cockpit_securitycam", // the HUD screens' stock frame
     true}, // the whole cab is ours (the stock pilot model mirrored, 2026-11-11)
    {"rep_fly_gunship",
     BuildGunship,
     "rep_1st_cockpit_gunship",
     "rep_cockpit_fx",
     "REP_LA_AT",
     "rep_cockpit_win",
     GUN_FINISHES,
     GUN_FINISH_COUNT,
     SPEEDER_CABIN_PARTS,
     &GUN_EYE,
     3,
     nullptr,
     true}, // the stock cockpit drawn by us, its dash enlarged (2026-11-11)
    {"rep_fly_gunship_ball2",
     BuildGunshipBall2,
     "rep_1st_cockpit_gunship",
     "rep_cockpit_fx",
     "REP_LA_AT",
     "rep_cockpit_win",
     GUN_FINISHES,
     GUN_FINISH_COUNT,
     SPEEDER_CABIN_PARTS,
     &GUN_BALL_EYE,
     0},
    {"rep_fly_gunship_ball3",
     BuildGunshipBall3,
     "rep_1st_cockpit_gunship",
     "rep_cockpit_fx",
     "REP_LA_AT",
     "rep_cockpit_win",
     GUN_FINISHES,
     GUN_FINISH_COUNT,
     SPEEDER_CABIN_PARTS,
     &GUN_BALL_EYE,
     0},
    {"rep_fly_gunship_door",
     BuildGunshipDoor,
     "rep_1st_cockpit_gunship",
     "rep_cockpit_fx",
     "REP_LA_AT",
     "rep_cockpit_win",
     GUN_FINISHES,
     GUN_FINISH_COUNT,
     SPEEDER_CABIN_PARTS,
     &GUN_BALL_EYE,
     0},
    {"cis_fly_maf",
     BuildMaf,
     "cis_1st_cockpit_MAF",
     "cis_1st_cockpit_fx",
     nullptr,
     nullptr,
     MAF_FINISHES,
     MAF_FINISH_COUNT,
     1,
     &MAF_EYE,
     0,
     "cis_1st_cockpit_securitycam"},
    {"cis_fly_maf_top",
     BuildMafTop,
     "cis_1st_cockpit_MAF",
     "cis_1st_cockpit_fx",
     nullptr,
     nullptr,
     MAF_FINISHES,
     MAF_FINISH_COUNT,
     SPEEDER_CABIN_PARTS,
     &MAF_EYE,
     3,
     "cis_1st_cockpit_securitycam"},
    {"cis_fly_maf_rear",
     BuildMafRear,
     "cis_1st_cockpit_MAF",
     "cis_1st_cockpit_fx",
     nullptr,
     nullptr,
     MAF_FINISHES,
     MAF_FINISH_COUNT,
     SPEEDER_CABIN_PARTS,
     &MAF_EYE,
     3,
     "cis_1st_cockpit_securitycam"},
    {"all_fly_snowspeeder",
     BuildSnowspeeder,
     "all_1st_cockpit_snowspeeder",
     "all_1st_cockpit_fx",
     "all_fly_speeder",
     "all_fly_snowspeeder_windscreen",
     SNOW_FINISHES,
     SNOW_FINISH_COUNT,
     SPEEDER_CABIN_PARTS, // the tail gunner's swivel station (SnowGunnerStation)
     &SNOW_EYE,
     0,
     "all_1st_cockpit_securitycam", // the HUD screens' stock frame (loaded on Hoth)
     true},
    {"rep_walk_atte", // the pilot's cab in the walker's nose (idren2023 2026-11-04)
     BuildAtteCab,
     "rep_1st_cockpit_ATTE",
     "rep_1st_cockpit_fx",
     "rep_walk_sixleg",
     "rep_cockpit_win", // the stock windshield's glass (loaded with its first-person cockpit)
     ATTE_FINISHES,
     ATTE_FINISH_COUNT,
     1,
     &ATTE_CAB_EYE,
     0,
     "rep_1st_cockpit_securitycam", // the dash's stock screens (loaded with the walker: its cannon's design uses it)
     true},
    {"rep_walk_atte_rear", // the rear gunner's station at the troop bay's back end (idren2023 2026-11-08)
     BuildAtteBay,
     "rep_1st_cockpit_ATTE",
     "rep_1st_cockpit_fx",
     "rep_inf_trooper", // the exterior slot carries the seated troopers' texture (the bay shows no exterior)
     nullptr,
     ATTE_FINISHES,
     ATTE_FINISH_COUNT,
     SPEEDER_CABIN_PARTS,
     &ATTE_BAY_EYE,
     0,
     "rep_1st_cockpit_securitycam",
     true},
    {"rep_walk_atte_cannon",
     BuildAtteCannon,
     "rep_1st_cockpit_ATTE",
     "rep_1st_cockpit_fx",
     "rep_walk_sixleg",
     nullptr,
     ATTE_FINISHES,
     ATTE_FINISH_COUNT,
     SPEEDER_CABIN_PARTS,
     &ATTE_CANNON_DESIGN_EYE,
     0,
     "rep_1st_cockpit_securitycam"}, // the HUD screens' stock frame
    {"imp_walk_atat",
     BuildAtat,
     "imp_1st_cockpit_ATAT",
     "imp_1st_cockpit_fx",
     nullptr,
     "imp_cockpit_win",
     ATAT_FINISHES,
     ATAT_FINISH_COUNT,
     1, // the gunner's plain co-pilot chair (AtatGunnerStation) is part of the cab
     &ATAT_EYE,
     0,
     "imp_1st_cockpit_securitycam", // the HUD screens' stock frame
     true}, // the whole cab is ours (the stock pilot model mirrored, 2026-11-11)
    {"cis_tread_hailfire",
     BuildHailfire,
     "cis_1st_cockpit_hailfiredroid",
     "cis_1st_cockpit_fx",
     nullptr,
     nullptr,
     HAIL_FINISHES,
     HAIL_FINISH_COUNT,
     1,
     &HAIL_EYE,
     0,
     nullptr,
     true}, // the whole cockpit (idren2023: an enclosed cabin with a viewscreen): the stock frame not drawn
    {"cis_walk_spider",
     BuildSpider,
     "cis_1st_cockpit_spiderwalker",
     "cis_1st_cockpit_fx",
     nullptr,
     nullptr,
     SPIDER_FINISHES,
     SPIDER_FINISH_COUNT,
     1,
     &SPIDER_EYE,
     0,
     nullptr,
     true}, // the whole cockpit (idren2023: a seat inside the hull's sphere): the stock plate not drawn
    {"cis_hover_aat",
     BuildAat,
     "cis_1st_cockpit_aat",
     "cis_1st_cockpit_fx",
     "cis_hover_aat",
     nullptr,
     AAT_FINISHES,
     AAT_FINISH_COUNT,
     SPEEDER_CABIN_PARTS,
     &TANK_PILOT_EYE,
     3,
     "cis_1st_cockpit_securitycam"},
    {"rep_hover_fightertank",
     BuildRepublicTank,
     "rep_1st_cockpit_fightertank",
     "rep_cockpit_fx",
     nullptr,
     nullptr,
     REP_FINISHES,
     REP_FINISH_COUNT,
     1,
     &TANK_PILOT_EYE,
     0,
     "rep_1st_cockpit_securitycam"},
    {"imp_hover_fightertank",
     BuildImperialTank,
     "imp_1st_cockpit_fightertank",
     "imp_1st_cockpit_fx",
     nullptr,
     nullptr,
     IMP_FINISHES,
     IMP_FINISH_COUNT,
     1,
     &TANK_PILOT_EYE,
     0,
     "imp_1st_cockpit_securitycam"},
    {"cis_hover_mtt",
     BuildMtt,
     "cis_1st_cockpit_mtt",
     "cis_1st_cockpit_fx",
     nullptr,
     nullptr,
     MTT_FINISHES,
     MTT_FINISH_COUNT,
     1,
     &MTT_PILOT_EYE,
     0,
     "cis_1st_cockpit_securitycam"},
    {"all_hover_combatspeeder",
     BuildCombatSpeeder,
     "all_1st_cockpit_combatspeeder",
     "all_1st_cockpit_fx",
     "all_hover_rebel_attack_speeder",
     "all_cockpit_xwingwin",
     SPD_FINISHES,
     SPD_FINISH_COUNT,
     SPEEDER_CABIN_PARTS,
     &SPEEDER_PILOT_EYE,
     3,
     "all_1st_cockpit_securitycam"},
    {"rep_hover_fightertank_gunner",
     BuildFighterTankTurret,
     "rep_1st_cockpit_fightertank",
     "rep_cockpit_fx",
     "rep_hover_fightertank",
     nullptr,
     REP_TURRET_FINISHES,
     TUR_FINISH_COUNT,
     3,
     &GUNNER_EYE,
     0,
     "rep_1st_cockpit_securitycam"},
    {"imp_hover_fightertank_gunner",
     BuildFighterTankTurret,
     "imp_1st_cockpit_fightertank",
     "imp_1st_cockpit_fx",
     "imp_hover_fightertank",
     nullptr,
     IMP_TURRET_FINISHES,
     TUR_FINISH_COUNT,
     3,
     &GUNNER_EYE,
     0,
     "imp_1st_cockpit_securitycam"},
    {"hoth_bldg_dish_turret_screen",
     BuildDishScreen,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     BUILDING_SCREEN_FINISHES,
     BUILDING_SCREEN_FINISH_COUNT,
     2,
     &DISH_GUNNER_EYE},
    {"geo_bldg_geoturret_screen",
     BuildGeoScreen,
     nullptr,
     nullptr,
     nullptr,
     nullptr,
     BUILDING_SCREEN_FINISHES,
     BUILDING_SCREEN_FINISH_COUNT,
     2,
     &GEO_GUNNER_EYE},
    {"hoth_bldg_hoth_turret_cabin",
     BuildHothTowerCabin,
     "turret256",
     "HOTH_bldg_bunker",
     "HOTH_bldg_command_center_console",
     "com_1st_cockpit_hothviewfinder",
     HTW_FINISHES,
     HTW_FINISH_COUNT,
     3, // part 2: the drum, still in the tower's frame (GunnerSeat)
     &HOTH_TOWER_EYE,
     0,
     "all_1st_cockpit_securitycam"},
    {"all_fly_xwing",
     BuildXwing,
     "all_1st_cockpit_xwing",
     "all_1st_cockpit_fx",
     "all_fly_xwing",
     "all_cockpit_xwingwin",
     XWG_FINISHES,
     XWG_FINISH_COUNT,
     1,
     &XWG_EYE,
     0,
     "all_1st_cockpit_securitycam",
     true},
    {"all_fly_ywing", // the Y-wing's pilot: the X-wing's cockpit in the Y-wing's own hull (the stock reuses the model too)
     BuildYwing,
     "all_1st_cockpit_ywing",
     "all_1st_cockpit_fx",
     "all_fly_ywing",
     "all_cockpit_xwingwin",
     XWG_FINISHES,
     XWG_FINISH_COUNT,
     SPEEDER_CABIN_PARTS, // the gunner's station behind the pilot (YwingGunnerStation)
     &YWG_EYE,
     0,
     "all_1st_cockpit_securitycam",
     true,
     true},
    {"imp_fly_tiebomber", // the TIE bomber's pilot: the TIE fighter's pod in the bomber's own hull (the films' one TIE set);
                          // its bombardier's station in the tube behind (SPEEDER_CABIN_PARTS, GunnerSeat's cabin)
     BuildTieBomber,
     "imp_1st_cockpit_tiefighter",
     "imp_1st_cockpit_fx",
     "imp_fly_tiebomber",
     "imp_cockpit_win",
     TIE_FINISHES,
     TIE_FINISH_COUNT,
     SPEEDER_CABIN_PARTS,
     &TIE_EYE,
     3,
     "imp_1st_cockpit_securitycam",
     true},
    {"imp_fly_tiefighter",
     BuildTieFighter,
     "imp_1st_cockpit_tiefighter",
     "imp_1st_cockpit_fx",
     "imp_fly_tiefighter",
     "imp_cockpit_win",
     TIE_FINISHES,
     TIE_FINISH_COUNT,
     1,
     &TIE_EYE,
     0,
     "imp_1st_cockpit_securitycam",
     true},
    {"rep_fly_jedifighter",
     BuildJediFighter,
     "rep_1st_cockpit_jedistarfighter",
     "rep_cockpit_fx",
     "rep_fly_fighter",
     "rep_cockpit_win",
     JED_FINISHES,
     JED_FINISH_COUNT,
     1,
     &JED_EYE,
     0,
     "rep_1st_cockpit_securitycam",
     true},
    {"cis_fly_droidfighter",
     BuildDroidFighter,
     "cis_1st_cockpit_starfighter",
     "cis_1st_cockpit_fx",
     "cis_fly_droidstarfighter",
     nullptr,
     DRD_FINISHES,
     DRD_FINISH_COUNT,
     1,
     &DRD_EYE,
     0,
     nullptr,
     true,
     false,
     2}, // the plate (its first textured finish is the visor's art)
    {"cis_fly_geofighter",
     BuildGeoFighter,
     nullptr,
     nullptr,
     "cis_fly_fighter",
     "cis_fly_fighter_canopy",
     GEO_FINISHES,
     GEO_FINISH_COUNT,
     1,
     &GEO_EYE,
     0,
     "cis_1st_cockpit_securitycam",
     true},
};

const Design* DesignOf(const char* odfName)
{
    if (!odfName)
    {
        return nullptr;
    }
    for (const Design& design : DESIGNS)
    {
        if (!strcmp(design.odfName, odfName))
        {
            return &design;
        }
    }
    return nullptr;
}

} // namespace

bool HasShell(const char* odfName)
{
    return DesignOf(odfName) != nullptr;
}

bool IsWholeCockpit(const char* odfName)
{
    const Design* design = DesignOf(odfName);
    return design && design->isWholeCockpit;
}

bool IsDisplayOnTop(const char* odfName)
{
    const Design* design = DesignOf(odfName);
    return design && design->isDisplayOnTop;
}

bool DesignEye(const char* odfName, float* eye)
{
    const Design* design = DesignOf(odfName);
    if (!design)
    {
        return false;
    }
    eye[0] = design->eye->x;
    eye[1] = design->eye->y;
    eye[2] = design->eye->z;
    return true;
}

bool PilotEye(const char* odfName, float* eye)
{
    const Design* design = DesignOf(odfName);
    if (!design || (design->partCount != 1 && !design->restPartCount)) // gunner designs: every part moves
    {
        return false;
    }
    eye[0] = design->eye->x;
    eye[1] = design->eye->y;
    eye[2] = design->eye->z;
    return true;
}

/// Most finishes a design's table has (Textured keeps the plain table beyond it).
constexpr int MOST_FINISHES = 96;

/// The design's finishes with no plain colour left on an unlit surface (idren2023 2026-11-09: "a lot of flat colors";
/// the texturing pass): every untextured finish that neither glows nor is glass takes the design's own plate (its
/// first plain textured finish of the cockpit's texture: the patch, its size and its texture) in its own colour, so
/// metal, rubber, pads and frames show the cockpit texture's grain. Lamps, screens and glass stay as they are. Kept
/// per design in static storage (a gunner's cabin and the pilot's shell are drawn at once; not the game's heap).
const Finish* Textured(const Design& design)
{
    static Finish tables[std::size(DESIGNS)][MOST_FINISHES];
    const int index = static_cast<int>(&design - DESIGNS);
    if (design.finishCount > MOST_FINISHES)
    {
        return design.finishes;
    }
    const Finish* plate = design.plateFinish >= 0 ? &design.finishes[design.plateFinish] : nullptr;
    for (int k = 0; k < design.finishCount && !plate; ++k)
    {
        const Finish& f = design.finishes[k];
        if (f.texture == 1 && f.glow == 0 && f.opacity >= 1.0f && !f.isOwnScale && design.textureName)
        {
            plate = &f;
        }
    }
    Finish* table = tables[index];
    for (int k = 0; k < design.finishCount; ++k)
    {
        table[k] = design.finishes[k];
        Finish& f = table[k];
        if (plate && f.texture == 0 && f.glow == 0 && f.opacity >= 1.0f)
        {
            f.patch = plate->patch;
            f.meters = plate->meters;
            f.texture = plate->texture;
            f.isTurned = plate->isTurned;
            f.isDense = plate->isDense;
        }
    }
    return table;
}

bool Build(const char* odfName, ShellKit::Vertex* vertices, int capacity, Shell* shell)
{
    const Design* design = DesignOf(odfName);
    if (!design)
    {
        return false;
    }
    _eye = *design->eye;
    const Finish* finishes = Textured(*design);
    Mesh mesh = {vertices, capacity, 0, false, finishes, _eye};
    mesh.texelsPerMeter = TEXELS_PER_METER;
    _displayCount = 0;
    _scanAreaCount = 0;
    _maskCount = 0;
    design->build(mesh);
    *shell = {
        design->textureName,
        design->effectsName,
        design->exteriorName,
        design->glassName,
        design->screenName,
        finishes,
        design->finishCount,
        mesh.count,
        mesh.isFull,
        _displays,
        _displayCount,
        _scanAreas,
        _scanAreaCount,
        _mask,
        _maskCount,
        design->partCount,
        design->restPartCount ? design->restPartCount : design->partCount};
    return true;
}

} // namespace ShellDesigns

} // namespace SwbfVr
