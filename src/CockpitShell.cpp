// CockpitShell.cpp: drawing the cockpit shells (see CockpitShell.h).
//
// The shell of the current stock tank is built once (ShellDesigns, about 11 000 triangles), copied into a managed
// vertex buffer grouped by finish and then drawn each eye pass with the cockpit model's own world matrix: one draw per
// finish, Direct3D's fixed-function lighting (as LeverModel): a key light from above and ahead fixed to the cockpit,
// a weak fill, a strong ambient (the game's cockpit textures carry their own baked shading), the finish's highlight;
// lamps and screens self-lit. Textured finishes take the cockpit's own texture or its effects texture (screens and
// lamps), modulated 2x by the lit colour.

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable (device->lpVtbl->Method(device,
// ...)), the form the rest of the mod uses. Has to be a macro: d3d9.h reads it.
#define CINTERFACE

#include "CockpitShell.h"

#include "D3dSurvey.h"
#include "FpGuard.h"
#include "GameTextures.h"
#include "Hud.h"
#include "HudDraws.h"
#include "Log.h"
#include "ShellDesigns.h"
#include "Stereo.h"
#include "Textures.h"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstring>

namespace SwbfVr
{

namespace CockpitShell
{

namespace
{

/// Room for the largest shell (three vertices a triangle): 60 000 triangles (30 000 until 2026-10-19; our own buffer,
/// not an engine limit: the shells are drawn by us, one draw per finish and part; 5.8 MB managed).
constexpr int MOST_VERTICES = 225000; // 75 000 triangles (idren2023 2026-10-27: the TIE bomber with its bombardier station)
/// The draw cost is logged once this many draws were timed.
constexpr int COST_LOG_DRAWS = 1000;
/// The glass pass's alpha floor (0..255) added to a window texture's own alpha (idren2023 2026-10-27).
constexpr DWORD GLASS_ALPHA_FLOOR = 0x22;
/// Lights, cockpit model space before normalising (toward the light), and their strength.
constexpr float KEY_LIGHT[3] = {0.3f, 1.0f, -0.55f}, FILL_LIGHT[3] = {-0.4f, -0.5f, 0.6f};
// The game draws its cockpit textures at about their own brightness (run237: the shell at 0.62 ambient looked dark
// beside the dashboard), so the light here stays near 1 with a modest key for shape.
constexpr float AMBIENT = 0.85f, KEY = 0.32f, FILL = 0.08f;

struct Vertex
{
    float x, y, z;
    float normalX, normalY, normalZ;
    float u, v;
};

constexpr DWORD VERTEX_FORMAT = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;

/// The HUD on the displays: the game's HUD alpha (about 0.4) raised, as on the hand, against the dark glass; room for
/// the vertices of one batch (the minimap's largest has a few dozen).
constexpr float DISPLAY_ALPHA = 2.0f;
constexpr int MOST_HUD_VERTICES = 3072;
/// HasDisplays holds this long after the last draw.
constexpr DWORD DISPLAYS_HOLD_MILLISECONDS = 250;

struct HudVertex
{
    float x, y, z;
    D3DCOLOR colour;
    float u, v, maskU, maskV;
};

constexpr DWORD HUD_FORMAT = D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2;

struct Group
{
    int part, finish, start, count;
};

/// Most moving parts a design has (a gunner's turret: base, cradle, barrel) and most groups (part x finish).
constexpr int MOST_PARTS = 6, MOST_GROUPS = 128;

ShellKit::Vertex _built[MOST_VERTICES];
ShellDesigns::Shell _shell = {};
Group _groups[MOST_GROUPS];
int _groupCount = 0;
const char* _builtFor = nullptr; // the stock name the buffer holds
IDirect3DVertexBuffer9* _vertexBuffer = nullptr;
IDirect3DStateBlock9* _savedState = nullptr;
IDirect3DDevice9* _device = nullptr;
// The design's stock screen made by us (Textures::MakeOwn) when the level did not load it (the Imperial walkers on
// Hoth: imp_1st_cockpit_securitycam is in FPM/IMP/impsccam.lvl, which only the Imperial tanks' levels load), and the
// name it was made for (the design's own string; null texture: it could not be made).
IDirect3DTexture9* _ownScreen = nullptr;
const char* _ownScreenName = nullptr;
// The shell being drawn is a pilot seat's (Draw), not a gunner station's (DrawParts): Display::seat.
bool _isPilotSeatDraw = true;
DWORD _displaysDrawnTick = 0; // when a shell with displays was last drawn (HasDisplays)
bool _haveDisplaysBeenDrawn = false;
unsigned _shownGroups = 0;    // the groups the displays drawn last showed (bit per HudDraws::Group, ShowsGroup)
bool _isReadoutTinted = true; // SetReadoutTint
volatile LONG _isGroupLogRequested = 0;
/// The readouts' scan lines (DrawScanLines): one plain glass square (four of its lines) covers this much height, as on
/// the periscope's display (its 279-texel glass is 0.104 m high: 28 texels = 0.0104 m); room for the strips; the
/// lines' colour (black) and strength (vertex alpha).
constexpr float SCAN_TILE_METERS = 0.0104f;
constexpr int MOST_SCAN_STRIPS = 32;
constexpr D3DCOLOR SCAN_LINE_COLOUR = 0x99000000;
float _pilotFrame[16];     // GetPilotFrame
DWORD _pilotFrameTick = 0; // when it was set
bool _hasPilotFrame = false;
bool _isCutPending = false; // CutOpenings: the game's cockpit is drawn this pass, the next shell draw cuts its openings
double _costMilliseconds = 0, _costMaximumMilliseconds = 0; // the shell draws timed since the last log line
int _costDraws = 0;
constexpr DWORD PILOT_FRAME_HOLD_MILLISECONDS = 100;

/// A HUD batch's colour on a stock screen (PLAN 57 j): the screen's readout colour at the batch's brightness (its
/// brightest channel), so every group reads as one monochrome display; reds (warnings: overheating, low health) keep
/// their own colour. A tint with ShellDesigns::TINT_KEEPS_RELATIONS (idren2023 2026-10-27, the droid) tints only the
/// minimap's background (the masked batch) and keeps every other colour: friend green, enemy red, third party yellow,
/// the player's white arrow, neutral grey.
D3DCOLOR ReadoutColour(D3DCOLOR colour, uint32_t tint, bool isMasked)
{
    if ((tint & ShellDesigns::TINT_KEEPS_RELATIONS) && !isMasked)
    {
        return colour & 0xFFFFFF;
    }
    const unsigned red = (colour >> 16) & 0xFF, green = (colour >> 8) & 0xFF, blue = colour & 0xFF;
    if (red > 2 * green && red > 2 * blue)
    {
        return colour & 0xFFFFFF;
    }
    const unsigned brightest = red > green ? (red > blue ? red : blue) : (green > blue ? green : blue);
    const auto channel = [&](int shift)
    {
        return (((tint >> shift) & 0xFF) * brightest / 255) << shift;
    };
    return channel(16) | channel(8) | channel(0);
}

void ReleaseOwnScreen()
{
    if (_ownScreen)
    {
        _ownScreen->lpVtbl->Release(_ownScreen);
        _ownScreen = nullptr;
    }
    _ownScreenName = nullptr;
}

// The design's stock screen texture: the game's when the level loaded it, else ours from the game's file.
IDirect3DTexture9* ScreenTexture(IDirect3DDevice9* device, const char* name)
{
    if (!name)
    {
        return nullptr;
    }
    if (auto* const game = static_cast<IDirect3DTexture9*>(Textures::FindByName(name)))
    {
        return game;
    }
    if (_ownScreenName == name)
    {
        return _ownScreen;
    }
    ReleaseOwnScreen();
    _ownScreenName = name;
    int width = 0, height = 0;
    const unsigned* pixels = GameTextures::Pixels(name, width, height);
    _ownScreen = static_cast<IDirect3DTexture9*>(Textures::MakeOwn(device, pixels, width, height));
    Log::Info("shell", "%s not loaded by the level: %s", name, _ownScreen ? "ours from the game's file" : "none");
    return _ownScreen;
}

void ReleaseBuffer()
{
    if (_vertexBuffer)
    {
        _vertexBuffer->lpVtbl->Release(_vertexBuffer);
        _vertexBuffer = nullptr;
    }
    _builtFor = nullptr;
}

// The shell of that vehicle into the vertex buffer, grouped by finish. False when it has none or it failed.
bool Fill(IDirect3DDevice9* device, const char* stockName)
{
    ReleaseBuffer();
    if (!ShellDesigns::Build(stockName, _built, MOST_VERTICES, &_shell) || _shell.vertexCount < 3)
    {
        return false;
    }
    if (FAILED(device->lpVtbl->CreateVertexBuffer(
            device,
            static_cast<UINT>(_shell.vertexCount * sizeof(Vertex)),
            D3DUSAGE_WRITEONLY,
            VERTEX_FORMAT,
            D3DPOOL_MANAGED,
            &_vertexBuffer,
            nullptr)))
    {
        _vertexBuffer = nullptr;
        Log::Warning("shell", "vertex buffer not created: no cockpit shell");
        return false;
    }
    Vertex* out = nullptr;
    if (FAILED(_vertexBuffer->lpVtbl->Lock(_vertexBuffer, 0, 0, reinterpret_cast<void**>(&out), 0)))
    {
        ReleaseBuffer();
        return false;
    }
    int written = 0;
    _groupCount = 0;
    for (int part = 0; part < _shell.partCount && part < MOST_PARTS; ++part)
    {
        for (int finish = 0; finish < _shell.finishCount && _groupCount < MOST_GROUPS; ++finish)
        {
            const int start = written;
            for (int index = 0; index < _shell.vertexCount; ++index)
            {
                const ShellKit::Vertex& v = _built[index];
                if (v.finish != finish || v.part != part)
                {
                    continue;
                }
                out[written++] =
                    {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z, v.u, v.v};
            }
            if (written > start)
            {
                _groups[_groupCount++] = {part, finish, start, written - start};
            }
        }
    }
    _vertexBuffer->lpVtbl->Unlock(_vertexBuffer);
    _builtFor = stockName;
    Log::Info(
        "shell",
        "cockpit shell for %s: %d triangles, %d parts, %d groups, %d HUD displays%s",
        stockName,
        _shell.vertexCount / 3,
        _shell.partCount,
        _groupCount,
        _shell.displayCount,
        _shell.isFull ? " (cut: too many)" : "");
    return true;
}

D3DCOLORVALUE ColorValue(uint32_t color, float scale)
{
    return {
        ((color >> 16) & 0xFF) / 255.0f * scale,
        ((color >> 8) & 0xFF) / 255.0f * scale,
        (color & 0xFF) / 255.0f * scale,
        1.0f};
}

// A finish's Direct3D material: halved under the 2x texture modulation; glow as emissive.
D3DMATERIAL9 MaterialOf(const ShellKit::Finish& finish, bool isTextured)
{
    const float half = isTextured ? 0.5f : 1.0f;
    D3DMATERIAL9 material = {};
    material.Diffuse = material.Ambient = ColorValue(finish.color, half * (1.0f - finish.glow));
    material.Emissive = ColorValue(finish.color, half * finish.glow);
    material.Specular = {finish.specular, finish.specular, finish.specular, 1};
    material.Power = finish.power;
    return material;
}

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

// The scan lines over a stock screen (PLAN 57 j; idren2023 2026-10-19: as the gunner screen's, fine and level, over the
// whole screen): the screen's own glass (texture 5, its plain square, whose alpha carries the stock scan lines) laid
// over the screen's glass (ShellDesigns::ScanArea) in strips SCAN_TILE_METERS high, before the readouts, one column of
// it only (the lines can only run level). The glass's alpha between the lines (glassAlphaFloor, the texture factor) is
// taken off and the rest raised (x 4 x the vertex alpha), so only the lines darken. Called inside DrawDisplays (no
// lighting, alpha blending, no depth write); its stage states are put back after.
void DrawScanLines(IDirect3DDevice9* device, const ShellDesigns::ScanArea& display, IDirect3DTexture9* screenTexture)
{
    if (!screenTexture || display.glassFinish < 0 || display.glassFinish >= _shell.finishCount)
    {
        return;
    }
    const ShellKit::Patch& patch = _shell.finishes[display.glassFinish].patch;
    const float u = (patch.u0 + patch.u1) * 0.5f;
    const ShellKit::Vec3 normal = {0, 0, 0}; // the area already lies before the readouts
    const ShellKit::Vec3 halfRight = display.right * (display.width * 0.5f);
    const int strips = 1 + static_cast<int>(display.height / SCAN_TILE_METERS);
    static HudVertex quads[6 * MOST_SCAN_STRIPS];
    int count = 0;
    for (int strip = 0; strip < strips && strip < MOST_SCAN_STRIPS; ++strip)
    {
        const float top = display.height * 0.5f - strip * SCAN_TILE_METERS;
        const float bottom =
            top - SCAN_TILE_METERS < -display.height * 0.5f ? -display.height * 0.5f : top - SCAN_TILE_METERS;
        const float v1 = patch.v0 + (patch.v1 - patch.v0) * (top - bottom) / SCAN_TILE_METERS;
        const ShellKit::Vec3 a = display.center + display.up * top + normal,
                             b = display.center + display.up * bottom + normal;
        const ShellKit::Vec3 corners[4] = {a - halfRight, a + halfRight, b + halfRight, b - halfRight};
        const float vs[4] = {patch.v0, patch.v0, v1, v1};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int index = 0; index < 6; ++index)
        {
            const ShellKit::Vec3& at = corners[order[index]];
            quads[count++] = {at.x, at.y, at.z, SCAN_LINE_COLOUR, u, vs[order[index]], 0, 0};
        }
    }
    const DWORD floor = static_cast<DWORD>(display.glassAlphaFloor * 255.0f) << 24;
    device->lpVtbl->SetRenderState(device, D3DRS_TEXTUREFACTOR, floor);
    device->lpVtbl->SetTexture(device, 0, reinterpret_cast<IDirect3DBaseTexture9*>(screenTexture));
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SUBTRACT);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
    device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_ALPHAOP, D3DTOP_MODULATE4X);
    device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
    device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->lpVtbl->DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, static_cast<UINT>(count / 3), quads, sizeof(HudVertex));
    device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
}

// The pilot's eye of the shell drawn (cockpit space), for the displays on ShellDesigns::HEAD_PART.
float _headEye[3] = {};
bool _hasHeadEye = false;

// The world matrix of the displays on ShellDesigns::HEAD_PART: cockpit space with the pilot's eye at the head (the
// head's last world matrix; its axes are cockpit space's, -z ahead). False without a head or an eye.
bool HasHeadDisplayMatrix(float* matrix)
{
    float head[16];
    if (!_hasHeadEye || !Stereo::GetSmoothedHeadWorld(head))
    {
        return false;
    }
    memcpy(matrix, head, sizeof(head));
    for (int axis = 0; axis < 3; ++axis)
    {
        matrix[12 + axis] = head[12 + axis] - _headEye[0] * head[axis] - _headEye[1] * head[4 + axis] -
                            _headEye[2] * head[8 + axis];
    }
    return true;
}

// The HUD groups the mod has taken from the game (HudDraws::IsTaken) on the shell's displays, each scaled to fill its
// room's width or height, straight, centred; texture times colour, alpha blended, depth tested, no depth written,
// lighting off. A masked batch (the minimap) multiplies its alpha by the mask's in stage 1. After the shell's passes,
// with the cockpit's world matrix set; a display on another part with that part's matrix (left out when that part is).
void DrawDisplays(
    IDirect3DDevice9* device,
    const float* const* partMatrices,
    int partCount,
    IDirect3DTexture9* screenTexture)
{
    static HudVertex vertices[MOST_HUD_VERTICES];
    device->lpVtbl->SetRenderState(device, D3DRS_LIGHTING, FALSE);
    device->lpVtbl->SetRenderState(device, D3DRS_SPECULARENABLE, FALSE);
    device->lpVtbl->SetRenderState(device, D3DRS_ALPHABLENDENABLE, TRUE);
    device->lpVtbl->SetRenderState(device, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    device->lpVtbl->SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    device->lpVtbl->SetRenderState(device, D3DRS_ZWRITEENABLE, FALSE);
    device->lpVtbl->SetFVF(device, HUD_FORMAT);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->lpVtbl->SetSamplerState(device, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    for (DWORD sampler = 0; sampler < 2; ++sampler)
    {
        device->lpVtbl->SetSamplerState(device, sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        device->lpVtbl->SetSamplerState(device, sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    device->lpVtbl->SetSamplerState(device, 1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->lpVtbl->SetSamplerState(device, 1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->lpVtbl->SetSamplerState(device, 1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    FpGuard fp;
    int shownDisplayPart = 0;
    float headMatrix[16];
    const bool hasHead = HasHeadDisplayMatrix(headMatrix);
    unsigned shownGroups = 0;
    for (int index = 0; index < _shell.displayCount; ++index)
    {
        const ShellDesigns::Display& display = _shell.displays[index];
        if (display.group < 0 || display.group >= static_cast<int>(HudDraws::Group::Count))
        {
            continue;
        }
        int part = display.part;
        if (part == ShellDesigns::HEAD_PART)
        {
            if (!hasHead)
            {
                continue;
            }
            if (part != shownDisplayPart)
            {
                shownDisplayPart = part;
                D3DMATRIX world;
                memcpy(&world, headMatrix, sizeof(world));
                device->lpVtbl->SetTransform(device, D3DTS_WORLD, &world);
            }
        }
        else if (part >= partCount)
        {
            if (part >= _shell.restPartCount)
            {
                continue;
            }
            part = 0;
        }
        if (part != ShellDesigns::HEAD_PART)
        {
            if (!partMatrices[part])
            {
                continue;
            }
            if (part != shownDisplayPart)
            {
                shownDisplayPart = part;
                D3DMATRIX world;
                memcpy(&world, partMatrices[part], sizeof(world));
                device->lpVtbl->SetTransform(device, D3DTS_WORLD, &world);
            }
        }
        if (display.seat != ShellDesigns::DISPLAY_EVERY_SEAT &&
            (display.seat == ShellDesigns::DISPLAY_PILOT_SEAT) != _isPilotSeatDraw)
        {
            continue; // the other seat's
        }
        const auto group = static_cast<HudDraws::Group>(display.group);
        shownGroups |= 1u << display.group;
        if (display.width <= 0 || display.height <= 0)
        {
            continue; // left out of this seat
        }
        HudDraws::GroupDrawing drawing{};
        if (!HudDraws::IsTaken(group) || !HudDraws::GetGroup(group, drawing))
        {
            continue;
        }
        const float groupWidth = drawing.maxX - drawing.minX, groupHeight = drawing.maxY - drawing.minY;
        if (groupWidth <= 0 || groupHeight <= 0)
        {
            continue;
        }
        // A point's place as drawn: fractions of the group's bounds, 0 at the left / bottom as it appears.
        const auto across = [&](float x)
        {
            return drawing.signX > 0 ? (x - drawing.minX) / groupWidth : (drawing.maxX - x) / groupWidth;
        };
        const auto upward = [&](float y)
        {
            return drawing.signY > 0 ? (y - drawing.minY) / groupHeight : (drawing.maxY - y) / groupHeight;
        };
        const bool isWhole =
            display.regionLeft <= 0 && display.regionBottom <= 0 && display.regionRight >= 1 && display.regionTop >= 1;
        // A batch is shown when the middle of its bounds lies in the region (whole batches: the group is slanted, so
        // its bars' triangles overlap in height; run324).
        const auto isShown = [&](const HudDraws::GroupBatch& batch)
        {
            if (isWhole)
            {
                return true;
            }
            float left = 1e30f, right = -1e30f, bottom = 1e30f, top = -1e30f;
            for (int vertex = 0; vertex < batch.vertexCount; ++vertex)
            {
                const HudDraws::GroupVertex& at = drawing.vertices[batch.firstVertex + vertex];
                const float a = across(at.x), b = upward(at.y);
                left = a < left ? a : left;
                right = a > right ? a : right;
                bottom = b < bottom ? b : bottom;
                top = b > top ? b : top;
            }
            const float a = (left + right) * 0.5f, b = (bottom + top) * 0.5f;
            return a >= display.regionLeft && a <= display.regionRight && b >= display.regionBottom &&
                   b <= display.regionTop;
        };
        if (_isGroupLogRequested)
        {
            for (int batchIndex = 0; batchIndex < drawing.batchCount; ++batchIndex)
            {
                const HudDraws::GroupBatch& batch = drawing.batches[batchIndex];
                float left = 1, right = 0, bottom = 1, top = 0;
                for (int vertex = 0; vertex < batch.vertexCount; ++vertex)
                {
                    const HudDraws::GroupVertex& at = drawing.vertices[batch.firstVertex + vertex];
                    const float a = across(at.x), b = upward(at.y);
                    left = a < left ? a : left;
                    right = a > right ? a : right;
                    bottom = b < bottom ? b : bottom;
                    top = b > top ? b : top;
                }
                Log::Info(
                    "shell",
                    "group %d (%.0f x %.0f) batch %d: colour %08X, %d vertices, across %.2f..%.2f, up %.2f..%.2f",
                    display.group,
                    static_cast<double>(groupWidth),
                    static_cast<double>(groupHeight),
                    batchIndex,
                    batch.colour,
                    batch.vertexCount,
                    static_cast<double>(left),
                    static_cast<double>(right),
                    static_cast<double>(bottom),
                    static_cast<double>(top));
            }
        }
        // The shown part's bounds (the whole group's unless a region is given), scaled to fill the room.
        float minX = drawing.minX, maxX = drawing.maxX, minY = drawing.minY, maxY = drawing.maxY;
        if (!isWhole)
        {
            minX = minY = 1e30f;
            maxX = maxY = -1e30f;
            for (int batchIndex = 0; batchIndex < drawing.batchCount; ++batchIndex)
            {
                const HudDraws::GroupBatch& batch = drawing.batches[batchIndex];
                if (!isShown(batch))
                {
                    continue;
                }
                for (int vertex = 0; vertex < batch.vertexCount; ++vertex)
                {
                    const HudDraws::GroupVertex& at = drawing.vertices[batch.firstVertex + vertex];
                    minX = at.x < minX ? at.x : minX;
                    maxX = at.x > maxX ? at.x : maxX;
                    minY = at.y < minY ? at.y : minY;
                    maxY = at.y > maxY ? at.y : maxY;
                }
            }
            if (maxX <= minX || maxY <= minY)
            {
                continue;
            }
        }
        const float byWidth = display.width / (maxX - minX), byHeight = display.height / (maxY - minY);
        const float scale = byWidth < byHeight ? byWidth : byHeight;
        const float middleX = (minX + maxX) * 0.5f, middleY = (minY + maxY) * 0.5f;
        for (int batchIndex = 0; batchIndex < drawing.batchCount; ++batchIndex)
        {
            const HudDraws::GroupBatch& batch = drawing.batches[batchIndex];
            if (batch.vertexCount < 3 || batch.vertexCount > MOST_HUD_VERTICES)
            {
                continue;
            }
            float alpha = static_cast<float>(batch.colour >> 24) / 255.0f * DISPLAY_ALPHA;
            alpha = alpha > 1 ? 1 : alpha;
            const D3DCOLOR rgb =
                display.tint && _isReadoutTinted ? ReadoutColour(batch.colour, display.tint, batch.d3dMask != nullptr) : batch.colour & 0xFFFFFF;
            const D3DCOLOR colour = (static_cast<D3DCOLOR>(alpha * 255) << 24) | rgb;
            if (!isShown(batch))
            {
                continue;
            }
            const int vertexCount = batch.vertexCount;
            for (int vertex = 0; vertex < vertexCount; ++vertex)
            {
                const HudDraws::GroupVertex& from = drawing.vertices[batch.firstVertex + vertex];
                const float x = (from.x - middleX) * scale * drawing.signX;
                const float y = (from.y - middleY) * scale * drawing.signY;
                const ShellKit::Vec3 at = display.center + display.right * x + display.up * y;
                vertices[vertex] = {at.x, at.y, at.z, colour, from.u, from.v, from.maskU, from.maskV};
            }
            device->lpVtbl->SetTexture(device, 0, static_cast<IDirect3DBaseTexture9*>(batch.d3dTexture));
            if (batch.d3dMask)
            {
                device->lpVtbl->SetTexture(device, 1, static_cast<IDirect3DBaseTexture9*>(batch.d3dMask));
                device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_TEXCOORDINDEX, 1);
                device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
                device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_COLORARG2, D3DTA_CURRENT);
                device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
            }
            device->lpVtbl->DrawPrimitiveUP(
                device,
                D3DPT_TRIANGLELIST,
                static_cast<UINT>(vertexCount / 3),
                vertices,
                sizeof(HudVertex));
            if (batch.d3dMask)
            {
                device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                device->lpVtbl->SetTexture(device, 1, nullptr);
            }
        }
    }
    // The stock screens' scan lines over everything on their glass (the HUD included).
    if (_isReadoutTinted)
    {
        for (int index = 0; index < _shell.scanAreaCount; ++index)
        {
            const ShellDesigns::ScanArea& area = _shell.scanAreas[index];
            int part = area.part;
            if (part >= partCount)
            {
                if (part >= _shell.restPartCount)
                {
                    continue;
                }
                part = 0;
            }
            if (!partMatrices[part])
            {
                continue;
            }
            if (part != shownDisplayPart)
            {
                shownDisplayPart = part;
                D3DMATRIX world;
                memcpy(&world, partMatrices[part], sizeof(world));
                device->lpVtbl->SetTransform(device, D3DTS_WORLD, &world);
            }
            DrawScanLines(device, area, screenTexture);
        }
    }
    _shownGroups = shownGroups;
    InterlockedExchange(&_isGroupLogRequested, 0);
}

// The shell of that design, each part with its own world matrix (part 0's also carries the lights).
// The shell of this stock vehicle built and in the vertex buffer (again after a device or vehicle change). False when
// there is none.
bool EnsureBuilt(IDirect3DDevice9* d3dDevice, const char* stockName)
{
    if (!stockName || !ShellDesigns::HasShell(stockName))
    {
        return false;
    }
    if (_device != d3dDevice)
    {
        OnReset();
        ReleaseBuffer();
        ReleaseOwnScreen();
        _device = d3dDevice;
    }
    if ((_builtFor != stockName || !_vertexBuffer) && !Fill(d3dDevice, stockName))
    {
        _builtFor = stockName; // do not try again every frame
        return false;
    }
    return _vertexBuffer != nullptr;
}

// The shell's depth mask over the game's cockpit (ShellDesigns::Shell::maskVertices): drawn after the shell at its
// real place a few millimetres before the game's surfaces it hides, depth only. The game queues its cockpit model's draw
// and renders it later in the pass (run344-346: a depth cut before the draw, then a scene copy pasted after it,
// both came too early and changed nothing), so the cockpit's surface behind the polygon then fails the depth test
// and the scene stays visible there; the shell's glass and frames are already drawn, and the hands and levers drawn
// later lie in front. Leaves the device's state to the caller.
void DrawCutouts(IDirect3DDevice9* device, const float* cockpitMatrix)
{
    D3DMATRIX world;
    memcpy(&world, cockpitMatrix, sizeof(world));
    device->lpVtbl->SetTransform(device, D3DTS_WORLD, &world);
    device->lpVtbl->SetVertexShader(device, nullptr);
    device->lpVtbl->SetPixelShader(device, nullptr);
    device->lpVtbl->SetFVF(device, D3DFVF_XYZ);
    device->lpVtbl->SetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE);
    device->lpVtbl->SetRenderState(device, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->lpVtbl->SetRenderState(device, D3DRS_ZWRITEENABLE, TRUE);
    device->lpVtbl->SetRenderState(device, D3DRS_COLORWRITEENABLE, 0);
    device->lpVtbl->SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
    device->lpVtbl->SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
    device->lpVtbl->SetRenderState(device, D3DRS_STENCILENABLE, FALSE);
    device->lpVtbl->SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    device->lpVtbl->SetRenderState(device, D3DRS_LIGHTING, FALSE);
    device->lpVtbl->DrawPrimitiveUP(
        device, D3DPT_TRIANGLELIST, _shell.maskVertexCount / 3, _shell.maskVertices, sizeof(ShellKit::Vec3));
    device->lpVtbl->SetRenderState(device, D3DRS_COLORWRITEENABLE, 0xF);
}

void DrawShell(void* device, const float* const* partMatrices, int partCount, const char* stockName)
{
    if (!device || !stockName || partCount < 1)
    {
        return;
    }
    const float* const cockpitMatrix = partMatrices[0];
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    const bool isDisplayOnTop = ShellDesigns::IsDisplayOnTop(stockName);
    if (!EnsureBuilt(d3dDevice, stockName))
    {
        return;
    }
    _hasHeadEye = ShellDesigns::PilotEye(stockName, _headEye);
    {
        // The cockpit against the game camera (headset run 19: the AT-ST's cockpit bounced round the player): the
        // range of the cockpit's origin in the camera's frame over each second, logged while it moves.
        FpGuard fp;
        float camera[16];
        static float low[3] = {1e9f, 1e9f, 1e9f}, high[3] = {-1e9f, -1e9f, -1e9f};
        static DWORD logTick = 0;
        if (Stereo::GetGameCamera(camera))
        {
            const float offset[3] = {cockpitMatrix[12] - camera[12], cockpitMatrix[13] - camera[13],
                                     cockpitMatrix[14] - camera[14]};
            for (int row = 0; row < 3; ++row)
            {
                const float along = offset[0] * camera[row * 4] + offset[1] * camera[row * 4 + 1] +
                                    offset[2] * camera[row * 4 + 2];
                low[row] = along < low[row] ? along : low[row];
                high[row] = along > high[row] ? along : high[row];
            }
            const DWORD now = GetTickCount();
            if (now - logTick >= 1000)
            {
                const float most = fmaxf(high[0] - low[0], fmaxf(high[1] - low[1], high[2] - low[2]));
                if (logTick && most > 0.01f)
                {
                    Log::Info("shell", "%s: cockpit against the camera moved %.3f %.3f %.3f m (x y z, last second)",
                              stockName, static_cast<double>(high[0] - low[0]), static_cast<double>(high[1] - low[1]),
                              static_cast<double>(high[2] - low[2]));
                }
                logTick = now;
                for (int row = 0; row < 3; ++row)
                {
                    low[row] = 1e9f;
                    high[row] = -1e9f;
                }
            }
        }
    }
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    IDirect3DTexture9* const textures[7] = {
        nullptr,
        static_cast<IDirect3DTexture9*>(Textures::FindByName(_shell.textureName)),
        static_cast<IDirect3DTexture9*>(Textures::FindByName(_shell.effectsName)),
        static_cast<IDirect3DTexture9*>(Textures::FindByName(_shell.exteriorName)),
        static_cast<IDirect3DTexture9*>(Textures::FindByName(_shell.glassName)),
        ScreenTexture(d3dDevice, _shell.screenName),
        static_cast<IDirect3DTexture9*>(Hud::ScopePicture())};
    if (!_savedState && FAILED(d3dDevice->lpVtbl->CreateStateBlock(d3dDevice, D3DSBT_ALL, &_savedState)))
    {
        _savedState = nullptr;
        return;
    }
    if (FAILED(_savedState->lpVtbl->Capture(_savedState)))
    {
        return;
    }
    D3DMATRIX world;
    memcpy(&world, cockpitMatrix, sizeof(world));
    d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_WORLD, &world);
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetFVF(d3dDevice, VERTEX_FORMAT);
    d3dDevice->lpVtbl->SetStreamSource(d3dDevice, 0, _vertexBuffer, 0, sizeof(Vertex));
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
    // The map's own lighting as the game lights its cockpit model (PLAN 57 l, idren2023 2026-10-19: each map defines its
    // own; runs 356-357): colour = texture x scale x (ambient + sun x N.L), read from the cockpit's own draw (the
    // previous frame's: the game draws it later in the pass). A finish of 0x808080 then shows exactly as the game's
    // cockpit does (material 0.25, MODULATE2X: hence GAME_LIGHT_GAIN). Without it, the fixed lights below.
    D3dSurvey::WatchLightOf(textures[1] ? textures[1] : textures[3]);
    float gameScale[3], gameAmbient[3], gameSun[3], gameSunDirection[3];
    const bool hasGameLight = D3dSurvey::GetWatchedLight(gameScale, gameAmbient, gameSun, gameSunDirection);
    constexpr float GAME_LIGHT_GAIN = 255.0f / 128.0f;
    DWORD ambientColour = 0;
    if (hasGameLight)
    {
        FpGuard fp;
        for (int channel = 0; channel < 3; ++channel)
        {
            const float level = fminf(gameAmbient[channel] * gameScale[channel] * GAME_LIGHT_GAIN, 1.0f);
            ambientColour |= static_cast<DWORD>(fmaxf(level, 0.0f) * 255.0f) << (16 - 8 * channel);
        }
    }
    else
    {
        const DWORD ambientLevel = static_cast<DWORD>(AMBIENT * 255.0f);
        ambientColour = (ambientLevel << 16) | (ambientLevel << 8) | ambientLevel;
    }
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LIGHTING, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_AMBIENT, 0xFF000000 | ambientColour);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SPECULARENABLE, hasGameLight ? FALSE : TRUE);
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
    if (hasGameLight)
    {
        FpGuard fp;
        D3DLIGHT9 sun = {};
        sun.Type = D3DLIGHT_DIRECTIONAL;
        sun.Diffuse = {
            gameSun[0] * gameScale[0] * GAME_LIGHT_GAIN,
            gameSun[1] * gameScale[1] * GAME_LIGHT_GAIN,
            gameSun[2] * gameScale[2] * GAME_LIGHT_GAIN,
            1};
        sun.Specular = {0, 0, 0, 1}; // the game's lit model shaders have no highlights (run359: bright patches)
        sun.Direction = {gameSunDirection[0], gameSunDirection[1], gameSunDirection[2]};
        d3dDevice->lpVtbl->SetLight(d3dDevice, 0, &sun);
        d3dDevice->lpVtbl->LightEnable(d3dDevice, 0, TRUE);
        for (int light = 1; light < 8; ++light)
        {
            d3dDevice->lpVtbl->LightEnable(d3dDevice, light, FALSE);
        }
    }
    else
    {
        FpGuard fp;
        const float* lights[2] = {KEY_LIGHT, FILL_LIGHT};
        const float strengths[2] = {KEY, FILL};
        for (int light = 0; light < 2; ++light)
        {
            // Toward the light, carried into the world by the cockpit matrix (rows 0-2).
            float toward[3];
            for (int axis = 0; axis < 3; ++axis)
            {
                toward[axis] = lights[light][0] * cockpitMatrix[axis] + lights[light][1] * cockpitMatrix[4 + axis] +
                               lights[light][2] * cockpitMatrix[8 + axis];
            }
            Normalize(toward);
            D3DLIGHT9 parameters = {};
            parameters.Type = D3DLIGHT_DIRECTIONAL;
            parameters.Diffuse = {strengths[light], strengths[light] * 0.97f, strengths[light] * 0.92f, 1};
            parameters.Specular = light == 0 ? D3DCOLORVALUE{0.9f, 0.88f, 0.84f, 1} : D3DCOLORVALUE{0, 0, 0, 1};
            parameters.Direction = {-toward[0], -toward[1], -toward[2]};
            d3dDevice->lpVtbl->SetLight(d3dDevice, light, &parameters);
            d3dDevice->lpVtbl->LightEnable(d3dDevice, light, TRUE);
        }
        for (int light = 2; light < 8; ++light)
        {
            d3dDevice->lpVtbl->LightEnable(d3dDevice, light, FALSE);
        }
    }
    // Opaque finishes first; then glass, blended over them without writing depth.
    int shownPart = 0; // the part whose matrix is set as the world transform
    // (passes 2 and 3: a station display drawn over everything, ShellDesigns::IsDisplayOnTop, opaque then glass)
    for (int pass = 0; pass < 4; ++pass)
    {
        const bool isGlassPass = pass == 1 || pass == 3, isTopPass = pass >= 2;
        if (isTopPass && !isDisplayOnTop)
        {
            break;
        }
        if (isGlassPass)
        {
            d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
            d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, FALSE);
        }
        else if (isTopPass)
        {
            d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, FALSE);
            d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, TRUE);
        }
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZFUNC, isTopPass ? D3DCMP_ALWAYS : D3DCMP_LESSEQUAL);
        for (int group = 0; group < _groupCount; ++group)
        {
            const ShellKit::Finish& finish = _shell.finishes[_groups[group].finish];
            if ((finish.opacity < 1.0f) != isGlassPass)
            {
                continue;
            }
            IDirect3DTexture9* const texture =
                finish.texture >= 1 && finish.texture <= 6 ? textures[finish.texture] : nullptr;
            const bool isTextured = texture != nullptr;
            d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(texture));
            d3dDevice->lpVtbl->SetTextureStageState(
                d3dDevice,
                0,
                D3DTSS_COLOROP,
                isTextured ? D3DTOP_MODULATE2X : D3DTOP_SELECTARG2);
            if (isGlassPass)
            {
                // Glass: the texture's own alpha (a window texture's grime) times the finish's opacity, plus a floor
                // (idren2023 2026-10-27: the windows looked like open air; the stock window textures are only 10-20 %
                // opaque): every pane keeps a faint body of its own.
                d3dDevice->lpVtbl->SetTextureStageState(
                    d3dDevice,
                    0,
                    D3DTSS_ALPHAOP,
                    isTextured ? D3DTOP_MULTIPLYADD : D3DTOP_SELECTARG2);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG0, D3DTA_TFACTOR);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
                // (not on self-lit art behind glass, the turrets' viewfinders: those keep their own alpha)
                d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_TEXTUREFACTOR, (finish.glow > 0 ? 0u : GLASS_ALPHA_FLOOR) << 24 | 0xFFFFFF);
            }
            D3DMATERIAL9 material = MaterialOf(finish, isTextured);
            material.Diffuse.a = finish.opacity;
            d3dDevice->lpVtbl->SetMaterial(d3dDevice, &material);
            // A part beyond the matrices given is drawn at part 0's place if the shell draws it at rest (a cockpit's
            // gunner seat in the pilot seat), else left out (its stop lamps); a null matrix leaves its part out.
            int part = _groups[group].part;
            if (part >= partCount)
            {
                if (part >= _shell.restPartCount)
                {
                    continue;
                }
                part = 0;
            }
            if (!partMatrices[part] || ((isDisplayOnTop && partCount > 2 && part >= 2) != isTopPass))
            {
                continue;
            }
            if (part != shownPart)
            {
                shownPart = part;
                memcpy(&world, partMatrices[part], sizeof(world));
                d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_WORLD, &world);
            }
            d3dDevice->lpVtbl
                ->DrawPrimitive(d3dDevice, D3DPT_TRIANGLELIST, _groups[group].start, _groups[group].count / 3);
        }
    }
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    if (_shell.displayCount > 0)
    {
        if (shownPart != 0)
        {
            memcpy(&world, cockpitMatrix, sizeof(world));
            d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_WORLD, &world);
        }
        _displaysDrawnTick = GetTickCount();
        _haveDisplaysBeenDrawn = true;
        DrawDisplays(d3dDevice, partMatrices, partCount, textures[5]);
    }
    if (_isCutPending && _shell.maskVertexCount >= 3)
    {
        DrawCutouts(d3dDevice, cockpitMatrix); // the game's cockpit is drawn this pass (CutOpenings)
    }
    _isCutPending = false;
    _savedState->lpVtbl->Apply(_savedState);
    // The draw's cost on the main thread (the calls' and the HUD's; not the GPU's time), logged as the levers' is
    // (idren2023 2026-10-19: what is our triangle budget).
    LARGE_INTEGER end, frequency;
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);
    const double milliseconds =
        static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    _costMilliseconds += milliseconds;
    _costMaximumMilliseconds = milliseconds > _costMaximumMilliseconds ? milliseconds : _costMaximumMilliseconds;
    if (++_costDraws >= COST_LOG_DRAWS)
    {
        Log::Info(
            "shell",
            "shell draw cost (%s): %.3f ms per eye pass on average, %.3f at most (%d triangles, %d groups, %d passes)",
            stockName,
            _costMilliseconds / _costDraws,
            _costMaximumMilliseconds,
            _shell.vertexCount / 3,
            _groupCount,
            _costDraws);
        _costMilliseconds = _costMaximumMilliseconds = 0;
        _costDraws = 0;
    }
}

} // namespace

void Draw(void* device, const float* cockpitMatrix, const char* stockName)
{
    float eye[3];
    if (cockpitMatrix && ShellDesigns::PilotEye(stockName, eye))
    {
        // The pilot's eye carried through the cockpit's matrix becomes the frame's origin (GetPilotFrame).
        memcpy(_pilotFrame, cockpitMatrix, sizeof(_pilotFrame));
        for (int axis = 0; axis < 3; ++axis)
        {
            _pilotFrame[12 + axis] = eye[0] * cockpitMatrix[axis] + eye[1] * cockpitMatrix[4 + axis] +
                                     eye[2] * cockpitMatrix[8 + axis] + cockpitMatrix[12 + axis];
        }
        _pilotFrameTick = GetTickCount();
        _hasPilotFrame = true;
    }
    const float* const matrices[1] = {cockpitMatrix};
    _isPilotSeatDraw = true;
    DrawShell(device, matrices, 1, stockName);
}

void CutOpenings(void* device, const float* cockpitMatrix, const char* stockName)
{
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    _isCutPending = d3dDevice && cockpitMatrix && EnsureBuilt(d3dDevice, stockName) && _shell.maskVertexCount >= 3;
}

void DrawParts(void* device, const float* const* partMatrices, int partCount, const char* designName)
{
    _isPilotSeatDraw = false;
    DrawShell(device, partMatrices, partCount, designName);
}

bool HasDisplays()
{
    // The tick exactly as stored: marking it (| 1) made the difference wrap to 0xFFFFFFFF within the same tick
    // (run246).
    return _haveDisplaysBeenDrawn && GetTickCount() - _displaysDrawnTick < DISPLAYS_HOLD_MILLISECONDS;
}

bool ShowsGroup(int group)
{
    return HasDisplays() && group >= 0 && group < 32 && (_shownGroups & (1u << group)) != 0;
}

void SetReadoutTint(bool isOn)
{
    _isReadoutTinted = isOn;
    Log::Info("shell", "HUD on the stock screens in %s", isOn ? "the screens' readout colour" : "the game's colours");
}

bool GetPilotFrame(float* frame)
{
    if (!_hasPilotFrame || GetTickCount() - _pilotFrameTick >= PILOT_FRAME_HOLD_MILLISECONDS)
    {
        return false;
    }
    memcpy(frame, _pilotFrame, sizeof(_pilotFrame));
    return true;
}

void RequestGroupLog()
{
    InterlockedExchange(&_isGroupLogRequested, 1);
}

void OnReset()
{
    if (_savedState)
    {
        _savedState->lpVtbl->Release(_savedState);
        _savedState = nullptr;
    }
    _device = nullptr;
}

} // namespace CockpitShell

} // namespace SwbfVr
