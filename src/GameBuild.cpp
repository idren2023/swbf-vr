// GameBuild.cpp: which Battlefront.exe this is, and every engine address the mod uses (CODE-STYLE section 7).
//
// Two tables below:
//  - DEFINITIONS: build-independent. For each address its features and its signature: a byte pattern of the game's
//    code around it ("??" = any byte: call targets and absolute addresses, which change between builds) and how the
//    address follows from where the pattern matches (Find). docs/offsets.md describes each address.
//  - STEAM_ADDRESSES: the Steam build's addresses (RVA) and where each signature matches (site), from
//    tools/signatures.py over re/battlefront-image.bin.
// A known build (hash of Battlefront.exe) takes its table and checks every address: its signature must match at the
// site and lead to the same address. An unknown build searches the exe's code for each signature, which must match
// exactly once. tools/signatures.py replays both on the image dump and reads its patterns from this file, so the
// table lines keep one entry per line.
//
// Offsets inside game objects (OFFSET_ in engine.h and the subsystems) are not in this table: no signature finds
// them, and they are assumed to be the same in every build. The code that uses them checks pointers before reading.

#include "GameBuild.h"

#include "Log.h"
#include "Main.h"

#include <windows.h>

#include <bcrypt.h>
#include <cstdio>
#include <cstring>

namespace SwbfVr
{

namespace GameBuild
{

namespace
{

constexpr int FEATURE_COUNT = static_cast<int>(Feature::Count);
constexpr int ADDRESS_COUNT = static_cast<int>(Address::Count);

/// One bit per feature, so an address can belong to several.
constexpr uint32_t Bit(Feature feature)
{
    return 1U << static_cast<int>(feature);
}

constexpr uint32_t STEREO = Bit(Feature::Stereo);
constexpr uint32_t LIMITS = Bit(Feature::Limits);
constexpr uint32_t CAMERA_CALLERS = Bit(Feature::CameraCallers);
constexpr uint32_t AIM = Bit(Feature::Aim);
constexpr uint32_t FIRST_PERSON_MODEL = Bit(Feature::FirstPersonModel);
constexpr uint32_t MUZZLE = Bit(Feature::Muzzle);
constexpr uint32_t ANALOG_MOVE = Bit(Feature::AnalogMove);
constexpr uint32_t FEEDBACK = Bit(Feature::Feedback);
constexpr uint32_t LUA = Bit(Feature::Lua);
constexpr uint32_t CONSOLE_OUTPUT = Bit(Feature::ConsoleOutput);
constexpr uint32_t ZOOM = Bit(Feature::Zoom);
constexpr uint32_t HUD_DRAWS = Bit(Feature::HudDraws);
constexpr uint32_t HOLSTERS = Bit(Feature::Holsters);
constexpr uint32_t JET_PACKS = Bit(Feature::JetPacks);
constexpr uint32_t DEATH_VIEW = Bit(Feature::DeathView);
constexpr uint32_t SQUAD = Bit(Feature::Squad);
constexpr uint32_t FULL_BODY = Bit(Feature::FullBody);
constexpr uint32_t VEHICLE_AIM = Bit(Feature::VehicleAim);
constexpr uint32_t VEHICLE_HANDS = Bit(Feature::VehicleHands);
constexpr uint32_t DROIDEKA_AIM = Bit(Feature::DroidekaAim);
constexpr uint32_t FOLIAGE = Bit(Feature::Foliage);

/// How an address follows from the place its signature matches (the site).
enum class Find
{
    /// The address is in the matched code: site + offset (a function start, a return address, a constant in code).
    Code,
    /// The matched code holds the address as a 32-bit absolute operand at site + offset (global data).
    Pointer,
    /// The matched code has a call (E8 rel32) at site + offset; the address is its target, after the incremental
    /// linker's jump thunks (E9 rel32, at most 3).
    CallTarget,
    /// No signature: another address (base, earlier in the table) + offset.
    Relative
};

struct Definition
{
    Address address;
    const char* name;
    uint32_t features;
    Find find;
    Address base; // Relative only
    int offset;
    const char* pattern; // null for Relative
};

constexpr Definition Signature(
    Address address,
    const char* name,
    uint32_t features,
    Find find,
    int offset,
    const char* pattern)
{
    return {address, name, features, find, Address::Count, offset, pattern};
}

constexpr Definition Relative(Address address, const char* name, uint32_t features, Address base, int offset)
{
    return {address, name, features, Find::Relative, base, offset, nullptr};
}

// What each address is: docs/offsets.md, sections "Functions", "Render view list", "Clip distances and field of
// view", "Renderer tables", "Player movement input", "Reticule and hit indicator", "Lua", "Scope reticle, spawn
// screen, jetpack gauge". The ranges (ChaseCameraCode..., ReticuleUpdate...End) and the record pool checks are
// kept at their distance from a function found by signature.
// clang-format off
constexpr Definition DEFINITIONS[] = {
    // void __cdecl Render(): draws every pass of a frame from the RedCamera, ends with Present.
    Signature(Address::Render, "Render", STEREO, Find::Code, 0, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC B8 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 56 57 E8 ?? ?? ?? ??"),
    // void __thiscall RedCamera::SetTransform(const float m[16]).
    Signature(Address::SetTransform, "SetTransform", STEREO, Find::Code, 0, "55 8B EC 8B 45 08 0F 10 00 0F 11 41 10 0F 10 40 10 0F 11 41 20 0F 10 40 20 0F 11 41 30 0F 10 40 30 0F 11 41 40 E8 ?? ?? ?? ??"),
    // void __thiscall RedCamera::SetFov(float horizontalFovRadians, float heightOverWidth).
    Signature(Address::SetFov, "SetFov", STEREO, Find::Code, 0, "55 8B EC F3 0F 10 45 08 F3 0F 59 05 ?? ?? ?? ?? 56 51 8B F1 F3 0F 11 04 24 E8 ?? ?? ?? ?? D9 5D 08"),
    // void __thiscall RedCamera::SetNearFar(float near, float far).
    Signature(Address::SetNearFar, "SetNearFar", STEREO, Find::Code, 0, "55 8B EC F3 0F 10 45 08 F3 0F 11 81 90 00 00 00 F3 0F 10 45 0C"),
    // RedCamera* [4], one per render view (read inside Render).
    Signature(Address::ViewCameras, "ViewCameras", STEREO, Find::Pointer, 10, "8D 85 70 FF FF FF 50 8B 0C 8D ?? ?? ?? ??"),
    // uint32: number of render views.
    Signature(Address::ViewCount, "ViewCount", STEREO, Find::Pointer, 5, "55 8B EC 51 A1 ?? ?? ?? ?? 56 33 F6 85 C0"),
    // uint8 [4]: per view, non-zero while the 3D scene is drawn.
    Signature(Address::ViewIs3d, "ViewIs3d", STEREO, Find::Pointer, 2, "C6 86 ?? ?? ?? ?? 01 E8 ?? ?? ?? ?? 66 0F 6E C0 F3 0F E6 C0"),

    // Return address of Render's SetNearFar call for the scene pass.
    Signature(Address::SceneNearFarReturn, "SceneNearFarReturn", CAMERA_CALLERS, Find::Code, 15, "D9 1C 24 8B 0C 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? D8 8D 68 FF FF FF"),
    // Return address of Render's SetNearFar call for the scene pass that follows the distant-terrain pass (near 0.5 m;
    // the main scene on Naboo in run206, where the call above did not run).
    Signature(Address::SceneAfterFarNearFarReturn, "SceneAfterFarNearFarReturn", CAMERA_CALLERS, Find::Code, 15, "8B 0C 9D ?? ?? ?? ?? D9 1C 24 E8 ?? ?? ?? ?? FF 75 CC 57 E8"),
    // Return address of Render's SetNearFar call for the first-person pass (near distance 0.3f in the pattern).
    Signature(Address::FirstPersonNearFarReturn, "FirstPersonNearFarReturn", CAMERA_CALLERS, Find::Code, 16, "D9 5C 24 04 C7 04 24 9A 99 99 3E E8 ?? ?? ?? ??"),
    // the sky pass's SetNearFar (near 2.0f as an immediate); its return (b658: VR moves that near in, the sky domes'
    // mountain rings sit about 2.9 units from the eye and were cut at 46 deg off the view axis, run 873)
    Signature(Address::SkyNearFarReturn, "SkyNearFarReturn", CAMERA_CALLERS, Find::Code, 16, "D9 5C 24 04 C7 04 24 00 00 00 40 E8 ?? ?? ?? ?? 8B 34 9D"),
    // Return address of the spawn screen camera's SetTransform call.
    Signature(Address::SpawnScreenCameraReturn, "SpawnScreenCameraReturn", CAMERA_CALLERS, Find::Code, 10, "75 23 8B 4B 08 E8 ?? ?? ?? ?? 51 8B 4B 08"),
    // The chase camera function that sets the camera both on the spawn screen and in play (inferred from the callers).
    Signature(Address::ChaseCameraUpdate, "ChaseCameraUpdate", CAMERA_CALLERS, Find::Code, 0, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 08 01 00 00 56 57 8B F9 83 7F 0C 00"),
    // Range of return addresses that count as the player's own view (chase camera code); observed in the caller log.
    Relative(Address::ChaseCameraCodeStart, "ChaseCameraCodeStart", CAMERA_CALLERS, Address::ChaseCameraUpdate, -0xA0),
    Relative(Address::ChaseCameraCodeEnd, "ChaseCameraCodeEnd", CAMERA_CALLERS, Address::ChaseCameraUpdate, 0x960),

    // void __cdecl Flush(mask): draws and empties the draw queues.
    Signature(Address::Flush, "Flush", LIMITS, Find::Code, 0, "55 8B EC 53 E8 ?? ?? ?? ?? 8B 5D 08 85 DB"),
    // The sorted draw queues, found in their constructors (capacity in the pattern).
    Signature(Address::SortedQueue1800, "SortedQueue1800", LIMITS, Find::Pointer, 10, "6A 00 6A FF 68 08 07 00 00 B9 ?? ?? ?? ??"),
    Signature(Address::SortedQueue1024, "SortedQueue1024", LIMITS, Find::Pointer, 10, "6A 00 6A FF 68 00 04 00 00 B9 ?? ?? ?? ??"),
    Signature(Address::SortedQueue2312, "SortedQueue2312", LIMITS, Find::Pointer, 10, "6A 00 6A FF 68 08 09 00 00 B9 ?? ?? ?? ??"),
    // The list queues.
    Signature(Address::ListQueue800, "ListQueue800", LIMITS, Find::Pointer, 3, "7F 11 B9 ?? ?? ?? ?? F7 D6 8D 45 D0 56 50"),
    Signature(Address::ListQueue512, "ListQueue512", LIMITS, Find::Pointer, 5, "84 C0 74 30 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 22"),
    // uint32 fill counts: draw items, matrix pool (in the block allocator), light records (in their allocator).
    Signature(Address::ItemCount, "ItemCount", LIMITS, Find::Pointer, 2, "C7 05 ?? ?? ?? ?? 00 00 00 00 85 DB 78 14"),
    Signature(Address::MatrixCount, "MatrixCount", LIMITS, Find::Pointer, 9, "55 8B EC 8B 55 08 56 8B 35 ?? ?? ?? ?? 8B C6"),
    Signature(Address::RecordCount, "RecordCount", LIMITS, Find::Pointer, 2, "8B 15 ?? ?? ?? ?? 8D 04 D5 00 00 00 00 8D 4A 01"),
    // The game's own tables, read from the code that uses them (same sites as the constants below).
    Signature(Address::Items, "Items", LIMITS, Find::Pointer, 11, "8B 45 DC 43 C1 E7 04 8B C8 81 C7 ?? ?? ?? ??"),
    Signature(Address::Matrices, "Matrices", LIMITS, Find::Pointer, 1, "05 ?? ?? ?? ?? 8D 0C 16 81 F9 F6 0B 00 00"),
    Signature(Address::Records, "Records", LIMITS, Find::Pointer, 13, "81 F9 00 08 00 00 1B C9 23 CA 8D 04 C5 ?? ?? ?? ??"),
    // 32-bit constants in the renderer's code that Limits rewrites (limit values 4648, 3062, 2048 in the patterns).
    Signature(Address::SubmitItemLimit, "SubmitItemLimit", LIMITS, Find::Code, 8, "8B 75 0C 89 75 C0 81 FB 28 12 00 00"),
    Signature(Address::SubmitItemTable, "SubmitItemTable", LIMITS, Find::Code, 11, "8B 45 DC 43 C1 E7 04 8B C8 81 C7 ?? ?? ?? ??"),
    Signature(Address::MatrixBlockTable, "MatrixBlockTable", LIMITS, Find::Code, 1, "05 ?? ?? ?? ?? 8D 0C 16 81 F9 F6 0B 00 00"),
    Signature(Address::MatrixBlockLimit, "MatrixBlockLimit", LIMITS, Find::Code, 2, "81 F9 F6 0B 00 00 1B C9 23 CE"),
    Signature(Address::MatrixSingleTable, "MatrixSingleTable", LIMITS, Find::Code, 2, "81 C6 ?? ?? ?? ?? 8D 41 01 3D F6 0B 00 00"),
    Signature(Address::MatrixSingleLimit, "MatrixSingleLimit", LIMITS, Find::Code, 4, "8D 41 01 3D F6 0B 00 00 8B C6"),
    Signature(Address::MatrixInPoolStart, "MatrixInPoolStart", LIMITS, Find::Code, 1, "3D ?? ?? ?? ?? 72 0B 3D ?? ?? ?? ?? 73 04 B0 01 5D C3"),
    Signature(Address::MatrixInPoolEnd, "MatrixInPoolEnd", LIMITS, Find::Code, 1, "3D ?? ?? ?? ?? 73 04 B0 01 5D C3 32 C0 5D"),
    Signature(Address::RecordAllocatorLimit, "RecordAllocatorLimit", LIMITS, Find::Code, 2, "81 F9 00 08 00 00 1B C9 23 CA"),
    Signature(Address::RecordAllocatorTable, "RecordAllocatorTable", LIMITS, Find::Code, 13, "81 F9 00 08 00 00 1B C9 23 CA 8D 04 C5 ?? ?? ?? ??"),
    // The two "is this record in the pool" functions are identical apart from a global, so each is found through a
    // call to it; the table's start and end are constants inside.
    Signature(Address::RecordInPoolFirst, "RecordInPoolFirst", LIMITS, Find::CallTarget, 4, "0F 11 45 8C E8 ?? ?? ?? ?? 8D 85 68 FF FF FF"),
    Relative(Address::RecordInPoolFirstStart, "RecordInPoolFirstStart", LIMITS, Address::RecordInPoolFirst, 9),
    Relative(Address::RecordInPoolFirstEnd, "RecordInPoolFirstEnd", LIMITS, Address::RecordInPoolFirst, 0x11),
    Signature(Address::RecordInPoolSecond, "RecordInPoolSecond", LIMITS, Find::CallTarget, 0, "E8 ?? ?? ?? ?? 8B 85 98 FD FF FF C1 E8 07"),
    Relative(Address::RecordInPoolSecondStart, "RecordInPoolSecondStart", LIMITS, Address::RecordInPoolSecond, 9),
    Relative(Address::RecordInPoolSecondEnd, "RecordInPoolSecondEnd", LIMITS, Address::RecordInPoolSecond, 0x11),

    // void __thiscall Aimer::Set(const float origin[3], const float direction[3]).
    Signature(Address::AimerSet, "AimerSet", AIM, Find::Code, 0, "55 8B EC 8B 45 08 F3 0F 7E 00 66 0F D6 41 7C"),
    // The call (E8 rel32) of the aim direction getter 0x8FF00 in the target search of PlayerController::Update (0x1F77E0,
    // offsets "Reticle target relation"); Aim points it at its own getter (PLAN 48).
    Signature(Address::TargetSearchDirectionCall, "TargetSearchDirectionCall", AIM, Find::Code, 14, "8D 4E 04 51 8D 4D A4 E8 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? 50 8D 4D 84 E8"),
    // void __thiscall FirstPersonRenderable::Render(unsigned flags).
    Signature(Address::FirstPersonRender, "FirstPersonRender", FIRST_PERSON_MODEL, Find::Code, 0, "55 8B EC 56 8B F1 E8 ?? ?? ?? ?? 83 F8 01"),
    // FirstPersonRenderable* per view (index 0 is the player's).
    Signature(Address::FirstPersonRenderables, "FirstPersonRenderables", FIRST_PERSON_MODEL | MUZZLE, Find::Pointer, 3, "8B 0C B5 ?? ?? ?? ?? 85 C9 74 5C 8B 01 53"),
    // unsigned __cdecl HashName(const char*): the hash model nodes and bones are keyed by.
    Signature(Address::HashName, "HashName", MUZZLE, Find::Code, 0, "55 8B EC 56 57 8B 7D 08 8B CF 8D 51 01"),
    // The light set the first-person draw (0x1306E0) passes to its model draws (model vtable slot 1, arms and weapon).
    Signature(Address::ModelLight, "ModelLight", HOLSTERS, Find::Pointer, 12, "8B 11 8D 85 98 FB FF FF 6A 00 57 68"),
    // void* __cdecl ModelByHash(unsigned nameHash): the game's model registry (0x6E59F0) by GeometryName hash; called
    // by the WeaponClass parser for its third-person model (0x243A90). Holster draws dispensed items with it (PLAN 41).
    Signature(Address::ModelByHash, "ModelByHash", HOLSTERS, Find::CallTarget, 8, "8B C8 E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 83 C4 04 89 47 3C 5B 85 C0 0F 84"),
    // void __thiscall Controllable::RequestWeapon(int index): asks for weapon `index` of the soldier's flat weapon array
    // (soldier+0xA30); Controllable::Update 0xC4510 deselects the old one of that channel and selects it. AI code uses it.
    Signature(Address::RequestWeapon, "RequestWeapon", HOLSTERS, Find::Code, 0, "55 8B EC 8B 45 08 89 81 2C 01 00 00 C6 81 50 01 00 00 00 C6 81 64 01"),
    // void __thiscall EntitySoldier::PlayAmmoPowerupEffect(): the class's Powerup_red effect (+0x16FC) on the soldier,
    // as the game plays it when ammo is picked up (0x1FFF60); Holster plays it when a manual reload is done.
    Signature(Address::AmmoPowerupEffect, "AmmoPowerupEffect", HOLSTERS, Find::Code, 0, "56 8B F1 8B 86 30 08 00 00 83 B8 FC 16 00 00 00 74"),

    // void __thiscall Controllable::SetStrafe(float) and SetMove(float), and PlayerController's calls of them.
    Signature(Address::SetStrafe, "SetStrafe", ANALOG_MOVE, Find::Code, 0, "55 8B EC F3 0F 10 45 08 F3 0F 11 41 58"),
    Signature(Address::SetMove, "SetMove", ANALOG_MOVE, Find::Code, 0, "55 8B EC F3 0F 10 45 08 F3 0F 11 41 54"),
    Signature(Address::SetStrafeReturn, "SetStrafeReturn", ANALOG_MOVE, Find::Code, 5, "E8 ?? ?? ?? ?? F3 0F 10 45 C4 51 8B 4F 04"),
    Signature(Address::SetMoveReturn, "SetMoveReturn", ANALOG_MOVE, Find::Code, 5, "E8 ?? ?? ?? ?? F3 0F 10 45 DC 51 8B 4F 04"),

    // The weapon fire function (WeaponCannon vtable slot 28), RegisterHit, HitValue, ReticuleDisplay's update (3184 bytes).
    Signature(Address::WeaponFire, "WeaponFire", FEEDBACK, Find::Code, 0, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC A8 00 00 00 56 57 8B F9 E8 ?? ?? ?? ?? 8B 4F 6C"),
    Signature(Address::RegisterHit, "RegisterHit", FEEDBACK, Find::Code, 0, "55 8B EC 56 FF 75 08 8B F1 8B 8E A4 00 00 00"),
    Signature(Address::HitValue, "HitValue", FEEDBACK, Find::Code, 0, "55 8B EC 8B 45 08 83 EC 08 F3 0F 10 84 81 D8 00 00 00"),
    Signature(Address::ReticuleUpdate, "ReticuleUpdate", FEEDBACK, Find::Code, 0, "55 8B EC 81 EC 88 00 00 00 53 8B D9 56"),
    Relative(Address::ReticuleUpdateEnd, "ReticuleUpdateEnd", FEEDBACK, Address::ReticuleUpdate, 0xC70),

    // int __cdecl lua_dobuffer(lua_State*, const char*, size_t, const char*); Register(lua_State*, name, function);
    // ToString(lua_State*, int), found through a call in the Lua library; the two script object pointers.
    Signature(Address::LuaDoBuffer, "LuaDoBuffer", LUA, Find::Code, 0, "55 8B EC FF 75 14 FF 75 10 FF 75 0C FF 75 08 E8 ?? ?? ?? ?? 83 C4 10 85 C0"),
    Signature(Address::LuaRegister, "LuaRegister", LUA, Find::Code, 0, "55 8B EC 6A 00 FF 75 10 FF 75 08"),
    Signature(Address::LuaToString, "LuaToString", LUA, Find::CallTarget, 10, "51 53 56 8B 75 08 57 6A 01 56 E8 ?? ?? ?? ?? 6A 02 56 8B D8 E8 ?? ?? ?? ?? 6A 03"),
    Signature(Address::MissionScript, "MissionScript", LUA, Find::Pointer, 14, "83 C4 08 80 3D ?? ?? ?? ?? 00 74 6A 8B 0D ?? ?? ?? ??"),
    Signature(Address::ShellScript, "ShellScript", LUA, Find::Pointer, 6, "33 C0 56 8B C8 A3 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B F0 56"),

    // void __cdecl ConsolePrintf(Console*, const char* format, ...) and void __thiscall Console::Puts(const char*).
    Signature(Address::ConsolePrintf, "ConsolePrintf", CONSOLE_OUTPUT, Find::Code, 0, "55 8B EC 81 EC 00 08 00 00 8D 45 10"),
    Signature(Address::ConsolePuts, "ConsolePuts", CONSOLE_OUTPUT, Find::Code, 0, "55 8B EC 83 EC 08 53 57 8B 7D 08 8B D9 89 5D FC"),

    // void __thiscall GetZoom(float*), the soldier's Trackable slot 2: Weapon+0xC0 while zoomed, else 1.
    Signature(Address::SoldierGetZoom, "SoldierGetZoom", ZOOM, Find::Code, 0, "55 8B EC 51 56 8B F1 57 80 BE 20 FE FF FF 00 0F 84"),
    // The jne (0F 85 rel32) in the soldier's first-person camera (slot 1) that skips the first-person arms and weapon's
    // update while zoomed (run103: their draw goes on, their update stops).
    Signature(Address::FirstPersonZoomHide, "FirstPersonZoomHide", ZOOM, Find::Code, 26, "8D 8F B4 FB FF FF E8 ?? ?? ?? ?? 85 C0 0F 88 ?? ?? ?? ?? 80 BF 20 FE FF FF 00 0F 85 ?? ?? ?? ?? 56 50 E8"),
    // The jne (0F 85 rel32) in the first-person draw (0x1306E0, called by FirstPersonRenderable::Render) that skips the
    // draw while the soldier is zoomed (Controllable+0x1C = soldier+0x26C, read by 0x1301D0).
    Signature(Address::FirstPersonDrawZoomSkip, "FirstPersonDrawZoomSkip", ZOOM, Find::Code, 9, "8B CE E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B 86 48 16 00 00"),
    // The call (E8 rel32) in the first-person draw 0x1306E0 to 0x12FFC0(view), "first person hidden": true while the
    // soldier's camera slot (soldier+0xA64, read by 0x1101F0) is 5 or 8, which zooming sets (run106).
    Signature(Address::FirstPersonHiddenCall, "FirstPersonHiddenCall", ZOOM, Find::Code, 6, "FF B6 48 16 00 00 E8 ?? ?? ?? ?? 83 C4 04 84 C0 0F 85"),
    // Texture* __cdecl TextureByHash(unsigned nameHash): the game's loaded textures by name hash (table 0x6E2EF8), 0 if
    // not loaded; called by the HUD bitmap element's texture getter 0x2AF8E0 (the weapon reticle's element).
    Signature(Address::TextureByHash, "TextureByHash", ZOOM, Find::CallTarget, 19, "8B 89 F8 00 00 00 85 C9 75 03 33 C0 C3 E8 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 83 C4 04 C3"),

    // HUD element draws (diagnostic): RedInterfaceElement::Draw(parentMatrix*, parentColour*), the per-element step of
    // the interface tree; RedBitmapElement::Render(world*, colour*); RedGlyphCacheElement::Render(world*, colour*).
    Signature(Address::ElementDraw, "ElementDraw", HUD_DRAWS, Find::Code, 0, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 83 EC 58 A1 ?? ?? ?? ?? 33 C5 89 45 FC 8B 43 08 56 57 8B F9"),
    Signature(Address::BitmapRender, "BitmapRender", HUD_DRAWS, Find::Code, 0, "55 8B EC 51 56 8B F1 89 75 FC 83 BE F8 00 00 00 00"),
    Signature(Address::GlyphRender, "GlyphRender", HUD_DRAWS, Find::Code, 0, "55 8B EC 83 EC 10 53 8B D9 83 BB 8C 00 00 00 00"),
    Signature(Address::MaskedBitmapRender, "MaskedBitmapRender", HUD_DRAWS, Find::Code, 0, "55 8B EC 51 56 8B F1 89 75 FC 8B 8E 00 01 00 00 85 C9 0F 84"),

    // bool __thiscall EntitySoldier::JetJump(): starts a jet jump (fuel check, velocity += direction * JetJump, action 7).
    Signature(Address::JetJump, "JetJump", JET_PACKS, Find::Code, 0, "55 8B EC 83 EC 50 57 8B F9 8B 87 30 08 00 00 F3 0F 10 8F F0 0A 00 00 F3 0F 10 80 EC 16 00 00"),
    // The call (E8 rel32) of the soldier's air movement 0x110BE0 (thiscall, five floats) in the jetting state (7).
    Signature(Address::JetHoverMoveCall, "JetHoverMoveCall", JET_PACKS, Find::Code, 10, "F3 0F 10 45 08 F3 0F 11 04 24 E8 ?? ?? ?? ?? 8B 4D D8 E8"),
    // The DeathCamera's vtable, written by its constructor (0x13DF61: mov [esi], vtable); slot 1 is its per-frame
    // update 0x13FE40 (thiscall, float dt, ret 4; offsets "Death camera").
    Signature(Address::DeathCameraTable, "DeathCameraTable", DEATH_VIEW, Find::Pointer, 19, "55 8B EC 56 FF 75 0C 8B F1 FF 75 08 E8 ?? ?? ?? ?? C7 06 ?? ?? ?? ?? 8B C6 C7 46 6C 00 00 00 00 5E 5D C2 08 00"),

    // void __cdecl SquadCommand(SquadMessage*): sends a squad command to every friendly AI soldier near the issuer
    // (45 m for commands 0-1, 30 m otherwise, x3 in a vehicle), plays the voice line and shows the HUD's squad
    // command display. Called by the soldier update 0x11ABB8 on the squad buttons (offsets "Squad commands").
    Signature(Address::SquadCommand, "SquadCommand", SQUAD, Find::Code, 0, "55 8B EC 83 EC 28 E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 53 57 8B 7D 08 8B 0F E8 ?? ?? ?? ?? 8B D8 89 5D F8 85 DB 0F 84"),
    // SquadMessage* __thiscall SquadMessage::SquadMessage(int command, void* issuer): +0 issuer, +4 command, +8 game
    // time, +0xC entity handle (empty).
    Signature(Address::SquadMessage, "SquadMessage", SQUAD, Find::Code, 0, "55 8B EC 8B 45 0C 56 57 8B F9 89 07 8D 4F 0C 8B 45 08 89 47 04 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 6A 00 8D 4F 0C D9 5F 08 E8"),
    // void __thiscall SquadCommandGroup::Show(int context, int highlighted), ret 8: the HUD's four squad commands
    // (labels from the table 0x62EB90 + context x 16), the highlighted one bright, the others dimmed; restarts its
    // fade (+0xB0 = 1). The group is StatusDisplay+0x30D0 (offsets "Squad commands", disassembly 2026-10-17).
    Signature(Address::SquadDisplayShow, "SquadDisplayShow", SQUAD, Find::Code, 0, "55 8B EC 51 53 56 8B 75 08 8B C1 57 C1 E6 04 33 FF 89 45 FC 81 C6 ?? ?? ?? ?? 8D 98 C0 00 00 00 FF 36 8B CB E8 ?? ?? ?? ?? 3B 7D 0C B9 7F 00 00 00 B8 FF 00 00 00 0F 45 C1"),

    // Full body (PLAN 51, offsets "Soldier body draw"). void __thiscall EntitySoldier render(flag, float, unknown) on
    // soldier+0x17C (vtable 0x416620 slot 3), ret 0xC; its last byte, for return-address checks.
    Signature(Address::SoldierRender, "SoldierRender", FULL_BODY, Find::Code, 0, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 58 06"),
    Relative(Address::SoldierRenderEnd, "SoldierRenderEnd", FULL_BODY, Address::SoldierRender, 0x1FE3),
    // unsigned __thiscall SoldierAnimator::GetBones(BoneSet* out), ret 4: this frame's pose into the render's bone set.
    Signature(Address::GetBones, "GetBones", FULL_BODY, Find::Code, 0, "55 8B EC 83 EC 40 57 8B F9 E8 ?? ?? ?? ?? D9 5D F0 E8 ?? ?? ?? ?? 8B 97"),
    // The jne (75 09) after the visibility check in the soldier render: falling through draws the view's own player
    // with alpha 0 in first person; jmp (EB 09) draws it.
    Signature(Address::SoldierVisibilityBranch, "SoldierVisibilityBranch", FULL_BODY, Find::Code, 4, "FF D0 84 C0 75 09 0F 57 C0 ?? ?? ?? ?? EB 05 F3 0F 10 43 0C F3 0F 11 45"),
    // void __thiscall RedModel draw(matrix, bone set, colour, flags, override), vtable 0x42C6A0 slot 1, ret 0x14.
    Signature(Address::ModelDraw, "ModelDraw", FULL_BODY, Find::Code, 0, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC B8 02 00 00 A1"),
    // Return address of the first-person arms' model draw in FirstPersonRenderable::Render.
    Signature(Address::FirstPersonArmsDrawReturn, "FirstPersonArmsDrawReturn", FULL_BODY, Find::Code, 19, "6A 00 57 68 ?? ?? ?? ?? 50 8D 86 70 15 00 00 50 FF 52 ?? ?? ?? ?? 16 00"),
    // Return address of Weapon::Render's (0x243000) model draw: the third-person weapon inside a soldier render.
    Signature(Address::WeaponModelDrawReturn, "WeaponModelDrawReturn", FULL_BODY, Find::Code, 12, "6A 00 FF 75 ?? ?? ?? ?? 56 FF 50 04 80 7B 03 00 ?? ?? ?? ?? 00 00 0F 10"),

    // void __thiscall Controllable::SetYaw(float) (+0x5C) and SetPitch(float) (+0x60), found through PlayerController's
    // calls of them (their bodies occur more than once in the exe), and the return addresses of those calls.
    Signature(Address::SetYaw, "SetYaw", VEHICLE_AIM, Find::CallTarget, 8, "8B 4F 04 F3 0F 11 04 24 E8 ?? ?? ?? ?? F3 0F 10 45 D8 51"),
    Signature(Address::SetPitch, "SetPitch", VEHICLE_AIM, Find::CallTarget, 14, "F3 0F 10 45 D8 51 8B 4F 04 F3 0F 11 04 24 E8 ?? ?? ?? ?? 8B 4F 24 E8"),
    Signature(Address::SetYawReturn, "SetYawReturn", VEHICLE_AIM, Find::Code, 5, "E8 ?? ?? ?? ?? F3 0F 10 45 D8 51 8B 4F 04"),
    Signature(Address::SetPitchReturn, "SetPitchReturn", VEHICLE_AIM, Find::Code, 19, "F3 0F 10 45 D8 51 8B 4F 04 F3 0F 11 04 24 E8 ?? ?? ?? ?? 8B 4F 24 E8"),
    // int __cdecl EntityQuery(const float center[3], float radius, Entity** out, int most, Entity* exclude, int mask):
    // entities near a point (mask 0 = 0xE; the soldier's enter code finds vehicles and turrets with it). Test command
    // `vehicle enter` (Vehicle).
    Signature(Address::EntityQuery, "EntityQuery", VEHICLE_AIM, Find::Code, 0, "55 8B EC 8B 0D ?? ?? ?? ?? 85 C9 74 2C 8B 45 1C BA 0E 00 00 00"),
    // void __cdecl TurretHideChildren(Model* model, PoseTable* table, Node* node): in a gunner seat in first person the
    // turret's pose (0x1964A0) hides the turret node's children in the hull's pose table with it; it maps each child's
    // index to its hash through the table's skeleton (+0x404), which is null for a hull drawn without its animation
    // (run273: the Imperial fighter tank borrows rep_hover_fightertank's, not loaded on Rhen Var). Vehicle guards it.
    Signature(Address::TurretHideChildren, "TurretHideChildren", VEHICLE_AIM, Find::Code, 0, "55 8B EC 53 8B 5D 08 56 8B 75 10 57 8B 7D 0C 90 8B 03 8B CB 56 FF 50 30"),
    // bool __thiscall EntityWalker Trackable visible(view) (the walker's Trackable E+0x44C vtable 0x4172B8 slot 11;
    // ret 4): false for the view's own vehicle in first person (through 0x228270), which skips the walker's whole render
    // 0x1279B0. Handlebars makes it true for a ridden mount (tauntaun, Kaadu), so the game draws the animal, animated,
    // round the rider in first person (static 2026-11-07; the hover's twin 0xFC8F0 differs at the +0x10C offset).
    Signature(Address::WalkerVisible, "WalkerVisible", VEHICLE_AIM, Find::Code, 0, "55 8B EC 57 8B F9 8D 8F BC FB FF FF E8 ?? ?? ?? ?? 84 C0 75 07 B0 01 5F 5D C2 04 00 80 BF 20 FE FF FF 00 74 ?? 8B 87 0C 01 00 00"),
    // The bone pose table a skinned model is drawn with (hash -> 4x4 matrix, 0x408 bytes): set up empty
    // (PoseTable* __thiscall Init()), and one bone set (void __thiscall Set(unsigned hash, const float* matrix)). Found
    // through the first-person arms draw 0x1306E0, which builds one on its stack from the arms' skeleton, and that
    // builder 0x2ADCA0 (Set is matched at another caller, 0x2C1906: the builder's two calls of it look alike). Aim draws
    // the arms on the tank levers with a table of its own.
    Signature(Address::PoseTableInit, "PoseTableInit", VEHICLE_HANDS, Find::CallTarget, 6, "8D 8D 98 FB FF FF E8 ?? ?? ?? ?? 8D 86 30 08 00 00 50"),
    Signature(Address::PoseTableSet, "PoseTableSet", VEHICLE_HANDS, Find::CallTarget, 12, "8B 4D 08 03 C3 50 8B 47 0C FF 34 B0 E8 ?? ?? ?? ?? 46 83 C3 40 3B 77 10"),

    // void __thiscall EntityDroideka aim (vtable slot 63; one stack argument, ret 4): the aim direction from the body
    // and its pitch (+0x4C4, read at the start), a 512 m ray along it, the hit point kept as the arms' target (Vehicle).
    Signature(Address::DroidekaAim, "DroidekaAim", DROIDEKA_AIM, Find::Code, 0, "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC E8 00 00 00 56 8B F1 57 51 F3 0F 10 86 C4 04 00 00"),
    // the flyer's update near its ceiling: jbe (near), the hull's matrix call, movss xmm0, [MaxPlayerFlyHeight]
    Signature(Address::MaxPlayerFlyHeight, "MaxPlayerFlyHeight", MUZZLE, Find::Pointer, 36, "0F 86 ?? ?? ?? ?? 8B 4D F8 83 EC 08 C7 44 24 04 00 00 80 3F C7 04 24 00 00 00 00 E8 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ??"),
    // the prop generator's pool size at level load (0x201980): cmp eax, 254; jle; mov [count], 254
    Signature(Address::PropPoolCapJump, "PropPoolCapJump", FOLIAGE, Find::Code, 5, "3D FE 00 00 00 7E 0F C7 05"),
};
// clang-format on

/// One address of a known build: the address (RVA) and where its signature matches (RVA of the site; 0 for
/// Relative addresses).
struct KnownAddress
{
    Address address;
    uint32_t rva;
    uint32_t site;
};

// The Steam build (Battlefront.exe linked 2020-07-31), checked by tools/signatures.py.
// clang-format off
constexpr KnownAddress STEAM_ADDRESSES[] = {
    {Address::Render, 0x295340, 0x295340},
    {Address::SetTransform, 0x2913C0, 0x2913C0},
    {Address::SetFov, 0x2912F0, 0x2912F0},
    {Address::SetNearFar, 0x291400, 0x291400},
    {Address::ViewCameras, 0x6ED3C0, 0x2954B7},
    {Address::ViewCount, 0x6ED3D4, 0x292C20},
    {Address::ViewIs3d, 0x6ED400, 0x292C9B},
    {Address::SceneNearFarReturn, 0x2954FA, 0x2954EB},
    {Address::SceneAfterFarNearFarReturn, 0x295D98, 0x295D89},
    {Address::FirstPersonNearFarReturn, 0x295724, 0x295714},
    {Address::SkyNearFarReturn, 0x295CBD, 0x295CAD},
    {Address::SpawnScreenCameraReturn, 0x13EED5, 0x13EECB},
    {Address::ChaseCameraUpdate, 0x13EEA0, 0x13EEA0},
    {Address::ChaseCameraCodeStart, 0x13EE00, 0x0},
    {Address::ChaseCameraCodeEnd, 0x13F800, 0x0},
    {Address::Flush, 0x27C1D0, 0x27C1D0},
    {Address::SortedQueue1800, 0x63B9C4, 0x47BD0},
    {Address::SortedQueue1024, 0x657AB8, 0x47C90},
    {Address::SortedQueue2312, 0x657AC8, 0x47C00},
    {Address::ListQueue800, 0x657AD8, 0x27D017},
    {Address::ListQueue512, 0x63B9B0, 0x27C32C},
    {Address::ItemCount, 0x639E10, 0x27C33E},
    {Address::MatrixCount, 0x639E0C, 0x275390},
    {Address::RecordCount, 0x639E14, 0x2753D0},
    {Address::Items, 0x688010, 0x27C9F0},
    {Address::Matrices, 0x657BF0, 0x2753A2},
    {Address::Records, 0x63B9D8, 0x2753E2},
    {Address::SubmitItemLimit, 0x27C9D8, 0x27C9D0},
    {Address::SubmitItemTable, 0x27C9FB, 0x27C9F0},
    {Address::MatrixBlockTable, 0x2753A3, 0x2753A2},
    {Address::MatrixBlockLimit, 0x2753AC, 0x2753AA},
    {Address::MatrixSingleTable, 0x275421, 0x27541F},
    {Address::MatrixSingleLimit, 0x275429, 0x275425},
    {Address::MatrixInPoolStart, 0x278E77, 0x278E76},
    {Address::MatrixInPoolEnd, 0x278E7E, 0x278E7D},
    {Address::RecordAllocatorLimit, 0x2753E4, 0x2753E2},
    {Address::RecordAllocatorTable, 0x2753EF, 0x2753E2},
    {Address::RecordInPoolFirst, 0x278260, 0x29D55F},
    {Address::RecordInPoolFirstStart, 0x278269, 0x0},
    {Address::RecordInPoolFirstEnd, 0x278271, 0x0},
    {Address::RecordInPoolSecond, 0x278770, 0x28872E},
    {Address::RecordInPoolSecondStart, 0x278779, 0x0},
    {Address::RecordInPoolSecondEnd, 0x278781, 0x0},
    {Address::AimerSet, 0x8CE90, 0x8CE90},
    {Address::TargetSearchDirectionCall, 0x1F77E0, 0x1F77D2},
    {Address::FirstPersonRender, 0x130320, 0x130320},
    {Address::FirstPersonRenderables, 0x4E3008, 0x12FA29},
    {Address::HashName, 0x3007F0, 0x3007F0},
    {Address::ModelLight, 0x6D939C, 0x13083D},
    {Address::ModelByHash, 0x64570, 0x243A88},
    {Address::RequestWeapon, 0xC3540, 0xC3540},
    {Address::AmmoPowerupEffect, 0x1189C0, 0x1189C0},
    {Address::SetStrafe, 0xDA0A0, 0xDA0A0},
    {Address::SetMove, 0xDA0C0, 0xDA0C0},
    {Address::SetStrafeReturn, 0x1F750C, 0x1F7507},
    {Address::SetMoveReturn, 0x1F751F, 0x1F751A},
    {Address::WeaponFire, 0x2461A0, 0x2461A0},
    {Address::RegisterHit, 0x9BBE0, 0x9BBE0},
    {Address::HitValue, 0x9B710, 0x9B710},
    {Address::ReticuleUpdate, 0x208000, 0x208000},
    {Address::ReticuleUpdateEnd, 0x208C70, 0x0},
    {Address::LuaDoBuffer, 0x264120, 0x264120},
    {Address::LuaRegister, 0x18AF60, 0x18AF60},
    {Address::LuaToString, 0xCB560, 0xCB073},
    {Address::MissionScript, 0x4E6DCC, 0x141EF1},
    {Address::ShellScript, 0x62DE8C, 0x20E3DA},
    {Address::ConsolePrintf, 0x61440, 0x61440},
    {Address::ConsolePuts, 0x27FF50, 0x27FF50},
    {Address::SoldierGetZoom, 0x118330, 0x118330},
    {Address::FirstPersonZoomHide, 0x1181A5, 0x11818B},
    {Address::FirstPersonDrawZoomSkip, 0x13072C, 0x130723},
    {Address::FirstPersonHiddenCall, 0x130713, 0x13070D},
    {Address::TextureByHash, 0x74290, 0x2AF8E0},
    {Address::ElementDraw, 0x298C60, 0x298C60},
    {Address::BitmapRender, 0x2AF980, 0x2AF980},
    {Address::GlyphRender, 0x2A4D90, 0x2A4D90},
    {Address::MaskedBitmapRender, 0x2CB9E0, 0x2CB9E0},
    {Address::JetJump, 0x1103F0, 0x1103F0},
    {Address::JetHoverMoveCall, 0x11A9C7, 0x11A9BD},
    {Address::DeathCameraTable, 0x418164, 0x13DF50},
    {Address::SquadCommand, 0x2208F0, 0x2208F0},
    {Address::SquadMessage, 0x10C0B0, 0x10C0B0},
    {Address::SquadDisplayShow, 0x222250, 0x222250},
    {Address::SoldierRender, 0x113690, 0x113690},
    {Address::SoldierRenderEnd, 0x115673, 0x0},
    {Address::GetBones, 0x216E50, 0x216E50},
    {Address::SoldierVisibilityBranch, 0x113705, 0x113701},
    {Address::ModelDraw, 0x288680, 0x288680},
    {Address::FirstPersonArmsDrawReturn, 0x130858, 0x130845},
    {Address::WeaponModelDrawReturn, 0x243046, 0x24303A},
    {Address::SetYaw, 0xDA0E0, 0x1F7525},
    {Address::SetPitch, 0xD9A60, 0x1F7532},
    {Address::SetYawReturn, 0x1F7532, 0x1F752D},
    {Address::SetPitchReturn, 0x1F7545, 0x1F7532},
    {Address::EntityQuery, 0xA2E10, 0xA2E10},
    {Address::TurretHideChildren, 0x196390, 0x196390},
    {Address::WalkerVisible, 0x128680, 0x128680},
    {Address::PoseTableInit, 0x92F10, 0x130743},
    {Address::PoseTableSet, 0x8D1E0, 0x2C18FA},
    {Address::DroidekaAim, 0xDFC90, 0xDFC90},
    {Address::MaxPlayerFlyHeight, 0x4A0B1C, 0xED8C5},
    {Address::PropPoolCapJump, 0x201B52, 0x201B4D},
};
// clang-format on

/// Both tables list every address once, in the order of enum Address (checked by the compiler, below).
constexpr bool IsInOrder()
{
    if (sizeof(DEFINITIONS) / sizeof(DEFINITIONS[0]) != ADDRESS_COUNT ||
        sizeof(STEAM_ADDRESSES) / sizeof(STEAM_ADDRESSES[0]) != ADDRESS_COUNT)
    {
        return false;
    }
    for (int index = 0; index < ADDRESS_COUNT; ++index)
    {
        const Definition& definition = DEFINITIONS[index];
        if (static_cast<int>(definition.address) != index || static_cast<int>(STEAM_ADDRESSES[index].address) != index)
        {
            return false;
        }
        // A Relative address needs its base resolved first.
        if (definition.find == Find::Relative && static_cast<int>(definition.base) >= index)
        {
            return false;
        }
    }
    return true;
}

static_assert(IsInOrder(), "DEFINITIONS and STEAM_ADDRESSES must follow enum Address");

// Game builds the mod knows, by the SHA-256 of Battlefront.exe.
struct KnownGameBuild
{
    const char* sha256;
    const char* name;
    const KnownAddress* addresses;
};

constexpr KnownGameBuild KNOWN_GAME_BUILDS[] = {
    {"deca98fb348310e5e3b6dd74cbffb026a612b0410442c7787d4589b9d845b772", "Steam, Classic 2004", STEAM_ADDRESSES},
};

const char* const FEATURE_NAMES[FEATURE_COUNT] = {
    "stereo rendering (core)",
    "renderer limits (core)",
    "near clipping and HUD placement by camera caller",
    "controller aim",
    "first-person arms and weapon on the controllers",
    "muzzle position (aiming line, fire origin)",
    "analog stick movement",
    "shot and hit feedback",
    "Lua (VR Settings screen, script commands lua and luashell)",
    "console output in the log",
    "zoom kept off the main camera in VR",
    "HUD element draws (diagnostic)",
    "holsters (weapons on the body, taken by hand)",
    "jet packs steered in VR (jump pitch by the head, hover height by the right stick)",
    "death without camera motion (the view stays where the player died)",
    "squad commands by hand gesture",
    "the player's own body in first person",
    "head aim in vehicles and turrets",
    "arms on the tank levers",
    "the droideka's guns aimed where the head looks",
    "props kept in the headset (the prop cluster pool at its cap)",
};

/// The exe is hashed in pieces of this size.
constexpr unsigned HASH_BUFFER_BYTES = 64 * 1024;
/// A SHA-256 digest: 32 bytes, 64 hex digits.
constexpr int SHA256_BYTES = 32;
/// Longest signature, in bytes.
constexpr int MAXIMUM_PATTERN_BYTES = 64;
/// A call target is followed through at most this many jump thunks (E9 rel32), as tools/dis.py does.
constexpr int MAXIMUM_THUNK_STEPS = 3;
constexpr uint8_t CALL_OPCODE = 0xE8;
constexpr uint8_t JUMP_OPCODE = 0xE9;
/// Length of a call or jump with a 32-bit displacement.
constexpr uintptr_t RELATIVE_JUMP_BYTES = 5;

// ---- State (written once in Identify, read-only afterwards) ----

bool _isIdentified = false;
bool _isKnownBuild = false;
bool _isFeatureOn[FEATURE_COUNT];
/// RVA per address; 0 = missing or failed its check.
uintptr_t _rvas[ADDRESS_COUNT];
/// Why an address is missing, for the feature's log line; null when it is fine.
const char* _problems[ADDRESS_COUNT];
uintptr_t _exeBase = 0;
uintptr_t _imageSize = 0;
/// The exe's code (.text) and read-only data (.rdata), as absolute addresses; end exclusive.
uintptr_t _codeStart = 0, _codeEnd = 0, _readOnlyStart = 0, _readOnlyEnd = 0;

/// A parsed signature: bytes, and which of them must match.
struct Pattern
{
    uint8_t bytes[MAXIMUM_PATTERN_BYTES];
    bool isFixed[MAXIMUM_PATTERN_BYTES];
    int length;
};

// SHA-256 of a file as 64 lower-case hex digits; false if the file cannot be read.
bool HashFile(const char* path, char (&hex)[SHA256_BYTES * 2 + 1])
{
    FILE* file = fopen(path, "rb");
    if (!file)
    {
        return false;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool isDone = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
                  BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    static unsigned char buffer[HASH_BUFFER_BYTES];
    size_t length = 0;
    while (isDone && (length = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        isDone = BCryptHashData(hash, buffer, static_cast<ULONG>(length), 0) == 0;
    }
    unsigned char digest[SHA256_BYTES];
    isDone = isDone && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0;
    if (hash)
    {
        BCryptDestroyHash(hash);
    }
    if (algorithm)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    fclose(file);
    for (int byte = 0; isDone && byte < SHA256_BYTES; ++byte)
    {
        snprintf(hex + byte * 2, 3, "%02x", digest[byte]);
    }
    return isDone;
}

// The test switches, read from swbf-vr.ini before Settings reads the whole file (that happens at the first frame,
// too late for this). Same line format as Settings: "name = value".
void ReadTestSwitches(bool& isUnknownForced, char (&brokenName)[64])
{
    isUnknownForced = false;
    brokenName[0] = 0;
    FILE* file = Main::OpenModFile("swbf-vr.ini", "r");
    if (!file)
    {
        return;
    }
    char line[160], name[64], value[64];
    while (fgets(line, sizeof(line), file))
    {
        if (sscanf(line, " %63[a-z_] = %63s", name, value) != 2)
        {
            continue;
        }
        if (strcmp(name, "test_unknown_build") == 0)
        {
            isUnknownForced = strcmp(value, "1") == 0;
        }
        else if (strcmp(name, "test_break_check") == 0)
        {
            strcpy(brokenName, value);
        }
    }
    fclose(file);
}

// The exe's sections from its headers in memory (the loaded image has the same layout as the file's sections).
bool FindSections()
{
    // The headers are C structs at the start of the image: IMAGE_DOS_HEADER, then at e_lfanew IMAGE_NT_HEADERS32.
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(_exeBase);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return false;
    }
    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS32*>(_exeBase + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
    {
        return false;
    }
    _imageSize = ntHeaders->OptionalHeader.SizeOfImage;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(ntHeaders);
    for (unsigned index = 0; index < ntHeaders->FileHeader.NumberOfSections; ++index, ++section)
    {
        const uintptr_t start = _exeBase + section->VirtualAddress;
        const uintptr_t end = start + section->Misc.VirtualSize;
        if (memcmp(section->Name, ".text", 6) == 0)
        {
            _codeStart = start;
            _codeEnd = end;
        }
        else if (memcmp(section->Name, ".rdata", 7) == 0)
        {
            _readOnlyStart = start;
            _readOnlyEnd = end;
        }
    }
    return _codeStart != 0;
}

int HexDigit(char character)
{
    if (character >= '0' && character <= '9')
    {
        return character - '0';
    }
    if (character >= 'A' && character <= 'F')
    {
        return character - 'A' + 10;
    }
    return -1;
}

// "55 8B ?? E8" -> bytes and mask. False for a malformed pattern (a bug in the table, logged by the caller).
bool ParsePattern(const char* text, Pattern& pattern)
{
    pattern.length = 0;
    const char* cursor = text;
    while (*cursor)
    {
        if (*cursor == ' ')
        {
            ++cursor;
            continue;
        }
        if (pattern.length == MAXIMUM_PATTERN_BYTES || !cursor[1])
        {
            return false;
        }
        const int index = pattern.length++;
        if (cursor[0] == '?' && cursor[1] == '?')
        {
            pattern.bytes[index] = 0;
            pattern.isFixed[index] = false;
        }
        else
        {
            const int high = HexDigit(cursor[0]), low = HexDigit(cursor[1]);
            if (high < 0 || low < 0)
            {
                return false;
            }
            pattern.bytes[index] = static_cast<uint8_t>(high * 16 + low);
            pattern.isFixed[index] = true;
        }
        cursor += 2;
    }
    return pattern.length > 0 && pattern.isFixed[0];
}

// Whether the pattern matches the code at an absolute address (the whole pattern must lie inside .text).
bool MatchesAt(const Pattern& pattern, uintptr_t site)
{
    if (site < _codeStart || site + pattern.length > _codeEnd)
    {
        return false;
    }
    const auto* code = reinterpret_cast<const uint8_t*>(site);
    for (int index = 0; index < pattern.length; ++index)
    {
        if (pattern.isFixed[index] && code[index] != pattern.bytes[index])
        {
            return false;
        }
    }
    return true;
}

// Searches .text for the pattern. Returns how many times it matches (counting stops at 2); site = the first match.
// memchr jumps to each occurrence of the pattern's first byte (always fixed, see ParsePattern).
int Search(const Pattern& pattern, uintptr_t& site)
{
    int count = 0;
    const uintptr_t last = _codeEnd - pattern.length;
    uintptr_t cursor = _codeStart;
    while (cursor <= last && count < 2)
    {
        const void* found = memchr(reinterpret_cast<const void*>(cursor), pattern.bytes[0], last - cursor + 1);
        if (!found)
        {
            break;
        }
        const uintptr_t candidate = reinterpret_cast<uintptr_t>(found);
        if (MatchesAt(pattern, candidate))
        {
            if (count++ == 0)
            {
                site = candidate;
            }
        }
        cursor = candidate + 1;
    }
    return count;
}

// The address a matched signature leads to (absolute); 0 if it leads outside the exe.
uintptr_t ResolveSite(const Definition& definition, uintptr_t site)
{
    const uintptr_t at = site + definition.offset;
    switch (definition.find)
    {
    case Find::Code:
        return at >= _codeStart && at < _codeEnd ? at : 0;
    case Find::Pointer:
    {
        // A 32-bit absolute address inside the instruction; memcpy because it need not be aligned.
        uint32_t value = 0;
        memcpy(&value, reinterpret_cast<const void*>(at), 4);
        return value >= _exeBase && value < _exeBase + _imageSize ? value : 0;
    }
    case Find::CallTarget:
    {
        uintptr_t target = at;
        // The call, then up to three jumps: each is opcode + rel32, relative to the next instruction.
        for (int step = 0; step <= MAXIMUM_THUNK_STEPS; ++step)
        {
            const uint8_t opcode = *reinterpret_cast<const uint8_t*>(target);
            if ((step == 0 && opcode != CALL_OPCODE) || (step > 0 && opcode != JUMP_OPCODE))
            {
                break;
            }
            int32_t displacement = 0;
            memcpy(&displacement, reinterpret_cast<const void*>(target + 1), 4);
            target = target + RELATIVE_JUMP_BYTES + static_cast<uintptr_t>(displacement);
            if (target < _codeStart || target >= _codeEnd)
            {
                return 0;
            }
        }
        return target == at ? 0 : target;
    }
    case Find::Relative:
    default:
        return 0;
    }
}

// Finds or checks one address; sets _rvas or _problems. known = the known build's entry, null for a search.
void ResolveAddress(int index, const KnownAddress* known, const char* brokenName)
{
    const Definition& definition = DEFINITIONS[index];
    uintptr_t absolute = 0;
    const char* problem = nullptr;
    uintptr_t site = 0;
    if (definition.find == Find::Relative)
    {
        const uintptr_t baseRva = _rvas[static_cast<int>(definition.base)];
        absolute = baseRva ? _exeBase + baseRva + definition.offset : 0;
        if (!baseRva)
        {
            problem = "the address it is measured from is missing";
        }
        else if (absolute < _codeStart || absolute > _codeEnd)
        {
            problem = "lies outside the code";
            absolute = 0;
        }
    }
    else
    {
        Pattern pattern{};
        if (!ParsePattern(definition.pattern, pattern))
        {
            problem = "its signature is malformed (a bug in the mod)";
        }
        else if (known)
        {
            site = _exeBase + known->site;
            if (!MatchesAt(pattern, site))
            {
                problem = "the game's code there differs from this build's table";
            }
        }
        else
        {
            const int count = Search(pattern, site);
            if (count == 0)
            {
                problem = "signature not found";
            }
            else if (count > 1)
            {
                problem = "signature found more than once";
            }
        }
        if (!problem)
        {
            absolute = ResolveSite(definition, site);
            if (!absolute)
            {
                problem = "its signature leads outside the exe";
            }
        }
    }
    // A known build: the signature must lead exactly to the table's address.
    if (!problem && known && absolute - _exeBase != known->rva)
    {
        problem = "its signature leads elsewhere than this build's table";
    }
    if (!problem && brokenName[0] && strcmp(brokenName, definition.name) == 0)
    {
        problem = "check failed on purpose (test_break_check in swbf-vr.ini)";
    }
    if (problem)
    {
        _problems[index] = problem;
        _rvas[index] = 0;
    }
    else
    {
        _rvas[index] = absolute - _exeBase;
    }
    if (!known)
    {
        if (problem)
        {
            Log::Warning("gamebuild", "%s: %s", definition.name, problem);
        }
        else if (definition.find == Find::Relative)
        {
            Log::Info("gamebuild", "%s at +0x%X", definition.name, static_cast<unsigned>(_rvas[index]));
        }
        else
        {
            Log::Info(
                "gamebuild",
                "%s at +0x%X (signature at +0x%X)",
                definition.name,
                static_cast<unsigned>(_rvas[index]),
                static_cast<unsigned>(site - _exeBase));
        }
    }
}

} // namespace

void Identify()
{
    if (_isIdentified)
    {
        return;
    }
    _exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    bool isUnknownForced = false;
    char brokenName[64];
    ReadTestSwitches(isUnknownForced, brokenName);

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char sha256[SHA256_BYTES * 2 + 1];
    const KnownGameBuild* build = nullptr;
    if (!HashFile(exePath, sha256))
    {
        Log::Warning("gamebuild", "game: %s cannot be read, game build unknown", exePath);
    }
    else
    {
        for (const KnownGameBuild& known : KNOWN_GAME_BUILDS)
        {
            if (strcmp(known.sha256, sha256) == 0)
            {
                build = &known;
            }
        }
        if (build)
        {
            Log::Info("gamebuild", "game: %s, known build, sha256 %s", build->name, sha256);
        }
        else
        {
            Log::Warning(
                "gamebuild",
                "game: unknown build of Battlefront.exe (sha256 %s). The mod is made for the Steam version; its "
                "engine functions are searched by signature, features whose functions are not found turn "
                "themselves off. Please send this log with your report.",
                sha256);
        }
    }
    const KnownGameBuild* const identifiedBuild = build;
    if (build && isUnknownForced)
    {
        Log::Warning(
            "gamebuild",
            "test: test_unknown_build = 1 in swbf-vr.ini, treated as an unknown build (signature search)");
        build = nullptr;
    }
    if (brokenName[0])
    {
        Log::Warning(
            "gamebuild",
            "test: test_break_check = %s in swbf-vr.ini, that check fails on purpose",
            brokenName);
    }
    _isKnownBuild = build != nullptr;

    if (!FindSections())
    {
        Log::Error("gamebuild", "the exe's code section was not found");
    }
    if (_codeStart)
    {
        if (!build)
        {
            Log::Info(
                "gamebuild",
                "searching %d signatures in the code (+0x%X to +0x%X)",
                ADDRESS_COUNT,
                static_cast<unsigned>(_codeStart - _exeBase),
                static_cast<unsigned>(_codeEnd - _exeBase));
        }
        for (int index = 0; index < ADDRESS_COUNT; ++index)
        {
            ResolveAddress(index, build ? &build->addresses[index] : nullptr, brokenName);
        }
    }
    else
    {
        for (int index = 0; index < ADDRESS_COUNT; ++index)
        {
            _problems[index] = "the exe's code section was not found";
        }
    }

    // The test of the search on a known build: every address found must equal the table's.
    if (identifiedBuild && !build && _codeStart)
    {
        int sameCount = 0;
        for (int index = 0; index < ADDRESS_COUNT; ++index)
        {
            const uint32_t expected = identifiedBuild->addresses[index].rva;
            if (_rvas[index] == expected)
            {
                ++sameCount;
            }
            else if (_rvas[index])
            {
                Log::Warning(
                    "gamebuild",
                    "test: %s found at +0x%X, the %s table has +0x%X",
                    DEFINITIONS[index].name,
                    static_cast<unsigned>(_rvas[index]),
                    identifiedBuild->name,
                    static_cast<unsigned>(expected));
            }
        }
        Log::Info(
            "gamebuild",
            "test: %d of %d addresses found by signature equal the %s table",
            sameCount,
            ADDRESS_COUNT,
            identifiedBuild->name);
    }

    // One line per feature that is off: its first failed address and the reason.
    int offCount = 0;
    for (int feature = 0; feature < FEATURE_COUNT; ++feature)
    {
        const uint32_t bit = 1U << feature;
        int firstFailure = -1, failureCount = 0;
        for (int index = 0; index < ADDRESS_COUNT; ++index)
        {
            if ((DEFINITIONS[index].features & bit) != 0 && _problems[index])
            {
                if (firstFailure < 0)
                {
                    firstFailure = index;
                }
                ++failureCount;
            }
        }
        _isFeatureOn[feature] = firstFailure < 0;
        if (firstFailure >= 0)
        {
            ++offCount;
            Log::Warning(
                "gamebuild",
                "feature off: %s. Check %s: %s%s",
                FEATURE_NAMES[feature],
                DEFINITIONS[firstFailure].name,
                _problems[firstFailure],
                failureCount > 1 ? " (and more addresses of this feature)" : "");
        }
    }
    _isIdentified = true;
    if (offCount == 0)
    {
        Log::Info(
            "gamebuild",
            "all %d engine addresses %s, all features on",
            ADDRESS_COUNT,
            build ? "checked against the table" : "found by signature");
    }
    if (!IsCoreAvailable())
    {
        LogCoreUnavailable();
    }
}

bool IsKnownBuild()
{
    return _isKnownBuild;
}

bool IsAvailable(Feature feature)
{
    const int index = static_cast<int>(feature);
    return _isIdentified && index >= 0 && index < FEATURE_COUNT && _isFeatureOn[index];
}

bool IsCoreAvailable()
{
    return IsAvailable(Feature::Stereo) && IsAvailable(Feature::Limits);
}

void LogCoreUnavailable()
{
    Log::Error(
        "gamebuild",
        "VR is off: %s could not be set up on this game build (see the gamebuild lines at the start of this log). "
        "The game runs flat, as without the mod. Please send GameData\\swbf-vr.log with your report.",
        !IsAvailable(Feature::Stereo) ? FEATURE_NAMES[static_cast<int>(Feature::Stereo)]
                                      : FEATURE_NAMES[static_cast<int>(Feature::Limits)]);
}

const char* FeatureName(Feature feature)
{
    const int index = static_cast<int>(feature);
    return index >= 0 && index < FEATURE_COUNT ? FEATURE_NAMES[index] : "?";
}

uintptr_t Rva(Address address)
{
    const int index = static_cast<int>(address);
    return index >= 0 && index < ADDRESS_COUNT ? _rvas[index] : 0;
}

uintptr_t Absolute(Address address)
{
    const uintptr_t rva = Rva(address);
    return rva ? _exeBase + rva : 0;
}

bool IsInReadOnlyData(uintptr_t pointer)
{
    return pointer >= _readOnlyStart && pointer < _readOnlyEnd;
}

} // namespace GameBuild

} // namespace SwbfVr
