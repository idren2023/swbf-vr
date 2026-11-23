#pragma once

/// Net: what the mod knows about a network game (docs/research/netcode.md).
///
/// A network game keeps two worlds in one process: the host world (the simulation the host owns; on a client it is
/// not simulated) and the client world (the predicted copy the player sees). The game switches between them several
/// times per frame; entity and weapon code runs in whichever is active. The addresses are the Steam build's
/// (GameBuild::IsKnownBuild); on another build every function answers "no network game".
///
/// All functions run on the game's main thread.

#include <cstdint>

namespace SwbfVr
{

namespace Net
{

/// Whether a network game is running (the game's own flag).
bool IsNetGame();
/// Whether this machine hosts the network game.
bool IsHost();
/// Whether the game is working in the client world right now. False outside a network game.
bool IsClientWorldActive();
/// The control state of one player for one game turn: all a network client sends the host about its controls
/// (20 bytes; docs/research/net/client-input.md section 1).
struct ControlState
{
    /// -1..1, right is positive.
    float strafe;
    /// -1..1, forward is positive.
    float move;
    /// Right is positive; -16..16 on foot, -1..1 in a vehicle, turret or seat.
    float turnRate;
    /// Up is positive; same ranges as the turn rate.
    float pitchRate;
    /// Bit = the game's control function (0 fire, 1 secondary fire, 2 jump, ...); bits 30-31 the device class.
    uint32_t buttons;
};

/// Changes the local player's control state of one turn.
using ControlStateWriter = void (*)(ControlState& state);
/// Has `writer` called once per game turn of a network game, on the local player's control state just after the
/// game copied it from its input mapper and before it is predicted locally and sent to the host. This is the only
/// place where the mod's controls reach the host; it is not called in single player. Installs the hook on first
/// use (Steam build only; elsewhere it does nothing). One writer; a second call replaces the first.
void SetLocalControlWriter(ControlStateWriter writer);
/// Presses one of the game's control buttons for the local player in the next game turn of a network game (the
/// game acts on such a button's rising edge, in the client world and on the host alike). For what the mod does by
/// calling the game directly in single player: such a call never reaches the host.
/// @param bit the button's bit in ControlState::buttons (8-11 squad commands, 12-15 weapon next / previous, ...).
void PressButton(int bit);
/// The local player's soldier in one of the two worlds.
/// @param isClientWorld true: the client world (the predicted copy the player sees); false: the host world.
/// @param transform set to the soldier's world matrix (16 floats: rows left, up, forward, position).
/// @param pitchRadians set to the soldier's aim pitch, up positive.
/// @return false outside a network game, or when the local player has no soldier there (dead, in a vehicle).
bool GetLocalSoldier(bool isClientWorld, float* transform, float* pitchRadians);
/// The point the local player's soldier aims at in the client world: where the game's own ray along the soldier's
/// aim ends (Controllable +0x10, written by the soldier's aim function each update).
/// @param point set to x, y, z (world) when the result is true.
/// @return false outside a network game or without a soldier.
bool GetLocalSoldierTarget(float* point);
/// The game's own world ray cast (0xA40C0, the one a soldier's aim uses, collision mask 0x4E) in the active world,
/// with the local player's soldier of that world left out. Game thread only.
/// @param from start of the ray, x, y, z (world).
/// @param direction unit vector.
/// @param rangeMeters length of the ray.
/// @param distanceMeters set to the distance of the first hit, or to rangeMeters when nothing was hit.
/// @return true when something was hit; false also outside a network game or on an unknown build.
bool CastAimRay(const float* from, const float* direction, float rangeMeters, float* distanceMeters);
/// Length of one game turn of the network game in seconds (the host's setting, 1/60 by default).
float GetTurnSeconds();
/// Test: one log line per game turn (`NT`) with the control state as sent and the local soldier of both worlds
/// (script command `net trace 1` / `net trace 0`; tools/nettrace.py reads them).
void SetTrace(bool isOn);
/// Whether the local player's own code should act now: always outside a network game; in one, only while the
/// client world is simulated for the first time in this turn. (The host world runs every player's controller, and
/// the client world simulates its unacknowledged turns again after each update from the host: a control loop fed
/// by those repeats swings, run 688.)
bool IsLocalPlayerStep();
/// Logs the network flags and, for each world, the local player's unit: position, heading, pitch and the four
/// control axes (script command `net`). The test of what reaches the host: on a listen server the host world's
/// soldier is driven only by the control state the client sends.
void LogState();

} // namespace Net
} // namespace SwbfVr
