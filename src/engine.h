// engine.h: struct offsets of the game that more than one subsystem uses.
//
// Offsets are byte offsets inside a game object, found on the Steam build (Battlefront.exe linked 2020-07-31) and
// assumed to be the same in other builds (no signature can find them). Each entry says what it is, how it was found
// and which section of docs/offsets.md describes it. Offsets that only one subsystem uses live in that subsystem's
// .cpp file. Engine addresses (functions, global data) are all in GameBuild.cpp, per game build.
// Observed means seen in a run or in the disassembly; inferred means deduced and not checked directly.
#pragma once

#include <cstdint>

namespace SwbfVr
{

// ---- First-person model (docs/offsets.md, function table rows 0x4E3008 to 0x130320) ----

// The table of FirstPersonRenderable* per view (index 0 is the player's) is GameBuild::Address::FirstPersonRenderables.
/// FirstPersonRenderable: pointer to the Controllable part of the soldier it belongs to (see
/// OFFSET_soldierControllable). Observed in a memory dump (run29); offsets.md first-person rows.
constexpr uintptr_t OFFSET_firstPersonControllable = 0x1610;
/// FirstPersonRenderable: 4x4 row-vector matrix the arms model is drawn with (world space). Observed (Render 0x130320).
constexpr uintptr_t OFFSET_firstPersonArmsMatrix = 0x1570;
/// FirstPersonRenderable: 4x4 row-vector matrix the weapon model is drawn with (world space). Observed (Render
/// 0x130320).
constexpr uintptr_t OFFSET_firstPersonWeaponMatrix = 0x15B0;
/// FirstPersonRenderable: the weapon object its draw calls (Weapon::Render, vtable slot 23); the draw skips the weapon
/// while it is null (0x130858). Observed in the first-person draw 0x1306E0. During a weapon change it stays the old
/// weapon through the lowering half of the game's swap animation (about 0.3 s, run393).
constexpr uintptr_t OFFSET_firstPersonWeaponObject = 0x1600;
/// FirstPersonRenderable: pointer to the arms pose (skeleton). The pose starts with a pointer to its definition
/// ({bones*, bone count}; bone record 0x70 bytes, name hash at +0x5C) followed at +0x10 by one 4x4 matrix per bone.
/// Observed with the bone log of Aim.cpp (BoneMatrix).
constexpr uintptr_t OFFSET_firstPersonSkeleton = 0x830;

// ---- Soldier, Aimer, weapon (docs/offsets.md, "Object layouts found by the survey") ----

/// Soldier: start of its Controllable base part; pointers to a soldier's Controllable point here, so
/// soldier = controllable - OFFSET_soldierControllable. Observed by the survey of 2026-10-05 (PlayerController +4
/// and the first-person object both point here).
constexpr uintptr_t OFFSET_soldierControllable = 0x250;
/// EntityDroideka: stand / ball mode (0 standing, 11 rolling up, 12 ball, 13 standing up; offsets "Droideka").
constexpr uintptr_t OFFSET_droidekaMode = 0x1E6C;
/// Soldier: its embedded Aimer (fire origin, aim direction, current weapon). Observed by the survey of 2026-10-05.
constexpr uintptr_t OFFSET_soldierAimer = 0x8F0;
/// Soldier: stance as int (0 standing, 1 crouched, 2 prone). Observed by the survey of 2026-10-05.
constexpr uintptr_t OFFSET_soldierStance = 0xA60;
/// Soldier: its body matrix (4x4, rows of 16 bytes); the row at +0x20 is the body's forward, which the jet jump
/// adds the move input along. Read in the jet jump 0x1103F0 (sub-agent, static, 2026-10-13).
constexpr uintptr_t OFFSET_soldierBodyMatrix = 0x1B0;
/// Soldier: pointer to its EntitySoldierClass (the ODF-defined soldier type). Static reading of the class parser
/// 0x116720 (2026-10-13).
constexpr uintptr_t OFFSET_soldierClass = 0x830;
/// Soldier: velocity (3 floats, y up, metres per second). Written by the jet jump and the hover code (static reading,
/// 2026-10-13).
constexpr uintptr_t OFFSET_soldierVelocity = 0x8CC;
/// Soldier: action / state as int; 7 = jetting (state switch of the soldier update 0x118F30, jump table 0x11B4F8;
/// the zoom code reads the same field as the "camera slot", 5 or 8 while zoomed). Static reading, 2026-10-13.
constexpr uintptr_t OFFSET_soldierAction = 0xA64;
constexpr int SOLDIER_ACTION_JETTING = 7;
/// EntitySoldierClass: JetJump (float, the jet jump's impulse) and the byte set by JetType = "hover" (the jet
/// trooper). Static reading of the class parser 0x116720, 2026-10-13.
constexpr uintptr_t OFFSET_soldierClassJetJump = 0x16D8;
constexpr uintptr_t OFFSET_soldierClassJetHover = 0x170C;
/// Aimer: pointer to the weapon currently held. Observed by the survey of 2026-10-05; the fire hook (run77) sees
/// the same weapon as `this`.
constexpr uintptr_t OFFSET_aimerWeapon = 0x128;
/// Weapon: pointer to its WeaponClass (the ODF-defined weapon type, shared by all weapons of that type; +0x40 is
/// its first-person model). Observed (offsets.md first-person rows).
constexpr uintptr_t OFFSET_weaponClass = 0x64;

// ---- Player controller and cameras (docs/offsets.md, "Object layouts found by the survey") ----

/// PlayerController: pointer to the Controllable it steers (EntitySoldier+0x250); null while dead or on the spawn
/// screen. Observed by the survey of 2026-10-05.
constexpr uintptr_t OFFSET_playerControllerControllable = 0x4;
/// CameraManager: the RedCamera (the engine camera Render() draws from), embedded. Observed by the survey of
/// 2026-10-05.
constexpr uintptr_t OFFSET_cameraManagerRedCamera = 0x260;
/// CameraManager: the chase camera (the player's first or third person view), embedded. Observed by the survey.
constexpr uintptr_t OFFSET_cameraManagerChaseCamera = 0x390;
/// RedCamera: camera-to-world 4x4 row-vector matrix (rows right, up, back, position). Observed by the survey;
/// written by RedCamera::SetTransform 0x2913C0.
constexpr uintptr_t OFFSET_cameraMatrix = 0x10;

} // namespace SwbfVr
