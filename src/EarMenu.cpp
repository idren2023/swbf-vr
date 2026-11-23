// EarMenu.cpp: the radial menu of the ear gesture (PLAN 32, idren2023 2026-10-13).
//
// While a hand at an ear holds its grip, the left stick picks one of four things instead of moving the player
// (Controls.cpp). This draws what those four are: a round hologram in the style of the wrist projector (Hud.cpp),
// lit from the ear the hand is at, with a label in each quarter and the quarter the stick points at lit up. It is
// placed in the head's frame (ahead, a little down and toward that ear), so it moves with the headset.
//
// Drawn by Hud::Composite into each eye image, inside its scene and its saved state block (every state touched here
// is put back by Composite). The labels are text drawn once with Windows GDI into a texture of our own (managed pool,
// so it survives a device Reset; made again if the device changes).
//
// Direct3D 9 is used through its C interface, as in Hud.cpp (d3dDevice->lpVtbl->Method(d3dDevice, ...)).

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable. Has to be a macro: d3d9.h reads
// it.
#define CINTERFACE

#include "EarMenu.h"

#include "FpGuard.h"
#include "Holster.h"
#include "Log.h"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace SwbfVr
{

namespace EarMenu
{

namespace
{

/// Where the menu sits in the head's frame (metres; x right, y up, z back): this far ahead, this far down, and this
/// far toward the ear it comes from. About 15 degrees to the side, so the middle of the view stays free. Moved out from
/// 42 cm with the same angular size (headset run 13: hard to read that close).
constexpr float AHEAD_METERS = 0.55f;
constexpr float DOWN_METERS = 0.05f;
constexpr float SIDEWAYS_METERS = 0.145f;
/// The ear the light comes from, in the head's frame (the gesture zone's centre, Gesture.cpp ZONES), toward the side.
constexpr float EAR_SIDEWAYS_METERS = 0.09f;
constexpr float EAR_DOWN_METERS = 0.02f;
constexpr float EAR_BACK_METERS = 0.02f;
/// The disc: its radius, the empty middle's radius, and how far the stick dot can move from the centre.
constexpr float RADIUS_METERS = 0.13f;
constexpr float INNER_RADIUS_METERS = 0.029f;
constexpr float DOT_TRAVEL_METERS = 0.021f;
constexpr float DOT_RADIUS_METERS = 0.0065f;
/// Labels: height of a texture row on the disc, and their distance from the centre (up / down; left / right labels
/// start this far out).
constexpr float LABEL_ROW_HEIGHT_METERS = 0.0195f;
constexpr float LABEL_UP_DOWN_METERS = 0.083f;
constexpr float LABEL_SIDE_START_METERS = 0.034f;
/// The stick points at a quarter once it is pushed this far (the flick that picks needs more, Controls.cpp).
constexpr float POINTED_STICK = 0.35f;
/// The menu fades in this long after the grip; a pick lights its quarter this long.
constexpr float FADE_IN_MILLISECONDS = 120.0f;
constexpr DWORD TAKEN_FLASH_MILLISECONDS = 350;
constexpr int SEGMENT_COUNT = 48; // of the disc and its rings (a multiple of 4, so quarters fall on segments)
constexpr int BEAM_LINE_COUNT = 12;
constexpr int SCAN_LINE_COUNT = 16;
constexpr int TICK_COUNT = 24;
/// The rim's look (b176): light spills out to this many radii; a second faint ring at this many; the rim is open this
/// far (radians) either side of the diagonals between the quarters.
constexpr float GLOW_OUTER = 1.12f;
constexpr float OUTER_RING = 1.06f;
constexpr float RIM_GAP_RADIANS = 0.06f;
/// The label texture: four rows (one per choice), text drawn with a font this many pixels high.
constexpr int LABEL_TEXTURE_WIDTH = 256;
constexpr int LABEL_TEXTURE_HEIGHT = 128;
constexpr int LABEL_ROW_PIXELS = 32;
constexpr int LABEL_FONT_PIXELS = 24;
constexpr float TWO_PI = 6.2831853f;
const char* const LABELS[] = {"RECENTER", "PLAYER LIST", "MAP", "PAUSE"}; // Choice order, from Recenter on

bool _isOpen = false;
float _side = -1.0f; // -1 left ear, 1 right ear
float _stickX = 0.0f, _stickY = 0.0f;
DWORD _openTick = 0;
Choice _taken = Choice::None;
DWORD _takenTick = 0;

IDirect3DTexture9* _labelTexture = nullptr;
IDirect3DDevice9* _labelDevice = nullptr;
bool _hasLabelFailed = false;
/// Each label's piece of the texture (u across), its width in texture pixels.
float _labelU0[4] = {}, _labelU1[4] = {}, _labelPixels[4] = {};

struct ColourVertex
{
    float x, y, z;
    D3DCOLOR colour;
};

struct ColourTexturedVertex
{
    float x, y, z;
    D3DCOLOR colour;
    float u, v;
};

/// The choice a stick position points at (the stronger axis wins), or None near the middle.
Choice PointedChoice(float x, float y)
{
    if (fabsf(x) < POINTED_STICK && fabsf(y) < POINTED_STICK)
    {
        return Choice::None;
    }
    if (fabsf(x) >= fabsf(y))
    {
        return x > 0 ? Choice::PlayerList : Choice::Pause;
    }
    return y > 0 ? Choice::Recenter : Choice::Map;
}

/// The direction of a choice's quarter on the disc, radians from "right", counter-clockwise.
float ChoiceRadians(Choice choice)
{
    switch (choice)
    {
    case Choice::Recenter:
        return TWO_PI * 0.25f;
    case Choice::Pause:
        return TWO_PI * 0.5f;
    case Choice::Map:
        return TWO_PI * 0.75f;
    default:
        return 0.0f;
    }
}

/// Writes the four labels with GDI into a bitmap and copies it into a texture: white, coverage in alpha.
bool MakeLabelTexture(IDirect3DDevice9* d3dDevice)
{
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = LABEL_TEXTURE_WIDTH;
    info.bmiHeader.biHeight = -LABEL_TEXTURE_HEIGHT; // negative: rows from the top
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC context = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = context ? CreateDIBSection(context, &info, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (!bitmap || !bits)
    {
        if (context)
        {
            DeleteDC(context);
        }
        return false;
    }
    memset(bits, 0, LABEL_TEXTURE_WIDTH * LABEL_TEXTURE_HEIGHT * 4);
    HGDIOBJ oldBitmap = SelectObject(context, bitmap);
    HFONT font = CreateFontA(
        -LABEL_FONT_PIXELS,
        0,
        0,
        0,
        FW_BOLD,
        FALSE,
        FALSE,
        FALSE,
        ANSI_CHARSET,
        OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        "Arial");
    HGDIOBJ oldFont = font ? SelectObject(context, font) : nullptr;
    SetBkMode(context, TRANSPARENT);
    SetTextColor(context, RGB(255, 255, 255));
    for (int label = 0; label < 4; ++label)
    {
        const int length = static_cast<int>(strlen(LABELS[label]));
        SIZE size = {};
        GetTextExtentPoint32A(context, LABELS[label], length, &size);
        const int left = (LABEL_TEXTURE_WIDTH - size.cx) / 2;
        TextOutA(context, left, label * LABEL_ROW_PIXELS + (LABEL_ROW_PIXELS - size.cy) / 2, LABELS[label], length);
        _labelPixels[label] = static_cast<float>(size.cx + 4);
        _labelU0[label] = static_cast<float>(left - 2) / LABEL_TEXTURE_WIDTH;
        _labelU1[label] = static_cast<float>(left + size.cx + 2) / LABEL_TEXTURE_WIDTH;
    }
    GdiFlush();
    bool isMade = false;
    if (SUCCEEDED(d3dDevice->lpVtbl->CreateTexture(
            d3dDevice,
            LABEL_TEXTURE_WIDTH,
            LABEL_TEXTURE_HEIGHT,
            1,
            0,
            D3DFMT_A8R8G8B8,
            D3DPOOL_MANAGED,
            &_labelTexture,
            nullptr)))
    {
        D3DLOCKED_RECT locked = {};
        if (SUCCEEDED(_labelTexture->lpVtbl->LockRect(_labelTexture, 0, &locked, nullptr, 0)))
        {
            const uint32_t* source = static_cast<const uint32_t*>(bits);
            for (int row = 0; row < LABEL_TEXTURE_HEIGHT; ++row)
            {
                uint32_t* target =
                    reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits) + row * locked.Pitch);
                for (int column = 0; column < LABEL_TEXTURE_WIDTH; ++column)
                {
                    // GDI's antialiased white text: its brightness is the coverage.
                    const uint32_t coverage = source[row * LABEL_TEXTURE_WIDTH + column] & 0xFF;
                    target[column] = (coverage << 24) | 0xFFFFFF;
                }
            }
            _labelTexture->lpVtbl->UnlockRect(_labelTexture, 0);
            isMade = true;
        }
        else
        {
            _labelTexture->lpVtbl->Release(_labelTexture);
            _labelTexture = nullptr;
        }
    }
    if (oldFont)
    {
        SelectObject(context, oldFont);
    }
    if (font)
    {
        DeleteObject(font);
    }
    SelectObject(context, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(context);
    return isMade;
}

} // namespace

void Update(bool isOpen, bool isAtRightEar, float stickX, float stickY, Choice taken)
{
    const DWORD now = GetTickCount();
    if (isOpen && !_isOpen)
    {
        _openTick = now;
    }
    _isOpen = isOpen;
    _side = isAtRightEar ? 1.0f : -1.0f;
    _stickX = stickX;
    _stickY = stickY;
    if (taken != Choice::None)
    {
        _taken = taken;
        _takenTick = now | 1;
    }
}

void Draw(void* device, const float* eyeMatrix, const float* headPosition)
{
    if (!_isOpen || !device)
    {
        return;
    }
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    if (_labelDevice != d3dDevice)
    {
        if (_labelTexture && _labelDevice)
        {
            _labelTexture->lpVtbl->Release(_labelTexture);
        }
        _labelTexture = nullptr;
        _labelDevice = d3dDevice;
        _hasLabelFailed = false;
    }
    if (!_labelTexture && !_hasLabelFailed)
    {
        _hasLabelFailed = !MakeLabelTexture(d3dDevice);
        Log::Info("earmenu", _hasLabelFailed ? "label texture could not be made: no labels" : "label texture made");
    }

    FpGuard guard; // RAII: a clean floating-point state for this block, the game's restored at the end
    const DWORD now = GetTickCount();
    float fade = static_cast<float>(now - _openTick) / FADE_IN_MILLISECONDS;
    fade = fade > 1 ? 1 : fade;
    // A slight flicker, as the wrist hologram's (b176: more like a hologram, headset run 13).
    const float timeSeconds = static_cast<float>(now % 60000) * 0.001f;
    fade *= 0.90f + 0.06f * sinf(timeSeconds * 31.0f) + 0.04f * sinf(timeSeconds * 7.3f);
    const Choice pointed = PointedChoice(_stickX, _stickY);
    const bool isFlashing = _takenTick && now - _takenTick < TAKEN_FLASH_MILLISECONDS;
    const Choice lit = isFlashing ? _taken : pointed;

    // The menu's plane in the head's frame: centre ahead, down and toward the ear, turned to face the eyes.
    const float centre[3] = {_side * SIDEWAYS_METERS, -DOWN_METERS, -AHEAD_METERS};
    const float centreLength = sqrtf(centre[0] * centre[0] + centre[1] * centre[1] + centre[2] * centre[2]);
    const float back[3] = {-centre[0] / centreLength, -centre[1] / centreLength, -centre[2] / centreLength};
    float right[3] = {back[2], 0, -back[0]}; // head up x back
    const float rightLength = sqrtf(right[0] * right[0] + right[2] * right[2]);
    right[0] /= rightLength;
    right[2] /= rightLength;
    const float up[3] = {
        back[1] * right[2] - back[2] * right[1],
        back[2] * right[0] - back[0] * right[2],
        back[0] * right[1] - back[1] * right[0]}; // back x right
    // A point of the head's frame in the world: the head's position plus its axes (the eye camera's rotation rows).
    const auto toWorld = [&](float x, float y, float z, float* out)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            out[axis] = headPosition[axis] + x * eyeMatrix[axis] + y * eyeMatrix[4 + axis] + z * eyeMatrix[8 + axis];
        }
    };
    // A point on the disc (x right, y up, metres) in the world.
    const auto onDisc = [&](float x, float y, float* out)
    {
        toWorld(
            centre[0] + x * right[0] + y * up[0],
            centre[1] + x * right[1] + y * up[1],
            centre[2] + x * right[2] + y * up[2],
            out);
    };
    // The holograms' colours (Hud.cpp HoloColours): blue, red for the CIS side.
    const bool isCis = Holster::IsCis();
    const D3DCOLOR base = isCis ? 0xFF3A24 : 0x2A7CFF, pale = isCis ? 0xFFB8A8 : 0xA8D8FF;
    const auto colour = [&](float alpha, D3DCOLOR rgb)
    {
        const float strength = alpha * fade;
        return (static_cast<D3DCOLOR>((strength > 1 ? 1 : strength) * 255) << 24) | rgb;
    };
    const auto discVertex = [&](float x, float y, float alpha, D3DCOLOR rgb)
    {
        float world[3];
        onDisc(x, y, world);
        return ColourVertex{world[0], world[1], world[2], colour(alpha, rgb)};
    };

    // Room for: per segment the pool, up to two lit triangles and two of the glow outside the rim; the beam's cone; the
    // dot's 12. Lines: per segment up to four ring pieces; the dividers, the ticks, the beam lines, two halves per scan
    // line and the sweep.
    static ColourVertex triangles[3 * (5 * SEGMENT_COUNT + BEAM_LINE_COUNT + 12)];
    static ColourVertex lines[2 * (4 * SEGMENT_COUNT + 4 + TICK_COUNT + BEAM_LINE_COUNT + 2 * SCAN_LINE_COUNT + 2)];
    ColourVertex* triangle = triangles;
    ColourVertex* line = lines;
    const float segmentRadians = TWO_PI / SEGMENT_COUNT;
    const float litRadians = ChoiceRadians(lit);
    float ear[3];
    toWorld(_side * EAR_SIDEWAYS_METERS, -EAR_DOWN_METERS, EAR_BACK_METERS, ear);
    for (int segment = 0; segment < SEGMENT_COUNT; ++segment)
    {
        const float start = static_cast<float>(segment) * segmentRadians,
                    end = static_cast<float>(segment + 1) * segmentRadians;
        const float cosStart = cosf(start), sinStart = sinf(start), cosEnd = cosf(end), sinEnd = sinf(end);
        // the pool of light
        *triangle++ = discVertex(0, 0, 0.20f, base);
        *triangle++ = discVertex(RADIUS_METERS * cosStart, RADIUS_METERS * sinStart, 0.06f, base);
        *triangle++ = discVertex(RADIUS_METERS * cosEnd, RADIUS_METERS * sinEnd, 0.06f, base);
        // the lit quarter: the segments within 45 degrees of its direction
        float offset = fmodf((start + end) * 0.5f - litRadians + TWO_PI * 1.5f, TWO_PI) - TWO_PI * 0.5f;
        if (lit != Choice::None && fabsf(offset) < TWO_PI / 8)
        {
            const float alpha = isFlashing ? 0.85f : 0.55f;
            *triangle++ = discVertex(INNER_RADIUS_METERS * cosStart, INNER_RADIUS_METERS * sinStart, alpha, pale);
            *triangle++ = discVertex(RADIUS_METERS * cosStart, RADIUS_METERS * sinStart, alpha * 0.4f, base);
            *triangle++ = discVertex(RADIUS_METERS * cosEnd, RADIUS_METERS * sinEnd, alpha * 0.4f, base);
            *triangle++ = discVertex(INNER_RADIUS_METERS * cosStart, INNER_RADIUS_METERS * sinStart, alpha, pale);
            *triangle++ = discVertex(RADIUS_METERS * cosEnd, RADIUS_METERS * sinEnd, alpha * 0.4f, base);
            *triangle++ = discVertex(INNER_RADIUS_METERS * cosEnd, INNER_RADIUS_METERS * sinEnd, alpha, pale);
        }
        // light spilling outward from the rim
        const float glowRadius = GLOW_OUTER * RADIUS_METERS;
        *triangle++ = discVertex(RADIUS_METERS * cosStart, RADIUS_METERS * sinStart, 0.18f, base);
        *triangle++ = discVertex(glowRadius * cosStart, glowRadius * sinStart, 0.0f, base);
        *triangle++ = discVertex(glowRadius * cosEnd, glowRadius * sinEnd, 0.0f, base);
        *triangle++ = discVertex(RADIUS_METERS * cosStart, RADIUS_METERS * sinStart, 0.18f, base);
        *triangle++ = discVertex(glowRadius * cosEnd, glowRadius * sinEnd, 0.0f, base);
        *triangle++ = discVertex(RADIUS_METERS * cosEnd, RADIUS_METERS * sinEnd, 0.18f, base);
        // the rim in four arcs, open at the diagonals between the quarters; the lit quarter's arc doubled and bright
        const float fromDiagonal = fabsf(fmodf((start + end) * 0.5f + TWO_PI, TWO_PI / 4) - TWO_PI / 8);
        if (fromDiagonal > RIM_GAP_RADIANS)
        {
            const bool isLitArc = lit != Choice::None && fabsf(offset) < TWO_PI / 8;
            const float rimAlpha = isLitArc ? 1.0f : 0.65f;
            *line++ = discVertex(RADIUS_METERS * cosStart, RADIUS_METERS * sinStart, rimAlpha, pale);
            *line++ = discVertex(RADIUS_METERS * cosEnd, RADIUS_METERS * sinEnd, rimAlpha, pale);
            if (isLitArc)
            {
                const float inside = RADIUS_METERS * 0.975f;
                *line++ = discVertex(inside * cosStart, inside * sinStart, 0.9f, pale);
                *line++ = discVertex(inside * cosEnd, inside * sinEnd, 0.9f, pale);
            }
        }
        // a faint second ring outside, and the inner ring
        const float outerRadius = OUTER_RING * RADIUS_METERS;
        *line++ = discVertex(outerRadius * cosStart, outerRadius * sinStart, 0.25f, pale);
        *line++ = discVertex(outerRadius * cosEnd, outerRadius * sinEnd, 0.25f, pale);
        *line++ = discVertex(INNER_RADIUS_METERS * cosStart, INNER_RADIUS_METERS * sinStart, 0.40f, pale);
        *line++ = discVertex(INNER_RADIUS_METERS * cosEnd, INNER_RADIUS_METERS * sinEnd, 0.40f, pale);
    }
    // the four dividers, faint and fading outward, on the diagonals
    for (int divider = 0; divider < 4; ++divider)
    {
        const float angle = TWO_PI / 8 + static_cast<float>(divider) * TWO_PI / 4;
        *line++ = discVertex(INNER_RADIUS_METERS * cosf(angle), INNER_RADIUS_METERS * sinf(angle), 0.18f, pale);
        *line++ = discVertex(RADIUS_METERS * cosf(angle), RADIUS_METERS * sinf(angle), 0.0f, pale);
    }
    // ticks on the outer ring, every 15 degrees
    for (int tick = 0; tick < TICK_COUNT; ++tick)
    {
        const float angle = static_cast<float>(tick) * TWO_PI / TICK_COUNT;
        const float from = OUTER_RING * RADIUS_METERS, to = (OUTER_RING + 0.035f) * RADIUS_METERS;
        *line++ = discVertex(from * cosf(angle), from * sinf(angle), 0.4f, pale);
        *line++ = discVertex(to * cosf(angle), to * sinf(angle), 0.4f, pale);
    }
    // the light from the ear: fine lines and a faint cone to the rim, brightest where they reach it
    for (int beam = 0; beam < BEAM_LINE_COUNT; ++beam)
    {
        const float start = static_cast<float>(beam) * TWO_PI / BEAM_LINE_COUNT,
                    end = static_cast<float>(beam + 1) * TWO_PI / BEAM_LINE_COUNT;
        *line++ = ColourVertex{ear[0], ear[1], ear[2], colour(0.0f, base)};
        *line++ = discVertex(RADIUS_METERS * cosf(start), RADIUS_METERS * sinf(start), 0.22f, pale);
        *triangle++ = ColourVertex{ear[0], ear[1], ear[2], colour(0.0f, base)};
        *triangle++ = discVertex(RADIUS_METERS * cosf(start), RADIUS_METERS * sinf(start), 0.05f, base);
        *triangle++ = discVertex(RADIUS_METERS * cosf(end), RADIUS_METERS * sinf(end), 0.05f, base);
    }
    // scan lines drifting up, as in the wrist hologram
    const float drift = fmodf(static_cast<float>(now % 60000) * 0.001f, 1.0f) * (2 * RADIUS_METERS / SCAN_LINE_COUNT);
    for (int scanLine = 0; scanLine < SCAN_LINE_COUNT; ++scanLine)
    {
        const float y = -RADIUS_METERS + drift + static_cast<float>(scanLine) * (2 * RADIUS_METERS / SCAN_LINE_COUNT);
        if (fabsf(y) >= RADIUS_METERS)
        {
            continue;
        }
        const float halfChord = sqrtf(RADIUS_METERS * RADIUS_METERS - y * y);
        *line++ = discVertex(-halfChord, y, 0.0f, base);
        *line++ = discVertex(0, y, 0.12f, pale);
        *line++ = discVertex(0, y, 0.12f, pale);
        *line++ = discVertex(halfChord, y, 0.0f, base);
    }
    { // one brighter line sweeping up every 3 s
        const float y = -RADIUS_METERS + fmodf(timeSeconds, 3.0f) / 3.0f * 2 * RADIUS_METERS;
        const float chordSquared = RADIUS_METERS * RADIUS_METERS - y * y;
        const float halfChord = sqrtf(chordSquared > 0 ? chordSquared : 0);
        *line++ = discVertex(-halfChord, y, 0.0f, pale);
        *line++ = discVertex(halfChord, y, 0.45f, pale);
    }
    // the stick: a bright dot that moves with it
    {
        const float dotX = _stickX * DOT_TRAVEL_METERS, dotY = _stickY * DOT_TRAVEL_METERS;
        for (int segment = 0; segment < 12; ++segment)
        {
            const float start = static_cast<float>(segment) * TWO_PI / 12,
                        end = static_cast<float>(segment + 1) * TWO_PI / 12;
            *triangle++ = discVertex(dotX, dotY, 0.95f, pale);
            *triangle++ =
                discVertex(dotX + DOT_RADIUS_METERS * cosf(start), dotY + DOT_RADIUS_METERS * sinf(start), 0.3f, pale);
            *triangle++ =
                discVertex(dotX + DOT_RADIUS_METERS * cosf(end), dotY + DOT_RADIUS_METERS * sinf(end), 0.3f, pale);
        }
    }

    // Light: plain colour, added to the picture (as the wrist hologram's glow).
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_ONE);
    d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, nullptr);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->DrawPrimitiveUP(
        d3dDevice,
        D3DPT_TRIANGLELIST,
        static_cast<UINT>((triangle - triangles) / 3),
        triangles,
        sizeof(ColourVertex));
    d3dDevice->lpVtbl->DrawPrimitiveUP(
        d3dDevice,
        D3DPT_LINELIST,
        static_cast<UINT>((line - lines) / 2),
        lines,
        sizeof(ColourVertex));

    if (!_labelTexture)
    {
        return;
    }
    // Labels: the texture's white text times the vertex colour, added like the light. The lit one pale and full,
    // the others softer.
    // Each label twice: a soft, slightly larger glow behind (pass 0), then the text.
    ColourTexturedVertex quads[2 * 6 * 4];
    ColourTexturedVertex* quad = quads;
    for (int index = 0; index < 8; ++index)
    {
        const int pass = index / 4, label = index % 4;
        const float glowScale = pass == 0 ? 1.15f : 1.0f;
        const Choice choice = static_cast<Choice>(label + 1);
        const float halfHeight = LABEL_ROW_HEIGHT_METERS * 0.5f,
                    halfWidth = LABEL_ROW_HEIGHT_METERS * _labelPixels[label] / LABEL_ROW_PIXELS * 0.5f;
        float centreX = 0, centreY = 0;
        switch (choice)
        {
        case Choice::Recenter:
            centreY = LABEL_UP_DOWN_METERS;
            break;
        case Choice::Map:
            centreY = -LABEL_UP_DOWN_METERS;
            break;
        case Choice::PlayerList:
            centreX = LABEL_SIDE_START_METERS + halfWidth;
            break;
        default:
            centreX = -(LABEL_SIDE_START_METERS + halfWidth);
            break;
        }
        const D3DCOLOR labelColour = pass == 0       ? colour(choice == lit ? 0.35f : 0.15f, base)
                                     : choice == lit ? colour(1.0f, pale)
                                                     : colour(0.50f, pale);
        const float v0 = static_cast<float>(label) / 4, v1 = static_cast<float>(label + 1) / 4;
        const float glowHalfWidth = halfWidth * glowScale, glowHalfHeight = halfHeight * glowScale;
        const float xs[4] =
            {centreX - glowHalfWidth, centreX + glowHalfWidth, centreX - glowHalfWidth, centreX + glowHalfWidth};
        const float ys[4] =
            {centreY + glowHalfHeight, centreY + glowHalfHeight, centreY - glowHalfHeight, centreY - glowHalfHeight};
        const float us[4] = {_labelU0[label], _labelU1[label], _labelU0[label], _labelU1[label]};
        const float vs[4] = {v0, v0, v1, v1};
        ColourTexturedVertex corners[4];
        for (int corner = 0; corner < 4; ++corner)
        {
            float world[3];
            onDisc(xs[corner], ys[corner], world);
            corners[corner] = {world[0], world[1], world[2], labelColour, us[corner], vs[corner]};
        }
        *quad++ = corners[0];
        *quad++ = corners[1];
        *quad++ = corners[2];
        *quad++ = corners[2];
        *quad++ = corners[1];
        *quad++ = corners[3];
    }
    d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(_labelTexture));
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLELIST, 16, quads, sizeof(ColourTexturedVertex));
}

void ProjectorPosition(const float* eyeMatrix, const float* headPosition, float* position)
{
    const float x = _side * EAR_SIDEWAYS_METERS, y = -EAR_DOWN_METERS, z = EAR_BACK_METERS;
    for (int axis = 0; axis < 3; ++axis)
    {
        position[axis] = headPosition[axis] + x * eyeMatrix[axis] + y * eyeMatrix[4 + axis] + z * eyeMatrix[8 + axis];
    }
}

} // namespace EarMenu

} // namespace SwbfVr
