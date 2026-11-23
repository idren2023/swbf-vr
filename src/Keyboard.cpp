// Keyboard.cpp: a virtual keyboard for the menus that want typed text (see Keyboard.h).

// d3d9.h's interfaces as C structs with an explicit vtable, the form the rest of the mod uses (before any include).
#define CINTERFACE

#include "Keyboard.h"

#include "Feedback.h"
#include "FpGuard.h"
#include "Input.h"
#include "Log.h"
#include "Pointer.h"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstring>

namespace SwbfVr
{

namespace Keyboard
{

namespace
{

/// One key: its label, its DirectInput key code, whether shift is held with it, and its place in the grid (row,
/// first column, columns wide).
struct KeyDef
{
    const char* label;
    unsigned code;
    bool isShifted;
    int row, column, span;
};

/// Key codes that are not typed: shift (the next key is typed with shift held) and hide.
constexpr unsigned CODE_SHIFT = 0x2A, CODE_HIDE = 0x1000, CODE_BACKSPACE = 0x0E;
constexpr int GRID_COLUMNS = 12, GRID_ROWS = 5;
const KeyDef KEYS[] = {
    {"1", 0x02, false, 0, 0, 1},
    {"2", 0x03, false, 0, 1, 1},
    {"3", 0x04, false, 0, 2, 1},
    {"4", 0x05, false, 0, 3, 1},
    {"5", 0x06, false, 0, 4, 1},
    {"6", 0x07, false, 0, 5, 1},
    {"7", 0x08, false, 0, 6, 1},
    {"8", 0x09, false, 0, 7, 1},
    {"9", 0x0A, false, 0, 8, 1},
    {"0", 0x0B, false, 0, 9, 1},
    {"-", 0x0C, false, 0, 10, 1},
    {"DEL", CODE_BACKSPACE, false, 0, 11, 1},
    {"Q", 0x10, false, 1, 0, 1},
    {"W", 0x11, false, 1, 1, 1},
    {"E", 0x12, false, 1, 2, 1},
    {"R", 0x13, false, 1, 3, 1},
    {"T", 0x14, false, 1, 4, 1},
    {"Y", 0x15, false, 1, 5, 1},
    {"U", 0x16, false, 1, 6, 1},
    {"I", 0x17, false, 1, 7, 1},
    {"O", 0x18, false, 1, 8, 1},
    {"P", 0x19, false, 1, 9, 1},
    {".", 0x34, false, 1, 10, 1},
    {"_", 0x0C, true, 1, 11, 1},
    {"A", 0x1E, false, 2, 0, 1},
    {"S", 0x1F, false, 2, 1, 1},
    {"D", 0x20, false, 2, 2, 1},
    {"F", 0x21, false, 2, 3, 1},
    {"G", 0x22, false, 2, 4, 1},
    {"H", 0x23, false, 2, 5, 1},
    {"J", 0x24, false, 2, 6, 1},
    {"K", 0x25, false, 2, 7, 1},
    {"L", 0x26, false, 2, 8, 1},
    {"ENTER", 0x1C, false, 2, 9, 3},
    {"SHIFT", CODE_SHIFT, false, 3, 0, 2},
    {"Z", 0x2C, false, 3, 2, 1},
    {"X", 0x2D, false, 3, 3, 1},
    {"C", 0x2E, false, 3, 4, 1},
    {"V", 0x2F, false, 3, 5, 1},
    {"B", 0x30, false, 3, 6, 1},
    {"N", 0x31, false, 3, 7, 1},
    {"M", 0x32, false, 3, 8, 1},
    {",", 0x33, false, 3, 9, 1},
    {"!", 0x02, true, 3, 10, 1},
    {"?", 0x35, true, 3, 11, 1},
    {"TAB", 0x0F, false, 4, 0, 2},
    {"SPACE", 0x39, false, 4, 2, 8},
    {"HIDE", CODE_HIDE, false, 4, 10, 2},
};
constexpr int KEY_COUNT = static_cast<int>(sizeof(KEYS) / sizeof(KEYS[0]));

/// Where the keys and the tab lie on the screen (fractions from the top left). The bottom corners stay free: the
/// menus have their BACK and OK there.
constexpr float PANEL_LEFT = 0.15f, PANEL_RIGHT = 0.85f, PANEL_TOP = 0.57f, PANEL_BOTTOM = 0.985f;
constexpr float TAB_LEFT = 0.43f, TAB_RIGHT = 0.57f, TAB_TOP = 0.945f, TAB_BOTTOM = 0.99f;
/// The texture: the keys in its upper part, the tab in a strip below.
constexpr int TEXTURE_WIDTH = 1024, TEXTURE_HEIGHT = 512, PANEL_PIXELS_HIGH = 440, TAB_PIXELS_TOP = 452,
              TAB_PIXELS_WIDE = 256, TAB_PIXELS_HIGH = 56, KEY_GAP_PIXELS = 4, FONT_PIXELS = 40,
              TAB_FONT_PIXELS = 30, BORDER_PIXELS = 3, PANEL_MARGIN_PIXELS = 6;
/// How long a key is held for the game, and how long it stays lit on the panel.
constexpr DWORD KEY_HOLD_MILLISECONDS = 70, KEY_LIT_MILLISECONDS = 180;
/// The pending key that stands for the tab.
constexpr LONG PENDING_NONE = -1, PENDING_TAB = -2;

struct Vertex
{
    float x, y, z, rhw;
    DWORD colour;
    float u, v;
};
constexpr DWORD VERTEX_FORMAT = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
constexpr int BOX_VERTICES = 6;

IDirect3DTexture9* _texture = nullptr;
IDirect3DStateBlock9* _savedState = nullptr;
IDirect3DDevice9* _device = nullptr;
bool _hasTextureFailed = false;
volatile bool _isOpen = false;
volatile bool _isShiftOn = false;
volatile LONG _pendingKey = PENDING_NONE; // index into KEYS taken by OnMouseButton, pressed by OnPresent
volatile bool _isButtonTaken = false;     // the press went to the keyboard: so does its release
int _heldKey = -1;                        // the key now held down for the game
bool _isHeldShifted = false;
DWORD _heldTick = 0;
int _litKey = -1;
DWORD _litTick = 0;

// The rectangle of a key in fractions of the panel.
void KeyRect(const KeyDef& key, float* left, float* top, float* right, float* bottom)
{
    *left = static_cast<float>(key.column) / GRID_COLUMNS;
    *right = static_cast<float>(key.column + key.span) / GRID_COLUMNS;
    *top = static_cast<float>(key.row) / GRID_ROWS;
    *bottom = static_cast<float>(key.row + 1) / GRID_ROWS;
}

// The key under the cursor: its index, PENDING_TAB for the tab (keys hidden), -1 for none. `isOnPanel` tells whether
// the cursor is anywhere on the shown keys (a click between two keys is the keyboard's, not the menu's behind it).
int KeyAtCursor(bool* isOnPanel)
{
    *isOnPanel = false;
    float u = 0, v = 0;
    if (!Pointer::GetCursor(&u, &v))
    {
        return -1;
    }
    if (!_isOpen)
    {
        return u >= TAB_LEFT && u <= TAB_RIGHT && v >= TAB_TOP && v <= TAB_BOTTOM ? PENDING_TAB : -1;
    }
    if (u < PANEL_LEFT || u > PANEL_RIGHT || v < PANEL_TOP || v > PANEL_BOTTOM)
    {
        return -1;
    }
    *isOnPanel = true;
    const float x = (u - PANEL_LEFT) / (PANEL_RIGHT - PANEL_LEFT), y = (v - PANEL_TOP) / (PANEL_BOTTOM - PANEL_TOP);
    for (int index = 0; index < KEY_COUNT; ++index)
    {
        float left, top, right, bottom;
        KeyRect(KEYS[index], &left, &top, &right, &bottom);
        if (x >= left && x < right && y >= top && y < bottom)
        {
            return index;
        }
    }
    return -1;
}

void FillBox(HDC context, int left, int top, int right, int bottom, COLORREF colour)
{
    const RECT box = {left, top, right, bottom};
    HBRUSH brush = CreateSolidBrush(colour);
    FillRect(context, &box, brush);
    DeleteObject(brush);
}

void Label(HDC context, const char* text, int left, int top, int right, int bottom)
{
    RECT box = {left, top, right, bottom};
    DrawTextA(context, text, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

HFONT MakeFont(int pixels)
{
    return CreateFontA(
        -pixels,
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
}

// The keys and the tab painted with GDI into one texture (managed pool: it outlives a device reset).
bool MakeTexture(IDirect3DDevice9* d3dDevice)
{
    BITMAPINFO info = {};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = TEXTURE_WIDTH;
    info.bmiHeader.biHeight = -TEXTURE_HEIGHT; // negative: rows from the top
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
    HGDIOBJ oldBitmap = SelectObject(context, bitmap);
    HFONT font = MakeFont(FONT_PIXELS), tabFont = MakeFont(TAB_FONT_PIXELS);
    HGDIOBJ oldFont = font ? SelectObject(context, font) : nullptr;
    SetBkMode(context, TRANSPARENT);
    SetTextColor(context, RGB(235, 245, 255));
    // The menus are blue with orange headings: a dark blue board, lighter keys, an orange edge.
    const COLORREF board = RGB(8, 22, 52), keyColour = RGB(28, 62, 120), wideColour = RGB(44, 84, 150),
                   edge = RGB(240, 120, 30);
    FillBox(context, 0, 0, TEXTURE_WIDTH, TEXTURE_HEIGHT, RGB(0, 0, 0));
    FillBox(context, 0, 0, TEXTURE_WIDTH, PANEL_PIXELS_HIGH, edge);
    FillBox(
        context, BORDER_PIXELS, BORDER_PIXELS, TEXTURE_WIDTH - BORDER_PIXELS, PANEL_PIXELS_HIGH - BORDER_PIXELS, board);
    const int innerWide = TEXTURE_WIDTH - 2 * PANEL_MARGIN_PIXELS, innerHigh = PANEL_PIXELS_HIGH - 2 * PANEL_MARGIN_PIXELS;
    for (const KeyDef& key : KEYS)
    {
        float left, top, right, bottom;
        KeyRect(key, &left, &top, &right, &bottom);
        const int x0 = PANEL_MARGIN_PIXELS + static_cast<int>(left * static_cast<float>(innerWide)) + KEY_GAP_PIXELS,
                  x1 = PANEL_MARGIN_PIXELS + static_cast<int>(right * static_cast<float>(innerWide)) - KEY_GAP_PIXELS,
                  y0 = PANEL_MARGIN_PIXELS + static_cast<int>(top * static_cast<float>(innerHigh)) + KEY_GAP_PIXELS,
                  y1 = PANEL_MARGIN_PIXELS + static_cast<int>(bottom * static_cast<float>(innerHigh)) - KEY_GAP_PIXELS;
        FillBox(context, x0, y0, x1, y1, key.span > 1 || key.code == CODE_BACKSPACE ? wideColour : keyColour);
        Label(context, key.label, x0, y0, x1, y1);
    }
    FillBox(context, 0, TAB_PIXELS_TOP, TAB_PIXELS_WIDE, TAB_PIXELS_TOP + TAB_PIXELS_HIGH, edge);
    FillBox(
        context,
        BORDER_PIXELS,
        TAB_PIXELS_TOP + BORDER_PIXELS,
        TAB_PIXELS_WIDE - BORDER_PIXELS,
        TAB_PIXELS_TOP + TAB_PIXELS_HIGH - BORDER_PIXELS,
        board);
    if (tabFont)
    {
        SelectObject(context, tabFont);
    }
    Label(context, "KEYBOARD", 0, TAB_PIXELS_TOP, TAB_PIXELS_WIDE, TAB_PIXELS_TOP + TAB_PIXELS_HIGH);
    GdiFlush();
    bool isMade = false;
    if (SUCCEEDED(d3dDevice->lpVtbl->CreateTexture(
            d3dDevice, TEXTURE_WIDTH, TEXTURE_HEIGHT, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &_texture, nullptr)))
    {
        D3DLOCKED_RECT locked = {};
        if (SUCCEEDED(_texture->lpVtbl->LockRect(_texture, 0, &locked, nullptr, 0)))
        {
            const uint32_t* source = static_cast<const uint32_t*>(bits);
            for (int row = 0; row < TEXTURE_HEIGHT; ++row)
            {
                uint32_t* target =
                    reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(locked.pBits) + row * locked.Pitch);
                for (int column = 0; column < TEXTURE_WIDTH; ++column)
                {
                    // A little see-through, so the menu behind is not lost.
                    target[column] = 0xEB000000 | (source[row * TEXTURE_WIDTH + column] & 0xFFFFFF);
                }
            }
            _texture->lpVtbl->UnlockRect(_texture, 0);
            isMade = true;
        }
        else
        {
            _texture->lpVtbl->Release(_texture);
            _texture = nullptr;
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
    if (tabFont)
    {
        DeleteObject(tabFont);
    }
    SelectObject(context, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(context);
    return isMade;
}

/// A rectangle: screen fractions for a place on the screen, texture fractions for a part of the texture.
struct Rect
{
    float left, top, right, bottom;
};

// One screen rectangle as two triangles; returns the number of vertices written.
int Box(Vertex* vertices, float width, float height, const Rect& place, DWORD colour, const Rect& texture)
{
    const float x0 = place.left * width - 0.5f, x1 = place.right * width - 0.5f, y0 = place.top * height - 0.5f,
                y1 = place.bottom * height - 0.5f;
    const Vertex corners[4] = {
        {x0, y0, 0, 1, colour, texture.left, texture.top},
        {x1, y0, 0, 1, colour, texture.right, texture.top},
        {x1, y1, 0, 1, colour, texture.right, texture.bottom},
        {x0, y1, 0, 1, colour, texture.left, texture.bottom}};
    const int order[BOX_VERTICES] = {0, 1, 2, 0, 2, 3};
    for (int index = 0; index < BOX_VERTICES; ++index)
    {
        vertices[index] = corners[order[index]];
    }
    return BOX_VERTICES;
}

} // namespace

bool IsOpen()
{
    return _isOpen;
}

void SetOpen(bool isOpen)
{
    if (isOpen != _isOpen)
    {
        _isOpen = isOpen;
        _isShiftOn = false;
        Log::Info("keyboard", "keys %s", isOpen ? "shown" : "hidden");
    }
}

bool OnMouseButton(bool isDown)
{
    if (!isDown)
    {
        const bool wasTaken = _isButtonTaken;
        _isButtonTaken = false;
        return wasTaken;
    }
    bool isOnPanel = false;
    const int key = KeyAtCursor(&isOnPanel);
    if (key == -1 && !isOnPanel)
    {
        return false;
    }
    if (key != -1)
    {
        InterlockedExchange(&_pendingKey, key);
    }
    _isButtonTaken = true;
    return true;
}

void OnPresent()
{
    const DWORD now = GetTickCount();
    if (_heldKey >= 0 && now - _heldTick >= KEY_HOLD_MILLISECONDS)
    {
        Input::Key(KEYS[_heldKey].code, false);
        if (_isHeldShifted)
        {
            Input::Key(CODE_SHIFT, false);
        }
        _heldKey = -1;
    }
    if (_heldKey >= 0)
    {
        return; // one key at a time
    }
    const LONG pending = InterlockedExchange(&_pendingKey, PENDING_NONE);
    if (pending == PENDING_TAB)
    {
        SetOpen(true);
        Feedback::Pulse(0, 0.04f, 0.5f);
        return;
    }
    if (pending < 0 || pending >= KEY_COUNT || !_isOpen)
    {
        return;
    }
    const KeyDef& key = KEYS[pending];
    _litKey = static_cast<int>(pending);
    _litTick = now;
    Feedback::Pulse(0, 0.03f, 0.5f);
    if (key.code == CODE_HIDE)
    {
        SetOpen(false);
        return;
    }
    if (key.code == CODE_SHIFT)
    {
        _isShiftOn = !_isShiftOn;
        return;
    }
    _isHeldShifted = key.isShifted || _isShiftOn;
    _isShiftOn = false;
    if (_isHeldShifted)
    {
        Input::Key(CODE_SHIFT, true);
    }
    Input::Key(key.code, true);
    _heldKey = static_cast<int>(pending);
    _heldTick = now;
    Log::Info("keyboard", "key %s%s", key.label, _isHeldShifted ? " with shift" : "");
}

void OnReset()
{
    if (_savedState)
    {
        _savedState->lpVtbl->Release(_savedState);
        _savedState = nullptr;
    }
}

void Draw(void* device, int width, int height)
{
    // The tab only while a pointer is on the screen (nobody at the monitor needs it); the keys whenever open.
    if (!device || width <= 0 || height <= 0 || (!_isOpen && !Pointer::IsActive()))
    {
        return;
    }
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    if (_device != d3dDevice)
    {
        OnReset();
        if (_texture)
        {
            _texture->lpVtbl->Release(_texture);
            _texture = nullptr;
        }
        _hasTextureFailed = false;
        _device = d3dDevice;
    }
    if (!_texture && !_hasTextureFailed && !MakeTexture(d3dDevice))
    {
        _hasTextureFailed = true;
        Log::Info("keyboard", "the key texture could not be made: no keyboard");
    }
    if (!_texture)
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
    constexpr int MOST_LIGHTS = 3; // the hovered key, the pressed key, shift
    Vertex textured[BOX_VERTICES], plain[MOST_LIGHTS * BOX_VERTICES];
    int texturedCount = 0, plainCount = 0;
    {
        FpGuard fp;
        const float w = static_cast<float>(width), h = static_cast<float>(height);
        const Rect none = {0, 0, 0, 0};
        bool isOnPanel = false;
        const int hovered = KeyAtCursor(&isOnPanel);
        if (_isOpen)
        {
            const Rect panel = {PANEL_LEFT, PANEL_TOP, PANEL_RIGHT, PANEL_BOTTOM};
            texturedCount += Box(
                textured, w, h, panel, 0xFFFFFFFF, {0, 0, 1, static_cast<float>(PANEL_PIXELS_HIGH) / TEXTURE_HEIGHT});
            const bool isLit = _litKey >= 0 && GetTickCount() - _litTick < KEY_LIT_MILLISECONDS;
            const float panelWide = PANEL_RIGHT - PANEL_LEFT, panelHigh = PANEL_BOTTOM - PANEL_TOP;
            for (int index = 0; index < KEY_COUNT; ++index)
            {
                const bool isShiftKey = KEYS[index].code == CODE_SHIFT && _isShiftOn;
                const DWORD colour = isLit && index == _litKey ? 0xB0FFC060
                                     : index == hovered        ? 0x50FFFFFF
                                     : isShiftKey              ? 0x70F07820
                                                               : 0;
                if (!colour || plainCount + BOX_VERTICES > MOST_LIGHTS * BOX_VERTICES)
                {
                    continue;
                }
                float left, top, right, bottom;
                KeyRect(KEYS[index], &left, &top, &right, &bottom);
                const Rect place = {
                    PANEL_LEFT + left * panelWide,
                    PANEL_TOP + top * panelHigh,
                    PANEL_LEFT + right * panelWide,
                    PANEL_TOP + bottom * panelHigh};
                plainCount += Box(plain + plainCount, w, h, place, colour, none);
            }
        }
        else
        {
            const Rect tab = {TAB_LEFT, TAB_TOP, TAB_RIGHT, TAB_BOTTOM};
            texturedCount += Box(
                textured,
                w,
                h,
                tab,
                0xFFFFFFFF,
                {0,
                 static_cast<float>(TAB_PIXELS_TOP) / TEXTURE_HEIGHT,
                 static_cast<float>(TAB_PIXELS_WIDE) / TEXTURE_WIDTH,
                 static_cast<float>(TAB_PIXELS_TOP + TAB_PIXELS_HIGH) / TEXTURE_HEIGHT});
            if (hovered == PENDING_TAB)
            {
                plainCount += Box(plain, w, h, tab, 0x50FFFFFF, none);
            }
        }
    }
    const bool isInScene = SUCCEEDED(d3dDevice->lpVtbl->BeginScene(d3dDevice));
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetFVF(d3dDevice, VERTEX_FORMAT);
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(_texture));
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LIGHTING, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_FOGENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHATESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_STENCILENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SCISSORTESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_COLORWRITEENABLE, 0x7);
    if (texturedCount)
    {
        d3dDevice->lpVtbl->DrawPrimitiveUP(
            d3dDevice, D3DPT_TRIANGLELIST, static_cast<UINT>(texturedCount / 3), textured, sizeof(Vertex));
    }
    if (plainCount)
    {
        d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, nullptr);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        d3dDevice->lpVtbl->DrawPrimitiveUP(
            d3dDevice, D3DPT_TRIANGLELIST, static_cast<UINT>(plainCount / 3), plain, sizeof(Vertex));
    }
    if (isInScene)
    {
        d3dDevice->lpVtbl->EndScene(d3dDevice);
    }
    _savedState->lpVtbl->Apply(_savedState);
}

} // namespace Keyboard

} // namespace SwbfVr
