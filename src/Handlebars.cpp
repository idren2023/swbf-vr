// Handlebars.cpp: the speeder bikes' and the STAP's pilot seat (PLAN 55; see Handlebars.h).
//
// Spaces. Hull model space is the msh's (x the rider's left, y up, z forward; the rider sits at the origin's
// hp_active, as the pilot animations human_speederbike_sit and human_stap_ride place it). Resting-pose space is the
// controllers' (x right, y up, z back, origin at the resting head; Gesture::Hand::pose). In the seat the level camera
// (the view's base, Stereo) stands at the rider's eye on the hull's level heading, so resting-pose space is hull model
// space moved to the eye and turned half round y: rest = (-(m.x - eye.x), m.y - eye.y, -(m.z - eye.z)).
//
// The hull is drawn by the game's own model draw (model vtable slot 1, as Holster's weapons) at the hull's matrix
// with its pitch and roll taken out (the view stays level; hovers rock while standing, run224). The speeder bike's
// hull is skinned (its handlebars, foils, rear flaps, pedals and gun are bones that the game's animation turns); it is
// drawn through a pose table of ours with every bone at its rest place, so the handlebars stay still.
//
// The animal mounts (tauntaun, Kaadu; the vehicle proposal 2026-11-04, idren2023: ridden in the third-person model as the
// bikes, reins with grips, controlled like the bikes) are walkers: skinned, and their run cycle is the game's. They
// are not drawn by us: in first person the game skips the walker's render for the player's own vehicle (its Trackable
// visibility check, GameBuild WalkerVisible), and a hook on that check lets it through for the ridden mount, so the game
// draws the animal itself, animated, round the rider's eye; the game's stock first-person model (the tauntaun's head
// and neck) is left out. A walker turns on its strafe input (Vehicle adds the lean there) and has no gun (no head aim).

#include "Handlebars.h"

#include "Feedback.h"
#include "FpGuard.h"
#include "GameBuild.h"
#include "Gesture.h"
#include "Holster.h"
#include "Log.h"
#include "Stereo.h"
#include "Vehicle.h"
#include "Xr.h"

#include <windows.h>

#include <MinHook.h>

#include <cmath>
#include <cstring>

namespace SwbfVr
{

namespace Handlebars
{

namespace
{

/// A grip press takes the grip when the hand is this close to it (as the tank levers' grip zone).
constexpr float REACH_METERS = 0.20f;
/// The game camera within this of the rider's eye means first person (third person: 5-6 m behind, ODF TrackCenter /
/// TrackOffset).
constexpr float FIRST_PERSON_RANGE_METERS = 3.0f;
/// Model draw flags added to the first-person pass's (as Holster's and the arms' draws).
constexpr unsigned MODEL_DRAW_FLAGS = 0x80001;
/// Pose table: the game's is 0x408 bytes (count, 0x80 hashes, 0x80 matrix pointers, ...); room as Aim's.
constexpr size_t POSE_TABLE_BYTES = 0x500;
constexpr float RADIANS_PER_DEGREE = 0.01745329f;
/// Lean steering (idren2023 2026-10-17): the neck's sideways place from the resting head turns the vehicle, nothing within
/// the dead zone, full turn at the full lean. The neck, not the eyes: turning the head to look round swings the eyes
/// about the neck by several centimetres; the neck pivot sits this far behind and below the point between the eyes.
constexpr float LEAN_DEAD_METERS = 0.04f, LEAN_FULL_METERS = 0.15f;
constexpr float NECK_BACK_METERS = 0.09f, NECK_DOWN_METERS = 0.08f;
/// Twist-grip throttle (idren2023 2026-10-17; an option since PLAN 57 f, ini `bike_throttle twist`): the right hand's
/// pitch since it took its grip, as a motorcycle's grip is rolled back (the hand's pointing line rises) = forward,
/// rolled forward = brake and reverse; dead zone, full.
constexpr float TWIST_DEAD_RADIANS = 5.0f * RADIANS_PER_DEGREE, TWIST_FULL_RADIANS = 30.0f * RADIANS_PER_DEGREE;
/// Lean throttle (idren2023 2026-10-18, PLAN 57 f, the default): the head's forward place relative to the hands, both
/// measured from where they were when the grips were taken (the rider's neutral posture). The head brought forward
/// over the hands = faster (dead zone, full); the hands pushed far out ahead of the head = reverse, which takes a
/// longer reach so that a rider settling the hands does not back up.
constexpr float LEAN_AHEAD_DEAD_METERS = 0.03f, LEAN_AHEAD_FULL_METERS = 0.18f;
constexpr float REACH_OUT_DEAD_METERS = 0.08f, REACH_OUT_FULL_METERS = 0.20f;
/// The rider's shoulder from the eyes (our hands on the bars: the forearm points from its elbow; Aim's SHOULDER_*).
constexpr float SHOULDER_BELOW_METERS = 0.24f, SHOULDER_SIDE_METERS = 0.19f, SHOULDER_BEHIND_METERS = 0.07f;
/// The drive log (test `bike log 1`): at most this often.
constexpr DWORD DRIVE_LOG_MILLISECONDS = 1000;
/// The game camera's place in the hull is logged at a seat change and this long after it (settled).
constexpr DWORD CAMERA_LOG_DELAY_MILLISECONDS = 3000;

/// A bone of a skinned hull at rest: its place in hull model space (all stock bike bones have no rest rotation).
struct BoneRest
{
    const char* name;
    float position[3];
};

/// imp_ / rep_hover_speederbike.msh (same skeleton; tools/msh.py): bone_root at 0.679 m, the others under it.
const BoneRest SPEEDER_BIKE_BONES[] = {
    {"bone_root", {0.0f, 0.679f, 0.0f}},
    {"bone_l_handlebar", {0.253f, 0.738f, 0.793f}},
    {"bone_r_handlebar", {-0.253f, 0.738f, 0.793f}},
    {"bone_l_foil", {0.274f, 0.632f, 2.180f}},
    {"bone_r_foil", {-0.274f, 0.632f, 2.180f}},
    {"gun", {0.0f, 0.525f, 0.871f}},
    {"bone_l_rearflap", {0.172f, 0.820f, -1.007f}},
    {"bone_r_rearflap", {-0.172f, 0.820f, -1.007f}},
    {"bone_l_footpedal", {0.206f, 0.468f, -0.674f}},
    {"bone_r_footpedal", {-0.206f, 0.468f, -0.674f}},
};
constexpr int MOST_BONES = 16;

/// A stock vehicle ridden with handlebars. Places in hull model space.
struct Bike
{
    const char* odfName;    // as Vehicle's table (Vehicle::CurrentStockName)
    const char* hullName;   // the ODF's GeometryName: the third-person model, drawn in first person
    float eye[3];           // the rider's resting eye
    float grip[3];          // the grips' centre (where the hand's grip pose sits): side (either way), height, ahead
    float gripPitchDegrees; // the hand on its grip points ahead, tilted down this far
    float gripRollDegrees;  // and rolled thumb-inward this far (the grips run sideways)
    const BoneRest* bones;  // a skinned hull's bones at rest, or null (a rigid hull)
    int boneCount;
    float barCentre[3];  // the right grip's bar (our hands, PLAN 61 j): a point on its axis where the fist goes
    float barOutward[3]; // and its direction outward (toward the rider's right)
    bool isMount = false;   // an animal (a walker seat): the game draws it, animated (WalkerVisibleHook), not DrawHull
};

// Speeder bike: the grips from the rider's fists in human_speederbike_sit (hands at x +-0.19, y 1.28, z 0.66, pointing
// ahead and 21 deg down) next to the grip nodes cube1 / cube2 (+-0.169, 1.245, 0.709), 2 cm inward onto the ribbed
// bars (run261). The roll: at 80 deg the first-person arms' forearms came in flat from the sides (run261), the
// arms model's forearm hanging below the hand in its rifle grip. That pose's eye (hp_camera,
// 0, 1.63, 0.51) is a racing crouch; the resting eye is put where a seated player's controllers at rest reach the
// grips: 35 cm above and 40 cm behind them (where the tank levers stood until b272). STAP: the rider stands on the
// foot rests (y 1.3-1.5) behind the pole; human_stap_ride's hands (relative to hp_active at y 0.36) close on the grip
// geometry at x +-0.3..0.4, y 2.4-2.5, z -0.5, pointing straight ahead; its eye (hp_camera) at (0, 2.94, -0.77). The
// eye was 35 cm behind the grips, "a little too far back" in headset run 16 (the speeder bike's felt right): 10 cm
// forward since PLAN 57 f, 25 cm behind them, 5 cm behind hp_camera.
const Bike BIKES[] = {
    {"imp_hover_speederbike",
     "imp_hover_speederbike",
     {0.0f, 1.62f, 0.28f},
     {0.16f, 1.27f, 0.68f},
     20.0f,
     40.0f,
     SPEEDER_BIKE_BONES,
     sizeof(SPEEDER_BIKE_BONES) / sizeof(SPEEDER_BIKE_BONES[0]),
     {-0.175f, 1.245f, 0.705f},
     {-0.998f, -0.012f, -0.054f}},
    {"rep_hover_speederbike",
     "rep_hover_speederbike",
     {0.0f, 1.62f, 0.28f},
     {0.16f, 1.27f, 0.68f},
     20.0f,
     40.0f,
     SPEEDER_BIKE_BONES,
     sizeof(SPEEDER_BIKE_BONES) / sizeof(SPEEDER_BIKE_BONES[0]),
     {-0.175f, 1.245f, 0.705f},
     {-0.998f, -0.012f, -0.054f}},
    // (headset run 19: the eye 10 cm higher, y 2.93)
    {"cis_hover_stap",
     "cis_hover_stap",
     {0.0f, 2.93f, -0.82f},
     {0.34f, 2.43f, -0.57f},
     5.0f,
     40.0f,
     nullptr,
     0,
     {-0.31f, 2.42f, -0.51f},
     {-0.803f, -0.595f, -0.032f}},
    // The mounts (first guesses from the mod tools' models, offline renders re/review/mounts/*_eye.png). The rider is
    // placed by the ODF's AnimatedPilotPosition bone (tauntaun bone_root (0, 2.06, -0.248), Kaadu bone_rider (0, 2.21,
    // -0.428)) in human_ride's pose, whose root sits on that bone: the rider's hands about (+-0.18..0.33, +0.46..0.59,
    // +0.48..0.53) from it, his head bone (0, +1.03, +0.52), a racing crouch over the neck. The grips (the reins in
    // the fists) at the hands' mean; the eye, as the bikes', where a seated player's resting controllers reach them,
    // sitting up: tauntaun 46 cm above and 33 behind them (its head, y up to 3.19 at z 1.0, under the horizon), Kaadu
    // 40 above and 35 behind. Fists upright on the reins (rolled 70 deg thumb up), pointing ahead 25 deg down.
    {"all_walk_tauntaun", "all_walk_tauntaun", {0.0f, 3.08f, -0.05f}, {0.18f, 2.62f, 0.28f}, 25.0f, 70.0f, nullptr, 0, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, true},
    {"gun_walk_kaadu", "gun_walk_kaadu", {0.0f, 3.135f, -0.27f}, {0.18f, 2.735f, 0.08f}, 25.0f, 70.0f, nullptr, 0, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, true},
};

/// The walker's Trackable within its entity (offsets "Camera": walker E+0x44C).
constexpr uintptr_t OFFSET_walkerTrackable = 0x44C;

using HashNameFunction = unsigned(__cdecl*)(const char*);
using PoseTableInitFunction = void*(__thiscall*)(void* table);
using PoseTableSetFunction = void(__thiscall*)(void* table, unsigned hash, const float* matrix);
/// void __thiscall model draw(const float* matrix, PoseTable* bones, void* light, unsigned flags, int), model vtable
/// slot 1 (offsets "RedModel"), as __fastcall.
using ModelDrawFunction = void(
    __fastcall*)(void* model, void* edx, const float* matrix, void* bones, void* light, unsigned flags, int unused);

/// One grip (index 0 right, 1 left, as the hands).
struct Grip
{
    bool isHeld;
    bool wasGripped;         // the hand's grip in the frame before: a grip is taken by a new press only
    float takenPitchRadians; // the controller's pitch when it took the grip (the twist grip's zero)
    float takenBackMeters;   // the controller's z (resting-pose space, back positive) when it took the grip
};

const Bike* _bike = nullptr; // the seat's bike this frame (OnPresent), or null
Grip _grips[2] = {};
unsigned _boneHashes[MOST_BONES] = {};
const Bike* _hashedBike = nullptr;                   // the bike _boneHashes belong to
const char* _missingModel = nullptr;                 // the hull name last logged as not loaded
float _testRollDegrees = -1, _testPitchDegrees = -1; // test `bike grip`: the hand on its grip, or -1 for the table's
float _move = 0, _turn = 0;                          // this frame's drive (-1..1; forward, right positive)
float _leanMeters = 0, _twistRadians = 0;            // the neck's lean (right positive), the right hand's twist
float _aheadMeters = 0;         // the lean throttle: head ahead of the hands since the grab (negative: hands out ahead)
float _neckTakenBackMeters = 0; // the neck's z when the first grip was taken (both free before)
bool _isTwistThrottle = false;  // ini bike_throttle: twist (true) or lean (false, the default)
bool _isDriveLogOn = false;
DWORD _lastDriveLogTick = 0;
DWORD _seatTick = 0; // when the seat began (for the camera log)
int _cameraLogsLeft = 0;

/// bool __thiscall Trackable visible(int view), as __fastcall (GameBuild WalkerVisible).
using VisibleFunction = bool(__fastcall*)(void* trackable, void* edx, int view);
VisibleFunction _originalWalkerVisible = nullptr;
bool _isMountHookTried = false;
bool _isMountHookOn = false;
/// The ridden mount's entity while the game is to draw it in first person (set in OnPresent, between frames; read by
/// the hook during the render), or 0.
volatile uintptr_t _mountEntity = 0;
unsigned _mountShownCount = 0; // renders the hook let through (logged once)

bool __fastcall WalkerVisibleHook(void* trackable, void* edx, int view)
{
    const bool isVisible = _originalWalkerVisible(trackable, edx, view);
    const uintptr_t mount = _mountEntity;
    if (!isVisible && mount && reinterpret_cast<uintptr_t>(trackable) == mount + OFFSET_walkerTrackable)
    {
        if (_mountShownCount++ == 0)
        {
            Log::Info("bike", "mount: the game's first-person hiding of the animal let through (drawn animated)");
        }
        return true;
    }
    return isVisible;
}

// Once VR runs: the hook on the walker's visibility check (main thread; MinHook as Vehicle's hooks).
void InstallMountHook()
{
    if (_isMountHookTried || !Xr::IsRunning())
    {
        return;
    }
    _isMountHookTried = true;
    if (!GameBuild::IsAvailable(GameBuild::Feature::VehicleAim))
    {
        Log::Info("bike", "mount hook: not installed (vehicle aim feature off): mounts keep the game's model");
        return;
    }
    void* const target = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::WalkerVisible));
    MH_Initialize(); // (already done by Vehicle: harmless)
    MH_STATUS status = MH_CreateHook(
        target,
        reinterpret_cast<void*>(&WalkerVisibleHook),
        reinterpret_cast<void**>(&_originalWalkerVisible));
    if (status == MH_OK)
    {
        status = MH_EnableHook(target);
    }
    _isMountHookOn = status == MH_OK;
    Log::Info("bike", "mount hook (walker visibility): %s", MH_StatusToString(status));
}

const Bike* FindBike(const char* stockName)
{
    if (!stockName)
    {
        return nullptr;
    }
    for (const Bike& bike : BIKES)
    {
        if (!strcmp(bike.odfName, stockName))
        {
            return &bike;
        }
    }
    return nullptr;
}

// The bike of the player's seat now, from Vehicle's state (any time on the main thread), or null.
const Bike* CurrentBike()
{
    const Vehicle::Seat seat = Vehicle::CurrentSeat();
    if (!Xr::IsRunning() || (seat != Vehicle::Seat::Hover && seat != Vehicle::Seat::Walker) ||
        Vehicle::CurrentDrive() != Vehicle::Drive::Handlebars)
    {
        return nullptr;
    }
    const Bike* bike = FindBike(Vehicle::CurrentStockName());
    return bike && bike->isMount == (seat == Vehicle::Seat::Walker) ? bike : nullptr;
}

// The hull's matrix on its level heading (rows: model x, up, forward, position; the model's x keeps its side of the
// hull's own row 0, whichever way the game's matrices are handed). FpGuard held.
bool LevelHull(float* level)
{
    float hull[16];
    if (!Vehicle::GetHullMatrix(hull))
    {
        return false;
    }
    const float length = sqrtf(hull[8] * hull[8] + hull[10] * hull[10]);
    if (length < 0.1f)
    {
        return false;
    }
    const float forwardX = hull[8] / length, forwardZ = hull[10] / length;
    // up x forward = (forwardZ, 0, -forwardX); flipped if the hull's own x points the other way.
    const float side = hull[0] * forwardZ - hull[2] * forwardX < 0 ? -1.0f : 1.0f;
    const float matrix[16] = {
        side * forwardZ,
        0,
        -side * forwardX,
        0,
        0,
        1,
        0,
        0,
        forwardX,
        0,
        forwardZ,
        0,
        hull[12],
        hull[13],
        hull[14],
        1};
    memcpy(level, matrix, sizeof(matrix));
    return true;
}

// A point of hull model space in the world, through the level hull matrix.
void ToWorld(const float* level, const float* point, float* world)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        world[axis] =
            point[0] * level[axis] + point[1] * level[4 + axis] + point[2] * level[8 + axis] + level[12 + axis];
    }
}

// A grip's centre in resting-pose space (hand 0 right, 1 left).
void GripRest(const Bike& bike, int hand, float* rest)
{
    rest[0] = hand == 0 ? bike.grip[0] : -bike.grip[0];
    rest[1] = bike.grip[1] - bike.eye[1];
    rest[2] = -(bike.grip[2] - bike.eye[2]);
}

// The pitch of a pose's pointing line (-z), up positive.
float Pitch(const float* pose)
{
    const float rise = -pose[9];
    return asinf(rise > 1.0f ? 1.0f : rise < -1.0f ? -1.0f : rise);
}

// A dead zone and a ramp to full: 0 within dead, +-1 from full on.
float Ramp(float value, float dead, float full)
{
    const float size = fabsf(value);
    if (size <= dead)
    {
        return 0.0f;
    }
    const float ramp = size >= full ? 1.0f : (size - dead) / (full - dead);
    return value < 0 ? -ramp : ramp;
}

// The neck pivot in resting-pose space (x right, z back), from the head pose (rows right, up, back, position): behind
// and below the eyes. False without a head pose.
bool GetNeck(float* neckRightMeters, float* neckBackMeters)
{
    float head[16];
    if (!Stereo::GetHeadPose(head))
    {
        return false;
    }
    *neckRightMeters = head[12] + head[8] * NECK_BACK_METERS - head[4] * NECK_DOWN_METERS;
    *neckBackMeters = head[14] + head[10] * NECK_BACK_METERS - head[6] * NECK_DOWN_METERS;
    return true;
}

// This frame's drive while a grip is held: steering by the neck's sideways place; the throttle by the head's place
// ahead of the hands (or, with the twist option, by the right hand's twist while it holds its grip).
void StepDrive()
{
    _move = _turn = _leanMeters = _twistRadians = _aheadMeters = 0.0f;
    float neckRight = 0, neckBack = 0;
    if ((_grips[0].isHeld || _grips[1].isHeld) && GetNeck(&neckRight, &neckBack))
    {
        _leanMeters = neckRight;
        _turn = Ramp(_leanMeters, LEAN_DEAD_METERS, LEAN_FULL_METERS);
        if (!_isTwistThrottle)
        {
            // Each held hand's forward travel since its grab (resting-pose z is back), against the neck's since the
            // first grab: the head over the hands = ahead positive.
            float handsAhead = 0;
            int heldCount = 0;
            for (int hand = 0; hand < 2; ++hand)
            {
                if (_grips[hand].isHeld)
                {
                    handsAhead += _grips[hand].takenBackMeters - Gesture::CurrentHand(hand).pose[14];
                    ++heldCount;
                }
            }
            _aheadMeters = (_neckTakenBackMeters - neckBack) - handsAhead / static_cast<float>(heldCount);
            _move = _aheadMeters >= 0 ? Ramp(_aheadMeters, LEAN_AHEAD_DEAD_METERS, LEAN_AHEAD_FULL_METERS)
                                      : Ramp(_aheadMeters, REACH_OUT_DEAD_METERS, REACH_OUT_FULL_METERS);
        }
    }
    if (_isTwistThrottle && _grips[0].isHeld)
    {
        const float twist = Pitch(Gesture::CurrentHand(0).pose) - _grips[0].takenPitchRadians;
        _twistRadians = twist > TWIST_FULL_RADIANS    ? TWIST_FULL_RADIANS
                        : twist < -TWIST_FULL_RADIANS ? -TWIST_FULL_RADIANS
                                                      : twist;
        _move = Ramp(_twistRadians, TWIST_DEAD_RADIANS, TWIST_FULL_RADIANS);
    }
}

// One grip's step: taken by a new grip press within reach, let go with the grip.
void StepGrip(const Bike& bike, int hand)
{
    Grip& grip = _grips[hand];
    Gesture::Hand state = Gesture::CurrentHand(hand);
    if (Vehicle::IsSticksMode())
    {
        state.isGripped = false; // vehicle_controls sticks: the bars stay put, the sticks ride
    }
    const bool hasGripStarted = state.isGripped && !grip.wasGripped;
    grip.wasGripped = state.isGripped;
    if (!grip.isHeld && state.isTracked && hasGripStarted)
    {
        float centre[3];
        GripRest(bike, hand, centre);
        const float* position = state.pose + 12;
        const float dx = position[0] - centre[0], dy = position[1] - centre[1], dz = position[2] - centre[2];
        if (sqrtf(dx * dx + dy * dy + dz * dz) < REACH_METERS)
        {
            // The first grip of a ride (both free before) takes the neck's place as the lean throttle's neutral.
            float neckRight = 0, neckBack = 0;
            if (!_grips[1 - hand].isHeld && GetNeck(&neckRight, &neckBack))
            {
                _neckTakenBackMeters = neckBack;
            }
            grip.isHeld = true;
            grip.takenPitchRadians = Pitch(state.pose);
            grip.takenBackMeters = position[2];
            Feedback::Pulse(hand, 0.02f, 0.4f);
            Log::Info("bike", "%s grip taken", hand == 0 ? "right" : "left");
        }
    }
    else if (grip.isHeld && (!state.isTracked || !state.isGripped))
    {
        grip.isHeld = false;
        Log::Info("bike", "%s grip let go", hand == 0 ? "right" : "left");
    }
}

// The skinned hull's rest pose into a pose table of ours (static: the draw reads the matrices through the table's
// pointers during the call). Returns the table, or null for a rigid hull or if the game's helpers are missing.
void* RestPoseTable(const Bike& bike)
{
    if (!bike.bones || !GameBuild::IsAvailable(GameBuild::Feature::VehicleHands) ||
        !GameBuild::IsAvailable(GameBuild::Feature::Muzzle))
    {
        return nullptr;
    }
    if (_hashedBike != &bike)
    {
        const auto hashName = reinterpret_cast<HashNameFunction>(GameBuild::Absolute(GameBuild::Address::HashName));
        for (int bone = 0; bone < bike.boneCount && bone < MOST_BONES; ++bone)
        {
            _boneHashes[bone] = hashName(bike.bones[bone].name);
        }
        _hashedBike = &bike;
    }
    alignas(16) static unsigned char table[POSE_TABLE_BYTES];
    static float matrices[MOST_BONES][16];
    const auto init = reinterpret_cast<PoseTableInitFunction>(GameBuild::Absolute(GameBuild::Address::PoseTableInit));
    const auto set = reinterpret_cast<PoseTableSetFunction>(GameBuild::Absolute(GameBuild::Address::PoseTableSet));
    init(table);
    for (int bone = 0; bone < bike.boneCount && bone < MOST_BONES; ++bone)
    {
        const float* position = bike.bones[bone].position;
        const float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, position[0], position[1], position[2], 1};
        memcpy(matrices[bone], matrix, sizeof(matrix));
        set(table, _boneHashes[bone], matrices[bone]);
    }
    return table;
}

} // namespace

void OnPresent()
{
    InstallMountHook();
    const Bike* bike = CurrentBike();
    if (bike != _bike)
    {
        _bike = bike;
        _grips[0] = _grips[1] = {};
        _seatTick = GetTickCount();
        _cameraLogsLeft = bike ? 2 : 0;
        Log::Info("bike", "%s", bike ? bike->odfName : "off");
    }
    // The ridden animal drawn by the game in first person (the hook); 0 when not riding one.
    _mountEntity = bike && bike->isMount && _isMountHookOn ? Vehicle::CurrentHullEntity() : 0;
    if (!bike)
    {
        return;
    }
    FpGuard fp;
    StepGrip(*bike, 0);
    StepGrip(*bike, 1);
    StepDrive();
    const DWORD now = GetTickCount();
    if (_isDriveLogOn && now - _lastDriveLogTick >= DRIVE_LOG_MILLISECONDS)
    {
        _lastDriveLogTick = now;
        Log::Info(
            "bike",
            "grips %s %s: lean %.1f cm -> turn %.2f; %s %.1f -> move %.2f",
            _grips[0].isHeld ? "R" : "-",
            _grips[1].isHeld ? "L" : "-",
            static_cast<double>(_leanMeters * 100),
            static_cast<double>(_turn),
            _isTwistThrottle ? "twist deg" : "head ahead of the hands cm",
            static_cast<double>(_isTwistThrottle ? _twistRadians / RADIANS_PER_DEGREE : _aheadMeters * 100),
            static_cast<double>(_move));
    }
}

bool GetDrive(float* move, float* turn)
{
    *move = _bike ? _move : 0.0f;
    *turn = _bike ? _turn : 0.0f;
    return _bike != nullptr;
}

bool GetSeatEye(const float* gameCameraPosition, float* eyeWorld)
{
    const Bike* bike = CurrentBike();
    if (!bike)
    {
        return false;
    }
    FpGuard fp;
    float level[16];
    if (!LevelHull(level))
    {
        return false;
    }
    ToWorld(level, bike->eye, eyeWorld);
    const float dx = gameCameraPosition[0] - eyeWorld[0], dy = gameCameraPosition[1] - eyeWorld[1],
                dz = gameCameraPosition[2] - eyeWorld[2];
    const bool isFirstPerson = dx * dx + dy * dy + dz * dz < FIRST_PERSON_RANGE_METERS * FIRST_PERSON_RANGE_METERS;
    const DWORD now = GetTickCount();
    if (_cameraLogsLeft > 0 && (_cameraLogsLeft == 2 || now - _seatTick >= CAMERA_LOG_DELAY_MILLISECONDS))
    {
        --_cameraLogsLeft;
        // The game camera in hull model space (level): the game's own first-person eye, for the eye constants.
        const float offset[3] = {
            gameCameraPosition[0] - level[12],
            gameCameraPosition[1] - level[13],
            gameCameraPosition[2] - level[14]};
        Log::Info(
            "bike",
            "%s: game camera in the hull at %.2f %.2f %.2f (%s), rider's eye %.2f %.2f %.2f",
            bike->odfName,
            static_cast<double>(offset[0] * level[0] + offset[1] * level[1] + offset[2] * level[2]),
            static_cast<double>(offset[1]),
            static_cast<double>(offset[0] * level[8] + offset[1] * level[9] + offset[2] * level[10]),
            isFirstPerson ? "first person" : "third person",
            static_cast<double>(bike->eye[0]),
            static_cast<double>(bike->eye[1]),
            static_cast<double>(bike->eye[2]));
    }
    return isFirstPerson;
}

bool GetHandPose(int hand, float* restPose)
{
    const Gesture::Hand& state = Gesture::CurrentHand(hand & 1);
    if (!_bike || !state.isTracked)
    {
        return false;
    }
    memcpy(restPose, state.pose, 64);
    if (!_grips[hand & 1].isHeld)
    {
        return true;
    }
    // On the grip: pointing ahead and down by the pitch, then rolled about the pointing line, thumb inward (right
    // hand to the left: negative roll, the right side going up). Rows right, up, back (pitch: up (0, cos, -sin), back
    // (0, sin, cos); roll r: right' = right cos r - up sin r, up' = up cos r + right sin r).
    FpGuard fp;
    const Bike& bike = *_bike;
    // The right hand turns about its grip with the twist-grip throttle (rolled back: its pointing line rises).
    const float pitch = (_testPitchDegrees >= 0 ? _testPitchDegrees : bike.gripPitchDegrees) * RADIANS_PER_DEGREE -
                        (hand & 1 ? 0.0f : _twistRadians);
    const float roll = (hand & 1 ? 1.0f : -1.0f) * (_testRollDegrees >= 0 ? _testRollDegrees : bike.gripRollDegrees) *
                       RADIANS_PER_DEGREE;
    const float pitchSine = sinf(pitch), pitchCosine = cosf(pitch), rollSine = sinf(roll), rollCosine = cosf(roll);
    float centre[3];
    GripRest(bike, hand & 1, centre);
    const float onGrip[16] = {
        rollCosine,
        -pitchCosine * rollSine,
        pitchSine * rollSine,
        0,
        rollSine,
        pitchCosine * rollCosine,
        -pitchSine * rollCosine,
        0,
        0,
        pitchSine,
        pitchCosine,
        0,
        centre[0],
        centre[1],
        centre[2],
        1};
    memcpy(restPose, onGrip, sizeof(onGrip));
    return true;
}

bool IsHolding(int hand)
{
    return _bike && _grips[hand & 1].isHeld;
}

bool GetBarFrame(int hand, float* barWorld, float* shoulderInBar, float* twistRadians)
{
    const Bike* bike = CurrentBike();
    if (!bike || bike->isMount || bike != _bike || !_grips[hand & 1].isHeld) // mounts: no bar measured (reins)
    {
        return false;
    }
    FpGuard fp;
    float level[16];
    if (!LevelHull(level))
    {
        return false;
    }
    const float side = hand & 1 ? -1.0f : 1.0f; // the left grip: the right one mirrored across the hull's middle (x)
    const float centre[3] = {side * bike->barCentre[0], bike->barCentre[1], bike->barCentre[2]};
    const float outward[3] = {side * bike->barOutward[0], bike->barOutward[1], bike->barOutward[2]};
    float origin[3], x[3], y[3], z[3];
    ToWorld(level, centre, origin);
    for (int axis = 0; axis < 3; ++axis)
    {
        x[axis] = outward[0] * level[axis] + outward[1] * level[4 + axis] + outward[2] * level[8 + axis];
        y[axis] = level[4 + axis];
    }
    float length = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    const float along = (x[0] * y[0] + x[1] * y[1] + x[2] * y[2]) / (length * length);
    for (int axis = 0; axis < 3; ++axis)
    {
        x[axis] /= length;
        y[axis] -= along * length * x[axis];
    }
    length = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    for (float& value : y)
    {
        value /= length;
    }
    // z = x cross y is back for the right grip (x the rider's right, y up); for the left the frame is mirrored so that
    // z stays back
    z[0] = side * (x[1] * y[2] - x[2] * y[1]);
    z[1] = side * (x[2] * y[0] - x[0] * y[2]);
    z[2] = side * (x[0] * y[1] - x[1] * y[0]);
    const float frame[16] =
        {x[0], x[1], x[2], 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, origin[0], origin[1], origin[2], 1};
    memcpy(barWorld, frame, sizeof(frame));
    // the rider's shoulder on that side: below, beside and behind the eyes (Aim's SHOULDER_* proportions), the eyes
    // from the head when known, else the rider's resting eye; the hull's axes (its x is the rider's left)
    float eye[3];
    float head[16];
    if (Stereo::GetHeadWorld(head) || Stereo::GetLastHeadWorld(head))
    {
        memcpy(eye, head + 12, sizeof(eye));
    }
    else
    {
        ToWorld(level, bike->eye, eye);
    }
    float shoulder[3];
    for (int axis = 0; axis < 3; ++axis)
    {
        shoulder[axis] = eye[axis] - side * SHOULDER_SIDE_METERS * level[axis] -
                         SHOULDER_BELOW_METERS * level[4 + axis] - SHOULDER_BEHIND_METERS * level[8 + axis];
    }
    const float offset[3] = {shoulder[0] - origin[0], shoulder[1] - origin[1], shoulder[2] - origin[2]};
    shoulderInBar[0] = offset[0] * x[0] + offset[1] * x[1] + offset[2] * x[2];
    shoulderInBar[1] = offset[0] * y[0] + offset[1] * y[1] + offset[2] * y[2];
    shoulderInBar[2] = offset[0] * z[0] + offset[1] * z[1] + offset[2] * z[2];
    *twistRadians = (hand & 1) == 0 && _isTwistThrottle ? _twistRadians : 0.0f;
    return true;
}

bool GetMountEye(float* eye)
{
    // The mounts have no first-person view in the game (no FirstPerson key: the chase camera stays 12 m behind, run644):
    // the view is put at the rider's eye instead (Stereo, where the chase camera sets the camera). FpGuard held.
    const Bike* bike = CurrentBike();
    float level[16];
    if (!bike || !bike->isMount || !LevelHull(level))
    {
        return false;
    }
    ToWorld(level, bike->eye, eye);
    return true;
}

bool DrawHull(unsigned flags)
{
    // the seat's bike from Vehicle at once (the seat changes during Update, OnPresent follows after the frame): the
    // first frame in the seat shows the hull, not the game's stretched first-person model (hands spotty on entry)
    const Bike* bike = CurrentBike();
    if (bike && bike->isMount)
    {
        // The game draws the animal itself (WalkerVisibleHook): nothing to draw, but the stock first-person model is
        // left out. Without the hook (or before it has the mount) the stock model stays.
        return _isMountHookOn && _mountEntity != 0;
    }
    if (!bike || !GameBuild::IsAvailable(GameBuild::Feature::Holsters))
    {
        return false;
    }
    void* model = Holster::LoadedModel(bike->hullName);
    if (!model)
    {
        if (_missingModel != bike->hullName)
        {
            _missingModel = bike->hullName;
            Log::Warning("bike", "model %s not loaded: the game's first-person model stays", bike->hullName);
        }
        return false;
    }
    float level[16];
    {
        FpGuard fp;
        if (!LevelHull(level))
        {
            return false;
        }
    }
    void* bones = RestPoseTable(*bike);
    void* light = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::ModelLight));
    const auto draw = (*reinterpret_cast<ModelDrawFunction**>(model))[1];
    draw(model, nullptr, level, bones, light, flags | MODEL_DRAW_FLAGS, 0);
    return true;
}

void SetDriveLog(bool isLogOn)
{
    _isDriveLogOn = isLogOn;
}

void SetTwistThrottle(bool isTwist)
{
    _isTwistThrottle = isTwist;
    Log::Info("bike", "throttle: %s", isTwist ? "right grip twist" : "lean (head ahead of the hands)");
}

void SetGripTest(float rollDegrees, float pitchDegrees)
{
    _testRollDegrees = rollDegrees;
    _testPitchDegrees = pitchDegrees;
    Log::Info(
        "bike",
        "test: hands on the grips rolled %.0f, pitched %.0f deg (-1 = the table's)",
        static_cast<double>(rollDegrees),
        static_cast<double>(pitchDegrees));
}

} // namespace Handlebars

} // namespace SwbfVr
