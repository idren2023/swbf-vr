// ReloadZone.cpp: the manual reload's insert zone as a red hologram under the gun (see ReloadZone.h).

// d3d9.h's interfaces as C structs with an explicit vtable, the form the rest of the mod uses (before any include).
#define CINTERFACE

#include "ReloadZone.h"

#include "FpGuard.h"
#include "Log.h"
#include "Stereo.h"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstring>

namespace SwbfVr
{

namespace ReloadZone
{

namespace
{

struct Vertex
{
    float x, y, z;
    DWORD colour;
};
constexpr DWORD VERTEX_FORMAT = D3DFVF_XYZ | D3DFVF_DIFFUSE;

/// The hologram: a rectangular pyramid standing on its tip, the tip in the gun a little ahead of the trigger finger,
/// opening downward (the gun's own down), as a beam that draws energy up into the gun (idren2023 2026-11-10).
/// The tip from the gun hand's grip pose (metres along the gun's forward and up; run 719: the pose's origin is about
/// 1.5 cm ahead of the trigger and 6 cm below it), the pyramid's height, and the half
/// size of its open end along the gun and across it.
constexpr float TIP_FORWARD = 0.135f, TIP_UP = 0.08f, HEIGHT = 0.24f, END_HALF_LONG = 0.085f, END_HALF_WIDE = 0.055f;
/// Colours: the body the red of the canister (item_powerup_ammo's red, about 214 33 31) and of the ammo pickup
/// effect (powerup_red.fx: pure red flares), and the hot colour of what is close to the tip (a flare's core).
constexpr float RED[3] = {0.92f, 0.09f, 0.08f}, HOT[3] = {1.0f, 0.38f, 0.36f};
/// The same pair while the game reloads (idren2023 2026-11-10: green from the canister let go in the zone until the
/// reload is done).
constexpr float GREEN[3] = {0.10f, 0.90f, 0.22f}, GREEN_HOT[3] = {0.55f, 1.0f, 0.62f};
/// Fine level lines over the four faces (a hologram's scan lines): how many, and the part of a step each fills.
constexpr int FINE_LINES = 38;
constexpr float FINE_LINE_PART = 0.28f;
/// Bands that rise from the open end into the tip, ever faster (the energy drawn in): how many, how many arrive
/// per second and level, and a band's height as a part of the pyramid's.
constexpr int BANDS = 4;
constexpr float BANDS_PER_SECOND[5] = {0, 0.45f, 0.8f, 2.0f, 2.0f};
constexpr float BAND_PART = 0.035f;
/// Strengths per level: the faces at the tip (they fade to nothing at the open end), the fine lines, the edges, the
/// rising bands.
constexpr float FACE_ALPHA[5] = {0, 0.10f, 0.16f, 0.26f, 0.26f};
constexpr float LINE_ALPHA[5] = {0, 0.16f, 0.26f, 0.40f, 0.40f};
constexpr float EDGE_ALPHA[5] = {0, 0.45f, 0.70f, 0.95f, 0.95f};
constexpr float BAND_ALPHA[5] = {0, 0.50f, 0.75f, 1.00f, 1.00f};
constexpr float EDGE_WIDTH_METERS = 0.0025f;
/// The canister's hologram (idren2023 2026-11-10): the canister's own model, half transparent (drawn by Holster at the
/// place GetCanister gives), inside a hologram layer drawn here (an eight-sided shell of its size, 6.6 cm across and
/// 12.9 cm tall, with four bright rings), that rises from the open end into the gun, then starts again at
/// the open end. Runs per second and level, its strength per level, how far up the pyramid its middle gets (part of
/// the height from the tip), the size it shrinks to on the way, and the height of a ring.
constexpr int CANISTER_SIDES = 8;
constexpr float CANISTER_HALF_WIDE = 0.033f, CANISTER_HALF_TALL = 0.0645f;
constexpr float CANISTER_RUNS_PER_SECOND[5] = {0, 0.45f, 0.55f, 0.9f, 0.9f};
constexpr float CANISTER_ALPHA[5] = {0, 0.40f, 0.60f, 0.85f, 0.85f};
constexpr float CANISTER_END_PART = 0.10f, CANISTER_END_SIZE = 0.6f, CANISTER_RING_METERS = 0.004f;
constexpr float CANISTER_RINGS[4] = {-1.0f, -0.55f, 0.55f, 1.0f};
constexpr float PI = 3.14159265f;

constexpr int MOST_VERTICES = (FINE_LINES + BANDS + 8) * 4 * 6 + 64 + CANISTER_SIDES * 6 * 7;
Vertex _vertices[MOST_VERTICES];
int _vertexCount = 0;
IDirect3DStateBlock9* _savedState = nullptr;
IDirect3DDevice9* _stateBlockDevice = nullptr;
int _forcedLevel = 0;

DWORD Colour(const float* rgb, float alpha)
{
    alpha = alpha < 0 ? 0 : alpha > 1 ? 1 : alpha;
    return (static_cast<DWORD>(alpha * 255.0f) << 24) | (static_cast<DWORD>(rgb[0] * 255.0f) << 16) |
           (static_cast<DWORD>(rgb[1] * 255.0f) << 8) | static_cast<DWORD>(rgb[2] * 255.0f);
}

void Quad(const float* a, const float* b, const float* c, const float* d, DWORD colour)
{
    if (_vertexCount + 6 > MOST_VERTICES)
    {
        return;
    }
    const float* corners[6] = {a, b, c, a, c, d};
    for (const float* corner : corners)
    {
        _vertices[_vertexCount++] = {corner[0], corner[1], corner[2], colour};
    }
}

void Triangle(const float* a, const float* b, const float* c, DWORD colourA, DWORD colourB, DWORD colourC)
{
    if (_vertexCount + 3 > MOST_VERTICES)
    {
        return;
    }
    _vertices[_vertexCount++] = {a[0], a[1], a[2], colourA};
    _vertices[_vertexCount++] = {b[0], b[1], b[2], colourB};
    _vertices[_vertexCount++] = {c[0], c[1], c[2], colourC};
}

// The pyramid of this frame: its tip, and for each of the four corners the vector from the tip to that corner of
// the open end.
float _tip[3], _toCorner[4][3];

// The point on the edge to corner `corner` at the part `part` of the height (0 = the tip, 1 = the open end).
void OnEdge(int corner, float part, float* point)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        point[axis] = _tip[axis] + _toCorner[corner & 3][axis] * part;
    }
}

// A level band round the pyramid between two parts of its height, one quad on each face.
void Band(float fromPart, float toPart, DWORD colour)
{
    fromPart = fromPart < 0 ? 0 : fromPart;
    toPart = toPart > 1 ? 1 : toPart;
    if (toPart <= fromPart)
    {
        return;
    }
    for (int face = 0; face < 4; ++face)
    {
        float a[3], b[3], c[3], d[3];
        OnEdge(face, fromPart, a);
        OnEdge(face + 1, fromPart, b);
        OnEdge(face + 1, toPart, c);
        OnEdge(face, toPart, d);
        Quad(a, b, c, d, colour);
    }
}

// The canister's run at this time: its middle's place along the pyramid (part of the height from the tip), its size
// (1 = the canister's own) and its strength (0..1 of the level's).
void CanisterRun(int index, float seconds, float* part, float* size, float* fade)
{
    const float phase = fmodf(seconds * CANISTER_RUNS_PER_SECOND[index], 1.0f);
    *part = 1.0f - (1.0f - CANISTER_END_PART) * phase * phase;
    *size = 1.0f - (1.0f - CANISTER_END_SIZE) * phase;
    const float fadeIn = phase / 0.12f, fadeOut = (1.0f - phase) / 0.15f;
    *fade = fadeIn < fadeOut ? (fadeIn < 1.0f ? fadeIn : 1.0f) : (fadeOut < 1.0f ? fadeOut : 1.0f);
}

float Seconds()
{
    return static_cast<float>(GetTickCount() % 600000) * 0.001f;
}

} // namespace

bool GetCanister(const float* gunWorld, Level level, float* matrix, float* size, float* strength)
{
    const int index = static_cast<int>(level);
    if (index < 1 || index > 4 || !gunWorld)
    {
        return false;
    }
    FpGuard fp;
    float part = 0, fade = 0;
    CanisterRun(index, Seconds(), &part, size, &fade);
    *strength = CANISTER_ALPHA[index] * fade;
    for (int row = 0; row < 3; ++row)
    {
        const float* from = gunWorld + row * 4;
        const float length = sqrtf(from[0] * from[0] + from[1] * from[1] + from[2] * from[2]);
        for (int axis = 0; axis < 3; ++axis)
        {
            matrix[row * 4 + axis] = length > 1e-6f ? from[axis] / length : (row == axis ? 1.0f : 0.0f);
        }
        matrix[row * 4 + 3] = 0;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        // The tip (forward = minus the third row), then down the pyramid's middle line.
        matrix[12 + axis] = gunWorld[12 + axis] - matrix[8 + axis] * TIP_FORWARD + matrix[4 + axis] * TIP_UP -
                            matrix[4 + axis] * HEIGHT * part;
    }
    matrix[15] = 1;
    return true;
}

void Draw(void* device, const float* gunWorld, Level level)
{
    const int index = static_cast<int>(level);
    if (!device || !gunWorld || index <= 0 || index > 4 || Stereo::IsScopePass())
    {
        return;
    }
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    if (_stateBlockDevice != d3dDevice)
    {
        OnReset();
        _stateBlockDevice = d3dDevice;
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
    {
        FpGuard fp;
        // The gun hand as unit vectors (whatever the scale of the matrix): right, up, and forward.
        float right[3], up[3], forward[3];
        const float* rows[3] = {gunWorld, gunWorld + 4, gunWorld + 8};
        float* axes[3] = {right, up, forward};
        for (int row = 0; row < 3; ++row)
        {
            const float length =
                sqrtf(rows[row][0] * rows[row][0] + rows[row][1] * rows[row][1] + rows[row][2] * rows[row][2]);
            for (int axis = 0; axis < 3; ++axis)
            {
                axes[row][axis] = length > 1e-6f ? rows[row][axis] / length : (row == axis ? 1.0f : 0.0f);
            }
        }
        // The matrix has the hand's back as its third row (run 719: the tip came out behind the grip).
        for (float& part : forward)
        {
            part = -part;
        }
        const float signs[4][2] = {{1, 1}, {1, -1}, {-1, -1}, {-1, 1}}; // along the gun, across it: round the end
        for (int axis = 0; axis < 3; ++axis)
        {
            _tip[axis] = gunWorld[12 + axis] + forward[axis] * TIP_FORWARD + up[axis] * TIP_UP;
            for (int corner = 0; corner < 4; ++corner)
            {
                _toCorner[corner][axis] = -up[axis] * HEIGHT + forward[axis] * END_HALF_LONG * signs[corner][0] +
                                          right[axis] * END_HALF_WIDE * signs[corner][1];
            }
        }
        const float* const RED = index == 4 ? SwbfVr::ReloadZone::GREEN : SwbfVr::ReloadZone::RED;
        const float* const HOT = index == 4 ? GREEN_HOT : SwbfVr::ReloadZone::HOT;
        const float seconds = Seconds();
        _vertexCount = 0;
        // The faces: a red haze, strongest at the tip, gone at the open end.
        const DWORD faceTip = Colour(RED, FACE_ALPHA[index]), faceEnd = Colour(RED, 0);
        for (int face = 0; face < 4; ++face)
        {
            float a[3], b[3];
            OnEdge(face, 1.0f, a);
            OnEdge(face + 1, 1.0f, b);
            Triangle(_tip, a, b, faceTip, faceEnd, faceEnd);
        }
        // The fine level lines, fading toward the open end.
        for (int line = 1; line <= FINE_LINES; ++line)
        {
            const float part = static_cast<float>(line) / (FINE_LINES + 1);
            Band(part, part + FINE_LINE_PART / (FINE_LINES + 1), Colour(RED, LINE_ALPHA[index] * (1.0f - part) * (1.0f - part * 0.5f)));
        }
        // The four edges: thin strips in both faces that meet at each edge, hot at the tip.
        for (int corner = 0; corner < 4; ++corner)
        {
            for (int side = 0; side < 2; ++side)
            {
                const int other = side ? corner + 1 : corner + 3;
                float end[3], toward[3], endInner[3], length = 0;
                OnEdge(corner, 1.0f, end);
                for (int axis = 0; axis < 3; ++axis)
                {
                    toward[axis] = _toCorner[other & 3][axis] - _toCorner[corner][axis];
                    length += toward[axis] * toward[axis];
                }
                length = sqrtf(length);
                for (int axis = 0; axis < 3; ++axis)
                {
                    endInner[axis] = end[axis] + toward[axis] / length * EDGE_WIDTH_METERS * 2.0f;
                }
                Triangle(_tip, end, endInner, Colour(HOT, EDGE_ALPHA[index]), Colour(RED, 0), Colour(RED, 0));
            }
        }
        // The rising bands: from the open end to the tip, faster and hotter on the way (part = 1 - phase squared).
        for (int band = 0; band < BANDS; ++band)
        {
            const float phase = fmodf(seconds * BANDS_PER_SECOND[index] + static_cast<float>(band) / BANDS, 1.0f);
            const float part = 1.0f - phase * phase;
            const float colour[3] = {RED[0] + (HOT[0] - RED[0]) * phase, RED[1] + (HOT[1] - RED[1]) * phase,
                                     RED[2] + (HOT[2] - RED[2]) * phase};
            const float fadeIn = 0.06f + 0.94f * phase; // brightest at the gun, all but gone at the open end
            Band(part, part + BAND_PART * (0.4f + 0.6f * part), Colour(colour, BAND_ALPHA[index] * fadeIn));
        }
        // The canister's hologram: up the pyramid's middle line into the gun, ever faster, then again from the open end.
        // Also while the game reloads, in green (idren2023 2026-11-10).
        if (index <= 4)
        {
            float part = 0, size = 0, fade = 0;
            CanisterRun(index, seconds, &part, &size, &fade);
            const float strength = CANISTER_ALPHA[index] * fade;
            const float halfTall = CANISTER_HALF_TALL * size;
            // The point of the can at corner `corner` of its outline and the height `height` over its middle.
            const auto onCan = [&](int corner, float height, float radius, float* point) {
                const float angle = 2.0f * PI * (static_cast<float>(corner) + 0.5f) / CANISTER_SIDES;
                const float across = cosf(angle) * radius, along = sinf(angle) * radius;
                for (int axis = 0; axis < 3; ++axis)
                {
                    point[axis] = _tip[axis] - up[axis] * (HEIGHT * part - height) + right[axis] * across +
                                  forward[axis] * along;
                }
            };
            const float radius = CANISTER_HALF_WIDE * size;
            for (int side = 0; side < CANISTER_SIDES; ++side)
            {
                float a[3], b[3], c[3], d[3];
                onCan(side, -halfTall, radius, a);
                onCan(side + 1, -halfTall, radius, b);
                onCan(side + 1, halfTall, radius, c);
                onCan(side, halfTall, radius, d);
                Quad(a, b, c, d, Colour(RED, strength * 0.14f)); // a faint shell over the model
                for (const float ring : CANISTER_RINGS)
                {
                    const float from = ring * halfTall - (ring > 0 ? CANISTER_RING_METERS * size : 0);
                    const float to = from + CANISTER_RING_METERS * size;
                    onCan(side, from, radius * 1.02f, a);
                    onCan(side + 1, from, radius * 1.02f, b);
                    onCan(side + 1, to, radius * 1.02f, c);
                    onCan(side, to, radius * 1.02f, d);
                    Quad(a, b, c, d, Colour(HOT, strength));
                }
            }
        }
    }
    // Blended over the picture (not added: that turned orange over grass, run 713), hidden by what is nearer (the gun, the hands), writing no depth.
    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
    d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_WORLD, &identity);
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetFVF(d3dDevice, VERTEX_FORMAT);
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, nullptr);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LIGHTING, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_FOGENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA); // (added light turned orange over grass, run 713)
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHATESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_STENCILENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_COLORWRITEENABLE, 0x7); // the colour only (alpha is the HUD's)
    if (_vertexCount >= 3)
    {
        d3dDevice->lpVtbl->DrawPrimitiveUP(
            d3dDevice, D3DPT_TRIANGLELIST, static_cast<UINT>(_vertexCount / 3), _vertices, sizeof(Vertex));
    }
    _savedState->lpVtbl->Apply(_savedState);
    static Level lastLevel = Level::None;
    if (level != lastLevel)
    {
        lastLevel = level;
        Log::Info("reloadzone", "zone drawn at level %d (%d triangles) at %.2f %.2f %.2f", index, _vertexCount / 3,
                  static_cast<double>(gunWorld[12]), static_cast<double>(gunWorld[13]), static_cast<double>(gunWorld[14]));
    }
}

void OnReset()
{
    if (_savedState)
    {
        _savedState->lpVtbl->Release(_savedState);
        _savedState = nullptr;
    }
    _stateBlockDevice = nullptr;
}

void SetForced(int level)
{
    _forcedLevel = level < 0 ? 0 : level > 4 ? 4 : level;
    Log::Info("reloadzone", "test: zone forced to level %d", _forcedLevel);
}

Level GetForced()
{
    return static_cast<Level>(_forcedLevel);
}

} // namespace ReloadZone

} // namespace SwbfVr
