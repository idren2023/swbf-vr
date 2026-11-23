// ControllerHolo.cpp: the controllers as red holograms in the view (see ControllerHolo.h).

// d3d9.h's interfaces as C structs with an explicit vtable, the form the rest of the mod uses (before any include).
#define CINTERFACE

#include "ControllerHolo.h"

#include "Aim.h"
#include "FpGuard.h"
#include "HudDraws.h"
#include "Log.h"
#include "Stereo.h"
#include "Vehicle.h"
#include "Xr.h"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace SwbfVr
{

namespace ControllerHolo
{

namespace
{

/// The model's parts: their file suffix (the right controller's names; the left's buttons are x and y) and what makes
/// each glow.
enum class Glow
{
    None,
    Grip,
    Trigger,
    Lower, // A / X
    Upper, // B / Y
    Stick
};

struct PartFile
{
    const char* rightName;
    const char* leftName;
    Glow glow;
};

const PartFile PARTS[] = {
    {"body", "body", Glow::None},
    {"grip", "grip", Glow::Grip},
    {"trigger", "trigger", Glow::Trigger},
    {"thumbstick", "thumbstick", Glow::Stick},
    {"button_a", "button_x", Glow::Lower},
    {"button_b", "button_y", Glow::Upper},
};
constexpr int PART_COUNT = sizeof(PARTS) / sizeof(PARTS[0]);

struct Vertex
{
    float x, y, z, nx, ny, nz;
};
constexpr DWORD VERTEX_FORMAT = D3DFVF_XYZ | D3DFVF_NORMAL;

/// Where each controller's screen sits, in the head's frame (metres: right, up, back) (idren2023 2026-10-27: low in the
/// middle, then out to the sides a little more), and the point of the model it is turned about (its middle).
constexpr float PLACE_RIGHT = 0.17f, PLACE_UP = -0.19f, PLACE_BACK = -0.45f;
/// Headset run 18 (idren2023: the droid's HUD hard to read, too close to the face): the whole layout drawn this much
/// farther from the eye (same angles), and its angles this much larger (ShellDesigns' droid displays alike).
constexpr float HUD_DEPTH_SCALE = 1.6f, HUD_GROWTH = 1.1f;
constexpr float MODEL_MIDDLE[3] = {-0.007f, -0.006f, 0.085f};
/// The hologram's look: a red body, its glowing parts (held, pressed) orange-white; how see-through it is.
constexpr float BODY_RED[3] = {1.0f, 0.22f, 0.12f}, GLOW_COLOR[3] = {1.0f, 0.78f, 0.62f};
constexpr float OPACITY = 1.0f;
/// A grip or the trigger counts as held / pulled beyond this.
constexpr float PRESSED = 0.5f;

std::vector<Vertex> _parts[2][PART_COUNT]; // [hand 0 right, 1 left][part]
bool _isLoaded = false, _hasTriedLoad = false;
bool _isForced = false;
IDirect3DStateBlock9* _savedState = nullptr;
IDirect3DDevice9* _stateBlockDevice = nullptr;
IDirect3DTexture9* _scanlines = nullptr; // the hologram's scan-line texture (1 x 16, made once per device)
IDirect3DTexture9* _fineScanlines = nullptr; // the viewscreen's (1 x 4: one dark row, so a line a few pixels apart
                                             // still filters cleanly; made once per device)

/// The viewscreen's scan-line texture: alpha 255 on one row, a little on its neighbours, none on the fourth.
IDirect3DTexture9* FineScanlines(IDirect3DDevice9* device)
{
    if (_fineScanlines)
    {
        return _fineScanlines;
    }
    if (FAILED(device->lpVtbl->CreateTexture(device, 1, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &_fineScanlines, nullptr)))
    {
        _fineScanlines = nullptr;
        return nullptr;
    }
    D3DLOCKED_RECT locked;
    if (FAILED(_fineScanlines->lpVtbl->LockRect(_fineScanlines, 0, &locked, nullptr, 0)))
    {
        return _fineScanlines;
    }
    const DWORD levels[4] = {255, 70, 0, 70};
    for (int row = 0; row < 4; ++row)
    {
        *reinterpret_cast<DWORD*>(static_cast<unsigned char*>(locked.pBits) + row * locked.Pitch) = levels[row] << 24;
    }
    _fineScanlines->lpVtbl->UnlockRect(_fineScanlines, 0);
    return _fineScanlines;
}

// The scan-line texture (1 x 16 texels: purely horizontal lines along v): a bright line, then a fading band down to a
// dim gap; its alpha thins the screen between the lines.
IDirect3DTexture9* Scanlines(IDirect3DDevice9* device)
{
    if (_scanlines)
    {
        return _scanlines;
    }
    if (FAILED(device->lpVtbl->CreateTexture(device, 1, 16, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &_scanlines, nullptr)))
    {
        _scanlines = nullptr;
        return nullptr;
    }
    D3DLOCKED_RECT locked;
    if (FAILED(_scanlines->lpVtbl->LockRect(_scanlines, 0, &locked, nullptr, 0)))
    {
        return _scanlines;
    }
    for (int row = 0; row < 16; ++row)
    {
        const int level = row < 3 ? 255 : row < 6 ? 200 : row < 10 ? 130 : 55;
        *reinterpret_cast<DWORD*>(static_cast<unsigned char*>(locked.pBits) + row * locked.Pitch) =
            static_cast<DWORD>(level) << 24 | static_cast<DWORD>(level) << 16 | static_cast<DWORD>(level) << 8 |
            static_cast<DWORD>(level);
    }
    _scanlines->lpVtbl->UnlockRect(_scanlines, 0);
    return _scanlines;
}

/// The screens: each controller drawn flat (orthographic, as the head sees it, turned live) into its own texture, the
/// texture shown on a panel in the view with horizontal scan lines scrolling down it (idren2023 2026-10-27: read as a
/// screen, not a model in the room); labels beside it, a line from a control to its label only while that control
/// faces the viewer.
constexpr int SCREEN_TEXELS = 256;
constexpr float SCREEN_HALF_METERS = 0.075f; // the panel's half size in the view (at PLACE_BACK)
constexpr float ORTHO_HALF_METERS = 0.095f;  // the model space the texture spans (half), round the model's middle
constexpr float SCANLINES_ON_SCREEN = 64.0f, SCANLINE_SCROLL_PER_SECOND = 0.35f; // lines a panel, panels a second

/// A label: its text, the part it points at (-1: none), and the part's outward direction in model space (the line is
/// drawn while that faces the viewer).
struct Label
{
    const char* text;
    int part;
    float facing[3];
};
// the parts' indices in PARTS: 1 grip, 2 trigger, 3 thumbstick, 4 lower button, 5 upper button
const Label LABELS[2][3] = {
    {{"TRIGGER: LASERS", 2, {0, -0.4f, -1}}, {"A: SECONDARY", 4, {0, 1, 0}}, {"GRIP: FLIGHT STICK", 1, {-1, 0, 0}}},
    {{"X: TAKE OFF / LAND", 4, {0, 1, 0}}, {"GRIP: THROTTLE", 1, {1, 0, 0}}, {nullptr, -1, {0, 0, 0}}},
};
constexpr int LABEL_TEXELS_WIDE = 256, LABEL_TEXELS_HIGH = 32;
/// The droid's viewscreen layer (idren2023 2026-10-27: as concept H, the whole picture red with fine scan lines; in front
/// of everything, the HUD included): over the whole eye image, the picture first multiplied by VIEWSCREEN_FILTER
/// (green and blue partly taken out), then washed with VIEWSCREEN_WASH (ARGB), then darkened along the scan lines by
/// VIEWSCREEN_LINE (ARGB, its alpha the lines' strength), VIEWSCREEN_LINES of them over the image's height (run499:
/// fine enough), scrolling down this many lines a second.
constexpr float VIEWSCREEN_LINES = 800.0f, VIEWSCREEN_SCROLL_LINES = 0.35f;
constexpr DWORD VIEWSCREEN_FILTER = 0xFFFFC8BC, VIEWSCREEN_WASH = 0x12C83C28, VIEWSCREEN_LINE = 0x30200000;
constexpr float LABEL_HALF_WIDE = 0.055f, LABEL_HALF_HIGH = 0.007f;

IDirect3DTexture9* _screens[2] = {};
IDirect3DSurface9* _screenDepth = nullptr;
IDirect3DTexture9* _labels[2][3] = {};
float _partMiddles[2][PART_COUNT][3];
IDirect3DTexture9* _eyes[2] = {};        // the eye gauges' shape: [0] its inside, [1] its outline (alpha)
IDirect3DTexture9* _gaugeLabels[2] = {}; // HULL, HEAT
IDirect3DTexture9* _targetLabel = nullptr; // TGT:
bool _wasTargetShown = false;     // the game showed a target last frame (logged when it changes)
int _targetGrabsLeft = 0;         // test: pictures still to take when a target shows (SetTargetGrabs)
DWORD _lastTargetGrabTick = 0;

/// The droid's eye gauges (idren2023 2026-10-27, concept H: the vulture's two long red eyes flank the reticle, the left
/// the hull's health, the right (mirrored, as the exterior's eyes face) the weapon's heat, each filled from its foot):
/// the outline traced from the eye in the droid's texture (texels of a 252 x 936 picture: the thin stalk with a
/// rounded top, the wedge at its foot narrowing to a point), drawn 4x larger than the texture and averaged down.
constexpr int EYE_OUTLINE[][2] = {{130, 14}, {136, 6}, {156, 6}, {163, 14}, {167, 482}, {176, 512}, {181, 872},
                                  {172, 893}, {155, 898}, {145, 886}, {52, 535}, {47, 505}, {58, 492}, {124, 486}};
constexpr int EYE_POINTS = sizeof(EYE_OUTLINE) / sizeof(EYE_OUTLINE[0]);
constexpr int EYE_WIDE = 252, EYE_HIGH = 936, EYE_TEXELS_WIDE = 64, EYE_TEXELS_HIGH = 256; // 4 picture texels a texel
/// In the head's frame at the panels' depth (metres): each eye's middle (x on the right; the left one at -x), its
/// height; the inside's colour empty and full (ARGB); the outline's.
constexpr float EYE_RIGHT = 0.082f, EYE_UP = 0.045f, EYE_HEIGHT = 0.17f;
constexpr DWORD EYE_EMPTY = 0x50FF3723, EYE_FULL = 0xE8FF3723, EYE_EDGE = 0xF0FF6E50;
/// The target's bar (ShellDesigns' droid: GROUP_TARGET 0.30 x 0.06 at 1 m ahead, 0.42 m down, the group 96 x 33 units
/// filling the height, its type icon at its right end) as seen at the panels' depth: the bar's middle height, its left
/// end, the icon's middle and the hexagon's radius round it (metres).
constexpr float TARGET_UP = -0.42f * 0.45f, TARGET_LEFT = -0.087f * 0.45f, TARGET_ICON_RIGHT = 0.068f * 0.45f;
constexpr float TARGET_HEX_RADIUS = 0.024f * 0.45f;

struct ScreenVertex
{
    float x, y, z;
    float u0, v0, u1, v1;
};
constexpr DWORD SCREEN_FORMAT = D3DFVF_XYZ | D3DFVF_TEX2;
struct LineVertex
{
    float x, y, z;
    DWORD color;
};
constexpr DWORD LINE_FORMAT = D3DFVF_XYZ | D3DFVF_DIFFUSE;
struct OverlayVertex
{
    float x, y, z, rhw;
    float u, v;
};
constexpr DWORD OVERLAY_FORMAT = D3DFVF_XYZRHW | D3DFVF_TEX1;
IDirect3DStateBlock9* _viewscreenState = nullptr; // DrawViewscreen's saved states
IDirect3DDevice9* _viewscreenStateDevice = nullptr;

// The SteamVR install (%LOCALAPPDATA%\openvr\openvrpaths.vrpath, its "runtime" list's first entry), or false.
bool SteamVrDirectory(char* out, size_t size)
{
    char path[MAX_PATH];
    const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", path, sizeof(path));
    if (length == 0 || length >= sizeof(path) - 40)
    {
        return false;
    }
    strcat_s(path, "\\openvr\\openvrpaths.vrpath");
    FILE* file = nullptr;
    if (fopen_s(&file, path, "rb") || !file)
    {
        return false;
    }
    char text[4096] = {};
    const size_t read = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[read] = 0;
    const char* runtime = strstr(text, "\"runtime\"");
    const char* open = runtime ? strchr(runtime + 9, '"') : nullptr;
    if (!open)
    {
        return false;
    }
    size_t used = 0;
    for (const char* at = open + 1; *at && *at != '"' && used + 1 < size; ++at)
    {
        if (at[0] == '\\' && at[1] == '\\')
        {
            ++at; // JSON's escaped backslash
        }
        out[used++] = *at;
    }
    out[used] = 0;
    return used > 0;
}

// One OBJ file's triangles (positions and normals; faces fanned).
bool LoadObj(const char* path, std::vector<Vertex>& out)
{
    FILE* file = nullptr;
    if (fopen_s(&file, path, "rb") || !file)
    {
        return false;
    }
    std::vector<float> positions, normals;
    char line[512];
    while (fgets(line, sizeof(line), file))
    {
        if (line[0] == 'v' && line[1] == ' ')
        {
            float x, y, z;
            if (sscanf_s(line + 2, "%f %f %f", &x, &y, &z) == 3)
            {
                positions.insert(positions.end(), {x, y, z});
            }
        }
        else if (line[0] == 'v' && line[1] == 'n')
        {
            float x, y, z;
            if (sscanf_s(line + 3, "%f %f %f", &x, &y, &z) == 3)
            {
                normals.insert(normals.end(), {x, y, z});
            }
        }
        else if (line[0] == 'f' && line[1] == ' ')
        {
            int indices[8][2], count = 0;
            for (char* token = strtok(line + 2, " \r\n"); token && count < 8; token = strtok(nullptr, " \r\n"))
            {
                const int position = atoi(token);
                const char* slash = strchr(token, '/');
                const char* second = slash ? strchr(slash + 1, '/') : nullptr;
                indices[count][0] = position - 1;
                indices[count][1] = second ? atoi(second + 1) - 1 : -1;
                ++count;
            }
            for (int k = 1; k + 1 < count; ++k)
            {
                const int corners[3] = {0, k, k + 1};
                for (int corner : corners)
                {
                    const int p = indices[corner][0], n = indices[corner][1];
                    if (p < 0 || 3 * p + 2 >= static_cast<int>(positions.size()))
                    {
                        continue;
                    }
                    Vertex vertex = {positions[3 * p], positions[3 * p + 1], positions[3 * p + 2], 0, 1, 0};
                    if (n >= 0 && 3 * n + 2 < static_cast<int>(normals.size()))
                    {
                        vertex.nx = normals[3 * n];
                        vertex.ny = normals[3 * n + 1];
                        vertex.nz = normals[3 * n + 2];
                    }
                    out.push_back(vertex);
                }
            }
        }
    }
    fclose(file);
    return !out.empty();
}

void Load()
{
    _hasTriedLoad = true;
    char directory[MAX_PATH];
    if (!SteamVrDirectory(directory, sizeof(directory)))
    {
        Log::Warning("holo", "SteamVR's install not found (openvrpaths.vrpath): no controller holograms");
        return;
    }
    int triangles = 0;
    for (int hand = 0; hand < 2; ++hand)
    {
        const char* side = hand == 0 ? "right" : "left";
        for (int part = 0; part < PART_COUNT; ++part)
        {
            char path[MAX_PATH * 2];
            sprintf_s(
                path,
                "%s\\resources\\rendermodels\\oculus_quest_plus_controller_%s\\oculus_quest_plus_controller_%s_%s.obj",
                directory,
                side,
                side,
                hand == 0 ? PARTS[part].rightName : PARTS[part].leftName);
            if (!LoadObj(path, _parts[hand][part]))
            {
                Log::Warning("holo", "model part not loaded: %s", path);
                if (part == 0)
                {
                    return;
                }
            }
            triangles += static_cast<int>(_parts[hand][part].size() / 3);
            float* middle = _partMiddles[hand][part];
            middle[0] = middle[1] = middle[2] = 0;
            for (const Vertex& vertex : _parts[hand][part])
            {
                middle[0] += vertex.x;
                middle[1] += vertex.y;
                middle[2] += vertex.z;
            }
            const float count = _parts[hand][part].empty() ? 1.0f : static_cast<float>(_parts[hand][part].size());
            for (int k = 0; k < 3; ++k)
            {
                middle[k] /= count;
            }
        }
    }
    _isLoaded = true;
    Log::Info("holo", "controller holograms: Touch Plus models from %s, %d triangles", directory, triangles);
}

bool IsGlowing(Glow glow, int hand, const Xr::ControllerInput& pad)
{
    if (!pad.isValid)
    {
        return false;
    }
    switch (glow)
    {
    case Glow::Grip:
        return pad.grip[hand] > PRESSED;
    case Glow::Trigger:
        return pad.trigger[hand] > PRESSED;
    case Glow::Lower:
        return pad.isLowerPressed[hand];
    case Glow::Upper:
        return pad.isUpperPressed[hand];
    case Glow::Stick:
        return pad.isStickPressed[hand] || fabsf(pad.stickX[hand]) > 0.3f || fabsf(pad.stickY[hand]) > 0.3f;
    default:
        return false;
    }
}

D3DMATERIAL9 Material(bool isGlowing)
{
    D3DMATERIAL9 material = {};
    const float* color = isGlowing ? GLOW_COLOR : BODY_RED;
    material.Diffuse = {color[0] * 0.55f, color[1] * 0.55f, color[2] * 0.55f, OPACITY};
    material.Ambient = {color[0] * 0.25f, color[1] * 0.25f, color[2] * 0.25f, OPACITY};
    const float emissive = isGlowing ? 0.85f : 0.30f;
    material.Emissive = {color[0] * emissive, color[1] * emissive, color[2] * emissive, OPACITY};
    material.Specular = {0.6f, 0.4f, 0.35f, 1};
    material.Power = 24;
    return material;
}

bool IsDroidSeat()
{
    const char* stock = Vehicle::CurrentStockName();
    return Vehicle::CurrentSeat() == Vehicle::Seat::Flyer && stock && !strcmp(stock, "cis_fly_droidfighter");
}

bool IsShown()
{
    if (_isForced)
    {
        return true;
    }
    const char* stock = Vehicle::CurrentStockName();
    return Vehicle::CurrentSeat() == Vehicle::Seat::Flyer && stock && !strcmp(stock, "cis_fly_droidfighter");
}

void ReleaseTargets()
{
    for (IDirect3DTexture9*& screen : _screens)
    {
        if (screen)
        {
            screen->lpVtbl->Release(screen);
            screen = nullptr;
        }
    }
    if (_screenDepth)
    {
        _screenDepth->lpVtbl->Release(_screenDepth);
        _screenDepth = nullptr;
    }
}

// A label's text as a texture (GDI: white text on black, its brightness the alpha), made once per device.
IDirect3DTexture9* LabelTexture(IDirect3DDevice9* device, const char* text)
{
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = LABEL_TEXELS_WIDE;
    info.bmiHeader.biHeight = -LABEL_TEXELS_HIGH;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap || !bits)
    {
        if (bitmap)
        {
            DeleteObject(bitmap);
        }
        if (dc)
        {
            DeleteDC(dc);
        }
        return nullptr;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    HFONT font = CreateFontA(26, 0, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Arial");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkColor(dc, RGB(0, 0, 0));
    SetTextColor(dc, RGB(255, 255, 255));
    RECT all = {0, 0, LABEL_TEXELS_WIDE, LABEL_TEXELS_HIGH};
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    TextOutA(dc, 2, 2, text, static_cast<int>(strlen(text)));
    GdiFlush();
    IDirect3DTexture9* texture = nullptr;
    if (SUCCEEDED(device->lpVtbl->CreateTexture(
            device, LABEL_TEXELS_WIDE, LABEL_TEXELS_HIGH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
    {
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(texture->lpVtbl->LockRect(texture, 0, &locked, nullptr, 0)))
        {
            for (int row = 0; row < LABEL_TEXELS_HIGH; ++row)
            {
                const DWORD* from = static_cast<const DWORD*>(bits) + row * LABEL_TEXELS_WIDE;
                DWORD* to = reinterpret_cast<DWORD*>(static_cast<unsigned char*>(locked.pBits) + row * locked.Pitch);
                for (int column = 0; column < LABEL_TEXELS_WIDE; ++column)
                {
                    const DWORD level = from[column] & 0xFF; // white text: any channel
                    to[column] = level << 24 | 0xFFFFFF;
                }
            }
            texture->lpVtbl->UnlockRect(texture, 0);
        }
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return texture;
}

// The eye gauge's shape as a texture: its inside, or its outline (7 picture texels wide), white with the shape in
// the alpha, drawn by GDI at 4x and averaged down (smooth edges). Made once per device.
IDirect3DTexture9* EyeTexture(IDirect3DDevice9* device, bool isOutline)
{
    constexpr int WIDE = EYE_TEXELS_WIDE * 4, HIGH = EYE_TEXELS_HIGH * 4;
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = WIDE;
    info.bmiHeader.biHeight = -HIGH;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap || !bits)
    {
        if (bitmap)
        {
            DeleteObject(bitmap);
        }
        if (dc)
        {
            DeleteDC(dc);
        }
        return nullptr;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    RECT all = {0, 0, WIDE, HIGH};
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    POINT points[EYE_POINTS];
    for (int k = 0; k < EYE_POINTS; ++k)
    {
        points[k] = {EYE_OUTLINE[k][0], EYE_OUTLINE[k][1]};
    }
    HPEN pen = CreatePen(PS_SOLID, 7, RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(dc, isOutline ? static_cast<HGDIOBJ>(pen) : GetStockObject(NULL_PEN));
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(isOutline ? NULL_BRUSH : WHITE_BRUSH));
    Polygon(dc, points, EYE_POINTS);
    GdiFlush();
    IDirect3DTexture9* texture = nullptr;
    if (SUCCEEDED(device->lpVtbl->CreateTexture(
            device, EYE_TEXELS_WIDE, EYE_TEXELS_HIGH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
    {
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(texture->lpVtbl->LockRect(texture, 0, &locked, nullptr, 0)))
        {
            for (int row = 0; row < EYE_TEXELS_HIGH; ++row)
            {
                DWORD* to = reinterpret_cast<DWORD*>(static_cast<unsigned char*>(locked.pBits) + row * locked.Pitch);
                for (int column = 0; column < EYE_TEXELS_WIDE; ++column)
                {
                    DWORD sum = 0;
                    for (int y = 0; y < 4; ++y)
                    {
                        const DWORD* from = static_cast<const DWORD*>(bits) + (row * 4 + y) * WIDE + column * 4;
                        sum += (from[0] & 0xFF) + (from[1] & 0xFF) + (from[2] & 0xFF) + (from[3] & 0xFF);
                    }
                    to[column] = (sum / 16) << 24 | 0xFFFFFF;
                }
            }
            texture->lpVtbl->UnlockRect(texture, 0);
        }
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBitmap);
    DeleteObject(pen);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return texture;
}

// The fixed-function states for the model's draw into a screen texture (lit, depth-tested against itself).
void ModelStates(IDirect3DDevice9* device)
{
    device->lpVtbl->SetVertexShader(device, nullptr);
    device->lpVtbl->SetPixelShader(device, nullptr);
    device->lpVtbl->SetFVF(device, VERTEX_FORMAT);
    device->lpVtbl->SetTexture(device, 0, nullptr);
    device->lpVtbl->SetTexture(device, 1, nullptr);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_TEXCOORDINDEX, 0);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    device->lpVtbl->SetTextureStageState(device, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    device->lpVtbl->SetRenderState(device, D3DRS_LIGHTING, TRUE);
    device->lpVtbl->SetRenderState(device, D3DRS_AMBIENT, 0xFF404040);
    device->lpVtbl->SetRenderState(device, D3DRS_SPECULARENABLE, TRUE);
    device->lpVtbl->SetRenderState(device, D3DRS_NORMALIZENORMALS, TRUE);
    device->lpVtbl->SetRenderState(device, D3DRS_COLORVERTEX, FALSE);
    device->lpVtbl->SetRenderState(device, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    device->lpVtbl->SetRenderState(device, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    device->lpVtbl->SetRenderState(device, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    device->lpVtbl->SetRenderState(device, D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
    device->lpVtbl->SetRenderState(device, D3DRS_FOGENABLE, FALSE);
    device->lpVtbl->SetRenderState(device, D3DRS_ZENABLE, D3DZB_TRUE);
    device->lpVtbl->SetRenderState(device, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    device->lpVtbl->SetRenderState(device, D3DRS_ZWRITEENABLE, TRUE);
    device->lpVtbl->SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
    device->lpVtbl->SetRenderState(device, D3DRS_ALPHABLENDENABLE, FALSE);
    device->lpVtbl->SetRenderState(device, D3DRS_ALPHATESTENABLE, FALSE);
    device->lpVtbl->SetRenderState(device, D3DRS_STENCILENABLE, FALSE);
    device->lpVtbl->SetRenderState(device, D3DRS_SCISSORTESTENABLE, FALSE);
    device->lpVtbl->SetRenderState(device, D3DRS_COLORWRITEENABLE, 0xF);
    for (int light = 1; light < 8; ++light)
    {
        device->lpVtbl->LightEnable(device, light, FALSE);
    }
    D3DLIGHT9 key = {};
    key.Type = D3DLIGHT_DIRECTIONAL;
    key.Diffuse = {1, 1, 1, 1};
    key.Specular = {1, 1, 1, 1};
    key.Direction = {0.25f, -0.7f, 0.65f}; // from above, left and in front of the screen (view space: z into it)
    device->lpVtbl->SetLight(device, 0, &key);
    device->lpVtbl->LightEnable(device, 0, TRUE);
}

// The controller's model space -> the screen's view space (x right, y up, z into the screen, as the head sees it):
// the controller's world axes in the head's frame, the model's middle at the origin. FpGuard held.
void ModelToView(const float* pose, const float* head, float* out)
{
    float axes[3][3];
    for (int row = 0; row < 3; ++row)
    {
        const float* axis = pose + row * 4;
        const float length = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        for (int k = 0; k < 3; ++k)
        {
            axes[row][k] = length > 1e-4f ? axis[k] / length : (row == k ? 1.0f : 0.0f);
        }
    }
    for (int row = 0; row < 3; ++row)
    {
        // the world axis in the head's right, up and (negated) back
        out[row * 4 + 0] = axes[row][0] * head[0] + axes[row][1] * head[1] + axes[row][2] * head[2];
        out[row * 4 + 1] = axes[row][0] * head[4] + axes[row][1] * head[5] + axes[row][2] * head[6];
        out[row * 4 + 2] = -(axes[row][0] * head[8] + axes[row][1] * head[9] + axes[row][2] * head[10]);
        out[row * 4 + 3] = 0;
    }
    for (int k = 0; k < 3; ++k)
    {
        out[12 + k] = -(MODEL_MIDDLE[0] * out[k] + MODEL_MIDDLE[1] * out[4 + k] + MODEL_MIDDLE[2] * out[8 + k]);
    }
    out[15] = 1;
}

// Draws one controller flat into its screen texture. The caller restores the render target. FpGuard held.
bool DrawScreen(IDirect3DDevice9* device, int hand, const float* modelToView, const Xr::ControllerInput& pad)
{
    if (!_screens[hand] &&
        FAILED(device->lpVtbl->CreateTexture(
            device, SCREEN_TEXELS, SCREEN_TEXELS, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
            &_screens[hand], nullptr)))
    {
        _screens[hand] = nullptr;
        return false;
    }
    if (!_screenDepth &&
        FAILED(device->lpVtbl->CreateDepthStencilSurface(
            device, SCREEN_TEXELS, SCREEN_TEXELS, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &_screenDepth, nullptr)))
    {
        _screenDepth = nullptr;
        return false;
    }
    IDirect3DSurface9* surface = nullptr;
    if (FAILED(_screens[hand]->lpVtbl->GetSurfaceLevel(_screens[hand], 0, &surface)))
    {
        return false;
    }
    device->lpVtbl->SetRenderTarget(device, 0, surface);
    device->lpVtbl->SetDepthStencilSurface(device, _screenDepth);
    surface->lpVtbl->Release(surface);
    const D3DVIEWPORT9 viewport = {0, 0, SCREEN_TEXELS, SCREEN_TEXELS, 0, 1};
    device->lpVtbl->SetViewport(device, &viewport);
    device->lpVtbl->Clear(device, 0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0x00000000, 1.0f, 0);
    ModelStates(device);
    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1;
    device->lpVtbl->SetTransform(device, D3DTS_VIEW, &identity);
    // orthographic: x and y of +-ORTHO_HALF to +-1, z from -0.2 to 0.2 to 0..1
    D3DMATRIX ortho = {};
    ortho._11 = ortho._22 = 1.0f / ORTHO_HALF_METERS;
    ortho._33 = 1.0f / 0.4f;
    ortho._43 = 0.2f / 0.4f;
    ortho._44 = 1;
    device->lpVtbl->SetTransform(device, D3DTS_PROJECTION, &ortho);
    D3DMATRIX world;
    memcpy(&world, modelToView, sizeof(world));
    device->lpVtbl->SetTransform(device, D3DTS_WORLD, &world);
    for (int part = 0; part < PART_COUNT; ++part)
    {
        const std::vector<Vertex>& vertices = _parts[hand][part];
        if (vertices.empty())
        {
            continue;
        }
        const D3DMATERIAL9 material = Material(IsGlowing(PARTS[part].glow, hand, pad));
        device->lpVtbl->SetMaterial(device, &material);
        device->lpVtbl->DrawPrimitiveUP(
            device, D3DPT_TRIANGLELIST, static_cast<UINT>(vertices.size() / 3), vertices.data(), sizeof(Vertex));
    }
    return true;
}

} // namespace

void Draw(void* device)
{
    float head[16];
    if (!device || !Xr::IsRunning() || Stereo::IsScopePass() || !IsShown() || !Stereo::GetSmoothedHeadWorld(head))
    {
        return;
    }
    if (!_hasTriedLoad)
    {
        Load();
    }
    if (!_isLoaded)
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
    Xr::ControllerInput pad{};
    Xr::GetControllerInput(&pad);
    // the eye's own render target and depth, given back after the screens are drawn
    IDirect3DSurface9* eyeTarget = nullptr;
    IDirect3DSurface9* eyeDepth = nullptr;
    d3dDevice->lpVtbl->GetRenderTarget(d3dDevice, 0, &eyeTarget);
    d3dDevice->lpVtbl->GetDepthStencilSurface(d3dDevice, &eyeDepth);
    FpGuard fp;
    float modelToView[2][16];
    bool hasScreen[2] = {};
    for (int hand = 0; hand < 2; ++hand)
    {
        float pose[16];
        if (!Aim::GetHandWorld(hand, pose))
        {
            continue;
        }
        ModelToView(pose, head, modelToView[hand]);
        hasScreen[hand] = DrawScreen(d3dDevice, hand, modelToView[hand], pad);
    }
    if (eyeTarget)
    {
        d3dDevice->lpVtbl->SetRenderTarget(d3dDevice, 0, eyeTarget);
        eyeTarget->lpVtbl->Release(eyeTarget);
    }
    d3dDevice->lpVtbl->SetDepthStencilSurface(d3dDevice, eyeDepth);
    if (eyeDepth)
    {
        eyeDepth->lpVtbl->Release(eyeDepth);
    }
    _savedState->lpVtbl->Apply(_savedState); // the eye's viewport, transforms and states
    _savedState->lpVtbl->Capture(_savedState);
    // The panels in the eye's view: each screen on a square facing the head, the scan lines (stage 1, their own texture
    // coordinates scrolling down) thinning its alpha; then the labels and their lines.
    IDirect3DTexture9* const scanlines = Scanlines(d3dDevice);
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LIGHTING, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_FOGENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZFUNC, D3DCMP_ALWAYS);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHATESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_STENCILENABLE, FALSE);
    D3DMATRIX identity = {};
    identity._11 = identity._22 = identity._33 = identity._44 = 1;
    d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_WORLD, &identity);
    for (int stage = 0; stage < 2; ++stage)
    {
        d3dDevice->lpVtbl->SetSamplerState(d3dDevice, stage, D3DSAMP_ADDRESSU, stage ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
        d3dDevice->lpVtbl->SetSamplerState(d3dDevice, stage, D3DSAMP_ADDRESSV, stage ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP);
        d3dDevice->lpVtbl->SetSamplerState(d3dDevice, stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d3dDevice->lpVtbl->SetSamplerState(d3dDevice, stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d3dDevice->lpVtbl->SetSamplerState(d3dDevice, stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, stage, D3DTSS_TEXCOORDINDEX, stage);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }
    const float scroll = static_cast<float>(GetTickCount() % 100000) * 0.001f * SCANLINE_SCROLL_PER_SECOND;
    const auto at = [&](float right, float up, float* out) // a point in the head's frame at the panels' depth
    {
        for (int k = 0; k < 3; ++k)
        {
            out[k] = head[12 + k] + (right * head[k] + up * head[4 + k]) * HUD_DEPTH_SCALE * HUD_GROWTH +
                     PLACE_BACK * HUD_DEPTH_SCALE * head[8 + k];
        }
    };
    // The eye gauges either side of the reticle: the hull's health (left) and the weapon's coolness (right, mirrored), each
    // the eye's inside faint, the part up to its value bright, its outline; a label under each.
    if (IsDroidSeat())
    {
        for (int k = 0; k < 2; ++k)
        {
            if (!_eyes[k])
            {
                _eyes[k] = EyeTexture(d3dDevice, k == 1);
            }
            if (!_gaugeLabels[k])
            {
                _gaugeLabels[k] = LabelTexture(d3dDevice, k == 0 ? "HULL" : "HEAT");
            }
        }
        // the heat eye full while cool, empty when overheated (idren2023 2026-10-27)
        const float heat = Vehicle::WeaponHeatFraction();
        const float values[2] = {Vehicle::HullHealthFraction(), heat < 0 ? -1 : 1 - heat};
        const float halfWide = EYE_HEIGHT * EYE_WIDE / EYE_HIGH * 0.5f, halfHigh = EYE_HEIGHT * 0.5f;
        const float uRight = static_cast<float>(EYE_WIDE) / (EYE_TEXELS_WIDE * 4);
        const float vBottom = static_cast<float>(EYE_HIGH) / (EYE_TEXELS_HIGH * 4);
        d3dDevice->lpVtbl->SetFVF(d3dDevice, SCREEN_FORMAT);
        d3dDevice->lpVtbl->SetTexture(d3dDevice, 1, nullptr);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        const float corners[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
        for (int gauge = 0; gauge < 2; ++gauge)
        {
            const float cx = gauge == 0 ? -EYE_RIGHT : EYE_RIGHT, bottom = EYE_UP - halfHigh;
            const bool isMirrored = gauge == 1;
            const float value = values[gauge] < 0 ? 0 : values[gauge] > 1 ? 1 : values[gauge];
            // a part of the eye from its foot up to a fraction of its height, in a colour, from one texture
            const auto drawPart = [&](IDirect3DTexture9* texture, float upTo, DWORD color)
            {
                if (!texture || upTo <= 0)
                {
                    return;
                }
                d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(texture));
                d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_TEXTUREFACTOR, color);
                const float top = bottom + upTo * EYE_HEIGHT, vTop = vBottom * (1 - upTo);
                ScreenVertex quad[4];
                for (int k = 0; k < 4; ++k)
                {
                    float p[3];
                    at(cx + corners[k][0] * halfWide, corners[k][1] > 0 ? top : bottom, p);
                    const float u = ((corners[k][0] < 0) != isMirrored) ? 0 : uRight;
                    quad[k] = {p[0], p[1], p[2], u, corners[k][1] > 0 ? vTop : vBottom, 0, 0};
                }
                const ScreenVertex fan[6] = {quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]};
                d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLELIST, 2, fan, sizeof(ScreenVertex));
            };
            drawPart(_eyes[0], 1, EYE_EMPTY);
            drawPart(_eyes[0], value, EYE_FULL);
            drawPart(_eyes[1], 1, EYE_EDGE);
            // the label under the eye, its text starting a little left of the eye's middle
            if (_gaugeLabels[gauge])
            {
                d3dDevice->lpVtbl->SetTexture(
                    d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(_gaugeLabels[gauge]));
                d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_TEXTUREFACTOR, 0xFFFF4A30);
                const float lx = cx - 0.012f + LABEL_HALF_WIDE, ly = bottom - 0.014f;
                ScreenVertex text[4];
                for (int k = 0; k < 4; ++k)
                {
                    float p[3];
                    at(lx + corners[k][0] * LABEL_HALF_WIDE, ly + corners[k][1] * LABEL_HALF_HIGH, p);
                    text[k] = {p[0], p[1], p[2], (corners[k][0] + 1) * 0.5f, (1 - corners[k][1]) * 0.5f, 0, 0};
                }
                const ScreenVertex textFan[6] = {text[0], text[1], text[2], text[0], text[2], text[3]};
                d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLELIST, 2, textFan, sizeof(ScreenVertex));
            }
        }
    }
    // The target's tag left of its bar and a hexagon round its type icon, while the game shows a target.
    if (IsDroidSeat() && HudDraws::IsTaken(HudDraws::Group::Target))
    {
        HudDraws::GroupDrawing target{};
        const bool isTargetShown = HudDraws::GetGroup(HudDraws::Group::Target, target) && target.batchCount > 0;
        if (isTargetShown != _wasTargetShown)
        {
            _wasTargetShown = isTargetShown;
            Log::Info("holo", "target %s", isTargetShown ? "shown" : "gone");
        }
        if (isTargetShown && _targetGrabsLeft > 0 && GetTickCount() - _lastTargetGrabTick > 2000)
        {
            --_targetGrabsLeft;
            _lastTargetGrabTick = GetTickCount();
            Stereo::GrabImages(1, 2);
        }
        if (isTargetShown)
        {
            if (!_targetLabel)
            {
                _targetLabel = LabelTexture(d3dDevice, "TGT:");
            }
            const float corners[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
            if (_targetLabel)
            {
                d3dDevice->lpVtbl->SetFVF(d3dDevice, SCREEN_FORMAT);
                d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(_targetLabel));
                d3dDevice->lpVtbl->SetTexture(d3dDevice, 1, nullptr);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
                d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_TEXTUREFACTOR, 0xFFFF4A30);
                // the text (about a quarter of the label's width) ending just left of the bar
                const float lx = TARGET_LEFT - 0.008f - LABEL_HALF_WIDE * 0.5f + LABEL_HALF_WIDE;
                ScreenVertex text[4];
                for (int k = 0; k < 4; ++k)
                {
                    float p[3];
                    at(lx - LABEL_HALF_WIDE + corners[k][0] * LABEL_HALF_WIDE, TARGET_UP + corners[k][1] * LABEL_HALF_HIGH,
                       p);
                    text[k] = {p[0], p[1], p[2], (corners[k][0] + 1) * 0.5f, (1 - corners[k][1]) * 0.5f, 0, 0};
                }
                const ScreenVertex textFan[6] = {text[0], text[1], text[2], text[0], text[2], text[3]};
                d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLELIST, 2, textFan, sizeof(ScreenVertex));
            }
            LineVertex hexagon[7];
            for (int k = 0; k <= 6; ++k)
            {
                const float angle = 3.14159265f / 3 * k;
                float p[3];
                at(TARGET_ICON_RIGHT + TARGET_HEX_RADIUS * cosf(angle), TARGET_UP + TARGET_HEX_RADIUS * sinf(angle), p);
                hexagon[k] = {p[0], p[1], p[2], 0xF0FF4A30};
            }
            d3dDevice->lpVtbl->SetFVF(d3dDevice, LINE_FORMAT);
            d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, nullptr);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_LINESTRIP, 6, hexagon, sizeof(LineVertex));
        }
    }
    for (int hand = 0; hand < 2; ++hand)
    {
        if (!hasScreen[hand])
        {
            continue;
        }
        const float side = hand == 0 ? 1.0f : -1.0f;
        const float cx = side * PLACE_RIGHT, cy = PLACE_UP, h = SCREEN_HALF_METERS;
        // the screen
        d3dDevice->lpVtbl->SetFVF(d3dDevice, SCREEN_FORMAT);
        d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(_screens[hand]));
        d3dDevice->lpVtbl->SetTexture(d3dDevice, 1, reinterpret_cast<IDirect3DBaseTexture9*>(scanlines));
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLORARG2, D3DTA_CURRENT);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, scanlines ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAARG2, D3DTA_CURRENT);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 2, D3DTSS_COLOROP, D3DTOP_DISABLE);
        ScreenVertex quad[4];
        const float corners[4][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, -1}};
        for (int k = 0; k < 4; ++k)
        {
            float p[3];
            at(cx + corners[k][0] * h, cy + corners[k][1] * h, p);
            const float u = (corners[k][0] + 1) * 0.5f, v = (1 - corners[k][1]) * 0.5f;
            quad[k] = {p[0], p[1], p[2], u, v, 0.5f, v * SCANLINES_ON_SCREEN / 16.0f - scroll};
        }
        const ScreenVertex fan[6] = {quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]};
        d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLELIST, 2, fan, sizeof(ScreenVertex));
        // the labels beside the screen (outward), their lines from the controls that face the viewer
        for (int label = 0; label < 3; ++label)
        {
            const Label& item = LABELS[hand][label];
            if (!item.text)
            {
                continue;
            }
            if (!_labels[hand][label])
            {
                _labels[hand][label] = LabelTexture(d3dDevice, item.text);
            }
            const float lx = cx + side * (h + 0.02f + LABEL_HALF_WIDE), ly = cy + h * 0.6f - label * 0.03f;
            const bool isLit = item.part >= 0 && IsGlowing(PARTS[item.part].glow, hand, pad);
            d3dDevice->lpVtbl->SetFVF(d3dDevice, SCREEN_FORMAT);
            d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(_labels[hand][label]));
            d3dDevice->lpVtbl->SetTexture(d3dDevice, 1, nullptr);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
            d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_TEXTUREFACTOR, isLit ? 0xFFFFC8A0 : 0xFFFF4A30);
            ScreenVertex text[4];
            for (int k = 0; k < 4; ++k)
            {
                float p[3];
                at(lx + corners[k][0] * LABEL_HALF_WIDE, ly + corners[k][1] * LABEL_HALF_HIGH, p);
                // the text reads left to right on either side; the left side's labels end at the screen
                const float u = (corners[k][0] + 1) * 0.5f, v = (1 - corners[k][1]) * 0.5f;
                text[k] = {p[0], p[1], p[2], u, v, 0, 0};
            }
            const ScreenVertex textFan[6] = {text[0], text[1], text[2], text[0], text[2], text[3]};
            d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLELIST, 2, textFan, sizeof(ScreenVertex));
            // the line: from the control's place on the screen to the label's near end, while the control faces the
            // viewer (its outward direction through the controller's turn has a part toward the screen's front)
            if (item.part < 0)
            {
                continue;
            }
            const float* turn = modelToView[hand];
            const float* f = item.facing;
            const float toward = -(f[0] * turn[2] + f[1] * turn[6] + f[2] * turn[10]); // view z into the screen
            if (toward < 0.25f)
            {
                continue;
            }
            const float* m = _partMiddles[hand][item.part];
            const float vx = m[0] * turn[0] + m[1] * turn[4] + m[2] * turn[8] + turn[12];
            const float vy = m[0] * turn[1] + m[1] * turn[5] + m[2] * turn[9] + turn[13];
            if (fabsf(vx) > ORTHO_HALF_METERS || fabsf(vy) > ORTHO_HALF_METERS)
            {
                continue;
            }
            const float sx = cx + vx / ORTHO_HALF_METERS * h, sy = cy + vy / ORTHO_HALF_METERS * h;
            const float ex = lx - side * LABEL_HALF_WIDE, ey = ly;
            const DWORD color = isLit ? 0xFFFFC8A0 : 0xD0FF4A30;
            LineVertex line[2];
            float p[3];
            at(sx, sy, p);
            line[0] = {p[0], p[1], p[2], color};
            at(ex, ey, p);
            line[1] = {p[0], p[1], p[2], color};
            d3dDevice->lpVtbl->SetFVF(d3dDevice, LINE_FORMAT);
            d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, nullptr);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
            d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_LINELIST, 1, line, sizeof(LineVertex));
        }
    }
    _savedState->lpVtbl->Apply(_savedState);
}

void DrawViewscreen(void* device)
{
    if (!device || Stereo::IsScopePass() || !IsDroidSeat())
    {
        return;
    }
    IDirect3DDevice9* const d3dDevice = static_cast<IDirect3DDevice9*>(device);
    if (_viewscreenStateDevice != d3dDevice && _viewscreenState)
    {
        _viewscreenState->lpVtbl->Release(_viewscreenState);
        _viewscreenState = nullptr;
    }
    if (!_viewscreenState &&
        FAILED(d3dDevice->lpVtbl->CreateStateBlock(d3dDevice, D3DSBT_ALL, &_viewscreenState)))
    {
        _viewscreenState = nullptr;
        return;
    }
    _viewscreenStateDevice = d3dDevice;
    _viewscreenState->lpVtbl->Capture(_viewscreenState);
    D3DVIEWPORT9 viewport;
    d3dDevice->lpVtbl->GetViewport(d3dDevice, &viewport);
    FpGuard fp;
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetFVF(d3dDevice, OVERLAY_FORMAT);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LIGHTING, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_FOGENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHATESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_STENCILENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_COLORWRITEENABLE, 0xF);
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, nullptr);
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 1, nullptr);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_TEXCOORDINDEX, 0);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    // the whole viewport (pixel centres on texel centres), the lines along v
    const float left = static_cast<float>(viewport.X) - 0.5f, top = static_cast<float>(viewport.Y) - 0.5f;
    const float right = left + viewport.Width, bottom = top + viewport.Height;
    const float scroll = static_cast<float>(GetTickCount() % 1000000) * 0.001f * VIEWSCREEN_SCROLL_LINES;
    const float vTop = -scroll, vBottom = VIEWSCREEN_LINES - scroll;
    const OverlayVertex quad[6] = {{left, top, 0, 1, 0, vTop},         {right, top, 0, 1, 0, vTop},
                                   {right, bottom, 0, 1, 0, vBottom},  {left, top, 0, 1, 0, vTop},
                                   {right, bottom, 0, 1, 0, vBottom},  {left, bottom, 0, 1, 0, vBottom}};
    // the filter (the picture times its colour), the wash, then the lines (alpha times the line texture)
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_ZERO);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_TEXTUREFACTOR, VIEWSCREEN_FILTER);
    d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLELIST, 2, quad, sizeof(OverlayVertex));
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_TEXTUREFACTOR, VIEWSCREEN_WASH);
    d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLELIST, 2, quad, sizeof(OverlayVertex));
    if (IDirect3DTexture9* const fine = FineScanlines(d3dDevice))
    {
        d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(fine));
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_TEXTUREFACTOR, VIEWSCREEN_LINE);
        d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLELIST, 2, quad, sizeof(OverlayVertex));
    }
    _viewscreenState->lpVtbl->Apply(_viewscreenState);
}

void OnReset()
{
    if (_viewscreenState)
    {
        _viewscreenState->lpVtbl->Release(_viewscreenState);
        _viewscreenState = nullptr;
    }
    if (_savedState)
    {
        _savedState->lpVtbl->Release(_savedState);
        _savedState = nullptr;
    }
    if (_scanlines)
    {
        _scanlines->lpVtbl->Release(_scanlines);
        _scanlines = nullptr;
    }
    if (_fineScanlines)
    {
        _fineScanlines->lpVtbl->Release(_fineScanlines);
        _fineScanlines = nullptr;
    }
    for (IDirect3DTexture9** texture : {&_eyes[0], &_eyes[1], &_gaugeLabels[0], &_gaugeLabels[1], &_targetLabel})
    {
        if (*texture)
        {
            (*texture)->lpVtbl->Release(*texture);
            *texture = nullptr;
        }
    }
    for (auto& row : _labels)
    {
        for (IDirect3DTexture9*& label : row)
        {
            if (label)
            {
                label->lpVtbl->Release(label);
                label = nullptr;
            }
        }
    }
    ReleaseTargets();
}

void SetTargetGrabs(int count)
{
    _targetGrabsLeft = count;
}

void SetForced(bool isForced)
{
    _isForced = isForced;
    Log::Info("holo", "controller holograms %s", isForced ? "forced on (every vehicle seat)" : "as the seat has them");
}

} // namespace ControllerHolo

} // namespace SwbfVr
