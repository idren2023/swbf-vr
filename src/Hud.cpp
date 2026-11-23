// Hud.cpp: the game's HUD as a texture, drawn back into each eye as quads in the world.
//
// The game draws its HUD and in-match menus in a last pass with a fixed projection (Stereo.cpp finds it). During a
// headset eye pass that pass is sent to an off-screen texture of the back buffer's size instead of the eye image
// (OnProjection redirects it, EndRedirect ends it). Just before the eye image is taken (Composite) pieces of that
// texture are drawn into the eye image as flat quads placed in the world, with the eye's own view and projection, so
// they have real depth and stay put when the head moves.
//
//   classic    the whole HUD as one quad 3 m along the aim: what the old viewport method showed, but a true quad.
//              Always used for menus, the spawn screen, the scope picture and any large overlay (the big map).
//   compact    the HUD groups (health, ammunition, target, score, map) moved in toward the centre of that plane.
//   immersive  ammunition and the target's health on the gun (right controller); the player's health, the score and
//              the map as a hologram projected up from the left controller while the left palm is turned up; the
//              rest as compact.
// The groups are not cut from the texture: HudDraws catches the game's own draws of them and the game leaves them out
// of the texture; they are drawn again from the game's textures, straight (the game slants the health and ammunition
// boxes), wherever the layout puts them (PLAN 11). The texture keeps what the game still draws itself: messages,
// markers, vehicle displays, the reticle.
//
// Alpha: the texture is cleared to transparent black and the game blends its HUD into it. With the separate alpha
// blend set here (alpha: one, 1 - source alpha) the texture ends up holding premultiplied colour and true
// coverage, so it is drawn back with (one, 1 - source alpha).
//
// Direct3D 9 is used through its C interface (CINTERFACE): every call goes through the object's function table and
// takes the object itself as first argument (d3dDevice->lpVtbl->SetViewport(d3dDevice, ...)), the equivalent of
// d3dDevice->SetViewport(...) in C++. Everything here runs on the game's render thread.

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable. Has to be a macro: d3d9.h reads
// it.
#define CINTERFACE

#include "Hud.h"

#include "Aim.h"
#include "Xr.h"
#include "CockpitShell.h"
#include "Controls.h"
#include "EarMenu.h"
#include "Feedback.h"
#include "FpGuard.h"
#include "Gesture.h"
#include "Holster.h"
#include "HudDraws.h"
#include "LockOn.h"
#include "Log.h"
#include "Main.h"
#include "Settings.h"
#include "Sight.h"
#include "Stereo.h"
#include "Vehicle.h"
#include "Zoom.h"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace SwbfVr
{

namespace Hud
{

namespace
{

// ---- Constants ----

/// The overlay probe: the size of the small copy of the HUD texture it looks at.
constexpr int PROBE_WIDTH = 64;
constexpr int PROBE_HEIGHT = 40;

/// Comfort vignette: the ring is drawn in this many segments, each this angle wide (2 pi / 32).
constexpr int VIGNETTE_SEGMENT_COUNT = 32;
constexpr float VIGNETTE_SEGMENT_RADIANS = 0.19634954f;
/// The vignette's strength (0..1) follows the motion through a lag with these time constants (b124: it came in over
/// 0.15 s and looked as if it appeared at once, idren2023, headset run 10), and motion below VIGNETTE_MOTION_START
/// (slow walking, gentle turns) gives none.
constexpr float VIGNETTE_IN_SECONDS = 0.35f;
constexpr float VIGNETTE_OUT_SECONDS = 0.45f;
constexpr float VIGNETTE_MOTION_START = 0.15f;

/// Wrist panel: how long everything that asks for it must hold before it comes up, and its fade in or out.
constexpr DWORD WRIST_HOLD_MILLISECONDS = 250;
constexpr float WRIST_FADE_MILLISECONDS = 150.0f;
/// Wrist panel: the head must look toward the palm, cos of the angle between the view and the palm: about
/// 37 degrees to come up, 45 to stay.
constexpr float WRIST_LOOK_TO_SHOW = 0.80f;
constexpr float WRIST_LOOK_TO_KEEP = 0.70f;
/// Wrist panel: the left hand must be at least this far from the right hand (not holding the gun two-handed).
constexpr float WRIST_MINIMUM_HAND_DISTANCE_METERS = 0.22f;

/// The hologram's pool of light and beam are drawn in this many segments: round enough that no corners show.
constexpr int HOLOGRAM_SEGMENT_COUNT = 48;

/// Width of the ammunition sign on the gun.
constexpr float AMMO_SIGN_WIDTH_METERS = 0.12f;
/// Where the hexagon icons' centres sit, as a fraction of the group's width: the ammunition group's from its left end,
/// the target's bar's from its right end (run190's dump: icons u 0.038-0.063 in 0.029-0.157, 0.937-0.964 in
/// 0.866-0.971).
constexpr float AMMO_ICON_FROM_LEFT = 0.168f;
constexpr float TARGET_ICON_FROM_RIGHT = 0.195f;
/// Between the ammunition sign and the target's health under it.
constexpr float TARGET_SIGN_GAP_METERS = 0.004f;

/// Near and far clip distances of the projection the quads are drawn with (no depth test: the near distance only cuts;
/// 1 mm so the ammunition on a gun held at the face is not cut, idren2023 2026-11-22).
constexpr float QUAD_NEAR_METERS = 0.001f;
constexpr float QUAD_FAR_METERS = 500.0f;
/// The killer line after death (PLAN 35): red, in this many pieces, about 0.17 degrees wide (at least 6 mm).
constexpr D3DCOLOR KILLER_LINE_RGB = 0xFF3C32;
constexpr int KILLER_LINE_SEGMENT_COUNT = 8;
constexpr float KILLER_LINE_HALF_WIDTH_PER_METER = 0.0015f;
constexpr float KILLER_LINE_MINIMUM_HALF_WIDTH_METERS = 0.003f;

// ---- State ----

IDirect3DDevice9* _device = nullptr;
IDirect3DTexture9* _texture = nullptr;
IDirect3DSurface9* _surface = nullptr;
IDirect3DSurface9* _previousTarget = nullptr; // the game's render target while the HUD pass is redirected
IDirect3DStateBlock9* _savedState = nullptr;
UINT _width = 0, _height = 0;
bool _isRedirected = false; // the HUD pass is drawing into the texture right now
bool _hasHud = false;       // the texture holds this eye pass's HUD
bool _isCompositing = false;
bool _hasFailed = false;
int _mode = 1;                              // 0 = old viewport panel (Stereo.cpp), 1 = through the texture
int _layout = 2;                            // 0 classic, 1 compact, 2 immersive
IDirect3DTexture9* _probeTexture = nullptr; // 64x40 copy of the middle of the HUD texture, to notice large overlays
IDirect3DSurface9 *_probeSurface = nullptr, *_probeCopy = nullptr;
bool _hasOverlay = false; // something large (the big map, the player list) covers the middle of the HUD
/// An overlay alone over the playing HUD is drawn this much larger than the panel.
constexpr float OVERLAY_ONLY_SCALE = 1.2f; // 1.6 until b176 (headset run 13: the big map at 3/4 of its size)
D3DCOLOR _panelFactor = 0xFFFFFFFF; // texture factor of the quads before the wrist pieces (holographic panels, b176)
float _overlayScale = 1.0f;         // OVERLAY_ONLY_SCALE while such an overlay is drawn, else 1 (OverlayScale)
unsigned _probeCount = 0;
unsigned _litAreaCount = 0;
/// Where the hologram's light falls on a panel (texture fractions: left, top, right, bottom; v down): the largest
/// connected piece of the HUD (MeasureLitArea), so the light surrounds the big map but not the objective text, and the
/// pause menu's coloured panel but not the back button beside it (idren2023, headset run 14).
float _litArea[4] = {0.0f, 0.0f, 1.0f, 1.0f};
/// The lit piece's outline (PLAN 58 o, headset run 17: the projection's edges on the pause menu's orange / blue border,
/// not a rectangle): LIT_OUTLINE_POINTS points (u, v texture fractions) walking its edge, each the first covered cell
/// met marching from a point on the piece's bounding box toward its middle (8 per side, from the bottom left).
constexpr int LIT_OUTLINE_POINTS = 32;
float _litOutline[LIT_OUTLINE_POINTS][2];
bool _hasLitOutline = false;
/// The hologram's source at the ear (headset run 15): core and halo radius (metres). The outlines traced round each
/// menu item were removed after headset run 16 (idren2023: too much).
constexpr int HOLOGRAM_SOURCE_SEGMENTS = 12;
constexpr float HOLOGRAM_SOURCE_CORE_METERS = 0.008f, HOLOGRAM_SOURCE_HALO_METERS = 0.03f;
constexpr float TWO_PI_HUD = 6.2831853f;
/// The Map group drawn wider than this (screen fraction) is the big map, not the minimap (b206).
constexpr float BIG_MAP_MINIMUM_WIDTH = 0.3f;
DWORD _wristChangeTick = 0; // when the wrist panel last began to appear or to go
bool _isWristWanted = false, _isWristMeant = false;
DWORD _wristMeantSinceTick = 0; // since when everything that asks for the wrist hologram has held
float _spread = 0.6f;           // compact: how far from the centre the elements sit, as a fraction of their own place
float _scale = 1.0f;            // compact: size of the moved elements
bool _shouldHideCrosshairWithController = true; // no centre crosshair while a controller aims the weapon
volatile LONG _dumpCount = 0;                   // HUD textures still to write as BMP files (set by DumpNext)
unsigned _compositeCount = 0;
// The scope window (end of this file).
IDirect3DDevice9* _scopeDevice = nullptr;
IDirect3DTexture9* _scopeTexture = nullptr;
IDirect3DSurface9* _scopeSurface = nullptr;
IDirect3DStateBlock9* _scopeSavedState = nullptr;
D3DVIEWPORT9 _gameViewport;

/// A vertex of a textured quad: position in the world, texture coordinates.
struct Vertex
{
    float x, y, z, u, v;
};

/// A vertex with a colour and texture coordinates.
struct ColourTexturedVertex
{
    float x, y, z;
    D3DCOLOR colour;
    float u, v;
};

/// Room in the vertex buffer: the whole panel (or the big map alone).
constexpr int MAXIMUM_QUAD_COUNT = 2;

// Groups the game drew and the mod draws again itself from the game's textures (HudDraws): straight, wherever the
// layout puts them. Batches of all groups of one eye pass.
constexpr int GROUP_VERTEX_CAPACITY = 4096;
constexpr int GROUP_DRAW_CAPACITY = 160;
/// On the gun and on the wrist the game's faint HUD alpha (0.4) is raised: the boxes stand alone there, against the
/// gun or the world (the cut-out version was drawn twice for the same reason, headset run 8).
constexpr float GROUP_ALPHA_ON_HAND = 1.8f;

struct GroupDraw
{
    void* d3dTexture;
    void* d3dMask; // a second texture whose alpha cuts the first out (the minimap), or null
    int firstVertex, vertexCount;
};

/// A vertex of a redrawn group: colour, the texture's coordinates and the mask's.
struct GroupDrawVertex
{
    float x, y, z;
    D3DCOLOR colour;
    float u, v, maskU, maskV;
};

/// A vertex of the hologram's light: position in the world and colour (alpha in the top byte).
struct ColourVertex
{
    float x, y, z;
    D3DCOLOR colour;
};

/// The holograms' colours (wrist projector, projected scope): blue, or red for the CIS side (idren2023, 2026-10-13).
/// @param base the light's colour (RGB, no alpha).
/// @param pale its bright, pale version (emitter, lines).
void HoloColours(D3DCOLOR* base, D3DCOLOR* pale)
{
    const bool isCis = Holster::IsCis();
    *base = isCis ? 0xFF3A24 : 0x2A7CFF;
    *pale = isCis ? 0xFFB8A8 : 0xA8D8FF;
}

void ReleaseTextures()
{
    if (_previousTarget)
    {
        _previousTarget->lpVtbl->Release(_previousTarget);
        _previousTarget = nullptr;
    }
    if (_surface)
    {
        _surface->lpVtbl->Release(_surface);
        _surface = nullptr;
    }
    if (_texture)
    {
        _texture->lpVtbl->Release(_texture);
        _texture = nullptr;
    }
    if (_savedState)
    {
        _savedState->lpVtbl->Release(_savedState);
        _savedState = nullptr;
    }
    if (_probeSurface)
    {
        _probeSurface->lpVtbl->Release(_probeSurface);
        _probeSurface = nullptr;
    }
    if (_probeTexture)
    {
        _probeTexture->lpVtbl->Release(_probeTexture);
        _probeTexture = nullptr;
    }
    if (_probeCopy)
    {
        _probeCopy->lpVtbl->Release(_probeCopy);
        _probeCopy = nullptr;
    }
    _width = _height = 0;
    _isRedirected = _hasHud = false;
}

void ReleaseScope()
{
    if (_scopeSavedState)
    {
        _scopeSavedState->lpVtbl->Release(_scopeSavedState);
        _scopeSavedState = nullptr;
    }
    if (_scopeSurface)
    {
        _scopeSurface->lpVtbl->Release(_scopeSurface);
        _scopeSurface = nullptr;
    }
    if (_scopeTexture)
    {
        _scopeTexture->lpVtbl->Release(_scopeTexture);
        _scopeTexture = nullptr;
    }
    _scopeDevice = nullptr;
}

bool EnsureTexture(IDirect3DDevice9* d3dDevice, const D3DSURFACE_DESC& target)
{
    if (_device != d3dDevice)
    {
        ReleaseTextures();
        _device = d3dDevice;
        _hasFailed = false;
    }
    if (_surface && _width == target.Width && _height == target.Height)
    {
        return true;
    }
    if (_hasFailed)
    {
        return false;
    }
    ReleaseTextures();
    if (target.MultiSampleType != D3DMULTISAMPLE_NONE ||
        FAILED(d3dDevice->lpVtbl->CreateTexture(
            d3dDevice,
            target.Width,
            target.Height,
            1,
            D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT,
            &_texture,
            nullptr)) ||
        FAILED(_texture->lpVtbl->GetSurfaceLevel(_texture, 0, &_surface)))
    {
        ReleaseTextures();
        _hasFailed = true;
        Log::Info(
            "hud",
            "no HUD texture (%ux%u, multisample %d): the HUD stays a viewport panel",
            target.Width,
            target.Height,
            static_cast<int>(target.MultiSampleType));
        return false;
    }
    _width = target.Width;
    _height = target.Height;
    Log::Info("hud", "HUD texture %ux%u created", _width, _height);
    return true;
}

void EndRedirect(IDirect3DDevice9* d3dDevice)
{
    if (!_isRedirected)
    {
        return;
    }
    _isRedirected = false;
    d3dDevice->lpVtbl->SetRenderTarget(d3dDevice, 0, _previousTarget);
    if (_previousTarget)
    {
        _previousTarget->lpVtbl->Release(_previousTarget);
        _previousTarget = nullptr;
    }
    d3dDevice->lpVtbl->SetViewport(d3dDevice, &_gameViewport);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
}

// Writes the HUD texture as a 32-bit BMP (blue, green, red, alpha; top row first) into GameData.
void DumpTexture(IDirect3DDevice9* d3dDevice)
{
    static unsigned imageNumber = 0;
    IDirect3DSurface9* copy = nullptr;
    if (FAILED(d3dDevice->lpVtbl->CreateOffscreenPlainSurface(
            d3dDevice,
            _width,
            _height,
            D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM,
            &copy,
            nullptr)))
    {
        return;
    }
    D3DLOCKED_RECT lock{};
    if (SUCCEEDED(d3dDevice->lpVtbl->GetRenderTargetData(d3dDevice, _surface, copy)) &&
        SUCCEEDED(copy->lpVtbl->LockRect(copy, &lock, nullptr, D3DLOCK_READONLY)))
    {
        char fileName[64];
        snprintf(fileName, sizeof(fileName), "swbf-vr-hud-%03u.bmp", imageNumber++);
        if (FILE* file = Main::OpenModFile(fileName, "wb"))
        {
            BITMAPFILEHEADER fileHeader{};
            BITMAPINFOHEADER infoHeader{};
            fileHeader.bfType = 0x4D42;
            fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
            fileHeader.bfSize = fileHeader.bfOffBits + _width * _height * 4;
            infoHeader.biSize = sizeof(infoHeader);
            infoHeader.biWidth = static_cast<LONG>(_width);
            infoHeader.biHeight = -static_cast<LONG>(_height);
            infoHeader.biPlanes = 1;
            infoHeader.biBitCount = 32;
            fwrite(&fileHeader, sizeof(fileHeader), 1, file);
            fwrite(&infoHeader, sizeof(infoHeader), 1, file);
            for (UINT y = 0; y < _height; ++y)
            {
                fwrite(
                    static_cast<const unsigned char*>(lock.pBits) + y * static_cast<size_t>(lock.Pitch),
                    4,
                    _width,
                    file);
            }
            fclose(file);
            Log::Info("hud", "texture written to %s", fileName);
        }
        copy->lpVtbl->UnlockRect(copy);
    }
    copy->lpVtbl->Release(copy);
}

// Two triangles for the piece (u0,v0)-(u1,v1) of the texture, as a rectangle on the plane `distance` ahead in the
// frame `frameMatrix` (camera convention: rows right, up, back, position), centre and half size given in metres on that
// plane.
void WriteQuad(
    Vertex* out,
    const float* frameMatrix,
    float distance,
    float centerX,
    float centerY,
    float halfWidth,
    float halfHeight,
    float u0,
    float v0,
    float u1,
    float v1)
{
    const float xs[4] = {centerX - halfWidth, centerX + halfWidth, centerX - halfWidth, centerX + halfWidth},
                ys[4] = {centerY + halfHeight, centerY + halfHeight, centerY - halfHeight, centerY - halfHeight};
    const float us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
    Vertex corners[4];
    for (int corner = 0; corner < 4; ++corner)
    {
        const float x = xs[corner], y = ys[corner], z = -distance;
        corners[corner] = {
            x * frameMatrix[0] + y * frameMatrix[4] + z * frameMatrix[8] + frameMatrix[12],
            x * frameMatrix[1] + y * frameMatrix[5] + z * frameMatrix[9] + frameMatrix[13],
            x * frameMatrix[2] + y * frameMatrix[6] + z * frameMatrix[10] + frameMatrix[14],
            us[corner],
            vs[corner]};
    }
    out[0] = corners[0];
    out[1] = corners[1];
    out[2] = corners[2];
    out[3] = corners[2];
    out[4] = corners[1];
    out[5] = corners[3];
}

// The 64x40 render target and its system memory copy for the probes, made at first use.
bool CreateProbeSurfaces(IDirect3DDevice9* d3dDevice)
{
    if (!_probeSurface)
    {
        if (FAILED(d3dDevice->lpVtbl->CreateTexture(
                d3dDevice,
                PROBE_WIDTH,
                PROBE_HEIGHT,
                1,
                D3DUSAGE_RENDERTARGET,
                D3DFMT_A8R8G8B8,
                D3DPOOL_DEFAULT,
                &_probeTexture,
                nullptr)) ||
            FAILED(_probeTexture->lpVtbl->GetSurfaceLevel(_probeTexture, 0, &_probeSurface)) ||
            FAILED(d3dDevice->lpVtbl->CreateOffscreenPlainSurface(
                d3dDevice,
                PROBE_WIDTH,
                PROBE_HEIGHT,
                D3DFMT_A8R8G8B8,
                D3DPOOL_SYSTEMMEM,
                &_probeCopy,
                nullptr)))
        {
            return false;
        }
    }
    return true;
}

// Every 32nd HUD (was every 8th until b268; each read-back makes the CPU wait for the graphics card): how much of the
// middle of the HUD (the half-size rectangle around the centre) is covered? A large overlay there (the big map, the
// player list, end-of-match tables) must not be cut into elements.
void RunProbes(IDirect3DDevice9* d3dDevice)
{
    ++_probeCount;
    if (_probeCount % 32 != 0 || !CreateProbeSurfaces(d3dDevice))
    {
        return;
    }
    const RECT middle = {
        static_cast<LONG>(_width / 4),
        static_cast<LONG>(_height / 4),
        static_cast<LONG>(_width * 3 / 4),
        static_cast<LONG>(_height * 3 / 4)};
    D3DLOCKED_RECT lock{};
    if (FAILED(d3dDevice->lpVtbl->StretchRect(d3dDevice, _surface, &middle, _probeSurface, nullptr, D3DTEXF_LINEAR)) ||
        FAILED(d3dDevice->lpVtbl->GetRenderTargetData(d3dDevice, _probeSurface, _probeCopy)) ||
        FAILED(_probeCopy->lpVtbl->LockRect(_probeCopy, &lock, nullptr, D3DLOCK_READONLY)))
    {
        return;
    }
    int covered = 0;
    for (int y = 0; y < PROBE_HEIGHT; ++y)
    {
        const unsigned char* row = static_cast<const unsigned char*>(lock.pBits) + y * lock.Pitch;
        for (int x = 0; x < PROBE_WIDTH; ++x)
        {
            covered += row[x * 4 + 3] > 24;
        }
    }
    _probeCopy->lpVtbl->UnlockRect(_probeCopy);
    // On above 30 %, off below 20 % (b150; was 10 / 6): the big map covers 78 %, the statistics 100 %, but something
    // covering 14-19 % (headset runs 10 and 12, after the first spawn: inferred to be a message in the middle) turned
    // the playing HUD into one panel for a whole life ("the spawn screen stays up", PLAN 29).
    const bool hasOverlay =
        _hasOverlay ? covered > PROBE_WIDTH * PROBE_HEIGHT * 20 / 100 : covered > PROBE_WIDTH * PROBE_HEIGHT * 30 / 100;
    if (hasOverlay != _hasOverlay)
    {
        _hasOverlay = hasOverlay;
        Log::Info(
            "hud",
            "%s (%d %% of the middle covered)",
            hasOverlay ? "large overlay: whole HUD as one panel" : "overlay gone: back to the layout",
            covered * 100 / (PROBE_WIDTH * PROBE_HEIGHT));
    }
}

// Every 8th HUD while a panel is a hologram: the bounds of the largest 4-connected piece of covered cells in a 64x40
// copy of the whole HUD texture go to _litArea (a cell is 30 x 30 game pixels at 1920x1200, so pieces apart by more
// than that stay apart; letters of one line of text may join, a separate line or button does not).
void MeasureLitArea(IDirect3DDevice9* d3dDevice)
{
    ++_litAreaCount;
    if (_litAreaCount % 8 != 1 || !CreateProbeSurfaces(d3dDevice))
    {
        return;
    }
    D3DLOCKED_RECT lock{};
    if (FAILED(d3dDevice->lpVtbl->StretchRect(d3dDevice, _surface, nullptr, _probeSurface, nullptr, D3DTEXF_LINEAR)) ||
        FAILED(d3dDevice->lpVtbl->GetRenderTargetData(d3dDevice, _probeSurface, _probeCopy)) ||
        FAILED(_probeCopy->lpVtbl->LockRect(_probeCopy, &lock, nullptr, D3DLOCK_READONLY)))
    {
        return;
    }
    static unsigned char isCovered[PROBE_HEIGHT][PROBE_WIDTH];
    for (int y = 0; y < PROBE_HEIGHT; ++y)
    {
        const unsigned char* row = static_cast<const unsigned char*>(lock.pBits) + y * lock.Pitch;
        for (int x = 0; x < PROBE_WIDTH; ++x)
        {
            isCovered[y][x] = row[x * 4 + 3] > 24 ? 1 : 0;
        }
    }
    _probeCopy->lpVtbl->UnlockRect(_probeCopy);
    // Flood fill each piece (an explicit stack of cells), keep the bounds of the one with the most cells.
    static short stack[PROBE_WIDTH * PROBE_HEIGHT];
    static short pieceOf[PROBE_HEIGHT][PROBE_WIDTH]; // each covered cell's piece number (1 on), 0 uncovered
    memset(pieceOf, 0, sizeof(pieceOf));
    int bestCount = 0, bestBounds[4] = {0, 0, PROBE_WIDTH - 1, PROBE_HEIGHT - 1};
    short piece = 0, bestPiece = 0;
    for (int startY = 0; startY < PROBE_HEIGHT; ++startY)
    {
        for (int startX = 0; startX < PROBE_WIDTH; ++startX)
        {
            if (isCovered[startY][startX] != 1)
            {
                continue;
            }
            int count = 0, top = 0, bounds[4] = {startX, startY, startX, startY};
            ++piece;
            isCovered[startY][startX] = 2;
            stack[top++] = static_cast<short>(startY * PROBE_WIDTH + startX);
            while (top > 0)
            {
                const int cell = stack[--top], x = cell % PROBE_WIDTH, y = cell / PROBE_WIDTH;
                ++count;
                pieceOf[y][x] = piece;
                bounds[0] = x < bounds[0] ? x : bounds[0];
                bounds[1] = y < bounds[1] ? y : bounds[1];
                bounds[2] = x > bounds[2] ? x : bounds[2];
                bounds[3] = y > bounds[3] ? y : bounds[3];
                const int neighbours[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
                for (const auto& neighbour : neighbours)
                {
                    const int nx = neighbour[0], ny = neighbour[1];
                    if (nx >= 0 && nx < PROBE_WIDTH && ny >= 0 && ny < PROBE_HEIGHT && isCovered[ny][nx] == 1)
                    {
                        isCovered[ny][nx] = 2;
                        stack[top++] = static_cast<short>(ny * PROBE_WIDTH + nx);
                    }
                }
            }
            if (count > bestCount)
            {
                bestCount = count;
                bestPiece = piece;
                memcpy(bestBounds, bounds, sizeof(bounds));
            }
        }
    }
    if (bestCount == 0)
    {
        return;
    }
    const float area[4] = {
        static_cast<float>(bestBounds[0]) / PROBE_WIDTH,
        static_cast<float>(bestBounds[1]) / PROBE_HEIGHT,
        static_cast<float>(bestBounds[2] + 1) / PROBE_WIDTH,
        static_cast<float>(bestBounds[3] + 1) / PROBE_HEIGHT};
    if (fabsf(area[0] - _litArea[0]) + fabsf(area[1] - _litArea[1]) + fabsf(area[2] - _litArea[2]) +
            fabsf(area[3] - _litArea[3]) >
        0.05f)
    {
        Log::Info(
            "hud",
            "hologram light area %.2f %.2f to %.2f %.2f (%d cells)",
            static_cast<double>(area[0]),
            static_cast<double>(area[1]),
            static_cast<double>(area[2]),
            static_cast<double>(area[3]),
            bestCount);
    }
    memcpy(_litArea, area, sizeof(_litArea));
    // The outline: from each point on the bounding box (cell centres) toward the middle, the first cell of the piece.
    const float left = static_cast<float>(bestBounds[0]), top = static_cast<float>(bestBounds[1]);
    const float right = static_cast<float>(bestBounds[2]), bottom = static_cast<float>(bestBounds[3]);
    const float middleX = (left + right) * 0.5f, middleY = (top + bottom) * 0.5f;
    constexpr int PER_SIDE = LIT_OUTLINE_POINTS / 4, MARCH_STEPS = 64;
    for (int point = 0; point < LIT_OUTLINE_POINTS; ++point)
    {
        const float along = static_cast<float>(point % PER_SIDE) / PER_SIDE;
        float x = 0, y = 0;
        switch (point / PER_SIDE)
        {
        case 0: // bottom, left to right (v grows downward)
            x = left + (right - left) * along;
            y = bottom;
            break;
        case 1: // right, bottom to top
            x = right;
            y = bottom - (bottom - top) * along;
            break;
        case 2: // top, right to left
            x = right - (right - left) * along;
            y = top;
            break;
        default: // left, top to bottom
            x = left;
            y = top + (bottom - top) * along;
            break;
        }
        float foundX = middleX, foundY = middleY;
        for (int step = 0; step <= MARCH_STEPS; ++step)
        {
            const float t = static_cast<float>(step) / MARCH_STEPS;
            const float cellX = x + (middleX - x) * t, cellY = y + (middleY - y) * t;
            const int column = static_cast<int>(cellX + 0.5f), row = static_cast<int>(cellY + 0.5f);
            if (column >= 0 && column < PROBE_WIDTH && row >= 0 && row < PROBE_HEIGHT && pieceOf[row][column] == bestPiece)
            {
                foundX = cellX;
                foundY = cellY;
                break;
            }
        }
        // Cell centres to texture fractions, pushed out half a cell (to the cell's outer edge) from the middle.
        const float outX = foundX < middleX ? -0.5f : foundX > middleX ? 0.5f : 0.0f;
        const float outY = foundY < middleY ? -0.5f : foundY > middleY ? 0.5f : 0.0f;
        _litOutline[point][0] = (foundX + 0.5f + outX) / PROBE_WIDTH;
        _litOutline[point][1] = (foundY + 0.5f + outY) / PROBE_HEIGHT;
    }
    _hasLitOutline = true;
}

} // namespace

// ---- comfort vignette: the edge of the view darkens while the sticks turn or move the player ----
namespace
{
int _vignetteLevel = 0;      // 0 off, 1 light, 2 strong
float _vignetteStrength = 0; // current strength 0..1
DWORD _vignetteTick = 0;
bool _isZoomVignetteOn = false; // whole view magnified: fixed vignette (SetZoomVignette)
IDirect3DStateBlock9* _vignetteSavedState = nullptr;

struct ScreenVertex
{
    float x, y, z, rhw;
    D3DCOLOR colour;
};
} // namespace

void SetVignette(int level)
{
    _vignetteLevel = level < 0 || level > 2 ? 0 : level;
    Log::Info("hud", "comfort vignette %s", _vignetteLevel == 0 ? "off" : _vignetteLevel == 1 ? "light" : "strong");
}

void SetZoomVignette(bool isOn)
{
    _isZoomVignetteOn = isOn;
}

void DrawVignette(void* device)
{
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    const DWORD now = GetTickCount();
    static ScreenVertex ring[2 * 6 * VIGNETTE_SEGMENT_COUNT];
    D3DVIEWPORT9 viewport{};
    {
        FpGuard guard;
        const float stepSeconds = _vignetteTick ? static_cast<float>(now - _vignetteTick) * 0.001f : 0.0f;
        _vignetteTick = now;
        float targetStrength =
            _vignetteLevel ? (Controls::CurrentMotion() - VIGNETTE_MOTION_START) / (1.0f - VIGNETTE_MOTION_START)
                           : 0.0f;
        targetStrength = targetStrength < 0 ? 0 : targetStrength > 1 ? 1 : targetStrength;
        const float timeConstant = targetStrength > _vignetteStrength ? VIGNETTE_IN_SECONDS : VIGNETTE_OUT_SECONDS;
        _vignetteStrength += (targetStrength - _vignetteStrength) *
                             (stepSeconds > 0.25f ? 1.0f : 1.0f - expf(-stepSeconds / timeConstant));
        if ((_vignetteStrength < 0.01f && !_isZoomVignetteOn) ||
            FAILED(d3dDevice->lpVtbl->GetViewport(d3dDevice, &viewport)) || viewport.Width < 16)
        {
            return;
        }
        // Clear inside `innerRadius` (as a fraction of the half picture), dark from `darkRadius` outward. Zoomed with
        // the whole view magnified: clear to 0.55 of the half picture (about 37 degrees off centre on a Quest 3),
        // black from 0.8; a guess until tried in the headset.
        const float depth = _vignetteLevel == 2 ? 0.62f : 0.42f;
        // Eased, so the ring starts gently.
        const float eased = _vignetteStrength * _vignetteStrength * (3.0f - 2.0f * _vignetteStrength);
        const float innerRadius = _isZoomVignetteOn ? 0.55f : 1.15f - depth * eased * 1.3f,
                    darkRadius = innerRadius + (_isZoomVignetteOn ? 0.25f : 0.28f), outerRadius = 1.6f;
        const float alpha = _isZoomVignetteOn ? 1.0f : eased * 1.6f > 1 ? 1.0f : eased * 1.6f;
        const D3DCOLOR black = static_cast<D3DCOLOR>(alpha * (_isZoomVignetteOn ? 255 : 245)) << 24;
        const float centerX = static_cast<float>(viewport.X) + static_cast<float>(viewport.Width) * 0.5f,
                    centerY = static_cast<float>(viewport.Y) + static_cast<float>(viewport.Height) * 0.5f;
        const float halfWidth = static_cast<float>(viewport.Width) * 0.5f,
                    halfHeight = static_cast<float>(viewport.Height) * 0.5f;
        const auto at = [&](float radius, float angle, D3DCOLOR colour)
        {
            return ScreenVertex{
                centerX + halfWidth * radius * cosf(angle),
                centerY + halfHeight * radius * sinf(angle),
                0.0f,
                1.0f,
                colour};
        };
        ScreenVertex* vertex = ring;
        for (int segment = 0; segment < VIGNETTE_SEGMENT_COUNT; ++segment)
        {
            const float angleStart = static_cast<float>(segment) * VIGNETTE_SEGMENT_RADIANS,
                        angleEnd = static_cast<float>(segment + 1) * VIGNETTE_SEGMENT_RADIANS;
            *vertex++ = at(innerRadius, angleStart, 0);
            *vertex++ = at(darkRadius, angleStart, black);
            *vertex++ = at(innerRadius, angleEnd, 0);
            *vertex++ = at(innerRadius, angleEnd, 0);
            *vertex++ = at(darkRadius, angleStart, black);
            *vertex++ = at(darkRadius, angleEnd, black);
            *vertex++ = at(darkRadius, angleStart, black);
            *vertex++ = at(outerRadius, angleStart, black);
            *vertex++ = at(darkRadius, angleEnd, black);
            *vertex++ = at(darkRadius, angleEnd, black);
            *vertex++ = at(outerRadius, angleStart, black);
            *vertex++ = at(outerRadius, angleEnd, black);
        }
    }
    if (_device != d3dDevice)
    {
        if (_vignetteSavedState)
        {
            _vignetteSavedState->lpVtbl->Release(_vignetteSavedState);
            _vignetteSavedState = nullptr;
        }
    }
    if (!_vignetteSavedState &&
        FAILED(d3dDevice->lpVtbl->CreateStateBlock(d3dDevice, D3DSBT_ALL, &_vignetteSavedState)))
    {
        _vignetteSavedState = nullptr;
        return;
    }
    if (FAILED(_vignetteSavedState->lpVtbl->Capture(_vignetteSavedState)))
    {
        return;
    }
    _isCompositing = true;
    const bool isInScene = SUCCEEDED(d3dDevice->lpVtbl->BeginScene(d3dDevice));
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, nullptr);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LIGHTING, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_FOGENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHATESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_STENCILENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SCISSORTESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_COLORWRITEENABLE, 0x7);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl
        ->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLELIST, 4 * VIGNETTE_SEGMENT_COUNT, ring, sizeof(ScreenVertex));
    if (isInScene)
    {
        d3dDevice->lpVtbl->EndScene(d3dDevice);
    }
    _vignetteSavedState->lpVtbl->Apply(_vignetteSavedState);
    _isCompositing = false;
}

// ---- test: the union of every HUD drawn during a time, to see where the game puts things (messages, bars) ----
namespace
{
unsigned char* _accumulation = nullptr;
DWORD _accumulateUntilTick = 0;
unsigned _accumulatedFrameCount = 0;

void AccumulateFrame(IDirect3DDevice9* d3dDevice)
{
    if (!_accumulateUntilTick)
    {
        return;
    }
    const DWORD now = GetTickCount();
    const size_t bytes = static_cast<size_t>(_width) * _height * 4;
    if (!_accumulation)
    {
        _accumulation = static_cast<unsigned char*>(calloc(bytes, 1));
        if (!_accumulation)
        {
            _accumulateUntilTick = 0;
            return;
        }
    }
    if (++_accumulatedFrameCount % 3 == 0)
    {
        IDirect3DSurface9* copy = nullptr;
        D3DLOCKED_RECT lock{};
        if (SUCCEEDED(d3dDevice->lpVtbl->CreateOffscreenPlainSurface(
                d3dDevice,
                _width,
                _height,
                D3DFMT_A8R8G8B8,
                D3DPOOL_SYSTEMMEM,
                &copy,
                nullptr)))
        {
            if (SUCCEEDED(d3dDevice->lpVtbl->GetRenderTargetData(d3dDevice, _surface, copy)) &&
                SUCCEEDED(copy->lpVtbl->LockRect(copy, &lock, nullptr, D3DLOCK_READONLY)))
            {
                for (UINT y = 0; y < _height; ++y)
                {
                    const unsigned char* source =
                        static_cast<const unsigned char*>(lock.pBits) + y * static_cast<size_t>(lock.Pitch);
                    unsigned char* destination = _accumulation + static_cast<size_t>(y) * _width * 4;
                    for (UINT x = 0; x < _width; ++x)
                    {
                        if (source[x * 4 + 3] > destination[x * 4 + 3])
                        {
                            memcpy(destination + x * 4, source + x * 4, 4);
                        }
                    }
                }
                copy->lpVtbl->UnlockRect(copy);
            }
            copy->lpVtbl->Release(copy);
        }
    }
    if (static_cast<LONG>(now - _accumulateUntilTick) >= 0)
    {
        _accumulateUntilTick = 0;
        if (FILE* file = Main::OpenModFile("swbf-vr-hud-accum.bmp", "wb"))
        {
            BITMAPFILEHEADER fileHeader{};
            BITMAPINFOHEADER infoHeader{};
            fileHeader.bfType = 0x4D42;
            fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
            fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(bytes);
            infoHeader.biSize = sizeof(infoHeader);
            infoHeader.biWidth = static_cast<LONG>(_width);
            infoHeader.biHeight = -static_cast<LONG>(_height);
            infoHeader.biPlanes = 1;
            infoHeader.biBitCount = 32;
            fwrite(&fileHeader, sizeof(fileHeader), 1, file);
            fwrite(&infoHeader, sizeof(infoHeader), 1, file);
            fwrite(_accumulation, 1, bytes, file);
            fclose(file);
            Log::Info("hud", "union of %u HUDs written to swbf-vr-hud-accum.bmp", _accumulatedFrameCount / 3);
        }
        free(_accumulation);
        _accumulation = nullptr;
    }
}
} // namespace

void Accumulate(unsigned milliseconds)
{
    _accumulatedFrameCount = 0;
    _accumulateUntilTick = (GetTickCount() + milliseconds) | 1;
    Log::Info("hud", "collecting every HUD for %u ms", milliseconds);
}

int CurrentMode()
{
    return _mode;
}

void SetMode(int mode)
{
    _mode = mode ? 1 : 0;
    Log::Info("hud", "%s", _mode == 0 ? "old viewport panel" : "drawn through the HUD texture");
}

void SetLayout(int layout, float spread, float scale)
{
    if (layout >= 0 && layout <= 2)
    {
        _layout = layout;
    }
    if (spread >= 0.2f && spread <= 1.0f)
    {
        _spread = spread;
    }
    if (scale >= 0.5f && scale <= 3.0f)
    {
        _scale = scale;
    }
    Log::Info(
        "hud",
        "layout %s, spread %.0f %%, element size %.0f %%",
        _layout == 0   ? "classic"
        : _layout == 1 ? "compact"
                       : "immersive",
        static_cast<double>(_spread * 100),
        static_cast<double>(_scale * 100));
}

void SetCrosshair(bool hideWithController)
{
    _shouldHideCrosshairWithController = hideWithController;
    Log::Info("hud", "centre crosshair %s", hideWithController ? "off while a controller aims" : "always on");
}

namespace
{
/// On a lens: the overlay's half width in lens radii (its half height is 3/4 of it). idren2023 2026-11-10: shrink the
/// design rather than widen it, to keep as much of the surround (the E-11's side brackets) on the lens as possible.
float _scopeOverlayLensHalfWide = 1.5f; // run 745: the E-11's whole hexagonal frame just fits the lens
} // namespace

void SetScopeOverlaySize(unsigned percent)
{
    if (percent >= 50 && percent <= 400)
    {
        _scopeOverlayLensHalfWide = static_cast<float>(percent) * 0.01f;
    }
}

void DumpNext(unsigned count)
{
    InterlockedExchange(&_dumpCount, static_cast<LONG>(count));
}

bool IsCompositing()
{
    return _isCompositing;
}

bool HasOverlay()
{
    return _hasOverlay;
}

float OverlayScale()
{
    return _overlayScale;
}

// The device is about to be reset: everything here lives in the default pool.
void OnReset()
{
    ReleaseTextures();
    _device = nullptr;
    ReleaseScope();
    // State blocks must go before a Reset too: the vignette's was kept, so with the vignette on the Reset after the
    // statistics screen's B failed (D3DERR_INVALIDCALL) and the game crashed on a texture it could not create (headset
    // run 14; in run 13 the vignette was switched on only after the last Reset).
    if (_vignetteSavedState)
    {
        _vignetteSavedState->lpVtbl->Release(_vignetteSavedState);
        _vignetteSavedState = nullptr;
    }
}

// During a headset eye pass: the game set a projection. isHudPass = it is the HUD pass's. Returns false if the texture
// cannot be used (the caller falls back to the viewport panel).
bool OnProjection(void* device, bool isHudPass)
{
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    if (_isCompositing)
    {
        return true;
    }
    if (!isHudPass)
    {
        EndRedirect(d3dDevice);
        return _mode != 0 && !_hasFailed;
    }
    if (_mode == 0)
    {
        return false;
    }
    if (_isRedirected)
    {
        return true;
    }
    IDirect3DSurface9* currentTarget = nullptr;
    if (FAILED(d3dDevice->lpVtbl->GetRenderTarget(d3dDevice, 0, &currentTarget)) || !currentTarget)
    {
        return false;
    }
    D3DSURFACE_DESC desc{};
    currentTarget->lpVtbl->GetDesc(currentTarget, &desc);
    if (!EnsureTexture(d3dDevice, desc) || FAILED(d3dDevice->lpVtbl->GetViewport(d3dDevice, &_gameViewport)) ||
        FAILED(d3dDevice->lpVtbl->SetRenderTarget(d3dDevice, 0, _surface)))
    {
        currentTarget->lpVtbl->Release(currentTarget);
        return false;
    }
    _previousTarget = currentTarget;
    _isRedirected = true;
    if (!_hasHud)
    {
        d3dDevice->lpVtbl->Clear(d3dDevice, 0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);
        _hasHud = true;
    }
    d3dDevice->lpVtbl->SetViewport(d3dDevice, &_gameViewport);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    return true;
}

/// The droideka's vision (PLAN 58 g): the eye image multiplied by this colour (red kept, green and blue dimmed), a dark
/// scan line every SCAN_LINE_PIXELS rows at this alpha, and the HUD panel times DROIDEKA_HUD_FACTOR (red).
/// Headset run 19 (idren2023: still too bright and saturated): red 0.75, green 0.52, blue 0.47 (was 1.0, 0.48, 0.43).
constexpr D3DCOLOR DROIDEKA_VISION_MULTIPLY = 0xFFC08478;
constexpr D3DCOLOR DROIDEKA_SCAN_LINE = 0x30000000;
constexpr int SCAN_LINE_PIXELS = 3;
constexpr D3DCOLOR DROIDEKA_HUD_FACTOR = 0xFFFF5A46;

// The droideka's red computer-screen view over the whole eye image, under the HUD. The HUD's state block is captured
// and its render states set (z off, no culling); this sets its own blending and stage 0.
void DrawDroidekaVision(IDirect3DDevice9* d3dDevice)
{
    D3DVIEWPORT9 viewport;
    if (FAILED(d3dDevice->lpVtbl->GetViewport(d3dDevice, &viewport)))
    {
        return;
    }
    const float left = static_cast<float>(viewport.X) - 0.5f, top = static_cast<float>(viewport.Y) - 0.5f;
    const float right = left + static_cast<float>(viewport.Width), bottom = top + static_cast<float>(viewport.Height);
    const ScreenVertex quad[4] = {
        {left, top, 0, 1, DROIDEKA_VISION_MULTIPLY},
        {right, top, 0, 1, DROIDEKA_VISION_MULTIPLY},
        {left, bottom, 0, 1, DROIDEKA_VISION_MULTIPLY},
        {right, bottom, 0, 1, DROIDEKA_VISION_MULTIPLY}};
    d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, nullptr);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_ZERO); // destination times the colour
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
    d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ScreenVertex));
    // Scan lines, in batches.
    constexpr int BATCH = 256;
    static ScreenVertex lines[BATCH * 2];
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    int count = 0;
    for (float y = top + 1.0f; y < bottom; y += static_cast<float>(SCAN_LINE_PIXELS))
    {
        lines[count * 2] = {left, y, 0, 1, DROIDEKA_SCAN_LINE};
        lines[count * 2 + 1] = {right, y, 0, 1, DROIDEKA_SCAN_LINE};
        if (++count == BATCH)
        {
            d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_LINELIST, BATCH, lines, sizeof(ScreenVertex));
            count = 0;
        }
    }
    if (count)
    {
        d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_LINELIST, static_cast<UINT>(count), lines, sizeof(ScreenVertex));
    }
}

// At the end of an eye pass, before the eye image is taken: draw the HUD into it.
//   eyeMatrix     the eye's camera matrix (rows right, up, back, position; world)
//   headPosition  the point between the eyes (world): panels that face the player face this point in both eyes
//   frameMatrix   the frame the panel hangs in (the aim camera)
//   isWhole       show the whole HUD as one panel (menus, spawn screen) whatever the layout
void Composite(
    void* device,
    int eyeIndex,
    const float* eyeMatrix,
    const float* headPosition,
    float tanX,
    float tanY,
    const float* frameMatrix,
    float distanceMeters,
    float halfWidthTan,
    bool isWhole)
{
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    EndRedirect(d3dDevice);
    if (!_hasHud || !_surface)
    {
        return;
    }
    _hasHud = false;
    if (_dumpCount > 0)
    {
        InterlockedDecrement(&_dumpCount);
        DumpTexture(d3dDevice);
    }
    AccumulateFrame(d3dDevice);
    // As a droideka (PLAN 58 g, idren2023 after headset run 17): the classic HUD, tinted red, over a red computer-screen
    // view (DrawDroidekaVision), whatever the layout.
    const bool isDroidekaVision = Aim::IsDroideka() && !isWhole;
    const int layout = isDroidekaVision ? 0 : _layout;
    if (!isWhole && layout != 0)
    {
        RunProbes(d3dDevice);
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
    _isCompositing = true;
    const bool isOverlayOnly = _hasOverlay && !isWhole; // the big map or a list over the playing HUD, no menu
    _overlayScale = isOverlayOnly ? OVERLAY_ONLY_SCALE : 1.0f;
    if (_hasOverlay)
    {
        isWhole = true;
    }

    static Vertex vertices[6 * MAXIMUM_QUAD_COUNT];
    static GroupDrawVertex groupVertices[GROUP_VERTEX_CAPACITY];
    static GroupDraw groupDraws[GROUP_DRAW_CAPACITY];
    int groupVertexCount = 0, groupDrawCount = 0;
    static ColourVertex glow[3 * 320], lines[2 * 480];
    int quadCount = 0, glowTriangleCount = 0, lineCount = 0;
    float fireLineFrom[3], fireLineDirection[3], rightHand[16], leftHand[16];
    const bool hasAimingLine =
        !isWhole && _shouldHideCrosshairWithController && Aim::GetFireLine(fireLineFrom, fireLineDirection);
    const bool areElementsMoved = !isWhole && layout != 0;
    const bool isImmersive = areElementsMoved && layout == 2;
    const Vehicle::Seat seat = Vehicle::CurrentSeat();
    const bool isInVehicle = seat != Vehicle::Seat::OnFoot && seat != Vehicle::Seat::None; // no gun in the hand
    const bool isAmmoOnGun = isImmersive && !isInVehicle && Aim::GetHandWorld(0, rightHand);
    // nothing in the hand (headset run 12) or a secondary item (idren2023 2026-11-22): no weapon HUD
    const bool isAmmoShown = !Holster::HoldsNothing() && !Holster::HoldsSecondary();
    // In a stock tank's cabin the groups are on its dashboard's displays (CockpitShell): not moved, not on the wrist.
    const bool isOnDashboard = isInVehicle && CockpitShell::HasDisplays();
    const bool hasWristPanel = isImmersive && !isOnDashboard && Aim::GetHandWorld(1, leftHand);
    // The wrist panel fades in while the left palm is up and out when it turns away (0.15 s).
    // It must be meant (it came up while walking and while reaching for the rifle: headset run 8): palm up, not
    // at the gun (Aim::IsLeftHandAtGun, b129: it came up at every sniper foregrip), the hand at least 22 cm from the
    // right hand, the head turned toward the palm (within about 37 degrees to come up, 45 to stay), and all of that for
    // a quarter of a second. Decided once per frame (left eye).
    const DWORD now = GetTickCount();
    if (eyeIndex == 0 || !hasWristPanel)
    {
        bool isMeant = hasWristPanel && Gesture::CurrentHand(1).isPalmUp && !Aim::IsLeftHandAtGun();
        if (isMeant)
        {
            FpGuard guard;
            const float toPalm[3] = {
                leftHand[12] - headPosition[0],
                leftHand[13] - headPosition[1],
                leftHand[14] - headPosition[2]};
            const float distanceToPalm = sqrtf(toPalm[0] * toPalm[0] + toPalm[1] * toPalm[1] + toPalm[2] * toPalm[2]);
            const float lookingAtPalm =
                distanceToPalm > 0.05f
                    ? -(toPalm[0] * eyeMatrix[8] + toPalm[1] * eyeMatrix[9] + toPalm[2] * eyeMatrix[10]) /
                          distanceToPalm
                    : 0.0f;
            if (lookingAtPalm < (_isWristWanted ? WRIST_LOOK_TO_KEEP : WRIST_LOOK_TO_SHOW))
            {
                isMeant = false;
            }
            if (isAmmoOnGun)
            {
                const float apartX = leftHand[12] - rightHand[12], apartY = leftHand[13] - rightHand[13],
                            apartZ = leftHand[14] - rightHand[14];
                if (apartX * apartX + apartY * apartY + apartZ * apartZ <
                    WRIST_MINIMUM_HAND_DISTANCE_METERS * WRIST_MINIMUM_HAND_DISTANCE_METERS)
                {
                    isMeant = false;
                }
            }
        }
        if (!isMeant)
        {
            _wristMeantSinceTick = 0;
        }
        else if (!_wristMeantSinceTick)
        {
            _wristMeantSinceTick = now | 1;
        }
        // Signed: the tick is stored `now | 1`, one more than now on an even tick (unsigned, the hold was skipped).
        _isWristMeant = isMeant && (_isWristWanted || static_cast<LONG>(now - _wristMeantSinceTick) >=
                                                          static_cast<LONG>(WRIST_HOLD_MILLISECONDS));
    }
    const bool isWristWanted = hasWristPanel && _isWristMeant;
    if (isWristWanted != _isWristWanted)
    {
        _isWristWanted = isWristWanted;
        _wristChangeTick = now;
    }
    float fade = static_cast<float>(now - _wristChangeTick) / WRIST_FADE_MILLISECONDS;
    if (fade > 1)
    {
        fade = 1;
    }
    if (!isWristWanted)
    {
        fade = 1 - fade;
    }
    if (!hasWristPanel)
    {
        fade = 0;
    }
    { // every 2 s: what is being shown
        static DWORD lastStateLogTick = 0;
        if (now - lastStateLogTick >= 2000)
        {
            lastStateLogTick = now;
            Log::Info(
                "hud",
                "state: layout %d, whole %d (overlay %d), aiming line %d, ammunition on the gun %d, wrist "
                "panel %d (palm up %d, fade %d %%), dashboard %d",
                layout,
                isWhole ? 1 : 0,
                _hasOverlay ? 1 : 0,
                hasAimingLine ? 1 : 0,
                isAmmoOnGun ? 1 : 0,
                hasWristPanel ? 1 : 0,
                isWristWanted ? 1 : 0,
                static_cast<int>(fade * 100),
                isOnDashboard ? 1 : 0);
        }
    }
    D3DMATRIX view{}, projection{};
    {
        FpGuard guard;
        const float halfWidth = distanceMeters * halfWidthTan,
                    halfHeight = halfWidth * static_cast<float>(_height) / static_cast<float>(_width);
        // A piece of the texture drawn with its centre at (toU, toV) of the panel.
        const auto piece = [&](float u0, float v0, float u1, float v1, float toU, float toV, float size = 1.0f)
        {
            WriteQuad(
                vertices + 6 * quadCount++,
                frameMatrix,
                distanceMeters,
                (toU * 2 - 1) * halfWidth,
                (1 - toV * 2) * halfHeight,
                (u1 - u0) * halfWidth * size,
                (v1 - v0) * halfHeight * size,
                u0,
                v0,
                u1,
                v1);
        };
        // A group the game drew (HudDraws), redrawn from its textures: straight, `width` metres wide, centre at
        // (centerX, centerY) on the plane of planeMatrix (`ahead` metres in front), alpha scaled by alphaScale.
        // Returns false when the game has not drawn it lately (the caller falls back to the HUD picture).
        const auto drawnGroup = [&](HudDraws::Group group,
                                    const float* planeMatrix,
                                    float ahead,
                                    float centerX,
                                    float centerY,
                                    float width,
                                    float alphaScale)
        {
            HudDraws::GroupDrawing drawing{};
            if (!HudDraws::GetGroup(group, drawing))
            {
                return false;
            }
            const float scale = width / (drawing.maxX - drawing.minX), middleX = (drawing.minX + drawing.maxX) * 0.5f,
                        middleY = (drawing.minY + drawing.maxY) * 0.5f;
            for (int batchIndex = 0; batchIndex < drawing.batchCount && groupDrawCount < GROUP_DRAW_CAPACITY;
                 ++batchIndex)
            {
                const HudDraws::GroupBatch& batch = drawing.batches[batchIndex];
                if (groupVertexCount + batch.vertexCount > GROUP_VERTEX_CAPACITY)
                {
                    break;
                }
                float alpha = static_cast<float>(batch.colour >> 24) / 255.0f * alphaScale;
                alpha = alpha > 1 ? 1 : (alpha < 0 ? 0 : alpha);
                const D3DCOLOR colour = (static_cast<D3DCOLOR>(alpha * 255) << 24) | (batch.colour & 0xFFFFFF);
                groupDraws[groupDrawCount++] =
                    GroupDraw{batch.d3dTexture, batch.d3dMask, groupVertexCount, batch.vertexCount};
                for (int index = 0; index < batch.vertexCount; ++index)
                {
                    const HudDraws::GroupVertex& from = drawing.vertices[batch.firstVertex + index];
                    const float x = centerX + (from.x - middleX) * scale * drawing.signX,
                                y = centerY + (from.y - middleY) * scale * drawing.signY, z = -ahead;
                    groupVertices[groupVertexCount++] = GroupDrawVertex{
                        x * planeMatrix[0] + y * planeMatrix[4] + z * planeMatrix[8] + planeMatrix[12],
                        x * planeMatrix[1] + y * planeMatrix[5] + z * planeMatrix[9] + planeMatrix[13],
                        x * planeMatrix[2] + y * planeMatrix[6] + z * planeMatrix[10] + planeMatrix[14],
                        colour,
                        from.u,
                        from.v,
                        from.maskU,
                        from.maskV};
                }
            }
            return true;
        };
        // A group in the compact layout: its place on the game's screen moved toward the centre, its screen width
        // times the layout's scale.
        const auto movedGroup = [&](HudDraws::Group group)
        {
            HudDraws::GroupDrawing drawing{};
            if (isOnDashboard && CockpitShell::ShowsGroup(static_cast<int>(group)))
            {
                return false; // on a screen in the cabin
            }
            if (!HudDraws::GetGroup(group, drawing))
            {
                return false;
            }
            const float centerU = 0.5f + ((drawing.screenLeft + drawing.screenRight) * 0.5f - 0.5f) * _spread,
                        centerV = 0.5f + ((drawing.screenTop + drawing.screenBottom) * 0.5f - 0.5f) * _spread;
            return drawnGroup(
                group,
                frameMatrix,
                distanceMeters,
                (centerU * 2 - 1) * halfWidth,
                (1 - centerV * 2) * halfHeight,
                (drawing.screenRight - drawing.screenLeft) * 2 * halfWidth * _scale,
                1.0f);
        };
        if (isOverlayOnly)
        {
            piece(0, 0, 1, 1, 0.5f, 0.5f, OVERLAY_ONLY_SCALE); // the big map (or a list) alone: larger
        }
        else
        {
            // Everything the game still draws itself: messages, markers, vehicle displays, the reticle unless the
            // hands aim (the groups the layout puts elsewhere are left out of it: HudDraws::SetTaken).
            piece(0, 0, 1, 1, 0.5f, 0.5f);
        }
        // The big map, the lists and the pause menu as holograms (b176-b177, idren2023 after headset run 13 and run185),
        // like the ear menu: scan lines drifting up, a sweep, a flicker and a tint, lit from the ear menu's projector.
        // Not the spawn screen.
        if (isOverlayOnly || (isWhole && Settings::IsMenuOpen()))
        {
            const float size = isOverlayOnly ? OVERLAY_ONLY_SCALE : 1.0f;
            const float panelHalfWidth = halfWidth * size, panelHalfHeight = halfHeight * size;
            // The light covers only the largest piece of the panel (b206, headset run 14): the big map without the
            // objective text, the pause menu's coloured panel without the back button.
            MeasureLitArea(d3dDevice);
            const float litLeft = (_litArea[0] * 2 - 1) * panelHalfWidth,
                        litRight = (_litArea[2] * 2 - 1) * panelHalfWidth;
            const float litBottom = (1 - _litArea[3] * 2) * panelHalfHeight,
                        litTop = (1 - _litArea[1] * 2) * panelHalfHeight;
            const float litHalfHeight = (litTop - litBottom) * 0.5f, litCentreX = (litLeft + litRight) * 0.5f;
            D3DCOLOR base = 0, pale = 0;
            HoloColours(&base, &pale);
            const auto onPanel = [&](float x, float y, float alpha, D3DCOLOR rgb)
            {
                const float z = -distanceMeters;
                const D3DCOLOR alphaByte = static_cast<D3DCOLOR>((alpha > 1 ? 1 : alpha < 0 ? 0 : alpha) * 255);
                return ColourVertex{
                    x * frameMatrix[0] + y * frameMatrix[4] + z * frameMatrix[8] + frameMatrix[12],
                    x * frameMatrix[1] + y * frameMatrix[5] + z * frameMatrix[9] + frameMatrix[13],
                    x * frameMatrix[2] + y * frameMatrix[6] + z * frameMatrix[10] + frameMatrix[14],
                    (alphaByte << 24) | rgb};
            };
            const float timeSeconds = static_cast<float>(GetTickCount() % 60000) * 0.001f;
            const float flicker = 0.92f + 0.05f * sinf(timeSeconds * 31.0f) + 0.03f * sinf(timeSeconds * 7.3f);
            ColourVertex* glowVertex = glow + 3 * glowTriangleCount;
            ColourVertex* lineVertex = lines + 2 * lineCount;
            // Scan lines across, drifting up; one brighter line sweeps up every 4 s.
            constexpr int PANEL_SCAN_LINE_COUNT = 48;
            const float spacing = 2 * panelHalfHeight / PANEL_SCAN_LINE_COUNT;
            const float drift = fmodf(timeSeconds, 1.0f) * spacing;
            for (int scanLine = 0; scanLine < PANEL_SCAN_LINE_COUNT; ++scanLine)
            {
                const float y = litBottom + drift + static_cast<float>(scanLine) * spacing;
                if (y >= litTop)
                {
                    continue;
                }
                *lineVertex++ = onPanel(litLeft, y, 0.0f, base);
                *lineVertex++ = onPanel(litCentreX, y, 0.07f, pale);
                *lineVertex++ = onPanel(litCentreX, y, 0.07f, pale);
                *lineVertex++ = onPanel(litRight, y, 0.0f, base);
            }
            const float sweep = litBottom + fmodf(timeSeconds, 4.0f) / 4.0f * 2 * litHalfHeight;
            *lineVertex++ = onPanel(litLeft, sweep, 0.0f, pale);
            *lineVertex++ = onPanel(litCentreX, sweep, 0.35f, pale);
            *lineVertex++ = onPanel(litCentreX, sweep, 0.35f, pale);
            *lineVertex++ = onPanel(litRight, sweep, 0.0f, pale);
            // The light comes from the ear menu's projector at the ear (b177, idren2023 after run185: no frame, no beam
            // from below; the same projector as the ear menu): fine lines and a faint cone to points around the edge.
            float ear[3];
            EarMenu::ProjectorPosition(eyeMatrix, headPosition, ear);
            constexpr int BEAM_COUNT = LIT_OUTLINE_POINTS;
            const auto edgePoint = [&](int index, float* x, float* y)
            { // walks the lit piece's own outline (MeasureLitArea), or its rectangle until it was measured
                if (_hasLitOutline)
                {
                    *x = (_litOutline[index][0] * 2 - 1) * panelHalfWidth;
                    *y = (1 - _litOutline[index][1] * 2) * panelHalfHeight;
                    return;
                }
                const int perSide = BEAM_COUNT / 4;
                const float along = static_cast<float>(index % perSide) / static_cast<float>(perSide);
                switch (index / perSide)
                {
                case 0:
                    *x = litLeft + (litRight - litLeft) * along;
                    *y = litBottom;
                    break;
                case 1:
                    *x = litRight;
                    *y = litBottom + (litTop - litBottom) * along;
                    break;
                case 2:
                    *x = litRight - (litRight - litLeft) * along;
                    *y = litTop;
                    break;
                default:
                    *x = litLeft;
                    *y = litTop - (litTop - litBottom) * along;
                    break;
                }
            };
            const ColourVertex earVertex{ear[0], ear[1], ear[2], base};
            for (int beam = 0; beam < BEAM_COUNT; ++beam)
            {
                float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
                edgePoint(beam, &x0, &y0);
                edgePoint((beam + 1) % BEAM_COUNT, &x1, &y1);
                *glowVertex++ = earVertex;
                *glowVertex++ = onPanel(x0, y0, 0.05f * flicker, base);
                *glowVertex++ = onPanel(x1, y1, 0.05f * flicker, base);
                *lineVertex++ = earVertex;
                *lineVertex++ = onPanel(x0, y0, 0.22f * flicker, pale);
            }
            ColourVertex* const glowEnd = glow + sizeof(glow) / sizeof(glow[0]);
            // The source: a bright core with a halo, facing the eye.
            const float pulse = 0.85f + 0.15f * sinf(timeSeconds * 9.0f);
            const auto sourceRim = [&](float angle, float radius, float alpha)
            {
                const float across = cosf(angle) * radius, up = sinf(angle) * radius;
                const D3DCOLOR alphaByte = static_cast<D3DCOLOR>((alpha > 1 ? 1 : alpha) * 255);
                return ColourVertex{
                    ear[0] + across * eyeMatrix[0] + up * eyeMatrix[4],
                    ear[1] + across * eyeMatrix[1] + up * eyeMatrix[5],
                    ear[2] + across * eyeMatrix[2] + up * eyeMatrix[6],
                    (alphaByte << 24) | pale};
            };
            for (int segment = 0; segment < HOLOGRAM_SOURCE_SEGMENTS && glowVertex + 9 <= glowEnd; ++segment)
            {
                const float angle0 = TWO_PI_HUD * static_cast<float>(segment) / HOLOGRAM_SOURCE_SEGMENTS,
                            angle1 = TWO_PI_HUD * static_cast<float>(segment + 1) / HOLOGRAM_SOURCE_SEGMENTS;
                *glowVertex++ = ColourVertex{ear[0], ear[1], ear[2], (static_cast<D3DCOLOR>(250) << 24) | pale};
                *glowVertex++ = sourceRim(angle0, HOLOGRAM_SOURCE_CORE_METERS, 0.75f * pulse);
                *glowVertex++ = sourceRim(angle1, HOLOGRAM_SOURCE_CORE_METERS, 0.75f * pulse);
                *glowVertex++ = sourceRim(angle0, HOLOGRAM_SOURCE_CORE_METERS, 0.45f * pulse);
                *glowVertex++ = sourceRim(angle0, HOLOGRAM_SOURCE_HALO_METERS, 0.0f);
                *glowVertex++ = sourceRim(angle1, HOLOGRAM_SOURCE_CORE_METERS, 0.45f * pulse);
                *glowVertex++ = sourceRim(angle1, HOLOGRAM_SOURCE_CORE_METERS, 0.45f * pulse);
                *glowVertex++ = sourceRim(angle0, HOLOGRAM_SOURCE_HALO_METERS, 0.0f);
                *glowVertex++ = sourceRim(angle1, HOLOGRAM_SOURCE_HALO_METERS, 0.0f);
            }
            glowTriangleCount = static_cast<int>(glowVertex - glow) / 3;
            lineCount = static_cast<int>(lineVertex - lines) / 2;
            // The panel itself a little toward the light's colour, flickering with it (colour is premultiplied, so
            // colour and alpha are scaled alike).
            const D3DCOLOR alphaByte = static_cast<D3DCOLOR>(flicker * 255);
            const auto channel = [&](int shift)
            {
                return static_cast<D3DCOLOR>((((base >> shift) & 0xFF) / 6 + 212) * flicker) << shift;
            };
            _panelFactor = (alphaByte << 24) | channel(16) | channel(8) | channel(0);
        }
        else
        {
            // Groups not on the gun or the wrist: moved toward the centre (compact, and immersive without the hand).
            if (areElementsMoved)
            {
                if (!isAmmoOnGun)
                {
                    movedGroup(HudDraws::Group::Ammunition);
                    movedGroup(HudDraws::Group::Target);
                }
                if (!hasWristPanel)
                {
                    movedGroup(HudDraws::Group::Health);
                    movedGroup(HudDraws::Group::Score);
                    movedGroup(HudDraws::Group::Map);
                }
            }
            // Ammunition on the gun: a small sign on the left side above the receiver, facing the shooter.
            // Drawn twice: the game's box is mostly transparent and would be lost against the gun.
            // It lies along the barrel on the gun's left side, facing left, reading from the muzzle end toward the
            // shooter (idren2023, headset run 8: turned a quarter clockwise seen from above, nearer the right hand).
            if (isAmmoOnGun && isAmmoShown)
            {
                const float* gunMatrix = rightHand;
                float signMatrix[16] = {};
                for (int axis = 0; axis < 3; ++axis)
                {
                    signMatrix[axis] = gunMatrix[8 + axis];     // the sign's right = the gun's back
                    signMatrix[4 + axis] = gunMatrix[4 + axis]; // up = the gun's up
                    signMatrix[8 + axis] = -gunMatrix[axis];    // facing = the gun's left
                    signMatrix[12 + axis] =
                        gunMatrix[12 + axis] - 0.030f * gunMatrix[axis] + 0.075f * gunMatrix[4 + axis] -
                        0.045f * gunMatrix[8 + axis]; // 3 cm left, 7.5 cm up, 4.5 cm ahead of the hand
                }
                float barrelMiddle[3];
                if (Aim::GetBarrelMiddleWorld(barrelMiddle))
                {
                    // The super battle droid: beside its barrels, halfway along them (b160, idren2023: it was far ahead).
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        signMatrix[12 + axis] =
                            barrelMiddle[axis] - 0.035f * gunMatrix[axis] + 0.025f * gunMatrix[4 + axis];
                    }
                }
                signMatrix[15] = 1;
                drawnGroup(
                    HudDraws::Group::Ammunition,
                    signMatrix,
                    0,
                    0,
                    0,
                    AMMO_SIGN_WIDTH_METERS,
                    GROUP_ALPHA_ON_HAND);
                // The health of what the player aims at (PLAN 11, 28; idren2023 2026-10-14: on the gun HUD, its icons in
                // line with the weapon and ammunition icons): under the ammunition at the same scale, its hexagon icon
                // (at its right end) under the ammunition's icon column (at its left end).
                HudDraws::GroupDrawing ammoDrawing{}, targetDrawing{};
                if (HudDraws::GetGroup(HudDraws::Group::Ammunition, ammoDrawing) &&
                    HudDraws::GetGroup(HudDraws::Group::Target, targetDrawing))
                {
                    const float metersPerUnit = AMMO_SIGN_WIDTH_METERS / (ammoDrawing.maxX - ammoDrawing.minX);
                    const float targetWidth = (targetDrawing.maxX - targetDrawing.minX) * metersPerUnit;
                    const float ammoHalfHeight = (ammoDrawing.maxY - ammoDrawing.minY) * 0.5f * metersPerUnit,
                                targetHalfHeight = (targetDrawing.maxY - targetDrawing.minY) * 0.5f * metersPerUnit;
                    drawnGroup(
                        HudDraws::Group::Target,
                        signMatrix,
                        0,
                        AMMO_SIGN_WIDTH_METERS * (AMMO_ICON_FROM_LEFT - 0.5f) -
                            targetWidth * (0.5f - TARGET_ICON_FROM_RIGHT),
                        -(ammoHalfHeight + TARGET_SIGN_GAP_METERS + targetHalfHeight),
                        targetWidth,
                        GROUP_ALPHA_ON_HAND);
                }
            }
            // Wrist panel: over the left controller, upright, facing the head.
            if (hasWristPanel && fade > 0.01f)
            {
                float planeMatrix[16] = {};
                // The projector is the palm (headset run 19: it hung ahead of the hand): the left grip pose's centre
                // (the middle of the handle, in the real palm) 4 cm out along the palm's normal (the left grip pose's
                // +x); the aim pose 8 cm ahead as before when the grip pose is missing.
                float palmPosition[3] = {
                    leftHand[12] - leftHand[8] * 0.08f,
                    leftHand[13] - leftHand[9] * 0.08f + 0.02f,
                    leftHand[14] - leftHand[10] * 0.08f};
                float gripPose[16], base[16];
                if (Xr::GetGripPose(1, gripPose) && Aim::GetBaseFrame(base))
                {
                    float grip[16];
                    for (int row = 0; row < 4; ++row)
                    {
                        for (int column = 0; column < 4; ++column)
                        {
                            grip[row * 4 + column] = gripPose[row * 4] * base[column] + gripPose[row * 4 + 1] * base[4 + column] +
                                                     gripPose[row * 4 + 2] * base[8 + column] +
                                                     gripPose[row * 4 + 3] * base[12 + column];
                        }
                    }
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        palmPosition[axis] = grip[12 + axis] + grip[axis] * 0.04f;
                    }
                }
                float back[3] = {
                    headPosition[0] - palmPosition[0],
                    0.0f,
                    headPosition[2] - palmPosition[2]}; // upright: turned toward the head, never tilted (headset run 8)
                const float backLength = sqrtf(back[0] * back[0] + back[1] * back[1] + back[2] * back[2]);
                if (backLength > 0.01f)
                {
                    for (float& component : back)
                    {
                        component /= backLength;
                    }
                    float right[3] = {back[2], 0, -back[0]}; // world up x back
                    const float rightLength = sqrtf(right[0] * right[0] + right[2] * right[2]);
                    if (rightLength > 0.01f)
                    {
                        right[0] /= rightLength;
                        right[2] /= rightLength;
                        const float up[3] = {
                            back[1] * right[2] - back[2] * right[1],
                            back[2] * right[0] - back[0] * right[2],
                            back[0] * right[1] - back[1] * right[0]}; // back x right
                        planeMatrix[0] = right[0];
                        planeMatrix[1] = right[1];
                        planeMatrix[2] = right[2];
                        planeMatrix[4] = up[0];
                        planeMatrix[5] = up[1];
                        planeMatrix[6] = up[2];
                        planeMatrix[8] = back[0];
                        planeMatrix[9] = back[1];
                        planeMatrix[10] = back[2];
                        planeMatrix[12] = palmPosition[0];
                        planeMatrix[13] = palmPosition[1];
                        planeMatrix[14] = palmPosition[2];
                        planeMatrix[15] = 1;
                        // Hologram: a pool of light over the hand with the map in it, the health bar and the score
                        // above; a beam from the hand up to it; fine lines drifting upward; a slight flicker.
                        const float timeSeconds = static_cast<float>(now % 60000) * 0.001f;
                        fade *= 0.90f + 0.06f * sinf(timeSeconds * 31.0f) + 0.04f * sinf(timeSeconds * 7.3f);
                        drawnGroup(
                            HudDraws::Group::Map,
                            planeMatrix,
                            0,
                            0.0f,
                            0.125f,
                            0.125f,
                            GROUP_ALPHA_ON_HAND * fade);
                        drawnGroup(
                            HudDraws::Group::Health,
                            planeMatrix,
                            0,
                            0.0f,
                            0.215f,
                            0.120f,
                            GROUP_ALPHA_ON_HAND * fade);
                        drawnGroup(
                            HudDraws::Group::Score,
                            planeMatrix,
                            0,
                            0.0f,
                            0.258f,
                            0.130f,
                            GROUP_ALPHA_ON_HAND * fade);
                        const auto at = [&](float x, float y, float alpha, D3DCOLOR rgb)
                        {
                            const D3DCOLOR alphaByte =
                                static_cast<D3DCOLOR>((alpha * fade > 1 ? 1 : alpha * fade) * 255);
                            return ColourVertex{
                                x * planeMatrix[0] + y * planeMatrix[4] + planeMatrix[12],
                                x * planeMatrix[1] + y * planeMatrix[5] + planeMatrix[13],
                                x * planeMatrix[2] + y * planeMatrix[6] + planeMatrix[14],
                                (alphaByte << 24) | rgb};
                        };
                        const auto world = [&](float x, float y, float z, float alpha, D3DCOLOR rgb) {   // offsets along the world's axes
                            const D3DCOLOR alphaByte = static_cast<D3DCOLOR>((alpha * fade > 1 ? 1 : alpha * fade) * 255);
                            return ColourVertex{planeMatrix[12] + x, planeMatrix[13] + y, planeMatrix[14] + z, (alphaByte << 24) | rgb};
                        };
                        D3DCOLOR blue = 0, pale = 0; // the light's colours (named for the default side)
                        HoloColours(&blue, &pale);
                        const float poolCenterY = 0.165f, poolRadiusX = 0.085f,
                                    poolRadiusY = 0.125f; // the pool of light
                        const float segmentRadians = 6.2831853f / HOLOGRAM_SEGMENT_COUNT;
                        ColourVertex* glowVertex = glow;
                        ColourVertex* lineVertex = lines;
                        for (int segment = 0; segment < HOLOGRAM_SEGMENT_COUNT; ++segment)
                        {
                            const float angleStart = static_cast<float>(segment) * segmentRadians,
                                        angleEnd = static_cast<float>(segment + 1) * segmentRadians;
                            // the pool of light behind the pieces
                            *glowVertex++ = at(0, poolCenterY, 0.22f, blue);
                            *glowVertex++ =
                                at(poolRadiusX * cosf(angleStart),
                                   poolCenterY + poolRadiusY * sinf(angleStart),
                                   0,
                                   blue);
                            *glowVertex++ =
                                at(poolRadiusX * cosf(angleEnd), poolCenterY + poolRadiusY * sinf(angleEnd), 0, blue);
                            // the beam: a cone from the palm up to a level ring 12 cm across under the map, fading
                            // upward
                            const float beamRadius = 0.06f, beamHeight = 0.065f;
                            *glowVertex++ = world(0, 0, 0, 0.28f, pale);
                            *glowVertex++ = world(
                                beamRadius * cosf(angleStart),
                                beamHeight,
                                beamRadius * sinf(angleStart),
                                0.02f,
                                blue);
                            *glowVertex++ = world(
                                beamRadius * cosf(angleEnd),
                                beamHeight,
                                beamRadius * sinf(angleEnd),
                                0.02f,
                                blue);
                            *lineVertex++ = world(
                                beamRadius * cosf(angleStart),
                                beamHeight,
                                beamRadius * sinf(angleStart),
                                0.35f,
                                pale);
                            *lineVertex++ = world(
                                beamRadius * cosf(angleEnd),
                                beamHeight,
                                beamRadius * sinf(angleEnd),
                                0.35f,
                                pale);
                            // the emitter: a small level ring on the palm and a soft bright dot
                            *lineVertex++ = world(0.018f * cosf(angleStart), 0, 0.018f * sinf(angleStart), 0.7f, pale);
                            *lineVertex++ = world(0.018f * cosf(angleEnd), 0, 0.018f * sinf(angleEnd), 0.7f, pale);
                            *glowVertex++ = at(0, 0, 0.9f, pale);
                            *glowVertex++ = at(0.009f * cosf(angleStart), 0.009f * sinf(angleStart), 0, pale);
                            *glowVertex++ = at(0.009f * cosf(angleEnd), 0.009f * sinf(angleEnd), 0, pale);
                            if (segment % 12 == 0)
                            {
                                *lineVertex++ = world(0, 0, 0, 0.45f, pale);
                                *lineVertex++ = world(
                                    beamRadius * cosf(angleStart),
                                    beamHeight,
                                    beamRadius * sinf(angleStart),
                                    0.05f,
                                    blue);
                            }
                        }
                        glowTriangleCount = static_cast<int>(glowVertex - glow) / 3;
                        // Scan lines every 6 mm inside the pool, drifting up 6 mm a second; one brighter line sweeps up
                        // every 3 s.
                        const float drift = fmodf(timeSeconds, 1.0f) * 0.006f,
                                    sweep =
                                        poolCenterY - poolRadiusY + fmodf(timeSeconds, 3.0f) / 3.0f * 2 * poolRadiusY;
                        for (int scanLine = 0; scanLine < 40; ++scanLine)
                        {
                            const float y =
                                poolCenterY - poolRadiusY + 0.004f + drift + static_cast<float>(scanLine) * 0.006f;
                            const float heightFraction = (y - poolCenterY) / poolRadiusY;
                            if (heightFraction <= -1 || heightFraction >= 1)
                            {
                                continue;
                            }
                            const float halfChord = poolRadiusX * sqrtf(1 - heightFraction * heightFraction);
                            *lineVertex++ = at(-halfChord, y, 0, blue);
                            *lineVertex++ = at(0, y, 0.20f, pale);
                            *lineVertex++ = at(0, y, 0.20f, pale);
                            *lineVertex++ = at(halfChord, y, 0, blue);
                        }
                        {
                            const float heightFraction = (sweep - poolCenterY) / poolRadiusY,
                                        halfChord = poolRadiusX * sqrtf(
                                                                      heightFraction > -1 && heightFraction < 1
                                                                          ? 1 - heightFraction * heightFraction
                                                                          : 0);
                            *lineVertex++ = at(-halfChord, sweep, 0, pale);
                            *lineVertex++ = at(0, sweep, 0.70f, pale);
                            *lineVertex++ = at(0, sweep, 0.70f, pale);
                            *lineVertex++ = at(halfChord, sweep, 0, pale);
                        }
                        lineCount = static_cast<int>(lineVertex - lines) / 2;
                    }
                }
            }
        }
        // View = inverse of the eye's camera matrix (rigid). Projection: right-handed, symmetric, row vectors.
        float* viewValues = &view.m[0][0];
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                viewValues[row * 4 + column] = eyeMatrix[column * 4 + row];
            }
        }
        for (int column = 0; column < 3; ++column)
        {
            viewValues[12 + column] =
                -(eyeMatrix[12] * viewValues[column] + eyeMatrix[13] * viewValues[4 + column] +
                  eyeMatrix[14] * viewValues[8 + column]);
        }
        viewValues[15] = 1;
        const float nearMeters = QUAD_NEAR_METERS, farMeters = QUAD_FAR_METERS;
        float* projectionValues = &projection.m[0][0];
        projectionValues[0] = 1.0f / tanX;
        projectionValues[5] = 1.0f / tanY;
        projectionValues[10] = farMeters / (nearMeters - farMeters);
        projectionValues[11] = -1.0f;
        projectionValues[14] = nearMeters * farMeters / (nearMeters - farMeters);
    }
    // After the player's death the view stays where it was (PLAN 35); a glowing red line from the body to the killer
    // lets the player see where the shot came from by looking, without the camera moving there.
    float body[3], killer[3];
    if (Stereo::GetKillerLine(body, killer))
    {
        FpGuard guard;
        ColourVertex* glowVertex = glow + 3 * glowTriangleCount;
        ColourVertex* const glowEnd = glow + sizeof(glow) / sizeof(glow[0]);
        float direction[3] = {killer[0] - body[0], killer[1] - body[1], killer[2] - body[2]};
        const float length =
            sqrtf(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
        for (int segment = 0; length > 0.1f && segment < KILLER_LINE_SEGMENT_COUNT && glowVertex + 6 <= glowEnd;
             ++segment)
        {
            ColourVertex corners[2][2];
            for (int end = 0; end < 2; ++end)
            {
                const float along = static_cast<float>(segment + end) / KILLER_LINE_SEGMENT_COUNT;
                const float point[3] = {
                    body[0] + direction[0] * along,
                    body[1] + direction[1] * along,
                    body[2] + direction[2] * along};
                const float fromHead[3] = {
                    point[0] - headPosition[0],
                    point[1] - headPosition[1],
                    point[2] - headPosition[2]};
                float side[3] = {
                    direction[1] * fromHead[2] - direction[2] * fromHead[1],
                    direction[2] * fromHead[0] - direction[0] * fromHead[2],
                    direction[0] * fromHead[1] - direction[1] * fromHead[0]};
                const float sideLength = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
                const float distance =
                    sqrtf(fromHead[0] * fromHead[0] + fromHead[1] * fromHead[1] + fromHead[2] * fromHead[2]);
                float halfWidth = distance * KILLER_LINE_HALF_WIDTH_PER_METER;
                halfWidth = halfWidth < KILLER_LINE_MINIMUM_HALF_WIDTH_METERS ? KILLER_LINE_MINIMUM_HALF_WIDTH_METERS
                                                                              : halfWidth;
                const float scale = sideLength > 1e-6f ? halfWidth / sideLength : 0.0f;
                // Bright at the body, fainter toward the killer.
                const D3DCOLOR colour = (static_cast<D3DCOLOR>(255 * (1.0f - 0.5f * along)) << 24) | KILLER_LINE_RGB;
                for (int edge = 0; edge < 2; ++edge)
                {
                    const float sign = edge == 0 ? 1.0f : -1.0f;
                    corners[end][edge] = ColourVertex{
                        point[0] + side[0] * scale * sign,
                        point[1] + side[1] * scale * sign,
                        point[2] + side[2] * scale * sign,
                        colour};
                }
            }
            *glowVertex++ = corners[0][0];
            *glowVertex++ = corners[0][1];
            *glowVertex++ = corners[1][0];
            *glowVertex++ = corners[1][0];
            *glowVertex++ = corners[0][1];
            *glowVertex++ = corners[1][1];
        }
        glowTriangleCount = static_cast<int>(glowVertex - glow) / 3;
    }
    static const D3DMATRIX IDENTITY = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    const bool isInScene = SUCCEEDED(d3dDevice->lpVtbl->BeginScene(d3dDevice));
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_WORLD, &IDENTITY);
    d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_VIEW, &view);
    d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_PROJECTION, &projection);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LIGHTING, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_FOGENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHATESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_STENCILENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SCISSORTESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_COLORWRITEENABLE, 0x7);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    if (isDroidekaVision)
    {
        DrawDroidekaVision(d3dDevice);
        _panelFactor = DROIDEKA_HUD_FACTOR;
    }
    if (glowTriangleCount)
    { // the hologram's light: plain colour, added to the picture
        d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZ | D3DFVF_DIFFUSE);
        d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, nullptr);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_ONE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        d3dDevice->lpVtbl->DrawPrimitiveUP(
            d3dDevice,
            D3DPT_TRIANGLELIST,
            static_cast<UINT>(glowTriangleCount),
            glow,
            sizeof(ColourVertex));
        if (lineCount)
        {
            d3dDevice->lpVtbl
                ->DrawPrimitiveUP(d3dDevice, D3DPT_LINELIST, static_cast<UINT>(lineCount), lines, sizeof(ColourVertex));
        }
    }
    d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZ | D3DFVF_TEX1);
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(_texture));
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_ONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_TEXCOORDINDEX, 0);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    HRESULT result = D3D_OK;
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_TEXTUREFACTOR, _panelFactor);
    _panelFactor = 0xFFFFFFFF;
    if (quadCount)
    {
        result = d3dDevice->lpVtbl->DrawPrimitiveUP(
            d3dDevice,
            D3DPT_TRIANGLELIST,
            static_cast<UINT>(quadCount * 2),
            vertices,
            sizeof(Vertex));
    }
    if (groupDrawCount)
    { // the groups redrawn from the game's textures: texture times colour, plain alpha blending; a masked batch (the
      // minimap) also multiplies its alpha by the mask's in stage 1
        d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        for (int drawIndex = 0; drawIndex < groupDrawCount; ++drawIndex)
        {
            const GroupDraw& draw = groupDraws[drawIndex];
            d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, static_cast<IDirect3DBaseTexture9*>(draw.d3dTexture));
            if (draw.d3dMask)
            {
                d3dDevice->lpVtbl->SetTexture(d3dDevice, 1, static_cast<IDirect3DBaseTexture9*>(draw.d3dMask));
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_TEXCOORDINDEX, 1);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLORARG2, D3DTA_CURRENT);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
            }
            d3dDevice->lpVtbl->DrawPrimitiveUP(
                d3dDevice,
                D3DPT_TRIANGLELIST,
                static_cast<UINT>(draw.vertexCount / 3),
                groupVertices + draw.firstVertex,
                sizeof(GroupDrawVertex));
            if (draw.d3dMask)
            {
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_TEXCOORDINDEX, 1);
                d3dDevice->lpVtbl->SetTexture(d3dDevice, 1, nullptr);
            }
        }
    }
    if (!isWhole)
    {
        EarMenu::Draw(d3dDevice, eyeMatrix, headPosition); // over everything, while a hand at an ear holds its grip
    }
    if (isInScene)
    {
        d3dDevice->lpVtbl->EndScene(d3dDevice);
    }
    _savedState->lpVtbl->Apply(_savedState);
    _isCompositing = false;
    // The game's reticle is left out whenever the right hand is tracked, also with an empty hand or a
    // secondary weapon, where there is no aiming line (PLAN 47).
    // Only on foot (a soldier with holsters, or an aiming line): vehicles keep their own reticle, except gunner seats
    // and the droideka, whose guns' own aiming lines replace it (PLAN 57 g, b).
    HudDraws::SetReticleHidden(
        !isWhole && ((_shouldHideCrosshairWithController && Aim::GetHandWorld(0, rightHand) &&
                      (Holster::HasPlaces() || Aim::GetFireLine(fireLineFrom, fireLineDirection))) ||
                     Sight::IsReplacingReticle()));
    // The game leaves out its own draws of every group when the layout puts them elsewhere (from the next HUD pass on).
    // Not the Map group while it is drawn as the big map (wide on screen): MapDisplay draws both, and taken, the big
    // map never showed (headset run 14: the ear menu's MAP showed only the objective text). Its draws are recorded
    // even while taken, so the width is known; once drawn, the overlay probe makes the panel whole and nothing is
    // taken.
    unsigned takenMask = areElementsMoved ? (1u << static_cast<int>(HudDraws::Group::Count)) - 1 : 0u;
    HudDraws::GroupDrawing mapDrawing{};
    if (HudDraws::GetGroup(HudDraws::Group::Map, mapDrawing) &&
        mapDrawing.screenRight - mapDrawing.screenLeft > BIG_MAP_MINIMUM_WIDTH)
    {
        takenMask &= ~(1u << static_cast<int>(HudDraws::Group::Map));
    }
    HudDraws::SetTaken(takenMask);
    if (++_compositeCount == 1)
    {
        Log::Info(
            "hud",
            "first HUD drawn from the texture (%d quads, result 0x%lx, in a scene of our own: %d)",
            quadCount,
            static_cast<unsigned long>(result),
            isInScene ? 1 : 0);
    }
}

// ---- Scope window (docs/ZOOM.md section 3, B1) ----
//
// While zoomed, Stereo draws the scene once more per frame from the scope's camera with a narrow field of view (the
// scope pass). Its picture is copied into a square texture before the game's HUD pass of that render begins
// (CaptureScope), and every eye pass then draws that texture as a round window in front of the aim (DrawScopeWindow):
// the same picture for both eyes, each placed on its eye's own line, so it lies at infinity. Magnification = the
// window's angle / the scope picture's angle.
namespace
{

/// Size of the square scope texture (pixels); the back buffer is shrunk into it.
constexpr UINT SCOPE_TEXTURE_SIZE = 1024;
/// Segments of the round window and its ring.
constexpr int SCOPE_SEGMENT_COUNT = 64;
/// The ring around the window: black from the edge to this fraction of the radius, then fading out to the outer one.
constexpr float SCOPE_RING_SOLID = 1.06f;
constexpr float SCOPE_RING_OUTER = 1.30f;
/// On the weapon's lens (B2): a thin dark rim out to this fraction of the lens radius, and a near clip plane close
/// enough for a lens held at the eye.
constexpr float SCOPE_LENS_RIM = 1.08f;
/// Projected scope (on a lens): the emitter sits this many picture radii under the picture's centre and this far ahead
/// of it, a dot this size; light reaches out to this many radii around the picture; this many scan lines across it.
constexpr float SCOPE_EMITTER_BELOW = 1.2f;
constexpr float SCOPE_EMITTER_AHEAD_METERS = 0.06f;
constexpr float SCOPE_EMITTER_RADIUS_METERS = 0.004f;
constexpr float SCOPE_HALO_OUTER = 1.35f;
constexpr int SCOPE_SCAN_LINE_COUNT = 14;
/// A lens emitter's light reaches this many lens radii out (b156).
constexpr float SCOPE_LENS_GLOW_OUTER = 1.8f;
constexpr float SCOPE_LENS_NEAR_METERS = 0.001f;
/// The game stretches the overlay's quarter texture over a quarter of its 4:3 screen, so the design is round only
/// when drawn 4/3 wider than tall (idren2023 2026-11-10: the E-11's scope design looked stretched; run 741: its dashed
/// circle 1.3 times taller than wide when drawn square). The half size above is the vertical one.
constexpr float SCOPE_OVERLAY_SCREEN_ASPECT = 4.0f / 3.0f;
/// On a shaped screen the design's full extent is this many times the outline's, along and across the screen: the
/// stock designs' frames lie at about 0.60 of their texture's half width and 0.71 of its half height (run 848, fit
/// 1.04: the frame two thirds of the way out), so these put the frame just inside the glass's edge.
constexpr float SCOPE_OVERLAY_FIT_ALONG = 1.58f;
constexpr float SCOPE_OVERLAY_FIT_ACROSS = 1.30f;
/// The weapon's own reticle in the window: half its longer side as a fraction of the window's radius, and its tint
/// (the game's reticle blue, 0x43A7F2 at ReticuleDisplay 0x208000, a little transparent).
constexpr float SCOPE_RETICLE_HALF_SIZE = 0.35f;
constexpr D3DCOLOR SCOPE_RETICLE_TINT = 0xE043A7F2;
/// ... and the game's enemy / friend reticle colours (offsets "Reticle target relation").
constexpr D3DCOLOR SCOPE_RETICLE_ENEMY = 0xDF2020, SCOPE_RETICLE_FRIEND = 0x20DF20;
/// The weapon's scope overlay (ScopeTexture) over the window: its quarter texture mirrored into four, as ScopeDisplay
/// does, at a half size (in window radii) that puts the overlay's clear circle (0.56 of its half size in the stock
/// weapon_scope textures, run113) on the window's edge. Drawn out to SOLID, fading out to OUTER.
constexpr float SCOPE_OVERLAY_HALF_SIZE = 1.0f / 0.56f;
constexpr float SCOPE_OVERLAY_SOLID = 1.35f;
constexpr float SCOPE_OVERLAY_OUTER = 1.6f;

bool _hasScopeFailed = false;

bool EnsureScopeTexture(IDirect3DDevice9* d3dDevice, D3DFORMAT format)
{
    if (_scopeDevice != d3dDevice)
    {
        ReleaseScope();
        _scopeDevice = d3dDevice;
        _hasScopeFailed = false;
    }
    if (_scopeSurface)
    {
        return true;
    }
    if (_hasScopeFailed)
    {
        return false;
    }
    if (FAILED(d3dDevice->lpVtbl->CreateTexture(
            d3dDevice,
            SCOPE_TEXTURE_SIZE,
            SCOPE_TEXTURE_SIZE,
            1,
            D3DUSAGE_RENDERTARGET,
            format,
            D3DPOOL_DEFAULT,
            &_scopeTexture,
            nullptr)) ||
        FAILED(_scopeTexture->lpVtbl->GetSurfaceLevel(_scopeTexture, 0, &_scopeSurface)))
    {
        ReleaseScope();
        _hasScopeFailed = true;
        Log::Warning("hud", "no scope texture (format %d): no scope window", static_cast<int>(format));
        return false;
    }
    Log::Info("hud", "scope texture %ux%u created", SCOPE_TEXTURE_SIZE, SCOPE_TEXTURE_SIZE);
    return true;
}

} // namespace

void* ScopePicture()
{
    return _scopeTexture;
}

namespace
{

/// The scope picture's edge as SCOPE_SEGMENT_COUNT + 1 points in radii (the last = the first), counter-clockwise: the
/// unit circle, or a convex outline (Zoom::Lens) walked with every corner and every crossing of the axes among the
/// points, the rest spread by length (each segment's triangle from the centre then stays in one quadrant, as the
/// overlay's mirroring needs, and the corners stay sharp). With a tilt the axes are the tilted ones: the outline is
/// turned back by it, walked, and the points turned forward again.
void ScopeEdge(const float* outline, int outlineCount, float (*edge)[2], float tiltDegrees = 0)
{
    const float tiltRadians = tiltDegrees * 0.017453293f;
    const float tiltCosine = cosf(tiltRadians), tiltSine = sinf(tiltRadians);
    float untilted[2 * Zoom::MAXIMUM_OUTLINE_POINTS];
    if (outline && outlineCount >= 3 && outlineCount <= Zoom::MAXIMUM_OUTLINE_POINTS)
    {
        for (int index = 0; index < outlineCount; ++index)
        {
            const float x = outline[2 * index], y = outline[2 * index + 1];
            untilted[2 * index] = x * tiltCosine + y * tiltSine;
            untilted[2 * index + 1] = -x * tiltSine + y * tiltCosine;
        }
        outline = untilted;
    }
    constexpr int MOST_CORNERS = 3 * Zoom::MAXIMUM_OUTLINE_POINTS;
    float corners[MOST_CORNERS][2];
    int cornerCount = 0;
    if (outline && outlineCount >= 3 && outlineCount <= Zoom::MAXIMUM_OUTLINE_POINTS)
    {
        float area = 0;
        for (int index = 0; index < outlineCount; ++index)
        {
            const float* p = &outline[2 * index];
            const float* q = &outline[2 * ((index + 1) % outlineCount)];
            area += p[0] * q[1] - q[0] * p[1];
        }
        for (int index = 0; index < outlineCount; ++index)
        {
            const int from = area >= 0 ? index : outlineCount - 1 - index;
            const int to = area >= 0 ? (index + 1) % outlineCount : (2 * outlineCount - 2 - index) % outlineCount;
            const float* p = &outline[2 * from];
            const float* q = &outline[2 * to];
            corners[cornerCount][0] = p[0];
            corners[cornerCount][1] = p[1];
            ++cornerCount;
            float crossings[2];
            int crossingCount = 0;
            for (int axis = 0; axis < 2; ++axis)
            {
                if (p[axis] * q[axis] < 0)
                {
                    crossings[crossingCount++] = p[axis] / (p[axis] - q[axis]);
                }
            }
            if (crossingCount == 2 && crossings[0] > crossings[1])
            {
                const float swap = crossings[0];
                crossings[0] = crossings[1];
                crossings[1] = swap;
            }
            for (int crossing = 0; crossing < crossingCount; ++crossing)
            {
                const float t = crossings[crossing];
                corners[cornerCount][0] = p[0] + t * (q[0] - p[0]);
                corners[cornerCount][1] = p[1] + t * (q[1] - p[1]);
                ++cornerCount;
            }
        }
    }
    if (cornerCount < 3)
    {
        constexpr float STEP_RADIANS = 6.2831853f / SCOPE_SEGMENT_COUNT;
        for (int segment = 0; segment <= SCOPE_SEGMENT_COUNT; ++segment)
        {
            edge[segment][0] = cosf(static_cast<float>(segment % SCOPE_SEGMENT_COUNT) * STEP_RADIANS);
            edge[segment][1] = sinf(static_cast<float>(segment % SCOPE_SEGMENT_COUNT) * STEP_RADIANS);
        }
        return;
    }
    float lengths[MOST_CORNERS];
    int pieces[MOST_CORNERS];
    int pieceCount = 0;
    for (int index = 0; index < cornerCount; ++index)
    {
        const float* p = corners[index];
        const float* q = corners[(index + 1) % cornerCount];
        lengths[index] = sqrtf((q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]));
        pieces[index] = 1;
        ++pieceCount;
    }
    while (pieceCount < SCOPE_SEGMENT_COUNT) // one more piece to the side with the longest pieces
    {
        int longest = 0;
        for (int index = 1; index < cornerCount; ++index)
        {
            if (lengths[index] * static_cast<float>(pieces[longest]) >
                lengths[longest] * static_cast<float>(pieces[index]))
            {
                longest = index;
            }
        }
        ++pieces[longest];
        ++pieceCount;
    }
    int point = 0;
    for (int index = 0; index < cornerCount && point < SCOPE_SEGMENT_COUNT; ++index)
    {
        const float* p = corners[index];
        const float* q = corners[(index + 1) % cornerCount];
        for (int piece = 0; piece < pieces[index] && point < SCOPE_SEGMENT_COUNT; ++piece, ++point)
        {
            const float t = static_cast<float>(piece) / static_cast<float>(pieces[index]);
            edge[point][0] = p[0] + t * (q[0] - p[0]);
            edge[point][1] = p[1] + t * (q[1] - p[1]);
        }
    }
    for (int index = 0; index < SCOPE_SEGMENT_COUNT; ++index)
    {
        const float x = edge[index][0], y = edge[index][1];
        edge[index][0] = x * tiltCosine - y * tiltSine;
        edge[index][1] = x * tiltSine + y * tiltCosine;
    }
    edge[SCOPE_SEGMENT_COUNT][0] = edge[0][0];
    edge[SCOPE_SEGMENT_COUNT][1] = edge[0][1];
}

} // namespace

bool CaptureScope(void* device)
{
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    IDirect3DSurface9* target = nullptr;
    if (FAILED(d3dDevice->lpVtbl->GetRenderTarget(d3dDevice, 0, &target)) || !target)
    {
        return false;
    }
    D3DSURFACE_DESC description{};
    target->lpVtbl->GetDesc(target, &description);
    const bool isCopied =
        EnsureScopeTexture(d3dDevice, description.Format) &&
        SUCCEEDED(d3dDevice->lpVtbl->StretchRect(d3dDevice, target, nullptr, _scopeSurface, nullptr, D3DTEXF_LINEAR));
    target->lpVtbl->Release(target);
    return isCopied;
}

void DrawScopeWindow(
    void* device,
    const float* eyeMatrix,
    float tanX,
    float tanY,
    const float* windowFrame,
    float radiusMeters,
    float distanceMeters,
    bool isLens,
    const float* emitters,
    int emitterCount,
    float emitterRadiusMeters,
    const float* outline,
    int outlineCount,
    float tiltDegrees)
{
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    if (!_scopeTexture || _scopeDevice != d3dDevice)
    {
        return;
    }
    static Vertex disc[3 * SCOPE_SEGMENT_COUNT];
    static ColourVertex ring[12 * SCOPE_SEGMENT_COUNT];
    static ColourVertex reticle[8];
    static Vertex reticleQuad[4];
    static ColourTexturedVertex overlay[9 * SCOPE_SEGMENT_COUNT];
    // Projected (lens) look, like the wrist projector (idren2023, 2026-10-13): a beam from an emitter on the gun, the
    // picture wreathed in light instead of a black ring, scan lines over it.
    static ColourVertex beam[2 * 12 * SCOPE_SEGMENT_COUNT], halo[6 * SCOPE_SEGMENT_COUNT];
    static ColourVertex holoLines[2 * (3 * SCOPE_SEGMENT_COUNT + 2 * 4 + SCOPE_SCAN_LINE_COUNT + 1)];
    int beamTriangleCount = 0, haloTriangleCount = 0, holoLineCount = 0;
    D3DCOLOR holoTint = 0xFFFFFFFF;
    unsigned overlayWidth = 0, overlayHeight = 0;
    IDirect3DTexture9* const overlayTexture =
        static_cast<IDirect3DTexture9*>(Zoom::ScopeTexture(overlayWidth, overlayHeight));
    unsigned reticleWidth = 0, reticleHeight = 0;
    IDirect3DTexture9* const reticleTexture =
        static_cast<IDirect3DTexture9*>(Zoom::ReticleTexture(reticleWidth, reticleHeight));
    D3DMATRIX view{}, projection{};
    {
        FpGuard guard;
        const float radius = radiusMeters;
        // A point of the window's plane, x and y in fractions of its radius, in the world.
        const auto point = [&](float x, float y, float* out)
        {
            const float z = -distanceMeters;
            for (int axis = 0; axis < 3; ++axis)
            {
                out[axis] = x * radius * windowFrame[axis] + y * radius * windowFrame[4 + axis] +
                            z * windowFrame[8 + axis] + windowFrame[12 + axis];
            }
        };
        const auto texturedAt = [&](float x, float y)
        {
            float position[3];
            point(x, y, position);
            return Vertex{position[0], position[1], position[2], 0.5f + 0.5f * x, 0.5f - 0.5f * y};
        };
        const auto colouredAt = [&](float x, float y, D3DCOLOR colour)
        {
            float position[3];
            point(x, y, position);
            return ColourVertex{position[0], position[1], position[2], colour};
        };
        // On a lens the design is shrunk to fit (its half width set by `hud scopesize`); the window keeps the old size.
        const float overlayHalfWide =
            isLens ? _scopeOverlayLensHalfWide : SCOPE_OVERLAY_HALF_SIZE * SCOPE_OVERLAY_SCREEN_ASPECT;
        // The overlay's texture coordinates: the quarter texture mirrored about both axes (centre at u = v = 1). Every
        // triangle lies within one quadrant (segment edges fall on the axes), so the mirroring interpolates exactly.
        // On a shaped screen the whole design (its 4:3 screen) is stretched over the outline's extent, along the
        // screen's tilted axes, so its frame follows the screen's edges (idren2023 2026-11-22).
        const bool isFitted = isLens && outline && outlineCount >= 3;
        const float tiltRadians = isFitted ? tiltDegrees * 0.017453293f : 0.0f;
        const float tiltCosine = cosf(tiltRadians), tiltSine = sinf(tiltRadians);
        float fittedHalfWide = 1.0f, fittedHalfTall = 1.0f;
        if (isFitted)
        {
            fittedHalfWide = fittedHalfTall = 0.0f;
            for (int corner = 0; corner < outlineCount; ++corner)
            {
                const float x = outline[2 * corner], y = outline[2 * corner + 1];
                fittedHalfWide = fmaxf(fittedHalfWide, fabsf(x * tiltCosine + y * tiltSine));
                fittedHalfTall = fmaxf(fittedHalfTall, fabsf(-x * tiltSine + y * tiltCosine));
            }
            fittedHalfWide *= SCOPE_OVERLAY_FIT_ALONG;
            fittedHalfTall *= SCOPE_OVERLAY_FIT_ACROSS;
        }
        const auto overlayAt = [&](float x, float y, D3DCOLOR colour)
        {
            float position[3];
            point(x, y, position);
            if (isFitted)
            {
                const float along = x * tiltCosine + y * tiltSine, across = -x * tiltSine + y * tiltCosine;
                return ColourTexturedVertex{
                    position[0],
                    position[1],
                    position[2],
                    colour,
                    1.0f - fabsf(along) / fittedHalfWide,
                    1.0f - fabsf(across) / fittedHalfTall};
            }
            return ColourTexturedVertex{
                position[0],
                position[1],
                position[2],
                colour,
                1.0f - fabsf(x) / overlayHalfWide,
                1.0f - fabsf(y) / (overlayHalfWide / SCOPE_OVERLAY_SCREEN_ASPECT)};
        };
        ColourTexturedVertex* overlayVertex = overlay;
        constexpr float STEP_RADIANS = 6.2831853f / SCOPE_SEGMENT_COUNT;
        constexpr D3DCOLOR BLACK = 0xFF000000, CLEAR = 0x00000000;
        // On a lens the ring is a thin rim and the overlay stops at the lens edge (its clear circle): nothing spills
        // onto the scope's body.
        const float radii[3] = {
            1.0f,
            isLens ? SCOPE_LENS_RIM : SCOPE_RING_SOLID,
            isLens ? SCOPE_LENS_RIM : SCOPE_RING_OUTER};
        const D3DCOLOR colours[3] = {BLACK, BLACK, CLEAR};
        Vertex* discVertex = disc;
        ColourVertex* ringVertex = ring;
        // the edge: round, or a lens's outline (a screen); the reticle fits inside the outline's nearest edge
        float edge[SCOPE_SEGMENT_COUNT + 1][2];
        ScopeEdge(isLens ? outline : nullptr, isLens ? outlineCount : 0, edge, isLens ? tiltDegrees : 0.0f);
        float nearestEdge = 1.0f;
        for (int segment = 0; segment < SCOPE_SEGMENT_COUNT; ++segment)
        {
            const float distance = sqrtf(edge[segment][0] * edge[segment][0] + edge[segment][1] * edge[segment][1]);
            nearestEdge = distance < nearestEdge ? distance : nearestEdge;
        }
        const float reticleHalfSize =
            SCOPE_RETICLE_HALF_SIZE < 0.9f * nearestEdge ? SCOPE_RETICLE_HALF_SIZE : 0.9f * nearestEdge;
        for (int segment = 0; segment < SCOPE_SEGMENT_COUNT; ++segment)
        {
            const float cosineStart = edge[segment][0], sineStart = edge[segment][1], cosineEnd = edge[segment + 1][0],
                        sineEnd = edge[segment + 1][1];
            *discVertex++ = texturedAt(0, 0);
            *discVertex++ = texturedAt(cosineStart, sineStart);
            *discVertex++ = texturedAt(cosineEnd, sineEnd);
            for (int band = 0; band < 2; ++band)
            {
                const float inner = radii[band], outer = radii[band + 1];
                *ringVertex++ = colouredAt(inner * cosineStart, inner * sineStart, colours[band]);
                *ringVertex++ = colouredAt(outer * cosineStart, outer * sineStart, colours[band + 1]);
                *ringVertex++ = colouredAt(inner * cosineEnd, inner * sineEnd, colours[band]);
                *ringVertex++ = colouredAt(inner * cosineEnd, inner * sineEnd, colours[band]);
                *ringVertex++ = colouredAt(outer * cosineStart, outer * sineStart, colours[band + 1]);
                *ringVertex++ = colouredAt(outer * cosineEnd, outer * sineEnd, colours[band + 1]);
            }
            constexpr D3DCOLOR WHITE = 0xFFFFFFFF, WHITE_CLEAR = 0x00FFFFFF;
            const float SOLID = isLens ? 1.0f : SCOPE_OVERLAY_SOLID, OUTER = isLens ? 1.0f : SCOPE_OVERLAY_OUTER;
            *overlayVertex++ = overlayAt(0, 0, WHITE);
            *overlayVertex++ = overlayAt(SOLID * cosineStart, SOLID * sineStart, WHITE);
            *overlayVertex++ = overlayAt(SOLID * cosineEnd, SOLID * sineEnd, WHITE);
            *overlayVertex++ = overlayAt(SOLID * cosineStart, SOLID * sineStart, WHITE);
            *overlayVertex++ = overlayAt(OUTER * cosineStart, OUTER * sineStart, WHITE_CLEAR);
            *overlayVertex++ = overlayAt(SOLID * cosineEnd, SOLID * sineEnd, WHITE);
            *overlayVertex++ = overlayAt(SOLID * cosineEnd, SOLID * sineEnd, WHITE);
            *overlayVertex++ = overlayAt(OUTER * cosineStart, OUTER * sineStart, WHITE_CLEAR);
            *overlayVertex++ = overlayAt(OUTER * cosineEnd, OUTER * sineEnd, WHITE_CLEAR);
        }
        // Reticle: the weapon's own texture, whole, centred, its aspect kept (as ReticuleDisplay draws it); without
        // one a thin dark cross with a gap in the middle.
        if (reticleTexture && reticleWidth > 0 && reticleHeight > 0)
        {
            const float longer = static_cast<float>(reticleWidth > reticleHeight ? reticleWidth : reticleHeight);
            const float halfX = reticleHalfSize * static_cast<float>(reticleWidth) / longer;
            const float halfY = reticleHalfSize * static_cast<float>(reticleHeight) / longer;
            const float corners[4][4] =
                {{-halfX, halfY, 0, 0}, {halfX, halfY, 1, 0}, {-halfX, -halfY, 0, 1}, {halfX, -halfY, 1, 1}};
            for (int index = 0; index < 4; ++index)
            {
                // turned with a tilted screen's design (idren2023 2026-11-22: level against the CIS launcher's tilted
                // frame it looked rotated)
                const float x = corners[index][0] * tiltCosine - corners[index][1] * tiltSine;
                const float y = corners[index][0] * tiltSine + corners[index][1] * tiltCosine;
                float position[3];
                point(x, y, position);
                reticleQuad[index] =
                    Vertex{position[0], position[1], position[2], corners[index][2], corners[index][3]};
            }
        }
        if (isLens && emitterCount > 0) // a projected picture; with no emitter it is on the glass (b176)
        {
            D3DCOLOR base = 0, pale = 0;
            HoloColours(&base, &pale);
            holoTint = 0xFF000000 | (((base >> 16 & 0xFF) / 5 + 204) << 16) | (((base >> 8 & 0xFF) / 5 + 204) << 8) |
                       ((base & 0xFF) / 5 + 204); // the picture a little toward the light's colour
            const float timeSeconds = static_cast<float>(GetTickCount() % 60000) * 0.001f;
            const float flicker = 0.90f + 0.06f * sinf(timeSeconds * 31.0f) + 0.04f * sinf(timeSeconds * 7.3f);
            const auto lit = [&](float x, float y, float z, float alpha, D3DCOLOR rgb)
            { // a point of the lens frame in metres (z toward the eye), lit with this alpha
                const D3DCOLOR alphaByte =
                    static_cast<D3DCOLOR>((alpha * flicker > 1 ? 1 : alpha * flicker) * 255) << 24;
                return ColourVertex{
                    x * windowFrame[0] + y * windowFrame[4] + z * windowFrame[8] + windowFrame[12],
                    x * windowFrame[1] + y * windowFrame[5] + z * windowFrame[9] + windowFrame[13],
                    x * windowFrame[2] + y * windowFrame[6] + z * windowFrame[10] + windowFrame[14],
                    alphaByte | rgb};
            };
            // The emitters (b155): given in the world, here in the frame (x, y, z toward the eye); without any, one on
            // the gun under the picture and ahead of it. Two (the bowcaster) share the light.
            float emitterXs[2] = {0, 0}, emitterYs[2] = {-radius * SCOPE_EMITTER_BELOW, 0},
                  emitterZs[2] = {-SCOPE_EMITTER_AHEAD_METERS, 0};
            const int sourceCount = emitters && emitterCount > 0 ? (emitterCount > 2 ? 2 : emitterCount) : 1;
            for (int source = 0; emitters && source < sourceCount; ++source)
            {
                const float offset[3] = {
                    emitters[3 * source] - windowFrame[12],
                    emitters[3 * source + 1] - windowFrame[13],
                    emitters[3 * source + 2] - windowFrame[14]};
                emitterXs[source] =
                    offset[0] * windowFrame[0] + offset[1] * windowFrame[1] + offset[2] * windowFrame[2];
                emitterYs[source] =
                    offset[0] * windowFrame[4] + offset[1] * windowFrame[5] + offset[2] * windowFrame[6];
                emitterZs[source] =
                    offset[0] * windowFrame[8] + offset[1] * windowFrame[9] + offset[2] * windowFrame[10];
            }
            const float share = 1.0f / static_cast<float>(sourceCount); // two beams add up to one
            ColourVertex* beamVertex = beam;
            ColourVertex* haloVertex = halo;
            ColourVertex* lineVertex = holoLines;
            for (int segment = 0; segment < SCOPE_SEGMENT_COUNT; ++segment)
            {
                const float angleStart = static_cast<float>(segment) * STEP_RADIANS,
                            angleEnd = angleStart + STEP_RADIANS;
                const float cosineStart = cosf(angleStart), sineStart = sinf(angleStart), cosineEnd = cosf(angleEnd),
                            sineEnd = sinf(angleEnd);
                for (int source = 0; source < sourceCount; ++source)
                {
                    const float ex = emitterXs[source], ey = emitterYs[source], ez = emitterZs[source];
                    // beam: from the emitter to the picture's edge, fading toward the picture
                    *beamVertex++ = lit(ex, ey, ez, 0.30f * share, pale);
                    *beamVertex++ = lit(radius * cosineStart, radius * sineStart, 0, 0.05f * share, base);
                    *beamVertex++ = lit(radius * cosineEnd, radius * sineEnd, 0, 0.05f * share, base);
                    if (emitterRadiusMeters > 0)
                    {
                        // a lens (b156, idren2023: the lenses should look on): the glass glows, bright in the middle,
                        // and its light spills past the rim
                        const float glass = emitterRadiusMeters, spill = emitterRadiusMeters * SCOPE_LENS_GLOW_OUTER;
                        *beamVertex++ = lit(ex, ey, ez, 0.95f, pale);
                        *beamVertex++ = lit(ex + glass * cosineStart, ey + glass * sineStart, ez, 0.55f, base);
                        *beamVertex++ = lit(ex + glass * cosineEnd, ey + glass * sineEnd, ez, 0.55f, base);
                        *beamVertex++ = lit(ex + glass * cosineStart, ey + glass * sineStart, ez, 0.55f, base);
                        *beamVertex++ = lit(ex + spill * cosineStart, ey + spill * sineStart, ez, 0, base);
                        *beamVertex++ = lit(ex + glass * cosineEnd, ey + glass * sineEnd, ez, 0.55f, base);
                        *beamVertex++ = lit(ex + glass * cosineEnd, ey + glass * sineEnd, ez, 0.55f, base);
                        *beamVertex++ = lit(ex + spill * cosineStart, ey + spill * sineStart, ez, 0, base);
                        *beamVertex++ = lit(ex + spill * cosineEnd, ey + spill * sineEnd, ez, 0, base);
                    }
                    else
                    {
                        // a point on the gun: a small bright dot facing the eye
                        *beamVertex++ = lit(ex, ey, ez, 0.95f, pale);
                        *beamVertex++ =
                            lit(ex + SCOPE_EMITTER_RADIUS_METERS * cosineStart,
                                ey + SCOPE_EMITTER_RADIUS_METERS * sineStart,
                                ez,
                                0,
                                pale);
                        *beamVertex++ =
                            lit(ex + SCOPE_EMITTER_RADIUS_METERS * cosineEnd,
                                ey + SCOPE_EMITTER_RADIUS_METERS * sineEnd,
                                ez,
                                0,
                                pale);
                    }
                }
                // halo: light from the picture's edge outward, fading
                const float outer = radius * SCOPE_HALO_OUTER;
                *haloVertex++ = lit(radius * cosineStart, radius * sineStart, 0, 0.55f, base);
                *haloVertex++ = lit(outer * cosineStart, outer * sineStart, 0, 0, base);
                *haloVertex++ = lit(radius * cosineEnd, radius * sineEnd, 0, 0.55f, base);
                *haloVertex++ = lit(radius * cosineEnd, radius * sineEnd, 0, 0.55f, base);
                *haloVertex++ = lit(outer * cosineStart, outer * sineStart, 0, 0, base);
                *haloVertex++ = lit(outer * cosineEnd, outer * sineEnd, 0, 0, base);
                // a bright edge line, and the emitter's ring
                *lineVertex++ = lit(radius * cosineStart, radius * sineStart, 0, 0.85f, pale);
                *lineVertex++ = lit(radius * cosineEnd, radius * sineEnd, 0, 0.85f, pale);
                const float ringRadius =
                    emitterRadiusMeters > 0 ? emitterRadiusMeters : SCOPE_EMITTER_RADIUS_METERS * 1.8f;
                for (int source = 0; source < sourceCount; ++source)
                {
                    const float ex = emitterXs[source], ey = emitterYs[source], ez = emitterZs[source];
                    *lineVertex++ = lit(ex + ringRadius * cosineStart, ey + ringRadius * sineStart, ez, 0.6f, pale);
                    *lineVertex++ = lit(ex + ringRadius * cosineEnd, ey + ringRadius * sineEnd, ez, 0.6f, pale);
                }
            }
            for (int source = 0; source < sourceCount; ++source) // four bright rays from each emitter to the edge
            {
                for (int edge = 0; edge < 4; ++edge)
                {
                    const float angle = 0.785398f + static_cast<float>(edge) * 1.5707963f;
                    *lineVertex++ = lit(emitterXs[source], emitterYs[source], emitterZs[source], 0.45f, pale);
                    *lineVertex++ = lit(radius * cosf(angle), radius * sinf(angle), 0, 0.08f, base);
                }
            }
            // Scan lines across the picture, drifting up, and one brighter line sweeping up every 3 s.
            const float spacing = 2.0f / static_cast<float>(SCOPE_SCAN_LINE_COUNT);
            const float drift = fmodf(timeSeconds, 1.0f) * spacing;
            for (int scanLine = 0; scanLine <= SCOPE_SCAN_LINE_COUNT; ++scanLine)
            {
                const bool isSweep = scanLine == SCOPE_SCAN_LINE_COUNT;
                const float y = isSweep ? -1.0f + fmodf(timeSeconds, 3.0f) / 3.0f * 2.0f
                                        : -1.0f + drift + static_cast<float>(scanLine) * spacing;
                if (y <= -1 || y >= 1)
                {
                    continue;
                }
                const float halfChord = sqrtf(1 - y * y);
                *lineVertex++ = lit(-halfChord * radius, y * radius, 0, isSweep ? 0.35f : 0.10f, isSweep ? pale : base);
                *lineVertex++ = lit(halfChord * radius, y * radius, 0, isSweep ? 0.35f : 0.10f, isSweep ? pale : base);
            }
            beamTriangleCount = static_cast<int>(beamVertex - beam) / 3;
            haloTriangleCount = static_cast<int>(haloVertex - halo) / 3;
            holoLineCount = static_cast<int>(lineVertex - holoLines) / 2;
        }
        constexpr D3DCOLOR LINE = 0xE0101010;
        constexpr float GAP = 0.08f, LENGTH = 0.95f;
        const float ends[8][2] =
            {{-LENGTH, 0}, {-GAP, 0}, {GAP, 0}, {LENGTH, 0}, {0, -LENGTH}, {0, -GAP}, {0, GAP}, {0, LENGTH}};
        for (int index = 0; index < 8; ++index)
        {
            reticle[index] = colouredAt(ends[index][0], ends[index][1], LINE);
        }
        // View = inverse of the eye's camera matrix (rigid). Projection: right-handed, symmetric, row vectors (as in
        // Composite).
        float* viewValues = &view.m[0][0];
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                viewValues[row * 4 + column] = eyeMatrix[column * 4 + row];
            }
        }
        for (int column = 0; column < 3; ++column)
        {
            viewValues[12 + column] =
                -(eyeMatrix[12] * viewValues[column] + eyeMatrix[13] * viewValues[4 + column] +
                  eyeMatrix[14] * viewValues[8 + column]);
        }
        viewValues[15] = 1;
        float* projectionValues = &projection.m[0][0];
        projectionValues[0] = 1.0f / tanX;
        projectionValues[5] = 1.0f / tanY;
        const float nearMeters = isLens ? SCOPE_LENS_NEAR_METERS : QUAD_NEAR_METERS;
        projectionValues[10] = QUAD_FAR_METERS / (nearMeters - QUAD_FAR_METERS);
        projectionValues[11] = -1.0f;
        projectionValues[14] = nearMeters * QUAD_FAR_METERS / (nearMeters - QUAD_FAR_METERS);
    }
    if (!_scopeSavedState && FAILED(d3dDevice->lpVtbl->CreateStateBlock(d3dDevice, D3DSBT_ALL, &_scopeSavedState)))
    {
        _scopeSavedState = nullptr;
        return;
    }
    if (FAILED(_scopeSavedState->lpVtbl->Capture(_scopeSavedState)))
    {
        return;
    }
    _isCompositing = true;
    static const D3DMATRIX IDENTITY = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    const bool isInScene = SUCCEEDED(d3dDevice->lpVtbl->BeginScene(d3dDevice));
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_WORLD, &IDENTITY);
    d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_VIEW, &view);
    d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_PROJECTION, &projection);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LIGHTING, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_FOGENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHATESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_STENCILENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SCISSORTESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_COLORWRITEENABLE, 0x7);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    const auto drawLight = [&](D3DPRIMITIVETYPE type, int count, const ColourVertex* vertices)
    { // plain colour, added to the picture
        if (count <= 0)
        {
            return;
        }
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_ONE);
        d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZ | D3DFVF_DIFFUSE);
        d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, nullptr);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, type, static_cast<UINT>(count), vertices, sizeof(ColourVertex));
    };
    drawLight(D3DPT_TRIANGLELIST, beamTriangleCount, beam); // behind the picture
    // The picture, opaque (on a lens tinted a little toward the hologram's colour).
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, FALSE);
    d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZ | D3DFVF_TEX1);
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(_scopeTexture));
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_TEXTUREFACTOR, holoTint);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_TEXCOORDINDEX, 0);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLELIST, SCOPE_SEGMENT_COUNT, disc, sizeof(Vertex));
    // Ring and reticle: plain colour, blended.
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, nullptr);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    if (isLens && emitterCount > 0)
    {
        drawLight(D3DPT_TRIANGLELIST, haloTriangleCount, halo); // light around the picture, not a black ring
        drawLight(D3DPT_LINELIST, holoLineCount, holoLines);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
    else
    {
        d3dDevice->lpVtbl
            ->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLELIST, 4 * SCOPE_SEGMENT_COUNT, ring, sizeof(ColourVertex));
    }
    if (overlayTexture)
    {
        // The game's scope overlay, white (as ScopeDisplay leaves it), its texture alpha faded by the vertices'.
        d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(overlayTexture));
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        // A shrunk design may end inside the lens: beyond the texture its edge goes on (the dark surround).
        d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d3dDevice->lpVtbl->DrawPrimitiveUP(
            d3dDevice,
            D3DPT_TRIANGLELIST,
            3 * SCOPE_SEGMENT_COUNT,
            overlay,
            sizeof(ColourTexturedVertex));
    }
    if (reticleTexture && reticleWidth > 0 && reticleHeight > 0)
    {
        d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZ | D3DFVF_TEX1);
        d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(reticleTexture));
        // In the game's reticle colours (as ReticuleDisplay tints it): red over an enemy, green over a friend.
        const int relation = Feedback::AimedRelation();
        // A homing launcher's lock fades it out (LockOn::ReticleOpacity).
        const D3DCOLOR alpha =
            static_cast<D3DCOLOR>(static_cast<float>(SCOPE_RETICLE_TINT >> 24) * LockOn::ReticleOpacity() + 0.5f);
        const D3DCOLOR tint = (alpha << 24) | (relation < 0   ? SCOPE_RETICLE_ENEMY
                                               : relation > 0 ? SCOPE_RETICLE_FRIEND
                                                              : SCOPE_RETICLE_TINT & 0xFFFFFF);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_TEXTUREFACTOR, tint);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
        d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
        d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLESTRIP, 2, reticleQuad, sizeof(Vertex));
    }
    else
    {
        d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_LINELIST, 4, reticle, sizeof(ColourVertex));
    }
    if (isInScene)
    {
        d3dDevice->lpVtbl->EndScene(d3dDevice);
    }
    _scopeSavedState->lpVtbl->Apply(_scopeSavedState);
    _isCompositing = false;
}

} // namespace Hud

} // namespace SwbfVr
