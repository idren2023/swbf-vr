// LeverModel.cpp: the drive handle's look (PLAN 54 a2, PLAN 57 d; see LeverModel.h).
//
// The mesh is made once, in the handle's own space (metres; origin at the pivot on the carriage's top, y up along the
// grip, x right, z back toward the player), from turned profiles (a lathe: rings of a radius at a height, 24 sides):
// the carriage, which slides with the handle but keeps the hull's axes, the grip on it, which leans for strafe, and
// the trigger, hinged on the grip. It goes into a
// managed vertex buffer (kept by Direct3D across device resets), grouped by part and material, with the style's
// texture patch baked into its coordinates; it is rebuilt only when the vehicle's style changes. Each eye pass is then
// a dozen small draws, as the game's own models are drawn: the graphics card transforms and lights the vertices
// (Direct3D's fixed-function lighting: a key light fixed to the hull from above, ahead and to the right, like the
// cockpit's baked light; a weak fill from below behind; ambient; Blinn highlights per material, toward the eye). b228
// lit and transformed every vertex on the processor for each eye: 1.18 ms per eye pass (run233); this way costs a few
// draw calls. Metal and rubber take a plain patch of the cockpit's own texture (by name from the game's texture table,
// GameBuild TextureByHash), modulated 2x by the lit colour so its grime and scratches show through.

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable (device->lpVtbl->Method(device,
// ...)), the form the rest of the mod uses. Has to be a macro: d3d9.h reads it.
#define CINTERFACE

#include "LeverModel.h"

#include "FpGuard.h"
#include "GameBuild.h"
#include "LeverPlacement.h"
#include "Log.h"
#include "Textures.h"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstring>

namespace SwbfVr
{

namespace LeverModel
{

namespace
{

/// A stock vehicle's look: its first-person cockpit texture (the name the game loads it by, from the cockpit model's
/// material) and a plain patch of it (u, v from the top left, 0..1), the metal and rubber colours (multiplied by the
/// texture twice over, so 0x80 is the texture as it is) and the thumb button's colour.
struct Style
{
    const char* odfName;
    const char* textureName;
    float patch[4]; // u0, v0, u1, v1
    D3DCOLOR metal, rubber, button;
};

// Patches picked by eye from the stock textures (BFBuilder Assets/Sides/*/MSH/*_1st_cockpit_*.tga, 512 x 512): the
// AAT's worn brown plate under its window frame, the fighter tanks' grey panel right of their green screen, the MTT's
// olive plate right of its switch box, the combat speeder's plain plate.
const Style STYLES[] = {
    {"cis_hover_aat", "cis_1st_cockpit_aat", {0.22f, 0.46f, 0.66f, 0.60f}, 0xFF8C8478, 0xFF3C3832, 0xFFE8401C},
    {"cis_hover_mtt", "cis_1st_cockpit_mtt", {0.62f, 0.25f, 0.94f, 0.41f}, 0xFF888C80, 0xFF34362E, 0xFFF0A020},
    {"rep_hover_fightertank",
     "rep_1st_cockpit_fightertank",
     {0.809f, 0.617f, 0.965f, 0.820f},
     0xFF8C8C90,
     0xFF2C2C30,
     0xFFE83020},
    {"imp_hover_fightertank",
     "imp_1st_cockpit_fightertank",
     {0.809f, 0.617f, 0.965f, 0.820f},
     0xFF84868C,
     0xFF262628,
     0xFFE83020},
    {"all_hover_combatspeeder",
     "all_1st_cockpit_combatspeeder",
     {0.215f, 0.512f, 0.430f, 0.645f},
     0xFF8A8C82,
     0xFF2E2A26,
     0xFFF08020},
};
/// Any other vehicle: no texture, neutral gunmetal and black rubber.
const Style DEFAULT_STYLE = {nullptr, nullptr, {0, 0, 1, 1}, 0xFF6E7076, 0xFF202022, 0xFFE03020};

/// Lathe sides.
constexpr int SIDES = 24;
/// The handle's parts, handle space (metres). The carriage: an oval block riding on the console's deck, its top at the
/// pivot (LeverPlacement); the hand closes about half way up the grip (LeverPlacement::GRIP_HEIGHT_METERS).
constexpr float CARRIAGE_HALF_WIDTH = LeverPlacement::CARRIAGE_HALF_WIDTH_METERS;
constexpr float CARRIAGE_DEPTH_SCALE = LeverPlacement::CARRIAGE_HALF_LENGTH_METERS / CARRIAGE_HALF_WIDTH;
constexpr float CARRIAGE_HEIGHT = LeverPlacement::CARRIAGE_HEIGHT_METERS;
constexpr float BOOT_TOP = 0.030f;
constexpr float SHAFT_TOP = 0.034f;
constexpr float GRIP_BOTTOM = 0.046f, GRIP_TOP = 0.154f, CAP_TOP = 0.174f;
/// The trigger (idren2023 2026-10-17): a curved blade under the index finger at the grip's front (-z), hinged at its top;
/// the controller's trigger turns it back toward the grip by up to this angle.
constexpr float TRIGGER_HINGE[2] = {GRIP_BOTTOM + 0.090f, -0.0205f}; // height, depth
/// The jet stick (Model::JetStick, handle space as the tank's: origin at the pivot on the floor plate): the gaiter's
/// top, the shaft, the grip (the hand closes JET_STICK_GRIP_METERS up), the head with its buttons; its trigger's hinge.
constexpr float JET_GAITER_TOP = 0.10f, JET_SHAFT_TOP = 0.262f, JET_GRIP_BOTTOM = 0.275f, JET_GRIP_TOP = 0.385f;
constexpr float JET_HEAD_TOP = 0.418f, JET_GRIP_DEPTH = 1.28f;
constexpr float JET_TRIGGER_HINGE[2] = {0.362f, -0.0245f};
/// The throttle (Model::Throttle; origin at its hinge, the arm along y): the hub's radius and half width, the arm's
/// length, the knob's half length and radius (the knob's middle THROTTLE_GRIP_METERS up).
constexpr float THROTTLE_HUB_RADIUS = 0.050f, THROTTLE_HUB_HALF_WIDTH = 0.022f, THROTTLE_ARM = 0.120f;
constexpr float THROTTLE_KNOB_HALF_LENGTH = 0.048f, THROTTLE_KNOB_RADIUS = 0.019f;
constexpr float TRIGGER_PULL_RADIANS = 0.32f;
/// Light (resting-pose space before normalising: toward the light) and its parts.
constexpr float KEY_LIGHT[3] = {0.35f, 1.0f, -0.45f}, FILL_LIGHT[3] = {-0.3f, -0.6f, 0.5f};
constexpr float AMBIENT = 0.34f, KEY = 0.78f, FILL = 0.14f;

enum class Material
{
    Metal,    // textured, strong highlight
    Polished, // the shaft and collar: textured, brighter and sharper
    Rubber,   // textured dark, soft highlight
    Button    // the thumb button: its own colour, barely lit
};

/// What a vertex moves with.
enum class Part
{
    Carriage, // slides with the handle, the hull's axes
    Grip,     // slides with the handle and leans for strafe
    Trigger   // as the grip, and turned about its hinge by the controller's trigger
};

struct MeshVertex
{
    float position[3], normal[3], u, v;
    Material material;
    Part part;
    Model model;
};

struct Vertex
{
    float x, y, z;
    float normalX, normalY, normalZ;
    float u, v;
};

constexpr DWORD VERTEX_FORMAT = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
constexpr int MOST_VERTICES = 36000; // the tank handle, the jet stick and the throttle

MeshVertex _mesh[MOST_VERTICES];
int _meshCount = 0;

/// The vertex buffer's groups: one draw each, with its part's frame and its material.
struct Group
{
    Model model;
    Part part;
    Material material;
    int start, count; // vertices
};

constexpr int MOST_GROUPS = 48;
Group _groups[MOST_GROUPS];
int _groupCount = 0;
IDirect3DVertexBuffer9* _vertexBuffer = nullptr;
const Style* _bufferStyle = nullptr; // the style whose patch is baked into the buffer
IDirect3DStateBlock9* _savedState = nullptr;
IDirect3DDevice9* _stateBlockDevice = nullptr;
const Style* _loggedStyle = nullptr; // the look last logged
/// Cost (CPU time of Draw, the transform, lighting and the draw call's upload): summed and logged every so many draws.
constexpr int COST_LOG_DRAWS = 1000;
double _costMilliseconds = 0, _costMaximumMilliseconds = 0;
int _costDraws = 0;

void Normalize(float* vector)
{
    const float length = sqrtf(vector[0] * vector[0] + vector[1] * vector[1] + vector[2] * vector[2]);
    if (length > 1e-6f)
    {
        vector[0] /= length;
        vector[1] /= length;
        vector[2] /= length;
    }
}

void AddTriangle(const MeshVertex& first, const MeshVertex& second, const MeshVertex& third)
{
    if (_meshCount + 3 > MOST_VERTICES)
    {
        return;
    }
    _mesh[_meshCount++] = first;
    _mesh[_meshCount++] = second;
    _mesh[_meshCount++] = third;
}

/// One ring of a lathe profile: height, radius; the texture's v runs with the distance along the profile.
struct Ring
{
    float height, radius;
};

// A turned surface through the rings (bottom to top), elliptic by depthScale (z / x), mapped onto the patch: u round
// the axis, v along the profile (one patch height per vSpan metres).
void AddLathe(const Ring* rings, int ringCount, float depthScale, Material material, Part part, float vSpan)
{
    float along = 0;
    float alongAt[64] = {};
    for (int ring = 1; ring < ringCount && ring < 64; ++ring)
    {
        const float dh = rings[ring].height - rings[ring - 1].height, dr = rings[ring].radius - rings[ring - 1].radius;
        along += sqrtf(dh * dh + dr * dr);
        alongAt[ring] = along;
    }
    for (int ring = 0; ring + 1 < ringCount && ring + 1 < 64; ++ring)
    {
        // Profile normals per ring: from the neighbours' slope (smooth across rings).
        float slopes[2][2];
        for (int end = 0; end < 2; ++end)
        {
            const int index = ring + end;
            const int below = index > 0 ? index - 1 : index, above = index + 1 < ringCount ? index + 1 : index;
            const float dh = rings[above].height - rings[below].height, dr = rings[above].radius - rings[below].radius;
            const float length = sqrtf(dh * dh + dr * dr);
            slopes[end][0] = length > 1e-6f ? dh / length : 1.0f;  // outward part
            slopes[end][1] = length > 1e-6f ? -dr / length : 0.0f; // upward part
        }
        for (int side = 0; side < SIDES; ++side)
        {
            MeshVertex corners[4];
            for (int corner = 0; corner < 4; ++corner)
            {
                const int end = corner >> 1, around = side + (corner & 1);
                const float angle = 6.2831853f * static_cast<float>(around) / SIDES;
                const float cosine = cosf(angle), sine = sinf(angle);
                const Ring& at = rings[ring + end];
                MeshVertex& vertex = corners[corner];
                vertex.position[0] = at.radius * cosine;
                vertex.position[1] = at.height;
                vertex.position[2] = at.radius * sine * depthScale;
                vertex.normal[0] = slopes[end][0] * cosine;
                vertex.normal[1] = slopes[end][1];
                vertex.normal[2] = slopes[end][0] * sine / depthScale;
                Normalize(vertex.normal);
                vertex.u = static_cast<float>(around) / SIDES;
                vertex.v = alongAt[ring + end] / vSpan;
                vertex.material = material;
                vertex.part = part;
            }
            AddTriangle(corners[0], corners[2], corners[1]);
            AddTriangle(corners[1], corners[2], corners[3]);
        }
    }
}

// A quad of the trigger blade (corners in order round it), flat-shaded with its own normal.
void AddFlatQuad(const float (*corners)[3], const float* normal)
{
    MeshVertex vertices[4];
    for (int corner = 0; corner < 4; ++corner)
    {
        MeshVertex& vertex = vertices[corner];
        memcpy(vertex.position, corners[corner], sizeof(vertex.position));
        memcpy(vertex.normal, normal, sizeof(vertex.normal));
        vertex.u = (corner == 1 || corner == 2) ? 1.0f : 0.0f;
        vertex.v = corner >= 2 ? 0.3f : 0.0f;
        vertex.material = Material::Polished;
        vertex.part = Part::Trigger;
    }
    AddTriangle(vertices[0], vertices[1], vertices[2]);
    AddTriangle(vertices[0], vertices[2], vertices[3]);
}

// The trigger: a blade 1.2 cm wide curving forward and down from its hinge, 4 mm thick, its tip curled forward.
void AddTrigger(const float* hinge)
{
    constexpr int STEPS = 8;
    constexpr float HALF_WIDTH = 0.006f, THICKNESS = 0.004f, LENGTH = 0.026f;
    float back[STEPS + 1][2], front[STEPS + 1][2]; // height, depth of the blade's rear (finger) and front faces
    for (int step = 0; step <= STEPS; ++step)
    {
        const float t = static_cast<float>(step) / STEPS;
        const float height = hinge[0] - LENGTH * t;
        const float depth = hinge[1] - 0.0105f * sinf(3.14159265f * 0.85f * t) - 0.004f * t * t;
        back[step][0] = height;
        back[step][1] = depth;
        // Front face: the thickness along the curve's outward normal (forward, -z).
        front[step][0] = height;
        front[step][1] = depth - THICKNESS;
    }
    for (int step = 0; step < STEPS; ++step)
    {
        // Normals of the faces of this stretch, in the y-z plane, from the rear curve's direction.
        const float dh = back[step + 1][0] - back[step][0], dd = back[step + 1][1] - back[step][1];
        const float length = sqrtf(dh * dh + dd * dd);
        const float outward[3] = {0, -dd / length, dh / length}; // toward the grip (+z) for the rear face
        const float forward[3] = {0, -outward[1], -outward[2]};
        const float rear[4][3] = {
            {-HALF_WIDTH, back[step][0], back[step][1]},
            {HALF_WIDTH, back[step][0], back[step][1]},
            {HALF_WIDTH, back[step + 1][0], back[step + 1][1]},
            {-HALF_WIDTH, back[step + 1][0], back[step + 1][1]}};
        AddFlatQuad(rear, outward);
        const float face[4][3] = {
            {-HALF_WIDTH, front[step][0], front[step][1]},
            {HALF_WIDTH, front[step][0], front[step][1]},
            {HALF_WIDTH, front[step + 1][0], front[step + 1][1]},
            {-HALF_WIDTH, front[step + 1][0], front[step + 1][1]}};
        AddFlatQuad(face, forward);
        for (int side = 0; side < 2; ++side)
        {
            const float x = side == 0 ? -HALF_WIDTH : HALF_WIDTH;
            const float sideNormal[3] = {side == 0 ? -1.0f : 1.0f, 0, 0};
            const float edge[4][3] = {
                {x, back[step][0], back[step][1]},
                {x, back[step + 1][0], back[step + 1][1]},
                {x, front[step + 1][0], front[step + 1][1]},
                {x, front[step][0], front[step][1]}};
            AddFlatQuad(edge, sideNormal);
        }
    }
    const float down[3] = {0, -1, 0};
    const float tip[4][3] = {
        {-HALF_WIDTH, back[STEPS][0], back[STEPS][1]},
        {HALF_WIDTH, back[STEPS][0], back[STEPS][1]},
        {HALF_WIDTH, front[STEPS][0], front[STEPS][1]},
        {-HALF_WIDTH, front[STEPS][0], front[STEPS][1]}};
    AddFlatQuad(tip, down);
}

// The vertices from `from` on belong to a model.
void TagModel(int from, Model model)
{
    for (int index = from; index < _meshCount; ++index)
    {
        _mesh[index].model = model;
    }
}

// Moves the vertices from `from` on by an offset (handle space).
void MoveFrom(int from, float x, float y, float z)
{
    for (int index = from; index < _meshCount; ++index)
    {
        _mesh[index].position[0] += x;
        _mesh[index].position[1] += y;
        _mesh[index].position[2] += z;
    }
}

// Turns the vertices from `from` on a quarter about z, so a lathe's axis (y) lies along x (the throttle's hub and
// knob): (x, y, z) -> (y, -x, z).
void LayAlongX(int from)
{
    for (int index = from; index < _meshCount; ++index)
    {
        float* p = _mesh[index].position;
        float* n = _mesh[index].normal;
        const float px = p[0], nx = n[0];
        p[0] = p[1];
        p[1] = -px;
        n[0] = n[1];
        n[1] = -nx;
    }
}

// A fighter's centre stick (idren2023 2026-10-27: a legitimate jet-fighter stick between the legs): a round floor plate
// with bolts, a ribbed rubber gaiter, a long polished shaft through a collar, a contoured grip (finger grooves in front,
// swelling under the palm) and a head with a hat switch and a pickle button on top; the trigger in front.
void BuildJetStick()
{
    const int start = _meshCount;
    const Ring plate[] = {{-0.014f, 0.078f}, {-0.004f, 0.078f}, {0.0f, 0.072f}, {0.0f, 0.0f}};
    AddLathe(plate, 4, 1.0f, Material::Metal, Part::Carriage, 0.12f);
    for (int bolt = 0; bolt < 6; ++bolt)
    {
        const int before = _meshCount;
        const Ring head[] = {{0.0f, 0.0f}, {0.0f, 0.007f}, {0.004f, 0.006f}, {0.005f, 0.0f}};
        AddLathe(head, 4, 1.0f, Material::Polished, Part::Carriage, 0.02f);
        const float angle = 6.2831853f * (bolt + 0.5f) / 6;
        MoveFrom(before, 0.062f * cosf(angle), 0, 0.062f * sinf(angle));
    }
    Ring gaiter[19];
    for (int ring = 0; ring < 19; ++ring)
    {
        const float t = static_cast<float>(ring) / 18.0f;
        const float rib = 0.5f + 0.5f * cosf(6.2831853f * 5.0f * t);
        gaiter[ring] = {t * JET_GAITER_TOP, 0.058f + t * (0.017f - 0.058f) + 0.006f * rib * (1.0f - 0.5f * t)};
    }
    AddLathe(gaiter, 19, 1.0f, Material::Rubber, Part::Grip, 0.12f);
    const Ring shaft[] = {
        {JET_GAITER_TOP - 0.004f, 0.0175f},
        {JET_GAITER_TOP + 0.004f, 0.0175f},
        {JET_GAITER_TOP + 0.008f, 0.0125f},
        {JET_SHAFT_TOP - 0.01f, 0.0115f},
        {JET_SHAFT_TOP, 0.0150f},
        {JET_GRIP_BOTTOM - 0.002f, 0.0185f},
        {JET_GRIP_BOTTOM, 0.0170f}};
    AddLathe(shaft, 7, 1.0f, Material::Polished, Part::Grip, 0.10f);
    Ring grip[25];
    for (int ring = 0; ring < 25; ++ring)
    {
        const float t = static_cast<float>(ring) / 24.0f;
        const float grooves = t < 0.70f ? 0.0022f * sinf(3.14159265f * 4.0f * t / 0.70f) : 0.0f;
        const float swell = 0.0035f * sinf(3.14159265f * t);
        grip[ring] = {JET_GRIP_BOTTOM + t * (JET_GRIP_TOP - JET_GRIP_BOTTOM), 0.0185f + swell - fabsf(grooves)};
    }
    AddLathe(grip, 25, JET_GRIP_DEPTH, Material::Rubber, Part::Grip, 0.12f);
    Ring head[11];
    for (int ring = 0; ring < 11; ++ring)
    {
        const float t = static_cast<float>(ring) / 10.0f;
        const float angle = 1.5707963f * (t < 0.4f ? 0.0f : (t - 0.4f) / 0.6f);
        head[ring] = {
            JET_GRIP_TOP + (JET_HEAD_TOP - JET_GRIP_TOP) * (t < 0.4f ? t / 0.4f * 0.4f : 0.4f + 0.6f * sinf(angle)),
            (t < 0.4f ? 0.0200f + 0.0040f * t / 0.4f : 0.0240f * cosf(angle)) + (ring == 10 ? 0.0f : 0.0003f)};
    }
    AddLathe(head, 11, 1.15f, Material::Metal, Part::Grip, 0.06f);
    // the hat switch toward the thumb (the pilot's side, +z, a little left), the pickle button ahead on the right
    const Ring hat[] = {{0.0f, 0.0f}, {0.0f, 0.0065f}, {0.007f, 0.0065f}, {0.009f, 0.0045f}, {0.009f, 0.0f}};
    int before = _meshCount;
    AddLathe(hat, 5, 1.0f, Material::Metal, Part::Grip, 0.02f);
    MoveFrom(before, -0.004f, JET_HEAD_TOP - 0.010f, 0.012f);
    const Ring pickle[] = {{0.0f, 0.0f}, {0.0f, 0.0055f}, {0.006f, 0.0055f}, {0.007f, 0.004f}, {0.007f, 0.0f}};
    before = _meshCount;
    AddLathe(pickle, 5, 1.0f, Material::Button, Part::Grip, 0.02f);
    MoveFrom(before, 0.009f, JET_HEAD_TOP - 0.008f, -0.010f);
    AddTrigger(JET_TRIGGER_HINGE);
    TagModel(start, Model::JetStick);
}

// A boat-style throttle (idren2023 2026-10-27): a round hub on its hinge across the console's slot, a polished arm
// swinging ahead and back over it, a fat crosswise rubber knob on top with metal end caps and a thumb button.
void BuildThrottle()
{
    const int start = _meshCount;
    int before = _meshCount;
    const Ring hub[] = {
        {-THROTTLE_HUB_HALF_WIDTH, 0.0f},
        {-THROTTLE_HUB_HALF_WIDTH, THROTTLE_HUB_RADIUS - 0.004f},
        {-THROTTLE_HUB_HALF_WIDTH + 0.004f, THROTTLE_HUB_RADIUS},
        {THROTTLE_HUB_HALF_WIDTH - 0.004f, THROTTLE_HUB_RADIUS},
        {THROTTLE_HUB_HALF_WIDTH, THROTTLE_HUB_RADIUS - 0.004f},
        {THROTTLE_HUB_HALF_WIDTH, 0.0f}};
    AddLathe(hub, 6, 1.0f, Material::Metal, Part::Carriage, 0.12f);
    LayAlongX(before);
    before = _meshCount;
    const Ring boss[] = {
        {-THROTTLE_HUB_HALF_WIDTH - 0.006f, 0.0f},
        {-THROTTLE_HUB_HALF_WIDTH - 0.006f, 0.020f},
        {THROTTLE_HUB_HALF_WIDTH + 0.006f, 0.020f},
        {THROTTLE_HUB_HALF_WIDTH + 0.006f, 0.0f}};
    AddLathe(boss, 4, 1.0f, Material::Polished, Part::Grip, 0.05f);
    LayAlongX(before);
    const Ring arm[] = {
        {0.0f, 0.016f}, {0.03f, 0.013f}, {THROTTLE_ARM - 0.02f, 0.010f}, {THROTTLE_ARM, 0.012f}, {THROTTLE_ARM + 0.004f, 0.0f}};
    AddLathe(arm, 5, 0.8f, Material::Polished, Part::Grip, 0.10f);
    before = _meshCount;
    Ring knob[17];
    for (int ring = 0; ring < 17; ++ring)
    {
        const float t = static_cast<float>(ring) / 16.0f;
        const float grooves = 0.0012f * fabsf(sinf(3.14159265f * 5.0f * t));
        knob[ring] = {
            -THROTTLE_KNOB_HALF_LENGTH + 2.0f * THROTTLE_KNOB_HALF_LENGTH * t,
            THROTTLE_KNOB_RADIUS * (0.86f + 0.14f * sinf(3.14159265f * t)) - grooves};
    }
    AddLathe(knob, 17, 1.0f, Material::Rubber, Part::Grip, 0.10f);
    for (int end = -1; end <= 1; end += 2)
    {
        const float x = end * THROTTLE_KNOB_HALF_LENGTH;
        const Ring cap[] = {{x, 0.0f}, {x, THROTTLE_KNOB_RADIUS * 0.9f}, {x + end * 0.006f, THROTTLE_KNOB_RADIUS * 0.75f}, {x + end * 0.008f, 0.0f}};
        if (end < 0)
        {
            const Ring flipped[] = {cap[3], cap[2], cap[1], cap[0]};
            AddLathe(flipped, 4, 1.0f, Material::Metal, Part::Grip, 0.03f);
        }
        else
        {
            AddLathe(cap, 4, 1.0f, Material::Metal, Part::Grip, 0.03f);
        }
    }
    LayAlongX(before);
    MoveFrom(before, 0, LeverModel::THROTTLE_GRIP_METERS, 0);
    const Ring thumb[] = {{0.0f, 0.0f}, {0.0f, 0.0060f}, {0.006f, 0.0060f}, {0.007f, 0.0045f}, {0.007f, 0.0f}};
    before = _meshCount;
    AddLathe(thumb, 5, 1.0f, Material::Button, Part::Grip, 0.02f);
    MoveFrom(before, 0.028f, LeverModel::THROTTLE_GRIP_METERS + THROTTLE_KNOB_RADIUS - 0.003f, 0.004f);
    TagModel(start, Model::Throttle);
}

// The whole handle, once.
void BuildMesh()
{
    // Carriage: an oval block, longer along the track, with a bevelled top edge, closed on top; a round lip round the
    // boot's foot.
    const Ring carriage[] = {
        {-CARRIAGE_HEIGHT, CARRIAGE_HALF_WIDTH},
        {-0.007f, CARRIAGE_HALF_WIDTH},
        {-0.002f, CARRIAGE_HALF_WIDTH - 0.004f},
        {0.0f, CARRIAGE_HALF_WIDTH - 0.010f},
        {0.0f, 0.0f}};
    AddLathe(
        carriage,
        sizeof(carriage) / sizeof(carriage[0]),
        CARRIAGE_DEPTH_SCALE,
        Material::Metal,
        Part::Carriage,
        0.12f);
    const Ring lip[] = {{0.0005f, 0.026f}, {0.004f, 0.024f}, {0.004f, 0.021f}, {0.0005f, 0.019f}};
    AddLathe(lip, sizeof(lip) / sizeof(lip[0]), 1.0f, Material::Polished, Part::Carriage, 0.05f);
    // Boot: ribbed rubber, from inside the lip up to the collar under the grip.
    Ring boot[13];
    for (int ring = 0; ring < 13; ++ring)
    {
        const float t = static_cast<float>(ring) / 12.0f;
        const float rib = 0.5f + 0.5f * cosf(6.2831853f * 3.0f * t);
        boot[ring] = {t * BOOT_TOP, 0.019f + t * (0.0130f - 0.019f) + 0.0028f * rib * (1.0f - 0.4f * t)};
    }
    AddLathe(boot, 13, 1.0f, Material::Rubber, Part::Grip, 0.12f);
    // Collar.
    const Ring collar[] = {
        {BOOT_TOP - 0.002f, 0.0125f},
        {SHAFT_TOP, 0.0145f},
        {SHAFT_TOP + 0.002f, 0.0165f},
        {GRIP_BOTTOM - 0.003f, 0.0165f},
        {GRIP_BOTTOM - 0.001f, 0.0150f},
        {GRIP_BOTTOM, 0.0150f}};
    AddLathe(collar, sizeof(collar) / sizeof(collar[0]), 1.0f, Material::Polished, Part::Grip, 0.05f);
    // Grip: oval rubber with three finger grooves on the way up, swelling under the palm toward the top.
    Ring grip[25];
    for (int ring = 0; ring < 25; ++ring)
    {
        const float t = static_cast<float>(ring) / 24.0f;
        const float grooves = t < 0.62f ? 0.0021f * sinf(3.14159265f * 3.0f * t / 0.62f) : 0.0f;
        const float swell = 0.0028f * sinf(3.14159265f * t);
        grip[ring] = {GRIP_BOTTOM + t * (GRIP_TOP - GRIP_BOTTOM), 0.0168f + swell - fabsf(grooves)};
    }
    AddLathe(grip, 25, 1.18f, Material::Rubber, Part::Grip, 0.12f);
    // Cap: a metal dome.
    Ring cap[9];
    for (int ring = 0; ring < 9; ++ring)
    {
        const float angle = 1.5707963f * static_cast<float>(ring) / 8.0f;
        cap[ring] = {
            GRIP_TOP + (CAP_TOP - GRIP_TOP) * sinf(angle),
            0.0185f * cosf(angle) + (ring == 8 ? 0.0f : 0.0004f)};
    }
    AddLathe(cap, 9, 1.18f, Material::Metal, Part::Grip, 0.06f);
    // Thumb button: a short cylinder standing on the cap, toward the player.
    const int before = _meshCount;
    const Ring button[] = {{0.0f, 0.0f}, {0.0f, 0.0058f}, {0.006f, 0.0058f}, {0.007f, 0.0045f}, {0.007f, 0.0f}};
    AddLathe(button, sizeof(button) / sizeof(button[0]), 1.0f, Material::Button, Part::Grip, 0.02f);
    for (int index = before; index < _meshCount; ++index)
    {
        _mesh[index].position[1] += CAP_TOP - 0.006f;
        _mesh[index].position[2] += 0.009f;
    }
    AddTrigger(TRIGGER_HINGE);
    TagModel(0, Model::Tank);
    BuildJetStick();
    BuildThrottle();
    Log::Info(
        "levers",
        "handle mesh: %d triangles%s",
        _meshCount / 3,
        _meshCount + 3 > MOST_VERTICES ? " (cut: too many)" : "");
}

const Style& StyleOf(const char* stockName)
{
    if (stockName)
    {
        for (const Style& style : STYLES)
        {
            if (!strcmp(style.odfName, stockName))
            {
                return style;
            }
        }
    }
    return DEFAULT_STYLE;
}

// The game's loaded texture of that name, or null (Textures::FindByName).
IDirect3DTexture9* FindTexture(const char* name)
{
    return static_cast<IDirect3DTexture9*>(Textures::FindByName(name));
}

D3DCOLOR Gray(float level)
{
    const DWORD value = static_cast<DWORD>(level * 255.0f);
    return 0xFF000000 | (value << 16) | (value << 8) | value;
}

D3DCOLORVALUE ColorValue(D3DCOLOR color, float scale)
{
    return {
        ((color >> 16) & 0xFF) / 255.0f * scale,
        ((color >> 8) & 0xFF) / 255.0f * scale,
        (color & 0xFF) / 255.0f * scale,
        1.0f};
}

// A material's Direct3D material. With the texture (2x modulation) the lit colour is halved, so the patch at middle
// grey shows the style's colour; the button glows in its own colour.
D3DMATERIAL9 MaterialOf(const Style& style, Material material, bool isTextured)
{
    const float half = isTextured ? 0.5f : 1.0f;
    D3DMATERIAL9 result = {};
    switch (material)
    {
    case Material::Metal:
        result.Diffuse = result.Ambient = ColorValue(style.metal, half);
        result.Specular = {0.42f, 0.42f, 0.42f, 1};
        result.Power = 24;
        break;
    case Material::Polished:
        result.Diffuse = result.Ambient = ColorValue(style.metal, 1.15f * half);
        result.Specular = {0.65f, 0.65f, 0.65f, 1};
        result.Power = 48;
        break;
    case Material::Rubber:
        result.Diffuse = result.Ambient = ColorValue(style.rubber, half);
        result.Specular = {0.10f, 0.10f, 0.10f, 1};
        result.Power = 8;
        break;
    case Material::Button:
        result.Diffuse = result.Ambient = ColorValue(style.button, 0.25f * half);
        result.Emissive = ColorValue(style.button, 0.75f * half);
        break;
    }
    return result;
}

void ReleaseBuffer()
{
    if (_vertexBuffer)
    {
        _vertexBuffer->lpVtbl->Release(_vertexBuffer);
        _vertexBuffer = nullptr;
    }
    _bufferStyle = nullptr;
}

// The mesh into the vertex buffer, grouped by part and material, the style's patch baked into its coordinates.
bool FillBuffer(IDirect3DDevice9* device, const Style& style)
{
    if (_meshCount == 0)
    {
        BuildMesh();
    }
    if (!_vertexBuffer && FAILED(device->lpVtbl->CreateVertexBuffer(
                              device,
                              static_cast<UINT>(_meshCount * sizeof(Vertex)),
                              D3DUSAGE_WRITEONLY,
                              VERTEX_FORMAT,
                              D3DPOOL_MANAGED,
                              &_vertexBuffer,
                              nullptr)))
    {
        _vertexBuffer = nullptr;
        Log::Warning("levers", "lever vertex buffer not created: levers not drawn");
        return false;
    }
    Vertex* out = nullptr;
    if (FAILED(_vertexBuffer->lpVtbl->Lock(_vertexBuffer, 0, 0, reinterpret_cast<void**>(&out), 0)))
    {
        return false;
    }
    const float patchWidth = style.patch[2] - style.patch[0], patchHeight = style.patch[3] - style.patch[1];
    const Part parts[3] = {Part::Carriage, Part::Grip, Part::Trigger};
    const Material materials[4] = {Material::Metal, Material::Polished, Material::Rubber, Material::Button};
    const Model models[3] = {Model::Tank, Model::JetStick, Model::Throttle};
    int written = 0;
    _groupCount = 0;
    for (const Model model : models)
    for (const Part part : parts)
    {
        for (const Material material : materials)
        {
            const int groupStart = written;
            for (int index = 0; index < _meshCount; ++index)
            {
                const MeshVertex& mesh = _mesh[index];
                if (mesh.model != model || mesh.part != part || mesh.material != material)
                {
                    continue;
                }
                const float v = mesh.v < 0 ? 0.0f : mesh.v > 1 ? 1.0f : mesh.v;
                out[written++] = {
                    mesh.position[0],
                    mesh.position[1],
                    mesh.position[2],
                    mesh.normal[0],
                    mesh.normal[1],
                    mesh.normal[2],
                    style.patch[0] + patchWidth * mesh.u,
                    style.patch[1] + patchHeight * v};
            }
            if (written > groupStart && _groupCount < MOST_GROUPS)
            {
                _groups[_groupCount++] = {model, part, material, groupStart, written - groupStart};
            }
        }
    }
    _vertexBuffer->lpVtbl->Unlock(_vertexBuffer);
    _bufferStyle = &style;
    return true;
}

} // namespace

void Draw(
    void* device,
    const float (*carriages)[16],
    const float (*grips)[16],
    const float* triggerPulls,
    int count,
    const char* stockName,
    const Model* models)
{
    if (!device || count <= 0)
    {
        return;
    }
    LARGE_INTEGER start, end, frequency;
    QueryPerformanceCounter(&start);
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    if (_stateBlockDevice != d3dDevice)
    {
        OnReset();
        ReleaseBuffer();
        _stateBlockDevice = d3dDevice;
    }
    const Style& style = StyleOf(stockName);
    IDirect3DTexture9* texture = FindTexture(style.textureName);
    if (_loggedStyle != &style)
    {
        _loggedStyle = &style;
        Log::Info(
            "levers",
            "lever look: %s, texture %s %s",
            stockName ? stockName : "(not stock)",
            style.textureName ? style.textureName : "-",
            texture ? "found" : "not loaded (plain colours)");
    }
    if ((_bufferStyle != &style || !_vertexBuffer) && !FillBuffer(d3dDevice, style))
    {
        return;
    }
    if (!_savedState && FAILED(d3dDevice->lpVtbl->CreateStateBlock(d3dDevice, D3DSBT_ALL, &_savedState)))
    {
        _savedState = nullptr;
        return;
    }
    if (FAILED(_savedState->lpVtbl->Capture(_savedState)))
    {
        return;
    }
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetFVF(d3dDevice, VERTEX_FORMAT);
    d3dDevice->lpVtbl->SetStreamSource(d3dDevice, 0, _vertexBuffer, 0, sizeof(Vertex));
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(texture));
    d3dDevice->lpVtbl
        ->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, texture ? D3DTOP_MODULATE2X : D3DTOP_SELECTARG2);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_TEXCOORDINDEX, 0);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LIGHTING, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_AMBIENT, Gray(AMBIENT));
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SPECULARENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LOCALVIEWER, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_NORMALIZENORMALS, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_COLORVERTEX, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_FOGENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHATESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_STENCILENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRGBWRITEENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_COLORWRITEENABLE, 0xF);
    for (int light = 2; light < 8; ++light)
    {
        d3dDevice->lpVtbl->LightEnable(d3dDevice, light, FALSE);
    }
    int drawCount = 0;
    {
        FpGuard fp;
        for (int lever = 0; lever < count; ++lever)
        {
            const Model model = models ? models[lever] : Model::Tank;
            // The lights, fixed to the hull (the carriage frame's axes); Direct3D's direction points from the light.
            const float* hull = carriages[lever];
            const float* lights[2] = {KEY_LIGHT, FILL_LIGHT};
            const float strengths[2] = {KEY, FILL};
            for (int light = 0; light < 2; ++light)
            {
                float toward[3];
                for (int axis = 0; axis < 3; ++axis)
                {
                    toward[axis] = lights[light][0] * hull[axis] + lights[light][1] * hull[4 + axis] +
                                   lights[light][2] * hull[8 + axis];
                }
                Normalize(toward);
                D3DLIGHT9 parameters = {};
                parameters.Type = D3DLIGHT_DIRECTIONAL;
                parameters.Diffuse = {strengths[light], strengths[light], strengths[light], 1};
                parameters.Specular = light == 0 ? D3DCOLORVALUE{1.0f, 0.98f, 0.95f, 1} : D3DCOLORVALUE{0, 0, 0, 1};
                parameters.Direction = {-toward[0], -toward[1], -toward[2]};
                d3dDevice->lpVtbl->SetLight(d3dDevice, light, &parameters);
                d3dDevice->lpVtbl->LightEnable(d3dDevice, light, TRUE);
            }
            // The trigger's frame: turned about its hinge (grip x), its lower end back toward the grip, then the
            // grip.
            const float pull = triggerPulls[lever] < 0 ? 0.0f : triggerPulls[lever] > 1 ? 1.0f : triggerPulls[lever];
            const float cosine = cosf(-pull * TRIGGER_PULL_RADIANS), sine = sinf(-pull * TRIGGER_PULL_RADIANS);
            const float* hingeAt = model == Model::JetStick ? JET_TRIGGER_HINGE : TRIGGER_HINGE;
            const float hy = hingeAt[0], hz = hingeAt[1];
            // translate(-hinge) x rotate x translate(hinge), row vectors: y' = y c - z s, z' = y s + z c.
            const float hinge[16] = {
                1,
                0,
                0,
                0,
                0,
                cosine,
                sine,
                0,
                0,
                -sine,
                cosine,
                0,
                0,
                hy - (hy * cosine - hz * sine),
                hz - (hy * sine + hz * cosine),
                1};
            float trigger[16];
            for (int row = 0; row < 4; ++row)
            {
                for (int column = 0; column < 4; ++column)
                {
                    float sum = 0;
                    for (int inner = 0; inner < 4; ++inner)
                    {
                        sum += hinge[row * 4 + inner] * grips[lever][inner * 4 + column];
                    }
                    trigger[row * 4 + column] = sum;
                }
            }
            for (int group = 0; group < _groupCount; ++group)
            {
                const Group& at = _groups[group];
                if (at.model != model)
                {
                    continue;
                }
                const float* frame = at.part == Part::Carriage ? carriages[lever]
                                     : at.part == Part::Grip   ? grips[lever]
                                                               : trigger;
                D3DMATRIX world;
                memcpy(&world, frame, sizeof(world));
                d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_WORLD, &world);
                const D3DMATERIAL9 material = MaterialOf(style, at.material, texture != nullptr);
                d3dDevice->lpVtbl->SetMaterial(d3dDevice, &material);
                d3dDevice->lpVtbl->DrawPrimitive(d3dDevice, D3DPT_TRIANGLELIST, at.start, at.count / 3);
                ++drawCount;
            }
        }
    }
    _savedState->lpVtbl->Apply(_savedState);
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);
    const double milliseconds =
        static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    _costMilliseconds += milliseconds;
    _costMaximumMilliseconds = milliseconds > _costMaximumMilliseconds ? milliseconds : _costMaximumMilliseconds;
    if (++_costDraws >= COST_LOG_DRAWS)
    {
        Log::Info(
            "levers",
            "lever draw cost: %.3f ms per eye pass on average, %.3f at most (%d triangles, %d draw calls, %d passes)",
            _costMilliseconds / _costDraws,
            _costMaximumMilliseconds,
            _meshCount / 3,
            drawCount,
            _costDraws);
        _costMilliseconds = _costMaximumMilliseconds = 0;
        _costDraws = 0;
    }
}

int PreviewMesh(Model model, float (*positions)[3], int* kinds, int most)
{
    if (_meshCount == 0)
    {
        BuildMesh();
    }
    int count = 0;
    for (int index = 0; index < _meshCount && count < most; ++index)
    {
        if (_mesh[index].model == model)
        {
            memcpy(positions[count], _mesh[index].position, sizeof(positions[count]));
            kinds[count++] = static_cast<int>(_mesh[index].part) * 4 + static_cast<int>(_mesh[index].material);
        }
    }
    return count;
}

void OnReset()
{
    if (_savedState)
    {
        _savedState->lpVtbl->Release(_savedState);
        _savedState = nullptr;
    }
    _stateBlockDevice = nullptr; // the managed vertex buffer stays (it outlives a reset), until the device changes
}

} // namespace LeverModel

} // namespace SwbfVr
