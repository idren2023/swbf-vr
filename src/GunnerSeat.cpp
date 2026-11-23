// GunnerSeat.cpp: our gunner's station on the stock fighter tanks in place of their turret (see GunnerSeat.h).
//
// How the game draws the hull (static reading of EntityHover's render 0xFBE60, PLAN 56): it fills the hull's pose
// table (EntityHover +0x1B34: bone hash -> model-space bone matrix) from its animation, lets the aimers (0x8CF30) and
// the mounted turrets (0x1964A0) write their nodes into it, then draws the hull model (+0x1F0, vtable slot 0, at
// 0xFC5C5 / 0xFC5FA) with that table; the model's draw reaches the RedModel draw (0x288680, hooked by Body) with the
// table as its bone set (expected; the "hull draw" log line says whether it was seen). The hull is one skinned mesh
// whose every vertex hangs on one bone (rep_hover_fightertank.msh), so pointing the three turret bones at a matrix
// with no rotation part collapses the 440 turret vertices into one point inside the hull: the stock turret is not
// drawn, and the rest of the hull is untouched. The entries are restored right after the draw, so anything else that
// reads the table (the barrels' fire points) sees the game's own matrices.

#include "GunnerSeat.h"

#include "Aim.h"
#include "Body.h"
#include "CockpitShell.h"
#include "D3dSurvey.h"
#include "Feedback.h"
#include "FpGuard.h"
#include "GameBuild.h"
#include "Gesture.h"
#include "Holster.h"
#include "Log.h"
#include "ShellDesigns.h"
#include "Stereo.h"
#include "Vehicle.h"
#include "Xr.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace SwbfVr
{

namespace GunnerSeat
{

namespace
{

/// Parts of a station, in the order of its bones (ShellDesigns.h: base on the yaw bone, cradle on the pitch bone,
/// barrel).
constexpr int PART_COUNT = 3;

/// A gunner seat with a station design: the stock vehicle, its design, the bones whose rotations turn its parts (part
/// order), each part's pivot at rest in the hull model's space (the bone's msh rest place, or another point the part
/// turns about; every rest rotation is the identity), the gunner's eye
/// in that space, the right grip on the yaw part (hand place in the hull model's space, the left one mirrored in x;
/// its top's lean ahead), and whether the stock turret is hidden (then its vertices collapse onto a point inside the
/// hull, hull model space).
struct Station
{
    const char* odfName;
    const char* designName;
    const char* boneNames[PART_COUNT];
    float rest[PART_COUNT][3];
    const float* eye;
    const float* grip;
    float gripLeanDegrees;
    bool hidesStockTurret;
    float collapsedPlace[3];
};

const Station STATIONS[] = {
    {"rep_hover_fightertank",
     "rep_hover_fightertank_gunner",
     {"turret_y", "turret_x", "turret_barrel1"},
     {{0.0f, 1.793f, -2.858f}, {0.0f, 2.069f, -2.318f}, {0.0f, 2.036f, -1.842f}},
     ShellDesigns::FIGHTER_TANK_GUNNER_EYE,
     ShellDesigns::FIGHTER_TANK_GUNNER_GRIP,
     ShellDesigns::FIGHTER_TANK_GUNNER_GRIP_LEAN_DEGREES,
     true,
     {0.0f, 1.4f, -2.858f}},
    {"imp_hover_fightertank",
     "imp_hover_fightertank_gunner",
     {"turret_y", "turret_x", "turret_barrel1"},
     {{0.0f, 1.793f, -2.858f}, {0.0f, 2.069f, -2.318f}, {0.0f, 2.036f, -1.842f}},
     ShellDesigns::FIGHTER_TANK_GUNNER_EYE,
     ShellDesigns::FIGHTER_TANK_GUNNER_GRIP,
     ShellDesigns::FIGHTER_TANK_GUNNER_GRIP_LEAN_DEGREES,
     true,
     {0.0f, 1.4f, -2.858f}},
};

/// EntityHover: the hull's pose table (offsets "Nodes"; rebuilt by every render of the hull).
constexpr uintptr_t OFFSET_hoverPoseTable = 0x1B34;
/// Pose table (lookup 0x8C230 -> 0x300890): an entry count, then 128 hash slots from +4 and their matrix pointers 128
/// slots after them (+0x204); open addressing from slot hash & 127, probing down and wrapping, an empty slot (hash 0)
/// ends the search.
constexpr uintptr_t OFFSET_poseHashes = 0x4;
constexpr int POSE_SLOTS = 128;
/// Most slots changed in one draw (each bone once; a hash set twice would have two slots).
constexpr int MOST_CHANGED = 8;
/// The game camera counts as first person within this of the gunner's eye (in a fighter tank's gunner seat it sits
/// at the turret plus EyePointOffset (0, 0.5, 3), about 3.1 m from the eye; the chase camera is further; inferred).
constexpr float FIRST_PERSON_RANGE_METERS = 6.0f;
/// A building turret's view, once first person, turns third person only past this (GetSeatEye; to be checked against
/// the game's third-person camera, which sits further behind).
constexpr float BUILDING_THIRD_PERSON_METERS = 11.0f;
/// The hull draw log: this long after a seat change it says what was seen.
constexpr DWORD HULL_LOG_DELAY_MILLISECONDS = 1500;
/// A bone matrix row counts as a unit vector within this.
constexpr float UNIT_TOLERANCE = 0.05f;
/// A grip press takes a grip when the hand is this close to it (as the tank levers' and the handlebars').
constexpr float REACH_METERS = 0.20f;
constexpr float RADIANS_PER_DEGREE = 0.01745329f;

using HashNameFunction = unsigned(__cdecl*)(const char* name);

struct ChangedSlot
{
    float** slot;
    float* original;
};

const Station* _station = nullptr;
uintptr_t _poseTable = 0;
unsigned _hashes[PART_COUNT] = {};
unsigned _frame = 0;        // presented frames
unsigned _captureFrame = 0; // the frame of the last hull draw seen (its bones kept)
unsigned _drawnFrame = 0;   // the frame our station was last drawn (first person)
bool _hasCapture = false, _hasDrawn = false;
float _bones[PART_COUNT][16];
int _boneFoundMask = 0;
float _hullDrawMatrix[16];
ChangedSlot _changed[MOST_CHANGED];
int _changedCount = 0;
int _hullDrawCount = 0, _hullDrawsThisSecond = 0;
DWORD _seatTick = 0;
bool _isHullLogPending = false;
volatile bool _isTurretOn = true;

/// One grip of the station (index 0 right, 1 left, as the hands).
struct Grip
{
    bool isHeld;
    bool wasGripped; // the hand's grip in the frame before: a grip is taken by a new press only
};

Grip _grips[2] = {};
// Each grip's pose in resting-pose space (rows right, up, back, place: the hand as on a pistol grip, pointing ahead
// across it, the place on the grip's axis), as the station was last drawn; the grips turn and tilt with the turret and
// the hull.
float _gripPoses[2][16];
bool _hasGripPoses = false;
// The collapsing matrix: no rotation part, so every vertex on the bone lands on its translation, the station's
// collapsed place (set at a seat change; read only by the hull's draw).
float _collapsed[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

/// A gunner seat inside a cabin shell (PLAN 56: the combat speeder's gunner sits in the raised rear seat of its cabin,
/// idren2023 2026-10-17; PLAN 57 k: the AAT's gunner in a cabin of his own): the vehicle, its shell (ShellDesigns' design
/// of that name, x right, y up, z back), the game's first-person model drawn with it (the speeder's pilot cabin; null:
/// none), whether the turning seat has end stops (the turret's ODF YawLimits; the AAT's turns all round), the gunner's
/// eye in the hull model's space (where the vehicle's own gunner sits) and in the shell's space (the shell is placed so
/// that the two meet, on the hull's level heading), the right grip's hand place in the shell's space, the x the left
/// grip mirrors about, the grips' lean, and how far the game camera may be from the gunner's eye in first person.
struct Cabin
{
    const char* odfName;
    const char* designName;
    const char* cockpitModelName;
    bool hasEndStops;
    float gunnerEyeInHull[3];
    float gunnerEye[3];
    float grip[3];
    float gripMirrorX;
    float gripLeanDegrees;
    float firstPersonRangeMeters;
    bool isPeriscopeByHead; // the display pitches with the head within its own limits (PLAN 58 d), not the gun
    bool isOnFlyer = false; // a flyer's seat: the cabin takes the hull's whole attitude (and the view, Stereo), not
                            // only its heading
    float aimDownDegrees = 0; // the gun aims this far below the head's line (Vehicle StepTurret), the middle of its pitch
                              // limits, so a level head is mid-range; the display is raised back by as much
    float periscopeFieldFactor = 1.0f; // the display's picture this much wider than the glass's angle (Periscope)
    float turretAhead = NAN; // a vehicle with more than one turret (the AT-TE): the turret whose origin is nearest this
                             // far ahead of the hull's origin (metres, along its heading) takes this cabin
    float turretRight = NAN; // with turretAhead: the turret's origin this far right of the hull's (the gunship's balls)
    bool isFacingAft = false;
    bool isViewHeld = false; // the view keeps its own heading while the vehicle turns under it, the turret follows the
                             // head (as the fighter tanks' gunner station; Controls, Stereo) // a flyer's seat whose turret works aft (the snowspeeder's tail gunner): the resting view
                              // faces the hull's back (Stereo), so a head looking ahead looks along the guns
    bool isPlainSeat = false; // no display (headset run 19, idren2023): the gunner looks out of the cab's own window or
                              // canopy, head aim and the aiming line
    bool isChairFixed = false; // with isPlainSeat: a fixed co-pilot chair, its grips stay with the cab (else they turn
                               // with the swivel chair)
    float pitchPivot[3] = {NAN, NAN, NAN}; // the pitching part (2) turns about this (shell space), not the eye
    float muzzle[3] = {NAN, NAN, NAN}; // the drawn barrel's muzzle (shell space, part 2): the aiming line starts here
    float yawMiddleDegrees = NAN; // a seat told apart by its turret's yaw limits: taken (before the turretAhead rows)
                                  // only when the limits' middle is this far to either side (the gunship's door
                                  // passenger, -95..-60, whose turret reads at the hull's origin as the co-pilot's)
    float viewscreen[4] = {NAN, NAN, NAN, NAN}; // the periscope's screen on the turning part (shell space centre, then
                                                // its half width), not on the rails of the pitching one (the AT-TE
                                                // cannon's console: its pitching part turns about the trunnion)
    // A sight on rails of its own size (else the speeder periscope's): its distance ahead of the eye and half width
    // (metres), and its highest pitch over the level (degrees): the Y-wing's canopy is 20 cm over the gunner's eyes
    // (idren2023 2026-11-09: the station cut through the canon canopy).
    float sightDistance = NAN;
    float sightHalfWidth = NAN;
    float sightHighDegrees = NAN;
};

const Cabin CABINS[] = {
    // all_hover_combatspeeder (hull all_hover_rebel_attack_speeder): the gunner's eye at hp_gunner (0, 0.318, -2.605)
    // moved as the game's pilot camera (ODF EyePointOffset (0, 1, -2.2)) is from hp_pilot (0, 0.316, -1.547): on the
    // hull's middle line, the nose gun straight ahead (b262; b260 put the shell's pilot eye on the pilot camera, which
    // left the gunner 0.58 m right of the hull's middle and 0.4 m further back: the shell is the first-person model's
    // 1.5 times wider cabin). The shell's gunner seat on its middle line (x 0.58; cushion top 1.02, back 3.12: the eye
    // 0.80 above the cushion and 0.19 before its back, as the pilot's) and the yoke's grips (0.58 +-0.2, 1.34..1.46, z
    // 2.30..2.28: the hand 0.6 of the way up); the game camera of the gunner seat sits at the nose turret plus
    // (0, 2, 4), about 7 m from the gunner's eye.
    {"all_hover_combatspeeder",
     "all_hover_combatspeeder",
     "all_1st_cockpit_combatspeeder",
     true,
     {0.0f, 1.002f, -3.258f},
     {0.58f, 1.82f, 2.93f},
     {0.85f, 1.32f, 2.66f}, // the right armrest's grip (ShellDesigns ARMREST_GRIP_*, idren2023 2026-10-19)
     0.58f,
     9.5f,
     12.0f,
     true},
    // cis_hover_aat (PLAN 57 k, idren2023 2026-10-19: a cabin like the speeder gunner's, the seat turning all the way
    // round, no window, no drawn barrel, a hatch overhead; then: in the tank's main cabin over the rear hatchway, as
    // the DK cross-section): the commander's eye on the turret's yaw axis through Gun_body (0, 3.621, -2.709), 0.13 m
    // above it; the AAT's pilot cabin shell and the game's AAT cockpit model around it, the commander's deck and yoke
    // in it (ShellDesigns AAT_CABIN_*); the seat's game camera sits at the turret plus (0, 2, -1.75).
    {"cis_hover_aat",
     "cis_hover_aat",
     "cis_1st_cockpit_AAT",
     false,
     {0.0f, 3.75f, -2.709f},
     {ShellDesigns::AAT_CABIN_EYE[0], ShellDesigns::AAT_CABIN_EYE[1], ShellDesigns::AAT_CABIN_EYE[2]},
     {ShellDesigns::AAT_CABIN_GRIP[0], ShellDesigns::AAT_CABIN_GRIP[1], ShellDesigns::AAT_CABIN_GRIP[2]},
     ShellDesigns::AAT_CABIN_EYE[0],
     ShellDesigns::AAT_CABIN_GRIP_LEAN_DEGREES,
     8.0f,
     false},
    // imp_fly_tiebomber (idren2023 2026-10-27): the bombardier (ODF TURRET1 "bomber", YawLimits -25..25) in a turning
    // chair in the pilot pod's tube behind the pilot's ball (ShellDesigns TieBomberStation, design
    // "imp_fly_tiebomber"); the eye in the hull model's space as the shell is placed in the pilot seat (stockexterior.py:
    // the pilot's eye at (-1.75, 2.74, 1.45), the shell's x and z turned round); the bomb sight on rails ahead (the
    // periscope along the bombs' line). The seat's game camera (EyePointOffset 0 3.3 4.0) is a few metres off.
    // all_fly_ywing (headset run 18, idren2023): the gunner (ODF TURRET1, the dorsal ion cannon, all round, barrels
    // -10..90) in a turning chair behind the pilot's bulkhead (ShellDesigns YwingGunnerStation, design "all_fly_ywing");
    // the eye in the hull model's space as the shell is placed in the pilot seat (stockexterior.py: the pilot's eye at
    // (0, 2.10, 5.15)); the display on rails with the gun, a level head aiming it 35 degrees up.
    {"all_fly_ywing",
     "all_fly_ywing",
     nullptr,
     false,
     {0.0f, 2.15f, 6.65f - ShellDesigns::YWING_GUNNER_EYE[2]}, // hull z = 6.65 - shell z (stockexterior's turn)
     {ShellDesigns::YWING_GUNNER_EYE[0], ShellDesigns::YWING_GUNNER_EYE[1], ShellDesigns::YWING_GUNNER_EYE[2]},
     {ShellDesigns::YWING_GUNNER_GRIP[0], ShellDesigns::YWING_GUNNER_GRIP[1], ShellDesigns::YWING_GUNNER_GRIP[2]},
     ShellDesigns::YWING_GUNNER_EYE[0],
     ShellDesigns::YWING_GUNNER_GRIP_LEAN_DEGREES,
     12.0f,
     false,
     true,
     -ShellDesigns::YWING_GUNNER_AIM_UP_DEGREES,
     1.4f, // a wider picture, as the bomb sight's
     NAN,
     NAN,
     false,
     false,
     false,
     false,
     {NAN, NAN, NAN},
     {NAN, NAN, NAN},
     NAN,
     {NAN, NAN, NAN, NAN},
     ShellDesigns::YWING_GUNNER_SIGHT_DISTANCE,
     ShellDesigns::YWING_GUNNER_SIGHT_HALF_WIDTH,
     ShellDesigns::YWING_GUNNER_SIGHT_HIGH_DEGREES},
    {"imp_fly_tiebomber",
     "imp_fly_tiebomber",
     nullptr,
     true,
     {-1.75f, 2.69f, -1.45f},
     {ShellDesigns::TIE_BOMBER_GUNNER_EYE[0], ShellDesigns::TIE_BOMBER_GUNNER_EYE[1], ShellDesigns::TIE_BOMBER_GUNNER_EYE[2]},
     {ShellDesigns::TIE_BOMBER_GUNNER_GRIP[0], ShellDesigns::TIE_BOMBER_GUNNER_GRIP[1], ShellDesigns::TIE_BOMBER_GUNNER_GRIP[2]},
     ShellDesigns::TIE_BOMBER_GUNNER_EYE[0],
     ShellDesigns::TIE_BOMBER_GUNNER_GRIP_LEAN_DEGREES,
     12.0f,
     false,
     true,
     15.0f, // idren2023 2026-10-27: the level head at the middle of the bomber's pitch limits (-10..40, down positive)
     1.8f}, // headset run 18: the bomb sight's view wider (idren2023: "a larger FOV would be nice")    // imp_walk_atst (the vehicle proposal 2026-11-04; idren2023 2026-11-11: the co-pilot on the pilot's right, his area
    // the pilot's mirrored): ODF TURRET1 (YawLimits -50..50, EyePointOffset 0 0 0) in the cab's right seat
    // (ShellDesigns ATST_GUNNER_X 2.60), his eye placed where the pilot's is (the hull's (0, 8, 0), ODF
    // EyePointOffset) so his aim matches the game's; the cab is the design's whole (no stock model); his twin grips.
    // cis_fly_maf (the vehicle proposal 2026-11-04): TURRET1 (the top gun) and TURRET2 (the rear cannon, its node
    // turret_rearcannon) on swivel stations behind the pilot (ShellDesigns BuildMafSeat: designs cis_fly_maf_top /
    // _rear); told apart by where their turrets sit (first guesses: the top one ahead, the rear one behind; run log).
    // 2026-11-07: the rear station moved off the stock window wall (ShellDesigns MAF_REAR_EYE).
    {"cis_fly_maf", "cis_fly_maf_top", "cis_1st_cockpit_MAF", true, {0.0f, 5.0f, 6.0f}, {2.10f, 1.5f, 0.60f},
     {2.30f, 1.20f, 0.33f}, 2.10f, 10.0f, 40.0f, false, true, 0.0f, 1.0f, -1000.0f}, // no node (run614)
    {"cis_fly_maf", "cis_fly_maf_rear", "cis_1st_cockpit_MAF", true, {0.0f, 5.0f, 6.0f}, {0.70f, 1.5f, 1.20f},
     {0.90f, 1.20f, 0.93f}, 0.70f, 10.0f, 40.0f, false, true, 0.0f, 1.0f, -5.44f}, // run614: 5.44 behind
    // rep_fly_gunship (the vehicle proposal 2026-11-04): the co-pilot (TURRET1, the top gun, YawLimits -89..89,
    // PitchLimits -20..50) on a swivel station in the nose's lower bubble (ShellDesigns GunshipCopilotStation); the
    // ball turrets (TURRET2 / 3: ball_turret_2 at the hull's x -2.8, ball_turret_1 at 2.93, both 1.6 ahead) each in
    // their glass ball with the ship round it (designs rep_fly_gunship_ball2 / 3). Cameras first guesses (run log).
    {"rep_fly_gunship", "rep_fly_gunship", nullptr, true, {0.0f, 2.5f, 7.5f}, {0.0f, 0.42f, -1.55f},
     {0.20f, 0.12f, -1.82f}, 0.0f, 10.0f, 40.0f, false, true, -15.0f, 1.0f, -1000.0f, NAN, false, false, true,
     false}, // no node (run605); headset run 19: no display (aiming line)
    // Headset run 19: the balls were swapped (in the port ball one aimed the starboard gun): the turret whose origin
    // reads +2.80 on the hull's x axis (the model's +x, the shooter's left) sits in the port ball, ball_turret_1 of
    // the msh (x 2.93, design ball3); the one at -2.93 in the starboard ball (ball2).
    // (2026-11-07: each ball's anchor is now the eye its exterior table was made at, stockexterior.py: ball3 the port
    // ball's (2.93), ball2 the starboard's (-2.80); since the swap they were crossed, which put the drawn ship 5.7 m to
    // the side of the real one; inferred from the code, not yet seen)
    {"rep_fly_gunship", "rep_fly_gunship_ball3", nullptr, true, {2.93f, 1.06f, 2.45f}, {0.0f, 1.5f, 1.5f},
     {0.20f, 1.28f, 1.20f}, 0.0f, 10.0f, 12.0f, false, true, 0.0f, 1.0f, 4.17f, 2.80f},
    {"rep_fly_gunship", "rep_fly_gunship_ball2", nullptr, true, {-2.80f, 1.06f, 2.45f}, {0.0f, 1.5f, 1.5f},
     {0.20f, 1.28f, 1.20f}, 0.0f, 10.0f, 12.0f, false, true, 0.0f, 1.0f, 4.17f, -2.93f},
    // The door passenger (TURRET4: YawLimits -95..-60, no weapon, PilotPosition hp_passenger1, "standing"; the vehicle
    // proposal 2026-11-04): standing in the troop bay beside the starboard door, facing ahead, the ship's own bay round
    // him (design rep_fly_gunship_door; ShellDesigns GUNSHIP_DOOR_GRIP: hand straps ahead of his face); his eye 1.65 m
    // over the floor at hp_passenger1. Stock has no first person in this seat (no FirstPerson key): this shows only if
    // the game gives the seat a first-person pass (first thing to see in a run: "gunner: cabin seat", "fp: gunner seat").
    // The game's camera (EyePointOffset 0 10 -3) is about 10 m off: range 40 as the co-pilot's.
    // 2026-11-08 (idren2023: the eye "way off"): hp_passenger1 from the shipped skeleton (Shipped Common Animations/
    // rep_fly_gunship/basepose.msh): DummyRoot (0, 2.649, -2.586) + (1.285, -2.548, 2.400) = (1.285, 0.10, -0.19) in
    // model space (x left), facing 79 deg to the left (the port door). Run661 measured his soldier in the game's hull
    // frame (whose origin is not the model's root): (-0.97, -2.55, 2.40), the eye 1.62 m over that. Was (-0.97, 1.75,
    // -0.19): 3.4 m from him.
    {"rep_fly_gunship", "rep_fly_gunship_door", nullptr, false, {-0.97f, -0.93f, 2.40f}, {0.0f, 1.5f, 1.5f},
     {0.22f, 1.45f, 1.20f}, 0.0f, 0.0f, 40.0f, false, true, 0.0f, 1.0f, NAN, NAN, false, false, true, true,
     {NAN, NAN, NAN}, {NAN, NAN, NAN}, 77.5f},
    // all_fly_snowspeeder (the vehicle proposal 2026-11-04): the tail gunner (FLYERSECTION TURRET, YawLimits 80..280,
    // PitchLimits -15..25) back to back with the pilot on a swivel station (ShellDesigns SnowGunnerStation): his eye
    // at hp_gunner (0, 0.38, -0.89) and his head; the turret camera (EyePointOffset 0 2 4) a few metres off.
    {"all_fly_snowspeeder", "all_fly_snowspeeder", nullptr, true, {0.0f, 1.23f, -0.95f}, {0.0f, 1.58f, 2.70f},
     {0.20f, 1.28f, 2.43f}, 0.0f, 10.0f, 12.0f, false, true, 5.0f, 1.0f, NAN, NAN, true, false,
     true, false}, // run602: faced ahead (b542: isFacingAft had landed in turretRight); headset run 19: no display
    // imp_walk_atat: ODF TURRET1 (aimer_head, YawLimits -65..65) on a swivel chair with a display on rails in the cab's
    // right seat (idren2023 2026-11-04; ShellDesigns AtatGunnerStation, ATAT_GUNNER_X), the cab the design's whole (the stock
    // pilot model mirrored, 2026-11-11), his twin grips on the chair's column; his eye
    // placed as the pilot's camera (EyePointOffset 1 16 9); the turret's own camera is 9 m ahead of its pivot and swings
    // with it (run600: up 14.6..15.7, ahead 18), hence the 15 m range.
    // rep_walk_atte (the vehicle proposal 2026-11-04): two turrets, told apart by where their origins sit (turretAhead):
    // TURRET1 the main cannon (aimer_maincannon_y, its camera (0, 2, 5) from it), its gunner on the raised seat at the
    // compartment's front looking over the pod (ShellDesigns ATTE_CANNON_EYE); TURRET2 the rear guns (YawLimits
    // 120..240), its gunner facing the back wall's window (ATTE_REAR_EYE). The stock pilot pod drawn as in the pilot
    // seat. First guesses for the cameras and the turrets' places (run log "fp: gunner seat").
    // idren2023 2026-11-04: TURRET1 is the open-air cannon on its back: the walker's exterior round its gunner at
    // hp_main_gunner, the turret's own seat, visor and cannon turned with it (ShellDesigns BuildAtteCannon), the view
    // held as in the fighter tanks' gunner station.
    {"rep_walk_atte", "rep_walk_atte_cannon", nullptr, false, {0.0f, 8.60f, 1.72f}, {0.0f, 1.5f, 1.5f},
     {0.20f, 1.28f, 1.20f}, 0.0f, 10.0f, 15.0f, false, false, 0.0f, 1.0f, 3.6f, NAN, false, true, false, false,
     {ShellDesigns::ATTE_CANNON_TRUNNION[0], ShellDesigns::ATTE_CANNON_TRUNNION[1],
      ShellDesigns::ATTE_CANNON_TRUNNION[2]},
     {ShellDesigns::ATTE_CANNON_MUZZLE[0], ShellDesigns::ATTE_CANNON_MUZZLE[1], ShellDesigns::ATTE_CANNON_MUZZLE[2]},
     NAN,
     {ShellDesigns::ATTE_CANNON_VIEWSCREEN[0], ShellDesigns::ATTE_CANNON_VIEWSCREEN[1],
      ShellDesigns::ATTE_CANNON_VIEWSCREEN[2], ShellDesigns::ATTE_CANNON_VIEWSCREEN[3]}},
    // run601: 3.6 ahead; headset run 19: the barrel about its trunnion (aimer_maincannon_x 0 7.40 4.34), its muzzle
    // hp_fire_maincannon; 2026-11-07: our station's console with the gun camera's viewscreen
    // run611: his view faced ahead (the turret's forward reads as the hull's): faces aft. idren2023 2026-11-08: his
    // station in the middle of the troop bay's back end (ShellDesigns BuildAtteBay: no stock pod), the seat and the
    // gun camera's viewscreen turning with the guns, the troopers behind him.
    {"rep_walk_atte", "rep_walk_atte_rear", nullptr, true, {0.0f, 8.0f, -4.0f}, {0.0f, 1.45f, 3.55f},
     {-0.20f, 0.86f, 3.825f}, 0.0f, 10.0f, 40.0f, false, false, 0.0f, 1.0f, -1000.0f, NAN, true, false, false, false,
     {NAN, NAN, NAN}, {NAN, NAN, NAN}, NAN,
     {ShellDesigns::ATTE_BAY_VIEWSCREEN[0], ShellDesigns::ATTE_BAY_VIEWSCREEN[1], ShellDesigns::ATTE_BAY_VIEWSCREEN[2],
      ShellDesigns::ATTE_BAY_VIEWSCREEN[3]}}, // no node (run601)
    {"imp_walk_atat", "imp_walk_atat", nullptr, true, {1.0f, 16.0f, 9.0f}, {1.92f, 1.45f, -0.20f},
     {2.12f, 0.90f, -0.50f}, 1.92f, 10.0f, 15.0f, false, false,
     15.0f, 1.0f, NAN, NAN, false, false, true, true}, // the gun's pitch -25..-5 (down positive 5..25): a level head at its
                                                 // middle; headset run 19: a plain co-pilot chair
    {"imp_walk_atst", "imp_walk_atst", nullptr, true, {0.0f, 8.0f, 0.0f}, {2.60f, 1.42f, 0.25f},
     {2.80f, 0.84f, 0.02f}, 2.60f, 18.0f, 8.0f, false, false, 0.0f, 1.0f, NAN, NAN, false, false, true, true},
    {"imp_walk_atst_snow", "imp_walk_atst", nullptr, true, {0.0f, 8.0f, 0.0f}, {2.60f, 1.42f, 0.25f},
     {2.80f, 0.84f, 0.02f}, 2.60f, 18.0f, 8.0f, false, false, 0.0f, 1.0f, NAN, NAN, false, false, true, true},
    {"imp_walk_atst_jungle", "imp_walk_atst", nullptr, true, {0.0f, 8.0f, 0.0f}, {2.60f, 1.42f, 0.25f},
     {2.80f, 0.84f, 0.02f}, 2.60f, 18.0f, 8.0f, false, false, 0.0f, 1.0f, NAN, NAN, false, false, true, true},
};

/// Model draw flags added to the first-person pass's (as Handlebars' and Holster's draws).
constexpr unsigned MODEL_DRAW_FLAGS = 0x80001;
/// A model's draw, vtable slot 1: world matrix, bone set (null: rigid), light, flags, 0 (offsets "RedModel").
using ModelDrawFunction = void(
    __fastcall*)(void* model, void* edx, const float* matrix, void* bones, void* light, unsigned flags, int unused);

const Cabin* _cabin = nullptr;

/// A building turret drawn as its own third-person model (PLAN 58 u, idren2023 2026-10-20; docs/research/turrets.md): the
/// game leaves the building out of the first-person view while its seat is taken (runs 399-401: no draw of its model
/// at all), so in the first-person pass the mod draws the model with a pose table of its own: every part above the
/// base turned by the turret's live yaw about the yaw part's pivot, the gun's parts also by its pitch about the gun's
/// pivot (the head aims both through Vehicle's turret loop). Each node: name, 1 turns with the yaw, 2 also pitches;
/// its rest place in the model (rows x, y, z, translation; from the msh, tools/mshgeo.py).
struct TurretNode
{
    const char* name;
    int kind;
    float rest[12];
};
struct BuildingTurret
{
    const char* odfName;
    const char* modelName;
    const TurretNode* nodes;
    int nodeCount;
    float yawPivot[3], pitchPivot[3];
    float eye[3];  // the gunner's eye in the model at rest (turns with the yaw)
    float grip[3]; // the right hand's grip on the gun, model space at rest (pitches with the gun; the left its mirror)
    float gripLeanDegrees;
    // +1 when the gun points along the model's +z, -1 along -z (its nodes then carry a half turn about y, so the game's
    // pitch about the node's x turns the other way in model space; the yaw is the same).
    float forwardZ;
    bool hasGrips;     // false: the hands stay free (nothing on the turret to hold)
    float gripCentreX; // the grips' mirror plane x (model space)
    // A viewscreen (PLAN 58 u, idren2023: the dish and the Geonosis turret show the gun's view): ShellDesigns' design of
    // that name, a picture on the model's own screen face (model centre, half width; on the gun's pitching part or
    // the yaw part), fed by the scope pass from a camera on the gun (model point, pitching with the gun). nullptr:
    // none.
    const char* screenDesign;
    bool isScreenOnGun;
    float screen[3];
    float screenHalfWidth;
    float camera[3];
    // The gunner sits on the gun's pitching part (the Bespin AAA's cradle): the eye moves with the pitch too (the view
    // stays level; only its place follows the seat).
    bool isEyeOnGun = false;
    // The grips sit on the turning part, not on the gun (the Hoth tower's control pedestal).
    bool isGripOnTurning = false;
    // Aimed by the right hand, not the head (IsAimedByHand).
    bool isAimedByHand = false;
    // A sliding viewscreen in place of the face (idren2023 2026-10-24: the dish and Geonosis back to head aim, the screen
    // never over the hands): `screen` is its centre ahead of the eye at pitch 0, and it rides a rail about the eye with
    // the head's pitch over the turning part's level, within these limits (degrees, up positive; both 0: on the face).
    float screenLowDegrees = 0, screenHighDegrees = 0;
    // The hands sit on the grips by themselves while the seat is taken (headset run 19, idren2023: the Bespin AAA).
    bool areGripsAutoHeld = false;
};
/// com_weap_gunturret: main_cylinder (pivot 0 0.6 0) turns, turret_main (pivot 0 1.596 0.72) pitches. The gunner
/// stands on the floor (hp_active 0 0.262 -0.152): the eye 1.6 m above it, just behind the gun's scope.
const TurretNode GUN_TURRET_NODES[] = {
    {"main_cylinder", 1, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.6000f, 0.0000f}},
    {"body_trimR4", 1, {0.2586f, 0.0000f, -0.9660f, 0.0000f, 1.0000f, 0.0000f, 0.9660f, 0.0000f, 0.2586f, 0.0000f, 0.7625f, 0.0000f}},
    {"body_trimR1", 1, {0.2586f, 0.0000f, 0.9660f, 0.0000f, 1.0000f, 0.0000f, -0.9660f, 0.0000f, 0.2586f, 0.0000f, 0.7625f, 0.0000f}},
    {"body_trimR2", 1, {-0.7071f, 0.0000f, 0.7071f, 0.0000f, 1.0000f, 0.0000f, -0.7071f, 0.0000f, -0.7071f, 0.0000f, 0.7625f, 0.0000f}},
    {"body_trimR3", 1, {-0.7071f, 0.0000f, -0.7071f, 0.0000f, 1.0000f, 0.0000f, 0.7071f, 0.0000f, -0.7071f, 0.0000f, 0.7625f, 0.0000f}},
    {"floor", 1, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"turret_base", 1, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.3546f, 0.7137f}},
    {"turret_main", 2, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0001f, 1.5961f, 0.7195f}},
    {"gunnozzle", 2, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0001f, 1.5961f, 0.6355f}},
    {"telshaft", 2, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0001f, 1.5961f, 0.6355f}},
    {"scope", 2, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0001f, 1.5961f, 0.6355f}},
    {"tube1", 2, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0001f, 1.5961f, 0.6355f}},
    {"tube2", 2, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0001f, 1.5961f, 0.6355f}},
    {"tube3", 2, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0001f, 1.5961f, 0.6355f}},
    {"tube4", 2, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0001f, 1.5961f, 0.6355f}},
    {"cylinderhead2", 2, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0001f, 1.5961f, 0.6355f}},
    {"gun_mount", 1, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 1.2500f, 1.2079f}},
    {"trim", 1, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.6000f, 0.0000f}},
    {"body_trimR", 1, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.7625f, 0.0000f}},
    {"guard", 1, {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 1.3016f, 1.1773f}},
};
/// tat_bldg_barge_turret (Mos Eisley's barge rail guns): aimer_01 (pivot 0 0 0) turns +-70 deg, turret_01 (pivot 0 0.30
/// 0.02) pitches; the gun points along -z.
const TurretNode BARGE_TURRET_NODES[] = {
    {"aimer_01",
     1,
     {-1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, -1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"gunfloater",
     1,
     {-1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, -1.0000f, -0.0012f, 0.2215f, 0.0249f}},
    {"turret_01",
     2,
     {-1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, -1.0000f, 0.0000f, 0.3000f, 0.0231f}},
};
/// hoth_bldg_dish_turret: turret_y (pivot 0 0.50 0.54) turns, turret_x (pivot 0 1.11 0.72) pitches -10..25 deg with the
/// dish; the gunner stands behind the column, the dish in front of the eye.
const TurretNode DISH_TURRET_NODES[] = {
    {"turret_y",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4984f, 0.5358f}},
    {"turret_x",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 1.1131f, 0.7195f}},
    {"Dish",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -0.0186f, 0.0000f, 0.0446f}},
    {"array2",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -0.0308f, -0.0352f, 0.1147f}},
    {"array",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -0.0308f, -0.0352f, 0.1147f}},
    {"cylinder1",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -0.0186f, 0.0000f, 0.0446f}},
    {"barrel1",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0030f, 2.2791f, 0.9787f}},
    {"barrel",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0030f, 2.2791f, 0.9787f}},
    {"support",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -0.0186f, 1.1131f, 0.7195f}},
    {"step_platform",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -0.0033f, 0.0000f, 0.0446f}},
    {"Body_Bulk",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -0.0524f, 0.4984f, 0.5358f}},
};
/// geo_bldg_geoturret: aimer_y (pivot 0 0.26 0) turns, aimer_x (pivot 0 1.60 -0.58) pitches -45..15 deg with the disc,
/// the viewfinder and the yoke.
const TurretNode GEO_TURRET_NODES[] = {
    {"aimer_y",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.2640f, 0.0000f}},
    {"Left_Base_hub",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"Right_legs",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"leg_hub",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"aimer_x",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 1.5991f, -0.5758f}},
    {"viewfinder",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0313f, 1.8623f, -0.8087f}},
    {"Main_body",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"Main_disc",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"ring_latch_4",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"ring_latch_5",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"ring_latch_2",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"ring_latch_3",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"ring_latch_1",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"barrel",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"rotation_piece",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"handles",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"gripbase",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 1.5991f, -0.5758f}},
    {"grip",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, -0.0000f, 0.0000f}},
    {"foot_rest_body_attachment_2",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 0.9313f, -0.3642f, 0.0000f, 0.3642f, 0.9313f, 0.0000f, 0.7585f, 0.3692f}},
    {"foot_rest_body_attachment_1",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 0.9313f, -0.3642f, 0.0000f, 0.3642f, 0.9313f, 0.0000f, 0.7585f, 0.3692f}},
    {"Cylinder_for_rear_legs",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.2646f, 0.0000f}},
    {"Left_legs",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"Right_Base_hub",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.4692f, 0.0000f}},
    {"footsupport_main",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"footpad_L",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"footpad_R",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
};
/// bes_bldg_AAA_turret (model kam_prop_antiaircraft, Bespin's platforms): aimer_y (pivot 0 0 0) turns all round at 3.8
/// rad/s, aimer_x (pivot 0 3.06 -1.60) pitches -30..20 deg with the barrels and the gunner's seat, 3.5 m behind the
/// yaw axis (idren2023: the turret moves the player a lot).
const TurretNode AAA_TURRET_NODES[] = {
    {"aimer_y",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"aimer_y_lowrez",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"aimer_x",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"rear_balance",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"rear_balance_lowrez",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"dash",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -0.0429f, 0.1195f, 0.0000f}},
    {"ring",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"aimer_x_lowrez",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"gun_hanlde",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"controlhub",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -0.0429f, 0.1195f, 0.0000f}},
    {"controlhub_lowrez",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -0.0429f, 0.1195f, 0.0000f}},
    {"barrel1",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"balance1",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"shaft1",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"blastshield_B",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"blastshield1",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -2.4495f, -0.0945f, -0.0062f}},
    {"shaft_lowrez2",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -2.4495f, 0.2751f, -1.8848f}},
    {"barrel2_lowrez1",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -2.5294f, 3.0563f, -1.5976f}},
    {"barrel2",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"balance2",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"container2",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"lid1",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"barrel2_lowrez",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"shaf2",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"blastshield_C",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -0.0004f, -0.0204f, -0.0062f}},
    {"shaft_lowrez1",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.3492f, -1.8848f}},
    {"blastshield2",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -0.0004f, -0.0204f, -0.0062f}},
    {"rear_balance1",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"rear_balance1_lowrez",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"recoil",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"recoil_lowrez",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.0563f, -1.5976f}},
    {"mainleg",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"mainleg1_lowrez2",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, -2.5153f, 0.0000f, 0.0000f}},
    {"mainleg1",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"mainleg1_lowrez",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
    {"shock1",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f}},
};
/// hoth_bldg_hoth_turret (the white tower, Hoth): turret_y (the domed head, pivot 0 3.57 0) turns all round, turret_x
/// (pivot 0 3.58 1.46, the gun in the head's front) pitches -10..10 deg. No gunner shown in the game: the mod seats
/// one in a cabin of its own inside the head (ShellDesigns BuildHothTowerCabin).
const TurretNode TOWER_TURRET_NODES[] = {
    {"turret_y",
     1,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.5655f, 0.0000f}},
    {"turret_x",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.5817f, 1.4640f}},
    {"lgun",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.5808f, 1.6008f}},
    {"lgun1",
     2,
     {1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 0.0000f, 0.0000f, 1.0000f, 0.0000f, 3.5808f, 1.9741f}},
};
const BuildingTurret BUILDING_TURRETS[] = {
    {"com_weap_gunturret",
     "com_weap_gunturret",
     GUN_TURRET_NODES,
     static_cast<int>(sizeof(GUN_TURRET_NODES) / sizeof(GUN_TURRET_NODES[0])),
     {0.0f, 0.6f, 0.0f},
     {0.0f, 1.596f, 0.72f},
     {0.0f, 1.86f, -0.10f},
     {-0.075f, 1.42f, 0.21f}, // the middle of the right one of the two handles under the gun's back end
     23.0f,                   // their tops lean ahead (0.06 m over 0.14 m)
     1.0f,
     true,
     0.0f,
     nullptr,
     false,
     {0, 0, 0},
     0,
     {0, 0, 0}},
    {"tat_bldg_barge_turret",
     "tat_bldg_barge_turret",
     BARGE_TURRET_NODES,
     static_cast<int>(sizeof(BARGE_TURRET_NODES) / sizeof(BARGE_TURRET_NODES[0])),
     {0.0f, 0.0f, 0.0f},
     {0.0f, 0.30f, 0.02f},
     {0.0f, 0.56f, 0.85f},   // 1.6 m over the gunner's feet (hp_active_01 0 -1.04 0.93, on the yaw part)
     {0.055f, 0.15f, 0.50f}, // the middle of the right one of the two spade grips at the gun's back
     32.0f,                  // their tops lean ahead (0.09 m over 0.145 m)
     -1.0f,
     true,
     0.0f,
     nullptr,
     false,
     {0, 0, 0},
     0,
     {0, 0, 0}},
    {"hoth_bldg_dish_turret",
     "hoth_bldg_dish_turret",
     DISH_TURRET_NODES,
     static_cast<int>(sizeof(DISH_TURRET_NODES) / sizeof(DISH_TURRET_NODES[0])),
     {0.0f, 0.50f, 0.54f},
     {0.0f, 1.11f, 0.72f},
     {0.0f, 1.75f, -1.30f},  // 1.6 m over the gunner's feet (hp_active 0 0.15 -1.34, behind the column)
     {-0.26f, 1.38f, -1.02f}, // the mod's right handle on the column's back (ShellDesigns BuildDishScreen)
     15.0f,
     1.0f,
     true,
     0.0f,
     "hoth_bldg_dish_turret_screen",
     false,
     // The rail screen 0.38 m ahead of the eye (as the speeder's periscope): the handles stay 44 deg or more below
     // the view over the gun's -10..25 (ShellDesigns DISH_SCREEN_*).
     {0.0f, 1.75f, -1.30f + ShellDesigns::DISH_SCREEN_RAIL_METERS},
     ShellDesigns::DISH_SCREEN_HALF_WIDTH_METERS,
     {0.0f, 2.65f, 0.79f}, // hp_eyepoint_1, over the dish on the barrel
     false,
     true,
     false,
     -25.0f,
     30.0f},
    {"geo_bldg_geoturret",
     "geo_bldg_geoturret",
     GEO_TURRET_NODES,
     static_cast<int>(sizeof(GEO_TURRET_NODES) / sizeof(GEO_TURRET_NODES[0])),
     {0.0f, 0.26f, 0.0f},
     {0.0f, 1.60f, -0.58f},
     {0.0f, 1.86f, -1.40f},     // 1.6 m over the gunner's feet (hp_active 0 0.26 -1.45, on the foot pads)
     {-0.09f, 1.565f, -1.015f}, // the yoke's two handles under the viewfinder (x -0.09 and 0.16)
     0.0f,
     1.0f,
     true,
     0.035f,
     "geo_bldg_geoturret_screen",
     false,
     // A small rail screen 0.30 m ahead of the eye, in the gap between the yoke's handles (18 x 14 deg; they cross
     // the line of sight when looking 15-25 deg down, 0.385 m ahead): toward the gap's middle (x 0.035) by 0.30 / 0.385.
     {ShellDesigns::GEO_SCREEN_OFFSET_X_METERS, 1.86f, -1.40f + ShellDesigns::GEO_SCREEN_RAIL_METERS},
     ShellDesigns::GEO_SCREEN_HALF_WIDTH_METERS,
     {0.0f, 2.75f, 0.75f}, // over the barrel's root, in front of the disc
     false,
     false,
     false,
     -45.0f,
     15.0f},
    {"bes_bldg_AAA_turret",
     "kam_prop_antiaircraft",
     AAA_TURRET_NODES,
     static_cast<int>(sizeof(AAA_TURRET_NODES) / sizeof(AAA_TURRET_NODES[0])),
     {0.0f, 0.0f, 0.0f},
     {0.0f, 3.06f, -1.60f},
     {0.0f, 4.40f, -3.45f},  // seated on hp_main_gunner (0 3.32 -3.49, on the cradle), over the control hub; 20 cm
                             // higher since headset run 18 (to see over the front)
     {-0.27f, 3.89f, -3.0f}, // the right one of the two handle rods running ahead to the hub
     80.0f,                  // the rods lie nearly level: the fist round them, pointing down across
     1.0f,
     true,
     0.0f,
     nullptr,
     false,
     {0, 0, 0},
     0,
     {0, 0, 0},
     true,
     false,
     false,
     0.0f,
     0.0f,
     true}, // headset run 19: the hands on the rods by themselves
    {"hoth_bldg_hoth_turret",
     "hoth_bldg_hoth_turret",
     TOWER_TURRET_NODES,
     static_cast<int>(sizeof(TOWER_TURRET_NODES) / sizeof(TOWER_TURRET_NODES[0])),
     {0.0f, 3.57f, 0.0f},
     {0.0f, 3.58f, 1.46f},
     {0.0f, 3.75f, -0.10f},  // seated in the head, low in the tower's top (ShellDesigns HOTH_TOWER_EYE)
     {-0.22f, 3.33f, 0.32f}, // the right grip on the control pedestal
     15.0f,
     1.0f,
     true,
     0.0f,
     "hoth_bldg_hoth_turret_cabin", // the cabin round the gunner (idren2023: as the AAT gunner's, a lore interior)
     false,
     {0.0f, 3.6555f, 0.429f}, // the viewscreen's window (the stock Hoth viewfinder art's blue window)
     0.205f,
     {0.0f, 3.85f, 2.30f}, // over the barrel's root, in front of the head
     false,
     true},
};
const BuildingTurret* _building = nullptr;
constexpr size_t BUILDING_POSE_TABLE_BYTES = 0x500; // as Handlebars' (the game's pose table: hashes, then pointers)
constexpr int MOST_TURRET_NODES = 64;
unsigned _buildingHashes[MOST_TURRET_NODES];
const BuildingTurret* _hashedBuilding = nullptr;
using PoseTableInitFunction = void*(__thiscall*)(void* table);
using PoseTableSetFunction = void(__thiscall*)(void* table, unsigned hash, const float* matrix);
const char* _missingModel = nullptr; // the cockpit model last logged as not loaded

/// The cabin seat turns with the turret (PLAN 57 i: the combat speeder's turning seat, ShellDesigns
/// SPEEDER_CABIN_PARTS): its heading relative to the cabin as last drawn (radians, toward the right), and the end stop
/// it stands at (1 right, -1 left, 0 none). A stop is entered within STOP_ENTER of a yaw limit and left beyond
/// STOP_LEAVE (one pulse per stop).
float _seatYaw = 0.0f;
float _drawnPitched[16] = {}; // the cabin's pitching part as last drawn (shell space -> world; GetDrawnMuzzle)
float _seatPitch = 0.0f; // the gun's pitch over the cabin's level (radians, up positive): the periscope's rail
int _stopSide = 0;
constexpr float STOP_ENTER_RADIANS = 0.005f, STOP_LEAVE_RADIANS = 0.035f;
constexpr float STOP_PULSE_SECONDS = 0.08f, STOP_PULSE_AMPLITUDE = 0.8f;

/// Cabin::turretAhead of a turret without a node of its own (its matrix reads at the hull's origin).
constexpr float NO_TURRET_NODE = -1000.0f;

// How far ahead of (and right of) the hull's origin the current turret's origin is (metres along the hull's heading).
bool TurretAhead(float* ahead, float* right = nullptr)
{
    float turret[16], hull[16];
    if (!Vehicle::GetTurretMatrix(turret) || !Vehicle::GetHullMatrix(hull))
    {
        return false;
    }
    FpGuard fp;
    const float level = sqrtf(hull[8] * hull[8] + hull[10] * hull[10]);
    if (level < 0.01f)
    {
        return false;
    }
    const float offset[3] = {turret[12] - hull[12], turret[13] - hull[13], turret[14] - hull[14]};
    if (fabsf(offset[0]) + fabsf(offset[1]) + fabsf(offset[2]) < 0.02f)
    {
        *ahead = NO_TURRET_NODE; // a turret without a node of its own (the AT-TE's rear guns run601, the gunship's
        if (right)               // top gun run605, run608): reads at the hull's own origin
        {
            *right = 0;
        }
        return true;
    }
    *ahead = ((turret[12] - hull[12]) * hull[8] + (turret[14] - hull[14]) * hull[10]) / level;
    if (right)
    {
        const float side = sqrtf(hull[0] * hull[0] + hull[1] * hull[1] + hull[2] * hull[2]);
        *right = side > 0.01f ? ((turret[12] - hull[12]) * hull[0] + (turret[13] - hull[13]) * hull[1] +
                                 (turret[14] - hull[14]) * hull[2]) / side
                              : 0;
    }
    return true;
}

const Cabin* CabinOf(const char* odfName)
{
    if (!odfName)
    {
        return nullptr;
    }
    // A seat known by its turret's yaw limits first (Cabin::yawMiddleDegrees).
    float yaw = 0, low = 0, high = 0;
    if (Vehicle::GetTurretYaw(&yaw, &low, &high) && high - low < 60.0f * RADIANS_PER_DEGREE)
    {
        const float middleDegrees = fabsf(low + high) * 0.5f / RADIANS_PER_DEGREE;
        for (const Cabin& cabin : CABINS)
        {
            // (run647: the door seat's limits read 0..0, run648 -95..-60: a turret with no yaw range counts too)
            if (!strcmp(cabin.odfName, odfName) && !isnan(cabin.yawMiddleDegrees) &&
                (fabsf(middleDegrees - cabin.yawMiddleDegrees) < 15.0f || high - low < 0.5f * RADIANS_PER_DEGREE))
            {
                return &cabin;
            }
        }
    }
    const Cabin* best = nullptr;
    float bestDistance = 1e9f, ahead = 0, right = 0;
    const bool hasAhead = TurretAhead(&ahead, &right);
    for (const Cabin& cabin : CABINS)
    {
        if (strcmp(cabin.odfName, odfName) || !isnan(cabin.yawMiddleDegrees))
        {
            continue;
        }
        if (isnan(cabin.turretAhead))
        {
            return &cabin;
        }
        const float across = isnan(cabin.turretRight) ? 0 : right - cabin.turretRight;
        const float distance = hasAhead ? fabsf(ahead - cabin.turretAhead) + fabsf(across) : 0;
        if (!best || distance < bestDistance)
        {
            best = &cabin;
            bestDistance = distance;
        }
    }
    return best;
}

const Station* StationOf(const char* odfName)
{
    if (!odfName)
    {
        return nullptr;
    }
    for (const Station& station : STATIONS)
    {
        if (!strcmp(station.odfName, odfName))
        {
            return &station;
        }
    }
    return nullptr;
}

// The slot the game's lookup finds for a hash (0x300890), or -1.
int LookupSlot(const unsigned* hashes, unsigned hash)
{
    int slot = static_cast<int>(hash & (POSE_SLOTS - 1));
    for (int probe = 0; probe < POSE_SLOTS && hash; ++probe)
    {
        if (hashes[slot] == hash)
        {
            return slot;
        }
        if (hashes[slot] == 0)
        {
            return -1;
        }
        slot = slot > 0 ? slot - 1 : POSE_SLOTS - 1;
    }
    return -1;
}

bool IsRotation(const float* matrix)
{
    for (int row = 0; row < 3; ++row)
    {
        const float* axis = matrix + row * 4;
        const float length = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (!(fabsf(length - 1.0f) <= UNIT_TOLERANCE))
        {
            return false;
        }
    }
    return true;
}

// out = a x b (4x4, row vectors: a is applied first).
void Multiply(float* out, const float* a, const float* b)
{
    float result[16];
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float sum = 0;
            for (int k = 0; k < 4; ++k)
            {
                sum += a[row * 4 + k] * b[k * 4 + column];
            }
            result[row * 4 + column] = sum;
        }
    }
    memcpy(out, result, sizeof(result));
}

// A point of the hull model's space into the world through a matrix (row vectors).
void ToWorld(const float* matrix, const float* point, float* world)
{
    for (int axis = 0; axis < 3; ++axis)
    {
        world[axis] =
            point[0] * matrix[axis] + point[1] * matrix[4 + axis] + point[2] * matrix[8 + axis] + matrix[12 + axis];
    }
}

// The inverse of a rigid matrix (rotation rows and a place; row vectors).
void InvertRigid(float* out, const float* matrix)
{
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            out[row * 4 + column] = matrix[column * 4 + row];
        }
        out[row * 4 + 3] = 0;
    }
    for (int column = 0; column < 3; ++column)
    {
        out[12 + column] = -(matrix[12] * out[column] + matrix[13] * out[4 + column] + matrix[14] * out[8 + column]);
    }
    out[15] = 1;
}

// Both grips' hand poses in resting-pose space from the matrix of the part they sit on (design space -> world): the
// hand's place on the grip, pointing ahead across it, the grip up through the fist and leaning ahead at the top (as
// the tank levers hold a hand). rightGrip is in design space (x right, y up, z back); the left grip is its mirror
// about x = mirrorX.
void PlaceGrips(const float* yawPart, const float* rightGrip, float mirrorX, float leanDegrees)
{
    float base[16], inverseBase[16];
    if (!Aim::GetBaseFrame(base)) // the frame the arms are drawn in
    {
        _hasGripPoses = false;
        return;
    }
    InvertRigid(inverseBase, base);
    const float lean = leanDegrees * RADIANS_PER_DEGREE;
    for (int hand = 0; hand < 2; ++hand)
    {
        const float x = hand == 0 ? rightGrip[0] : 2 * mirrorX - rightGrip[0];
        const float onGrip[16] =
            {1, 0, 0, 0, 0, cosf(lean), -sinf(lean), 0, 0, sinf(lean), cosf(lean), 0, x, rightGrip[1], rightGrip[2], 1};
        float world[16];
        Multiply(world, onGrip, yawPart);
        Multiply(_gripPoses[hand], world, inverseBase);
    }
    _hasGripPoses = true;
}

// One grip's step (once a frame): taken by a new grip press within reach, let go with the grip or when the station
// is not shown.
void StepGrip(int hand, bool isShown)
{
    Grip& grip = _grips[hand];
    const Gesture::Hand& state = Gesture::CurrentHand(hand);
    const bool hasGripStarted = state.isGripped && !grip.wasGripped;
    grip.wasGripped = state.isGripped;
    if (_building && _building->areGripsAutoHeld)
    {
        const bool isHeld = isShown && _hasGripPoses && state.isTracked;
        if (isHeld != grip.isHeld)
        {
            grip.isHeld = isHeld;
            Log::Info("gunner", "%s grip %s by itself", hand == 0 ? "right" : "left", isHeld ? "taken" : "let go");
        }
        return;
    }
    if (!grip.isHeld && isShown && _hasGripPoses && state.isTracked && hasGripStarted)
    {
        FpGuard fp;
        // From the hand's palm (where it would close) to the grip.
        const float* place = _gripPoses[hand] + 12;
        float position[3] = {state.pose[12], state.pose[13], state.pose[14]};
        float palm[3];
        if (Aim::GetPalmInHand(palm))
        {
            palm[0] = hand == 0 ? palm[0] : -palm[0];
            for (int axis = 0; axis < 3; ++axis)
            {
                position[axis] +=
                    palm[0] * state.pose[axis] + palm[1] * state.pose[4 + axis] + palm[2] * state.pose[8 + axis];
            }
        }
        const float dx = position[0] - place[0], dy = position[1] - place[1], dz = position[2] - place[2];
        const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        Log::Info(
            "gunner",
            "%s grip press: hand at %.2f %.2f %.2f, palm %.2f %.2f %.2f, grip %.2f %.2f %.2f: %.0f cm",
            hand == 0 ? "right" : "left",
            static_cast<double>(state.pose[12]),
            static_cast<double>(state.pose[13]),
            static_cast<double>(state.pose[14]),
            static_cast<double>(position[0]),
            static_cast<double>(position[1]),
            static_cast<double>(position[2]),
            static_cast<double>(place[0]),
            static_cast<double>(place[1]),
            static_cast<double>(place[2]),
            static_cast<double>(distance * 100));
        if (distance < REACH_METERS)
        {
            grip.isHeld = true;
            Feedback::Pulse(hand, 0.02f, 0.4f);
            Log::Info("gunner", "%s grip taken", hand == 0 ? "right" : "left");
        }
    }
    else if (grip.isHeld && (!isShown || !state.isTracked || !state.isGripped))
    {
        grip.isHeld = false;
        Log::Info("gunner", "%s grip let go", hand == 0 ? "right" : "left");
    }
}

// Whether two points lie within a distance of each other.
bool IsNear(const float* first, const float* second, float meters)
{
    const float dx = first[0] - second[0], dy = first[1] - second[1], dz = first[2] - second[2];
    return dx * dx + dy * dy + dz * dz < meters * meters;
}

// The cabin seat's shell on the hull's level heading (as the pilot seat's cockpit): shell space (x right, z back) ->
// hull model space (x left, z forward; the shell's gunner eye on the vehicle's own gunner's) -> the world through the
// hull's level matrix (its heading only, at the hull's place). False without a hull. FpGuard held.
bool CabinMatrix(float* matrix)
{
    float hull[16];
    if (!_cabin || !Vehicle::GetHullMatrix(hull))
    {
        return false;
    }
    const float length = sqrtf(hull[8] * hull[8] + hull[10] * hull[10]);
    if (length < 0.1f)
    {
        return false;
    }
    const float forwardX = hull[8] / length, forwardZ = hull[10] / length;
    // up x forward = (forwardZ, 0, -forwardX); flipped if the hull's own x points the other way (as Handlebars).
    const float side = hull[0] * forwardZ - hull[2] * forwardX < 0 ? -1.0f : 1.0f;
    float level[16] = {
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
    if (_cabin->isOnFlyer)
    {
        // a flyer pitches and rolls: the hull's own axes (scale taken out), as its pilot's cockpit
        for (int row = 0; row < 3; ++row)
        {
            const float* axis = hull + row * 4;
            const float size = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
            if (size < 0.1f)
            {
                return false;
            }
            for (int k = 0; k < 3; ++k)
            {
                level[row * 4 + k] = axis[k] / size;
            }
        }
    }
    const float* eye = _cabin->gunnerEye;
    const float* anchor = _cabin->gunnerEyeInHull;
    const float toHull[16] =
        {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, eye[0] + anchor[0], anchor[1] - eye[1], eye[2] + anchor[2], 1};
    Multiply(matrix, toHull, level);
    return true;
}

bool ForcedEye(float* eye)
{
    // The gunship's door passenger has no first-person view in the game (the chase camera stays outside, run647): the
    // view is put at his eye (Stereo, where the chase camera sets the camera). FpGuard held by the caller.
    const Cabin* cabin = _cabin;
    float matrix[16];
    if (!_isTurretOn || !cabin || Vehicle::CurrentSeat() != Vehicle::Seat::Turret || strcmp(cabin->designName, "rep_fly_gunship_door") || !CabinMatrix(matrix))
    {
        return false;
    }
    ToWorld(matrix, cabin->gunnerEye, eye);
    return true;
}

// The cabin seat's moving parts: `turned`, the cabin's matrix turned about the vertical through the gunner's eye by
// the turret's heading relative to the cabin (the turret's forward in shell space: x right, z back); `pitched`, that
// also turned about the horizontal through the eye by the gun's pitch (the periscope on its rails). Both stay as they
// were when the turret cannot be read. FpGuard held.
void TurnCabinSeat(const float* matrix, float* turned, float* pitched)
{
    // A gun aimed below the head's line (Cabin::aimDownDegrees): the display raised back by as much, so it stays in
    // front of the eyes and a level head sees the middle of the range.
    const float aimRaise = _cabin->aimDownDegrees * 0.01745329f;
    float turret[16];
    if (Vehicle::GetTurretMatrix(turret))
    {
        float local[3]; // the shell's axes are the matrix's rows
        for (int axis = 0; axis < 3; ++axis)
        {
            const float* row = matrix + axis * 4;
            local[axis] = turret[8] * row[0] + turret[9] * row[1] + turret[10] * row[2];
        }
        const float level = sqrtf(local[0] * local[0] + local[2] * local[2]);
        if (level > 0.1f)
        {
            _seatYaw = atan2f(local[0], -local[2]);
            _seatPitch = atan2f(local[1], level) + aimRaise;
        }
    }
    // The gun's pitch from its fire line where there is one: the AAT's turret matrix turns only with the yaw (the
    // pitch is Main_gun's; run333: the display stayed level); the speeder's agrees with its turret's.
    float line[6];
    if (Vehicle::GetSeatFireLines(line, 1) >= 1)
    {
        float local[3];
        for (int axis = 0; axis < 3; ++axis)
        {
            const float* row = matrix + axis * 4;
            local[axis] = line[3] * row[0] + line[4] * row[1] + line[5] * row[2];
        }
        const float level = sqrtf(local[0] * local[0] + local[2] * local[2]);
        if (level > 0.1f)
        {
            _seatPitch = atan2f(local[1], level) + aimRaise;
        }
    }
    // The speeder's periscope (PLAN 58 d, headset run 17: it hardly moved with the gun's -15..10): the head's pitch over
    // the cabin's level, within the display's own limits.
    float head[16];
    if (_cabin->isPeriscopeByHead && Stereo::GetLastHeadWorld(head))
    {
        float local[3];
        for (int axis = 0; axis < 3; ++axis)
        {
            const float* row = matrix + axis * 4;
            local[axis] = -(head[8] * row[0] + head[9] * row[1] + head[10] * row[2]);
        }
        const float level = sqrtf(local[0] * local[0] + local[2] * local[2]);
        constexpr float RADIANS_PER_DEGREE = 0.01745329f;
        constexpr float LOW = ShellDesigns::SPEEDER_PERISCOPE_LOW_DEGREES * RADIANS_PER_DEGREE;
        constexpr float HIGH = ShellDesigns::SPEEDER_PERISCOPE_HIGH_DEGREES * RADIANS_PER_DEGREE;
        const float pitch = atan2f(local[1], level);
        _seatPitch = pitch < LOW ? LOW : pitch > HIGH ? HIGH : pitch;
    }
    if (!isnan(_cabin->sightHighDegrees) && _seatPitch > _cabin->sightHighDegrees * 0.01745329f)
    {
        _seatPitch = _cabin->sightHighDegrees * 0.01745329f; // under the canopy's roof
    }
    const float cosine = cosf(_seatYaw), sine = sinf(_seatYaw);
    const float* pivot = _cabin->gunnerEye;
    const float turn[16] = {
        cosine,
        0,
        sine,
        0,
        0,
        1,
        0,
        0,
        -sine,
        0,
        cosine,
        0,
        pivot[0] - (pivot[0] * cosine - pivot[2] * sine),
        0,
        pivot[2] - (pivot[0] * sine + pivot[2] * cosine),
        1};
    Multiply(turned, turn, matrix);
    // Forward (-z) raised toward up by the pitch: y -> (0, cos, sin), z -> (0, -sin, cos), about the eye (or the
    // cabin's pitch pivot: the AT-TE cannon's trunnion).
    if (!isnan(_cabin->pitchPivot[0]))
    {
        pivot = _cabin->pitchPivot;
    }
    const float pitchCosine = cosf(_seatPitch), pitchSine = sinf(_seatPitch);
    const float raise[16] = {
        1,
        0,
        0,
        0,
        0,
        pitchCosine,
        pitchSine,
        0,
        0,
        -pitchSine,
        pitchCosine,
        0,
        0,
        pivot[1] - (pivot[1] * pitchCosine - pivot[2] * pitchSine),
        pivot[2] - (pivot[1] * pitchSine + pivot[2] * pitchCosine),
        1};
    Multiply(pitched, raise, turned);
}

// The cabin seat's end stops (once a frame): at a yaw limit of the turret one pulse in both controllers and the stop
// lamps of that side (drawn by DrawCabin); the turret's yaw and the drawn seat's are logged at each stop (which of
// them the limits apply to was not known).
void StepStops()
{
    float yaw = 0, low = 0, high = 0;
    int side = 0;
    if (_cabin && _cabin->hasEndStops && Vehicle::GetTurretYaw(&yaw, &low, &high))
    {
        FpGuard fp;
        const float margin = _stopSide ? STOP_LEAVE_RADIANS : STOP_ENTER_RADIANS;
        // The side comes from the limit reached, not from the drawn seat's sign: a ball turret resting at a limit
        // (yaw 0, limits 0..115) has the seat at +-0 and flipped sides every frame (log spam, runs 647-649). The seat
        // is drawn at minus the turret's yaw, so the high limit is the seat's left.
        if (yaw >= high - margin)
        {
            side = -1;
        }
        else if (yaw <= low + margin)
        {
            side = 1;
        }
    }
    if (side == _stopSide)
    {
        return;
    }
    _stopSide = side;
    if (side)
    {
        Feedback::Pulse(0, STOP_PULSE_SECONDS, STOP_PULSE_AMPLITUDE);
        Feedback::Pulse(1, STOP_PULSE_SECONDS, STOP_PULSE_AMPLITUDE);
    }
    Log::Info(
        "gunner",
        "end stop %s: turret yaw %.1f deg (limits %.1f..%.1f), seat drawn at %.1f deg",
        side > 0   ? "right"
        : side < 0 ? "left"
                   : "off",
        static_cast<double>(yaw / RADIANS_PER_DEGREE),
        static_cast<double>(low / RADIANS_PER_DEGREE),
        static_cast<double>(high / RADIANS_PER_DEGREE),
        static_cast<double>(_seatYaw / RADIANS_PER_DEGREE));
}

// The cabin seat in first person: the game's pilot first-person model and the pilot cabin's shell, placed as in the
// pilot seat, the gunner's seat turned with the turret, the hands on his yoke. False when the cabin cannot be placed or
// the game camera is far from the gunner's eye (third person).
bool DrawCabin(unsigned flags)
{
    if (!_isTurretOn)
    {
        return false;
    }
    float matrix[16], turned[16], pitched[16];
    {
        FpGuard fp;
        float camera[16], eye[3];
        if (!CabinMatrix(matrix) || !Stereo::GetGameCamera(camera))
        {
            return false;
        }
        ToWorld(matrix, _cabin->gunnerEye, eye);
        if (!IsNear(camera + 12, eye, _cabin->firstPersonRangeMeters))
        {
            return false;
        }
        TurnCabinSeat(matrix, turned, pitched);
        memcpy(_drawnPitched, pitched, sizeof(_drawnPitched));
    }
    void* model = _cabin->cockpitModelName && GameBuild::IsAvailable(GameBuild::Feature::Holsters)
                      ? Holster::LoadedModel(_cabin->cockpitModelName)
                      : nullptr;
    if (model)
    {
        // Openings the cabin cuts in the game's cockpit model (the combat speeder's front right window).
        CockpitShell::CutOpenings(D3dSurvey::PresentingDevice(), matrix, _cabin->designName);
        void* light = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::ModelLight));
        const auto draw = (*reinterpret_cast<ModelDrawFunction**>(model))[1];
        draw(model, nullptr, matrix, nullptr, light, flags | MODEL_DRAW_FLAGS, 0);
    }
    else if (_cabin->cockpitModelName && _missingModel != _cabin->cockpitModelName)
    {
        _missingModel = _cabin->cockpitModelName;
        Log::Warning("gunner", "model %s not loaded: the cabin is drawn without it", _cabin->cockpitModelName);
    }
    const float* parts[ShellDesigns::SPEEDER_CABIN_PARTS] =
        {matrix, turned, pitched, _stopSide > 0 ? pitched : nullptr, _stopSide < 0 ? pitched : nullptr, pitched};
    CockpitShell::DrawParts(
        D3dSurvey::PresentingDevice(),
        parts,
        ShellDesigns::SPEEDER_CABIN_PARTS,
        _cabin->designName);
    {
        FpGuard fp;
        PlaceGrips(_cabin->isChairFixed ? matrix : turned, _cabin->grip, _cabin->gripMirrorX, _cabin->gripLeanDegrees);
    }
    _drawnFrame = _frame;
    _hasDrawn = true;
    return true;
}

// Whether our station was drawn in the last frame (first person, a station seat, the test on).
bool IsShown()
{
    return (_station || _cabin || _building) && _isTurretOn && _hasDrawn && _drawnFrame + 1 >= _frame;
}

// Once, some time after a seat change: whether the hull's draw was seen with its pose table, the bones' live places
// (model space expected: at rest each is its msh rest place) and how far the draw's matrix is from the hull entity's.
void LogHullDraw()
{
    if (!_isHullLogPending || GetTickCount() - _seatTick < HULL_LOG_DELAY_MILLISECONDS)
    {
        return;
    }
    _isHullLogPending = false;
    if (!_hasCapture)
    {
        Log::Warning(
            "gunner",
            "%s: the hull's draw with its pose table was not seen; the stock turret stays and so does the game's "
            "frame",
            _station->odfName);
        return;
    }
    FpGuard fp;
    float entity[16];
    float offset = -1, turn = -1;
    if (Vehicle::GetHullMatrix(entity))
    {
        offset = sqrtf(
            (entity[12] - _hullDrawMatrix[12]) * (entity[12] - _hullDrawMatrix[12]) +
            (entity[13] - _hullDrawMatrix[13]) * (entity[13] - _hullDrawMatrix[13]) +
            (entity[14] - _hullDrawMatrix[14]) * (entity[14] - _hullDrawMatrix[14]));
        turn = 0;
        for (int element = 0; element < 12; ++element)
        {
            turn = fmaxf(turn, fabsf(entity[element] - _hullDrawMatrix[element]));
        }
    }
    Log::Info(
        "gunner",
        "%s: hull draws %d a second, bones found %d/%d; %s at %.3f %.3f %.3f fwd %.2f %.2f %.2f, %s at %.3f %.3f %.3f "
        "fwd %.2f %.2f %.2f, %s at %.3f %.3f %.3f; draw matrix vs entity: %.3f m, %.3f in the axes",
        _station->odfName,
        _hullDrawsThisSecond,
        (_boneFoundMask & 1) + ((_boneFoundMask >> 1) & 1) + ((_boneFoundMask >> 2) & 1),
        PART_COUNT,
        _station->boneNames[0],
        static_cast<double>(_bones[0][12]),
        static_cast<double>(_bones[0][13]),
        static_cast<double>(_bones[0][14]),
        static_cast<double>(_bones[0][8]),
        static_cast<double>(_bones[0][9]),
        static_cast<double>(_bones[0][10]),
        _station->boneNames[1],
        static_cast<double>(_bones[1][12]),
        static_cast<double>(_bones[1][13]),
        static_cast<double>(_bones[1][14]),
        static_cast<double>(_bones[1][8]),
        static_cast<double>(_bones[1][9]),
        static_cast<double>(_bones[1][10]),
        _station->boneNames[2],
        static_cast<double>(_bones[2][12]),
        static_cast<double>(_bones[2][13]),
        static_cast<double>(_bones[2][14]),
        static_cast<double>(offset),
        static_cast<double>(turn));
}

// For a few seconds after a seat change, once a second: where each bone's forward points in the hull model (heading
// from straight ahead, left positive as the model's x; pitch up positive), to tell model-space rotations (all three
// share the yaw bone's heading) from rotations relative to the parent bone.
void LogBoneHeadings()
{
    static DWORD lastTick = 0;
    const DWORD now = GetTickCount();
    if (!_hasCapture || now - _seatTick > 12000 || now - lastTick < 1000)
    {
        return;
    }
    lastTick = now;
    FpGuard fp;
    double heading[PART_COUNT], pitch[PART_COUNT];
    for (int part = 0; part < PART_COUNT; ++part)
    {
        const float* forward = _bones[part] + 8;
        heading[part] = atan2(forward[0], forward[2]) * 57.29578;
        pitch[part] = asin(fmax(-1.0, fmin(1.0, forward[1]))) * 57.29578;
    }
    Log::Info(
        "gunner",
        "bones (heading, pitch in degrees): yaw %.1f %.1f, pitch %.1f %.1f, barrel %.1f %.1f",
        heading[0],
        pitch[0],
        heading[1],
        pitch[1],
        heading[2],
        pitch[2]);
}

} // namespace

void OnPresent()
{
    ++_frame;
    static DWORD secondTick = 0;
    const DWORD now = GetTickCount();
    if (now - secondTick >= 1000)
    {
        secondTick = now;
        _hullDrawsThisSecond = _hullDrawCount;
        _hullDrawCount = 0;
    }
    const char* stockName =
        Vehicle::CurrentSeat() == Vehicle::Seat::Turret && Xr::IsRunning() ? Vehicle::CurrentStockName() : nullptr;
    const BuildingTurret* building = nullptr;
    for (const BuildingTurret& candidate : BUILDING_TURRETS)
    {
        if (stockName && !strcmp(candidate.odfName, stockName))
        {
            building = &candidate;
        }
    }
    if (building && !GameBuild::IsAvailable(GameBuild::Feature::VehicleHands))
    {
        building = nullptr; // the pose table helpers (PoseTableInit / Set)
    }
    if (building != _building)
    {
        _building = building;
        Log::Info("gunner", "building turret %s", building ? building->odfName : "off");
    }
    const Station* station = StationOf(stockName);
    const Cabin* cabin = station ? nullptr : CabinOf(stockName);
    if ((station || cabin) && !(GameBuild::IsAvailable(GameBuild::Feature::FullBody) &&
                                GameBuild::IsAvailable(GameBuild::Feature::Muzzle) && Body::InstallModelDrawHook()))
    {
        station = nullptr; // logged once by GameBuild / Body
        cabin = nullptr;
    }
    if (cabin != _cabin)
    {
        _cabin = cabin;
        _hasDrawn = false;
        Log::Info("gunner", "cabin seat %s (%s)", cabin ? cabin->odfName : "off", cabin ? cabin->designName : "-");
    }
    if (station != _station)
    {
        _station = station;
        _hasCapture = _hasDrawn = false;
        _boneFoundMask = 0;
        _seatTick = now;
        _isHullLogPending = station != nullptr;
        if (station)
        {
            memcpy(_collapsed + 12, station->collapsedPlace, sizeof(station->collapsedPlace));
            const auto hashName = reinterpret_cast<HashNameFunction>(GameBuild::Absolute(GameBuild::Address::HashName));
            for (int part = 0; part < PART_COUNT; ++part)
            {
                _hashes[part] = hashName(station->boneNames[part]);
            }
        }
        Log::Info("gunner", "station %s", station ? station->designName : "off");
    }
    const uintptr_t hull = station || cabin ? Vehicle::CurrentHullEntity() : 0;
    _poseTable = hull && !IsBadReadPtr(reinterpret_cast<void*>(hull + OFFSET_hoverPoseTable), 0x408)
                     ? hull + OFFSET_hoverPoseTable
                     : 0;
    if (station)
    {
        LogHullDraw();
        LogBoneHeadings();
    }
    StepStops();
    const bool isShown = IsShown();
    StepGrip(0, isShown);
    StepGrip(1, isShown);
}

bool BeforeHullDraw(uintptr_t boneSet, const float* matrix)
{
    if (!_station || !_poseTable || boneSet != _poseTable || !matrix) // a cabin seat's hull: ShouldSkipHullDraw
    {
        return false;
    }
    ++_hullDrawCount;
    const unsigned* hashes = reinterpret_cast<const unsigned*>(boneSet + OFFSET_poseHashes);
    float** matrices = reinterpret_cast<float**>(boneSet + OFFSET_poseHashes + POSE_SLOTS * sizeof(unsigned));
    // Keep the live bones the game's lookup would find (once a frame: the first draw is the scene's).
    if (_captureFrame != _frame || !_hasCapture)
    {
        int found = 0;
        for (int part = 0; part < PART_COUNT; ++part)
        {
            const int slot = LookupSlot(hashes, _hashes[part]);
            if (slot >= 0 && matrices[slot])
            {
                memcpy(_bones[part], matrices[slot], sizeof(_bones[part]));
                found |= 1 << part;
            }
        }
        _boneFoundMask = found;
        memcpy(_hullDrawMatrix, matrix, sizeof(_hullDrawMatrix));
        _captureFrame = _frame;
        _hasCapture = true;
    }
    // Hide the stock turret (or show its gun) only while our station is drawn (first person, the last frame): in third
    // person the game's turret stays as it is.
    if (!_isTurretOn || !_hasDrawn || _drawnFrame + 1 < _frame)
    {
        return false;
    }
    if (!_station->hidesStockTurret)
    {
        return false;
    }
    _changedCount = 0;
    for (int slot = 0; slot < POSE_SLOTS && _changedCount < MOST_CHANGED; ++slot)
    {
        const unsigned hash = hashes[slot];
        if (!hash || !matrices[slot] || (hash != _hashes[0] && hash != _hashes[1] && hash != _hashes[2]))
        {
            continue;
        }
        _changed[_changedCount++] = {&matrices[slot], matrices[slot]};
        matrices[slot] = _collapsed;
    }
    return _changedCount > 0;
}

bool ShouldSkipHullDraw(uintptr_t boneSet)
{
    return _cabin && _poseTable && boneSet == _poseTable && IsShown();
}

void AfterHullDraw()
{
    for (int index = 0; index < _changedCount; ++index)
    {
        *_changed[index].slot = _changed[index].original;
    }
    _changedCount = 0;
}

bool BuildingTurretFrames(float* yawDelta, float* pitchDelta, float* building); // with DrawTurret below

bool GetSeatEye(const float* gameCameraPosition, float* eyeWorld)
{
    if (_building)
    {
        FpGuard fp;
        float yawDelta[16], pitchDelta[16], building[16], eyeModel[3];
        if (!BuildingTurretFrames(yawDelta, pitchDelta, building))
        {
            return false;
        }
        ToWorld(_building->isEyeOnGun ? pitchDelta : yawDelta, _building->eye, eyeModel); // turns with its part
        ToWorld(building, eyeModel, eyeWorld);
        // With hysteresis (headset run 18: on the Bespin AAA, whose seat rides the gun's pitch 3.5 m behind the yaw
        // axis, looking up dropped the view to the game's camera behind the turret): first person from within the
        // range, third person only past BUILDING_THIRD_PERSON_METERS.
        static bool isFirstPerson = false;
        const float dx = gameCameraPosition[0] - eyeWorld[0], dy = gameCameraPosition[1] - eyeWorld[1],
                    dz = gameCameraPosition[2] - eyeWorld[2];
        const float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        const bool wasFirstPerson = isFirstPerson;
        isFirstPerson = distance < (isFirstPerson ? BUILDING_THIRD_PERSON_METERS : FIRST_PERSON_RANGE_METERS);
        static DWORD lastLogTick = 0;
        if (isFirstPerson != wasFirstPerson || GetTickCount() - lastLogTick > 2000)
        {
            lastLogTick = GetTickCount();
            Log::Info("gunner", "building turret: game camera %.2f m from the gunner's eye: %s", static_cast<double>(distance),
                      isFirstPerson ? "first person" : "third person");
        }
        return isFirstPerson;
    }
    if (_cabin && _isTurretOn && _hasDrawn && _drawnFrame + 1 >= _frame)
    {
        FpGuard fp;
        float matrix[16];
        if (!CabinMatrix(matrix))
        {
            return false;
        }
        ToWorld(matrix, _cabin->gunnerEye, eyeWorld);
        return IsNear(gameCameraPosition, eyeWorld, _cabin->firstPersonRangeMeters);
    }
    if (!_station || !_isTurretOn || !_hasDrawn || _drawnFrame + 1 < _frame)
    {
        return false;
    }
    float hull[16];
    if (!Vehicle::GetHullMatrix(hull))
    {
        return false;
    }
    FpGuard fp;
    ToWorld(hull, _station->eye, eyeWorld);
    const float dx = gameCameraPosition[0] - eyeWorld[0], dy = gameCameraPosition[1] - eyeWorld[1],
                dz = gameCameraPosition[2] - eyeWorld[2];
    return dx * dx + dy * dy + dz * dz < FIRST_PERSON_RANGE_METERS * FIRST_PERSON_RANGE_METERS;
}

float AimDownRadians()
{
    return _cabin ? _cabin->aimDownDegrees * 0.01745329f : 0.0f;
}

bool IsFlyerCabinSeat()
{
    return _cabin && _cabin->isOnFlyer && IsShown();
}

bool GetDrawnMuzzle(float* position)
{
    if (!_cabin || isnan(_cabin->muzzle[0]) || !IsShown())
    {
        return false;
    }
    FpGuard fp;
    ToWorld(_drawnPitched, _cabin->muzzle, position);
    return true;
}

bool IsFacingAft()
{
    return _cabin && _cabin->isFacingAft && IsShown();
}

bool IsHeldViewCabinSeat()
{
    return _cabin && _cabin->isViewHeld && IsShown();
}

bool IsStationSeat()
{
    return _station != nullptr && _isTurretOn;
}

// A camera at `from` looking along `forward`, upright as `up` (world; camera convention: right, up, back, position).
bool LookAlong(const float* from, const float* forward, const float* up, float* camera)
{
    float right[3] = {
        forward[1] * up[2] - forward[2] * up[1],
        forward[2] * up[0] - forward[0] * up[2],
        forward[0] * up[1] - forward[1] * up[0]};
    const float length = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    if (length < 1e-3f)
    {
        return false;
    }
    for (float& value : right)
    {
        value /= length;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        const int next = (axis + 1) % 3, after = (axis + 2) % 3;
        camera[axis] = right[axis];
        camera[4 + axis] = right[next] * forward[after] - right[after] * forward[next];
        camera[8 + axis] = -forward[axis];
        camera[12 + axis] = from[axis];
        camera[3 + axis * 4] = 0;
    }
    camera[15] = 1;
    return true;
}

bool IsRailScreen()
{
    return _building->screenLowDegrees != 0 || _building->screenHighDegrees != 0;
}

// A building turret's rail screen (BuildingTurret.screenLowDegrees): design space (x right, y up, z toward the
// gunner) -> world, the turning part's frame raised about the horizontal through the eye by the head's pitch over its
// level, within the screen's limits. FpGuard held.
void BuildingRailScreen(const float* yawDelta, const float* building, float* part)
{
    const float f = _building->forwardZ;
    const float toModel[16] = {-f, 0, 0, 0, 0, 1, 0, 0, 0, 0, -f, 0, 0, 0, 0, 1};
    float turningPart[16], turning[16];
    Multiply(turningPart, toModel, yawDelta);
    Multiply(turning, turningPart, building);
    constexpr float RADIANS_PER_DEGREE = 0.01745329f;
    const float low = _building->screenLowDegrees * RADIANS_PER_DEGREE;
    const float high = _building->screenHighDegrees * RADIANS_PER_DEGREE;
    float pitch = 0, head[16];
    if (Stereo::GetLastHeadWorld(head))
    {
        float local[3];
        for (int axis = 0; axis < 3; ++axis)
        {
            const float* row = turning + axis * 4;
            local[axis] = -(head[8] * row[0] + head[9] * row[1] + head[10] * row[2]);
        }
        pitch = atan2f(local[1], sqrtf(local[0] * local[0] + local[2] * local[2]));
    }
    pitch = pitch < low ? low : pitch > high ? high : pitch;
    // Forward (-z) raised toward up: y -> (0, cos, sin), z -> (0, -sin, cos), about the eye.
    const float* e = _building->eye;
    const float eye[3] = {-f * e[0], e[1], -f * e[2]};
    const float c = cosf(pitch), s = sinf(pitch);
    const float raise[16] = {
        1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, eye[1] - (eye[1] * c - eye[2] * s), eye[2] - (eye[1] * s + eye[2] * c), 1};
    Multiply(part, raise, turning);
}

// A building turret's viewscreen (BuildingTurret.screenDesign): the screen face and the gun's camera, from this
// frame's turret angles (the scope pass runs before the first-person draw).
bool GetBuildingPeriscope(Periscope& periscope)
{
    FpGuard fp;
    float yawDelta[16], pitchDelta[16], building[16];
    if (!BuildingTurretFrames(yawDelta, pitchDelta, building))
    {
        return false;
    }
    const float f = _building->forwardZ;
    const float toModel[16] = {-f, 0, 0, 0, 0, 1, 0, 0, 0, 0, -f, 0, 0, 0, 0, 1};
    float gun[16], turning[16], part[16];
    Multiply(gun, pitchDelta, building);
    if (IsRailScreen())
    {
        BuildingRailScreen(yawDelta, building, part);
        const float* s = _building->screen;
        const float centre[3] = {-f * s[0], s[1], -f * s[2]}; // design space
        memcpy(periscope.screen, part, sizeof(periscope.screen));
        ToWorld(part, centre, periscope.screen + 12);
    }
    else
    {
        Multiply(turning, _building->isScreenOnGun ? pitchDelta : yawDelta, building);
        Multiply(part, toModel, turning); // design space (x right, y up, z toward the gunner) -> world
        memcpy(periscope.screen, part, sizeof(periscope.screen));
        ToWorld(turning, _building->screen, periscope.screen + 12);
    }
    periscope.screen[15] = 1;
    periscope.halfWidthMeters = _building->screenHalfWidth;
    float from[3];
    ToWorld(gun, _building->camera, from);
    const float forward[3] = {f * gun[8], f * gun[9], f * gun[10]};
    return LookAlong(from, forward, periscope.screen + 4, periscope.camera);
}

bool GetPeriscope(Periscope& periscope)
{
    if (_building && _building->screenDesign && _isTurretOn && _hasDrawn && _drawnFrame + 1 >= _frame)
    {
        return GetBuildingPeriscope(periscope);
    }
    if (!_cabin || _cabin->isPlainSeat || !_isTurretOn || !_hasDrawn || _drawnFrame + 1 < _frame)
    {
        return false;
    }
    constexpr int MOST_LINES = 4;
    float lines[MOST_LINES * 6];
    const int lineCount = Vehicle::GetSeatFireLines(lines, MOST_LINES);
    FpGuard fp;
    float matrix[16], turned[16], pitched[16];
    if (lineCount < 1 || !CabinMatrix(matrix))
    {
        return false;
    }
    TurnCabinSeat(matrix, turned, pitched);
    // The screen: the housing's frame (x right, y up, z toward the eye) at its place on the rails, or the cabin's own
    // viewscreen on the turning part.
    const float* eye = _cabin->gunnerEye;
    const bool isFixedScreen = !isnan(_cabin->viewscreen[0]);
    const float sightDistance =
        isnan(_cabin->sightDistance) ? ShellDesigns::SPEEDER_PERISCOPE_DISTANCE_METERS : _cabin->sightDistance;
    const float railCentre[3] = {eye[0], eye[1], eye[2] - sightDistance};
    const float* centre = isFixedScreen ? _cabin->viewscreen : railCentre;
    const float* screenMatrix = isFixedScreen ? turned : pitched;
    memcpy(periscope.screen, screenMatrix, sizeof(periscope.screen));
    ToWorld(screenMatrix, centre, periscope.screen + 12);
    periscope.screen[15] = 1;
    periscope.halfWidthMeters =
        isFixedScreen                 ? _cabin->viewscreen[3]
        : isnan(_cabin->sightHalfWidth) ? ShellDesigns::SPEEDER_PERISCOPE_HALF_WIDTH_METERS
                                        : _cabin->sightHalfWidth;
    periscope.fieldFactor = _cabin->periscopeFieldFactor;
    // The camera: between the guns' fire points, along the first gun's shot, upright as the screen.
    float from[3] = {0, 0, 0};
    for (int line = 0; line < lineCount; ++line)
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            from[axis] += lines[line * 6 + axis] / static_cast<float>(lineCount);
        }
    }
    // Its direction: the first gun's shot, or with the display pitched by the head the display's own forward (the
    // picture a window at the display's pitch; the reticle stays on the gun's line).
    float screenForward[3] = {-pitched[8], -pitched[9], -pitched[10]};
    const float* forward = _cabin->isPeriscopeByHead ? screenForward : lines + 3;
    return LookAlong(from, forward, periscope.screen + 4, periscope.camera);
}

// The building turret's frame of the turning part: model space -> world, with the yaw (and, with `withPitch`, the
// gun's pitch) applied about their pivots; `delta` the model-space change alone (rest -> now). FpGuard held.
bool BuildingTurretFrames(float* yawDelta, float* pitchDelta, float* building)
{
    float pitch = 0, yaw = 0;
    if (!_building || !Vehicle::GetHullMatrix(building) || !Vehicle::GetTurretAngles(&pitch, &yaw))
    {
        return false;
    }
    // Rotations about +y by the yaw (+z toward +x) and about +x by the elevation (-pitch: +z toward +y), row vectors,
    // each about its pivot: T(-p) R T(p).
    const float cy = cosf(yaw), sy = sinf(yaw), up = -pitch * _building->forwardZ, cp = cosf(up), sp = sinf(up);
    const float* p1 = _building->yawPivot;
    const float* p2 = _building->pitchPivot;
    const float ry[16] = {cy, 0, -sy, 0, 0, 1, 0, 0, sy, 0, cy, 0, 0, 0, 0, 1};
    const float rx[16] = {1, 0, 0, 0, 0, cp, -sp, 0, 0, sp, cp, 0, 0, 0, 0, 1};
    const auto aboutPivot = [](const float* rotation, const float* pivot, float* out)
    {
        memcpy(out, rotation, 64);
        for (int axis = 0; axis < 3; ++axis)
        {
            out[12 + axis] = pivot[axis] - (pivot[0] * rotation[axis] + pivot[1] * rotation[4 + axis] +
                                            pivot[2] * rotation[8 + axis]);
        }
    };
    float pitchAbout[16];
    aboutPivot(ry, p1, yawDelta);
    aboutPivot(rx, p2, pitchAbout);
    Multiply(pitchDelta, pitchAbout, yawDelta); // pitch first (in the unturned model), then the yaw
    return true;
}

// The building turret's model drawn with its turning parts posed (see BuildingTurret). Render thread.
bool DrawBuildingTurret(unsigned flags)
{
    void* model = Holster::LoadedModel(_building->modelName);
    if (!model)
    {
        static const char* missing = nullptr;
        if (missing != _building->modelName)
        {
            missing = _building->modelName;
            Log::Warning("gunner", "model %s not loaded: the game's first-person model stays", _building->modelName);
        }
        return false;
    }
    if (_hashedBuilding != _building)
    {
        const auto hashName = reinterpret_cast<HashNameFunction>(GameBuild::Absolute(GameBuild::Address::HashName));
        for (int node = 0; node < _building->nodeCount && node < MOST_TURRET_NODES; ++node)
        {
            _buildingHashes[node] = hashName(_building->nodes[node].name);
        }
        _hashedBuilding = _building;
    }
    alignas(16) static unsigned char table[BUILDING_POSE_TABLE_BYTES];
    static float matrices[MOST_TURRET_NODES][16];
    static float screenPart[16], gunPart[16], stillPart[16];
    bool hasScreen = false;
    float yawDelta[16], pitchDelta[16], building[16];
    {
        FpGuard fp;
        if (!BuildingTurretFrames(yawDelta, pitchDelta, building))
        {
            return false;
        }
        const auto init = reinterpret_cast<PoseTableInitFunction>(GameBuild::Absolute(GameBuild::Address::PoseTableInit));
        const auto set = reinterpret_cast<PoseTableSetFunction>(GameBuild::Absolute(GameBuild::Address::PoseTableSet));
        init(table);
        for (int node = 0; node < _building->nodeCount && node < MOST_TURRET_NODES; ++node)
        {
            const float* r = _building->nodes[node].rest;
            const float rest[16] = {r[0], r[1], r[2], 0, r[3], r[4], r[5], 0, r[6], r[7], r[8], 0, r[9], r[10], r[11], 1};
            Multiply(matrices[node], rest, _building->nodes[node].kind == 2 ? pitchDelta : yawDelta);
            set(table, _buildingHashes[node], matrices[node]);
        }
        // The handles pitch with the gun: design space (x right, z back) -> model (gun along +z: x left, z ahead; along
        // -z: the same as design space) -> the gun now.
        const float f = _building->forwardZ;
        const float toModel[16] = {-f, 0, 0, 0, 0, 1, 0, 0, 0, 0, -f, 0, 0, 0, 0, 1};
        float gun[16], gunWorld[16];
        Multiply(gun, toModel, pitchDelta);
        Multiply(gunWorld, gun, building);
        float turningPart[16], turningWorld[16];
        Multiply(turningPart, toModel, yawDelta);
        Multiply(turningWorld, turningPart, building);
        Multiply(stillPart, toModel, building); // a design's part 2: the tower's own frame (the Hoth tower's drum)
        const float rightGrip[3] = {-f * _building->grip[0], _building->grip[1], -f * _building->grip[2]}; // design
        if (_building->hasGrips)
        {
            PlaceGrips(
                _building->isGripOnTurning ? turningWorld : gunWorld,
                rightGrip,
                -f * _building->gripCentreX,
                _building->gripLeanDegrees);
        }
        else
        {
            _hasGripPoses = false;
        }
        // The viewscreen's picture (GetPeriscope's camera, rendered by the scope pass) on the screen face.
        if (_building->screenDesign)
        {
            memcpy(screenPart, _building->isScreenOnGun ? gunWorld : turningWorld, sizeof(screenPart));
            if (IsRailScreen())
            {
                BuildingRailScreen(yawDelta, building, gunPart); // part 1: the rail screen (the dish's handles part 0)
            }
            else
            {
                memcpy(gunPart, gunWorld, sizeof(gunPart)); // a design's part 1 (the Hoth tower's breech)
            }
            hasScreen = true;
        }
    }
    void* light = reinterpret_cast<void*>(GameBuild::Absolute(GameBuild::Address::ModelLight));
    const auto draw = (*reinterpret_cast<ModelDrawFunction**>(model))[1];
    draw(model, nullptr, building, table, light, flags | MODEL_DRAW_FLAGS, 0);
    if (hasScreen)
    {
        const float* parts[3] = {screenPart, gunPart, stillPart};
        CockpitShell::DrawParts(D3dSurvey::PresentingDevice(), parts, 3, _building->screenDesign);
    }
    static bool hasLogged = false;
    if (!hasLogged)
    {
        hasLogged = true;
        Log::Info("gunner", "building turret %s drawn as its model (%d nodes posed)", _building->odfName, _building->nodeCount);
    }
    _drawnFrame = _frame;
    _hasDrawn = true;
    return true;
}

bool DrawTurret(unsigned flags)
{
    if (_building)
    {
        return DrawBuildingTurret(flags);
    }
    if (_cabin)
    {
        return DrawCabin(flags);
    }
    if (!_station || !_isTurretOn || !_hasCapture || _captureFrame != _frame || (_boneFoundMask & 3) != 3)
    {
        return false;
    }
    FpGuard fp;
    for (int part = 0; part < 2; ++part) // the yaw and pitch bones turn every part (the barrel rides the pitch bone)
    {
        if (!IsRotation(_bones[part]))
        {
            return false;
        }
    }
    // First person only: the game camera (moved to the gunner's eye once the station shows) near the eye; in third
    // person the first-person pass must not draw the station over the scene.
    float camera[16], eye[3];
    ToWorld(_hullDrawMatrix, _station->eye, eye);
    if (!Stereo::GetGameCamera(camera) || (camera[12] - eye[0]) * (camera[12] - eye[0]) +
                                                  (camera[13] - eye[1]) * (camera[13] - eye[1]) +
                                                  (camera[14] - eye[2]) * (camera[14] - eye[2]) >
                                              FIRST_PERSON_RANGE_METERS * FIRST_PERSON_RANGE_METERS)
    {
        return false;
    }
    // Each part's pivot in the hull model, chained from the rest pivots: the yaw pivot stays, the next pivot's offset
    // from it turns with the yaw bone, the barrel's offset with the pitch bone (rotations taken as model space: the
    // rest rotations are the identity). The bones' own translations are not used: in a gunner seat in first person
    // the game writes the turret's bones with their live rotation but the translation (0, 0, -1000) (0x1964E7 ->
    // 0x6C0A0: its own way of keeping the turret out of the gunner's view; run266), and a part may turn about another
    // point than its bone's. Part = design space x (turned half way round: hull x =
    // -x, z = -z) x (minus the part's rest pivot) x the bone's rotation at its pivot x the hull's draw matrix.
    static float matrices[PART_COUNT][16];
    const float* parts[PART_COUNT];
    float place[PART_COUNT][3];
    for (int part = 0; part < PART_COUNT; ++part)
    {
        const float* rest = _station->rest[part];
        // The barrel rides the pitch bone: for the first seconds in the seat the game turns the barrel bone up to
        // 100 degrees sideways (run267; an activation animation, inferred), which our barrel on its cradle must not.
        const float* bone = _bones[part < 2 ? part : 1];
        for (int axis = 0; axis < 3; ++axis)
        {
            if (part == 0)
            {
                place[part][axis] = rest[axis];
            }
            else
            {
                const float* parentRest = _station->rest[part - 1];
                const float* parent = _bones[part - 1 < 2 ? part - 1 : 1];
                const float offset[3] = {rest[0] - parentRest[0], rest[1] - parentRest[1], rest[2] - parentRest[2]};
                place[part][axis] = place[part - 1][axis] + offset[0] * parent[axis] + offset[1] * parent[4 + axis] +
                                    offset[2] * parent[8 + axis];
            }
        }
        float placed[16];
        memcpy(placed, bone, sizeof(placed));
        placed[12] = place[part][0];
        placed[13] = place[part][1];
        placed[14] = place[part][2];
        placed[15] = 1;
        const float toBone[16] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, -rest[0], -rest[1], -rest[2], 1};
        Multiply(matrices[part], toBone, placed);
        Multiply(matrices[part], matrices[part], _hullDrawMatrix);
        parts[part] = matrices[part];
    }
    CockpitShell::DrawParts(D3dSurvey::PresentingDevice(), parts, PART_COUNT, _station->designName);
    const float rightGrip[3] = {-_station->grip[0], _station->grip[1], -_station->grip[2]}; // design space
    PlaceGrips(matrices[0], rightGrip, 0.0f, _station->gripLeanDegrees);
    _drawnFrame = _frame;
    _hasDrawn = true;
    return true;
}

bool GetHandPose(int hand, float* restPose)
{
    const Gesture::Hand& state = Gesture::CurrentHand(hand & 1);
    if (!IsShown() || !state.isTracked)
    {
        return false;
    }
    if (!_grips[hand & 1].isHeld || !_hasGripPoses)
    {
        memcpy(restPose, state.pose, 64);
        return true;
    }
    // The hand pose's origin is not the palm (on foot it is the rifle's, the palm round the pistol grip below it): the
    // pose is moved so that the arms' palm closes on the grip's axis (left hand: the mirror of the right's).
    const float* grip = _gripPoses[hand & 1];
    memcpy(restPose, grip, 64);
    float palm[3];
    if (Aim::GetPalmInHand(palm))
    {
        FpGuard fp;
        if (hand & 1)
        {
            palm[0] = -palm[0];
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            restPose[12 + axis] -= palm[0] * grip[axis] + palm[1] * grip[4 + axis] + palm[2] * grip[8 + axis];
        }
    }
    return true;
}

float GunForwardZ()
{
    return _building ? _building->forwardZ : 1.0f;
}

bool IsAimedByHand()
{
    return _building && _building->isAimedByHand && _isTurretOn;
}

bool IsHolding(int hand)
{
    return IsShown() && _grips[hand & 1].isHeld;
}

void SetTurretTest(bool isOn)
{
    _isTurretOn = isOn;
    Log::Info("gunner", "test: %s", isOn ? "our station" : "the game's turret and frame");
}

bool GetForcedEye(float* eye)
{
    return ForcedEye(eye);
}

} // namespace GunnerSeat

} // namespace SwbfVr
