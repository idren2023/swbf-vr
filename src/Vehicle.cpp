// Vehicle.cpp: the player in vehicles, turrets and gunner seats (PLAN 54; see Vehicle.h).
//
// The engine (static reading, 2026-10-17, docs/offsets.md "Vehicles, turrets, seats"): PlayerController::Update
// 0x1F7150 hands each frame's look rates to the player's Controllable with two small setters, SetYaw 0xDA0E0 (+0x5C,
// called at 0x1F752D) and SetPitch 0xD9A60 (+0x60, called at 0x1F7540); values are rates, clamped to +-1 from a pad
// and +-16 from the mouse. Every seat class integrates them with its own ODF rates and filters into its aim angles,
// clamps them to its ODF limits, and writes its aim direction to Controllable+0xEC (the body or turret forward turned
// by those angles; in third person a camera ray). The seat class is told by the Controllable's RTTI: the pilot's
// Controllable is the vehicle at +0x250, a turret's is the MountedTurret at +8.
//
// Head aim: both setters are hooked; for PlayerController's calls in a seat with a gun the values are replaced by
// value = error / (TIME_CONSTANT * gain), error = the angle from the seat's aim (+0xEC, as of the last update) to the
// head's forward, per axis (heading right-positive, elevation up-positive). The gain (aim radians per second per unit
// of input, signed) is not known for a modded vehicle, so it is measured while the loop runs: the aim's angular speed
// divided by the input sent the frame before, averaged.

#include "Vehicle.h"

#include "Aim.h"
#include "Flight.h"
#include "FpGuard.h"
#include "GameBuild.h"
#include "GunnerSeat.h"
#include "Handlebars.h"
#include "Holster.h"
#include "Levers.h"
#include "Log.h"
#include "Net.h"
#include "Stereo.h"
#include "WalkerDrive.h"
#include "Xr.h"

#include <windows.h>

#include <MinHook.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace SwbfVr
{

namespace Vehicle
{

namespace
{

/// Controllable: the first of the four inputs PlayerController sets each frame (move, strafe, look yaw, look pitch;
/// offsets "Vehicles, turrets, seats").
constexpr uintptr_t OFFSET_controllableMove = 0x54;
/// Controllable: the seat's aim direction in the world (3 floats), written by the seat class's aim function.
constexpr uintptr_t OFFSET_controllableAim = 0xEC;
/// Controllable of a pilot seat: the vehicle starts this far before it; of a turret or passenger slot, 8.
constexpr uintptr_t OFFSET_vehicleControllable = 0x250, OFFSET_turretControllable = 0x8;
/// A vehicle's health and its most (its Damageable at +8: +4 and +8), a flyer's first weapon (WeaponCannon) and a
/// weapon's heat (0 cool, 1 overheated) (run501, the droid starfighter: 1620 / 1620 at +0xC / +0x10).
constexpr uintptr_t OFFSET_hullHealth = 0xC, OFFSET_flyerWeapon = 0x58C, OFFSET_weaponHeat = 0x90;
/// MountedTurret: the vehicle or building it sits on; its world matrix (offsets "Vehicles, turrets, seats").
constexpr uintptr_t OFFSET_turretParent = 0x2C8, OFFSET_turretMatrix = 0x210;
/// Entity: its world matrix (4x4, row 2 = forward; docs/offsets.md "EntitySoldier", JetJump).
constexpr uintptr_t OFFSET_entityMatrix = 0x1B0;
// A flyer's velocity: entity +0x4DC (3 floats, world, m/s; run576), written by the game each update (run577: our y
// write did nothing); +0x5A4: 0 landed, 16.4 hovering (run578), not a target height (run579).
/// MountedTurret: its Aimers, one per weapon, T+0x290 + 4i (offsets "Vehicles, turrets, seats").
constexpr uintptr_t OFFSET_turretAimers = 0x290;
constexpr int TURRET_AIMER_SLOTS = 4;
/// EntityDroideka: its Aimers, E+0x4C8 + 4i (heap; offsets "Droideka"). The stock droideka's one weapon has two
/// (ODF AimerNodeName hp_gun_1, hp_gun_2: one per arm).
constexpr uintptr_t OFFSET_droidekaAimers = 0x4C8;
constexpr int DROIDEKA_AIMER_SLOTS = 2;
/// Aimer (offsets "Aimer"): a link to another Aimer (+0x18, followed only to Aimers of the same owner), its owner
/// (+0x20, whose target point it aims at), the aim direction (+0x48, unit, world) and the fire point (+0x7C, world):
/// what the fire code reads for a shot (0x24B040).
constexpr uintptr_t OFFSET_aimerNext = 0x18, OFFSET_aimerOwner = 0x20, OFFSET_aimerDirection = 0x48,
                    OFFSET_aimerFirePoint = 0x7C;
/// An Aimer is read up to its fire point (+0x7C, 3 floats).
constexpr size_t AIMER_READ_BYTES = 0x88;
constexpr int MOST_SEAT_AIMERS = 8;
/// Links followed from each slot's Aimer.
constexpr int MOST_AIMER_LINKS = 4;
/// The seat's Aimers are looked for again this often (a weapon's Aimers may come later than the seat).
constexpr DWORD AIMER_REFRESH_MILLISECONDS = 1000;
/// The trace (`vehicle trace`): a line this often.
constexpr DWORD TRACE_INTERVAL_MILLISECONDS = 50;
/// A matrix row counts as a unit vector within this.
constexpr float UNIT_TOLERANCE = 0.05f;

/// MountedTurret update (0x1974F0, `this` = T+0x1EC; static reading 2026-10-18, PLAN 56): its aim angles, pitch
/// T+0x2B8 (negative = up) and yaw T+0x2C0, relative to the parent's matrix (the parent's vtable slot 14; a hover's
/// is its entity matrix +0x1B0), each with a filtered rate after it (+0x2BC, +0x2C4). Each update, with a player in
/// the seat (Controllable +0xD4 >= 0): rate = lerp(rate, -input * Rate, min(1, Filter * dt)); angle = actual +
/// (angle - actual) * |input|, actual = the angle of the turret's forward (T+0x210 row 2) in the parent's frame
/// (yaw atan2(x, z), pitch -asin(y)); angle += rate * dt; clamped to the section's limits; T+0x210 and the aim
/// (Controllable +0xEC = its row 2) are built from the angles. So with no input the turret keeps its direction in the
/// world while the hull moves, and the stock fighter tank's PitchFilter 1.0 lags the pitch rate by a second: the rate
/// loop overshot there (run267: head +20 deg, gun 29-31).
constexpr uintptr_t OFFSET_turretAngles = 0x2B8; // pitch, pitch rate, yaw, yaw rate
constexpr uintptr_t OFFSET_turretSection = 0x2CC;
/// Turret section (ODF): yaw limits (min, max) at +0xE30, the pitch limits right after them (+0xE38), radians (the
/// update clamps the pitch with +0xE38 / +0xE3C, run269); a pair applies only inside (-pi, pi), otherwise the angle
/// wraps.
constexpr uintptr_t OFFSET_sectionLimits = 0xE30;
/// The parent's vtable slot that returns its world matrix (the turret update calls it first).
constexpr int SLOT_PARENT_MATRIX = 14;
/// The game's own step after our write is measured; a larger one is a reset of the game's (seat change), not a step.
constexpr float LARGEST_GAME_STEP_RADIANS = 0.2f;
/// The input's sign (which way the game's rate filter pulls) changes only past this speed of ours (run 937, idren2023: the
/// grey gun turret shook at rest: our speed near 0 flipped the input every update, the game's step flipped with it,
/// and the measured step, taken off the next write, then had the wrong sign: about 0.7 deg back and forth).
constexpr float INPUT_SIGN_SPEED_RADIANS = 0.01f;
/// Limits at least this close to -pi..pi count as none (the game's test: 0x420230 / 0x420224).
constexpr float LIMIT_RANGE_RADIANS = 3.1414f;

/// Head aim: the aim closes the angle to the head with this time constant.
constexpr float TIME_CONSTANT_SECONDS = 0.15f;
/// Head aim of a turret in a network game (see where it is used): time constant, and the share of the aim's own
/// turning speed taken off the wanted speed.
constexpr float NET_TURRET_TIME_CONSTANT_SECONDS = 0.25f, NET_TURRET_DAMPING = 1.0f;
/// Head aim: the largest input sent (the mouse's range).
constexpr float LARGEST_INPUT = 16.0f;
/// Gain measurement: only frames with at least this much input count; samples beyond the second are dropped; the
/// average moves this far toward each sample; the gain is kept at least this large (radians per second per unit).
constexpr float GAIN_SAMPLE_INPUT = 0.2f, GAIN_SAMPLE_LIMIT = 100.0f, GAIN_SMOOTHING = 0.05f, SMALLEST_GAIN = 0.2f;
/// Gain measurement: the aim must turn at least this fast for a frame to count.
constexpr float MOVING_RADIANS_PER_SECOND = 0.02f;
/// Gain at a seat change: heading turns right and elevation goes up for a positive input (run218, a building
/// turret: a negative pitch input took the gun down to its limit). Only the size is measured; the sign stays.
constexpr float INITIAL_YAW_GAIN = 1.0f, INITIAL_PITCH_GAIN = 1.0f;
/// A pilot seat's hull turned toward the head: the heading error closed with this time constant, at most this fast
/// (headset run 19: 45 deg/s; the stock tanks turn about that fast on the keys).
constexpr float HULL_STEER_SECONDS = 0.6f, HULL_STEER_RADIANS_PER_SECOND = 45.0f / 57.29578f;
/// Longer steps between two updates are taken as a pause: no gain sample, no input.
constexpr float LONGEST_STEP_SECONDS = 0.25f;
constexpr DWORD LOG_INTERVAL_MILLISECONDS = 1000;
constexpr float DEGREES_PER_RADIAN = 57.29578f;
constexpr float PI = 3.14159265f;

using SetAxisFunction = void(__fastcall*)(void* self, void* edx, float value);
SetAxisFunction _originalSetYaw = nullptr, _originalSetPitch = nullptr;
using HideChildrenFunction = void(__cdecl*)(void* model, uintptr_t table, void* node);
HideChildrenFunction _originalHideChildren = nullptr;

/// The droideka's aim function (EntityDroideka vtable slot 63, 0xDFC90; static reading 2026-10-18, PLAN 57 b):
/// bool __thiscall on the entity E with one stack argument (ret 4; true, `mov al, 1`), received as __fastcall. It sets
/// the aim direction
/// E+0x33C (Controllable+0xEC) from the body's heading and its pitch, casts a 512 m ray from E+0x254 (Controllable+4)
/// along it (the world ray cast 0xA40C0, mask 0x4E, the droideka itself ignored) and keeps the hit point at E+0x260
/// (Controllable+0x10): the point its arms' Aimers turn toward, within the ODF's AimerYawLimits +-40 and
/// AimerPitchLimits +-25 deg of the body (offsets "Droideka"). run308 (trace): the arms follow the body within 0.2 deg,
/// and the body turns at about 35 deg/s whatever the error, the game's turn limit (our loop asks for up to 720), so a
/// 40 deg head turn took 1.2 s to reach the guns (headset run 16: "bolts take a moment to catch up").
using DroidekaAimFunction = bool(__fastcall*)(void* entity, void* edx, int argument);
DroidekaAimFunction _originalDroidekaAim = nullptr;
/// EntityDroideka: the aim ray's start (Controllable+4) and the arms' target point (Controllable+0x10); the ray's
/// length.
constexpr uintptr_t OFFSET_droidekaRayStart = 0x254, OFFSET_droidekaTarget = 0x260;
constexpr float DROIDEKA_RAY_METERS = 512.0f;
/// The trace (`vehicle trace`): when it ends, when its last line was written.
DWORD _traceEndTick = 0, _traceTick = 0;
/// Pose table: its skeleton (bone index -> hash), set when the model's animation is set up (0x2AE140 reads it).
constexpr uintptr_t OFFSET_poseTableSkeleton = 0x404;
uintptr_t _setYawReturn = 0, _setPitchReturn = 0;
// Network game: what the seat's hooks would have set, kept for the control state of the next turn (game thread).
NetSeatInputs _netSeat = {};
DWORD _netSeatTick = 0;
bool _areHooksTried = false;

bool _isHeadAimOn = true; // the view holds the hull's heading (Stereo HoldHullHeading), so the aim does not chase it
bool _isLogOn = true;
/// Test `vehicle enter`: the seat kind wanted (Seat::Other = any), or -1; taken by the next SetPitch call.
volatile LONG _enterRequest = -1;
/// Test `vehicle enter <kind>+`: only a vehicle with at least one gunner seat (MountedTurret).
volatile bool _isGunnerSeatWanted = false;
/// Test `vehicle enter <kind> <odf name>`: only that vehicle; empty for any.
char _enterName[64] = "";
/// Entity: the count (byte) of its MountedTurrets, hover / flyer (offsets "Vehicles, turrets, seats"; walker not
/// known).
constexpr uintptr_t OFFSET_hoverTurretCount = 0x540, OFFSET_flyerTurretCount = 0x574;
/// Test `vehicle enter`: entities within this distance of the player are tried (farther for one vehicle by name).
constexpr float ENTER_SEARCH_METERS = 150.0f, ENTER_NAMED_SEARCH_METERS = 1000.0f;
constexpr int ENTER_SEARCH_MOST = 128;
/// Controllable vtable: bool CanEnter(Entity*) at slot 16, bool Enter(Entity*) at slot 17 (the soldier's: 0x10CE10,
/// 0x10EB50; offsets "Vehicles, turrets, seats"). Enter takes the vehicle's first free seat, the pilot's first.
constexpr int SLOT_CAN_ENTER = 16, SLOT_ENTER = 17;
/// Controllable: the eye position (3 floats), the soldier's ray origin; and the point that ray hit (the aim's target).
constexpr uintptr_t OFFSET_controllableEye = 0x4, OFFSET_controllableTarget = 0x10;
/// Parallax: the target point counts from this far from the head (closer: the ray hit the vehicle itself) and the ray's
/// start from no further than this (else it is not the seat's camera and the head's direction is used as is).
constexpr float NEAREST_TARGET_METERS = 3.0f, FARTHEST_RAY_START_METERS = 12.0f;
using EntityQueryFunction =
    int(__cdecl*)(const float* center, float radius, uintptr_t* out, int most, uintptr_t exclude, int mask);
using EntityCallFunction = bool(__fastcall*)(void* self, void* edx, void* entity);

/// Stock vehicles with their own way of driving (idren2023 2026-10-17: sticks by default for modded vehicles, overrides
/// for the stock ones). Keyed by ODF class name; the class is found by its name hash inside the vehicle's class object
/// (no name strings are kept there, offsets.md "Soldier weapons" row; the hash's place is looked for, see
/// FindStockVehicle).
struct StockVehicle
{
    const char* odfName;
    Drive drive;
};

/// The stock turrets (armed buildings) the mod knows (PLAN 58 u, docs/research/turrets.md): their seat's
/// CurrentStockName is the building's ODF name.
const char* const STOCK_TURRETS[] = {"com_weap_gunturret",
                                     "tat_bldg_barge_turret",
                                     "bes_bldg_AAA_turret",
                                     "hoth_bldg_hoth_turret",
                                     "hoth_bldg_dish_turret",
                                     "geo_bldg_geoturret"};
/// A building's class object is not at a known offset: the first this many bytes of the building are searched for a
/// pointer to a class holding one of the names above (logged with its offset).
constexpr uintptr_t BUILDING_CLASS_SEARCH_BYTES = 0x800;

const StockVehicle STOCK_VEHICLES[] = {
    {"cis_hover_aat", Drive::Levers},
    {"cis_hover_mtt", Drive::Levers},
    {"rep_hover_fightertank", Drive::Levers},
    {"imp_hover_fightertank", Drive::Levers},
    {"all_hover_combatspeeder", Drive::Levers},
    {"imp_hover_speederbike", Drive::Handlebars},
    {"rep_hover_speederbike", Drive::Handlebars},
    {"cis_hover_stap", Drive::Handlebars},
    {"all_fly_xwing", Drive::FlightStick}, // the single-pilot starfighters (PLAN starfighters): their cockpit shells
    {"imp_fly_tiefighter", Drive::FlightStick},
    {"rep_fly_jedifighter", Drive::FlightStick},
    {"cis_fly_droidfighter", Drive::FlightStick},
    {"cis_fly_geofighter", Drive::FlightStick},
    {"all_fly_ywing", Drive::FlightStick}, // pilot seats with our cockpits (their gunner seats stay the game's)
    {"imp_fly_tiebomber", Drive::FlightStick},
    // walkers (the vehicle proposal 2026-11-04): the throttle lever and head steering
    {"imp_walk_atst", Drive::Throttle},
    {"imp_walk_atst_snow", Drive::Throttle},
    {"imp_walk_atst_jungle", Drive::Throttle},
    {"imp_walk_atat", Drive::Throttle},
    {"rep_walk_atte", Drive::Throttle},
    {"cis_walk_spider", Drive::Throttle},
    {"all_walk_tauntaun", Drive::Handlebars}, // mounts: ridden as the speeder bikes (reins, lean; Handlebars)
    {"gun_walk_kaadu", Drive::Handlebars},
    {"cis_tread_hailfire", Drive::Throttle}, // a hover: the throttle lever, its hull turned to the head by the yaw loop
    {"all_fly_snowspeeder", Drive::FlightStick},
    {"rep_fly_gunship", Drive::FlightStick},
    {"cis_fly_maf", Drive::FlightStick},
};
/// Vehicle entity: its class object, by pilot seat class (offsets "Vehicles, turrets, seats").
constexpr uintptr_t OFFSET_hoverClass = 0x500, OFFSET_flyerClass = 0x534, OFFSET_walkerClass = 0x4E0;
/// How much of a class object is searched for its name hash (the hover class's sections end near 0x2210).
constexpr uintptr_t CLASS_SEARCH_BYTES = 0x2400;
using HashNameFunction = unsigned(__cdecl*)(const char*);

volatile Seat _seat = Seat::None;
volatile Drive _drive = Drive::Sticks;
const char* _stockName = nullptr; // the table entry's name of the pilot seat's vehicle, or null
uintptr_t _seatControllable = 0;  // the Controllable _seat was worked out for

/// One aim axis of the loop.
struct Axis
{
    float gain;      // aim radians per second per unit of input, signed (measured)
    float lastAngle; // the aim's angle at the previous update
    float lastInput; // the input sent at the previous update
    float gameInput; // the game's own input this update (for the log)
    float input;     // the input sent this update
    float errorRadians;
    bool hasLast;
};

Axis _yaw = {INITIAL_YAW_GAIN, 0, 0, 0, 0, 0, false}, _pitch = {INITIAL_PITCH_GAIN, 0, 0, 0, 0, 0, false};
LARGE_INTEGER _lastUpdate = {};
bool _isAiming = false; // the loop sends the inputs this update
bool _isRemoteDroid = false; // IsRemoteDroid

/// A turret seat's aim, held in the turret's own angles (pitch, yaw: radians, as MountedTurret +0x2B8 / +0x2C0).
struct TurretAim
{
    bool hasState;
    float angles[2];   // smoothed toward the head
    float written[2];  // written last update
    float gameStep[2]; // what the game's update added after our write (its rate times its step), measured
    float target[2];   // the head's direction, within the limits (for the log)
    float rayStartMeters, targetMeters; // the game's aim ray's start and its target point from the head (for the log)
    float aimMeters;                    // the target point's distance as aimed at: smoothed (ParallaxAim)
    bool isBeyondReach;                 // the head looked past a yaw stop last update: aimed along the head
    bool hasHandYaw;                    // a turret aimed by the right hand (StepTurretByHand): the yaw it steers to
    float handYaw;
    float inputSign[2]; // the input sent last update (pitch, yaw): -1 or 1, 0 before the first
};

/// How fast the distance ParallaxAim aims at follows the game's target point (per update; run362: taken at once, the
/// target point, which moves with the aim, swung the aim 10-20 deg between updates).
constexpr float AIM_DISTANCE_FOLLOW = 0.08f;

TurretAim _turret = {};
using MatrixFunction = const float*(__fastcall*)(void* self, void* edx);

Seat SeatOf(uintptr_t controllable)
{
    if (Holster::IsObjectType(controllable, "EntitySoldier") || Holster::IsObjectType(controllable, "EntityDroideka"))
    {
        return Seat::OnFoot;
    }
    if (Holster::IsObjectType(controllable, "EntityHover") || Holster::IsObjectType(controllable, "EntityCommandHover"))
    {
        return Seat::Hover;
    }
    if (Holster::IsObjectType(controllable, "EntityFlyer") || Holster::IsObjectType(controllable, "EntityCarrier"))
    {
        return Seat::Flyer;
    }
    if (Holster::IsObjectType(controllable, "EntityWalker") ||
        Holster::IsObjectType(controllable, "EntityCommandWalker") || Holster::IsObjectType(controllable, "CommandWalker"))
    {
        return Seat::Walker;
    }
    if (Holster::IsObjectType(controllable, "MountedTurret"))
    {
        return Seat::Turret;
    }
    if (Holster::IsObjectType(controllable, "PassengerSlot"))
    {
        return Seat::Passenger;
    }
    return Seat::Other;
}

const char* SeatName(Seat seat)
{
    switch (seat)
    {
    case Seat::None:
        return "none";
    case Seat::OnFoot:
        return "on foot";
    case Seat::Hover:
        return "hover pilot";
    case Seat::Flyer:
        return "flyer pilot";
    case Seat::Walker:
        return "walker pilot";
    case Seat::Turret:
        return "turret";
    case Seat::Passenger:
        return "passenger";
    case Seat::Other:
        return "other";
    }
    return "?";
}

float Wrap(float radians)
{
    return radians > PI ? radians - 2 * PI : radians < -PI ? radians + 2 * PI : radians;
}

// Heading (right-positive, from -z) and elevation (up-positive) of a world direction.
void Angles(const float* direction, float* heading, float* elevation)
{
    *heading = atan2f(direction[0], -direction[2]);
    *elevation = atan2f(direction[1], sqrtf(direction[0] * direction[0] + direction[2] * direction[2]));
}

// One axis of the loop: the measured gain from the last step, then this step's input. FpGuard held.
void StepAxis(
    Axis& axis, float angle, float targetAngle, float stepSeconds, float timeConstantSeconds = TIME_CONSTANT_SECONDS,
    float damping = 0.0f)
{
    // How fast the aim is turning now (for the damping).
    const float turningSpeed = axis.hasLast && stepSeconds > 0 ? Wrap(angle - axis.lastAngle) / stepSeconds : 0.0f;
    if (axis.hasLast && fabsf(axis.lastInput) >= GAIN_SAMPLE_INPUT)
    {
        // A gun at its limit does not move: such frames say nothing about the gain (run218: the gain fell to its
        // floor while the gun sat at its lowest pitch).
        const float speed = Wrap(angle - axis.lastAngle) / stepSeconds;
        const float sample = speed / axis.lastInput * (axis.gain < 0 ? -1.0f : 1.0f);
        if (fabsf(speed) > MOVING_RADIANS_PER_SECOND && sample > 0 && sample < GAIN_SAMPLE_LIMIT)
        {
            const float size = fabsf(axis.gain) + (sample - fabsf(axis.gain)) * GAIN_SMOOTHING;
            const float kept = size < SMALLEST_GAIN ? SMALLEST_GAIN : size;
            axis.gain = axis.gain < 0 ? -kept : kept;
        }
    }
    axis.errorRadians = Wrap(targetAngle - angle);
    float input = (axis.errorRadians / timeConstantSeconds - damping * turningSpeed) / axis.gain;
    input = input > LARGEST_INPUT ? LARGEST_INPUT : input < -LARGEST_INPUT ? -LARGEST_INPUT : input;
    axis.input = input;
    axis.lastAngle = angle;
    axis.lastInput = input;
    axis.hasLast = true;
}

// The turret's parent's world matrix as the turret's update reads it (vtable slot 14), or null when it is not a
// readable rotation. Game thread.
const float* TurretParentMatrix(uintptr_t turret)
{
    if (IsBadReadPtr(reinterpret_cast<void*>(turret + OFFSET_turretParent), 4))
    {
        return nullptr;
    }
    const uintptr_t parent = *reinterpret_cast<const uintptr_t*>(turret + OFFSET_turretParent);
    if (parent < 0x10000 || IsBadReadPtr(reinterpret_cast<void*>(parent), 4))
    {
        return nullptr;
    }
    void** const vtable = *reinterpret_cast<void***>(parent);
    if (IsBadReadPtr(static_cast<const void*>(vtable + SLOT_PARENT_MATRIX), 4))
    {
        return nullptr;
    }
    const auto getMatrix = reinterpret_cast<MatrixFunction>(vtable[SLOT_PARENT_MATRIX]);
    const float* matrix = getMatrix(reinterpret_cast<void*>(parent), nullptr);
    if (!matrix || IsBadReadPtr(matrix, 64))
    {
        return nullptr;
    }
    for (int row = 0; row < 3; ++row)
    {
        const float* axis = matrix + row * 4;
        if (!(fabsf(sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]) - 1.0f) <= UNIT_TOLERANCE))
        {
            return nullptr;
        }
    }
    return matrix;
}

// A turret seat's head aim (PLAN 56): the turret's angles are written directly, as the angles of the head's forward
// in the parent's frame (the game's own formulas, so no sign or axis is assumed), smoothed with TIME_CONSTANT_SECONDS
// and held within the section's limits; the inputs sent are +-1, the one size at which the game's update keeps the
// written angle instead of blending it toward the turret's last direction. The game then adds its filtered rate
// times its step; that rate is written as our own speed (the turret's motor sound follows it, inferred from the
// update's use of |rate|), and the step the game adds is measured each update and taken off the next write.
// Returns false when the turret's memory or its parent's matrix cannot be read (the rate loop is used). FpGuard held.
// PLAN 57 h3 (idren2023 2026-10-19: aiming as in the original game, with the head): the game aims a seat's guns at the
// point its own ray hits, from Controllable+4 along the aim +0xEC, and its Aimers converge there (+0x10); in a gunner
// seat that ray starts at the game's camera (the turret matrix x the seat's EyePointOffset, the AAT's (0, 2, -1.75)),
// not at the VR eye, so aimed along the head's direction the shots landed off the head's line of sight up close
// (above it in the AAT, whose camera sits about 1.9 m over our cabin eye; inferred). The direction aimed along is
// therefore the one from the ray's start to the point on the head's line of sight as far from the head as the game's
// last target point: the game's ray then passes there, and the shots land where the head looks, on every machine (only
// the turret's angles are ours, as before). FpGuard held. Returns the direction to aim along (unit, world).
void ParallaxAim(uintptr_t controllable, const float* headPosition, const float* headForward, float* direction)
{
    memcpy(direction, headForward, 12);
    if (!headPosition || IsBadReadPtr(
                             reinterpret_cast<const void*>(controllable + OFFSET_controllableEye),
                             OFFSET_controllableTarget + 12))
    {
        return;
    }
    const float* start = reinterpret_cast<const float*>(controllable + OFFSET_controllableEye);
    const float* target = reinterpret_cast<const float*>(controllable + OFFSET_controllableTarget);
    float startFromHead = 0, distance = 0;
    for (int axis = 0; axis < 3; ++axis)
    {
        startFromHead += (start[axis] - headPosition[axis]) * (start[axis] - headPosition[axis]);
        distance += (target[axis] - headPosition[axis]) * (target[axis] - headPosition[axis]);
    }
    startFromHead = sqrtf(startFromHead);
    distance = sqrtf(distance);
    _turret.rayStartMeters = startFromHead;
    _turret.targetMeters = distance;
    if (!(startFromHead < FARTHEST_RAY_START_METERS) || !(distance > NEAREST_TARGET_METERS))
    {
        return;
    }
    // Beyond the turret's reach the game's target point is not on the head's line at all: aimed along the head.
    if (_turret.isBeyondReach)
    {
        _turret.aimMeters = 0;
        return;
    }
    _turret.aimMeters =
        _turret.aimMeters > 0 ? _turret.aimMeters + (distance - _turret.aimMeters) * AIM_DISTANCE_FOLLOW : distance;
    distance = _turret.aimMeters;
    float toPoint[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        toPoint[axis] = headPosition[axis] + headForward[axis] * distance - start[axis];
    }
    const float length = sqrtf(toPoint[0] * toPoint[0] + toPoint[1] * toPoint[1] + toPoint[2] * toPoint[2]);
    if (length > 1.0f)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            direction[axis] = toPoint[axis] / length;
        }
    }
}

/// Beyond a turret's yaw stops the aim stays at its stop until the head is this much nearer the other one.
constexpr float STOP_SWITCH_RADIANS = 20.0f * 3.14159265f / 180.0f;
/// The head counts as looking past the turret's reach this far beyond a yaw stop (ParallaxAim then aims along it).
constexpr float REACH_MARGIN_RADIANS = 5.0f * 3.14159265f / 180.0f;

bool StepTurret(uintptr_t controllable, const float* headPosition, const float* headForwardRaw, float stepSeconds)
{
    float headForward[3];
    ParallaxAim(controllable, headPosition, headForwardRaw, headForward);
    const uintptr_t turret = controllable - OFFSET_turretControllable;
    float* const angles = reinterpret_cast<float*>(turret + OFFSET_turretAngles); // pitch, rate, yaw, rate
    const float* parent = TurretParentMatrix(turret);
    if (!parent || IsBadWritePtr(angles, 16) || IsBadReadPtr(reinterpret_cast<void*>(turret + OFFSET_turretSection), 4))
    {
        return false;
    }
    const uintptr_t section = *reinterpret_cast<const uintptr_t*>(turret + OFFSET_turretSection);
    if (section < 0x10000 || IsBadReadPtr(reinterpret_cast<void*>(section + OFFSET_sectionLimits), 16))
    {
        return false;
    }
    const float* limits = reinterpret_cast<const float*>(section + OFFSET_sectionLimits); // yaw, pitch pairs
    // The head's forward in the parent's frame: the parent's rows are its axes in the world.
    float local[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        const float* row = parent + axis * 4;
        local[axis] = headForward[0] * row[0] + headForward[1] * row[1] + headForward[2] * row[2];
    }
    const float length = sqrtf(local[0] * local[0] + local[1] * local[1] + local[2] * local[2]);
    if (length < 0.5f)
    {
        return false;
    }
    const float sine = fmaxf(-1.0f, fminf(1.0f, local[1] / length));
    // Pitch, yaw (the game's order in memory); a building gun along its model's -z has yaw 0 facing the parent's -z.
    const float gunForward = GunnerSeat::GunForwardZ();
    float target[2] = {-asinf(sine) + GunnerSeat::AimDownRadians(), atan2f(gunForward * local[0], gunForward * local[2])};
    const float game[2] = {angles[0], angles[2]};
    if (!_turret.hasState)
    {
        _turret.angles[0] = game[0];
        _turret.angles[1] = game[1];
        _turret.gameStep[0] = _turret.gameStep[1] = 0;
    }
    const float follow = 1.0f - expf(-stepSeconds / TIME_CONSTANT_SECONDS);
    float speeds[2];
    for (int axis = 0; axis < 2; ++axis)
    {
        const float low = limits[(1 - axis) * 2], high = limits[(1 - axis) * 2 + 1];
        const bool isLimited = low > -LIMIT_RANGE_RADIANS && high < LIMIT_RANGE_RADIANS && low < high;
        if (axis == 1)
        {
            _turret.isBeyondReach = isLimited && (target[axis] < low - REACH_MARGIN_RADIANS ||
                                                  target[axis] > high + REACH_MARGIN_RADIANS);
        }
        if (isLimited && (target[axis] < low || target[axis] > high))
        {
            // Beyond a stop: held at the stop the turret is at, the other one taken only when the head is clearly
            // nearer it (idren2023 2026-10-19: looking behind past the speeder turret's +-80 deg, the nearer stop flipped
            // as the head's angle wobbled across 180 deg and the turret and the gunner's chair swung between them).
            const float toLow = fabsf(Wrap(target[axis] - low)), toHigh = fabsf(Wrap(target[axis] - high));
            const bool isAtHigh = _turret.angles[axis] > (low + high) * 0.5f;
            const bool takesHigh = isAtHigh ? toHigh < toLow + STOP_SWITCH_RADIANS : toHigh + STOP_SWITCH_RADIANS < toLow;
            target[axis] = takesHigh ? high : low;
        }
        _turret.target[axis] = target[axis];
        if (_turret.hasState)
        {
            const float step =
                isLimited ? game[axis] - _turret.written[axis] : Wrap(game[axis] - _turret.written[axis]);
            _turret.gameStep[axis] = fabsf(step) < LARGEST_GAME_STEP_RADIANS ? step : 0.0f;
        }
        const float before = _turret.angles[axis];
        const float error = isLimited ? target[axis] - before : Wrap(target[axis] - before);
        float next = before + error * follow;
        next = isLimited ? fmaxf(low, fminf(high, next)) : Wrap(next);
        _turret.angles[axis] = next;
        speeds[axis] = (isLimited ? next - before : Wrap(next - before)) / stepSeconds;
        const float written = isLimited ? next - _turret.gameStep[axis] : Wrap(next - _turret.gameStep[axis]);
        _turret.written[axis] = written;
        angles[axis * 2] = written;
        angles[axis * 2 + 1] = speeds[axis];
    }
    _turret.hasState = true;
    // rate = -input * Rate: an input of the speed's opposite sign leaves the game's rate filter pulling the same way.
    for (int axis = 0; axis < 2; ++axis)
    {
        if (_turret.inputSign[axis] == 0 || fabsf(speeds[axis]) > INPUT_SIGN_SPEED_RADIANS)
        {
            _turret.inputSign[axis] = speeds[axis] > 0 ? -1.0f : 1.0f;
        }
    }
    _pitch.input = _turret.inputSign[0];
    _yaw.input = _turret.inputSign[1];
    _pitch.errorRadians = target[0] - _turret.angles[0];
    _yaw.errorRadians = target[1] - _turret.angles[1];
    return true;
}

/// A turret aimed by the right hand (GunnerSeat::IsAimedByHand, idren2023 2026-10-21: the Hoth dish and the Geonosian
/// turret, so that the head can stay on their stock screens): the controller's pitch over the turret's level is the
/// gun's; its yaw away from the gun's turns the gun like a joystick (the two turn all the way round, further than a
/// wrist): nothing within HAND_YAW_DEAD_RADIANS, then faster up to HAND_YAW_FULL_RADIANS, at most
/// HAND_YAW_RADIANS_PER_SECOND. The view turns with the gun (Stereo HoldHullHeading, GetTurretForward), so the hand's
/// yaw is the same against the gun as against the view.
constexpr float HAND_YAW_DEAD_RADIANS = 6.0f * 3.14159265f / 180.0f;
constexpr float HAND_YAW_FULL_RADIANS = 40.0f * 3.14159265f / 180.0f;
constexpr float HAND_YAW_RADIANS_PER_SECOND = 90.0f * 3.14159265f / 180.0f;
/// The steered yaw stays at most this far ahead of the gun's (at a yaw stop, or with the game turning the gun slower
/// than we steer, it would run away from it).
constexpr float HAND_YAW_MOST_LEAD_RADIANS = 0.2f;

bool StepTurretByHand(uintptr_t controllable, const float* eyePosition, float stepSeconds)
{
    float hand[16];
    const uintptr_t turret = controllable - OFFSET_turretControllable;
    const float* parent = TurretParentMatrix(turret);
    if (!parent || !Aim::GetHandWorld(0, hand) || IsBadReadPtr(reinterpret_cast<void*>(turret + OFFSET_turretAngles), 16))
    {
        return false;
    }
    const float* angles = reinterpret_cast<const float*>(turret + OFFSET_turretAngles); // pitch, rate, yaw, rate
    float local[3]; // the hand's pointing (-z) in the parent's frame
    for (int axis = 0; axis < 3; ++axis)
    {
        const float* row = parent + axis * 4;
        local[axis] = -(hand[8] * row[0] + hand[9] * row[1] + hand[10] * row[2]);
    }
    const float level = sqrtf(local[0] * local[0] + local[2] * local[2]);
    if (level < 0.2f) // pointing nearly straight up or down: no heading
    {
        return false;
    }
    const float f = GunnerSeat::GunForwardZ();
    const float elevation = atan2f(local[1], level);
    if (!_turret.hasHandYaw)
    {
        _turret.handYaw = angles[2];
        _turret.hasHandYaw = true;
    }
    const float offset = Wrap(atan2f(f * local[0], f * local[2]) - angles[2]);
    const float beyond = fabsf(offset) - HAND_YAW_DEAD_RADIANS;
    if (beyond > 0)
    {
        const float share = fminf(1.0f, beyond / (HAND_YAW_FULL_RADIANS - HAND_YAW_DEAD_RADIANS));
        _turret.handYaw = Wrap(_turret.handYaw + copysignf(share * HAND_YAW_RADIANS_PER_SECOND * stepSeconds, offset));
    }
    else if (_turret.hasState)
    {
        _turret.handYaw = _turret.angles[1]; // the hand back in the middle: the gun stops where it is (runs 423-424
                                             // coasted 17 deg on toward the steered yaw)
    }
    const float lead = Wrap(_turret.handYaw - angles[2]);
    if (fabsf(lead) > HAND_YAW_MOST_LEAD_RADIANS)
    {
        _turret.handYaw = Wrap(angles[2] + copysignf(HAND_YAW_MOST_LEAD_RADIANS, lead));
    }
    // The direction to aim along, in the world: the steered yaw, the hand's pitch (StepTurret's limits apply).
    const float yawSine = sinf(_turret.handYaw), yawCosine = cosf(_turret.handYaw);
    const float aimLocal[3] = {f * yawSine * cosf(elevation), sinf(elevation), f * yawCosine * cosf(elevation)};
    float forward[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        forward[axis] = aimLocal[0] * parent[axis] + aimLocal[1] * parent[4 + axis] + aimLocal[2] * parent[8 + axis];
    }
    return StepTurret(controllable, eyePosition, forward, stepSeconds);
}

// The game's other name hash (FNV-1a over the bytes | 0x20, offsets 0x3008F0; model names use it).
unsigned HashLowerFnv(const char* name)
{
    unsigned hash = 0x811C9DC5u;
    for (const char* character = name; *character; ++character)
    {
        hash = (hash ^ (static_cast<unsigned char>(*character) | 0x20u)) * 0x01000193u;
    }
    return hash;
}

// The class object of a vehicle entity by its pilot seat's class, or 0 if there is none or it is not readable.
uintptr_t VehicleClass(uintptr_t entity, Seat seat)
{
    const uintptr_t classOffset = seat == Seat::Hover    ? OFFSET_hoverClass
                                  : seat == Seat::Flyer  ? OFFSET_flyerClass
                                  : seat == Seat::Walker ? OFFSET_walkerClass
                                                         : 0;
    if (!classOffset || IsBadReadPtr(reinterpret_cast<void*>(entity + classOffset), 4))
    {
        return 0;
    }
    const uintptr_t vehicleClass = *reinterpret_cast<const uintptr_t*>(entity + classOffset);
    if (vehicleClass < 0x10000 || IsBadReadPtr(reinterpret_cast<void*>(vehicleClass), CLASS_SEARCH_BYTES))
    {
        return 0;
    }
    return vehicleClass;
}

// Where a class object holds the hash of this ODF name (either of the game's name hashes), as a byte offset; -1 if
// nowhere. isFnv tells which hash it was.
int FindNameHash(uintptr_t vehicleClass, const char* odfName, bool* isFnv)
{
    const auto hashName = reinterpret_cast<HashNameFunction>(GameBuild::Absolute(GameBuild::Address::HashName));
    const unsigned hashes[2] = {hashName(odfName), HashLowerFnv(odfName)};
    const unsigned* words = reinterpret_cast<const unsigned*>(vehicleClass);
    for (uintptr_t word = 0; word < CLASS_SEARCH_BYTES / 4; ++word)
    {
        if (words[word] == hashes[0] || words[word] == hashes[1])
        {
            *isFnv = words[word] == hashes[1];
            return static_cast<int>(word * 4);
        }
    }
    return -1;
}

const char* DriveName(Drive drive)
{
    return drive == Drive::Levers       ? "levers"
           : drive == Drive::Handlebars  ? "handlebars"
           : drive == Drive::FlightStick ? "flight stick"
           : drive == Drive::Throttle    ? "throttle"
                                         : "sticks";
}

// Whether an entity is of this ODF name: its class object (the seat's class offset) holds the name's hash, or one of
// the class objects among the entity's pointers does (walkers: run570).
bool IsNamedEntity(uintptr_t entity, uintptr_t vehicleClass, const char* odfName)
{
    bool isFnv = false;
    if (vehicleClass && FindNameHash(vehicleClass, odfName, &isFnv) >= 0)
    {
        return true;
    }
    for (uintptr_t offset = 0; offset < BUILDING_CLASS_SEARCH_BYTES; offset += 4)
    {
        if (IsBadReadPtr(reinterpret_cast<void*>(entity + offset), 4))
        {
            break;
        }
        const uintptr_t candidate = *reinterpret_cast<const uintptr_t*>(entity + offset);
        if (candidate >= 0x10000 && candidate != vehicleClass &&
            !IsBadReadPtr(reinterpret_cast<void*>(candidate), CLASS_SEARCH_BYTES) &&
            FindNameHash(candidate, odfName, &isFnv) >= 0)
        {
            return true;
        }
    }
    return false;
}

// At a seat change (game thread): the stock vehicle of a pilot seat, or of the vehicle a gunner seat is mounted on, by
// looking for the hash of each table name (both of the game's name hashes) in the vehicle's class object. Logs where
// it was found, or that it was not. A gunner seat drives nothing: sticks.
Drive FindStockVehicle(uintptr_t controllable, Seat seat)
{
    _stockName = nullptr;
    if (!GameBuild::IsAvailable(GameBuild::Feature::Muzzle))
    {
        return Drive::Sticks;
    }
    uintptr_t entity = controllable - OFFSET_vehicleControllable;
    Seat hullSeat = seat;
    if (seat == Seat::Turret || seat == Seat::Passenger)
    {
        const uintptr_t turret = controllable - OFFSET_turretControllable; // a PassengerSlot's parent as a turret's
        entity = IsBadReadPtr(reinterpret_cast<void*>(turret + OFFSET_turretParent), 4)
                     ? 0
                     : *reinterpret_cast<const uintptr_t*>(turret + OFFSET_turretParent);
        if (!entity || IsBadReadPtr(reinterpret_cast<void*>(entity + OFFSET_vehicleControllable), 4))
        {
            return Drive::Sticks;
        }
        hullSeat = SeatOf(entity + OFFSET_vehicleControllable); // a building's turret: not a vehicle
    }
    else if (seat != Seat::Hover && seat != Seat::Flyer && seat != Seat::Walker)
    {
        return Drive::Sticks;
    }
    const uintptr_t vehicleClass = VehicleClass(entity, hullSeat);
    if (!vehicleClass && seat == Seat::Turret)
    {
        // A building's turret (PLAN 58 u): its class found by searching the building for a pointer to a class object
        // that holds a stock turret's name hash.
        for (uintptr_t offset = 0; offset < BUILDING_CLASS_SEARCH_BYTES; offset += 4)
        {
            if (IsBadReadPtr(reinterpret_cast<void*>(entity + offset), 4))
            {
                break;
            }
            const uintptr_t candidate = *reinterpret_cast<const uintptr_t*>(entity + offset);
            if (candidate < 0x10000 || IsBadReadPtr(reinterpret_cast<void*>(candidate), CLASS_SEARCH_BYTES))
            {
                continue;
            }
            for (const char* name : STOCK_TURRETS)
            {
                bool isFnv = false;
                const int hashOffset = FindNameHash(candidate, name, &isFnv);
                if (hashOffset >= 0)
                {
                    Log::Info(
                        "vehicle",
                        "drive: stock turret %s (building +0x%x -> class, name hash %s at class +0x%x)",
                        name,
                        static_cast<unsigned>(offset),
                        isFnv ? "FNV" : "HashName",
                        static_cast<unsigned>(hashOffset));
                    _stockName = name;
                    return Drive::Sticks;
                }
            }
        }
        Log::Info("vehicle", "drive: a building's turret, not one of the stock turrets");
        return Drive::Sticks;
    }
    if (!vehicleClass)
    {
        Log::Info("vehicle", "drive: no vehicle class object, sticks");
        return Drive::Sticks;
    }
    for (const StockVehicle& stock : STOCK_VEHICLES)
    {
        bool isFnv = false;
        const int offset = FindNameHash(vehicleClass, stock.odfName, &isFnv);
        if (offset >= 0)
        {
            Log::Info(
                "vehicle",
                "drive: stock %s (name hash %s at class +0x%x), %s",
                stock.odfName,
                isFnv ? "FNV" : "HashName",
                static_cast<unsigned>(offset),
                DriveName(stock.drive));
            _stockName = stock.odfName;
            return seat == Seat::Turret || seat == Seat::Passenger ? Drive::Sticks : stock.drive;
        }
    }
    // Not at the class offset (run570: the AT-ST's hash was not found there): the class object searched for among
    // the entity's pointers, as for a building's turret; logs where it was found.
    for (uintptr_t offset = 0; offset < BUILDING_CLASS_SEARCH_BYTES; offset += 4)
    {
        if (IsBadReadPtr(reinterpret_cast<void*>(entity + offset), 4))
        {
            break;
        }
        const uintptr_t candidate = *reinterpret_cast<const uintptr_t*>(entity + offset);
        if (candidate < 0x10000 || candidate == vehicleClass ||
            IsBadReadPtr(reinterpret_cast<void*>(candidate), CLASS_SEARCH_BYTES))
        {
            continue;
        }
        for (const StockVehicle& stock : STOCK_VEHICLES)
        {
            bool isFnv = false;
            const int hashOffset = FindNameHash(candidate, stock.odfName, &isFnv);
            if (hashOffset >= 0)
            {
                Log::Info(
                    "vehicle",
                    "drive: stock %s (entity +0x%x -> class, name hash %s at class +0x%x; the %s class offset is +0x%x), %s",
                    stock.odfName,
                    static_cast<unsigned>(offset),
                    isFnv ? "FNV" : "HashName",
                    static_cast<unsigned>(hashOffset),
                    SeatName(hullSeat),
                    static_cast<unsigned>(hullSeat == Seat::Walker ? OFFSET_walkerClass : 0),
                    DriveName(stock.drive));
                _stockName = stock.odfName;
                return seat == Seat::Turret || seat == Seat::Passenger ? Drive::Sticks : stock.drive;
            }
        }
    }
    Log::Info("vehicle", "drive: not a stock vehicle in the table, sticks");
    return Drive::Sticks;
}

// Called from SetYawHook (PlayerController's call, game thread), before SetPitch: works out the seat and, in a seat
// with a gun, both inputs of this update.
void Update(uintptr_t controllable)
{
    if (controllable != _seatControllable)
    {
        _seatControllable = controllable;
        _seat = SeatOf(controllable);
        _isRemoteDroid = _seat == Seat::Other && Holster::IsObjectType(controllable, "EntityDroid");
        if (_isRemoteDroid)
        {
            Log::Info("vehicle", "recon droid: head aims it, left stick moves, triggers fire");
        }
        _drive = FindStockVehicle(controllable, _seat);
        if (_seat == Seat::Flyer && _drive == Drive::Sticks)
        {
            _drive = Drive::FlightStick; // idren2023 2026-10-27: fighter jet controls in every flyer
        }
        if (_seat == Seat::Walker && _drive == Drive::Sticks)
        {
            _drive = Drive::Throttle; // the vehicle proposal 2026-11-04: a throttle lever and head steering in every walker
        }
        _yaw = Axis{INITIAL_YAW_GAIN, 0, 0, 0, 0, 0, false};
        _pitch = Axis{INITIAL_PITCH_GAIN, 0, 0, 0, 0, 0, false};
        _turret = {};
    }
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    FpGuard guard;
    const float stepSeconds = static_cast<float>(
        static_cast<double>(now.QuadPart - _lastUpdate.QuadPart) / static_cast<double>(frequency.QuadPart));
    _lastUpdate = now;
    const Seat seat = _seat;
    // (a mount, a walker ridden with reins, has no gun: its look yaw would turn the animal toward the head)
    const bool hasGun = seat == Seat::Turret || (seat == Seat::Walker && _drive != Drive::Handlebars) || seat == Seat::Hover ||
                        _isRemoteDroid;
    float head[16];
    _isAiming = _isHeadAimOn && hasGun && Xr::IsRunning() && Stereo::GetLastHeadWorld(head) && stepSeconds > 0 &&
                stepSeconds < LONGEST_STEP_SECONDS;
    if (!_isAiming)
    {
        _yaw.hasLast = _pitch.hasLast = false;
        _turret = {};
        return;
    }
    const float* aim = reinterpret_cast<const float*>(controllable + OFFSET_controllableAim);
    const float headForward[3] = {-head[8], -head[9], -head[10]};
    // In a network game the turret's angles are the host's: writing them here would only fight its corrections. The
    // rate loop below steers them there (by the head, also where the hand aims in single player), and its rates go
    // into the control state (GetNetSeatInputs).
    const bool isDirect = !Net::IsNetGame();
    if (isDirect && seat == Seat::Turret && GunnerSeat::IsAimedByHand())
    {
        if (!StepTurretByHand(controllable, head + 12, stepSeconds))
        {
            _turret.hasState = false; // the hand not tracked: the gun stays where the game has it
        }
        return;
    }
    if (isDirect && seat == Seat::Turret && StepTurret(controllable, head + 12, headForward, stepSeconds))
    {
        return;
    }
    _turret = {};
    float aimHeading, aimElevation, headHeading, headElevation;
    Angles(aim, &aimHeading, &aimElevation);
    Angles(headForward, &headHeading, &headElevation);
    // Headings relative to the hull: a gunner seat on a vehicle that turns (run220: an AI drove the AAT while the
    // player sat at its gun) would otherwise count the hull's turning as the gun's own rate.
    float hullX = 0, hullZ = 0;
    if (GetHullForward(&hullX, &hullZ))
    {
        const float hullHeading = atan2f(hullX, -hullZ);
        aimHeading = Wrap(aimHeading - hullHeading);
        headHeading = Wrap(headHeading - hullHeading);
    }
    // A turret in a network game: its angles answer the input late (the input is sent with the next turn and a
    // seat's input is limited to 1) and with inertia (the turret's rate follows the input over about 0.2 s), so
    // the plain loop swings round the target (runs 688-690, measured in run 693). There the loop is slower and
    // damped by the aim's own turning speed: a critically damped pair for that inertia.
    const bool isNetTurret = seat == Seat::Turret && Net::IsNetGame();
    const float timeConstant = isNetTurret ? NET_TURRET_TIME_CONSTANT_SECONDS : TIME_CONSTANT_SECONDS;
    const float damping = isNetTurret ? NET_TURRET_DAMPING : 0.0f;
    StepAxis(_yaw, aimHeading, headHeading, stepSeconds, timeConstant, damping);
    StepAxis(_pitch, aimElevation, headElevation, stepSeconds, timeConstant, damping);
    if (Net::IsNetGame() && static_cast<LONG>(_traceEndTick - GetTickCount()) > 0)
    {
        // Test (`vehicle trace` in a network game): the loop per game turn. For a turret also its own pitch and
        // filtered pitch rate (T = Controllable - 8).
        const bool isTurret = seat == Seat::Turret;
        const float* turret = reinterpret_cast<const float*>(controllable - 8 + OFFSET_turretAngles);
        Log::Info(
            "vehicle", "NS step %.4f yaw aim %.2f head %.2f in %.3f got %.3f gain %.2f | pitch aim %.2f head %.2f in %.3f "
            "got %.3f gain %.2f | turret %.2f %.3f %.2f %.3f",
            stepSeconds, aimHeading * 57.29578f, headHeading * 57.29578f, _yaw.input, _yaw.gameInput, _yaw.gain,
            aimElevation * 57.29578f, headElevation * 57.29578f, _pitch.input, _pitch.gameInput, _pitch.gain,
            isTurret ? turret[0] * 57.29578f : 0.0f, isTurret ? turret[1] : 0.0f,
            isTurret ? turret[2] * 57.29578f : 0.0f, isTurret ? turret[3] : 0.0f);
    }
    if (seat == Seat::Hover && _yaw.gain != 0)
    {
        // A pilot seat whose hull turns toward the head (tanks, speeders, the hailfire; walkers turn by WalkerDrive): turned gently and
        // never faster than HULL_STEER_RADIANS_PER_SECOND (headset run 19: the combat speeder and the hailfire
        // snapped round to the head's heading, and the turning hull swung the lever out from under the hand).
        // and only while a hand holds the drive stick or the throttle lever (headset run 19: looking round the combat
        // speeder's cabin turned the speeder): let go, the head looks round freely
        const bool isDriving = Levers::IsHolding(0) || WalkerDrive::IsHolding(0) || WalkerDrive::IsHolding(1);
        float rate = isDriving ? _yaw.errorRadians / HULL_STEER_SECONDS : 0.0f;
        rate = rate > HULL_STEER_RADIANS_PER_SECOND ? HULL_STEER_RADIANS_PER_SECOND
               : rate < -HULL_STEER_RADIANS_PER_SECOND ? -HULL_STEER_RADIANS_PER_SECOND
                                                       : rate;
        _yaw.input = rate / _yaw.gain;
        _yaw.lastInput = _yaw.input;
    }
    if (seat == Seat::Hover && !Levers::IsShown() && !WalkerDrive::IsShown())
    {
        _yaw.input = 0; // the hover's look yaw turns the hull: the sticks keep it (idren2023: sticks first)
        _yaw.hasLast = false;
    } // a tank's pilot with the drive stick: the hull turns toward the head's heading (PLAN 59 a; the view is held)
}

void __fastcall SetYawHook(void* self, void* edx, float value)
{
    // In a network game PlayerController also runs in the host world, there for every player: only the client
    // world's calls are the local player's. And what is set here would not reach the host: the look rates go into
    // the control state of the next turn instead (Controls, GetNetSeatInputs) and come back through `value`.
    const bool isNetGame = Net::IsNetGame();
    if (reinterpret_cast<uintptr_t>(__builtin_return_address(0)) == _setYawReturn && Net::IsLocalPlayerStep())
    {
        _yaw.gameInput = value;
        Update(reinterpret_cast<uintptr_t>(self));
        const bool isYawOurs = _isAiming && (_seat != Seat::Hover || Levers::IsShown() || WalkerDrive::IsShown());
        if (isNetGame)
        {
            _netSeat.hasYaw = isYawOurs;
            _netSeat.yaw = _yaw.input;
        }
        else if (isYawOurs)
        {
            value = _yaw.input;
        }
    }
    _originalSetYaw(self, edx, value);
}

// Test `vehicle enter` (game thread, after PlayerController has set the inputs; the rest of its update reads the
// Controllable afresh): the soldier enters the nearest vehicle of the kind wanted that lets it in, as the enter key
// does next to one. Candidates are logged by class and distance.
void EnterNearest(uintptr_t controllable, Seat wanted)
{
    if (SeatOf(controllable) != Seat::OnFoot)
    {
        Log::Info("vehicle", "enter: the player is not on foot");
        return;
    }
    FpGuard fp; // our float maths (distances) must not disturb the game's floating-point state
    const auto query = reinterpret_cast<EntityQueryFunction>(GameBuild::Absolute(GameBuild::Address::EntityQuery));
    uintptr_t found[ENTER_SEARCH_MOST] = {};
    const float* eye = reinterpret_cast<const float*>(controllable + OFFSET_controllableEye);
    const float searchMeters = _enterName[0] ? ENTER_NAMED_SEARCH_METERS : ENTER_SEARCH_METERS;
    const int count = query(eye, searchMeters, found, ENTER_SEARCH_MOST, controllable - OFFSET_vehicleControllable, 0);
    void** const vtable = *reinterpret_cast<void***>(controllable);
    const auto canEnter = reinterpret_cast<EntityCallFunction>(vtable[SLOT_CAN_ENTER]);
    const auto enter = reinterpret_cast<EntityCallFunction>(vtable[SLOT_ENTER]);
    float distances[ENTER_SEARCH_MOST];
    bool isWanted[ENTER_SEARCH_MOST] = {};
    for (int index = 0; index < count && index < ENTER_SEARCH_MOST; ++index)
    {
        const uintptr_t entity = found[index];
        const char* type = Holster::ObjectTypeName(entity);
        // The entity's pilot seat, where it has one, tells its kind; anything else (a building with turrets) counts
        // as a turret.
        const Seat pilot = SeatOf(entity + OFFSET_vehicleControllable);
        const Seat kind = pilot == Seat::Hover || pilot == Seat::Flyer || pilot == Seat::Walker ? pilot : Seat::Turret;
        const bool isAllowed =
            canEnter(reinterpret_cast<void*>(controllable), nullptr, reinterpret_cast<void*>(entity));
        const float* position = reinterpret_cast<const float*>(entity + OFFSET_entityMatrix + 48);
        const float dx = position[0] - eye[0], dy = position[1] - eye[1], dz = position[2] - eye[2];
        distances[index] = sqrtf(dx * dx + dy * dy + dz * dz);
        const int turrets = kind == Seat::Hover   ? *reinterpret_cast<const uint8_t*>(entity + OFFSET_hoverTurretCount)
                            : kind == Seat::Flyer ? *reinterpret_cast<const uint8_t*>(entity + OFFSET_flyerTurretCount)
                                                  : -1;
        bool isFnv = false;
        uintptr_t vehicleClass =
            _enterName[0] && GameBuild::IsAvailable(GameBuild::Feature::Muzzle) ? VehicleClass(entity, pilot) : 0;
        if (_enterName[0] && !vehicleClass && kind == Seat::Turret &&
            !IsBadReadPtr(reinterpret_cast<void*>(entity + 4), 4))
        {
            // A building: its class at +0x4 as for the grey gun turret (run399; FindStockVehicle searches further).
            const uintptr_t buildingClass = *reinterpret_cast<const uintptr_t*>(entity + 4);
            vehicleClass =
                buildingClass >= 0x10000 && !IsBadReadPtr(reinterpret_cast<void*>(buildingClass), CLASS_SEARCH_BYTES)
                    ? buildingClass
                    : 0;
        }
        const bool isNamed = !_enterName[0] || (vehicleClass && FindNameHash(vehicleClass, _enterName, &isFnv) >= 0) ||
                             (kind == Seat::Walker && IsNamedEntity(entity, vehicleClass, _enterName));
        isWanted[index] =
            isAllowed && isNamed && (wanted == Seat::Other || wanted == kind) && (!_isGunnerSeatWanted || turrets > 0);
        if (!isNamed)
        {
            continue; // a search by name logs only that vehicle's candidates
        }
        Log::Info(
            "vehicle",
            "enter: candidate %s (%s, %d gunner seats%s) at %.0f m, %s",
            type ? type : "(unknown)",
            SeatName(kind),
            turrets,
            _enterName[0] ? ", named" : "",
            static_cast<double>(distances[index]),
            isAllowed ? "may enter" : "refused");
    }
    for (;;) // nearest first; the next one if the game refuses
    {
        int nearest = -1;
        for (int index = 0; index < count && index < ENTER_SEARCH_MOST; ++index)
        {
            if (isWanted[index] && (nearest < 0 || distances[index] < distances[nearest]))
            {
                nearest = index;
            }
        }
        if (nearest < 0)
        {
            break;
        }
        isWanted[nearest] = false;
        const bool isIn =
            enter(reinterpret_cast<void*>(controllable), nullptr, reinterpret_cast<void*>(found[nearest]));
        Log::Info(
            "vehicle",
            "enter: the one at %.0f m: %s",
            static_cast<double>(distances[nearest]),
            isIn ? "entered" : "the game refused");
        if (isIn)
        {
            return;
        }
    }
    Log::Info(
        "vehicle",
        "enter: nothing entered (%d entities within %.0f m)",
        count,
        static_cast<double>(searchMeters));
}

// The tank levers (Levers) and a bike's rider (Handlebars: lean, twist grip) add to the seat's move, strafe and turn
// inputs, as PlayerController has just set them from the sticks and keys (its SetPitch call is the last of the four
// setters; the rest of its update and the vehicle's own update read the Controllable afresh). Game thread.
void AddLeverDrive(uintptr_t controllable, bool isNetGame)
{
    float move = 0, turn = 0, strafe = 0, riderMove = 0, riderTurn = 0, flightMove = 0, flightYaw = 0, pitch = 0,
          flightStrafe = 0;
    Levers::GetDrive(&move, &turn, &strafe);
    Handlebars::GetDrive(&riderMove, &riderTurn);
    Flight::GetDrive(&flightMove, &flightYaw, &pitch, &flightStrafe);
    // A hover craft's elevation (Flight::GetClimb): the vertical part of the flyer's velocity (entity +0x4DC, run576)
    // (run577: writing the velocity's y did nothing; run578: +0x5A4 rose 0 -> 16.4 at take-off and fell while landing:
    // the hover height? moved here by the climb rate over the step)
    const float climb = _seat == Seat::Flyer ? Flight::GetClimb() : 0.0f;
    {
        static DWORD lastTick = 0;
        const DWORD now = GetTickCount();
        const float seconds = lastTick && now - lastTick < 250 ? static_cast<float>(now - lastTick) * 0.001f : 0.0f;
        lastTick = now;
        // (run579: +0x5A4 moved did nothing either: the height is the matrix's alone; moved here by the climb)
        // (run580: the game keeps the height it is moved to, climbing 5 m/s held exactly; under the map's player fly
        // ceiling, GameBuild MaxPlayerFlyHeight)
        float* height = reinterpret_cast<float*>(controllable - OFFSET_vehicleControllable + OFFSET_entityMatrix + 52);
        // (not in a network game: the host owns the position, the write would only fight its corrections)
        if (climb != 0.0f && seconds > 0.0f && !isNetGame && !IsBadWritePtr(height, 4))
        {
            FpGuard fp;
            float next = *height + climb * seconds;
            if (climb > 0.0f && GameBuild::IsAvailable(GameBuild::Feature::Muzzle))
            {
                const float ceiling =
                    *reinterpret_cast<const float*>(GameBuild::Absolute(GameBuild::Address::MaxPlayerFlyHeight));
                if (next > ceiling && ceiling > -1e4f && ceiling < 1e4f)
                {
                    next = *height > ceiling ? *height : ceiling;
                }
            }
            *height = next;
        }
    }
    float walkerMove = 0, walkerTurn = 0;
    // vehicle_controls sticks: the right stick turns the view (Controls' smooth / snap turn, as on foot) and head
    // steering turns the walker after it (run612: also feeding the stick to the turn input fought the aim loop)
    WalkerDrive::GetDrive(&walkerMove, &walkerTurn);
    if (_seat == Seat::Walker)
    {
        walkerTurn += riderTurn; // a mount's rider (Handlebars): a walker turns on its strafe input, right positive
        riderTurn = 0;
    }
    move += riderMove + flightMove + walkerMove;
    strafe += walkerTurn + flightStrafe; // a walker turns on its strafe input (A / D, run570); a gunship strafes
    turn += riderTurn + flightYaw;
    if (isNetGame)
    {
        // Writes into the Controllable never reach the host: Controls adds these to the control state of the next
        // turn instead (GetNetSeatInputs), and the game sets the Controllable from that in both worlds.
        _netSeat.move = move;
        _netSeat.strafe = strafe;
        _netSeat.turn = turn;
        _netSeat.pitch = pitch;
        return;
    }
    if (move == 0.0f && turn == 0.0f && strafe == 0.0f && pitch == 0.0f)
    {
        return;
    }
    FpGuard fp;
    float* inputs = reinterpret_cast<float*>(controllable + OFFSET_controllableMove);
    const auto add = [](float value, float added)
    {
        const float sum = value + added;
        return sum > 1.0f ? 1.0f : sum < -1.0f ? -1.0f : sum;
    };
    inputs[0] = add(inputs[0], move);   // +0x54 move
    inputs[1] = add(inputs[1], strafe); // +0x58 strafe (D = +1)
    inputs[2] = add(inputs[2], turn);   // +0x5C look yaw: turns a hover's hull (a flyer's nose)
    // +0x60 look pitch: a flyer's nose, up positive (run456); not clamped to 1, the flight stick may ask for more
    inputs[3] += pitch;
}

void __fastcall SetPitchHook(void* self, void* edx, float value)
{
    const bool isNetGame = Net::IsNetGame(); // see SetYawHook
    const bool isPlayerCall =
        reinterpret_cast<uintptr_t>(__builtin_return_address(0)) == _setPitchReturn && Net::IsLocalPlayerStep();
    const bool isSeat = isPlayerCall && reinterpret_cast<uintptr_t>(self) == _seatControllable;
    if (isPlayerCall)
    {
        _pitch.gameInput = value;
        if (isNetGame)
        {
            _netSeat.hasPitch = _isAiming && isSeat;
            _netSeat.lookPitch = _pitch.input;
        }
        else if (_isAiming && isSeat)
        {
            value = _pitch.input;
        }
    }
    _originalSetPitch(self, edx, value);
    if (isSeat)
    {
        AddLeverDrive(reinterpret_cast<uintptr_t>(self), isNetGame);
        if (isNetGame)
        {
            _netSeat.isInSeat = _seat != Seat::OnFoot;
            _netSeatTick = GetTickCount();
        }
    }
    // Test `vehicle enter`: in a network game the entry has to happen in the host world (the host owns who sits
    // where); possible only when this machine hosts, and meant for a listen server with no other player (any
    // player's controller would take the request).
    const bool isEnterCall = reinterpret_cast<uintptr_t>(__builtin_return_address(0)) == _setPitchReturn &&
                             (!isNetGame || (Net::IsHost() && !Net::IsClientWorldActive()));
    const LONG request = isEnterCall ? InterlockedExchange(&_enterRequest, -1) : -1;
    if (request >= 0)
    {
        EnterNearest(reinterpret_cast<uintptr_t>(self), static_cast<Seat>(request));
    }
}

// The turret's first-person hiding of its children (TurretHideChildren, offsets "MountedTurret pose"): skipped for a
// pose table without a skeleton, where the game would read through a null pointer (run273: the Imperial fighter
// tank's gunner seat on Rhen Var crashed at the seat change; the stock game's flat view rarely takes this first-person
// path). The turret node itself is already hidden by then; only its children (gun, barrel) stay drawn. Game thread
// (the hull's render), cdecl.
void __cdecl HideChildrenHook(void* model, uintptr_t table, void* node)
{
    if (!table || IsBadReadPtr(reinterpret_cast<void*>(table + OFFSET_poseTableSkeleton), 4) ||
        !*reinterpret_cast<const uintptr_t*>(table + OFFSET_poseTableSkeleton))
    {
        static bool isLogged = false;
        if (!isLogged)
        {
            isLogged = true;
            Log::Warning("vehicle", "turret: the hull's pose table has no skeleton; the turret's parts are not hidden");
        }
        return;
    }
    _originalHideChildren(model, table, node);
}

// After the game's own aim (game thread, the droideka's update): for the player's droideka with head aim on, the arms'
// target point is moved onto the head's line of sight, as far from the ray's start as the game's own hit along the
// body's aim. The arms then turn toward it at once, as far as their limits allow (+-40 deg), while the body catches up
// at its own pace; the aim direction (+0x33C: the target search's ray, the camera) stays the body's. The game's result
// is passed back: b279-b280 returned none, and the caller, given a stray value, stopped aiming the droideka (run309:
// its aim froze while the body turned on) or crashed writing through a float-like address (run310; inferred from the
// function's `mov al, 1` before `ret 4`). Those builds also cast a second ray after the game's (its out argument's size
// and the manager's hit data not known): dropped for the game's own hit distance. PLAN 57 b.
bool __fastcall DroidekaAimHook(void* entity, void* edx, int argument)
{
    const bool result = _originalDroidekaAim(entity, edx, argument);
    const uintptr_t droideka = reinterpret_cast<uintptr_t>(entity);
    float head[16];
    // (not in a network game: the host aims the arms itself, along the droideka's own look, which Controls steers
    // toward the head by mouse counts, and those do reach it)
    if (!_isHeadAimOn || droideka + OFFSET_vehicleControllable != _seatControllable || _seat != Seat::OnFoot ||
        !Xr::IsRunning() || Net::IsNetGame() || !Stereo::GetLastHeadWorld(head))
    {
        return result;
    }
    FpGuard fp;
    const float length = sqrtf(head[8] * head[8] + head[9] * head[9] + head[10] * head[10]);
    const float* start = reinterpret_cast<const float*>(droideka + OFFSET_droidekaRayStart);
    float* target = reinterpret_cast<float*>(droideka + OFFSET_droidekaTarget);
    const float toTarget[3] = {target[0] - start[0], target[1] - start[1], target[2] - start[2]};
    const float distance = sqrtf(toTarget[0] * toTarget[0] + toTarget[1] * toTarget[1] + toTarget[2] * toTarget[2]);
    if (length < 0.5f || !(distance > 1.0f && distance <= DROIDEKA_RAY_METERS * 1.01f))
    {
        return result;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        target[axis] = start[axis] - head[8 + axis] / length * distance;
    }
    return result;
}

// The droideka's head aim (PLAN 57 b), its own feature: a game build without it keeps the game's aim.
void InstallDroidekaAimHook()
{
    if (!GameBuild::IsAvailable(GameBuild::Feature::DroidekaAim))
    {
        Log::Warning(
            "vehicle",
            "droideka aim not found (%s is off for this game build): its guns follow its body",
            GameBuild::FeatureName(GameBuild::Feature::DroidekaAim));
        return;
    }
    void* const aim = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::DroidekaAim));
    MH_Initialize();
    MH_STATUS status =
        MH_CreateHook(aim, reinterpret_cast<void*>(&DroidekaAimHook), reinterpret_cast<void**>(&_originalDroidekaAim));
    if (status == MH_OK)
    {
        status = MH_EnableHook(aim);
    }
    Log::Info("vehicle", "droideka aim hook: %s", MH_StatusToString(status));
}

void InstallHooks()
{
    _areHooksTried = true;
    InstallDroidekaAimHook();
    if (!GameBuild::IsAvailable(GameBuild::Feature::VehicleAim))
    {
        Log::Warning(
            "vehicle",
            "look setters not found (%s is off for this game build): vehicles keep the game's aim",
            GameBuild::FeatureName(GameBuild::Feature::VehicleAim));
        return;
    }
    _setYawReturn = GameBuild::Absolute(GameBuild::Address::SetYawReturn);
    _setPitchReturn = GameBuild::Absolute(GameBuild::Address::SetPitchReturn);
    void* const setYaw = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::SetYaw));
    void* const setPitch = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::SetPitch));
    MH_Initialize();
    MH_STATUS yawStatus =
        MH_CreateHook(setYaw, reinterpret_cast<void*>(&SetYawHook), reinterpret_cast<void**>(&_originalSetYaw));
    if (yawStatus == MH_OK)
    {
        yawStatus = MH_EnableHook(setYaw);
    }
    MH_STATUS pitchStatus =
        MH_CreateHook(setPitch, reinterpret_cast<void*>(&SetPitchHook), reinterpret_cast<void**>(&_originalSetPitch));
    if (pitchStatus == MH_OK)
    {
        pitchStatus = MH_EnableHook(setPitch);
    }
    void* const hideChildren = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::TurretHideChildren));
    MH_STATUS hideStatus = MH_CreateHook(
        hideChildren,
        reinterpret_cast<void*>(&HideChildrenHook),
        reinterpret_cast<void**>(&_originalHideChildren));
    if (hideStatus == MH_OK)
    {
        hideStatus = MH_EnableHook(hideChildren);
    }
    Log::Info(
        "vehicle",
        "look setter hooks: %s, %s; turret hiding guard: %s",
        MH_StatusToString(yawStatus),
        MH_StatusToString(pitchStatus),
        MH_StatusToString(hideStatus));
}

// Once per second in a vehicle: the seat's inputs, its aim, the head, the loop.
void LogSummary()
{
    const uintptr_t controllable = _seatControllable;
    if (!controllable || IsBadReadPtr(reinterpret_cast<void*>(controllable), OFFSET_controllableAim + 12))
    {
        return;
    }
    FpGuard guard;
    const float* inputs = reinterpret_cast<const float*>(controllable + OFFSET_controllableMove);
    const float* aim = reinterpret_cast<const float*>(controllable + OFFSET_controllableAim);
    float aimHeading, aimElevation;
    Angles(aim, &aimHeading, &aimElevation);
    float head[16];
    float headHeading = 0, headElevation = 0;
    if (Stereo::GetLastHeadWorld(head))
    {
        const float headForward[3] = {-head[8], -head[9], -head[10]};
        Angles(headForward, &headHeading, &headElevation);
    }
    float hullX = 0, hullZ = 0;
    char hullText[16] = "none";
    if (GetHullForward(&hullX, &hullZ))
    {
        snprintf(
            hullText,
            sizeof(hullText),
            "%.0f deg",
            static_cast<double>(atan2f(hullX, -hullZ) * DEGREES_PER_RADIAN));
    }
    Log::Info(
        "vehicle",
        "%s: move %.2f strafe %.2f yaw %.2f pitch %.2f (game %.2f %.2f); aim %.0f / %.0f deg, head %.0f / %.0f; "
        "hull %s; loop %s, gains %.2f %.2f",
        SeatName(_seat),
        static_cast<double>(inputs[0]),
        static_cast<double>(inputs[1]),
        static_cast<double>(inputs[2]),
        static_cast<double>(inputs[3]),
        static_cast<double>(_yaw.gameInput),
        static_cast<double>(_pitch.gameInput),
        static_cast<double>(aimHeading * DEGREES_PER_RADIAN),
        static_cast<double>(aimElevation * DEGREES_PER_RADIAN),
        static_cast<double>(headHeading * DEGREES_PER_RADIAN),
        static_cast<double>(headElevation * DEGREES_PER_RADIAN),
        hullText,
        _isAiming ? "on" : "off",
        static_cast<double>(_yaw.gain),
        static_cast<double>(_pitch.gain));
    float hull[16];
    if ((_seat == Seat::Flyer || _seat == Seat::Walker || _seat == Seat::Hover) && GetHullMatrix(hull))
    {
        // flight study (PLAN starfighters; walkers and hovers too, the vehicle proposal 2026-11-04): the hull's place,
        // attitude and speed between two summaries
        static float lastPosition[3] = {};
        static DWORD lastTick = 0;
        const DWORD now = GetTickCount();
        const float* position = hull + 12;
        const float dx = position[0] - lastPosition[0], dy = position[1] - lastPosition[1],
                    dz = position[2] - lastPosition[2];
        const float seconds = lastTick ? static_cast<float>(now - lastTick) * 0.001f : 0.0f;
        const float speed = seconds > 0.0f ? sqrtf(dx * dx + dy * dy + dz * dz) / seconds : 0.0f;
        Log::Info(
            "vehicle",
            "%s: at (%.1f %.1f %.1f), speed %.1f m/s (climb %.1f), nose pitch %.0f deg, roll %.0f deg",
            SeatName(_seat),
            static_cast<double>(position[0]),
            static_cast<double>(position[1]),
            static_cast<double>(position[2]),
            static_cast<double>(speed),
            static_cast<double>(seconds > 0.0f ? dy / seconds : 0.0f),
            static_cast<double>(asinf(hull[9] < -1.0f ? -1.0f : hull[9] > 1.0f ? 1.0f : hull[9]) * DEGREES_PER_RADIAN),
            static_cast<double>(atan2f(hull[1], hull[5]) * DEGREES_PER_RADIAN));
        // Velocity study (the gunship's elevation stick, the vehicle proposal): the entity's floats that hold this
        // velocity (three in a row within 15 %: the game's own velocity vector), logged while it moves faster than 4 m/s.
        if (seconds > 0.0f && _seat == Seat::Flyer && sqrtf(dx * dx + dy * dy + dz * dz) / seconds > 4.0f)
        {
            const float velocity[3] = {dx / seconds, dy / seconds, dz / seconds};
            const float size = sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
            const uintptr_t entity = controllable - OFFSET_vehicleControllable;
            char found[256] = "";
            size_t used = 0;
            for (uintptr_t offset = 0; offset + 12 <= 0x2000 && used + 16 < sizeof(found); offset += 4)
            {
                if (IsBadReadPtr(reinterpret_cast<void*>(entity + offset), 12))
                {
                    break;
                }
                const float* v = reinterpret_cast<const float*>(entity + offset);
                float error = 0;
                for (int axis = 0; axis < 3; ++axis)
                {
                    error += (v[axis] - velocity[axis]) * (v[axis] - velocity[axis]);
                }
                if (sqrtf(error) < 0.15f * size)
                {
                    used += static_cast<size_t>(
                        snprintf(found + used, sizeof(found) - used, " +0x%x", static_cast<unsigned>(offset)));
                }
            }
            Log::Info(
                "vehicle",
                "velocity study: (%.1f %.1f %.1f) m/s found at%s",
                static_cast<double>(velocity[0]),
                static_cast<double>(velocity[1]),
                static_cast<double>(velocity[2]),
                used ? found : " nothing");
        }
        memcpy(lastPosition, position, sizeof(lastPosition));
        lastTick = now;
    }
    if (_seat == Seat::Turret && _turret.hasState && _isAiming)
    {
        const float* angles =
            reinterpret_cast<const float*>(controllable - OFFSET_turretControllable + OFFSET_turretAngles);
        Log::Info(
            "vehicle",
            "turret angles (deg, pitch up negative): game %.1f / %.1f, ours %.1f / %.1f, head %.1f / %.1f within the "
            "limits; game step %.2f / %.2f",
            static_cast<double>(angles[0] * DEGREES_PER_RADIAN),
            static_cast<double>(angles[2] * DEGREES_PER_RADIAN),
            static_cast<double>(_turret.angles[0] * DEGREES_PER_RADIAN),
            static_cast<double>(_turret.angles[1] * DEGREES_PER_RADIAN),
            static_cast<double>(_turret.target[0] * DEGREES_PER_RADIAN),
            static_cast<double>(_turret.target[1] * DEGREES_PER_RADIAN),
            static_cast<double>(_turret.gameStep[0] * DEGREES_PER_RADIAN),
            static_cast<double>(_turret.gameStep[1] * DEGREES_PER_RADIAN));
    }
}

} // namespace

Seat CurrentSeat()
{
    return _seat;
}

bool IsRemoteDroid()
{
    return _isRemoteDroid;
}

bool _isSticksMode = false;

bool IsSticksMode()
{
    return _isSticksMode;
}

void SetSticksMode(bool isSticks)
{
    _isSticksMode = isSticks;
    Log::Info("vehicle", "controls: %s", isSticks ? "sticks" : "motion");
}

Drive CurrentDrive()
{
    return _drive;
}

float HullHealthFraction()
{
    const Seat seat = _seat;
    const uintptr_t hull = CurrentHullEntity();
    if (seat == Seat::Turret || seat == Seat::Passenger || !hull ||
        IsBadReadPtr(reinterpret_cast<void*>(hull + OFFSET_hullHealth), 8))
    {
        return -1;
    }
    const float health = *reinterpret_cast<const float*>(hull + OFFSET_hullHealth);
    const float most = *reinterpret_cast<const float*>(hull + OFFSET_hullHealth + 4);
    return most > 0 ? health / most : -1;
}

float WeaponHeatFraction()
{
    const uintptr_t hull = _seat == Seat::Flyer ? CurrentHullEntity() : 0;
    if (!hull || IsBadReadPtr(reinterpret_cast<void*>(hull + OFFSET_flyerWeapon), 4))
    {
        return -1;
    }
    const uintptr_t weapon = *reinterpret_cast<const uintptr_t*>(hull + OFFSET_flyerWeapon);
    if (!Holster::IsObjectType(weapon, "WeaponCannon") ||
        IsBadReadPtr(reinterpret_cast<void*>(weapon + OFFSET_weaponHeat), 4))
    {
        return -1;
    }
    return *reinterpret_cast<const float*>(weapon + OFFSET_weaponHeat);
}

const char* CurrentStockName()
{
    return _stockName;
}

uintptr_t CurrentHullEntity()
{
    const Seat seat = _seat;
    const uintptr_t controllable = _seatControllable;
    if (seat == Seat::None || seat == Seat::OnFoot || !controllable)
    {
        return 0;
    }
    if (seat != Seat::Turret && seat != Seat::Passenger)
    {
        return controllable - OFFSET_vehicleControllable;
    }
    const uintptr_t turret = controllable - OFFSET_turretControllable;
    if (IsBadReadPtr(reinterpret_cast<void*>(turret + OFFSET_turretParent), 4))
    {
        return 0;
    }
    return *reinterpret_cast<uintptr_t*>(turret + OFFSET_turretParent);
}

bool GetHullMatrix(float* hullMatrix)
{
    const uintptr_t hull = CurrentHullEntity();
    if (!hull || IsBadReadPtr(reinterpret_cast<void*>(hull + OFFSET_entityMatrix), 64))
    {
        return false;
    }
    const float* matrix = reinterpret_cast<const float*>(hull + OFFSET_entityMatrix);
    for (int row = 0; row < 3; ++row)
    {
        const float* axis = matrix + row * 4;
        if (fabsf(sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]) - 1.0f) > UNIT_TOLERANCE)
        {
            return false;
        }
    }
    memcpy(hullMatrix, matrix, 64);
    return true;
}

bool GetSeatTarget(float* point)
{
    const uintptr_t controllable = _seatControllable;
    if (_seat != Seat::Turret || !controllable ||
        IsBadReadPtr(reinterpret_cast<const void*>(controllable + OFFSET_controllableTarget), 12))
    {
        return false;
    }
    memcpy(point, reinterpret_cast<const void*>(controllable + OFFSET_controllableTarget), 12);
    return true;
}

uintptr_t CurrentSeatControllable()
{
    return _seatControllable;
}

bool GetTurretMatrix(float* matrix)
{
    const uintptr_t controllable = _seatControllable;
    if (_seat != Seat::Turret || !controllable)
    {
        return false;
    }
    const uintptr_t turret = controllable - OFFSET_turretControllable;
    if (IsBadReadPtr(reinterpret_cast<void*>(turret + OFFSET_turretMatrix), 64))
    {
        return false;
    }
    const float* source = reinterpret_cast<const float*>(turret + OFFSET_turretMatrix);
    for (int row = 0; row < 3; ++row)
    {
        const float* axis = source + row * 4;
        if (fabsf(sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]) - 1.0f) > UNIT_TOLERANCE)
        {
            return false;
        }
    }
    memcpy(matrix, source, 64);
    return true;
}

bool GetTurretForward(float* forwardX, float* forwardZ)
{
    const uintptr_t controllable = _seatControllable;
    if (_seat != Seat::Turret || !controllable)
    {
        return false;
    }
    const uintptr_t turret = controllable - OFFSET_turretControllable;
    const float* parent = TurretParentMatrix(turret);
    if (!parent || IsBadReadPtr(reinterpret_cast<void*>(turret + OFFSET_turretAngles), 16))
    {
        return false;
    }
    // As StepTurret's yaw: the gun's forward in the parent's frame is f (sin yaw, 0, cos yaw).
    const float yaw = reinterpret_cast<const float*>(turret + OFFSET_turretAngles)[2];
    const float f = GunnerSeat::GunForwardZ(), sine = f * sinf(yaw), cosine = f * cosf(yaw);
    const float x = sine * parent[0] + cosine * parent[8], z = sine * parent[2] + cosine * parent[10];
    const float level = sqrtf(x * x + z * z);
    if (level < 0.1f)
    {
        return false;
    }
    *forwardX = x / level;
    *forwardZ = z / level;
    return true;
}

bool GetTurretAngles(float* pitch, float* yaw)
{
    const uintptr_t controllable = _seatControllable;
    if (_seat != Seat::Turret || !controllable)
    {
        return false;
    }
    const uintptr_t turret = controllable - OFFSET_turretControllable;
    if (IsBadReadPtr(reinterpret_cast<void*>(turret + OFFSET_turretAngles), 16))
    {
        return false;
    }
    const float* angles = reinterpret_cast<const float*>(turret + OFFSET_turretAngles); // pitch, rate, yaw, rate
    *pitch = angles[0];
    *yaw = angles[2];
    return true;
}

bool GetTurretYaw(float* yaw, float* low, float* high)
{
    const uintptr_t controllable = _seatControllable;
    if (_seat != Seat::Turret || !controllable)
    {
        return false;
    }
    const uintptr_t turret = controllable - OFFSET_turretControllable;
    if (IsBadReadPtr(reinterpret_cast<void*>(turret + OFFSET_turretAngles), 16) ||
        IsBadReadPtr(reinterpret_cast<void*>(turret + OFFSET_turretSection), 4))
    {
        return false;
    }
    const uintptr_t section = *reinterpret_cast<const uintptr_t*>(turret + OFFSET_turretSection);
    if (section < 0x10000 || IsBadReadPtr(reinterpret_cast<void*>(section + OFFSET_sectionLimits), 8))
    {
        return false;
    }
    const float* limits = reinterpret_cast<const float*>(section + OFFSET_sectionLimits);
    if (!(limits[0] > -LIMIT_RANGE_RADIANS && limits[1] < LIMIT_RANGE_RADIANS && limits[0] < limits[1]))
    {
        return false;
    }
    *yaw = reinterpret_cast<const float*>(turret + OFFSET_turretAngles)[2];
    *low = limits[0];
    *high = limits[1];
    return true;
}

namespace
{

uintptr_t _aimers[MOST_SEAT_AIMERS] = {};
int _aimerCount = 0;
uintptr_t _aimerControllable = 0; // the seat _aimers were looked up for
DWORD _aimerTick = 0;

// Whether an address is an Aimer we may read (aligned, committed and readable up to its fire point, RTTI "Aimer").
// The memory is asked with VirtualQuery first: a turret's unused Aimer slots hold leftover floats (run304), and
// IsBadReadPtr on those raised a first-chance exception at every look-up.
// During one FindAimers the regions VirtualQuery reported are kept: the pilot scan asks about ~3000 addresses, and a
// VirtualQuery each made a ~110 ms frame at every refresh in the snowspeeder (run 913, RenderSampler).
struct QueriedRegion
{
    uintptr_t base, end;
    bool isReadable;
};
constexpr int QUERIED_REGION_COUNT = 32;
QueriedRegion _queriedRegions[QUERIED_REGION_COUNT];
int _queriedRegionCount = 0, _nextQueriedRegion = 0;
bool _isRegionCacheOn = false;

// Whether [address, address + bytes) lies in one committed, readable region.
bool IsReadableRange(uintptr_t address, size_t bytes)
{
    if (_isRegionCacheOn)
    {
        for (int index = 0; index < _queriedRegionCount; ++index)
        {
            const QueriedRegion& region = _queriedRegions[index];
            if (address >= region.base && address < region.end)
            {
                return region.isReadable && address + bytes <= region.end;
            }
        }
    }
    MEMORY_BASIC_INFORMATION memoryInfo;
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &memoryInfo, sizeof(memoryInfo)))
    {
        return false;
    }
    const uintptr_t base = reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress),
                    end = base + memoryInfo.RegionSize;
    const bool isReadable =
        memoryInfo.State == MEM_COMMIT && !(memoryInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    if (_isRegionCacheOn)
    {
        _queriedRegions[_nextQueriedRegion] = {base, end, isReadable};
        _nextQueriedRegion = (_nextQueriedRegion + 1) % QUERIED_REGION_COUNT;
        if (_queriedRegionCount < QUERIED_REGION_COUNT)
        {
            ++_queriedRegionCount;
        }
    }
    return isReadable && address + bytes <= end;
}

bool IsAimer(uintptr_t address)
{
    if (address < 0x10000 || (address & 3) || !IsReadableRange(address, AIMER_READ_BYTES))
    {
        return false;
    }
    // An Aimer's vtable is in the exe's image: anything else is no Aimer (spares the RTTI walk's IsBadReadPtr calls).
    static uintptr_t imageStart = 0, imageEnd = 0;
    if (!imageStart)
    {
        imageStart = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageStart);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(imageStart + static_cast<uintptr_t>(dos->e_lfanew));
        imageEnd = imageStart + nt->OptionalHeader.SizeOfImage;
    }
    const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(address);
    if (vtable < imageStart + 4 || vtable >= imageEnd)
    {
        return false;
    }
    return Holster::IsObjectType(address, "Aimer");
}

// The first Aimer of the seat's weapons and how many slots there are: a turret's (MountedTurret = Controllable - 8),
// a droideka's (EntityDroideka = Controllable - 0x250); 0 for other seats.
uintptr_t AimerSlots(uintptr_t controllable, int* slotCount)
{
    if (_seat == Seat::Turret)
    {
        *slotCount = TURRET_AIMER_SLOTS;
        return controllable - OFFSET_turretControllable + OFFSET_turretAimers;
    }
    if (_seat == Seat::OnFoot && Aim::IsDroideka())
    {
        *slotCount = DROIDEKA_AIMER_SLOTS;
        return controllable - OFFSET_vehicleControllable + OFFSET_droidekaAimers;
    }
    *slotCount = 0;
    return 0;
}

// A pilot seat's Aimers (tanks, speeders, walkers, fighters; headset run 19: aiming lines in every vehicle): the
// vehicle classes keep them at offsets of their own, so the entity's first PILOT_SCAN_BYTES are searched for pointers
// to Aimers owned by the vehicle itself (+0x20) and for Aimers embedded in it, each with a unit direction and a fire
// point within PILOT_AIMER_RANGE of the hull. Once a second (GetSeatFireLines' refresh).
constexpr uintptr_t PILOT_SCAN_BYTES = 0x1800;
constexpr float PILOT_AIMER_RANGE = 25.0f;

bool IsPilotAimer(uintptr_t aimer, uintptr_t entity)
{
    if (!IsAimer(aimer))
    {
        return false;
    }
    const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(aimer + OFFSET_aimerOwner);
    if (owner != entity && (owner < entity || owner >= entity + PILOT_SCAN_BYTES))
    {
        return false;
    }
    FpGuard fp;
    const float* direction = reinterpret_cast<const float*>(aimer + OFFSET_aimerDirection);
    const float* from = reinterpret_cast<const float*>(aimer + OFFSET_aimerFirePoint);
    const float* hull = reinterpret_cast<const float*>(entity + OFFSET_entityMatrix);
    const float length = sqrtf(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    const float dx = from[0] - hull[12], dy = from[1] - hull[13], dz = from[2] - hull[14];
    return fabsf(length - 1.0f) <= 0.01f && dx * dx + dy * dy + dz * dz < PILOT_AIMER_RANGE * PILOT_AIMER_RANGE;
}

int ScanPilotAimers(uintptr_t entity, uintptr_t* found, char* how)
{
    if (IsBadReadPtr(reinterpret_cast<void*>(entity), PILOT_SCAN_BYTES))
    {
        return 0;
    }
    int count = 0;
    for (uintptr_t offset = 0; offset < PILOT_SCAN_BYTES && count < MOST_SEAT_AIMERS; offset += 4)
    {
        const uintptr_t value = *reinterpret_cast<const uintptr_t*>(entity + offset);
        uintptr_t aimer = 0;
        if (IsPilotAimer(value, entity))
        {
            aimer = value;
        }
        else if (offset >= 4 && IsPilotAimer(entity + offset, entity))
        {
            aimer = entity + offset; // embedded
        }
        bool isKnown = !aimer;
        for (int index = 0; index < count && !isKnown; ++index)
        {
            isKnown = found[index] == aimer;
        }
        if (!isKnown)
        {
            how[count] = 'p';
            found[count++] = aimer;
        }
    }
    static uintptr_t loggedEntity = 0;
    if (entity != loggedEntity)
    {
        loggedEntity = entity;
        Log::Info("vehicle", "pilot aimers: %d found in the vehicle %p", count, reinterpret_cast<void*>(entity));
    }
    return count;
}

// Looks up the seat's Aimers again (each slot's Aimer and the Aimers linked from it with the same owner), and logs
// them when the set changes.
void FindAimers(uintptr_t controllable)
{
    _isRegionCacheOn = true; // regions do not change within one look-up
    _queriedRegionCount = _nextQueriedRegion = 0;
    struct RegionCacheOff
    {
        ~RegionCacheOff()
        {
            _isRegionCacheOn = false;
        }
    } regionCacheOff;
    int slotCount = 0;
    const uintptr_t slots = AimerSlots(controllable, &slotCount);
    uintptr_t found[MOST_SEAT_AIMERS] = {};
    int count = 0;
    char how[MOST_SEAT_AIMERS] = {};
    if (slots && !IsBadReadPtr(reinterpret_cast<void*>(slots), slotCount * sizeof(uintptr_t)))
    {
        for (int slot = 0; slot < slotCount && count < MOST_SEAT_AIMERS; ++slot)
        {
            uintptr_t aimer = reinterpret_cast<const uintptr_t*>(slots)[slot];
            if (!IsAimer(aimer))
            {
                continue;
            }
            const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(aimer + OFFSET_aimerOwner);
            for (int link = 0; link <= MOST_AIMER_LINKS && count < MOST_SEAT_AIMERS; ++link)
            {
                bool isKnown = false;
                for (int index = 0; index < count; ++index)
                {
                    isKnown = isKnown || found[index] == aimer;
                }
                if (isKnown || !IsAimer(aimer) ||
                    *reinterpret_cast<const uintptr_t*>(aimer + OFFSET_aimerOwner) != owner)
                {
                    break;
                }
                how[count] = static_cast<char>(link == 0 ? '0' + slot : 'a' + slot);
                found[count++] = aimer;
                aimer = *reinterpret_cast<const uintptr_t*>(aimer + OFFSET_aimerNext);
            }
        }
    }
    if (count == 0 && (_seat == Seat::Hover || _seat == Seat::Flyer || _seat == Seat::Walker))
    {
        count = ScanPilotAimers(controllable - OFFSET_vehicleControllable, found, how);
    }
    bool isSame = count == _aimerCount;
    for (int index = 0; index < count && isSame; ++index)
    {
        isSame = found[index] == _aimers[index];
    }
    memcpy(_aimers, found, sizeof(_aimers));
    _aimerCount = count;
    if (isSame)
    {
        return;
    }
    Log::Info("vehicle", "seat aimers: %d (slot digit = a slot's Aimer, letter = linked from that slot)", count);
    FpGuard fp;
    for (int index = 0; index < count; ++index)
    {
        const float* direction = reinterpret_cast<const float*>(found[index] + OFFSET_aimerDirection);
        const float* from = reinterpret_cast<const float*>(found[index] + OFFSET_aimerFirePoint);
        Log::Info(
            "vehicle",
            "  aimer %c %p owner %p: fire point %.2f %.2f %.2f, direction %.3f %.3f %.3f",
            how[index],
            reinterpret_cast<void*>(found[index]),
            reinterpret_cast<void*>(*reinterpret_cast<const uintptr_t*>(found[index] + OFFSET_aimerOwner)),
            static_cast<double>(from[0]),
            static_cast<double>(from[1]),
            static_cast<double>(from[2]),
            static_cast<double>(direction[0]),
            static_cast<double>(direction[1]),
            static_cast<double>(direction[2]));
    }
}

// The trace line (`vehicle trace`): headings in degrees (right positive) of the head, the seat's aim, the body and
// each Aimer, with the elevations of the head and the aim.
void LogTrace()
{
    const uintptr_t controllable = _seatControllable;
    if (!controllable || IsBadReadPtr(reinterpret_cast<void*>(controllable + OFFSET_controllableAim), 12))
    {
        return;
    }
    FpGuard fp;
    float head[16], headHeading = 0, headElevation = 0, aimHeading = 0, aimElevation = 0, bodyHeading = 0, unused = 0;
    if (Stereo::GetLastHeadWorld(head))
    {
        const float headForward[3] = {-head[8], -head[9], -head[10]};
        Angles(headForward, &headHeading, &headElevation);
    }
    Angles(reinterpret_cast<const float*>(controllable + OFFSET_controllableAim), &aimHeading, &aimElevation);
    const uintptr_t body = _seat == Seat::Turret ? controllable - OFFSET_turretControllable + OFFSET_turretMatrix
                                                 : controllable - OFFSET_vehicleControllable + OFFSET_entityMatrix;
    if (!IsBadReadPtr(reinterpret_cast<void*>(body), 64))
    {
        Angles(reinterpret_cast<const float*>(body) + 8, &bodyHeading, &unused);
    }
    char aimers[160] = "";
    size_t used = 0;
    for (int index = 0; index < _aimerCount && used + 16 < sizeof(aimers); ++index)
    {
        float heading = 0, elevation = 0;
        if (IsAimer(_aimers[index]))
        {
            Angles(reinterpret_cast<const float*>(_aimers[index] + OFFSET_aimerDirection), &heading, &elevation);
        }
        used += static_cast<size_t>(snprintf(
            aimers + used,
            sizeof(aimers) - used,
            " %.1f/%.1f",
            static_cast<double>(heading * DEGREES_PER_RADIAN),
            static_cast<double>(elevation * DEGREES_PER_RADIAN)));
    }
    Log::Info(
        "vehicle",
        "trace: head %.1f/%.1f, aim %.1f/%.1f, body %.1f, ray start %.2f m and target %.1f m from the head, aimers%s",
        static_cast<double>(headHeading * DEGREES_PER_RADIAN),
        static_cast<double>(headElevation * DEGREES_PER_RADIAN),
        static_cast<double>(aimHeading * DEGREES_PER_RADIAN),
        static_cast<double>(aimElevation * DEGREES_PER_RADIAN),
        static_cast<double>(bodyHeading * DEGREES_PER_RADIAN),
        static_cast<double>(_turret.rayStartMeters),
        static_cast<double>(_turret.targetMeters),
        aimers);
}

} // namespace

int GetSeatFireLines(float* lines, int most)
{
    const uintptr_t controllable = _seatControllable;
    const DWORD now = GetTickCount();
    if (controllable != _aimerControllable || now - _aimerTick >= AIMER_REFRESH_MILLISECONDS)
    {
        _aimerControllable = controllable;
        _aimerTick = now;
        LARGE_INTEGER start, end, frequency;
        QueryPerformanceCounter(&start);
        FindAimers(controllable);
        QueryPerformanceCounter(&end);
        QueryPerformanceFrequency(&frequency);
        const double milliseconds =
            static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
        static DWORD loggedTick = 0;
        static uintptr_t loggedControllable = 0;
        if (controllable != loggedControllable || milliseconds > 2.0 || now - loggedTick >= 10000)
        {
            loggedControllable = controllable;
            loggedTick = now;
            Log::Info("vehicle", "aimer look-up: %.2f ms (%d regions asked)", milliseconds, _queriedRegionCount);
        }
    }
    int lineCount = 0;
    FpGuard fp;
    for (int index = 0; index < _aimerCount && lineCount < most; ++index)
    {
        const uintptr_t aimer = _aimers[index];
        if (IsBadReadPtr(reinterpret_cast<void*>(aimer), AIMER_READ_BYTES))
        {
            continue;
        }
        const float* direction = reinterpret_cast<const float*>(aimer + OFFSET_aimerDirection);
        const float* from = reinterpret_cast<const float*>(aimer + OFFSET_aimerFirePoint);
        const float length =
            sqrtf(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
        if (!(fabsf(length - 1.0f) <= UNIT_TOLERANCE) || (from[0] == 0 && from[1] == 0 && from[2] == 0))
        {
            continue;
        }
        float* line = lines + lineCount * 6;
        for (int axis = 0; axis < 3; ++axis)
        {
            line[axis] = from[axis];
            line[3 + axis] = direction[axis] / length;
        }
        ++lineCount;
    }
    return lineCount;
}

void SetTrace(unsigned milliseconds)
{
    _traceEndTick = GetTickCount() + milliseconds;
    _traceTick = 0;
    Log::Info("vehicle", "trace for %u ms", milliseconds);
}

bool GetHullForward(float* forwardX, float* forwardZ)
{
    float matrix[16];
    if (!GetHullMatrix(matrix))
    {
        return false;
    }
    const float x = matrix[8], z = matrix[10];
    const float level = sqrtf(x * x + z * z);
    if (level < 0.1f)
    {
        return false;
    }
    *forwardX = x / level;
    *forwardZ = z / level;
    return true;
}

void OnPresent()
{
    if (!_areHooksTried && Xr::IsRunning())
    {
        InstallHooks();
    }
    static Seat loggedSeat = Seat::None;
    static uintptr_t loggedControllable = 0;
    const Seat seat = _seat;
    const uintptr_t controllable = _seatControllable;
    if (seat != loggedSeat || controllable != loggedControllable)
    {
        loggedSeat = seat;
        loggedControllable = controllable;
        const uintptr_t vehicle = seat == Seat::Turret || seat == Seat::Passenger
                                      ? controllable - OFFSET_turretControllable
                                      : controllable - OFFSET_vehicleControllable;
        const uintptr_t parent =
            seat == Seat::Turret && !IsBadReadPtr(reinterpret_cast<void*>(vehicle + OFFSET_turretParent), 4)
                ? *reinterpret_cast<uintptr_t*>(vehicle + OFFSET_turretParent)
                : 0;
        Log::Info(
            "vehicle",
            "seat: %s (Controllable %p, object %p, parent %p)",
            SeatName(seat),
            reinterpret_cast<void*>(controllable),
            reinterpret_cast<void*>(vehicle),
            reinterpret_cast<void*>(parent));
        // Every seat taken (each seat of a vehicle, a turret, a fighter) starts from the seat's own view: the head's
        // pose now becomes the resting pose, so a player who turned round or walked in the room is not half through
        // the seat or facing back (headset run 19).
        if (seat != Seat::OnFoot && seat != Seat::None)
        {
            Xr::Recenter();
            Log::Info("vehicle", "view recentred on entry");
        }
    }
    static DWORD lastLogTick = 0;
    const DWORD now = GetTickCount();
    if (static_cast<LONG>(_traceEndTick - now) > 0 && now - _traceTick >= TRACE_INTERVAL_MILLISECONDS)
    {
        _traceTick = now;
        LogTrace();
    }
    if (_isLogOn && seat != Seat::OnFoot && seat != Seat::None && now - lastLogTick >= LOG_INTERVAL_MILLISECONDS)
    {
        lastLogTick = now;
        LogSummary();
    }
}

void RequestEnter(const char* kind)
{
    const Seat wanted = !strncmp(kind, "hover", 5)    ? Seat::Hover
                        : !strncmp(kind, "flyer", 5)  ? Seat::Flyer
                        : !strncmp(kind, "walker", 6) ? Seat::Walker
                        : !strncmp(kind, "turret", 6) ? Seat::Turret
                                                      : Seat::Other;
    _isGunnerSeatWanted = strchr(kind, '+') != nullptr;
    const char* name = strchr(kind, ' ');
    snprintf(_enterName, sizeof(_enterName), "%s", name ? name + 1 : "");
    InterlockedExchange(&_enterRequest, static_cast<LONG>(wanted));
}

uint8_t _snapshot[0x2000];
bool _hasSnapshot = false;

void Snapshot(bool isDiff)
{
    const uintptr_t controllable = _seatControllable;
    const uintptr_t entity = _seat == Seat::Turret || _seat == Seat::Passenger ? controllable - OFFSET_turretControllable
                                                                                  : controllable - OFFSET_vehicleControllable;
    if (!controllable || IsBadReadPtr(reinterpret_cast<void*>(entity), sizeof(_snapshot)))
    {
        Log::Info("vehicle", "snapshot: no seat entity");
        return;
    }
    if (!isDiff)
    {
        memcpy(_snapshot, reinterpret_cast<void*>(entity), sizeof(_snapshot));
        _hasSnapshot = true;
        Log::Info("vehicle", "snapshot: %u bytes of %p kept", static_cast<unsigned>(sizeof(_snapshot)), reinterpret_cast<void*>(entity));
        return;
    }
    if (!_hasSnapshot)
    {
        Log::Info("vehicle", "snapshot: none to compare");
        return;
    }
    FpGuard fp;
    int shown = 0;
    char line[512];
    size_t used = 0;
    for (size_t offset = 0; offset + 4 <= sizeof(_snapshot) && shown < 80; offset += 4)
    {
        float before, after;
        memcpy(&before, _snapshot + offset, 4);
        memcpy(&after, reinterpret_cast<const uint8_t*>(entity) + offset, 4);
        const bool isFloat = std::isfinite(before) && std::isfinite(after) && fabsf(before) < 1e5f && fabsf(after) < 1e5f &&
                             (fabsf(before) > 1e-4f || fabsf(after) > 1e-4f);
        if (!isFloat || before == after || fabsf(after - before) < 0.05f)
        {
            continue;
        }
        used += static_cast<size_t>(snprintf(line + used, sizeof(line) - used, " +0x%x %.2f>%.2f", static_cast<unsigned>(offset), static_cast<double>(before), static_cast<double>(after)));
        ++shown;
        if (used > 400)
        {
            Log::Info("vehicle", "diff:%s", line);
            used = 0;
        }
    }
    if (used)
    {
        Log::Info("vehicle", "diff:%s", line);
    }
    Log::Info("vehicle", "diff: %d floats changed by 0.05 or more", shown);
}

void SetTest(bool isHeadAimOn, bool isLogOn)
{
    _isHeadAimOn = isHeadAimOn;
    _isLogOn = isLogOn;
    Log::Info("vehicle", "head aim %s, summary log %s", isHeadAimOn ? "on" : "off", isLogOn ? "on" : "off");
}

bool GetNetSeatInputs(NetSeatInputs* inputs)
{
    if (!_netSeatTick || GetTickCount() - _netSeatTick > 200)
    {
        return false;
    }
    *inputs = _netSeat;
    return true;
}

} // namespace Vehicle

} // namespace SwbfVr
