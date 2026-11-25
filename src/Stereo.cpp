// Stereo.cpp: stereo rendering.
//
// How the game draws a frame (found with probes, see docs/offsets.md): the per-frame function calls
// Update(dt) and then Render(), a plain function with no arguments at RVA 0x295340. Render() draws every
// pass from the engine camera (a RedCamera) and ends by calling Present.
//
// So stereo is: call the game's Render() twice per frame, once per eye, with the camera moved for each eye.
// Two outputs:
//  - Headset (OpenXR, Xr.cpp): the camera follows the headset, each eye's finished image is copied out of the
//    back buffer and handed to the headset. The monitor shows the right eye.
//  - Desktop test ("stereo <mm>"): both images side by side in the window, no headset needed.
// Anything the game presents without a 3D scene (menus, loading screens) goes to the headset as a flat virtual screen.
//
// Rule learned the hard way: the camera must not change between Update() and Render() in ways the game does not
// expect. So the headset's field of view is applied where the game sets its own (SetFov, during Update), and the
// head pose is applied where the game sets its camera (SetTransform, during Update). Render() then only moves the
// camera by the small difference between the head pose at Update and each eye's latest pose.
//
// The hooks are MinHook detours: MinHook patches the start of a game function with a jump to ours and gives back a
// pointer ("original") through which the game's own code can still be called. The game's methods use __thiscall
// (the object in ECX); our detours are declared __fastcall with an unused second parameter (EDX) to receive it.

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable (device->lpVtbl->Method(device,
// ...)), the form the hooks in D3dSurvey.cpp use. Has to be a macro: d3d9.h reads it.
#define CINTERFACE

#include "Stereo.h"

#include "Aim.h"
#include "BodyArm.h"
#include "PlayerBody.h"
#include "ControllerHolo.h"
#include "CockpitShell.h"
#include "Controls.h"
#include "D3dSurvey.h"
#include "EngineSurvey.h"
#include "Foliage.h"
#if SWBF_VR_DEV
#include "FoliageProbe.h" // test: per-Render counts of the prop draw path
#include "RenderSampler.h" // test: samples the render thread in long eye renders
#endif
#include "FpGuard.h"
#include "GameBuild.h"
#include "GpuTransfer.h"
#include "GunnerSeat.h"
#include "Handlebars.h"
#include "Holster.h"
#include "Hud.h"
#include "Keyboard.h"
#include "HudDraws.h"
#include "Levers.h"
#include "ReloadZone.h"
#include "Limits.h"
#include "Log.h"
#include "Main.h"
#include "Settings.h"
#include "LockOn.h"
#include "Sight.h"
#include "Textures.h"
#include "Vehicle.h"
#include "Xr.h"
#include "Zoom.h"
#include "engine.h"

#include <windows.h>
#include <d3d9.h>

#include <MinHook.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace SwbfVr
{

namespace Stereo
{

namespace
{

// ---- Engine addresses ----
// The functions hooked here (Render, RedCamera::SetTransform, SetFov, SetNearFar), the render view list and the
// callers of the camera come from GameBuild (Feature::Stereo and Feature::CameraCallers; docs/offsets.md,
// "Functions", "Render view list", "Clip distances and field of view", "Scope reticle, spawn screen, jetpack gauge").
// Render() loops over a list of views (more than one in split screen): per view the camera (RedCamera* [4]; view 0
// equals CameraManager + OFFSET_cameraManagerRedCamera), their number (uint32) and whether it shows the 3D scene
// (uint8 [4], zero in menus). Callers of SetTransform whose return address lies in the chase camera's code are the
// player's own first or third person view; any other caller is a scripted or menu camera (observed in the caller
// log). The spawn screen's camera has its own return address (playing is +0x13F4C2 on Steam; vehicles unknown).
// SetNearFar is called by Render() once for the scene pass and once for the first-person pass (near distance 0.3f).
// Inside RedCamera (OFFSET_cameraMatrix, the camera-to-world matrix, is in engine.h):
/// RedCamera: start of the block logged once around the field of view (+0x80 to +0xCF).
constexpr uintptr_t OFFSET_cameraFovBlock = 0x80;
/// RedCamera: tan(half horizontal fov), tan(half vertical fov) as set by SetFov.
constexpr uintptr_t OFFSET_cameraTan = 0x98;
/// RedCamera: the two tangents after the zoom factor: what the projection really uses.
constexpr uintptr_t OFFSET_cameraZoomedTan = 0xA4;

// ---- Constants ----

/// Test ("vr test <bits>"): parts of the headset path switched off.
/// The game camera is not levelled.
/// The scope window's distance in its frame (metres); with the frame on each eye it only matters to the clip planes.
constexpr float SCOPE_WINDOW_DISTANCE_METERS = 10.0f;
/// Limits of the lens's half angle tangent as seen from the head (B2): a lens held far off or right at the eye.
constexpr float LENS_HALF_TAN_MINIMUM = 0.01f;
constexpr float LENS_HALF_TAN_MAXIMUM = 1.0f;
constexpr unsigned TEST_OFF_LEVEL_CAMERA = 1;
/// No HUD panel (the HUD pass is drawn as the game draws it).
constexpr unsigned TEST_OFF_HUD_PANEL = 2;
/// The head camera is not given to the game in Update (eyes are still placed in Render).
constexpr unsigned TEST_OFF_HEAD_IN_UPDATE = 4;
/// The forced field of view goes on every camera, not only the scene's.
constexpr unsigned TEST_FORCED_FOV_ON_ALL_CAMERAS = 8;

// HUD panel. The game draws its HUD and in-match menus last, with a projection of their own, over the whole
// picture - in a headset that would put the health bar and the map at the far edges of vision. So for that pass
// the viewport is shrunk to a panel floating 3 m ahead. In the player's own view the panel sits where the game
// aims, which keeps the crosshair on target while the head looks around freely.
/// Distance of the HUD panel from the head.
constexpr float HUD_DISTANCE_METERS = 3.0f;
/// Half width of the HUD panel as tan(angle): about 38 degrees.
constexpr float HUD_HALF_WIDTH_TAN = 0.78f;
/// The statistics after a match: further away and a little smaller than the HUD panel (idren2023 2026-10-14, PLAN 30).
constexpr float STATISTICS_DISTANCE_METERS = 4.5f;
constexpr float STATISTICS_HALF_WIDTH_TAN = 0.66f;
/// The game's fixed HUD projection (m11, m22): a projection with these scales is the HUD pass.
constexpr float HUD_PROJECTION_SCALE[2] = {1.7320508f, 2.3094011f};

/// The flat virtual screen of Xr.cpp (menus, loading): its distance ahead of the resting pose and its width.
constexpr float FLAT_SCREEN_DISTANCE_METERS = 2.0f;
constexpr float FLAT_SCREEN_WIDTH_METERS = 2.4f;

/// Crouch by head height: speed at which the view is raised or lowered toward the wanted lift, and the longest
/// time step taken into account.
constexpr float LIFT_SPEED_METERS_PER_SECOND = 2.5f;
constexpr float LIFT_LONGEST_STEP_SECONDS = 0.1f;

/// Most images one grab keeps in memory.
constexpr unsigned MAXIMUM_GRAB_IMAGES = 128;

// ---- State ----

/// void __cdecl Render().
using RenderFunction = void(__cdecl*)();
/// void __thiscall RedCamera::SetTransform(const float matrix[16]).
using SetTransformFunction = void(__thiscall*)(void* self, const float* matrix);
/// void __thiscall RedCamera::SetFov(float fov, float heightOverWidth); SetNearFar has the same shape (thiscall with
/// two floats).
using SetFovFunction = void(__thiscall*)(void* self, float fov, float aspect);
// The game's functions behind our hooks, called through MinHook's trampolines.
RenderFunction _originalRender = nullptr;
SetTransformFunction _originalSetTransform = nullptr;
SetFovFunction _originalSetFov = nullptr;
SetFovFunction _originalSetNearFar = nullptr;

bool _isInstalled = false;
uintptr_t _exeBase = 0;
/// A return address that no call has: used for the camera callers when the game build lacks them, so that the
/// comparisons below never match.
constexpr uintptr_t NO_CALLER = ~static_cast<uintptr_t>(0);
// From GameBuild, set by Install: return addresses (RVAs) of the game's calls and the chase camera's code range.
uintptr_t _sceneNearFarReturn = NO_CALLER, _sceneAfterFarNearFarReturn = NO_CALLER,
          _firstPersonNearFarReturn = NO_CALLER, _skyNearFarReturn = NO_CALLER;
uintptr_t _spawnScreenCameraReturn = NO_CALLER;
uintptr_t _chaseCameraCodeStart = NO_CALLER, _chaseCameraCodeEnd = NO_CALLER;
// The render view list (absolute addresses), set by Install.
const uint32_t* _viewCount = nullptr;
const uint8_t* _viewIs3d = nullptr;
void* const* _viewCameras = nullptr;
/// Desktop side-by-side test: half the eye distance; 0 = off. volatile LONG: set by the script with Interlocked
/// calls (atomic, like C#'s Interlocked).
volatile LONG _desktopHalfEyeDistanceMicrons = 0;
int _currentEye = -1;               // which eye the current Render() call is for; -1 outside our double render
bool _isHeadsetPass = false;        // the current double render is for the headset (not the desktop test)
bool _isFlatFrameSubmitted = false; // a headset frame was already submitted for this game frame
bool _isInRender = false;           // inside the game's Render()
float _lastPlayView[16];       // the game camera last set by the chase camera (the player's own view), for respawn
bool _hasLastPlayView = false;
void* _sceneCamera = nullptr;       // camera of the first view while it shows the 3D scene, else null
float _desktopBaseCamera[16];       // desktop test: the camera Update left, restored after both eyes
bool _isEyeImageSubmitted[2];       // the headset accepted this frame's image of each eye
bool _isLeftOnCard = false;         // this frame's left eye went to the headset on the graphics card (GpuTransfer)
bool _isLeftCopied = false;         // this frame's left eye is in _eyeSurfaces[0] (read back after the right eye)
uint32_t _frameCount = 0;

// Field of view forced on the engine camera: the headset's, or a test value from the script ("fov"). 0 = the game's
// own.
float _forcedTanX = 0, _forcedTanY = 0, _forcedFovRadians = 0, _forcedHeightOverWidth = 0;
bool _isFovForcedByScript = false;

// Headset state.
float _headPose[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // head (between the eyes) relative to its
                                                                        // resting pose, latest known
float _eyePoses[2][16];             // eye poses relative to the resting pose, for the frame being drawn
float _gameCamera[16];              // the camera the game asked for in this frame's Update
float _levelCamera[16];             // ... with pitch and roll removed: where the resting head is
bool _didUpdateWithHead = false;    // this frame's Update ran with the head camera
bool _isPlayerViewCamera = false;   // ... and that camera is the player's own view (it follows the aim), not a fly-by
uintptr_t _cameraCaller = 0;        // return address (RVA) of the latest SetTransform call on the scene camera
volatile LONG _nearFarLogsLeft = 0; // SetNearFar calls still to log ("near log <n>")
bool _isFirstPersonPass = false;    // between the first-person pass's SetNearFar and the HUD pass (or the next scene)
// Held view heading (b141, PLAN 21; headset run 12: turning the view with the character was nauseating). While the
// player is on foot with controllers (SetViewFree), the level camera keeps a heading of its own: the character turns
// under it (Controls turns it toward the left controller) and only TurnView (snap / smooth turn) turns the view.
// Otherwise the level camera takes the game's heading, as before.
bool _isViewFreeWanted = false;              // set by Controls at Present, read in Update
bool _isViewHeld = false;                    // the level camera has its own heading this frame
float _viewForwardX = 0, _viewForwardZ = -1; // that heading: forward in the world's horizontal plane (unit)
float _pendingViewTurnRadians = 0;           // TurnView not yet applied; main thread
float _gameYawFromViewRadians = 0;           // the character's heading relative to the held view, to the right positive
unsigned _viewRecenterCount = 0;             // Xr::RecenterCount when the view heading was last looked at

/// Where the HUD panel lies in one eye's image.
struct HudPlacement
{
    float x, y;  // panel centre in the eye image, -1..1
    float scale; // size factor (1 at the panel's own distance)
    bool isValid;
};

HudPlacement _hudPlacements[2];

bool _isHudViewportSet = false;
float _eyeCameras[2][16], _hudFrame[16]; // this frame's eye cameras and the frame the HUD panel hangs in (world)
float _headWorldPosition[3];             // the point between the eyes (world)
bool _isHudWhole = false;                // this frame shows the whole HUD as one panel (menu, spawn screen)
/// The spawn screen's panel frame: the head's level heading when the spawn screen came up (headset run 18: after a
/// death the menu hung ahead of the camera the player died with, not ahead of the head); valid while it stays up.
float _spawnPanelFrame[16];
bool _hasSpawnPanelFrame = false;
// Scope view while zoomed (docs/ZOOM.md section 3, B1): one more Render() per frame from the scope's camera, copied
// into Hud's scope texture and shown in each eye as a round window.
bool _isScopePass = false;     // inside that extra Render()
bool _isScopeCaptured = false; // its picture is in the scope texture
// Whole view magnified (lensless weapon, ZOOM.md B3): the field of view of the eye passes' scene, 0 when not. The
// headset still gets its own field of view, so the picture is magnified; the first-person pass goes back to the
// headset's field of view (SetNearFarHook), so arms and weapon keep their size.
float _fullViewFovRadians = 0;
bool _hasScopeView = false; // this frame has a scope picture to show
float _scopeFrame[16];      // the scope's camera (world); the window hangs along its forward axis
float _scopeHalfTan = 0;    // tan of half the window's angle, or of the lens's as seen from the head
bool _isLensView = false;   // this frame's scope picture is for the weapon's lens (B2), not the window
bool _isPeriscope = false;  // this frame's scope picture is the speeder gunner's periscope (GunnerSeat)
GunnerSeat::Periscope _periscope = {};
D3DVIEWPORT9 _fullViewport{};
float _passTanX = 1, _passTanY = 1;
uint32_t _headsetFrameCount = 0, _notReadyFrameCount = 0;
DWORD _rateStartTick = 0;
uint32_t _rateFrameCount = 0;

IDirect3DTexture9* _eyeTextures[2] = {};
IDirect3DSurface9* _eyeSurfaces[2] = {};
IDirect3DSurface9* _readbackSurface = nullptr; // system-memory copy of the back buffer for the headset
UINT _eyeTargetWidth = 0, _eyeTargetHeight = 0, _readbackWidth = 0, _readbackHeight = 0;

// result = a * b for 4x4 row matrices (points are row vectors: p' = p * a * b).
void MultiplyMatrices(float* result, const float* a, const float* b)
{
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            result[row * 4 + column] = a[row * 4] * b[column] + a[row * 4 + 1] * b[4 + column] +
                                       a[row * 4 + 2] * b[8 + column] + a[row * 4 + 3] * b[12 + column];
        }
    }
}

/// Test bits, TEST_OFF_... and TEST_FORCED_FOV_ON_ALL_CAMERAS.
unsigned _testPartsOff = 0;

// In the headset only the player's head may tilt the view. The game camera's pitch (mouse up/down) is removed
// so the horizon stays level; its heading and position are kept. The weapon still follows the game's aim.
/// Walker bob damping: the eye's height smoothed with this time constant; a jump larger than this resets it.
/// Off since b508: idren2023 saw b507 (run588) as "way worse" (to be understood before it comes back).
constexpr bool IS_WALKER_BOB_DAMPED = false;
constexpr float WALKER_BOB_SECONDS = 0.5f, WALKER_BOB_RESET_METERS = 2.0f; // two stages: as a well sprung seat

void LevelCamera(float* matrix)
{
    float backX = matrix[8], backZ = matrix[10];
    const float length = sqrtf(backX * backX + backZ * backZ);
    if (length < 1e-4f)
    {
        return; // looking straight up or down: leave as is
    }
    backX /= length;
    backZ /= length;
    matrix[0] = backZ;
    matrix[1] = 0.0f;
    matrix[2] = -backX; // right = up x back
    matrix[4] = 0.0f;
    matrix[5] = 1.0f;
    matrix[6] = 0.0f; // up
    matrix[8] = backX;
    matrix[9] = 0.0f;
    matrix[10] = backZ; // back
}

// Gives the level camera its held heading (see _isViewHeld), or takes the game's and lets go. A recentre lets go
// for one Update, so the view faces the character's heading again. Update, FpGuard held.
void HoldViewHeading()
{
    const float gameForwardX = -_levelCamera[8], gameForwardZ = -_levelCamera[10];
    const unsigned recenterCount = Xr::RecenterCount();
    const bool isRecentred = recenterCount != _viewRecenterCount;
    _viewRecenterCount = recenterCount;
    if (!_isViewFreeWanted || !_isPlayerViewCamera || _cameraCaller == _spawnScreenCameraReturn || isRecentred ||
        gameForwardX * gameForwardX + gameForwardZ * gameForwardZ < 0.5f)
    {
        if (_isViewHeld)
        {
            Log::Info("stereo", "view heading follows the character again");
        }
        _isViewHeld = false;
        _pendingViewTurnRadians = 0;
        _gameYawFromViewRadians = 0;
        return;
    }
    if (!_isViewHeld)
    {
        _isViewHeld = true;
        _viewForwardX = gameForwardX;
        _viewForwardZ = gameForwardZ;
        Log::Info("stereo", "view heading held (the character turns under it)");
    }
    if (_pendingViewTurnRadians != 0)
    {
        // Right of forward (x, z) is (-z, x); turning right by a: forward * cos a + right * sin a.
        const float cosine = cosf(_pendingViewTurnRadians), sine = sinf(_pendingViewTurnRadians);
        const float x = _viewForwardX * cosine - _viewForwardZ * sine,
                    z = _viewForwardZ * cosine + _viewForwardX * sine;
        const float length = sqrtf(x * x + z * z);
        _viewForwardX = x / length;
        _viewForwardZ = z / length;
        _pendingViewTurnRadians = 0;
    }
    _gameYawFromViewRadians = atan2f(
        gameForwardX * -_viewForwardZ + gameForwardZ * _viewForwardX,
        gameForwardX * _viewForwardX + gameForwardZ * _viewForwardZ);
    _levelCamera[0] = -_viewForwardZ;
    _levelCamera[1] = 0.0f;
    _levelCamera[2] = _viewForwardX; // right
    _levelCamera[8] = -_viewForwardX;
    _levelCamera[9] = 0.0f;
    _levelCamera[10] = -_viewForwardZ; // back
}

// In a vehicle seat (PLAN 54) the level camera takes the hull's heading instead of the game camera's: in first
// person the game's camera follows the seat's aim angles, and with the guns following the head (Vehicle) a view that
// turned with them would chase the aim round and round (run218). The gun turns under the view, the view turns with
// the vehicle. In a fighter tank's gunner station the view keeps its own heading (HoldViewHeading, PLAN 58 c: the
// driver turning the hull must not turn the gunner); the eye is still put at the station's. Update, FpGuard held.
void HoldHullHeading()
{
    float forwardX = 0, forwardZ = 0;
    if (!_isPlayerViewCamera || !Xr::IsRunning() || !Vehicle::GetHullForward(&forwardX, &forwardZ))
    {
        return;
    }
    // A building gun along its model's -z (GunnerSeat::GunForwardZ): the view faces the way the gun does at yaw 0. A
    // turret aimed by the hand (the Hoth dish, the Geonosian turret) turns the view with its gun, so that its stock
    // screen stays ahead of a still head.
    forwardX *= GunnerSeat::GunForwardZ();
    forwardZ *= GunnerSeat::GunForwardZ();
    if (GunnerSeat::IsFacingAft()) // the AT-TE's rear gunner (run611; a flyer's is turned below)
    {
        forwardX = -forwardX;
        forwardZ = -forwardZ;
    }
    if (GunnerSeat::IsAimedByHand())
    {
        Vehicle::GetTurretForward(&forwardX, &forwardZ);
    }
    if (!_isViewHeld) // held in a fighter tank's gunner station (PLAN 58 c): only the eye's place below
    {
        _levelCamera[0] = -forwardZ;
        _levelCamera[1] = 0.0f;
        _levelCamera[2] = forwardX; // right
        _levelCamera[8] = -forwardX;
        _levelCamera[9] = 0.0f;
        _levelCamera[10] = -forwardZ; // back
        // A flyer's pilot sits in the cockpit (PLAN 54 d, idren2023: the view with the hull's pitch and roll, the head
        // free): the level camera takes the hull's whole attitude, so the stick, throttle and lever (Flight), which
        // stand in resting-pose space, stay in the cockpit as it pitches and rolls. Up row 1 as the hull's, the rest
        // made orthonormal round its forward.
        float hull[16];
        if ((Vehicle::CurrentSeat() == Vehicle::Seat::Flyer || GunnerSeat::IsFlyerCabinSeat()) && Vehicle::GetHullMatrix(hull))
        {
            const float back[3] = {-hull[8], -hull[9], -hull[10]};
            float right[3] = {
                hull[5] * back[2] - hull[6] * back[1],
                hull[6] * back[0] - hull[4] * back[2],
                hull[4] * back[1] - hull[5] * back[0]};
            const float length = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
            if (length > 0.5f)
            {
                for (float& value : right)
                {
                    value /= length;
                }
                const float up[3] = {
                    back[1] * right[2] - back[2] * right[1],
                    back[2] * right[0] - back[0] * right[2],
                    back[0] * right[1] - back[1] * right[0]};
                // the snowspeeder's tail gunner faces aft (GunnerSeat::IsFacingAft): turned round the hull's up
                const float facing = GunnerSeat::IsFacingAft() ? -1.0f : 1.0f;
                for (int axis = 0; axis < 3; ++axis)
                {
                    _levelCamera[axis] = facing * right[axis];
                    _levelCamera[4 + axis] = up[axis];
                    _levelCamera[8 + axis] = facing * back[axis];
                }
            }
        }
    }
    // On a bike in first person (Handlebars) the resting eye is the rider's in the vehicle's model, which is drawn
    // round it, and in a fighter tank's gunner seat (GunnerSeat) the standing gunner's in our station; the game's
    // camera moves there too, so the HUD panel along its aim stays centred.
    float eye[3];
    if (Handlebars::GetSeatEye(_gameCamera + 12, eye) || GunnerSeat::GetSeatEye(_gameCamera + 12, eye))
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            _levelCamera[12 + axis] = eye[axis];
            _gameCamera[12 + axis] = eye[axis];
        }
    }
}

// The camera of the first view if it is showing the 3D scene right now (in a match), else null (menus, loading).
void* FindSceneCamera()
{
    if (!_viewCount || *_viewCount == 0)
    {
        return nullptr;
    }
    if (*_viewIs3d == 0)
    {
        return nullptr;
    }
    return *_viewCameras;
}

void SetForcedFov(float tanX, float tanY, const char* reason)
{
    if (tanX == _forcedTanX && tanY == _forcedTanY)
    {
        return;
    }
    _forcedTanX = tanX;
    _forcedTanY = tanY;
    _forcedFovRadians = tanX > 0 ? 2.0f * atanf(tanX) : 0.0f;
    _forcedHeightOverWidth = tanX > 0 ? tanY / tanX : 0.0f;
    if (tanX > 0)
    {
        Log::Info(
            "stereo",
            "field of view forced to tan %d.%03d x %d.%03d (half angles), %s",
            static_cast<int>(tanX),
            static_cast<int>(tanX * 1000) % 1000,
            static_cast<int>(tanY),
            static_cast<int>(tanY * 1000) % 1000,
            reason);
    }
    else
    {
        Log::Info("stereo", "field of view back to the game's own");
    }
}

// The game set a projection matrix on the device. During an eye pass this tells the 3D scene from the HUD: for the
// HUD pass the viewport is shrunk to the HUD panel, for anything else it is set back to the whole image.
void SetHudViewport(IDirect3DDevice9* device, bool isHud)
{
    if (isHud == _isHudViewportSet)
    {
        return;
    }
    if (isHud && (_testPartsOff & TEST_OFF_HUD_PANEL))
    {
        return;
    }
    if (!isHud)
    {
        device->lpVtbl->SetViewport(device, &_fullViewport);
        _isHudViewportSet = false;
        return;
    }
    const HudPlacement& placement = _hudPlacements[_currentEye];
    if (!placement.isValid || FAILED(device->lpVtbl->GetViewport(device, &_fullViewport)) || _fullViewport.Width < 16 ||
        _fullViewport.Height < 16)
    {
        return;
    }
    const float width = static_cast<float>(_fullViewport.Width), height = static_cast<float>(_fullViewport.Height);
    float halfWidth = width * 0.5f * HUD_HALF_WIDTH_TAN / _passTanX * placement.scale;
    float halfHeight = height * 0.5f * (HUD_HALF_WIDTH_TAN * height / width) / _passTanY * placement.scale;
    if (halfWidth > width * 0.5f)
    {
        halfWidth = width * 0.5f;
    }
    if (halfHeight > height * 0.5f)
    {
        halfHeight = height * 0.5f;
    }
    float centreX = width * 0.5f * (1.0f + placement.x), centreY = height * 0.5f * (1.0f - placement.y);
    if (!(centreX >= halfWidth))
    {
        centreX = halfWidth; // also catches NaN
    }
    if (centreX > width - halfWidth)
    {
        centreX = width - halfWidth;
    }
    if (!(centreY >= halfHeight))
    {
        centreY = halfHeight;
    }
    if (centreY > height - halfHeight)
    {
        centreY = height - halfHeight;
    }
    D3DVIEWPORT9 viewport = _fullViewport;
    viewport.X = _fullViewport.X + static_cast<DWORD>(centreX - halfWidth);
    viewport.Y = _fullViewport.Y + static_cast<DWORD>(centreY - halfHeight);
    viewport.Width = static_cast<DWORD>(halfWidth * 2.0f);
    viewport.Height = static_cast<DWORD>(halfHeight * 2.0f);
    if (viewport.Width < 8 || viewport.Height < 8)
    {
        return;
    }
    if (SUCCEEDED(device->lpVtbl->SetViewport(device, &viewport)))
    {
        _isHudViewportSet = true;
    }
}

// Remembers the latest head pose and the headset's field of view for the next Update. Returns false if the headset
// has not reported them yet. Caller holds an FpGuard.
bool RefreshHead()
{
    float tanX = 0, tanY = 0;
    if (!Xr::GetViews(_eyePoses[0], _eyePoses[1], &tanX, &tanY))
    {
        return false;
    }
    memcpy(_headPose, _eyePoses[0], sizeof(_headPose));
    for (int element = 12; element < 15; ++element)
    {
        _headPose[element] = (_eyePoses[0][element] + _eyePoses[1][element]) * 0.5f;
    }
    if (!_isFovForcedByScript)
    {
        SetForcedFov(tanX, tanY, "from the headset");
    }
    return true;
}

float _sceneNearMeters = 0.15f;       // near clip distance of the scene while VR runs; 0 = leave the game's (0.5 m)
// the same for the first-person arms and weapon; 0 = leave the game's (0.3 m). 1 mm (idren2023 2026-11-22: the weapon
// still clipped at the face with 3 cm; run 858: whole at 1 mm, no depth fighting: that pass's far is 165 m)
float _firstPersonNearMeters = 0.001f;
// The sky pass's near while VR runs (the game's is 2): the domes must stay whole out to the view's corners, about 62
// deg off axis with the headset's view (b658).
constexpr float SKY_NEAR_METERS = 0.5f;
float _liftMeters = 0, _liftTargetMeters = 0; // how far the base frame is raised (SetLift)
DWORD _liftTick = 0;
float _eyePositions[2][3];                 // world position of each eye of the frame being drawn (for Sight.cpp)
float _headWorld[16];                      // the left eye's axes at the point between both eyes, this frame (Body.cpp)
/// The head's turn relative to the level camera, followed with a lag (GetSmoothedHeadWorld): rows right, up, back.
float _smoothHeadTurn[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
DWORD _smoothHeadTick = 0;
float _smoothHeadWorld[16];
/// The lag's time constant: a HUD on the head (the droid's) then sits still in the world through small head moves,
/// where the compositor's reprojection made a strictly head-locked one judder (headset run 18).
constexpr float SMOOTH_HEAD_SECONDS = 0.10f;
float _backBufferHeightOverWidth = 0.625f; // of the back buffer (1200 / 1920); refreshed at Present
volatile LONG _cameraLogCount = 0;         // test ("camlog <n>"): log the next n camera matrices the game sets

// Hook of RedCamera::SetTransform. The game sets its camera here, during Update (and in rare cases inside Render).
// Called by the game (thiscall: `self` arrives in ECX, the unused `edx` parameter takes EDX).
void __fastcall SetTransformHook(void* self, void* /*edx*/, const float* matrix)
{
    if (_cameraLogCount > 0 && self == _sceneCamera)
    {
        InterlockedDecrement(&_cameraLogCount);
        FpGuard fp;
        const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - _exeBase;
        Log::Info(
            "cam",
            "frame %u from +0x%x %s pos %.5f %.5f %.5f back %.6f %.6f %.6f",
            _frameCount,
            static_cast<unsigned>(caller),
            _isInRender ? "render" : "update",
            static_cast<double>(matrix[12]),
            static_cast<double>(matrix[13]),
            static_cast<double>(matrix[14]),
            static_cast<double>(matrix[8]),
            static_cast<double>(matrix[9]),
            static_cast<double>(matrix[10]));
    }
    if (self == _sceneCamera && !_isInRender && Xr::IsRunning())
    {
        // Update, headset on: the game continues with the head's camera, so everything it prepares for drawing
        // (visibility, level of detail, sound, markers) is prepared for what the player really looks at.
        float headCamera[16];
        // Who set the camera: the chase camera (the player's own first or third person view) or something else.
        const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - _exeBase;
        _isPlayerViewCamera = caller >= _chaseCameraCodeStart && caller < _chaseCameraCodeEnd;
        // Respawn (PLAN 58 p, headset run 17): the spawn screen after a death keeps the view where the player was (no
        // jump to the map's spawn-screen cameras, often behind a wall); its menu hangs ahead of that view. At a match's
        // first spawn there is no such view yet (cleared while no scene is shown).
        // The spawn screen's caller lies in the chase camera's code too (offsets: it shows ChaseCamera+0x10, the
        // third-person place behind the soldier; run370), so it is looked at first.
        if (caller == _spawnScreenCameraReturn)
        {
            if (_hasLastPlayView)
            {
                matrix = _lastPlayView;
            }
        }
        else if (_isPlayerViewCamera)
        {
            memcpy(_lastPlayView, matrix, sizeof(_lastPlayView));
            _hasLastPlayView = true;
        }
        {
            FpGuard fp; // our float maths must not disturb the game's floating-point state
            memcpy(_gameCamera, matrix, sizeof(_gameCamera));
            memcpy(_levelCamera, matrix, sizeof(_levelCamera));
            if (!(_testPartsOff & TEST_OFF_LEVEL_CAMERA))
            {
                LevelCamera(_levelCamera);
                HoldViewHeading();
                HoldHullHeading();
            }
            { // A mount's view at the rider's eye (the game keeps its chase camera behind the animal)
                float riderEye[3];
                if (_isPlayerViewCamera && (Handlebars::GetMountEye(riderEye) || GunnerSeat::GetForcedEye(riderEye)))
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        _levelCamera[12 + axis] = riderEye[axis];
                        _gameCamera[12 + axis] = riderEye[axis];
                    }
                }
            }
            { // Crouch by head height: the game lowers its camera for the crouch, the player has lowered the head
                // already; the difference is given back here (view, hands and HUD frame alike), eased.
                const DWORD now = GetTickCount();
                const float elapsedSeconds = _liftTick ? static_cast<float>(now - _liftTick) * 0.001f : 0.0f;
                _liftTick = now;
                const float step =
                    LIFT_SPEED_METERS_PER_SECOND *
                    (elapsedSeconds > LIFT_LONGEST_STEP_SECONDS ? LIFT_LONGEST_STEP_SECONDS : elapsedSeconds);
                if (_liftMeters < _liftTargetMeters)
                {
                    _liftMeters += step;
                    if (_liftMeters > _liftTargetMeters)
                    {
                        _liftMeters = _liftTargetMeters;
                    }
                }
                else if (_liftMeters > _liftTargetMeters)
                {
                    _liftMeters -= step;
                    if (_liftMeters < _liftTargetMeters)
                    {
                        _liftMeters = _liftTargetMeters;
                    }
                }
                _levelCamera[13] += _liftMeters;
                _gameCamera[13] += _liftMeters;
            }
            { // Walker bob damped (idren2023 2026-11-04: the stride's bob would be jarring in VR, "as if the pilot seats
                // have really good shock absorption"): in a walker's seat the eye's height follows the camera's through
                // two low-pass stages (WALKER_BOB_SECONDS each: the AT-ST's ~1 Hz step kept to ~10 %, the AT-AT's ~0.5 Hz
                // to ~30 %), while climbs and slopes still come through; the cockpit, drawn at the game camera, keeps
                // with the eye (a still cockpit is the comfortable anchor).
                static float smoothY = 0, stageY = 0;
                static bool hasSmooth = false;
                static DWORD lastTick = 0;
                const DWORD now = GetTickCount();
                const float seconds = lastTick ? static_cast<float>(now - lastTick) * 0.001f : 0.0f;
                lastTick = now;
                const float y = _levelCamera[13];
                if (IS_WALKER_BOB_DAMPED && Vehicle::CurrentSeat() == Vehicle::Seat::Walker && Xr::IsRunning() &&
                    _isPlayerViewCamera)
                {
                    if (!hasSmooth || fabsf(y - smoothY) > WALKER_BOB_RESET_METERS || seconds > 0.5f)
                    {
                        smoothY = stageY = y;
                        hasSmooth = true;
                    }
                    const float share = 1.0f - expf(-seconds / WALKER_BOB_SECONDS);
                    stageY += (y - stageY) * share;
                    smoothY += (stageY - smoothY) * share;
                    static DWORD logTick = 0;
                    static float lowY = 1e9f, highY = -1e9f, lowSmooth = 1e9f, highSmooth = -1e9f;
                    lowY = y < lowY ? y : lowY;
                    highY = y > highY ? y : highY;
                    lowSmooth = smoothY < lowSmooth ? smoothY : lowSmooth;
                    highSmooth = smoothY > highSmooth ? smoothY : highSmooth;
                    if (now - logTick >= 1000)
                    {
                        if (logTick)
                        {
                            Log::Info("stereo", "walker bob: eye height range %.2f m in the game, %.2f m damped (last second)",
                                      static_cast<double>(highY - lowY), static_cast<double>(highSmooth - lowSmooth));
                        }
                        logTick = now;
                        lowY = lowSmooth = 1e9f;
                        highY = highSmooth = -1e9f;
                    }
                    _levelCamera[13] = smoothY;
                    _gameCamera[13] += smoothY - y;
                }
                else
                {
                    hasSmooth = false;
                }
            }
            MultiplyMatrices(headCamera, _headPose, _levelCamera);
        }
        _didUpdateWithHead = true;
        if (_testPartsOff & TEST_OFF_HEAD_IN_UPDATE)
        {
            _originalSetTransform(self, matrix);
            return;
        }
        if (caller != _cameraCaller)
        {
            _cameraCaller = caller;
            static unsigned loggedCount = 0;
            if (loggedCount++ < 20)
            {
                Log::Info(
                    "stereo",
                    "camera now set from +0x%x (%s)",
                    static_cast<unsigned>(caller),
                    _isPlayerViewCamera ? "player view" : "other view");
            }
        }
        _originalSetTransform(self, headCamera);
        return;
    }
    // Inside Render the game only ever re-sets the camera from the camera's own current matrix, which already
    // is the eye's. Pass it through.
    _originalSetTransform(self, matrix);
}

// Hook of RedCamera::SetFov (thiscall, see SetTransformHook). The game sets its field of view here, once per Update.
// A forced field of view replaces it at this point, so Update and Render always agree.
void __fastcall SetFovHook(void* self, void* /*edx*/, float fov, float aspect)
{
    { // Test: who sets which field of view on which camera (first few different ones).

        static struct
        {
            void* self;
            uintptr_t caller;
            float fov;
        } seen[24];

        static unsigned seenCount = 0;
        const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - _exeBase;
        bool isKnown = false;
        for (unsigned index = 0; index < seenCount; ++index)
        {
            if (seen[index].self == self && seen[index].caller == caller && seen[index].fov == fov)
            {
                isKnown = true;
            }
        }
        if (!isKnown && seenCount < 24)
        {
            seen[seenCount++] = {self, caller, fov};
            FpGuard fp;
            Log::Info(
                "stereo",
                "SetFov on camera %p (%s) from +0x%x: fov %.4f aspect %.4f, %s, frame %u",
                self,
                self == _sceneCamera ? "scene" : "other",
                static_cast<unsigned>(caller),
                static_cast<double>(fov),
                static_cast<double>(aspect),
                _isInRender ? "render" : "update",
                _frameCount);
        }
    }
    if (_forcedFovRadians > 0 && (self == _sceneCamera || (_testPartsOff & TEST_FORCED_FOV_ON_ALL_CAMERAS)))
    {
        _originalSetFov(self, _forcedFovRadians, _forcedHeightOverWidth);
    }
    else
    {
        _originalSetFov(self, fov, aspect);
    }
}

// Hook of RedCamera::SetNearFar (thiscall, see SetTransformHook). Render() sets the camera's clip distances for each
// pass with it: the scene (0.5 m near on Naboo, call at +0x2954F5), the sky (2.0), and the pass that draws the
// first-person arms and weapon (0.3 m, a constant at +0x29571B). On Naboo in run206 the scene came from the second
// scene call +0x295D98 (after the distant-terrain pass at 150 m) and +0x2954F5 did not run: both count as the scene.
// 0.3 m cut the arms and the weapon off a forearm's length from the face (idren2023, headset run 5). While VR runs both
// distances are moved in; the far distances stay.
void __fastcall SetNearFarHook(void* self, void* /*edx*/, float nearDistance, float farDistance)
{
    {
        const uintptr_t passCaller = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - _exeBase;
        if (passCaller == _firstPersonNearFarReturn)
        {
            _isFirstPersonPass = true;
            if (Aim::IsMirrorDrawn())
            {
                if (IDirect3DDevice9* device = static_cast<IDirect3DDevice9*>(D3dSurvey::PresentingDevice()))
                {
                    device->lpVtbl->SetRenderState(device, D3DRS_CULLMODE, D3DCULL_NONE);
                }
            }
        }
        else if (passCaller == _sceneNearFarReturn || passCaller == _sceneAfterFarNearFarReturn)
        {
            _isFirstPersonPass = false;
        }
    }
    if (Xr::IsRunning() && _isInRender)
    {
        const uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - _exeBase;
        FpGuard fp;
        float wantedNear = nearDistance;
        if (caller == _firstPersonNearFarReturn && _isScopePass && !_isPeriscope)
        {
            LockOn::DrawInScope(D3dSurvey::PresentingDevice()); // the lens's picture: the rocket lock-on display
        }
        if (caller == _firstPersonNearFarReturn && (_currentEye >= 0 || (_isScopePass && _isPeriscope)))
        {
            Sight::Draw(D3dSurvey::PresentingDevice()); // the scene is complete, the first-person pass begins
            if (_fullViewFovRadians > 0)
            {
                _originalSetFov(self, _forcedFovRadians, _forcedHeightOverWidth); // arms and weapon unmagnified
            }
        }
        if (caller == _firstPersonNearFarReturn && _firstPersonNearMeters > 0)
        {
            wantedNear = _firstPersonNearMeters;
        }
        else if (caller == _skyNearFarReturn && nearDistance > SKY_NEAR_METERS)
        {
            // The sky domes are drawn round the eye (Kashyyyk Docks' mountain ring 2.9 units out): the game's near of
            // 2 cut the ring 46 deg off the view axis, inside the headset's wide view (run 873; flat views reach 35).
            wantedNear = SKY_NEAR_METERS;
        }
        else if (
            (caller == _sceneNearFarReturn || caller == _sceneAfterFarNearFarReturn) && _sceneNearMeters > 0 &&
            _sceneNearMeters < nearDistance)
        {
            wantedNear = _sceneNearMeters;
        }
        if (wantedNear != nearDistance)
        {
            static unsigned loggedCount = 0;
            if (loggedCount < 4)
            {
                ++loggedCount;
                Log::Info(
                    "stereo",
                    "near clip distance %.2f m -> %.2f m (pass at +0x%x)",
                    static_cast<double>(nearDistance),
                    static_cast<double>(wantedNear),
                    static_cast<unsigned>(caller));
            }
            nearDistance = wantedNear;
        }
    }
    if (_nearFarLogsLeft > 0 && InterlockedDecrement(&_nearFarLogsLeft) >= 0)
    {
        Log::Info(
            "stereo",
            "SetNearFar from +0x%x: near %.3f far %.1f, eye %d, in render %d",
            static_cast<unsigned>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)) - _exeBase),
            static_cast<double>(nearDistance),
            static_cast<double>(farDistance),
            _currentEye,
            _isInRender ? 1 : 0);
    }
    _originalSetNearFar(self, nearDistance, farDistance);
}

// The scope's camera while zoomed: on the line the shots take (Aim's fire line: the head's line with head aim, the
// weapon's without), turned about that line as the head is, or as the weapon is for a picture on its lens (upHint:
// the weapon's up axis), so the horizon stays level when the weapon is tilted. Without a fire line: the head.
void ComputeScopeCamera(float* scopeCamera, const float* upHint)
{
    MultiplyMatrices(scopeCamera, _headPose, _levelCamera);
    float from[3], forward[3];
    if (!Aim::GetFireLine(from, forward))
    {
        return;
    }
    const float up[3] = {
        upHint ? upHint[0] : scopeCamera[4],
        upHint ? upHint[1] : scopeCamera[5],
        upHint ? upHint[2] : scopeCamera[6]};
    float right[3] = {
        forward[1] * up[2] - forward[2] * up[1],
        forward[2] * up[0] - forward[0] * up[2],
        forward[0] * up[1] - forward[1] * up[0]};
    const float length = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (length < 1e-3f)
    {
        return; // looking straight along the head's up axis: keep the head
    }
    for (float& value : right)
    {
        value /= length;
    }
    const float newUp[3] = {
        right[1] * forward[2] - right[2] * forward[1],
        right[2] * forward[0] - right[0] * forward[2],
        right[0] * forward[1] - right[1] * forward[0]};
    for (int axis = 0; axis < 3; ++axis)
    {
        scopeCamera[axis] = right[axis];
        scopeCamera[4 + axis] = newUp[axis];
        scopeCamera[8 + axis] = -forward[axis];
        scopeCamera[12 + axis] = from[axis];
    }
}

// At the end of an eye pass while zoomed: the projected scope picture over the weapon (B2; b155: lit from the weapon's
// lenses), seen from behind only (the eye on the eyepiece's side), or else as the window at infinity in front of the
// aim.
void DrawScope(IDirect3DDevice9* d3dDevice)
{
    const float* eye = _eyeCameras[_currentEye];
    if (_isPeriscope)
    { // the shell draws the periscope's picture on its display
        return;
    }
    Zoom::Lens lenses[Zoom::MAXIMUM_LENS_COUNT];
    float weaponWorld[16];
    const int lensCount = _isLensView ? Zoom::GetLenses(lenses) : 0;
    if (lensCount > 0 && Aim::GetWeaponWorld(weaponWorld))
    {
        for (int index = 0; index < lensCount; ++index)
        {
            float frame[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            float behind = 0;
            {
                FpGuard fp;
                // The weapon's rows are model x, y, z (z forward); the lens frame looks along the weapon from behind:
                // right = -x, up = y, back = -z, at the lens centre.
                for (int axis = 0; axis < 3; ++axis)
                {
                    frame[axis] = -weaponWorld[axis];
                    frame[4 + axis] = weaponWorld[4 + axis];
                    frame[8 + axis] = -weaponWorld[8 + axis];
                    frame[12 + axis] = lenses[index].centre[0] * weaponWorld[axis] +
                                       lenses[index].centre[1] * weaponWorld[4 + axis] +
                                       lenses[index].centre[2] * weaponWorld[8 + axis] + weaponWorld[12 + axis];
                    behind += (eye[12 + axis] - frame[12 + axis]) * frame[8 + axis];
                }
            }
            float emitters[Zoom::MAXIMUM_EMITTER_COUNT][3];
            {
                FpGuard fp;
                for (int source = 0; source < lenses[index].emitterCount; ++source)
                {
                    const float* point = lenses[index].emitters[source];
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        emitters[source][axis] = point[0] * weaponWorld[axis] + point[1] * weaponWorld[4 + axis] +
                                                 point[2] * weaponWorld[8 + axis] + weaponWorld[12 + axis];
                    }
                }
            }
            // A picture on the glass is seen by the eye behind the scope only (headset run 15: scopes felt as if seen
            // from the left eye, not along the gun: one flat picture at the lens, seen by the other eye from the side,
            // shows the target beside the gun's line). The eye nearer the lens's axis looks through it; the other
            // sees the lens as the model draws it, as with a real scope. Holoscopes stay visible to both eyes.
            bool isAimingEye = true;
            if (lenses[index].emitterCount == 0 && !lenses[index].isScreen) // a screen: both eyes (Zoom::Lens)
            {
                FpGuard fp;
                float offAxisSquared[2];
                for (int eyeIndex = 0; eyeIndex < 2; ++eyeIndex)
                {
                    float toEye[3], along = 0;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        toEye[axis] = _eyeCameras[eyeIndex][12 + axis] - frame[12 + axis];
                        along += toEye[axis] * frame[8 + axis];
                    }
                    offAxisSquared[eyeIndex] = 0;
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        const float off = toEye[axis] - along * frame[8 + axis];
                        offAxisSquared[eyeIndex] += off * off;
                    }
                }
                const int aimingEye = offAxisSquared[0] <= offAxisSquared[1] ? 0 : 1;
                isAimingEye = _currentEye == aimingEye;
                static int loggedEye = -1;
                if (aimingEye != loggedEye)
                {
                    loggedEye = aimingEye;
                    Log::Info("stereo", "scope picture for the %s eye only", aimingEye ? "right" : "left");
                }
            }
            if (behind > 0 && isAimingEye)
            {
                Hud::DrawScopeWindow(
                    d3dDevice,
                    eye,
                    _passTanX,
                    _passTanY,
                    frame,
                    lenses[index].radiusMeters,
                    0,
                    true,
                    &emitters[0][0],
                    lenses[index].emitterCount,
                    lenses[index].emitterRadiusMeters,
                    &lenses[index].outline[0][0],
                    lenses[index].outlineCount,
                    lenses[index].tiltDegrees);
            }
        }
        return;
    }
    float windowFrame[16];
    memcpy(windowFrame, _scopeFrame, sizeof(windowFrame));
    memcpy(&windowFrame[12], &eye[12], 3 * sizeof(float)); // on the eye's line: at infinity
    Hud::DrawScopeWindow(
        d3dDevice,
        eye,
        _passTanX,
        _passTanY,
        windowFrame,
        SCOPE_WINDOW_DISTANCE_METERS * _scopeHalfTan,
        SCOPE_WINDOW_DISTANCE_METERS,
        false);
}

float PeriscopeHalfTan();
void RenderScopePass(void* camera, float factor, float halfTan, const float* frame);

// While zoomed (or in the speeder gunner's seat, its periscope): the extra Render() of the scope view. The camera gets the scope's pose and a square field of view of
// window angle / zoom factor for this call only, and the headset's field of view back afterwards; the picture is
// taken before the HUD pass (OnProjection) or at its Present, which is then skipped (OnPresent).
void RenderScope(void* camera)
{
    _hasScopeView = false;
    _fullViewFovRadians = 0;
    _isPeriscope = _forcedFovRadians > 0 && GunnerSeat::GetPeriscope(_periscope);
    const float factor = _isPeriscope ? 1.0f : Zoom::StepFactor();
    if (_isPeriscope)
    {
        RenderScopePass(camera, factor, PeriscopeHalfTan(), _periscope.camera);
        return;
    }
    if (!Zoom::IsZoomed() || _forcedFovRadians <= 0 || Holster::HoldsNothing()) // empty hand: no scope (PLAN 27)
    {
        return;
    }
    if (Zoom::IsFullView())
    { // no lens on this weapon: no scope pass, the eye passes are drawn magnified instead (ZOOM.md B3)
        FpGuard fp;
        if (factor > 1.001f)
        {
            _fullViewFovRadians = 2.0f * atanf(tanf(_forcedFovRadians * 0.5f) / factor);
        }
        return;
    }
    float scopeFovRadians = 0;
    {
        FpGuard fp;
        // On the weapon's lens (B2) the picture covers the lens's angle as seen from the head, so the magnification
        // is the zoom factor itself; in the window it covers the window's angle.
        Zoom::Lens lenses[Zoom::MAXIMUM_LENS_COUNT];
        float weaponWorld[16];
        _isLensView = Zoom::GetLenses(lenses) > 0 && Aim::GetWeaponWorld(weaponWorld);
        if (_isLensView)
        {
            float distanceSquared = 0;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float centre = lenses[0].centre[0] * weaponWorld[axis] +
                                     lenses[0].centre[1] * weaponWorld[4 + axis] +
                                     lenses[0].centre[2] * weaponWorld[8 + axis] + weaponWorld[12 + axis];
                distanceSquared += (centre - _headWorldPosition[axis]) * (centre - _headWorldPosition[axis]);
            }
            const float halfTan = lenses[0].radiusMeters / sqrtf(distanceSquared > 1e-6f ? distanceSquared : 1e-6f);
            _scopeHalfTan = halfTan < LENS_HALF_TAN_MINIMUM   ? LENS_HALF_TAN_MINIMUM
                            : halfTan > LENS_HALF_TAN_MAXIMUM ? LENS_HALF_TAN_MAXIMUM
                                                              : halfTan;
            ComputeScopeCamera(_scopeFrame, &weaponWorld[4]);
            // Every 2 s while zoomed on a lens: how far the lens's angle moved (headset run 16: it pumped while
            // walking, the weapon placed a frame before the head).
            static float smallestTan = 1e9f, largestTan = 0;
            static unsigned frameCount = 0;
            static DWORD summaryTick = 0;
            smallestTan = halfTan < smallestTan ? halfTan : smallestTan;
            largestTan = halfTan > largestTan ? halfTan : largestTan;
            ++frameCount;
            const DWORD now = GetTickCount();
            if (!summaryTick)
            {
                summaryTick = now;
            }
            else if (now - summaryTick >= 2000)
            {
                Log::Info(
                    "stereo",
                    "scope: lens half tan %.4f to %.4f over %u frames",
                    static_cast<double>(smallestTan),
                    static_cast<double>(largestTan),
                    frameCount);
                smallestTan = 1e9f;
                largestTan = 0;
                frameCount = 0;
                summaryTick = now;
            }
        }
        else
        {
            _scopeHalfTan = Zoom::WindowHalfTan();
            ComputeScopeCamera(_scopeFrame, nullptr);
        }
        scopeFovRadians = 2.0f * atanf(_scopeHalfTan / factor);
    }
    (void)scopeFovRadians;
    // A homing launcher (LockOn, idren2023 2026-10-20): the picture as wide as its lock cone needs and more, whatever the
    // zoom (the lens's own field at zoom 2 was 2.4 deg of a 6 deg cone, run372).
    const float lockHalfTan = _isLensView ? LockOn::ScopeHalfTan() : 0.0f;
    // Only ever wider: the CIS AV and wrist rockets home with a 1 deg cone, which would have zoomed their scope in.
    const float pictureFactor = lockHalfTan > 0 && lockHalfTan > _scopeHalfTan / factor ? _scopeHalfTan / lockHalfTan
                                                                                        : factor;
    RenderScopePass(camera, pictureFactor, _scopeHalfTan, _scopeFrame);
}

// The periscope's picture spans the display's width as seen from the head (1x, as a window onto the muzzle's view).
float PeriscopeHalfTan()
{
    FpGuard fp;
    float distanceSquared = 0;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float offset = _periscope.screen[12 + axis] - _headWorldPosition[axis];
        distanceSquared += offset * offset;
    }
    const float halfTan = _periscope.fieldFactor * _periscope.halfWidthMeters /
                          sqrtf(distanceSquared > 1e-6f ? distanceSquared : 1e-6f);
    return halfTan < LENS_HALF_TAN_MINIMUM ? LENS_HALF_TAN_MINIMUM
                                           : halfTan > LENS_HALF_TAN_MAXIMUM ? LENS_HALF_TAN_MAXIMUM : halfTan;
}

// The scope pass's Render() with the camera at this frame and a square field of view of 2 atan(halfTan / factor).
void RenderScopePass(void* camera, float factor, float halfTan, const float* frame)
{
    float scopeFovRadians = 0;
    {
        FpGuard fp;
        _scopeHalfTan = halfTan;
        if (frame != _scopeFrame)
        {
            memcpy(_scopeFrame, frame, sizeof(_scopeFrame));
        }
        scopeFovRadians = 2.0f * atanf(halfTan / factor);
    }
    _originalSetFov(camera, scopeFovRadians, 1.0f);
    _originalSetTransform(camera, _scopeFrame);
    _isScopePass = true;
    _isScopeCaptured = false;
    {
        Profiling::Scope profile(Profiling::Stage::RenderScope);
#if SWBF_VR_DEV
        FoliageProbe::BeforeRender();
#endif
        _originalRender();
#if SWBF_VR_DEV
        FoliageProbe::AfterRender("scope");
#endif
    }
    _isScopePass = false;
    _hasScopeView = _isScopeCaptured;
    _originalSetFov(camera, _forcedFovRadians, _forcedHeightOverWidth);
    static bool hasLogged = false;
    if (!hasLogged || !_hasScopeView)
    {
        static unsigned failedCount = 0;
        if (!hasLogged || ++failedCount <= 3)
        {
            hasLogged = true;
            FpGuard fp;
            Log::Info(
                "stereo",
                "scope pass: picture %s for the %s (factor %.2f, scope tan %.4f, window or lens tan %.4f)",
                _hasScopeView ? "taken" : "NOT taken",
                _isPeriscope ? "periscope" : _isLensView ? "lens" : "window",
                static_cast<double>(factor),
                static_cast<double>(_scopeHalfTan / factor),
                static_cast<double>(_scopeHalfTan));
        }
    }
}

// One game frame for the headset: places the HUD panel and the reticle, then calls the game's Render() once per eye
// with the camera on that eye, and submits the frame.
void RenderForHeadset(void* camera)
{
    float tanX = 0, tanY = 0, eyeCameras[2][16];
    {
        FpGuard fp;
        RefreshHead(); // the freshest eye poses, predicted for when this frame is shown
        const float* tans =
            reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(camera) + OFFSET_cameraZoomedTan);
        tanX = tans[0];
        tanY = tans[1];
        MultiplyMatrices(eyeCameras[0], _eyePoses[0], _levelCamera);
        MultiplyMatrices(eyeCameras[1], _eyePoses[1], _levelCamera);
        _passTanX = tanX;
        _passTanY = tanY;
        static bool wasZoomed = false; // the headset's field of view must stay the same while zoomed (ZOOM.md)
        if (Zoom::IsZoomed() != wasZoomed)
        {
            wasZoomed = !wasZoomed;
            Log::Info(
                "stereo",
                "zoom %s: image tan %.4f x %.4f handed to the headset",
                wasZoomed ? "on" : "off",
                static_cast<double>(tanX),
                static_cast<double>(tanY));
        }
        _isHudWhole = !_isPlayerViewCamera || _cameraCaller == _spawnScreenCameraReturn || Settings::IsMenuOpen();
        const bool isPanelAhead = _isHudWhole || Hud::HasOverlay(); // menus and lists: straight ahead, for the pointer
        // HUD panel: a point 3 m along the aim (or straight ahead), seen from each eye. While the view heading is held
        // (on foot with controllers) the playing HUD follows the head (idren2023, headset run 12); menus stay ahead.
        float headCamera[16];
        MultiplyMatrices(headCamera, _headPose, _levelCamera);
        // Every panel shown ahead (the spawn screen, the pause menu, the settings, maps and lists) hangs where the
        // head looked, level, when it opened (headset run 19: the pause menu appeared aside or behind after the
        // player had turned in the room; only the spawn screen did this before).
        if (!isPanelAhead)
        {
            _hasSpawnPanelFrame = false;
        }
        else if (!_hasSpawnPanelFrame)
        {
            // level: the head's back axis without its pitch, world up, from the head's place
            const float backLength = sqrtf(headCamera[8] * headCamera[8] + headCamera[10] * headCamera[10]);
            if (backLength > 1e-4f)
            {
                const float backX = headCamera[8] / backLength, backZ = headCamera[10] / backLength;
                const float frame[16] = {backZ, 0, -backX, 0, 0, 1, 0, 0, backX, 0, backZ, 0,
                                         headCamera[12], headCamera[13], headCamera[14], 1};
                memcpy(_spawnPanelFrame, frame, sizeof(frame));
                _hasSpawnPanelFrame = true;
            }
        }
        const float* aim = _hasSpawnPanelFrame   ? _spawnPanelFrame
                           : !_isPlayerViewCamera ? _levelCamera
                           : _isViewHeld        ? (isPanelAhead ? _levelCamera : headCamera)
                                                : _gameCamera;
        const float panelCentre[3] = {
            aim[12] - aim[8] * HUD_DISTANCE_METERS,
            aim[13] - aim[9] * HUD_DISTANCE_METERS,
            aim[14] - aim[10] * HUD_DISTANCE_METERS};
        memcpy(_eyeCameras, eyeCameras, sizeof(_eyeCameras));
        memcpy(_hudFrame, aim, sizeof(_hudFrame));
        for (int axis = 0; axis < 3; ++axis)
        {
            _headWorldPosition[axis] = (eyeCameras[0][12 + axis] + eyeCameras[1][12 + axis]) * 0.5f;
        }
        for (int eye = 0; eye < 2; ++eye)
        {
            const float* eyeCamera = eyeCameras[eye];
            const float toPanel[3] = {
                panelCentre[0] - eyeCamera[12],
                panelCentre[1] - eyeCamera[13],
                panelCentre[2] - eyeCamera[14]};
            const float x = toPanel[0] * eyeCamera[0] + toPanel[1] * eyeCamera[1] + toPanel[2] * eyeCamera[2],
                        y = toPanel[0] * eyeCamera[4] + toPanel[1] * eyeCamera[5] + toPanel[2] * eyeCamera[6];
            float depth = -(toPanel[0] * eyeCamera[8] + toPanel[1] * eyeCamera[9] + toPanel[2] * eyeCamera[10]);
            _hudPlacements[eye].isValid = tanX > 0.01f && tanY > 0.01f;
            if (depth < 0.5f)
            {
                depth = 0.5f; // behind the head or far to the side: the clamp in SetHudViewport keeps it at the edge
            }
            _hudPlacements[eye].x = x / depth / tanX;
            _hudPlacements[eye].y = y / depth / tanY;
            _hudPlacements[eye].scale = HUD_DISTANCE_METERS / depth;
        }
    }
    Aim::BeforeRender();
    float updateCamera[16];
    memcpy(
        updateCamera,
        reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(camera) + OFFSET_cameraMatrix),
        sizeof(updateCamera));
    { // Test: the camera's numbers around the field of view, once (looking for the near clip distance).
        static bool isLogged = false;
        if (!isLogged)
        {
            isLogged = true;
            FpGuard fp;
            const float* block =
                reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(camera) + OFFSET_cameraFovBlock);
            char line[400];
            int length = 0;
            for (int index = 0; index < 20; ++index)
            {
                length += snprintf(
                    line + length,
                    sizeof(line) - static_cast<size_t>(length),
                    " +%x=%.4g",
                    0x80 + index * 4,
                    static_cast<double>(block[index]));
            }
            Log::Info("stereo", "camera floats:%s", line);
        }
    }
    if (_cameraLogCount > 0)
    { // test: what the camera holds when Render starts
        FpGuard fp;
        Log::Info(
            "cam",
            "frame %u at render start: pos %.5f %.5f %.5f back %.6f %.6f %.6f (level camera back %.6f %.6f %.6f)",
            _frameCount,
            static_cast<double>(updateCamera[12]),
            static_cast<double>(updateCamera[13]),
            static_cast<double>(updateCamera[14]),
            static_cast<double>(updateCamera[8]),
            static_cast<double>(updateCamera[9]),
            static_cast<double>(updateCamera[10]),
            static_cast<double>(_levelCamera[8]),
            static_cast<double>(_levelCamera[9]),
            static_cast<double>(_levelCamera[10]));
    }
    RenderScope(camera);
    Hud::SetZoomVignette(_fullViewFovRadians > 0);
    _isHeadsetPass = true;
    _isEyeImageSubmitted[0] = _isEyeImageSubmitted[1] = false;
    memcpy(_headWorld, eyeCameras[0], sizeof(_headWorld));
    for (int axis = 0; axis < 3; ++axis)
    {
        _headWorld[12 + axis] = (eyeCameras[0][12 + axis] + eyeCameras[1][12 + axis]) * 0.5f;
    }
    {
        // the head's turn relative to the level camera (rows: head axes in the level camera's frame), followed
        FpGuard fp;
        float turn[9];
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                turn[row * 3 + column] = _headWorld[row * 4] * _levelCamera[column * 4] +
                                         _headWorld[row * 4 + 1] * _levelCamera[column * 4 + 1] +
                                         _headWorld[row * 4 + 2] * _levelCamera[column * 4 + 2];
            }
        }
        const DWORD now = GetTickCount();
        const float seconds = _smoothHeadTick ? static_cast<float>(now - _smoothHeadTick) * 0.001f : 1.0f;
        _smoothHeadTick = now;
        const float share = seconds > 0.5f ? 1.0f : 1.0f - expf(-seconds / SMOOTH_HEAD_SECONDS);
        for (int k = 0; k < 9; ++k)
        {
            _smoothHeadTurn[k] += (turn[k] - _smoothHeadTurn[k]) * share;
        }
        // orthonormal again: back, then right from up x back, up from back x right
        float* right = _smoothHeadTurn;
        float* up = _smoothHeadTurn + 3;
        float* back = _smoothHeadTurn + 6;
        const auto normalize = [](float* v)
        {
            const float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (length > 1e-6f)
            {
                v[0] /= length, v[1] /= length, v[2] /= length;
            }
        };
        normalize(back);
        right[0] = up[1] * back[2] - up[2] * back[1];
        right[1] = up[2] * back[0] - up[0] * back[2];
        right[2] = up[0] * back[1] - up[1] * back[0];
        normalize(right);
        up[0] = back[1] * right[2] - back[2] * right[1];
        up[1] = back[2] * right[0] - back[0] * right[2];
        up[2] = back[0] * right[1] - back[1] * right[0];
        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                _smoothHeadWorld[row * 4 + column] = _smoothHeadTurn[row * 3] * _levelCamera[column] +
                                                     _smoothHeadTurn[row * 3 + 1] * _levelCamera[4 + column] +
                                                     _smoothHeadTurn[row * 3 + 2] * _levelCamera[8 + column];
            }
            _smoothHeadWorld[row * 4 + 3] = 0;
        }
        memcpy(_smoothHeadWorld + 12, _headWorld + 12, 3 * sizeof(float));
        _smoothHeadWorld[15] = 1;
    }
    for (int eye = 0; eye < 2; ++eye)
    {
        _originalSetTransform(camera, eyeCameras[eye]);
        if (_fullViewFovRadians > 0)
        {
            _originalSetFov(camera, _fullViewFovRadians, _forcedHeightOverWidth);
        }
        memcpy(_eyePositions[eye], &eyeCameras[eye][12], sizeof(_eyePositions[eye]));
        _currentEye = eye;
        // Profiling::Scope: measures the time until it goes out of scope (its destructor runs at the closing brace).
        Profiling::Scope profile(eye == 0 ? Profiling::Stage::RenderLeft : Profiling::Stage::RenderRight);
#if SWBF_VR_DEV
        FoliageProbe::BeforeRender();
        RenderSampler::BeforeRender();
#endif
        _originalRender();
#if SWBF_VR_DEV
        RenderSampler::AfterRender();
        FoliageProbe::AfterRender(eye == 0 ? "left" : "right");
#endif
    }
    _currentEye = -1;
    _isHeadsetPass = false;
    if (_fullViewFovRadians > 0)
    {
        _originalSetFov(camera, _forcedFovRadians, _forcedHeightOverWidth);
    }
    _originalSetTransform(camera, updateCamera); // back to what Update left
    {
        FpGuard fp;
        Xr::EndStereoFrame(tanX, tanY, _isEyeImageSubmitted[0] && _isEyeImageSubmitted[1]);
    }
    if (++_headsetFrameCount == 1)
    {
        Log::Info(
            "stereo",
            "first headset frame drawn (image tan %d.%03d x %d.%03d)",
            static_cast<int>(tanX),
            static_cast<int>(tanX * 1000) % 1000,
            static_cast<int>(tanY),
            static_cast<int>(tanY * 1000) % 1000);
    }
}

// Hook of Render() (cdecl, no arguments). Decides per frame: headset (two eye passes), desktop side by side (two eye
// passes), or the game's single pass.
void __cdecl RenderHook()
{
    _isFlatFrameSubmitted = false;
    _isFirstPersonPass = false;
    {
        FpGuard fp;
        Xr::Service();
    }
    _isInRender = true;
    Limits::Apply();
    Foliage::BeforeFrame();
    Zoom::BeforeFrame(Xr::IsRunning());
    Textures::BeforeFrame();
    void* const camera = FindSceneCamera();
    if (camera != _sceneCamera)
    {
        static unsigned loggedCount = 0;
        if (loggedCount++ < 20)
        {
            Log::Info(
                "stereo",
                "scene camera %p (camera manager's: %p)",
                camera,
                reinterpret_cast<void*>(EngineSurvey::Resolve("red")));
        }
    }
    _sceneCamera = camera;
    if (!camera)
    {
        _hasLastPlayView = false; // menus, loading: the next match starts from its own spawn camera
    }
    const bool isVrRunning = Xr::IsRunning();
    if (!isVrRunning && !_isFovForcedByScript && _forcedFovRadians > 0)
    {
        FpGuard fp;
        SetForcedFov(0, 0, "");
    }
    int headsetFrame = 0; // Xr::BeginStereoFrame: 2 = draw both eyes, 1 = frame already closed, nothing to draw
    if (camera && isVrRunning)
    {
        // Draw for the headset only if this frame's Update already ran with the head camera and the headset's field of
        // view.
        bool isReady = false;
        {
            FpGuard fp;
            const float* tans = reinterpret_cast<const float*>(reinterpret_cast<uintptr_t>(camera) + OFFSET_cameraTan);
            isReady = _didUpdateWithHead && _forcedTanX > 0 && fabsf(tans[0] - _forcedTanX) < 2e-3f &&
                      fabsf(tans[1] - _forcedTanY) < 2e-3f;
            if (isReady)
            {
                headsetFrame = Xr::BeginStereoFrame();
            }
        }
        if (!isReady && ++_notReadyFrameCount <= 5)
        {
            Log::Info(
                "stereo",
                "frame not ready for the headset (head camera in update: %d), shown flat",
                _didUpdateWithHead ? 1 : 0);
        }
    }
    if (headsetFrame == 2)
    {
        RenderForHeadset(camera);
    }
    else if (camera && _desktopHalfEyeDistanceMicrons && !isVrRunning)
    {
        memcpy(
            _desktopBaseCamera,
            reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(camera) + OFFSET_cameraMatrix),
            sizeof(_desktopBaseCamera));
        const float halfEyeDistanceMeters = static_cast<float>(_desktopHalfEyeDistanceMicrons) * 1e-6f;
        for (int eye = 0; eye < 2; ++eye)
        {
            float eyeCamera[16];
            {
                FpGuard fp;
                const float shift = eye == 0 ? -halfEyeDistanceMeters : halfEyeDistanceMeters;
                memcpy(eyeCamera, _desktopBaseCamera, sizeof(eyeCamera));
                eyeCamera[12] += _desktopBaseCamera[0] * shift;
                eyeCamera[13] += _desktopBaseCamera[1] * shift;
                eyeCamera[14] += _desktopBaseCamera[2] * shift;
            }
            _originalSetTransform(camera, eyeCamera);
            _currentEye = eye;
            _originalRender();
        }
        _currentEye = -1;
        _originalSetTransform(camera, _desktopBaseCamera);
    }
    else
    {
        _isFlatFrameSubmitted = headsetFrame == 1; // the headset frame for this game frame was already closed
        Profiling::Scope profile(Profiling::Stage::RenderFlat);
        _originalRender();
    }
    _didUpdateWithHead = false;
    _isInRender = false;
    if (++_frameCount == 1)
    {
        Log::Info("stereo", "Render hook active");
    }
    // Frame rate while drawing twice, every 10 s.
    if (_currentEye < 0 && (isVrRunning || _desktopHalfEyeDistanceMicrons))
    {
        const DWORD now = GetTickCount();
        ++_rateFrameCount;
        if (!_rateStartTick)
        {
            _rateStartTick = now;
            _rateFrameCount = 0;
        }
        else if (now - _rateStartTick >= 10000)
        {
            Log::Info(
                "stereo",
                "%u frames per second (%s)",
                static_cast<unsigned>(_rateFrameCount * 1000ull / (now - _rateStartTick)),
                isVrRunning ? "headset path" : "side by side");
            _rateStartTick = now;
            _rateFrameCount = 0;
        }
    }
    else
    {
        _rateStartTick = 0;
    }
}

void ReleaseEyeTargets()
{
    for (int eye = 0; eye < 2; ++eye)
    {
        if (_eyeSurfaces[eye])
        {
            _eyeSurfaces[eye]->lpVtbl->Release(_eyeSurfaces[eye]);
            _eyeSurfaces[eye] = nullptr;
        }
        if (_eyeTextures[eye])
        {
            _eyeTextures[eye]->lpVtbl->Release(_eyeTextures[eye]);
            _eyeTextures[eye] = nullptr;
        }
    }
    if (_readbackSurface)
    {
        _readbackSurface->lpVtbl->Release(_readbackSurface);
        _readbackSurface = nullptr;
    }
    _eyeTargetWidth = _eyeTargetHeight = _readbackWidth = _readbackHeight = 0;
}

// The two render targets the side-by-side picture is composed from, the size of the back buffer.
bool EnsureEyeTargets(IDirect3DDevice9* device, const D3DSURFACE_DESC& backBufferDescription)
{
    if (_eyeSurfaces[0] && _eyeTargetWidth == backBufferDescription.Width &&
        _eyeTargetHeight == backBufferDescription.Height)
    {
        return true;
    }
    for (int eye = 0; eye < 2; ++eye)
    {
        if (_eyeSurfaces[eye])
        {
            _eyeSurfaces[eye]->lpVtbl->Release(_eyeSurfaces[eye]);
            _eyeSurfaces[eye] = nullptr;
        }
        if (_eyeTextures[eye])
        {
            _eyeTextures[eye]->lpVtbl->Release(_eyeTextures[eye]);
            _eyeTextures[eye] = nullptr;
        }
        if (FAILED(device->lpVtbl->CreateTexture(
                device,
                backBufferDescription.Width,
                backBufferDescription.Height,
                1,
                D3DUSAGE_RENDERTARGET,
                backBufferDescription.Format,
                D3DPOOL_DEFAULT,
                &_eyeTextures[eye],
                nullptr)) ||
            FAILED(_eyeTextures[eye]->lpVtbl->GetSurfaceLevel(_eyeTextures[eye], 0, &_eyeSurfaces[eye])))
        {
            Log::Warning("stereo", "could not create the eye render targets, desktop stereo switched off");
            InterlockedExchange(&_desktopHalfEyeDistanceMicrons, 0);
            return false;
        }
    }
    _eyeTargetWidth = backBufferDescription.Width;
    _eyeTargetHeight = backBufferDescription.Height;
    return true;
}

// Copies the back buffer into system memory and passes the pixels to `use`. Returns false if that failed.
// A template: `use` can be any callable, here always a lambda (bool(const void* pixels, int pitch)); the compiler
// makes one copy of this function per lambda.
template <class Use>
bool ReadBackBuffer(
    IDirect3DDevice9* device,
    IDirect3DSurface9* backBuffer,
    const D3DSURFACE_DESC& description,
    Use use)
{
    if (description.Format != D3DFMT_A8R8G8B8 && description.Format != D3DFMT_X8R8G8B8)
    {
        return false;
    }
    if (!_readbackSurface || _readbackWidth != description.Width || _readbackHeight != description.Height)
    {
        if (_readbackSurface)
        {
            _readbackSurface->lpVtbl->Release(_readbackSurface);
            _readbackSurface = nullptr;
        }
        if (FAILED(device->lpVtbl->CreateOffscreenPlainSurface(
                device,
                description.Width,
                description.Height,
                description.Format,
                D3DPOOL_SYSTEMMEM,
                &_readbackSurface,
                nullptr)))
        {
            return false;
        }
        _readbackWidth = description.Width;
        _readbackHeight = description.Height;
    }
    D3DLOCKED_RECT lockedRect{};
    {
        Profiling::Scope profile(Profiling::Stage::Readback);
        if (FAILED(device->lpVtbl->GetRenderTargetData(device, backBuffer, _readbackSurface)))
        {
            return false;
        }
        if (FAILED(_readbackSurface->lpVtbl->LockRect(_readbackSurface, &lockedRect, nullptr, D3DLOCK_READONLY)))
        {
            return false;
        }
    }
    const bool isUsed = use(lockedRect.pBits, lockedRect.Pitch);
    _readbackSurface->lpVtbl->UnlockRect(_readbackSurface);
    return isUsed;
}

// Death without camera motion (PLAN 35, comfort): on the player's death the game switches to its DeathCamera
// (CameraManager::StartDeathCamera 0x99630), which starts from the chase camera's last live matrix (+0x10) and then,
// in its per-frame update, turns to the body, rises and swings round to the killer. In VR that update is skipped, so
// the view stays where the player died; the spawn screen then shows the chase camera's matrix of that same frame
// (offsets "Death camera"). Only CameraManager::Update reads the death camera; the sound listener follows the render
// camera.
using DeathCameraUpdateFunction = void(__fastcall*)(void* camera, void* edx, float seconds);
DeathCameraUpdateFunction _originalDeathCameraUpdate = nullptr;
constexpr int DEATH_CAMERA_UPDATE_SLOT = 1;
/// CameraManager (its RedCamera at +0x260, which is the scene camera): the active camera and the death camera of view
/// 0. DeathCamera: its target (soldier+0x44C) and the killer (a Character record, 0 when the player killed himself).
/// Character: its unit (+0xB8 a vehicle, else +0xB4 the soldier's Controllable; 0 while dead), whose +0xE0 is its
/// position. Soldier: world position +0x1E0. Read in the code (sub-agent, 2026-10-14; offsets "Death camera").
constexpr uintptr_t OFFSET_cameraManagerActiveCamera = 0x24;
constexpr uintptr_t OFFSET_cameraManagerDeathCamera = 0x44;
constexpr uintptr_t OFFSET_deathCameraTarget = 0xC;
constexpr uintptr_t OFFSET_deathCameraKiller = 0x6C;
constexpr uintptr_t OFFSET_soldierTargetInterface = 0x44C;
constexpr uintptr_t OFFSET_characterVehicle = 0xB8;
constexpr uintptr_t OFFSET_characterUnit = 0xB4;
constexpr uintptr_t OFFSET_unitPosition = 0xE0;
constexpr uintptr_t OFFSET_soldierPosition = 0x1E0;
/// The line runs between these heights above the body's and the killer's positions (chest height).
constexpr float KILLER_LINE_HEIGHT_METERS = 1.0f;
/// The killer's end, on foot: its unit's position lies about 1.5 m over its feet (headset run 15, idren2023: the line
/// ended about 1.5 m too high, above the head), so the waist is half a metre below it. Inferred from that report.
constexpr float KILLER_LINE_SOLDIER_END_METERS = -0.5f;
uintptr_t _killerLineCamera = 0; // the death camera the line belongs to
uintptr_t _killerUnit = 0;       // the killer's unit when the line began; another one ends the line
bool _hasKillerLineEnded = false;

// Vtable slot 1 of DeathCamera: thiscall (ecx = the camera), one float on the stack (ret 4); game thread.
void __fastcall DeathCameraUpdateHook(void* camera, void* edx, float seconds)
{
    if (Xr::IsRunning())
    {
        static bool hasSaid = false;
        if (!hasSaid)
        {
            hasSaid = true;
            Log::Info("stereo", "death camera held where the player died");
        }
        return;
    }
    _originalDeathCameraUpdate(camera, edx, seconds);
}

void InstallDeathView()
{
    if (_originalDeathCameraUpdate || !GameBuild::IsAvailable(GameBuild::Feature::DeathView))
    {
        return;
    }
    uintptr_t* const entry = reinterpret_cast<uintptr_t*>(GameBuild::Absolute(GameBuild::Address::DeathCameraTable)) +
                             DEATH_CAMERA_UPDATE_SLOT;
    DWORD oldProtection = 0;
    if (!VirtualProtect(entry, 4, PAGE_READWRITE, &oldProtection))
    {
        Log::Warning("stereo", "death camera not held: its vtable could not be written");
        return;
    }
    _originalDeathCameraUpdate = reinterpret_cast<DeathCameraUpdateFunction>(*entry);
    InterlockedExchange(
        reinterpret_cast<volatile LONG*>(entry),
        static_cast<LONG>(reinterpret_cast<uintptr_t>(&DeathCameraUpdateHook)));
    VirtualProtect(entry, 4, oldProtection, &oldProtection);
    Log::Info("stereo", "death camera update hooked (the view stays where the player dies)");
}

template <typename T> bool ReadGame(uintptr_t address, T* value)
{
    if (!address || IsBadReadPtr(reinterpret_cast<void*>(address), sizeof(T)))
    {
        return false;
    }
    memcpy(value, reinterpret_cast<const void*>(address), sizeof(T));
    return true;
}

// Installs the four hooks (once VR starts, or when a desktop test needs them) and the renderer limits.
bool Install()
{
    if (_isInstalled)
    {
        return true;
    }
    // The game build's check (GameBuild::Identify) decides; without these functions VR cannot run at all.
    if (!GameBuild::IsAvailable(GameBuild::Feature::Stereo))
    {
        static bool hasLogged = false;
        if (!hasLogged)
        {
            hasLogged = true;
            Log::Error(
                "stereo",
                "hooks not installed: %s is off for this game build",
                GameBuild::FeatureName(GameBuild::Feature::Stereo));
        }
        return false;
    }
    _exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    _viewCount = reinterpret_cast<const uint32_t*>(GameBuild::Absolute(GameBuild::Address::ViewCount));
    _viewIs3d = reinterpret_cast<const uint8_t*>(GameBuild::Absolute(GameBuild::Address::ViewIs3d));
    _viewCameras = reinterpret_cast<void* const*>(GameBuild::Absolute(GameBuild::Address::ViewCameras));
    if (GameBuild::IsAvailable(GameBuild::Feature::CameraCallers))
    {
        _sceneNearFarReturn = GameBuild::Rva(GameBuild::Address::SceneNearFarReturn);
        _sceneAfterFarNearFarReturn = GameBuild::Rva(GameBuild::Address::SceneAfterFarNearFarReturn);
        _firstPersonNearFarReturn = GameBuild::Rva(GameBuild::Address::FirstPersonNearFarReturn);
        _skyNearFarReturn = GameBuild::Rva(GameBuild::Address::SkyNearFarReturn);
        _spawnScreenCameraReturn = GameBuild::Rva(GameBuild::Address::SpawnScreenCameraReturn);
        _chaseCameraCodeStart = GameBuild::Rva(GameBuild::Address::ChaseCameraCodeStart);
        _chaseCameraCodeEnd = GameBuild::Rva(GameBuild::Address::ChaseCameraCodeEnd);
    }
    const MH_STATUS initStatus = MH_Initialize();
    if (initStatus != MH_OK && initStatus != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log::Error("stereo", "MinHook init failed");
        return false;
    }
    RenderFunction renderDetour = RenderHook;

    const struct
    {
        GameBuild::Address address;
        void* detour;
        void** original;
        const char* name;
    } hooks[] = {
        {GameBuild::Address::SetTransform,
         reinterpret_cast<void*>(&SetTransformHook),
         reinterpret_cast<void**>(&_originalSetTransform),
         "SetTransform"},
        {GameBuild::Address::SetFov,
         reinterpret_cast<void*>(&SetFovHook),
         reinterpret_cast<void**>(&_originalSetFov),
         "SetFov"},
        {GameBuild::Address::Render,
         reinterpret_cast<void*>(renderDetour),
         reinterpret_cast<void**>(&_originalRender),
         "Render"},
        {GameBuild::Address::SetNearFar,
         reinterpret_cast<void*>(&SetNearFarHook),
         reinterpret_cast<void**>(&_originalSetNearFar),
         "SetNearFar"},
    };

    for (const auto& hook : hooks)
    {
        void* const target = reinterpret_cast<void*>(GameBuild::Absolute(hook.address));
        MH_STATUS status = MH_CreateHook(target, hook.detour, hook.original);
        if (status == MH_OK)
        {
            status = MH_EnableHook(target);
        }
        if (status != MH_OK)
        {
            Log::Error("stereo", "hook %s failed: %s", hook.name, MH_StatusToString(status));
            return false;
        }
    }
    Limits::Install();
    InstallDeathView();
    Zoom::Install();
    Textures::Install();
    HudDraws::Install();
    Log::Info("stereo", "hooks installed (Render, SetTransform, SetFov, SetNearFar)");
    _isInstalled = true;
    return true;
}

} // namespace

// Test ("grab <n> [mode]"): keeps the next n images in memory and writes them when the last one is in, as
// GameData\swbf-vr-grab-<number>-<kind>.bmp. Kinds: l or r an eye pass before any composing, f a frame not drawn per
// eye, p what is really presented to the window (mode 1). Sizes and cuts: GrabImages in Stereo.h.
volatile LONG _grabRemaining = 0;
bool _isSimulatedReadbackOn = true;
bool _isSimulatedSideBySide = true; // simulated headset on the monitor: both eyes side by side, or (test) the right eye
                                    // alone like a real headset run
unsigned _frameRateCap = 0;
bool _isGrabbingPresented = false, _isGrabbingHalfSize = false, _isGrabbingCut = false;
int _grabCutXPercent = 50, _grabCutYPercent = 50;

struct GrabImage
{
    unsigned char* pixels; // 24-bit BGR rows, top down, from malloc
    int width, height;
    char kind;
    DWORD tick;
};

GrabImage _grabImages[MAXIMUM_GRAB_IMAGES];
unsigned _grabImageCount = 0, _grabFileNumber = 0;

void WriteGrabImages()
{
    for (unsigned index = 0; index < _grabImageCount; ++index)
    {
        const GrabImage& image = _grabImages[index];
        char name[64];
        snprintf(name, sizeof(name), "swbf-vr-grab-%03u-%c.bmp", _grabFileNumber++, image.kind);
        if (FILE* file = fopen(name, "wb"))
        {
            BITMAPFILEHEADER fileHeader{};
            BITMAPINFOHEADER infoHeader{};
            fileHeader.bfType = 0x4D42; // "BM"
            fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
            fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(image.width * image.height * 3);
            infoHeader.biSize = sizeof(infoHeader);
            infoHeader.biWidth = image.width;
            infoHeader.biHeight = -image.height; // negative: rows top down
            infoHeader.biPlanes = 1;
            infoHeader.biBitCount = 24;
            fwrite(&fileHeader, sizeof(fileHeader), 1, file);
            fwrite(&infoHeader, sizeof(infoHeader), 1, file);
            fwrite(image.pixels, 3, static_cast<size_t>(image.width * image.height), file);
            fclose(file);
        }
        Log::Info("grab", "%s at tick %lu", name, image.tick);
        free(image.pixels);
    }
    _grabImageCount = 0;
}

// Test clips ("record <name> <seconds> <fps>", idren2023 2026-11-11: video clips instead of a headset run): the left eye
// pass (or a flat frame) at half size, at most fps times a second, each written at once as
// GameData\swbf-vr-rec-<name>-<frame>.bmp (no frames held in memory: a clip may run for minutes).
char _recordName[32] = "";
DWORD _recordEndTick = 0, _recordNextTick = 0, _recordIntervalMilliseconds = 0;
unsigned _recordFrame = 0;
int _recordEye = 0; // which eye pass a clip shows (flat frames always)

void RecordFrame(IDirect3DDevice9* device)
{
    const DWORD now = GetTickCount();
    if (!_recordName[0] || static_cast<LONG>(now - _recordNextTick) < 0)
    {
        return;
    }
    if (static_cast<LONG>(now - _recordEndTick) >= 0)
    {
        Log::Info("record", "clip %s: %u frames", _recordName, _recordFrame);
        _recordName[0] = 0;
        return;
    }
    _recordNextTick += _recordIntervalMilliseconds;
    if (static_cast<LONG>(now - _recordNextTick) > 0)
    {
        _recordNextTick = now + _recordIntervalMilliseconds; // fell behind: no burst to catch up
    }
    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(device->lpVtbl->GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
    {
        return;
    }
    D3DSURFACE_DESC description{};
    backBuffer->lpVtbl->GetDesc(backBuffer, &description);
    const int width = static_cast<int>(description.Width / 2) & ~3, height = static_cast<int>(description.Height / 2);
    ReadBackBuffer(
        device,
        backBuffer,
        description,
        [&](const void* pixels, int pitch)
        {
            char name[96];
            snprintf(name, sizeof(name), "swbf-vr-rec-%s-%04u.bmp", _recordName, _recordFrame++);
            FILE* file = fopen(name, "wb");
            if (!file)
            {
                return false;
            }
            static unsigned char row[8192 * 3];
            BITMAPFILEHEADER fileHeader{};
            BITMAPINFOHEADER infoHeader{};
            fileHeader.bfType = 0x4D42; // "BM"
            fileHeader.bfOffBits = sizeof(fileHeader) + sizeof(infoHeader);
            fileHeader.bfSize = fileHeader.bfOffBits + static_cast<DWORD>(width * height * 3);
            infoHeader.biSize = sizeof(infoHeader);
            infoHeader.biWidth = width;
            infoHeader.biHeight = -height; // rows top down
            infoHeader.biPlanes = 1;
            infoHeader.biBitCount = 24;
            fwrite(&fileHeader, sizeof(fileHeader), 1, file);
            fwrite(&infoHeader, sizeof(infoHeader), 1, file);
            for (int y = 0; y < height && width <= 8192; ++y)
            {
                const unsigned char* source =
                    static_cast<const unsigned char*>(pixels) + static_cast<size_t>(y * 2) * static_cast<size_t>(pitch);
                for (int x = 0; x < width; ++x)
                {
                    row[x * 3] = source[x * 8];
                    row[x * 3 + 1] = source[x * 8 + 1];
                    row[x * 3 + 2] = source[x * 8 + 2];
                }
                fwrite(row, 3, static_cast<size_t>(width), file);
            }
            fclose(file);
            return true;
        });
    backBuffer->lpVtbl->Release(backBuffer);
}

void GrabBackBuffer(IDirect3DDevice9* device, char kind)
{
    if (InterlockedDecrement(&_grabRemaining) < 0)
    {
        InterlockedExchange(&_grabRemaining, 0);
        return;
    }
    IDirect3DSurface9* backBuffer = nullptr;
    if (_grabImageCount < MAXIMUM_GRAB_IMAGES &&
        SUCCEEDED(device->lpVtbl->GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) && backBuffer)
    {
        D3DSURFACE_DESC description{};
        backBuffer->lpVtbl->GetDesc(backBuffer, &description);
        // Presented: every 4th pixel. Eye passes: a full-resolution cut from the lower middle (ground, grass),
        // or (half size) the whole image at half size.
        const int step = _isGrabbingPresented ? 4 : _isGrabbingHalfSize ? 2 : 1;
        int width = static_cast<int>(description.Width / (_isGrabbingPresented || !_isGrabbingHalfSize ? 4 : 2)) & ~3,
            height = static_cast<int>(description.Height / (_isGrabbingPresented || !_isGrabbingHalfSize ? 4 : 2));
        int left = step == 1 ? static_cast<int>(description.Width) * 3 / 8 : 0,
            top = step == 1 ? static_cast<int>(description.Height) / 2 : 0;
        if (_isGrabbingCut)
        { // mode 3: 800x500 at full resolution around the given point (percent of the image)
            width = 800;
            height = 500;
            if (width > static_cast<int>(description.Width))
            {
                width = static_cast<int>(description.Width) & ~3;
            }
            if (height > static_cast<int>(description.Height))
            {
                height = static_cast<int>(description.Height);
            }
            left = static_cast<int>(description.Width) * _grabCutXPercent / 100 - width / 2;
            top = static_cast<int>(description.Height) * _grabCutYPercent / 100 - height / 2;
            if (left < 0)
            {
                left = 0;
            }
            if (top < 0)
            {
                top = 0;
            }
            if (left > static_cast<int>(description.Width) - width)
            {
                left = static_cast<int>(description.Width) - width;
            }
            if (top > static_cast<int>(description.Height) - height)
            {
                top = static_cast<int>(description.Height) - height;
            }
        }
        // The lambda captures everything by reference ([&]): it reads and writes this function's locals directly.
        ReadBackBuffer(
            device,
            backBuffer,
            description,
            [&](const void* pixels, int pitch)
            {
                unsigned char* copy = static_cast<unsigned char*>(malloc(static_cast<size_t>(width * height * 3)));
                if (!copy)
                {
                    return false;
                }
                for (int y = 0; y < height; ++y)
                {
                    const unsigned char* source = static_cast<const unsigned char*>(pixels) +
                                                  static_cast<size_t>(top + y * step) * static_cast<size_t>(pitch) +
                                                  left * 4;
                    unsigned char* row = copy + y * width * 3;
                    for (int x = 0; x < width; ++x)
                    {
                        row[x * 3] = source[x * step * 4];
                        row[x * 3 + 1] = source[x * step * 4 + 1];
                        row[x * 3 + 2] = source[x * step * 4 + 2];
                    }
                }
                _grabImages[_grabImageCount++] = {copy, width, height, kind, GetTickCount()};
                return true;
            });
        backBuffer->lpVtbl->Release(backBuffer);
    }
    if (_grabRemaining <= 0)
    {
        WriteGrabImages();
    }
}

bool OnPresent(void* device)
{
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    static IDirect3DDevice9* identifiedDevice = nullptr;
    if (d3dDevice != identifiedDevice)
    { // tell the headset code which graphics adapter the game is on
        identifiedDevice = d3dDevice;
        IDirect3D9* d3d = nullptr;
        D3DDEVICE_CREATION_PARAMETERS creation{};
        D3DADAPTER_IDENTIFIER9 adapter{};
        if (SUCCEEDED(d3dDevice->lpVtbl->GetDirect3D(d3dDevice, &d3d)) && d3d)
        {
            if (SUCCEEDED(d3dDevice->lpVtbl->GetCreationParameters(d3dDevice, &creation)) &&
                SUCCEEDED(d3d->lpVtbl->GetAdapterIdentifier(d3d, creation.AdapterOrdinal, 0, &adapter)))
            {
                Xr::SetGameAdapter(adapter.VendorId, adapter.DeviceId);
                Log::Info(
                    "stereo",
                    "the game renders on %s (%04lx:%04lx)",
                    adapter.Description,
                    adapter.VendorId,
                    adapter.DeviceId);
            }
            d3d->lpVtbl->Release(d3d);
        }
    }
    if (!_isInRender)
    { // a present from outside Render(); inside, RenderHook has done this at the start of the frame
        FpGuard fp;
        Xr::Service();
    }
    if (Xr::IsStarted() && !_isInstalled)
    {
        Install();
    }
    if (_isScopePass)
    { // the scope view's Render(): keep its picture (if the HUD pass did not come first), show nothing
        if (!_isScopeCaptured)
        {
            _isScopeCaptured = Hud::CaptureScope(d3dDevice);
        }
        return true;
    }
    if (_isHudViewportSet)
    {
        SetHudViewport(d3dDevice, false);
    }
    if (_currentEye >= 0 && _isHeadsetPass)
    {
        Hud::DrawVignette(d3dDevice);
    }
    if (_currentEye >= 0 && _isHeadsetPass)
    {
        const bool isStatistics = Controls::IsStatisticsScreen();
        Hud::Composite(
            d3dDevice,
            _currentEye,
            _eyeCameras[_currentEye],
            _headWorldPosition,
            _passTanX,
            _passTanY,
            _hudFrame,
            isStatistics ? STATISTICS_DISTANCE_METERS : HUD_DISTANCE_METERS,
            isStatistics ? STATISTICS_HALF_WIDTH_TAN : HUD_HALF_WIDTH_TAN,
            _isHudWhole);
        if (_hasScopeView)
        {
            DrawScope(d3dDevice);
        }
        ControllerHolo::DrawViewscreen(d3dDevice); // the droid's viewscreen, over everything (ControllerHolo.h)
    }
    if (_currentEye < 0)
    {
        // A flat frame (a menu): the virtual keyboard over it, before it is grabbed, presented or sent to the headset.
        IDirect3DSurface9* target = nullptr;
        if (SUCCEEDED(d3dDevice->lpVtbl->GetBackBuffer(d3dDevice, 0, 0, D3DBACKBUFFER_TYPE_MONO, &target)) && target)
        {
            D3DSURFACE_DESC size{};
            target->lpVtbl->GetDesc(target, &size);
            target->lpVtbl->Release(target);
            Keyboard::Draw(d3dDevice, static_cast<int>(size.Width), static_cast<int>(size.Height));
        }
    }
    if (_grabRemaining > 0 && !_isGrabbingPresented)
    {
        GrabBackBuffer(d3dDevice, _currentEye == 0 ? 'l' : _currentEye == 1 ? 'r' : 'f');
    }
    if (_currentEye < 0 || _currentEye == _recordEye)
    {
        RecordFrame(d3dDevice);
    }
    const bool isHeadsetFlat = _currentEye < 0 && Xr::IsRunning() && !_isFlatFrameSubmitted;
    if (_currentEye < 0 && !isHeadsetFlat)
    {
        _isFlatFrameSubmitted = false;
        return false;
    }
    _isFlatFrameSubmitted = false;

    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(d3dDevice->lpVtbl->GetBackBuffer(d3dDevice, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer)
    {
        return false;
    }
    D3DSURFACE_DESC description{};
    backBuffer->lpVtbl->GetDesc(backBuffer, &description);
    if (description.Width)
    {
        _backBufferHeightOverWidth = static_cast<float>(description.Height) / static_cast<float>(description.Width);
    }
    bool shouldSkipPresent = false;
    GpuTransfer::SharedImage sharedImage{};
    const int flatSlot = 2;
    if (isHeadsetFlat && Xr::TakesSharedImages() && GpuTransfer::Stage(d3dDevice, backBuffer, flatSlot) &&
        GpuTransfer::Transfer(&flatSlot, 1, !Xr::IsSimulated(), &sharedImage))
    { // on the graphics card (GpuTransfer)
        FpGuard fp;
        Xr::SubmitFlatShared(sharedImage);
        RefreshHead();
    }
    else if (isHeadsetFlat)
    {
        // Not the 3D scene (menu, loading screen, or a frame the headset did not want): show it as a flat screen.
        ReadBackBuffer(
            d3dDevice,
            backBuffer,
            description,
            [&](const void* pixels, int pitch)
            {
                FpGuard fp;
                Xr::SubmitFlatFrame(
                    pixels,
                    pitch,
                    static_cast<int>(description.Width),
                    static_cast<int>(description.Height));
                return true;
            });
        {
            FpGuard fp;
            RefreshHead();
        }
    }
    else if (_isHeadsetPass)
    {
        const int eye = _currentEye;
        const bool isReadbackOn = !Xr::IsSimulated() || _isSimulatedReadbackOn;
        // Best: on the graphics card (GpuTransfer), no CPU wait for the card at all.
        const bool isStaged = isReadbackOn && Xr::TakesSharedImages() && GpuTransfer::Stage(d3dDevice, backBuffer, eye);
        bool isOnCard = false;
        if (isStaged && GpuTransfer::Transfer(&eye, 1, !Xr::IsSimulated(), &sharedImage))
        { // each eye at once: the card starts on the left eye while the right one is being drawn
            FpGuard fp;
            _isEyeImageSubmitted[eye] = Xr::SubmitEyeShared(eye, sharedImage);
            isOnCard = true;
        }
        // Else one wait for the card per frame: the left eye is copied on the card and read back after the right
        // eye's render, so the card draws the left eye while the right one is being submitted. A read-back waits for
        // all work queued before it, so the right eye's read-back after it costs only the transfer.
        const bool hasLeftCopy = EnsureEyeTargets(d3dDevice, description);
        if (eye == 0)
        {
            _isLeftOnCard = isOnCard;
            _isLeftCopied = false;
            if (hasLeftCopy && (!isOnCard || (Xr::IsSimulated() && _isSimulatedSideBySide)))
            { // for the read-back after the right eye, or for the simulated side by side
                d3dDevice->lpVtbl->StretchRect(d3dDevice, backBuffer, nullptr, _eyeSurfaces[0], nullptr, D3DTEXF_NONE);
                _isLeftCopied = true;
            }
            else if (hasLeftCopy && isOnCard)
            {
                // The left eye went to the headset on the card and its Present is skipped, so Direct3D 9 on 12 has
                // an empty command list when the game asks its "has the card finished" query before the right eye's
                // render (battlefront.exe+0x275540 spins on GetData): that query then never completes and the game
                // hangs (headset test b337; runs 732-734). One pixel filled keeps the list from being empty.
                const RECT onePixel = {0, 0, 1, 1};
                d3dDevice->lpVtbl->ColorFill(d3dDevice, _eyeSurfaces[0], &onePixel, 0);
            }
        }
        const auto submit = [&](int submittedEye, IDirect3DSurface9* surface)
        {
            return ReadBackBuffer(
                d3dDevice,
                surface,
                description,
                [&](const void* pixels, int pitch)
                {
                    FpGuard fp;
                    return Xr::SubmitEye(
                        submittedEye,
                        pixels,
                        pitch,
                        static_cast<int>(description.Width),
                        static_cast<int>(description.Height));
                });
        };
        if (!isReadbackOn)
        {
            _isEyeImageSubmitted[eye] = true; // test: the simulated headset without the read-back
        }
        else if (eye == 0 && !isOnCard && !_isLeftCopied)
        {
            _isEyeImageSubmitted[0] = submit(0, backBuffer); // no copy of the left eye: read back now
        }
        else if (eye == 1)
        {
            if (!_isLeftOnCard && _isLeftCopied)
            {
                _isEyeImageSubmitted[0] = submit(0, _eyeSurfaces[0]);
            }
            if (!isOnCard)
            {
                _isEyeImageSubmitted[1] = submit(1, backBuffer);
            }
        }
        shouldSkipPresent = eye == 0; // the monitor shows the right eye
        if (Xr::IsSimulated() && _isSimulatedSideBySide && hasLeftCopy)
        { // simulated headset: show both eyes side by side instead (the left eye was copied above)
            if (eye == 1 && _isLeftCopied)
            {
                d3dDevice->lpVtbl->StretchRect(d3dDevice, backBuffer, nullptr, _eyeSurfaces[1], nullptr, D3DTEXF_NONE);
                const LONG width = static_cast<LONG>(description.Width), height = static_cast<LONG>(description.Height);
                RECT leftHalf = {0, 0, width / 2, height}, rightHalf = {width / 2, 0, width, height};
                d3dDevice->lpVtbl
                    ->StretchRect(d3dDevice, _eyeSurfaces[0], nullptr, backBuffer, &leftHalf, D3DTEXF_LINEAR);
                d3dDevice->lpVtbl
                    ->StretchRect(d3dDevice, _eyeSurfaces[1], nullptr, backBuffer, &rightHalf, D3DTEXF_LINEAR);
            }
        }
    }
    else if (EnsureEyeTargets(d3dDevice, description))
    {
        d3dDevice->lpVtbl
            ->StretchRect(d3dDevice, backBuffer, nullptr, _eyeSurfaces[_currentEye], nullptr, D3DTEXF_NONE);
        if (_currentEye == 0)
        {
            shouldSkipPresent = true;
        }
        else
        {
            const LONG width = static_cast<LONG>(description.Width), height = static_cast<LONG>(description.Height);
            RECT leftHalf = {0, 0, width / 2, height}, rightHalf = {width / 2, 0, width, height};
            d3dDevice->lpVtbl->StretchRect(d3dDevice, _eyeSurfaces[0], nullptr, backBuffer, &leftHalf, D3DTEXF_LINEAR);
            d3dDevice->lpVtbl->StretchRect(d3dDevice, _eyeSurfaces[1], nullptr, backBuffer, &rightHalf, D3DTEXF_LINEAR);
        }
    }
    backBuffer->lpVtbl->Release(backBuffer);
    return shouldSkipPresent;
}

void OnReset()
{
    ReleaseEyeTargets();
    GpuTransfer::OnReset();
    Sight::OnReset();
    LockOn::OnReset();
    Levers::OnReset();
    Keyboard::OnReset();
    ReloadZone::OnReset();
    BodyArm::OnReset();
    PlayerBody::OnReset();
    CockpitShell::OnReset();
    Hud::OnReset();
}

void SetGrabCut(unsigned xPercent, unsigned yPercent)
{
    _grabCutXPercent = static_cast<int>(xPercent > 100 ? 50 : xPercent);
    _grabCutYPercent = static_cast<int>(yPercent > 100 ? 50 : yPercent);
}

void Record(const char* name, unsigned seconds, unsigned framesPerSecond, int eye)
{
    _recordName[0] = 0;
    _recordEye = eye == 1 ? 1 : 0;
    if (!seconds || !framesPerSecond || !name[0])
    {
        return;
    }
    _recordFrame = 0;
    _recordIntervalMilliseconds = 1000 / framesPerSecond;
    _recordNextTick = GetTickCount();
    _recordEndTick = _recordNextTick + seconds * 1000;
    snprintf(_recordName, sizeof(_recordName), "%s", name);
    Log::Info("record", "clip %s: %u s at %u frames per second", _recordName, seconds, framesPerSecond);
}

void GrabImages(unsigned count, unsigned mode)
{
    _isGrabbingPresented = mode == 1;
    _isGrabbingHalfSize = mode == 2;
    _isGrabbingCut = mode == 3;
    InterlockedExchange(&_grabRemaining, static_cast<LONG>(count > MAXIMUM_GRAB_IMAGES ? MAXIMUM_GRAB_IMAGES : count));
}

void SetSimulatedSideBySide(bool isSideBySide)
{
    _isSimulatedSideBySide = isSideBySide;
    Log::Info("stereo", "simulated headset shown %s", isSideBySide ? "side by side" : "as the right eye alone");
}

int CurrentEye()
{
    return _currentEye;
}

void SetTestPartsOff(unsigned bits)
{
    _testPartsOff = bits;
    Log::Info("stereo", "test: headset path parts off: %u", bits);
}

void LogNearFarCalls(unsigned count)
{
    InterlockedExchange(&_nearFarLogsLeft, static_cast<LONG>(count));
}

void SetNearClip(unsigned sceneMillimeters, unsigned firstPersonMillimeters)
{
    _sceneNearMeters = static_cast<float>(sceneMillimeters) * 0.001f;
    if (firstPersonMillimeters)
    {
        _firstPersonNearMeters = static_cast<float>(firstPersonMillimeters) * 0.001f;
    }
    Log::Info(
        "stereo",
        "near clip distance: scene %u mm (0 = the game's), first person %.0f mm",
        sceneMillimeters,
        static_cast<double>(_firstPersonNearMeters * 1000));
}

void StartCameraLog(unsigned count)
{
    InterlockedExchange(&_cameraLogCount, static_cast<LONG>(count));
}

void SetTestOptions(int simulatedReadback, int frameRateCap)
{
    if (simulatedReadback >= 0)
    {
        _isSimulatedReadbackOn = simulatedReadback != 0;
    }
    if (frameRateCap >= 0)
    {
        _frameRateCap = static_cast<unsigned>(frameRateCap);
    }
    Log::Info(
        "stereo",
        "test settings: read-back in the simulated headset %s, frame rate cap %u",
        _isSimulatedReadbackOn ? "on" : "off",
        _frameRateCap);
}

void PaceBeforePresent()
{
    // Function-level statics: kept between calls (like static fields), initialised on the first call.
    static LARGE_INTEGER counterFrequency, lastPresent, statisticsStart;
    static double sumMilliseconds = 0, sumOfSquares = 0, shortestMilliseconds = 1e9, longestMilliseconds = 0;
    static unsigned presentCount = 0;
    FpGuard fp;
    if (!counterFrequency.QuadPart)
    {
        QueryPerformanceFrequency(&counterFrequency);
        QueryPerformanceCounter(&lastPresent);
        statisticsStart = lastPresent;
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (_frameRateCap)
    {
        const double wantedSeconds = 1.0 / _frameRateCap;
        for (;;)
        {
            const double passedSeconds = static_cast<double>(now.QuadPart - lastPresent.QuadPart) /
                                         static_cast<double>(counterFrequency.QuadPart);
            if (passedSeconds >= wantedSeconds)
            {
                break;
            }
            if (wantedSeconds - passedSeconds > 0.002)
            {
                Sleep(1);
            }
            QueryPerformanceCounter(&now);
        }
    }
    const double milliseconds = static_cast<double>(now.QuadPart - lastPresent.QuadPart) * 1000.0 /
                                static_cast<double>(counterFrequency.QuadPart);
    lastPresent = now;
    sumMilliseconds += milliseconds;
    sumOfSquares += milliseconds * milliseconds;
    if (milliseconds < shortestMilliseconds)
    {
        shortestMilliseconds = milliseconds;
    }
    if (milliseconds > longestMilliseconds)
    {
        longestMilliseconds = milliseconds;
    }
    ++presentCount;
    if (static_cast<double>(now.QuadPart - statisticsStart.QuadPart) >=
        2.0 * static_cast<double>(counterFrequency.QuadPart))
    {
        const double mean = sumMilliseconds / presentCount,
                     deviation = sqrt(
                         sumOfSquares / presentCount - mean * mean > 0 ? sumOfSquares / presentCount - mean * mean : 0);
        if (Xr::IsRunning())
        {
            Profiling::Report(presentCount);
        }
        if (Xr::IsRunning() || _frameRateCap)
        {
            Log::Info(
                "pace",
                "%u presents, ms between them: mean %.2f, deviation %.2f, min %.2f, max %.2f",
                presentCount,
                mean,
                deviation,
                shortestMilliseconds,
                longestMilliseconds);
        }
        sumMilliseconds = sumOfSquares = 0;
        shortestMilliseconds = 1e9;
        longestMilliseconds = 0;
        presentCount = 0;
        statisticsStart = now;
    }
}

void GrabPresented(void* device)
{
    if (_grabRemaining > 0 && _isGrabbingPresented)
    {
        GrabBackBuffer(static_cast<IDirect3DDevice9*>(device), 'p');
    }
}

void SetDesktopEyeDistance(unsigned eyeDistanceMillimeters)
{
    if (eyeDistanceMillimeters && !Install())
    {
        return;
    }
    InterlockedExchange(&_desktopHalfEyeDistanceMicrons, static_cast<LONG>(eyeDistanceMillimeters) * 500);
    Log::Info(
        "stereo",
        "desktop side-by-side %s, eye distance %u mm",
        eyeDistanceMillimeters ? "on" : "off",
        eyeDistanceMillimeters);
}

void ForceFov(unsigned tanXThousandths, unsigned tanYThousandths)
{
    if (tanXThousandths && !Install())
    {
        return;
    }
    FpGuard fp;
    _isFovForcedByScript = tanXThousandths != 0;
    SetForcedFov(
        static_cast<float>(tanXThousandths) * 0.001f,
        static_cast<float>(tanYThousandths ? tanYThousandths : tanXThousandths) * 0.001f,
        "by script");
}

void OnProjection(void* device, const float* projectionMatrix)
{
    const float* matrix = projectionMatrix;
    if (matrix[0] > HUD_PROJECTION_SCALE[0] - 0.002f && matrix[0] < HUD_PROJECTION_SCALE[0] + 0.002f &&
        matrix[5] > HUD_PROJECTION_SCALE[1] - 0.002f && matrix[5] < HUD_PROJECTION_SCALE[1] + 0.002f)
    {
        _isFirstPersonPass = false; // the HUD pass
    }
    { // Test: the different projections the game sets (first 24): looking for the near clip distance of each pass.
        static float seen[24][4];
        static unsigned seenCount = 0;
        bool isKnown = false;
        for (unsigned index = 0; index < seenCount; ++index)
        {
            if (seen[index][0] == matrix[0] && seen[index][1] == matrix[5] && seen[index][2] == matrix[10] &&
                seen[index][3] == matrix[14])
            {
                isKnown = true;
            }
        }
        if (!isKnown && seenCount < 24)
        {
            seen[seenCount][0] = matrix[0];
            seen[seenCount][1] = matrix[5];
            seen[seenCount][2] = matrix[10];
            seen[seenCount][3] = matrix[14];
            ++seenCount;
            FpGuard fp;
            Log::Info(
                "stereo",
                "projection set: xscale %.4f yscale %.4f m33 %.6f m43 %.6f m34 %.1f -> near %.4f far %.1f (eye "
                "%d)",
                static_cast<double>(matrix[0]),
                static_cast<double>(matrix[5]),
                static_cast<double>(matrix[10]),
                static_cast<double>(matrix[14]),
                static_cast<double>(matrix[11]),
                matrix[10] != 0 ? static_cast<double>(-matrix[14] / matrix[10]) : 0.0,
                matrix[10] != 1 ? static_cast<double>(matrix[14] / (1 - matrix[10])) : 0.0,
                _currentEye);
        }
    }
    if (_isScopePass && !_isScopeCaptured)
    {
        FpGuard fp;
        if (fabsf(matrix[0] - HUD_PROJECTION_SCALE[0]) < 0.002f && fabsf(matrix[5] - HUD_PROJECTION_SCALE[1]) < 0.002f)
        {
            _isScopeCaptured = Hud::CaptureScope(device); // the scene is complete, the game's HUD comes next
        }
    }
    if (_currentEye < 0 || !_isHeadsetPass)
    {
        return;
    }
    FpGuard fp;
    if (Hud::IsCompositing())
    {
        return;
    }
    const bool isHud =
        fabsf(matrix[0] - HUD_PROJECTION_SCALE[0]) < 0.002f && fabsf(matrix[5] - HUD_PROJECTION_SCALE[1]) < 0.002f;
    if (!(_testPartsOff & TEST_OFF_HUD_PANEL) && Hud::OnProjection(device, isHud))
    {
        if (_isHudViewportSet)
        {
            SetHudViewport(static_cast<IDirect3DDevice9*>(device), false);
        }
        return;
    }
    SetHudViewport(static_cast<IDirect3DDevice9*>(device), isHud);
}

bool GetBaseCamera(float* matrix)
{
    if (!_sceneCamera || !Xr::IsRunning())
    {
        return false;
    }
    memcpy(matrix, _levelCamera, sizeof(_levelCamera));
    return true;
}

// Menu screens the pointer can reach:
//  - No 3D scene (main menu, loading): the flat screen of Xr.cpp, 2 m ahead of the resting pose, 2.4 m wide.
//  - 3D scene but the player has not spawned (camera set by the spawn screen's camera code) or the VR Settings
//    screen is open: the HUD panel, 3 m along the game's camera, which carries the spawn screen and its cursor.
//  - Playing: none. (The game's pause menu inside a match is not covered: it cannot be told apart yet.)
bool ComputePointerUV(const float* handMatrix, float* u, float* v)
{
    const float forwardX = -handMatrix[8], forwardY = -handMatrix[9], forwardZ = -handMatrix[10];
    if (!_isInstalled || !FindSceneCamera())
    {
        if (forwardZ > -0.05f)
        {
            return false; // pointing away from the screen
        }
        const float distance = (-FLAT_SCREEN_DISTANCE_METERS - handMatrix[14]) / forwardZ;
        const float x = handMatrix[12] + forwardX * distance, y = handMatrix[13] + forwardY * distance;
        const float height = FLAT_SCREEN_WIDTH_METERS * _backBufferHeightOverWidth;
        *u = (x + FLAT_SCREEN_WIDTH_METERS * 0.5f) / FLAT_SCREEN_WIDTH_METERS;
        *v = (height * 0.5f - y) / height;
        return true;
    }
    // Large overlays (end-of-match statistics, headset run 12: no way to press Done) take the pointer too.
    const bool isOverlay = Hud::HasOverlay();
    if (_cameraCaller != _spawnScreenCameraReturn && !Settings::IsMenuOpen() && !isOverlay)
    {
        return false;
    }
    // The ray in the space of the camera the HUD panel is placed along.
    const float* aim = _hasSpawnPanelFrame                      ? _spawnPanelFrame
                       : _isPlayerViewCamera && !_isViewHeld ? _gameCamera
                                                             : _levelCamera;
    float handWorld[16], aimInverse[16], handInAim[16];
    MultiplyMatrices(handWorld, handMatrix, _levelCamera);
    // rigid inverse of aim: transposed rotation, position rotated back and negated
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            aimInverse[row * 4 + column] = aim[column * 4 + row];
        }
    }
    aimInverse[3] = aimInverse[7] = aimInverse[11] = 0;
    aimInverse[15] = 1;
    for (int column = 0; column < 3; ++column)
    {
        aimInverse[12 + column] =
            -(aim[12] * aimInverse[column] + aim[13] * aimInverse[4 + column] + aim[14] * aimInverse[8 + column]);
    }
    MultiplyMatrices(handInAim, handWorld, aimInverse);
    const float localForwardX = -handInAim[8], localForwardY = -handInAim[9], localForwardZ = -handInAim[10];
    if (localForwardZ > -0.05f)
    {
        return false;
    }
    const bool isStatistics = Controls::IsStatisticsScreen(); // placed as in the Composite call
    const float panelDistance = isStatistics ? STATISTICS_DISTANCE_METERS : HUD_DISTANCE_METERS;
    const float distance = (-panelDistance - handInAim[14]) / localForwardZ;
    const float x = handInAim[12] + localForwardX * distance, y = handInAim[13] + localForwardY * distance;
    const float halfWidth = panelDistance * (isStatistics ? STATISTICS_HALF_WIDTH_TAN : HUD_HALF_WIDTH_TAN),
                halfHeight = halfWidth * _backBufferHeightOverWidth;
    const float scale = isOverlay ? Hud::OverlayScale() : 1.0f; // the picture's size on the panel
    *u = 0.5f + ((x + halfWidth) / (2 * halfWidth) - 0.5f) / scale;
    *v = 0.5f + ((halfHeight - y) / (2 * halfHeight) - 0.5f) / scale;
    return true;
}

// The killer line (PLAN 35): read from the death camera each eye pass while it is the active camera.
bool GetKillerLine(float* body, float* killer)
{
    if (!_originalDeathCameraUpdate || !_sceneCamera || !Xr::IsRunning())
    {
        return false;
    }
    const uintptr_t cameraManager = reinterpret_cast<uintptr_t>(_sceneCamera) - OFFSET_cameraManagerRedCamera;
    uintptr_t active = 0, deathCamera = 0, target = 0, character = 0, unit = 0, vehicle = 0;
    if (!ReadGame(cameraManager + OFFSET_cameraManagerActiveCamera, &active) ||
        !ReadGame(cameraManager + OFFSET_cameraManagerDeathCamera, &deathCamera) || !deathCamera ||
        active != deathCamera)
    {
        _killerLineCamera = 0;
        return false;
    }
    if (!ReadGame(deathCamera + OFFSET_deathCameraTarget, &target) || !target ||
        !ReadGame(deathCamera + OFFSET_deathCameraKiller, &character) || !character)
    {
        return false;
    }
    if (ReadGame(character + OFFSET_characterVehicle, &vehicle) && vehicle)
    {
        unit = vehicle;
    }
    else
    {
        ReadGame(character + OFFSET_characterUnit, &unit);
    }
    if (_killerLineCamera != deathCamera)
    { // a new death (the death camera is embedded in the CameraManager, so the same address each time: it is reset
      // whenever another camera is active): the line starts with the killer's unit of this moment
        _killerLineCamera = deathCamera;
        _killerUnit = unit;
        _hasKillerLineEnded = !unit;
        Log::Info("stereo", "killer line %s", unit ? "on" : "off (the killer is not alive)");
    }
    if (_hasKillerLineEnded || unit != _killerUnit)
    {
        if (!_hasKillerLineEnded)
        {
            _hasKillerLineEnded = true;
            Log::Info("stereo", "killer line off: the killer died");
        }
        return false;
    }
    float bodyPosition[3], killerPosition[3];
    const uintptr_t soldier = target - OFFSET_soldierTargetInterface;
    if (!ReadGame(soldier + OFFSET_soldierPosition, &bodyPosition) ||
        !ReadGame(unit + OFFSET_unitPosition, &killerPosition))
    {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        body[axis] = bodyPosition[axis];
        killer[axis] = killerPosition[axis];
    }
    body[1] += KILLER_LINE_HEIGHT_METERS;
    killer[1] += vehicle ? KILLER_LINE_HEIGHT_METERS : KILLER_LINE_SOLDIER_END_METERS;
    return true;
}

// Zoom: Zoom.cpp keeps the game's zoom off the camera and says whether the player is zoomed.
bool GetGameCamera(float* matrix)
{
    if (!_isInstalled || !_sceneCamera || !Xr::IsRunning() || !_didUpdateWithHead || !_isPlayerViewCamera)
    {
        return false;
    }
    memcpy(matrix, _gameCamera, sizeof(_gameCamera));
    matrix[13] -= _liftMeters; // the crouch lift is ours (Update)
    return true;
}

bool GetZoomedHeadCamera(float* headCamera)
{
    if (!_isInstalled || !_sceneCamera || !Xr::IsRunning() || !Zoom::IsZoomed())
    {
        return false;
    }
    FpGuard fp;
    MultiplyMatrices(headCamera, _headPose, _levelCamera);
    return true;
}

bool GetHeadPose(float* matrix)
{
    if (!_isInstalled || !Xr::IsRunning())
    {
        return false;
    }
    memcpy(matrix, _headPose, sizeof(_headPose));
    return true;
}

bool IsScopePass()
{
    return _isScopePass;
}

bool IsPeriscopePass()
{
    return _isScopePass && _isPeriscope;
}

bool IsPeriscopeShown()
{
    return _isPeriscope;
}

bool GetScopeCamera(float* matrix)
{
    if (!_isScopePass)
    {
        return false;
    }
    memcpy(matrix, _scopeFrame, sizeof(_scopeFrame));
    return true;
}

bool IsSceneShown()
{
    return _sceneCamera != nullptr;
}

bool IsSpawnScreen()
{
    return _isInstalled && Xr::IsRunning() && _cameraCaller == _spawnScreenCameraReturn;
}

void SetLift(float meters)
{
    _liftTargetMeters = meters;
}

void SetViewFree(bool isFree)
{
    _isViewFreeWanted = isFree;
}

void TurnView(float rightRadians)
{
    if (_isViewHeld)
    {
        _pendingViewTurnRadians += rightRadians;
    }
}

bool GetCharacterYaw(float* rightRadians)
{
    if (!_isViewHeld)
    {
        return false;
    }
    *rightRadians = _gameYawFromViewRadians;
    return true;
}

bool GetHeadPitch(float* pitchRadians)
{
    if (!Xr::IsRunning())
    {
        return false;
    }
    FpGuard fp;
    const float forwardX = -_headPose[8], forwardY = -_headPose[9], forwardZ = -_headPose[10];
    *pitchRadians = atan2f(forwardY, sqrtf(forwardX * forwardX + forwardZ * forwardZ));
    return true;
}

bool GetGamePitch(float* pitchRadians)
{
    if (!_isViewHeld)
    {
        return false;
    }
    FpGuard fp;
    const float backX = _gameCamera[8], backY = _gameCamera[9], backZ = _gameCamera[10];
    *pitchRadians = atan2f(-backY, sqrtf(backX * backX + backZ * backZ));
    return true;
}

bool IsFirstPersonPass()
{
    return _isFirstPersonPass;
}

bool GetHeadYaw(float* yawRadians)
{
    if (!Xr::IsRunning())
    {
        return false;
    }
    FpGuard fp;
    const float forwardX = -_headPose[8], forwardZ = -_headPose[10];
    if (forwardX * forwardX + forwardZ * forwardZ < 0.04f)
    {
        return false;
    }
    *yawRadians = atan2f(forwardX, -forwardZ);
    return true;
}

bool GetEyePosition(float* position)
{
    if (_isScopePass && _isPeriscope)
    {
        memcpy(position, &_scopeFrame[12], 3 * sizeof(float));
        return true;
    }
    if (_currentEye < 0)
    {
        return false;
    }
    memcpy(position, _eyePositions[_currentEye], sizeof(_eyePositions[0]));
    return true;
}

bool GetHeadWorld(float* matrix)
{
    if (_currentEye < 0)
    {
        return false;
    }
    memcpy(matrix, _headWorld, sizeof(_headWorld));
    return true;
}

bool GetSmoothedHeadWorld(float* matrix)
{
    if (!Xr::IsRunning() || _headWorld[15] == 0)
    {
        return false;
    }
    memcpy(matrix, _smoothHeadWorld, sizeof(_smoothHeadWorld));
    return true;
}

bool GetLastHeadWorld(float* matrix)
{
    if (!Xr::IsRunning() || _headWorld[15] == 0)
    {
        return false;
    }
    memcpy(matrix, _headWorld, sizeof(_headWorld));
    return true;
}

} // namespace Stereo

} // namespace SwbfVr
