#pragma once

/// Gesture: where the hands are relative to the head and the body (zones), which way the palms face and whether the
/// grip is held, for controls done by motion (Gesture.cpp). Read by Controls (ear menu, reload, zoom) and Hud (wrist
/// panel).
///
/// All functions run on the game's main thread.

namespace SwbfVr
{

namespace Gesture
{

/// The zones a hand can be in (centres and radii in the table at the top of Gesture.cpp).
enum class Zone
{
    None,
    EarRight,
    EarLeft,
    Eye,
    ChestRight,
    ChestLeft,
    HipRight,
    HipLeft,
    UnderGun,
    BeltLeft // the ammo canister's place, front left of the belt (left hand only; manual reload, Holster)
};

/// The state of one hand in this presented frame.
struct Hand
{
    bool isTracked;
    Zone zone;
    bool hasEntered; // the zone was entered in this frame
    bool isPalmUp;
    bool isGripped;
    float pose[16]; // the controller's pose in resting-pose space
};

/// Works out both hands' state from the head and controller poses. Once per presented frame, after Aim::OnPresent
/// (called from the Present hook in D3dSurvey.cpp).
void OnPresent();

/// One hand's state of this frame.
/// @param hand 0 right, 1 left (only the lowest bit is used).
/// @return the hand's state; all false and Zone::None while the hand is not tracked.
const Hand& CurrentHand(int hand);

/// A zone's name for the log ("right ear", ...).
/// @param zone the zone.
/// @return its name, "?" for a value outside the enum.
const char* ZoneName(Zone zone);

/// The torso frame of this presented frame: under the head, upright, turned like the body (estimated), in
/// resting-pose space (rows right, up, back, position).
/// @param matrix receives the 4x4 row-vector matrix.
/// @return false while the head is not tracked.
bool GetTorso(float* matrix);
/// Switches the log lines of zone and palm changes on or off (on by default).
/// @param isOn true logs them.
void SetLogging(bool isOn);

} // namespace Gesture

} // namespace SwbfVr
