#pragma once

#include <cstdint>

/// Vehicle: the player in vehicles, turrets and gunner seats (PLAN 54). Generic by the engine class of the seat, so
/// vehicles made by modders work too: every seat type reads the same four inputs on its Controllable (+0x54 move,
/// +0x58 strafe, +0x5C look yaw, +0x60 look pitch, set by PlayerController::Update), and the engine turns them into
/// turret, cannon or flight motion within the ODF's limits and rates.
///
/// Head aim (idren2023, 2026-10-17): in a seat with a gun the look yaw / pitch inputs are replaced by a closed loop that
/// turns the seat's aim (Controllable+0xEC, which the engine builds from its aim angles) toward where the head looks.
/// The engine's rate per input unit differs by class and by ODF, so the loop measures it while it runs.
namespace SwbfVr
{
namespace Vehicle
{
/// The kind of seat the player is in, by the engine class of its Controllable (RTTI).
enum class Seat
{
    None,      // not in a match, or not seen yet
    OnFoot,    // EntitySoldier, EntityDroideka
    Hover,     // EntityHover, EntityCommandHover: pilot (look yaw turns the hull)
    Flyer,     // EntityFlyer, EntityCarrier: pilot (all four inputs fly)
    Walker,    // EntityWalker, EntityCommandWalker: pilot (look yaw / pitch aim its gun)
    Turret,    // MountedTurret: a gunner seat of a vehicle, or a building turret
    Passenger, // PassengerSlot
    Other
};

/// How the player drives the vehicle of a pilot seat (idren2023 2026-10-17): the sticks by default, so modded vehicles
/// work; stock vehicles may have an override for immersion (the table at the top of Vehicle.cpp).
enum class Drive
{
    Sticks,    // the thumbsticks only (default; animal mounts, every vehicle not in the table)
    Levers,    // two tank levers (Levers.cpp) besides the sticks
    Handlebars, // the vehicle's own model round the rider, hands on its grips (Handlebars.cpp); the sticks drive
    FlightStick, // a flyer's stick, throttle and take-off lever (Flight.cpp) besides the sticks (every flyer by default)
    Throttle     // a walker's throttle lever (WalkerDrive.cpp), the head steering (every walker by default)
};

/// The player's seat, as PlayerController last handed it its input (game thread; any thread may read it).
Seat CurrentSeat();

/// While the player flies a recon droid (an EntityDroid, Seat::Other; idren2023 2026-11-22): the head aims it (the
/// look loop as for a gun seat, the view held so the droid turns under it), the left stick moves it, the right
/// trigger fires its blaster, the left trigger its orbital strike.
bool IsRemoteDroid();

/// How the vehicle of the player's pilot seat is driven (Sticks outside a pilot seat).
Drive CurrentDrive();

/// Setting vehicle_controls (idren2023 2026-11-04): sticks = the handles, levers and reins are not taken, every vehicle
/// drives from the thumbsticks (A takes off and lands, the right stick's turn overrides head steering).
bool IsSticksMode();
void SetSticksMode(bool isSticks);

/// The ODF name of the stock vehicle of the player's pilot or gunner seat (a gunner seat: the vehicle it is mounted
/// on), when it is one of the table's, else null.
const char* CurrentStockName();

/// The player's seat's Controllable (0 outside a seat), to tell a seat change apart.
uintptr_t CurrentSeatControllable();

/// The entity the player's seat sits in: the vehicle of a pilot seat, the vehicle or building a turret or passenger
/// slot is mounted on (MountedTurret +0x2C8); 0 on foot or outside a seat. Game thread.
uintptr_t CurrentHullEntity();

/// The pilot seat's vehicle's health as a fraction of its most (the vehicle's Damageable at +8: health +0xC, most
/// +0x10; run501), or -1 outside a pilot seat. Game thread.
float HullHealthFraction();

/// The heat of a flyer pilot's weapon (its WeaponCannon at flyer +0x58C, heat +0x90, 0 cool to 1 overheated; run501),
/// or -1 outside a flyer's pilot seat or with another kind of weapon there. Game thread.
float WeaponHeatFraction();

/// The world matrix of the player's gunner seat's turret (MountedTurret +0x210; rows as the entity matrices), for the
/// gunner seats (Aim). Game thread.
/// @param matrix set to it (16 floats) when the result is true.
/// @return false outside a gunner seat or when it does not look like a rotation.
bool GetTurretMatrix(float* matrix);

/// The yaw of the player's gunner seat's turret relative to its parent (MountedTurret +0x2C0, radians) and the
/// section's yaw limits (+0xE30), for the combat speeder's turning seat and its end stops (GunnerSeat). Game thread.
/// @return false outside a gunner seat, when unreadable or when the turret's yaw is not limited (turns all the way).
bool GetTurretYaw(float* yaw, float* low, float* high);

/// The turret seat's own angles (MountedTurret +0x2B8 pitch, negative = up; +0x2C0 yaw, toward the right positive?;
/// radians, relative to the turret's parent), as the game uses them this frame.
/// @return false outside a turret seat.
bool GetTurretAngles(float* pitch, float* yaw);

/// The lines the player's seat's guns shoot along (world), for the aiming line in seats (PLAN 57 g, h, b): each of
/// the seat's Aimers' fire point and aim direction, as the game's fire code reads them (the shot's direction before
/// spread and ShotElevate). Seats with Aimers of their own: a turret or gunner seat (MountedTurret) and the droideka
/// (whose arms the game aims, rate-limited, at the point it looks at). Render or game thread (one thread).
/// @param lines set to up to `most` lines, 6 floats each: the fire point, then the unit direction.
/// @param most the room in `lines`, in lines.
/// @return how many lines were written; 0 in other seats, on foot as a soldier, or when nothing looks valid.
int GetSeatFireLines(float* lines, int most);

/// The point the game aims the guns of the player's turret seat at (Controllable+0x10, world): where its own aim ray
/// hits, which the Aimers converge on (shots land there). Game or render thread (one thread).
/// @param point set to it (3 floats) when the result is true.
/// @return false outside a turret seat or when it is not readable.
bool GetSeatTarget(float* point);

/// Test (script command `vehicle trace <milliseconds>`): for that long, a log line every 50 ms with the headings of
/// the head, the seat's aim (Controllable +0xEC), the body (a droideka's or a vehicle's matrix) and each Aimer, to
/// tell the game's turn limits from our loop's lag (PLAN 57 b).
/// @param milliseconds how long; 0 stops.
void SetTrace(unsigned milliseconds);

/// The level heading of the hull the player's seat sits in (the vehicle; for a turret, what it is mounted on), for
/// the view in a vehicle (Stereo). Game thread.
/// @param forwardX, forwardZ set to the unit horizontal forward in the world when the result is true.
/// @return false on foot, outside a seat, or when the hull's matrix does not look like one.
bool GetHullForward(float* forwardX, float* forwardZ);

/// The level heading of the gun in a turret seat (its yaw on what it is mounted on), for the view in a turret aimed by
/// the hand (Stereo; GunnerSeat::IsAimedByHand). Game thread.
/// @param forwardX, forwardZ set to the unit horizontal forward in the world when the result is true.
/// @return false outside a turret seat or when its mount's matrix does not look like one.
bool GetTurretForward(float* forwardX, float* forwardZ);

/// The world matrix of the hull the player's seat sits in (as GetHullForward), for the cockpit model (Aim).
/// @param matrix set to the hull entity's matrix (16 floats, row vectors: right, up, forward, position) when the
/// result is true.
/// @return false on foot, outside a seat, or when the hull's matrix does not look like one.
bool GetHullMatrix(float* matrix);

/// Once per presented frame (main thread): installs the hooks once VR runs; logs a seat change and, while in a
/// vehicle, a once-per-second summary.
void OnPresent();

/// Test (script command `vehicle enter [hover|flyer|walker|turret][+] [odf name]`): the player on foot enters the
/// nearest vehicle of that kind within 150 m that lets it in (any kind without a word), with the next update; with `+`
/// only one with a gunner seat (hover, flyer); with an ODF name only that vehicle (e.g. imp_hover_speederbike).
/// @param kind the kind's word, then optionally a space and the ODF name; or empty.
void RequestEnter(const char* kind);

/// Test switches (script command `vehicle`): head aim on or off (default on), and the summary log.
/// @param isHeadAimOn the look inputs follow the head in seats with a gun.
/// @param isLogOn the once-per-second summary while in a vehicle.
void SetTest(bool isHeadAimOn, bool isLogOn);

/// Network game: what the mod wants of the player's seat (vehicle, turret, gunner seat) in the next game turn. In
/// single player the seat's hooks set these on the Controllable themselves; that never reaches a host.
struct NetSeatInputs
{
    /// The player sits in a seat (not on foot).
    bool isInSeat;
    /// The look yaw / look pitch rates are the mod's (head aim): they replace the game's.
    bool hasYaw, hasPitch;
    /// Look rates in the game's units for a seat (-1..1).
    float yaw, lookPitch;
    /// The levers' drive (tank handles, bike lean, flight stick, walker lever), added to the game's four inputs.
    float move, strafe, turn, pitch;
};
/// @param inputs set when the result is true.
/// @return false outside a network game or when the seat's hooks have not run within the last 200 ms.
bool GetNetSeatInputs(NetSeatInputs* inputs);

/// Test (reverse engineering): `vehicle snap` keeps a copy of the seat's entity (0x2000 bytes); `vehicle diff` logs the
/// floats that changed since (offset, old, new; at most 80, the largest changes first not sorted: in offset order).
void Snapshot(bool isDiff);
} // namespace Vehicle
} // namespace SwbfVr
