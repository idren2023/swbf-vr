#pragma once

/// GameBuild: which Battlefront.exe this is, and where the engine functions and data the mod uses are in it
/// (GameBuild.cpp, CODE-STYLE section 7).
///
/// At start (Identify) the exe is hashed. A known build (today only the Steam build) takes its addresses from its
/// table, and each one is checked against its signature (the game's bytes around it). An unknown build is searched
/// for each signature instead. Every address belongs to one or more features; a feature whose address is missing or
/// fails its check is switched off with one log line, and the rest of the mod keeps working. If a core feature
/// (stereo rendering, renderer limits) is off, VR stays off and the game runs flat.
///
/// The table is written once, in Identify, on the game's main thread before any hook that reads it is installed, and
/// never changes afterwards; any thread may read it.

#include <cstdint>

namespace SwbfVr
{

namespace GameBuild
{

/// A part of the mod that needs engine addresses; switched off as a whole when one of them is missing.
enum class Feature
{
    /// Core: the hooks on Render, RedCamera::SetTransform / SetFov / SetNearFar and the render view list (Stereo).
    Stereo,
    /// Core: the renderer's larger tables (Limits). Without them the headset's wide view crashes the game.
    Limits,
    /// Near clipping per pass and HUD placement, both decided by who called the camera (Stereo).
    CameraCallers,
    /// Aim from the controller: the hook on Aimer::Set (Aim).
    Aim,
    /// First-person arms and weapon placed on the controllers: the hook on FirstPersonRenderable::Render (Aim).
    FirstPersonModel,
    /// The muzzle of the first-person weapon (aiming line, fire origin): the game's name hash (Aim).
    Muzzle,
    /// Analog stick movement: the hooks on Controllable::SetStrafe / SetMove (Controls).
    AnalogMove,
    /// Vibration, recoil and hit flashes: the hooks on the fire, hit and hit-value functions (Feedback).
    Feedback,
    /// Lua in the game's interpreters: the VR Settings screen and the script commands lua / luashell (Lua).
    Lua,
    /// The game's console output written to our log (Lua).
    ConsoleOutput,
    /// Zoom kept off the main camera in VR: the hook on the soldier's GetZoom, the first-person hide patch (Zoom).
    Zoom,
    /// Diagnostic: where the game draws each HUD element (the interface element draws; HudDraws).
    HudDraws,
    /// Holstered weapons drawn on the body and taken by hand: the model light and Controllable::RequestWeapon
    /// (Holster).
    Holsters,
    /// Jet packs steered in VR: the hook on the jet jump and the patched call in the hover code (Jet).
    JetPacks,
    /// Death without camera motion: the death camera's per-frame update skipped in VR (Stereo, PLAN 35).
    DeathView,
    /// Squad commands by hand gesture: the game's squad command and its message (Squad, PLAN 33).
    Squad,
    /// The player's own body in first person: the soldier render, its bone set and the model draw (Body, PLAN 51).
    FullBody,
    /// Head aim in vehicles and turrets: the hooks on Controllable::SetYaw / SetPitch (Vehicle, PLAN 54).
    VehicleAim,
    /// Arms drawn on the tank levers: the game's bone pose table, set up and filled (Aim, PLAN 54 a2).
    VehicleHands,
    /// The droideka's guns aimed where the head looks: the hook on its aim function (Vehicle, PLAN 57 b).
    DroidekaAim,
    /// Props kept in the headset: the prop generator's cluster pool always at its cap (Foliage).
    Foliage,
    Count
};

/// Every engine address the mod uses, in the order of the table in GameBuild.cpp (docs/offsets.md has the
/// details). Addresses are relative to the exe's load address (RVAs).
enum class Address
{
    // Stereo (core)
    Render,
    SetTransform,
    SetFov,
    SetNearFar,
    ViewCameras,
    ViewCount,
    ViewIs3d,
    // Camera callers
    SceneNearFarReturn,
    SceneAfterFarNearFarReturn,
    FirstPersonNearFarReturn,
    SkyNearFarReturn,
    SpawnScreenCameraReturn,
    ChaseCameraUpdate,
    ChaseCameraCodeStart,
    ChaseCameraCodeEnd,
    // Renderer limits (core)
    Flush,
    SortedQueue1800,
    SortedQueue1024,
    SortedQueue2312,
    ListQueue800,
    ListQueue512,
    ItemCount,
    MatrixCount,
    RecordCount,
    Items,
    Matrices,
    Records,
    SubmitItemLimit,
    SubmitItemTable,
    MatrixBlockTable,
    MatrixBlockLimit,
    MatrixSingleTable,
    MatrixSingleLimit,
    MatrixInPoolStart,
    MatrixInPoolEnd,
    RecordAllocatorLimit,
    RecordAllocatorTable,
    RecordInPoolFirst,
    RecordInPoolFirstStart,
    RecordInPoolFirstEnd,
    RecordInPoolSecond,
    RecordInPoolSecondStart,
    RecordInPoolSecondEnd,
    // Aim, first-person model, muzzle
    AimerSet,
    TargetSearchDirectionCall,
    FirstPersonRender,
    FirstPersonRenderables,
    HashName,
    ModelLight,
    ModelByHash,
    RequestWeapon,
    AmmoPowerupEffect,
    // Analog movement
    SetStrafe,
    SetMove,
    SetStrafeReturn,
    SetMoveReturn,
    // Feedback
    WeaponFire,
    RegisterHit,
    HitValue,
    ReticuleUpdate,
    ReticuleUpdateEnd,
    // Lua
    LuaDoBuffer,
    LuaRegister,
    LuaToString,
    MissionScript,
    ShellScript,
    // Console output
    ConsolePrintf,
    ConsolePuts,
    // Zoom
    SoldierGetZoom,
    FirstPersonZoomHide,
    FirstPersonDrawZoomSkip,
    FirstPersonHiddenCall,
    TextureByHash,
    // HUD draws
    ElementDraw,
    BitmapRender,
    GlyphRender,
    MaskedBitmapRender,
    // Jet packs
    JetJump,
    JetHoverMoveCall,
    // Death view
    DeathCameraTable,
    // Squad commands
    SquadCommand,
    SquadMessage,
    SquadDisplayShow,
    // Full body
    SoldierRender,
    SoldierRenderEnd,
    GetBones,
    SoldierVisibilityBranch,
    ModelDraw,
    FirstPersonArmsDrawReturn,
    WeaponModelDrawReturn,
    // Vehicle aim
    SetYaw,
    SetPitch,
    SetYawReturn,
    SetPitchReturn,
    EntityQuery,
    TurretHideChildren,
    WalkerVisible,
    // Vehicle hands
    PoseTableInit,
    PoseTableSet,
    // Droideka aim
    DroidekaAim,
    // Flight: the map's player fly ceiling (float, Lua SetMaxPlayerFlyHeight; read by the flyer's update, 0xEC430)
    MaxPlayerFlyHeight,
    // Foliage: the jle (7E rel8) past the prop cluster pool's cap at level load, 0x201B52 (Foliage)
    PropPoolCapJump,
    Count
};

/// Hashes Battlefront.exe, picks the known build's table or searches the signatures, checks every address and logs
/// the result (one line per feature that is off). Call once, on the game's main thread, after the startup header and
/// before anything that hooks the game (Main.cpp, first DirectInput8Create; the game's code is decrypted by then).
/// Test switches in swbf-vr.ini, read here: `test_unknown_build = 1` treats a known build as unknown (signature
/// search), `test_break_check = <Address name>` makes that address fail its check.
void Identify();

/// Whether the exe is a build with an address table (Steam). False before Identify and in the test_unknown_build test.
bool IsKnownBuild();

/// Whether all addresses of a feature were found and passed their checks. False before Identify.
/// @param feature the feature.
bool IsAvailable(Feature feature);

/// Whether the core features (Stereo, Limits) are available: without them VR must stay off.
bool IsCoreAvailable();

/// Logs (as an error) that VR stays off because a core feature is missing, and asks the user to send the log. Called
/// by Identify and by Xr when a start is requested while the core is not available.
void LogCoreUnavailable();

/// A feature's name for log lines ("stereo rendering (core)", ...).
/// @param feature the feature.
const char* FeatureName(Feature feature);

/// An address relative to the exe's load address (RVA), as in docs/offsets.md.
/// @param address which address.
/// @return the RVA, 0 when the address is missing or failed its check.
uintptr_t Rva(Address address);

/// An address in the running game: exe load address + RVA.
/// @param address which address.
/// @return the address, 0 when it is missing or failed its check.
uintptr_t Absolute(Address address);

/// Whether a pointer lies in the exe's read-only data (.rdata), where the game's vtables are: the test that a
/// pointer is a game C++ object (its first word points there). Build-independent: read from the exe's headers.
/// @param pointer an address in the running game.
bool IsInReadOnlyData(uintptr_t pointer);

} // namespace GameBuild

} // namespace SwbfVr
