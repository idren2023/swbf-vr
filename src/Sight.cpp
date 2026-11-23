// Sight.cpp: an aiming line for the hand-held weapon, and for the guns of a gunner seat or a droideka.
//
// With the weapon in the hand the game's crosshair (the middle of the HUD) says nothing about where a shot goes.
// The mod draws a thin line from the muzzle along the weapon's aim instead, like a laser sight: it is tested
// against the scene's depth, so it ends on whatever the weapon points at. In a gunner seat and as a droideka the guns
// are the game's, aimed by their Aimers: one line per Aimer from its fire point (PLAN 57 g, h, b; headset run 16: the
// fighter tank's reticle felt off, and the droideka's bolts went where its arms had caught up to, not where it
// looked), and the game's reticle is left out meanwhile (IsReplacingReticle).
//
// Drawn once per eye, at the moment Render() switches from the scene to the first-person pass (Stereo.cpp calls
// Sight::Draw from its SetNearFar hook): the scene and its depth are complete, and Direct3D's VIEW and PROJECTION
// transforms are still the scene's for this eye (the game sets them besides its shader constants). The line is
// drawn without shaders, and every state touched is put back through a state block.

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable (device->lpVtbl->Method(device,
// ...)), the form the rest of the mod uses. Has to be a macro: d3d9.h reads it.
#define CINTERFACE

#include "Sight.h"

#include "Aim.h"
#include "Controls.h"
#include "Feedback.h"
#include "FpGuard.h"
#include "GunnerSeat.h"
#include "Holster.h"
#include "Log.h"
#include "Stereo.h"
#include "Textures.h"
#include "Vehicle.h"
#include "Zoom.h"

#include <windows.h>
#include <d3d9.h>

#include <cmath>

namespace SwbfVr
{

namespace Sight
{

namespace
{

bool _isOn = true;
float _lengthMeters = 80.0f;
D3DCOLOR _color = 0x606CB4FF; // pale blue like the game's reticle, see-through (idren2023, headset run 8; fainter after
                              // run 18); hits flash it in their colour
IDirect3DStateBlock9* _savedState = nullptr;
IDirect3DDevice9* _stateBlockDevice = nullptr;
unsigned _drawCount = 0;
unsigned _seatDrawCount = 0;
DWORD _seatLineTick = 0; // when seat lines were last drawn
bool _haveSeatLinesBeenDrawn = false;
/// Most lines drawn at once (a seat's Aimers: the fighter tank's turret has one, the droideka two).
constexpr int MOST_LINES = 4;
/// The game's reticle stays out this long after the last seat line (the line is drawn per eye pass).
constexpr DWORD RETICLE_HIDE_MILLISECONDS = 250;

struct Vertex
{
    float x, y, z;
    D3DCOLOR color;
};

DWORD _flashStartTick = 0; // 0 = no flash
D3DCOLOR _flashColor = 0;
/// The line's colour while aimed at an enemy or a friend: pale red and pale green, as see-through as the normal line.
/// Enemy: stronger than the friend's green (b129: the pale red was not noticed in headset run 11, though the log has
/// the game reporting enemies under the reticle 8 times, most for under half a second).
constexpr D3DCOLOR ENEMY_COLOR = 0x84FF3C32; // headset run 18: all three fainter
/// A target's colour stays this long after the game stops reporting it (brief reports, 16 to 970 ms, headset run 11).
constexpr DWORD RELATION_HOLD_MILLISECONDS = 300;
constexpr D3DCOLOR FRIEND_COLOR = 0x607AE68C;
/// The periscope's reticle: the gunner cannon's ReticuleTexture, half its width as a tangent of the muzzle camera's
/// view (about 5 deg across of the display's 38; 3.4 until b291: too small and faint, idren2023), where it stands
/// without a target point, and its colours (the game's reticle blue, red over an enemy, green over a friend, lightened;
/// drawn a second time added on, for the stock reticle's glow).
constexpr const char* PERISCOPE_RETICLE_NAME = "reticule_rifle";
constexpr float PERISCOPE_RETICLE_HALF_TAN = 0.045f, PERISCOPE_RETICLE_FALLBACK_METERS = 100.0f;
constexpr D3DCOLOR PERISCOPE_RETICLE_COLOR = 0xFF6CC8FF, PERISCOPE_ENEMY_COLOR = 0xFFFF5A46,
                   PERISCOPE_FRIEND_COLOR = 0xFF8CFF9C;
constexpr DWORD FLASH_MILLISECONDS = 220;
/// During a flash the line is this many times as wide.
constexpr float FLASH_WIDTH_FACTOR = 3.0f;
/// Half the ribbon's width per metre of distance from the eye (about 0.11 degrees wide: two pixels of the eye
/// image), and the least half width close to the eye.
constexpr float HALF_WIDTH_PER_METER = 0.0010f;
constexpr float MINIMUM_HALF_WIDTH_METERS = 0.0012f;
/// The length the table of ribbon points in Draw is laid out for; the points are scaled to _lengthMeters.
constexpr float POINT_TABLE_LENGTH_METERS = 80.0f;

} // namespace

// A hit (Feedback.cpp): the line is drawn bright and wide in this colour for a moment, also when it is switched off.
void Flash(unsigned rgb)
{
    _flashColor = 0xFF000000 | (rgb & 0xFFFFFF);
    _flashStartTick = GetTickCount() | 1; // never 0, which means "no flash"
}

void SetEnabled(bool isOn)
{
    _isOn = isOn;
    Log::Info("sight", "aiming line %s", isOn ? "on" : "off");
}

// The state block lives in the default pool: release it before the device is reset.
void OnReset()
{
    if (_savedState)
    {
        _savedState->lpVtbl->Release(_savedState);
        _savedState = nullptr;
    }
    _stateBlockDevice = nullptr;
}

// The stock reticle in the periscope's picture (idren2023 2026-10-19): the gunner cannon's ReticuleTexture
// (all_weap_hover_combatspeeder_cannon: reticule_rifle) facing the muzzle camera on the first gun's line (the one the
// camera looks along, so near the picture's middle) as far out as the point the game aims the guns at (its own ray's
// hit, Controllable+0x10), else 100 m. Not on that point itself: at an end stop it follows the head past the limit
// while the guns stay (run318); not on the guns' mean line: the seat's second Aimer pointed elsewhere (run319). A
// constant angle wide, drawn over the scene, in the game's reticle colours (blue, red on an enemy, green on a friend).
// Device state as Draw left it (no depth test is set here).
void DrawPeriscopeReticle(IDirect3DDevice9* device, const float* line, int relation)
{
    IDirect3DTexture9* const texture = static_cast<IDirect3DTexture9*>(Textures::FindByName(PERISCOPE_RETICLE_NAME));
    float camera[16], target[3];
    if (!texture || !Stereo::GetScopeCamera(camera))
    {
        return;
    }

    struct TexturedVertex
    {
        float x, y, z;
        D3DCOLOR color;
        float u, v;
    };

    TexturedVertex quad[4];
    {
        FpGuard fp;
        const float* from = line;
        const float* direction = line + 3; // a unit vector (Vehicle::GetSeatFireLines)
        constexpr float length = 1.0f;
        float range = PERISCOPE_RETICLE_FALLBACK_METERS;
        if (Vehicle::GetSeatTarget(target))
        {
            const float dx = target[0] - from[0], dy = target[1] - from[1], dz = target[2] - from[2];
            range = sqrtf(dx * dx + dy * dy + dz * dz);
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            target[axis] = from[axis] + direction[axis] / length * range;
        }
        float distance = 0; // along the camera's forward (-back)
        for (int axis = 0; axis < 3; ++axis)
        {
            distance -= (target[axis] - camera[12 + axis]) * camera[8 + axis];
        }
        if (distance < 0.5f)
        {
            return;
        }
        const float half = distance * PERISCOPE_RETICLE_HALF_TAN;
        const D3DCOLOR color = relation < 0   ? PERISCOPE_ENEMY_COLOR
                               : relation > 0 ? PERISCOPE_FRIEND_COLOR
                                              : PERISCOPE_RETICLE_COLOR;
        const float corners[4][4] = {{-1, 1, 0, 0}, {1, 1, 1, 0}, {-1, -1, 0, 1}, {1, -1, 1, 1}}; // x, y, u, v
        for (int corner = 0; corner < 4; ++corner)
        {
            float* position = &quad[corner].x;
            for (int axis = 0; axis < 3; ++axis)
            {
                position[axis] =
                    target[axis] + (corners[corner][0] * camera[axis] + corners[corner][1] * camera[4 + axis]) * half;
            }
            quad[corner].color = color;
            quad[corner].u = corners[corner][2];
            quad[corner].v = corners[corner][3];
        }
    }
    device->lpVtbl->SetRenderState(device, D3DRS_ZENABLE, D3DZB_FALSE);
    device->lpVtbl->SetFVF(device, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    device->lpVtbl->SetTexture(device, 0, reinterpret_cast<IDirect3DBaseTexture9*>(texture));
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    device->lpVtbl->SetTextureStageState(device, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    device->lpVtbl->SetSamplerState(device, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    device->lpVtbl->SetSamplerState(device, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    device->lpVtbl->SetSamplerState(device, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    device->lpVtbl->SetSamplerState(device, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    device->lpVtbl->DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TexturedVertex));
    device->lpVtbl->SetRenderState(device, D3DRS_DESTBLEND, D3DBLEND_ONE); // again, added on: brighter
    device->lpVtbl->DrawPrimitiveUP(device, D3DPT_TRIANGLESTRIP, 2, quad, sizeof(TexturedVertex));
    static bool hasLogged = false;
    if (!hasLogged)
    {
        hasLogged = true;
        Log::Info("sight", "periscope reticle drawn (%s)", PERISCOPE_RETICLE_NAME);
    }
}

bool IsReplacingReticle()
{
    // The tick exactly as stored: stored as `now | 1` it lay one past an even `now`, the difference wrapped and the
    // game's reticle came back for that frame (the fighter tank gunner's flickering reticle, idren2023 2026-10-19).
    return _isOn && _haveSeatLinesBeenDrawn && GetTickCount() - _seatLineTick < RETICLE_HIDE_MILLISECONDS;
}

void Draw(void* device)
{
    const bool isFlashing = _flashStartTick && GetTickCount() - _flashStartTick < FLASH_MILLISECONDS;
    // Not in the scope picture's own render: the line would cut across it (docs/ZOOM.md). In the eye passes it stays on
    // while zoomed (headset run 15, idren2023). In the speeder gunner's periscope picture only the stock reticle on the
    // gun's line (b289-b290 drew the guns' lines too: from the muzzle each runs off to its own vanishing point, which
    // read as two lines converging and crossing; idren2023 2026-10-19: remove them).
    const bool isPeriscope = Stereo::IsPeriscopePass();
    if ((!_isOn && !isFlashing && !isPeriscope) || !device || (Stereo::IsScopePass() && !isPeriscope))
    {
        return;
    }
    // Each line: start, unit direction, and whether it begins at its start (a barrel opening or a fire point) rather
    // than 10 cm along.
    float lines[MOST_LINES][6];
    bool isFromStart[MOST_LINES] = {};
    int lineCount = 0;
    const Vehicle::Seat seat = Vehicle::CurrentSeat();
    const bool isInVehicle = seat != Vehicle::Seat::OnFoot && seat != Vehicle::Seat::None; // no hand-held weapon
    const bool isDroideka = seat == Vehicle::Seat::OnFoot && Aim::IsDroideka();
    // (pilot seats too since headset run 19: tanks, speeders, walkers and fighters aim by the line, not the reticle)
    const bool isSeatWithGuns = seat == Vehicle::Seat::Turret || seat == Vehicle::Seat::Hover ||
                                seat == Vehicle::Seat::Flyer || seat == Vehicle::Seat::Walker || isDroideka;
    if (isDroideka && !Controls::IsDroidekaStanding())
    {
        return; // rolled up: no lines (PLAN 58 f)
    }
    if (isSeatWithGuns)
    {
        // The droideka: its two arm guns only (slot 0 and the one linked from it; slot 1's third line from the body
        // went, PLAN 58 f).
        lineCount = Vehicle::GetSeatFireLines(&lines[0][0], isDroideka ? 2 : MOST_LINES);
        for (int line = 0; line < lineCount; ++line)
        {
            isFromStart[line] = true;
        }
        float muzzle[3];
        if (lineCount > 0 && GunnerSeat::GetDrawnMuzzle(muzzle))
        {
            memcpy(lines[0], muzzle, sizeof(muzzle)); // from the barrel as drawn (the AT-TE cannon; headset run 19)
        }
        if (lineCount > 0)
        {
            _seatLineTick = GetTickCount();
            _haveSeatLinesBeenDrawn = true;
        }
    }
    else if (!isInVehicle && !Holster::HoldsNothing() && !Holster::HoldsSecondary() &&
             Aim::GetFireLine(lines[0], lines[0] + 3))
    {
        // The droid: from its barrels as drawn this frame. The fire line kept from the game's update jumped about 12 cm
        // along the barrels from frame to frame (run181), and sat a few cm ahead of the drawn opening (PLAN 31).
        if (!Aim::GetBarrelOpeningWorld(lines[0], lines[0] + 3))
        {
            Aim::GetDrawnFireLine(lines[0], lines[0] + 3); // the muzzle as drawn (headset run 18: off it while walking)
        }
        isFromStart[0] = Aim::IsFireLineFromOpening();
        lineCount = 1;
    }
    if (lineCount == 0)
    {
        return;
    }
    // Over a target the line takes on the game's reticle colour, faintly (b124, idren2023 after headset run 9).
    static int heldRelation = 0;
    static DWORD relationTick = 0;
    const int reported = Feedback::AimedRelation();
    const DWORD now = GetTickCount();
    if (reported != 0 || now - relationTick > RELATION_HOLD_MILLISECONDS)
    {
        heldRelation = reported;
        relationTick = reported != 0 ? now : relationTick;
    }
    const int relation = heldRelation;
    const D3DCOLOR lineColor = isFlashing     ? _flashColor
                               : relation < 0 ? ENEMY_COLOR
                               : relation > 0 ? FRIEND_COLOR
                                              : _color;
    const float widthFactor = isFlashing ? FLASH_WIDTH_FACTOR : 1.0f;
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

    // A ribbon facing the eye, about 0.11 degrees wide at every distance (a plain 1-pixel line was too faint, run63).
    constexpr int POINT_COUNT = 7;
    static const float DISTANCES_ALONG_METERS[POINT_COUNT] = {0.10f, 0.5f, 2.0f, 6.0f, 15.0f, 35.0f, 80.0f};
    Vertex ribbons[MOST_LINES][POINT_COUNT * 2];
    float eye[3];
    if (!Stereo::GetEyePosition(eye))
    {
        _savedState->lpVtbl->Apply(_savedState);
        return;
    }
    for (int line = 0; line < lineCount; ++line)
    {
        const float* start = lines[line];
        const float* direction = lines[line] + 3;
        Vertex* ribbon = ribbons[line];
        FpGuard guard; // RAII: saves the floating-point state here and restores it at the closing brace
        for (int point = 0; point < POINT_COUNT; ++point)
        {
            // From the barrels' opening itself where the fire line starts there (the droid, b160; a seat's fire point).
            const float distanceAlong = point == 0 && isFromStart[line]
                                            ? 0.0f
                                            : DISTANCES_ALONG_METERS[point] * _lengthMeters / POINT_TABLE_LENGTH_METERS;
            const float position[3] = {
                start[0] + direction[0] * distanceAlong,
                start[1] + direction[1] * distanceAlong,
                start[2] + direction[2] * distanceAlong};
            const float fromEye[3] = {position[0] - eye[0], position[1] - eye[1], position[2] - eye[2]};
            // Across the line and across the view: the cross product of the direction and the view ray.
            float side[3] = {
                direction[1] * fromEye[2] - direction[2] * fromEye[1],
                direction[2] * fromEye[0] - direction[0] * fromEye[2],
                direction[0] * fromEye[1] - direction[1] * fromEye[0]};
            const float sideLength = sqrtf(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
            const float distance = sqrtf(fromEye[0] * fromEye[0] + fromEye[1] * fromEye[1] + fromEye[2] * fromEye[2]);
            float halfWidth =
                distance * HALF_WIDTH_PER_METER * widthFactor; // about 0.11 degrees wide: two pixels of the eye image
            if (halfWidth < MINIMUM_HALF_WIDTH_METERS * widthFactor)
            {
                halfWidth = MINIMUM_HALF_WIDTH_METERS * widthFactor;
            }
            const float sideScale = sideLength > 1e-6f ? halfWidth / sideLength : 0.0f;
            const D3DCOLOR color =
                point == POINT_COUNT - 1 ? (lineColor & 0x00FFFFFF) : lineColor; // fades out at the far end
            ribbon[point * 2] = {
                position[0] + side[0] * sideScale,
                position[1] + side[1] * sideScale,
                position[2] + side[2] * sideScale,
                color};
            ribbon[point * 2 + 1] = {
                position[0] - side[0] * sideScale,
                position[1] - side[1] * sideScale,
                position[2] - side[2] * sideScale,
                color};
        }
    }
    static const D3DMATRIX IDENTITY = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}};
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_WORLD, &IDENTITY);
    d3dDevice->lpVtbl->SetFVF(d3dDevice, D3DFVF_XYZ | D3DFVF_DIFFUSE);
    d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, nullptr);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LIGHTING, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_FOGENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHATESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_STENCILENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_COLORWRITEENABLE, 0xF);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    // The first stretch (to the second point, 0.5 m) is drawn over the scene like the weapon itself, which the
    // first-person pass draws without the scene's depth: tested against the scene, grass and ground near the hand hid
    // the line's start, so it seemed to begin centimetres ahead of the droid's barrels (PLAN 31, inferred from
    // run170's pictures). The rest is tested against the scene, so it still ends on what the weapon points at.
    HRESULT result = D3D_OK;
    for (int line = 0; line < (isPeriscope ? 0 : lineCount); ++line)
    {
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_FALSE);
        d3dDevice->lpVtbl->DrawPrimitiveUP(d3dDevice, D3DPT_TRIANGLESTRIP, 2, ribbons[line], sizeof(Vertex));
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_TRUE);
        result = d3dDevice->lpVtbl->DrawPrimitiveUP(
            d3dDevice,
            D3DPT_TRIANGLESTRIP,
            POINT_COUNT * 2 - 4,
            ribbons[line] + 2,
            sizeof(Vertex));
    }
    if (isPeriscope)
    {
        DrawPeriscopeReticle(d3dDevice, lines[0], relation);
    }
    _savedState->lpVtbl->Apply(_savedState);
    unsigned& drawCount = isSeatWithGuns ? _seatDrawCount : _drawCount;
    if (++drawCount == 1)
    {
        Log::Info(
            "sight",
            "first %s drawn (result 0x%lx): %d from %.2f %.2f %.2f",
            isSeatWithGuns ? "seat aiming line" : "aiming line",
            static_cast<unsigned long>(result),
            lineCount,
            static_cast<double>(lines[0][0]),
            static_cast<double>(lines[0][1]),
            static_cast<double>(lines[0][2]));
    }
}

} // namespace Sight

} // namespace SwbfVr
