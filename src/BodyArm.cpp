// BodyArm: our own first-person right arm (BodyArm.h). The data (tools/body_export.py -> build/gen/BodyArm.inc):
// bones with rest matrices in arm space (Blender's hand frame: metres, the wrist at the origin, +y toward the middle
// knuckle, +z the back of the hand; column vectors), three parts (glove, sleeve, cuff stud) with four bone weights a
// vertex, the pose library as Euler angles per bone, and per weapon the matrix from its msh model space into the hand
// bone's rest frame (it carries our scale, 0.712 of the stock model's, so the arm drawn on the game's weapon comes out
// at the stock arms' size: the weapon is drawn by the game at scale 1).
//
// Each draw: the pose's bone matrices (parents first: rest-relative x Euler), the skin matrices (posed x inverse rest),
// every vertex skinned on the CPU into a managed vertex buffer (linear blend of four), drawn with the game's own
// textures by fixed-function lighting; the world matrix is arm space -> the weapon's msh space (inverse grip) -> the
// game's model space (AXES, chosen by eye: see Configure) -> the weapon's world matrix as the game drew it.

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable (device->lpVtbl->Method(device,
// ...)), the form the rest of the mod uses. Has to be a macro: d3d9.h reads it.
#define CINTERFACE

#include "BodyArm.h"

#include "Aim.h"
#include "FpGuard.h"
#include "GameBuild.h"
#include "DrawState.h"
#include "GameMeshes.h"
#include "GameTextures.h"
#include "Holster.h"
#include "Log.h"
#include "PlayerBody.h"
#include "Textures.h"
#include "Vehicle.h"
#include "Xr.h"
#include "Zoom.h"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

namespace SwbfVr
{

namespace BodyArm
{

int CurrentSkeleton();
int ControllerPose(int hand, float triggerPull);

namespace
{

/// A bone of the arm (BodyArm.inc).
struct BodyArmBone
{
    const char* name;
    int parent;          // index of the parent bone, -1 for the root (the forearm)
    float rest[16];      // bone space -> arm space, column vectors
    const char* order;   // the Euler order of its pose angles: "XYZ" (X applied first) or "ZXY" (Z first)
    float rollShare;     // a twist bone's share of the hand's roll about its length (0 for the others)
};

/// A vertex of a part: arm space at rest.
struct BodyArmVertex
{
    float position[3];
    float normal[3];
    float uv[2];
    unsigned char bones[4];
    float weights[4]; // summing to 1
};

/// A part: one texture, triangles over its own vertices.
struct BodyArmPart
{
    const char* name;
    const char* textureName; // the game's loaded texture (Textures::FindByName)
    const BodyArmVertex* vertices;
    unsigned vertexCount;
    const unsigned short* indices;
    unsigned indexCount;
    int skeleton = 0; // BODY_ARM_SKELETONS: the skeleton its vertices' bones belong to
};

/// One bone's angles in one pose of the library (degrees, in the bone's own frame).
struct BodyArmPoseAngle
{
    int pose;
    int bone;
    float degrees[3];
};

/// A weapon we fitted the hand to: msh model space -> the hand bone's rest frame (column vectors).
struct BodyArmGrip
{
    const char* name; // the first-person model is all_1st_weap_inf_<name>
    float matrix[16];
    float axisPoint[3];     // the grip's axis in the hand (arm space): through the middle finger's curl
    float axisDirection[3]; // from the frame toward the butt (the hand's thumb to its little finger)
    int skeleton = 0;       // the skeleton whose hand bone the matrix is relative to
};

/// A skeleton of the arm (PLAN 60 step 4: the droids' hands have their own): its bones' range in BODY_ARM_BONES, one
/// table with the human skeleton first (all parents inside the range, its root the forearm and the hand its child).
/// Poses of a skeleton other than the human are labelled "<name> <group>: <label>" (FindSkeletonPose).
struct BodyArmSkeleton
{
    const char* name;
    int firstBone;
    int boneCount;
};

/// A texture of ours drawn in place of the game's texture of that name (tools/body_recolour.py: the sleeve in the
/// body's grey): A8R8G8B8, rows from the top.
struct BodyArmTexture
{
    const char* replaces;
    int width, height;
    const unsigned* pixels;
};

#if __has_include("BodyArm.inc")
#include "BodyArm.inc"
constexpr bool HAS_DATA = true;
#else
constexpr BodyArmBone BODY_ARM_BONES[1] = {};
constexpr BodyArmPart BODY_ARM_PARTS[1] = {};
constexpr BodyArmPoseAngle BODY_ARM_POSE_ANGLES[1] = {};
constexpr const char* BODY_ARM_POSE_NAMES[1] = {""};
constexpr BodyArmGrip BODY_ARM_GRIPS[1] = {};
constexpr BodyArmTexture BODY_ARM_TEXTURES[1] = {};
constexpr bool HAS_DATA = false;
#endif

constexpr int BONE_COUNT = static_cast<int>(sizeof(BODY_ARM_BONES) / sizeof(BODY_ARM_BONES[0]));
constexpr int PART_COUNT = static_cast<int>(sizeof(BODY_ARM_PARTS) / sizeof(BODY_ARM_PARTS[0]));
constexpr int GRIP_COUNT = static_cast<int>(sizeof(BODY_ARM_GRIPS) / sizeof(BODY_ARM_GRIPS[0]));
constexpr int TEXTURE_COUNT = static_cast<int>(sizeof(BODY_ARM_TEXTURES) / sizeof(BODY_ARM_TEXTURES[0]));
#if !defined(BODY_ARM_HAS_SKELETONS) // data from before the per-unit skeletons: one, the human
constexpr BodyArmSkeleton BODY_ARM_SKELETONS[] = {{"human", 0, BONE_COUNT}};
#endif
constexpr int SKELETON_COUNT = static_cast<int>(sizeof(BODY_ARM_SKELETONS) / sizeof(BODY_ARM_SKELETONS[0]));
constexpr int MAXIMUM_BONES = 64;
constexpr int MAXIMUM_SKELETONS = 4;
static_assert(SKELETON_COUNT <= MAXIMUM_SKELETONS, "more skeletons than MAXIMUM_SKELETONS");

/// The pose for each grip and the one whose index finger the trigger blends toward (tools/body_poses.py labels).
struct GripPose
{
    const char* grip;
    const char* pose;
};
constexpr GripPose GRIP_POSES[] = {
    {"pistol", "fitted: blaster pistol (fitted)"},
    {"rifle", "fitted: blaster rifle (fitted)"},
    {"thermaldetonator", "fitted: thermal detonator (fitted)"},
    {"launcher", "fitted: rocket launcher (fitted)"}, // 2026-10-31 (tools/body_gripbox.py, the stock hold)
    {"imp_rifle", "fitted: E-11 rifle (fitted)"}, // 2026-10-31: the other sides' weapons (tools/body_tunegrip.py)
    {"imp_pistol", "fitted: Imperial pistol (fitted)"},
    {"imp_sniperrifle", "fitted: Imperial sniper rifle (fitted)"},
    {"rep_rifle", "fitted: DC-15 rifle (fitted)"},
    {"rep_pistol", "fitted: DC-17 pistol (fitted)"},
    {"sniperrifle", "fitted: Rebel sniper rifle (stock-hold fit)"},
    {"rep_sniperrifle", "fitted: clone sniper rifle (fitted)"},
    {"fusioncutter", "fitted: fusion cutter (fitted)"},
    {"imp_fusioncutter", "fitted: Imperial fusion cutter (fitted)"},
    {"rep_fusioncutter", "fitted: clone fusion cutter (fitted)"},
    {"imp_launcher", "fitted: Imperial launcher (fitted)"}, // 2026-10-31: grabbed (tools/body_grab.py)
    {"rep_launcher", "fitted: clone launcher (fitted)"},
    {"imp_thermaldetonator", "fitted: thermal detonator (fitted)"},
    {"seeker", "fitted: seeker (fitted)"}, // 2026-11-01: the Rebel pilot's shotgun (tools/body_grab.py)
    // the items Holster draws in the hand (tools/body_hold.py; found through Holster::HeldItemGrip, never by model)
    {"com_grenadethermal", "fitted: grenade (fitted)"},
    {"item_landmine", "fitted: landmine (fitted)"},
    {"item_timebomb", "fitted: time bomb (fitted)"},
    {"item_powerup", "fitted: health / ammo dispenser (fitted)"},
    {"item_probedroid", "fitted: recon droid (fitted)"},
    // 2026-11-01 (PLAN 60 step 3): the Wookiee's bowcaster and mortar launcher (com_1st_weap_inf_torpedo)
    {"bowcaster", "fitted: bowcaster (fitted)"},
    {"com_torpedo", "fitted: torpedo launcher (fitted)"},
    {"item_thermaldetonator", "fitted: grenade (fitted)"}, // the thermals at Holster's size (run537: the Rebel
    {"item_imp_thermaldetonator", "fitted: grenade (fitted)"}, // vanguard's grenade is imp_1st_weap_inf_thermaldetonator
    // 2026-11-02 (PLAN 60 step 4): the B1's CIS weapons (skeleton b1; its common models and items take the labels above,
    // which FindSkeletonPose looks up as "b1 fitted: ...")
    {"cis_rifle", "fitted: E-5 rifle (stock hold)"},
    {"cis_pistol", "fitted: CIS pistol (stock hold)"},
    {"cis_sniperrifle", "fitted: CIS sniper rifle (fitted)"},
    {"cis_launcher", "fitted: CIS rocket launcher (stock hold)"},
    {"cis_fusioncutter", "fitted: CIS fusion cutter (stock hold)"}};
constexpr const char* TRIGGER_POSE = "primary: rifle / pistol grip, trigger pulled";
/// The units our arm is drawn for (idren2023 2026-10-31: hands and arms for every unit), by the player's first-person arms
/// model (its root node's name hash): the glove's and the sleeve's textures. Every human glove of the stock game is
/// painted on the one glove pattern our glove was built on (the Rebel trooper's), and these units' sleeves on the
/// trooper's sleeve-and-cuff layout. A unit not in the table keeps the stock arms (Wookiee, droids, armoured forearms
/// until they have models of their own).
struct UnitSkin
{
    const char* armsModel;
    const char* hands;
    const char* arms;
    const char* forearm = nullptr; // an armoured forearm part of its own (tools/body_armour.py) in place of our sleeve
    const char* skeleton = nullptr; // its skeleton in BODY_ARM_SKELETONS by name, the human one when null
};
constexpr UnitSkin UNIT_SKINS[] = {
    {"all_1st_trooper", "all_1st_inf_trooper_hands", "all_1st_inf_trooper_arms"},
    {"all_1st_medic", "all_1st_inf_vanguard_hands", "all_1st_inf_vanguard_arms"},
    {"all_1st_marksperson", "all_1st_inf_marksperson_hands", "all_1st_inf_marksperson_arms"},
    {"all_1st_pilot", "all_1st_inf_pilot_hands", "all_1st_inf_pilot_arms"},
    {"all_1st_hothtrooper", "all_1st_hothtrooper_hands", "all_1st_hothtrooper_arms"},
    {"all_1st_hothmarksperson", "all_1st_inf_hothmarksperson_hands", "all_1st_inf_hothmarksperson_arms"},
    {"imp_1st_pilot", "imp_1st_inf_pilot_hands", "imp_1st_inf_pilot_arms"},
    {"imp_1st_scout", "imp_1st_inf_scout_hands", "imp_1st_inf_scout_arms"},
    {"imp_1st_snowtrooper", "imp_1st_hothtrooper_hands", "imp_1st_hothtrooper_arms"},
    // armoured forearms (2026-10-31, PLAN 60 step 2): the stock plates over the undersuit, rounded, on our arm's bones;
    // the clones share the clone trooper's shape in their own colours
    {"imp_1st_stormtrooper", "imp_1st_inf_stormtrooper_hands", "imp_1st_inf_stormtrooper_arms", "forearm_imp_1st_stormtrooper"},
    {"imp_1st_darktrooper", "imp_1st_inf_darktrooper_hands", "imp_1st_inf_darktrooper_arms", "forearm_imp_1st_darktrooper"},
    {"rep_1st_trooper", "rep_1st_inf_clonetrooper_hands", "rep_1st_inf_clonetrooper_arms", "forearm_rep_1st_trooper"},
    {"rep_1st_arctrooper", "rep_1st_inf_clonetrooper_hands", "rep_1st_inf_arctrooper_arms", "forearm_rep_1st_trooper"},
    {"rep_1st_jettrooper", "rep_1st_inf_clonetrooper_hands", "rep_1st_inf_jettrooper_arms", "forearm_rep_1st_trooper"},
    {"rep_1st_pilot", "rep_1st_inf_clonetrooper_hands", "rep_1st_inf_clonepilot_arms", "forearm_rep_1st_trooper"},
    {"rep_1st_sharpshooter", "rep_1st_inf_clonetrooper_hands", "rep_1st_inf_sharpshooter_arms", "forearm_rep_1st_trooper"},
    // 2026-11-01 (PLAN 60 step 3): the Wookiee's fur forearm with its hair skirts (hair_all_1st_wookie), its hands
    // baked into our glove's layout (tools/body_unithands.py: a texture of ours by that name)
    {"all_1st_wookie", "all_1st_inf_wookie_hands", "all_1st_inf_wookie", "forearm_all_1st_wookie"},
    // 2026-11-02 (PLAN 60 step 4): the B1 battle droid's own jointed hand and rod (skeleton b1, its own texture names)
    {"cis_1st_bdroid", "cis_1st_bdroid_hands", "cis_1st_bdroid_hands", nullptr, "b1"},
    // 2026-11-03 (PLAN 60 step 4e): the super battle droid's arm of ours (skeleton sbd: forearm with its barrels, hand;
    // its texture ours, the stock one upscaled 4x, BODY_ARM_TEXTURES)
    {"cis_1st_sbdroid", "cis_1st_sbdroid_hands", "cis_1st_sbdroid_hands", nullptr, "sbd"},
};
/// The super battle droid's weapons are its forearm (its wrist blaster's model cis_weap_inf_sbdroidmuzzle, root node
/// cis_weap_inf_wristblaster, is only the hp_fire point): this grip puts that model at our barrels' muzzles, the hand
/// takes the controller's poses (ControllerPose) and the forearm keeps its line to the weapon (no wrist swing).
constexpr const char* FOREARM_WEAPON_GRIP = "cis_wristblaster";
/// The texture names the arm was exported with (the Rebel trooper's), replaced by the unit's.
constexpr const char* EXPORTED_HANDS = "all_1st_inf_trooper_hands";
constexpr const char* EXPORTED_ARMS = "all_1st_inf_trooper_arms";
/// FirstPersonRenderable: the arms model (Aim's OFFSET_firstPersonArmsModel).
constexpr uintptr_t OFFSET_firstPersonArmsModel = 0x15FC;
/// How far toward the trigger pose's index finger a full pull curls it (headset run 18: the whole way went through the
/// guns' trigger guards).
constexpr float TRIGGER_CURL_SHARE = 0.5f;
/// The texture alpha a hair part's texel needs to be drawn (the Wookiee's strands: alpha 0-255, a third under 64).
constexpr DWORD HAIR_ALPHA_REFERENCE = 0x80;
/// A weapon's barrel in its model space where it is not +z (as Aim's BARREL_CORRECTIONS, measured from the mod
/// tools' meshes): the forearm is lined up with it (and tools/body_poses.py weapon= does the same).
struct GripBarrel
{
    const char* grip;
    float axis[3];
};
constexpr GripBarrel GRIP_BARRELS[] = {{"pistol", {0.0778f, 0.0f, 0.9970f}}};

/// The weapon's msh coordinates -> the game's model space (diagonal), by Configure "axes". 0: as they are. A model
/// looks along +z and its +x is the shooter's left (Aim's turn: pose x -> model -x), so the arm's fingers point along
/// the barrel, the grip down and the back of the hand to the shooter's right. x negated (b376-b377) made it a left
/// hand (idren2023); z negated left the forearm pointing ahead (run428); x and z negated reversed it (run427).
constexpr float AXES[4][3] = {{1, 1, 1}, {-1, 1, 1}, {1, 1, -1}, {-1, 1, -1}};

constexpr DWORD VERTEX_FORMAT = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
struct Vertex
{
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

volatile bool _isOn = true;
volatile int _axes = 0;
volatile bool _shouldLog = false;
volatile bool _isWristStraight = true; // Configure "wrist 0|1"
volatile float _gripTiltDegrees = 0;
volatile int _forcedPose = -1; // Configure "pose <name>": the empty hand in that pose of the library, -1 by the trigger
constexpr const char* EMPTY_POSE = "controller: nothing held";
constexpr const char* CANISTER_POSE = "fitted: ammo canister (fitted)"; // the left hand round the reload canister
constexpr float CANISTER_MIDDLE_X = 0.0135f; // item_powerup_ammo's middle across (Holster AMMO_MODEL_CENTRE)
volatile bool _isLeftOn = true; // Configure "left 0|1": our left arm in place of the stock one
constexpr const char* EMPTY_TRIGGER_POSE = "controller: trigger only";    // Configure "tilt <degrees>": the hand about the controller's handle (x)
volatile bool _isForwardMinusZ = false; // Configure "forward": which msh z is the barrel (checked by eye)
bool _isPrepared = false;
// The last pose of each hand (0 right, 1 left), for PlayerBody's one body: arm space -> world (rows) and the bones'
// skin matrices (arm space; column matrices, read as rows they are the row form), and when.
float _lastArmToWorld[2][16];
float _lastSkin[2][MAXIMUM_BONES][16];
DWORD _lastTick[2] = {};
float _inverseRest[MAXIMUM_BONES][16];
float _restInParent[MAXIMUM_BONES][16]; // inverse(parent rest) x rest
int _triggerPose[MAXIMUM_SKELETONS] = {-1, -1, -1, -1};
int _gripPose[GRIP_COUNT];
IDirect3DDevice9* _device = nullptr;
IDirect3DVertexBuffer9* _vertexBuffers[PART_COUNT] = {};
IDirect3DIndexBuffer9* _indexBuffers[PART_COUNT] = {};
bool _isPartShown[PART_COUNT] = {}; // for the player's unit (UpdateShownParts, before each skinning)
bool _isDrawingLeft = false; // the left arm is being skinned (a skeleton with parts named *_l draws only those for it)
IDirect3DStateBlock9* _savedState = nullptr;
IDirect3DTexture9* _ownTextures[TEXTURE_COUNT] = {}; // BODY_ARM_TEXTURES on the device (managed: kept over resets)
uintptr_t _lastWeaponClass = 0;
int _lastGripSkeleton = -1;
int _lastGrip = -1;
double _costMilliseconds = 0, _costMaximumMilliseconds = 0;
int _costDraws = 0;
constexpr int COST_LOG_DRAWS = 600;

// ---- Column-vector 4x4 matrices (element [column * 4 + row]) ----

void MultiplyColumns(float* product, const float* first, const float* second)
{
    float result[16];
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            float sum = 0;
            for (int inner = 0; inner < 4; ++inner)
            {
                sum += first[inner * 4 + row] * second[column * 4 + inner];
            }
            result[column * 4 + row] = sum;
        }
    }
    memcpy(product, result, sizeof(result));
}

// An affine matrix's inverse (rotation, scale and translation; the grips carry a scale).
void InvertAffine(float* inverse, const float* matrix)
{
    const float a = matrix[0], b = matrix[4], c = matrix[8];
    const float d = matrix[1], e = matrix[5], f = matrix[9];
    const float g = matrix[2], h = matrix[6], i = matrix[10];
    const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const float scale = fabsf(determinant) > 1e-12f ? 1.0f / determinant : 0.0f;
    float result[16] = {};
    result[0] = (e * i - f * h) * scale;
    result[4] = (c * h - b * i) * scale;
    result[8] = (b * f - c * e) * scale;
    result[1] = (f * g - d * i) * scale;
    result[5] = (a * i - c * g) * scale;
    result[9] = (c * d - a * f) * scale;
    result[2] = (d * h - e * g) * scale;
    result[6] = (b * g - a * h) * scale;
    result[10] = (a * e - b * d) * scale;
    for (int row = 0; row < 3; ++row)
    {
        result[12 + row] =
            -(result[row] * matrix[12] + result[4 + row] * matrix[13] + result[8 + row] * matrix[14]);
    }
    result[15] = 1;
    memcpy(inverse, result, sizeof(result));
}

// Rotation about one axis (0 x, 1 y, 2 z), column vectors.
void AxisRotation(float* matrix, int axis, float radians)
{
    const float cosine = cosf(radians), sine = sinf(radians);
    memset(matrix, 0, 64);
    matrix[15] = 1;
    const int first = (axis + 1) % 3, second = (axis + 2) % 3;
    matrix[axis * 4 + axis] = 1;
    matrix[first * 4 + first] = cosine;
    matrix[second * 4 + second] = cosine;
    matrix[first * 4 + second] = sine;  // column first, row second
    matrix[second * 4 + first] = -sine; // column second, row first
}

// A bone's pose rotation from its Euler angles as Blender applies them: "XYZ" = Rz Ry Rx (X first), "ZXY" = Ry Rx Rz.
void EulerRotation(float* matrix, const char* order, const float* degrees)
{
    constexpr float RADIANS_PER_DEGREE = 3.14159265f / 180.0f;
    float rotation[16], step[16];
    AxisRotation(rotation, order[0] - 'X', degrees[order[0] - 'X'] * RADIANS_PER_DEGREE);
    for (int index = 1; index < 3; ++index)
    {
        const int axis = order[index] - 'X';
        AxisRotation(step, axis, degrees[axis] * RADIANS_PER_DEGREE);
        MultiplyColumns(rotation, step, rotation);
    }
    memcpy(matrix, rotation, sizeof(rotation));
}

int FindPose(const char* name)
{
    for (int pose = 0; pose < static_cast<int>(sizeof(BODY_ARM_POSE_NAMES) / sizeof(BODY_ARM_POSE_NAMES[0])); ++pose)
    {
        if (BODY_ARM_POSE_NAMES[pose] && !strcmp(BODY_ARM_POSE_NAMES[pose], name))
        {
            return pose;
        }
    }
    return -1;
}

// A pose of a skeleton by its label: the human skeleton's as it is, another's with its name before it ("b1 controller:
// nothing held"), so the same label picks each skeleton's own version.
int FindSkeletonPose(int skeleton, const char* name)
{
    if (skeleton <= 0 || skeleton >= SKELETON_COUNT)
    {
        return skeleton == 0 ? FindPose(name) : -1;
    }
    char label[160];
    snprintf(label, sizeof(label), "%s %s", BODY_ARM_SKELETONS[skeleton].name, name);
    return FindPose(label);
}

void Prepare()
{
    if (_isPrepared)
    {
        return;
    }
    _isPrepared = true;
    for (int bone = 0; bone < BONE_COUNT && bone < MAXIMUM_BONES; ++bone)
    {
        InvertAffine(_inverseRest[bone], BODY_ARM_BONES[bone].rest);
        const int parent = BODY_ARM_BONES[bone].parent;
        if (parent >= 0)
        {
            MultiplyColumns(_restInParent[bone], _inverseRest[parent], BODY_ARM_BONES[bone].rest);
        }
        else
        {
            memcpy(_restInParent[bone], BODY_ARM_BONES[bone].rest, 64);
        }
    }
    for (int skeleton = 0; skeleton < SKELETON_COUNT; ++skeleton)
    {
        _triggerPose[skeleton] = FindSkeletonPose(skeleton, TRIGGER_POSE);
    }
    for (int grip = 0; grip < GRIP_COUNT; ++grip)
    {
        _gripPose[grip] = -1;
        for (const GripPose& entry : GRIP_POSES)
        {
            if (BODY_ARM_GRIPS[grip].name && !strcmp(entry.grip, BODY_ARM_GRIPS[grip].name))
            {
                _gripPose[grip] = FindSkeletonPose(BODY_ARM_GRIPS[grip].skeleton, entry.pose);
            }
        }
    }
    int vertexTotal = 0, triangleTotal = 0;
    for (const BodyArmPart& part : BODY_ARM_PARTS)
    {
        vertexTotal += static_cast<int>(part.vertexCount);
        triangleTotal += static_cast<int>(part.indexCount / 3);
    }
    Log::Info(
        "bodyarm",
        "arm data: %d bones in %d skeletons, %d parts, %d vertices, %d triangles, %d grips, trigger pose %d",
        BONE_COUNT,
        SKELETON_COUNT,
        PART_COUNT,
        vertexTotal,
        triangleTotal,
        GRIP_COUNT,
        _triggerPose[0]);
}

// A grip for an item Holster draws in the hand at the item's size (item_, and the grenade's com_grenadethermal: the
// grip divides that size out), found only through Holster::HeldItemGrip.
bool IsItemGrip(const char* gripName)
{
    return !strncmp(gripName, "item_", 5) || !strcmp(gripName, "com_grenadethermal");
}

// The grip for a weapon class: its first-person model is all_1st_weap_inf_<name> (or that name with _mesh), as
// Zoom::IsCisWeapon tells the CIS side's models; a grip named <side>_<name> (imp_rifle) is <side>_1st_weap_inf_<name>.
// An item Holster draws in the hand takes the grip Holster names (not cached: the same class has no grip in the game's
// own draws): an item_ grip by that name alone, the grenade's also by its model's node.
int FindGrip(uintptr_t weaponClass)
{
    if (!weaponClass)
    {
        return -1;
    }
    // only the grips fitted to the player's skeleton's hand
    const int skeleton = CurrentSkeleton();
    const char* itemGrip = Holster::HeldItemGrip(weaponClass);
    if (itemGrip && !strncmp(itemGrip, "item_", 5))
    {
        for (int grip = 0; grip < GRIP_COUNT; ++grip)
        {
            if (BODY_ARM_GRIPS[grip].name && BODY_ARM_GRIPS[grip].skeleton == skeleton &&
                !strcmp(BODY_ARM_GRIPS[grip].name, itemGrip))
            {
                return grip;
            }
        }
        return -1;
    }
    if (weaponClass == _lastWeaponClass && skeleton == _lastGripSkeleton && !itemGrip)
    {
        return _lastGrip;
    }
    // A skeleton whose weapons are its forearm (the super battle droid: blaster, tri-shot, rocket; only the blaster
    // has a model, the others none): every weapon takes that grip (idren2023 2026-11-04: all fire from our barrels).
    for (int grip = 0; grip < GRIP_COUNT && skeleton > 0; ++grip)
    {
        if (BODY_ARM_GRIPS[grip].name && BODY_ARM_GRIPS[grip].skeleton == skeleton &&
            !strcmp(BODY_ARM_GRIPS[grip].name, FOREARM_WEAPON_GRIP))
        {
            if (weaponClass != _lastWeaponClass || skeleton != _lastGripSkeleton)
            {
                Log::Info("bodyarm", "weapon class %08X: %s (the forearm)", static_cast<unsigned>(weaponClass),
                          FOREARM_WEAPON_GRIP);
            }
            _lastWeaponClass = weaponClass;
            _lastGripSkeleton = skeleton;
            _lastGrip = grip;
            return grip;
        }
    }
    using HashNameFunction = unsigned(__cdecl*)(const char*);
    const auto hashName = reinterpret_cast<HashNameFunction>(GameBuild::Absolute(GameBuild::Address::HashName));
    int found = -1;
    for (int grip = 0; grip < GRIP_COUNT && found < 0 && hashName; ++grip)
    {
        // the model's root node: its name, or with "_mesh" / "_model" (the clone pistol), or "1st__" (the clone rifle),
        // or without "inf_" (the CIS launcher: cis_1st_weap_launcher, run550)
        const char* gripName = BODY_ARM_GRIPS[grip].name ? BODY_ARM_GRIPS[grip].name : "";
        if ((IsItemGrip(gripName) && (!itemGrip || strcmp(gripName, itemGrip))) ||
            BODY_ARM_GRIPS[grip].skeleton != skeleton)
        {
            continue;
        }
        const bool isOtherSide = strlen(gripName) > 4 && gripName[3] == '_';
        const char* side = isOtherSide ? gripName : "all";
        const char* weapon = gripName + (isOtherSide ? 4 : 0);
        const char* const forms[] = {
            "%.3s_1st_weap_inf_%s", "%.3s_1st_weap_inf_%s_mesh", "%.3s_1st_weap_inf_%s_model", "%.3s_1st__weap_inf_%s",
            "%.3s_1st_weap_%s", "%.3s_weap_inf_%s"};  // (the last: the super battle droid's cis_weap_inf_wristblaster)
        for (const char* form : forms)
        {
            char name[80];
            snprintf(name, sizeof(name), form, side, weapon);
            found = found < 0 && Zoom::HasFirstPersonNode(weaponClass, hashName(name)) ? grip : found;
        }
    }
    if (itemGrip && found >= 0 && !IsItemGrip(BODY_ARM_GRIPS[found].name))
    {
        // a grenade whose model is a thermal detonator (run537): its grip at Holster's size, item_<grip>
        char itemName[48];
        snprintf(itemName, sizeof(itemName), "item_%s", BODY_ARM_GRIPS[found].name);
        found = -1;
        for (int grip = 0; grip < GRIP_COUNT && found < 0; ++grip)
        {
            found = BODY_ARM_GRIPS[grip].name && BODY_ARM_GRIPS[grip].skeleton == skeleton &&
                            !strcmp(BODY_ARM_GRIPS[grip].name, itemName)
                        ? grip
                        : -1;
        }
    }
    // (else the grip Holster names, on this skeleton: the common grenade's root node is imp_1st_weap_inf_thermaldetonator,
    // whose grips only the human hand has; run552, the B1 holds com_grenadethermal)
    for (int grip = 0; itemGrip && found < 0 && grip < GRIP_COUNT; ++grip)
    {
        found = BODY_ARM_GRIPS[grip].name && BODY_ARM_GRIPS[grip].skeleton == skeleton &&
                        !strcmp(BODY_ARM_GRIPS[grip].name, itemGrip)
                    ? grip
                    : -1;
    }
    if (itemGrip)
    {
        return found;
    }
    _lastWeaponClass = weaponClass;
    _lastGripSkeleton = skeleton;
    _lastGrip = found;
    Log::Info(
        "bodyarm",
        "weapon class %08X: %s",
        static_cast<unsigned>(weaponClass),
        found >= 0 ? BODY_ARM_GRIPS[found].name : "no grip of ours (stock arm)");
    return found;
}

// The rotation (column vectors, 4x4 about the origin) turning the unit vector from onto the unit vector to.
void RotationBetween(float* matrix, const float* from, const float* to)
{
    const float axis[3] = {from[1] * to[2] - from[2] * to[1], from[2] * to[0] - from[0] * to[2],
                           from[0] * to[1] - from[1] * to[0]};
    const float sine = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    const float cosine = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
    memset(matrix, 0, 64);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1;
    if (sine < 1e-6f)
    {
        return;
    }
    const float x = axis[0] / sine, y = axis[1] / sine, z = axis[2] / sine, t = 1 - cosine;
    matrix[0] = t * x * x + cosine;
    matrix[1] = t * x * y + sine * z;
    matrix[2] = t * x * z - sine * y;
    matrix[4] = t * x * y - sine * z;
    matrix[5] = t * y * y + cosine;
    matrix[6] = t * y * z + sine * x;
    matrix[8] = t * x * z + sine * y;
    matrix[9] = t * y * z - sine * x;
    matrix[10] = t * z * z + cosine;
}

// The posed bones' skin matrices (posed x inverse rest, arm space) for a pose, the index finger blended toward the
// trigger pose by the pull. forearmToward, when given (arm space, unit): the forearm swung about the wrist so that it
// runs from the elbow along it, the hand kept where it is (the stock rest has the wrist bent back about 11 degrees
// and turned 9: behind a pistol the forearm then angled off to the side, idren2023 2026-10-23).
void ComputeSkin(float (*skin)[16], int pose, float triggerPull, const float* forearmToward)
{
    float angles[MAXIMUM_BONES][3] = {};
    for (const BodyArmPoseAngle& angle : BODY_ARM_POSE_ANGLES)
    {
        if (angle.pose == pose && angle.bone >= 0 && angle.bone < MAXIMUM_BONES)
        {
            memcpy(angles[angle.bone], angle.degrees, sizeof(angles[0]));
        }
    }
    const int skeleton = CurrentSkeleton();
    const int triggerPose = skeleton >= 0 && skeleton < SKELETON_COUNT ? _triggerPose[skeleton] : -1;
    if (triggerPull > 0 && triggerPose >= 0)
    {
        for (const BodyArmPoseAngle& angle : BODY_ARM_POSE_ANGLES)
        {
            if (angle.pose == triggerPose && angle.bone >= 0 && angle.bone < MAXIMUM_BONES &&
                (!strncmp(BODY_ARM_BONES[angle.bone].name, "index", 5) ||
                 !strncmp(BODY_ARM_BONES[angle.bone].name, "trigger", 7)))
            {
                for (int axis = 0; axis < 3; ++axis)
                {
                    angles[angle.bone][axis] +=
                        (angle.degrees[axis] - angles[angle.bone][axis]) * triggerPull * TRIGGER_CURL_SHARE;
                }
            }
        }
    }
    float posed[MAXIMUM_BONES][16];
    for (int bone = 0; bone < BONE_COUNT && bone < MAXIMUM_BONES; ++bone)
    {
        float rotation[16], local[16];
        EulerRotation(rotation, BODY_ARM_BONES[bone].order, angles[bone]);
        MultiplyColumns(local, _restInParent[bone], rotation);
        const int parent = BODY_ARM_BONES[bone].parent;
        if (parent >= 0)
        {
            MultiplyColumns(posed[bone], posed[parent], local);
        }
        else
        {
            memcpy(posed[bone], local, sizeof(local));
        }
        if (forearmToward && parent < 0)
        {
            // the root (the forearm, elbow to wrist): turned about its tail, the wrist (its rest y axis's end)
            const float* rest = BODY_ARM_BONES[bone].rest;
            const float length = sqrtf(rest[12] * rest[12] + rest[13] * rest[13] + rest[14] * rest[14]);
            const float along[3] = {-rest[12] / length, -rest[13] / length, -rest[14] / length};
            float turn[16], moved[16];
            RotationBetween(turn, along, forearmToward);
            // about the wrist at the origin of arm space (the hand bone's head): a rotation about the origin
            MultiplyColumns(moved, turn, posed[bone]);
            memcpy(posed[bone], moved, sizeof(moved));
        }
        else if (forearmToward && parent >= 0 && BODY_ARM_BONES[parent].parent < 0 &&
                 !strcmp(BODY_ARM_BONES[bone].name, "hand"))
        {
            memcpy(posed[bone], BODY_ARM_BONES[bone].rest, 64); // the hand stays on the grip
            float rotation[16];
            EulerRotation(rotation, BODY_ARM_BONES[bone].order, angles[bone]);
            MultiplyColumns(posed[bone], posed[bone], rotation);
        }
        MultiplyColumns(skin[bone], posed[bone], _inverseRest[bone]);
    }
}

void ReleaseBuffers()
{
    for (IDirect3DTexture9*& texture : _ownTextures)
    {
        if (texture)
        {
            texture->lpVtbl->Release(texture);
            texture = nullptr;
        }
    }
    for (int part = 0; part < PART_COUNT; ++part)
    {
        if (_vertexBuffers[part])
        {
            _vertexBuffers[part]->lpVtbl->Release(_vertexBuffers[part]);
            _vertexBuffers[part] = nullptr;
        }
        if (_indexBuffers[part])
        {
            _indexBuffers[part]->lpVtbl->Release(_indexBuffers[part]);
            _indexBuffers[part] = nullptr;
        }
    }
}

bool CreateBuffers(IDirect3DDevice9* device)
{
    for (int part = 0; part < PART_COUNT; ++part)
    {
        const BodyArmPart& data = BODY_ARM_PARTS[part];
        if (_vertexBuffers[part])
        {
            continue;
        }
        if (FAILED(device->lpVtbl->CreateVertexBuffer(
                device,
                data.vertexCount * sizeof(Vertex),
                D3DUSAGE_WRITEONLY,
                VERTEX_FORMAT,
                D3DPOOL_MANAGED,
                &_vertexBuffers[part],
                nullptr)) ||
            FAILED(device->lpVtbl->CreateIndexBuffer(
                device,
                data.indexCount * sizeof(unsigned short),
                D3DUSAGE_WRITEONLY,
                D3DFMT_INDEX16,
                D3DPOOL_MANAGED,
                &_indexBuffers[part],
                nullptr)))
        {
            ReleaseBuffers();
            Log::Warning("bodyarm", "arm buffers not created: arm not drawn");
            return false;
        }
        void* indices = nullptr;
        if (FAILED(_indexBuffers[part]->lpVtbl->Lock(_indexBuffers[part], 0, 0, &indices, 0)))
        {
            ReleaseBuffers();
            return false;
        }
        memcpy(indices, data.indices, data.indexCount * sizeof(unsigned short));
        _indexBuffers[part]->lpVtbl->Unlock(_indexBuffers[part]);
    }
    return true;
}

// Every shown part's vertices skinned into its vertex buffer (arm space).
bool Skin(const float (*skin)[16])
{
    for (int part = 0; part < PART_COUNT; ++part)
    {
        const BodyArmPart& data = BODY_ARM_PARTS[part];
        if (!_isPartShown[part])
        {
            continue;
        }
        Vertex* out = nullptr;
        if (FAILED(_vertexBuffers[part]->lpVtbl->Lock(_vertexBuffers[part], 0, 0, reinterpret_cast<void**>(&out), 0)))
        {
            return false;
        }
        for (unsigned index = 0; index < data.vertexCount; ++index)
        {
            const BodyArmVertex& vertex = data.vertices[index];
            float position[3] = {}, normal[3] = {};
            for (int influence = 0; influence < 4; ++influence)
            {
                const float weight = vertex.weights[influence];
                if (weight <= 0)
                {
                    continue;
                }
                const float* matrix = skin[vertex.bones[influence]];
                for (int row = 0; row < 3; ++row)
                {
                    position[row] += weight * (matrix[row] * vertex.position[0] + matrix[4 + row] * vertex.position[1] +
                                               matrix[8 + row] * vertex.position[2] + matrix[12 + row]);
                    normal[row] += weight * (matrix[row] * vertex.normal[0] + matrix[4 + row] * vertex.normal[1] +
                                             matrix[8 + row] * vertex.normal[2]);
                }
            }
            out[index] = {
                position[0],
                position[1],
                position[2],
                normal[0],
                normal[1],
                normal[2],
                vertex.uv[0],
                vertex.uv[1]};
        }
        _vertexBuffers[part]->lpVtbl->Unlock(_vertexBuffers[part]);
    }
    return true;
}

void LogMatrix(const char* name, const float* matrix)
{
    Log::Info(
        "bodyarm",
        "%s: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
        name,
        static_cast<double>(matrix[0]),
        static_cast<double>(matrix[1]),
        static_cast<double>(matrix[2]),
        static_cast<double>(matrix[3]),
        static_cast<double>(matrix[4]),
        static_cast<double>(matrix[5]),
        static_cast<double>(matrix[6]),
        static_cast<double>(matrix[7]),
        static_cast<double>(matrix[8]),
        static_cast<double>(matrix[9]),
        static_cast<double>(matrix[10]),
        static_cast<double>(matrix[11]),
        static_cast<double>(matrix[12]),
        static_cast<double>(matrix[13]),
        static_cast<double>(matrix[14]),
        static_cast<double>(matrix[15]));
}

} // namespace

// Arm space -> the controller's grip pose frame (columns) for a grip: the grip's axis on the handle's axis (see
// GetWeaponInGrip), the tilt of Configure "tilt" included.
static void ArmToGrip(int grip, float* result)
{
    const BodyArmGrip& data = BODY_ARM_GRIPS[grip];
    // The grip pose's axes in arm space (OpenXR: -z along the handle toward the thumb, +x to the user's right, for
    // the right hand out of the back of the hand, y = z x x): z the grip's axis (toward the little finger), y the
    // hand's knuckles-to-wrist direction square to it, x = y x z (the back of the hand).
    float z[3], y[3] = {0, -1, 0}, x[3];
    const float zLength = sqrtf(data.axisDirection[0] * data.axisDirection[0] +
                                data.axisDirection[1] * data.axisDirection[1] +
                                data.axisDirection[2] * data.axisDirection[2]);
    for (int axis = 0; axis < 3; ++axis)
    {
        z[axis] = data.axisDirection[axis] / zLength;
    }
    const float along = y[0] * z[0] + y[1] * z[1] + y[2] * z[2];
    float yLength = 0;
    for (int axis = 0; axis < 3; ++axis)
    {
        y[axis] -= z[axis] * along;
        yLength += y[axis] * y[axis];
    }
    yLength = sqrtf(yLength);
    for (float& value : y)
    {
        value /= yLength;
    }
    x[0] = y[1] * z[2] - y[2] * z[1];
    x[1] = y[2] * z[0] - y[0] * z[2];
    x[2] = y[0] * z[1] - y[1] * z[0];
    // arm space -> the grip pose's frame (columns): rotate onto those axes, scale to the drawn size (the grips carry
    // our 0.712; the arm is drawn at the weapon's stock scale), the axis point to the pose's origin, then the tilt
    // about the pose's x
    const float scale = 1.0f / 0.7116788f;
    float armToGrip[16] = {};
    for (int column = 0; column < 3; ++column)
    {
        armToGrip[column * 4 + 0] = x[column] * scale;
        armToGrip[column * 4 + 1] = y[column] * scale;
        armToGrip[column * 4 + 2] = z[column] * scale;
    }
    for (int row = 0; row < 3; ++row)
    {
        armToGrip[12 + row] = -(armToGrip[row] * data.axisPoint[0] + armToGrip[4 + row] * data.axisPoint[1] +
                                armToGrip[8 + row] * data.axisPoint[2]);
    }
    armToGrip[15] = 1;
    float tilt[16];
    AxisRotation(tilt, 0, _gripTiltDegrees * 3.14159265f / 180.0f);
    MultiplyColumns(result, tilt, armToGrip);
}

// Row-major 4x4 product (the game's row-vector convention): a then b.
static void MultiplyRows(float* product, const float* a, const float* b)
{
    float result[16];
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float sum = 0;
            for (int inner = 0; inner < 4; ++inner)
            {
                sum += a[row * 4 + inner] * b[inner * 4 + column];
            }
            result[row * 4 + column] = sum;
        }
    }
    memcpy(product, result, sizeof(result));
}

// Arm space -> world on a controller's grip pose (rows): round the rifle's handle as the closest fit; the left arm
// mirrored across the grip frame's x. FpGuard held.
static void ArmToGripWorld(const float* gripWorld, bool isLeft, float* world)
{
    int handle = 0;
    for (int grip = 0; grip < GRIP_COUNT; ++grip)
    {
        handle = BODY_ARM_GRIPS[grip].name && !strcmp(BODY_ARM_GRIPS[grip].name, "rifle") ? grip : handle;
    }
    float armToGrip[16]; // columns stored [column * 4 + row] read as rows: the same map for row vectors
    ArmToGrip(handle, armToGrip);
    if (isLeft)
    {
        const float mirror[16] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        MultiplyRows(armToGrip, armToGrip, mirror);
    }
    MultiplyRows(world, armToGrip, gripWorld);
}

bool GetWeaponInGrip(uintptr_t weaponClass, float* weaponInGrip)
{
    if (!HAS_DATA || !_isOn)
    {
        return false;
    }
    Prepare();
    const int grip = FindGrip(weaponClass);
    if (grip < 0)
    {
        return false;
    }
    FpGuard fp; // RAII: a clean floating-point state for our arithmetic, the game's restored at the end
    float tilted[16], toModel[16] = {}, modelToArm[16], modelToGrip[16];
    ArmToGrip(grip, tilted);
    // the game's model space -> msh (AXES is a diagonal of +-1: its own inverse) -> arm (the grip)
    const int axes = _axes & 3;
    toModel[0] = AXES[axes][0];
    toModel[5] = AXES[axes][1];
    toModel[10] = AXES[axes][2];
    toModel[15] = 1;
    MultiplyColumns(modelToArm, BODY_ARM_GRIPS[grip].matrix, toModel);
    MultiplyColumns(modelToGrip, tilted, modelToArm);
    // a column matrix stored [column * 4 + row] read as rows is the same map for row vectors (the game's convention)
    memcpy(weaponInGrip, modelToGrip, 64);
    return true;
}

bool GetGripPointInModel(uintptr_t weaponClass, float* point)
{
    if (!HAS_DATA || !_isOn)
    {
        return false;
    }
    Prepare();
    const int grip = FindGrip(weaponClass);
    if (grip < 0)
    {
        return false;
    }
    FpGuard fp; // RAII: a clean floating-point state for our arithmetic, the game's restored at the end
    // the grip's axis point (arm space) -> the weapon's msh space (inverse grip) -> the game's model space (AXES)
    float inverseGrip[16];
    InvertAffine(inverseGrip, BODY_ARM_GRIPS[grip].matrix);
    const float* armPoint = BODY_ARM_GRIPS[grip].axisPoint;
    const int axes = _axes & 3;
    for (int row = 0; row < 3; ++row)
    {
        point[row] = AXES[axes][row] * (inverseGrip[row] * armPoint[0] + inverseGrip[4 + row] * armPoint[1] +
                                        inverseGrip[8 + row] * armPoint[2] + inverseGrip[12 + row]);
    }
    return true;
}

float GripTiltDegrees()
{
    return _gripTiltDegrees;
}

// A unit's hair skirts round its forearm and fur cards on the back of its hand and fingers (the Wookiee's: strands in
// the texture's alpha; tools/body_armour.py, tools/body_furcards.py), drawn alpha-tested.
bool IsHairPart(const char* partName)
{
    return !strncmp(partName, "hair_", 5);
}

// The player's unit in UNIT_SKINS by the first-person arms model now in the renderable (cached by its pointer), or -1.
int CurrentUnit()
{
    static uintptr_t cachedModel = 0;
    static int cachedUnit = -1;
    const uintptr_t renderables = GameBuild::Absolute(GameBuild::Address::FirstPersonRenderables);
    const uintptr_t renderable =
        renderables && !IsBadReadPtr(reinterpret_cast<void*>(renderables), 4) ? *reinterpret_cast<uintptr_t*>(renderables) : 0;
    if (!renderable || IsBadReadPtr(reinterpret_cast<void*>(renderable + OFFSET_firstPersonArmsModel), 4))
    {
        return -1;
    }
    const uintptr_t model = *reinterpret_cast<uintptr_t*>(renderable + OFFSET_firstPersonArmsModel);
    if (model == cachedModel)
    {
        return cachedUnit;
    }
    cachedModel = model;
    cachedUnit = -1;
    // The arms model's own nodes are dummyroot and its bones, the same for every unit (run521): the unit is found by
    // looking each candidate up in the game's model registry (ModelByHash, keyed by the name's FNV-1a with each byte
    // | 0x20) and comparing the pointer.
    if (GameBuild::IsAvailable(GameBuild::Feature::Holsters))
    {
        using ModelByHashFunction = void*(__cdecl*)(unsigned);
        const auto byHash = reinterpret_cast<ModelByHashFunction>(GameBuild::Absolute(GameBuild::Address::ModelByHash));
        for (int unit = 0; byHash && unit < static_cast<int>(sizeof(UNIT_SKINS) / sizeof(UNIT_SKINS[0])); ++unit)
        {
            unsigned hash = 0x811C9DC5u;
            for (const char* character = UNIT_SKINS[unit].armsModel; *character != 0; ++character)
            {
                hash = (hash ^ (static_cast<unsigned char>(*character) | 0x20u)) * 0x01000193u;
            }
            if (reinterpret_cast<uintptr_t>(byHash(hash)) == model)
            {
                cachedUnit = unit;
            }
        }
    }
    // In a vehicle the renderable holds the seat's model, not the arms (PLAN 61 j: our hands on the bike grips): the
    // unit last seen on foot.
    static int footUnit = -1;
    const Vehicle::Seat seat = Vehicle::CurrentSeat();
    const bool isInVehicle = seat != Vehicle::Seat::OnFoot && seat != Vehicle::Seat::None;
    if (cachedUnit >= 0)
    {
        footUnit = cachedUnit;
    }
    else if (isInVehicle && footUnit >= 0)
    {
        cachedUnit = footUnit;
        Log::Info(
            "bodyarm",
            "first-person model %08X in a vehicle: the unit on foot, %s",
            static_cast<unsigned>(model),
            UNIT_SKINS[cachedUnit].armsModel);
        return cachedUnit;
    }
    Log::Info(
        "bodyarm",
        "first-person arms model %08X: %s",
        static_cast<unsigned>(model),
        cachedUnit >= 0 ? UNIT_SKINS[cachedUnit].armsModel : "no skin of ours (stock arms)");
    return cachedUnit;
}

// The player's unit's skeleton in BODY_ARM_SKELETONS, or -1 (no unit of ours, or its skeleton not in the data: the
// stock arms).
int CurrentSkeleton()
{
    const int unit = CurrentUnit();
    if (unit < 0 || !UNIT_SKINS[unit].skeleton)
    {
        return unit < 0 ? -1 : 0;
    }
    for (int skeleton = 0; skeleton < SKELETON_COUNT; ++skeleton)
    {
        if (BODY_ARM_SKELETONS[skeleton].name && !strcmp(BODY_ARM_SKELETONS[skeleton].name, UNIT_SKINS[unit].skeleton))
        {
            return skeleton;
        }
    }
    return -1;
}

// A part's texture for the player's unit: the exported trooper's glove and sleeve swapped for the unit's.
const char* SkinTexture(const char* exported)
{
    const int unit = CurrentUnit();
    if (unit < 0)
    {
        return exported;
    }
    return !strcmp(exported, EXPORTED_HANDS) ? UNIT_SKINS[unit].hands
           : !strcmp(exported, EXPORTED_ARMS) ? UNIT_SKINS[unit].arms
                                              : exported;
}

// Whether a part is drawn for the player's unit: our sleeve (and its stud) unless the unit has a forearm of its own,
// that forearm, and the glove always.
bool IsPartShown(int part)
{
    if (BODY_ARM_PARTS[part].skeleton != CurrentSkeleton())
    {
        return false;
    }
    const char* partName = BODY_ARM_PARTS[part].name;
    // a left arm of its own (the super battle droid's: no guns): *_l parts for the left hand only, the rest not for it
    const size_t nameLength = strlen(partName);
    const bool isLeftPart = nameLength > 2 && !strcmp(partName + nameLength - 2, "_l");
    bool hasLeftParts = false;
    for (int other = 0; other < PART_COUNT && !hasLeftParts; ++other)
    {
        const size_t length = strlen(BODY_ARM_PARTS[other].name);
        hasLeftParts = BODY_ARM_PARTS[other].skeleton == BODY_ARM_PARTS[part].skeleton && length > 2 &&
                       !strcmp(BODY_ARM_PARTS[other].name + length - 2, "_l");
    }
    if (hasLeftParts)
    {
        return isLeftPart == _isDrawingLeft;
    }
    const int unit = CurrentUnit();
    const char* forearm = unit >= 0 ? UNIT_SKINS[unit].forearm : nullptr;
    if (!strncmp(partName, "forearm_", 8))
    {
        return forearm && !strcmp(partName, forearm);
    }
    if (IsHairPart(partName)) // hair_<x> (the forearm's skirts) and hair_<x>_hand (fur cards on the hand) with forearm_<x>
    {
        const size_t length = forearm ? strlen(forearm + 8) : 0;
        return forearm && !strncmp(partName + 5, forearm + 8, length) &&
               (!partName[5 + length] || !strcmp(partName + 5 + length, "_hand"));
    }
    return !forearm || strncmp(partName, "r_forearm", 9) != 0;
}

void UpdateShownParts()
{
    for (int part = 0; part < PART_COUNT; ++part)
    {
        _isPartShown[part] = IsPartShown(part);
    }
}

bool IsWanted(uintptr_t weaponClass)
{
    if (!HAS_DATA || !_isOn || CurrentSkeleton() < 0)
    {
        return false;
    }
    Prepare();
    return FindGrip(weaponClass) >= 0;
}

// Keeps a hand's pose for PlayerBody (GetHandPose).
static void Remember(int hand, const float* world, const float (*skin)[16])
{
    memcpy(_lastArmToWorld[hand], world, sizeof(_lastArmToWorld[hand]));
    memcpy(_lastSkin[hand], skin, sizeof(_lastSkin[hand]));
    _lastTick[hand] = GetTickCount();
}

// The skinned parts drawn with the world matrix (rows) into the current eye image; the cost logged now and then.
// Our texture in place of the game's of this name (BODY_ARM_TEXTURES), made on the device at its first use with a
// box-filtered mip chain; null when there is none (or it could not be made).
static IDirect3DTexture9* OwnTexture(IDirect3DDevice9* d3dDevice, const char* gameName)
{
    for (int index = 0; index < TEXTURE_COUNT; ++index)
    {
        const BodyArmTexture& data = BODY_ARM_TEXTURES[index];
        if (!data.replaces || !gameName || strcmp(data.replaces, gameName))
        {
            continue;
        }
        if (_ownTextures[index])
        {
            return _ownTextures[index];
        }
        // the pixels made at start from the player's own game texture (GameTextures; not compiled in)
        char table[32];
        snprintf(table, sizeof(table), "BODY_ARM_TEXTURE_%d", index);
        int madeWidth = 0, madeHeight = 0;
        const unsigned* pixels = GameTextures::Pixels(table, madeWidth, madeHeight);
        if (!pixels || madeWidth != data.width || madeHeight != data.height)
        {
            return nullptr;
        }
        auto* const texture = static_cast<IDirect3DTexture9*>(Textures::MakeOwn(d3dDevice, pixels, data.width, data.height));
        if (!texture)
        {
            return nullptr;
        }
        const DWORD levels = texture->lpVtbl->GetLevelCount(texture);
        _ownTextures[index] = texture;
        Log::Info(
            "bodyarm",
            "texture %s drawn as ours (%d x %d, %lu levels)",
            gameName,
            data.width,
            data.height,
            static_cast<unsigned long>(levels));
        return texture;
    }
    return nullptr;
}

static void Render(IDirect3DDevice9* d3dDevice, const float* world, LARGE_INTEGER start, const char* label)
{
    LARGE_INTEGER end, frequency;
    if (!_savedState && FAILED(d3dDevice->lpVtbl->CreateStateBlock(d3dDevice, D3DSBT_ALL, &_savedState)))
    {
        _savedState = nullptr;
        return;
    }
    if (FAILED(_savedState->lpVtbl->Capture(_savedState)))
    {
        return;
    }
    DrawState::Reset(d3dDevice); // a known state, nothing inherited from the game's last draw
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetFVF(d3dDevice, VERTEX_FORMAT);
    D3DMATRIX worldMatrix;
    memcpy(&worldMatrix, world, sizeof(worldMatrix));
    d3dDevice->lpVtbl->SetTransform(d3dDevice, D3DTS_WORLD, &worldMatrix);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_TEXCOORDINDEX, 0);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetTextureStageState(d3dDevice, 1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
    d3dDevice->lpVtbl->SetSamplerState(d3dDevice, 0, D3DSAMP_SRGBTEXTURE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_LIGHTING, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_AMBIENT, 0xFF8C8C8C);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SPECULARENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_NORMALIZENORMALS, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_COLORVERTEX, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_FOGENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZENABLE, D3DZB_TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ZWRITEENABLE, TRUE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHATESTENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_STENCILENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_CULLMODE, D3DCULL_NONE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHABLENDENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_SRGBWRITEENABLE, FALSE);
    d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_COLORWRITEENABLE, 0xF);
    // One light from above and in front of the world (the game's sun is not read yet), plus the ambient; the texture
    // at its own brightness (run430: doubled, the sleeve shone white beside the stock left arm).
    D3DLIGHT9 light = {};
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse = {0.5f, 0.5f, 0.48f, 1};
    light.Direction = {0.3f, -0.8f, 0.5f};
    d3dDevice->lpVtbl->SetLight(d3dDevice, 0, &light);
    d3dDevice->lpVtbl->LightEnable(d3dDevice, 0, TRUE);
    for (int other = 1; other < 8; ++other)
    {
        d3dDevice->lpVtbl->LightEnable(d3dDevice, other, FALSE);
    }
    D3DMATERIAL9 material = {};
    material.Diffuse = material.Ambient = {1, 1, 1, 1};
    d3dDevice->lpVtbl->SetMaterial(d3dDevice, &material);
    int triangles = 0;
    for (int part = 0; part < PART_COUNT; ++part)
    {
        const BodyArmPart& data = BODY_ARM_PARTS[part];
        if (!_isPartShown[part])
        {
            continue;
        }
        // a unit's own forearm in that unit's arms texture (the clones share one shape); hair in our copy of it with
        // its alpha (the game's has none usable: run539 drew the skirts solid)
        const bool isHair = IsHairPart(data.name);
        const char* textureName = isHair ? data.textureName
                                  : !strncmp(data.name, "forearm_", 8) && CurrentUnit() >= 0
                                      ? UNIT_SKINS[CurrentUnit()].arms
                                      : SkinTexture(data.textureName);
        IDirect3DTexture9* texture = OwnTexture(d3dDevice, textureName);
        texture = texture ? texture : static_cast<IDirect3DTexture9*>(Textures::FindByName(textureName));
        d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(texture));
        d3dDevice->lpVtbl
            ->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, texture ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
        d3dDevice->lpVtbl->SetTextureStageState(
            d3dDevice, 0, D3DTSS_ALPHAARG1, isHair && texture ? D3DTA_TEXTURE : D3DTA_DIFFUSE);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHATESTENABLE, isHair && texture);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHAREF, HAIR_ALPHA_REFERENCE);
        d3dDevice->lpVtbl->SetRenderState(d3dDevice, D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
        d3dDevice->lpVtbl->SetStreamSource(d3dDevice, 0, _vertexBuffers[part], 0, sizeof(Vertex));
        d3dDevice->lpVtbl->SetIndices(d3dDevice, _indexBuffers[part]);
        d3dDevice->lpVtbl->DrawIndexedPrimitive(
            d3dDevice,
            D3DPT_TRIANGLELIST,
            0,
            0,
            data.vertexCount,
            0,
            data.indexCount / 3);
        triangles += static_cast<int>(data.indexCount / 3);
    }
    _savedState->lpVtbl->Apply(_savedState);
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);
    const double milliseconds =
        static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    _costMilliseconds += milliseconds;
    _costMaximumMilliseconds = milliseconds > _costMaximumMilliseconds ? milliseconds : _costMaximumMilliseconds;
    if (++_costDraws >= COST_LOG_DRAWS)
    {
        Log::Info(
            "bodyarm",
            "arm draw cost: %.3f ms per eye pass on average, %.3f at most (%d triangles, skinned on the CPU, %s)",
            _costMilliseconds / _costDraws,
            _costMaximumMilliseconds,
            triangles,
            label);
        _costMilliseconds = _costMaximumMilliseconds = 0;
        _costDraws = 0;
    }
}

static bool DrawWeapon(void* device, const float* weaponWorld, uintptr_t weaponClass, float triggerPull, bool isLeft)
{
    if (!HAS_DATA || !_isOn || !device || !weaponWorld)
    {
        return false;
    }
    Prepare();
    const int grip = FindGrip(weaponClass);
    const bool isForearmWeapon = grip >= 0 && !strcmp(BODY_ARM_GRIPS[grip].name, FOREARM_WEAPON_GRIP);
    if (grip < 0 || (_gripPose[grip] < 0 && !isForearmWeapon) || (isLeft && !isForearmWeapon))
    {
        return false;
    }
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    if (_device != d3dDevice)
    {
        OnReset();
        ReleaseBuffers();
        _device = d3dDevice;
    }
    if (!CreateBuffers(d3dDevice))
    {
        return false;
    }
    FpGuard fp; // RAII: a clean floating-point state for our arithmetic, the game's restored at the end
    static float skin[MAXIMUM_BONES][16];
    // the forearm in line with the barrel: the weapon's forward (msh -z: a model looks along +z in the game, whose x
    // the msh keeps) carried into arm space by the grip
    float forward[3], barrel[3] = {0, 0, _isForwardMinusZ ? -1.0f : 1.0f};
    for (const GripBarrel& entry : GRIP_BARRELS)
    {
        if (!strcmp(entry.grip, BODY_ARM_GRIPS[grip].name))
        {
            memcpy(barrel, entry.axis, sizeof(barrel));
        }
    }
    const float* grip3 = BODY_ARM_GRIPS[grip].matrix;
    for (int row = 0; row < 3; ++row)
    {
        forward[row] = grip3[row] * barrel[0] + grip3[4 + row] * barrel[1] + grip3[8 + row] * barrel[2];
    }
    const float forwardLength = sqrtf(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    for (float& value : forward)
    {
        value /= forwardLength;
    }
    // (a thrown item has no barrel to line the forearm up with: its wrist stays as fitted; headset run 18, the grenade's
    // grip looked wrong)
    // (and its index finger stays off the trigger)
    const bool isThrown = strstr(BODY_ARM_GRIPS[grip].name, "thermaldetonator") != nullptr || // any side's
                          IsItemGrip(BODY_ARM_GRIPS[grip].name);
    if (isForearmWeapon)
    {
        const int pose = _forcedPose >= 0 ? _forcedPose : ControllerPose(isLeft ? 1 : 0, triggerPull);
        if (pose < 0)
        {
            return false;
        }
        ComputeSkin(skin, pose, 0.0f, nullptr);
    }
    else
    {
        ComputeSkin(
            skin,
            _gripPose[grip],
            isThrown || triggerPull < 0 ? 0.0f : triggerPull > 1 ? 1.0f : triggerPull,
            _isWristStraight && !isThrown ? forward : nullptr);
    }
    // arm space -> the weapon's msh space (inverse grip) -> the game's model space (AXES) -> world (the weapon's
    // matrix, rows). A column-vector matrix stored [column * 4 + row] read as rows is its transpose, which is the
    // same map for row vectors, so it multiplies straight onto the game's row matrix.
    float inverseGrip[16], toModel[16] = {}, armToModel[16], world[16];
    InvertAffine(inverseGrip, BODY_ARM_GRIPS[grip].matrix);
    const int axes = _axes & 3;
    toModel[0] = AXES[axes][0];
    toModel[5] = AXES[axes][1];
    toModel[10] = AXES[axes][2];
    toModel[15] = 1;
    MultiplyColumns(armToModel, toModel, inverseGrip);
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            float sum = 0;
            for (int inner = 0; inner < 4; ++inner)
            {
                sum += armToModel[row * 4 + inner] * weaponWorld[inner * 4 + column];
            }
            world[row * 4 + column] = sum;
        }
    }
    Remember(isLeft ? 1 : 0, world, skin);
    _isDrawingLeft = isLeft;
    const bool isSkinned = !PlayerBody::IsDrawingHands() && (UpdateShownParts(), Skin(skin));
    _isDrawingLeft = false;
    if (!isSkinned)
    {
        return false;
    }
    if (_shouldLog)
    {
        _shouldLog = false;
        LogMatrix("weapon world", weaponWorld);
        LogMatrix("arm to weapon model (columns)", armToModel);
        LogMatrix("arm world (rows)", world);
    }
    Render(d3dDevice, world, start, BODY_ARM_GRIPS[grip].name);
    return true;
}

void Draw(void* device, const float* weaponWorld, uintptr_t weaponClass, float triggerPull)
{
    DrawWeapon(device, weaponWorld, weaponClass, triggerPull, false);
}

bool DrawLeftForearmWeapon(void* device, const float* mirroredWeaponWorld, uintptr_t weaponClass, float triggerPull)
{
    return DrawWeapon(device, mirroredWeaponWorld, weaponClass, triggerPull, true);
}

bool IsEmptyHandWanted()
{
    return HAS_DATA && _isOn && CurrentSkeleton() >= 0;
}

// The arm drawn with nothing in the weapon's place: arm space -> the controller's grip pose frame (ArmToGrip, round the
// rifle's handle as the closest) -> the grip's world; the left arm is the right one mirrored across the grip frame's
// x (OpenXR's grip x points to the user's right for both hands: the left hand is the right one's mirror image in it).
static void DrawAtGrip(void* device, const float* gripWorld, int pose, bool isLeft)
{
    if (!HAS_DATA || !_isOn || !device || !gripWorld || pose < 0)
    {
        return;
    }
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    if (_device != d3dDevice)
    {
        OnReset();
        ReleaseBuffers();
        _device = d3dDevice;
    }
    if (!CreateBuffers(d3dDevice))
    {
        return;
    }
    FpGuard fp; // RAII: a clean floating-point state for our arithmetic, the game's restored at the end
    static float skin[MAXIMUM_BONES][16];
    const float straight[3] = {0, 1, 0};
    ComputeSkin(skin, pose, 0.0f, _isWristStraight ? straight : nullptr);
    float world[16];
    ArmToGripWorld(gripWorld, isLeft, world);
    Remember(isLeft ? 1 : 0, world, skin);
    _isDrawingLeft = isLeft;
    const bool isSkinned = !PlayerBody::IsDrawingHands() && (UpdateShownParts(), Skin(skin));
    _isDrawingLeft = false;
    if (!isSkinned)
    {
        return;
    }
    Render(d3dDevice, world, start, BODY_ARM_POSE_NAMES[pose]);
}

// The open hand's pose from the controller (headset run 18, idren2023: the rest of the poses in the game): grip and
// trigger closed a fist (the thumb off the face: thumbs up), the grip alone a point with the finger off the trigger
// (else the grip-only hold), the trigger alone its pull, nothing the open hand. Without touch sensors only the
// grip and trigger values count.
int ControllerPose(int hand, float triggerPull)
{
    Xr::ControllerInput pad{};
    Xr::GetControllerInput(&pad);
    const bool hasTouch = pad.isValid && pad.hasTouch[hand];
    // the trigger as the controller has it (headset run 19: the left fist turned to a point after a second, when the
    // left trigger's use was spent and the pull handed in fell to 0)
    const bool isGrip = pad.isValid && pad.grip[hand] > 0.5f;
    const bool isTrigger = triggerPull > 0.5f || (pad.isValid && pad.trigger[hand] > 0.5f);
    const char* name = EMPTY_POSE;
    if (hand == 1 && Aim::IsTwoHanded())
    {
        name = "controller: grip only"; // round the gun's fore-end
    }
    else if (isGrip && hasTouch && !pad.isThumbTouched[hand] && (isTrigger || pad.isTriggerTouched[hand]))
    {
        name = "controller: thumbs up (grip + trigger, thumb off)";
    }
    else if (isGrip && isTrigger)
    {
        name = "controller: fist (grip + trigger, thumb on the face)";
    }
    else if (isGrip)
    {
        name = hasTouch && !pad.isTriggerTouched[hand] ? "controller: point (grip, trigger untouched)"
                                                        : "controller: grip only";
    }
    else if (isTrigger)
    {
        name = EMPTY_TRIGGER_POSE;
    }
    const int skeleton = CurrentSkeleton();
    const int pose = FindSkeletonPose(skeleton, name);
    return pose >= 0 ? pose : FindSkeletonPose(skeleton, isTrigger ? EMPTY_TRIGGER_POSE : EMPTY_POSE);
}

void DrawEmptyHand(void* device, const float* gripWorld, float triggerPull)
{
    if (!HAS_DATA || !_isOn)
    {
        return;
    }
    Prepare();
    DrawAtGrip(
        device,
        gripWorld,
        _forcedPose >= 0 ? _forcedPose : ControllerPose(0, triggerPull),
        false);
}

void DrawLeft(void* device, const float* gripWorld, float triggerPull, bool isHoldingCanister)
{
    if (!HAS_DATA || !_isOn)
    {
        return;
    }
    Prepare();
    const int canisterPose = isHoldingCanister ? FindSkeletonPose(CurrentSkeleton(), CANISTER_POSE) : -1;
    DrawAtGrip(
        device,
        gripWorld,
        canisterPose >= 0 ? canisterPose : _forcedPose >= 0 ? _forcedPose : ControllerPose(1, triggerPull),
        true);
}

// ---- Handlebar grips (PLAN 61 j, headset run 19: the stock rifle fist sat across the bike's bar) ----

/// Each skeleton's bar through its fist in the bike grip pose (arm space; tools/body_bikegrip.py fit, its "AXIS"
/// lines and re/review/hands/bikegrip-fit.json): a point on the bar's axis and its direction toward the little finger.
struct BarAxis
{
    const char* skeleton;
    float point[3];
    float direction[3];
};
constexpr BarAxis BAR_AXES[] = {
    {"human", {-0.0059f, 0.0819f, -0.0421f}, {0.9665f, -0.2541f, 0.0354f}},
    {"b1", {-0.0128f, 0.0555f, -0.0439f}, {1.0f, 0.0f, 0.0f}},
    {"sbd", {-0.0015f, 0.0590f, -0.0220f}, {0.9889f, -0.1483f, 0.0f}},
};
constexpr const char* BAR_POSE = "vehicle: bike grip (fitted)";
constexpr const char* BAR_POSE_FALLBACK = "vehicle: turret / bike bar: full wrap"; // an include without the fit
constexpr float BAR_ARM_SCALE = 0.7116788f;  // arm space is the game's size times this (ArmToGrip)
constexpr float BAR_UPPER_ARM_METERS = 0.30f; // the rider's upper arm (Aim's UPPER_ARM_METERS)
constexpr float MAX_BAR_WRIST_DEGREES = 30.0f; // the wrist bends at most this far off the forearm's line
/// The elbow's side in the bar frame (out along the bar, up, back): out and down, a little back.
constexpr float BAR_POLE[3] = {0.581238f, -0.774984f, 0.193746f};

namespace
{

float Dot3(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void Cross3(const float* a, const float* b, float* out)
{
    const float result[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    memcpy(out, result, sizeof(result));
}

bool Normalize3(float* v)
{
    const float length = sqrtf(Dot3(v, v));
    if (length < 1e-6f)
    {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        v[axis] /= length;
    }
    return true;
}

/// The hand on the bar at one roll: arm space -> bar frame as R(roll) (rows of the rotation applied to column
/// vectors: x along the bar), the wrist in the bar frame, and how well the straight forearm points along the line
/// from the solved elbow to the wrist (cosine).
struct BarRoll
{
    float rotation[3][3]; // bar = rotation x arm (column vectors)
    float wrist[3];
    float want[3]; // elbow -> wrist, bar frame
    float score;
};

void RotateArmToBar(const float (*rotation)[3], const float* v, float* out)
{
    for (int row = 0; row < 3; ++row)
    {
        out[row] = rotation[row][0] * v[0] + rotation[row][1] * v[1] + rotation[row][2] * v[2];
    }
}

void SolveRoll(
    const float (*atZero)[3],
    const float* axisPoint,
    const float* shoulder,
    float forearmMeters,
    float roll,
    BarRoll* out)
{
    const float c = cosf(roll), s = sinf(roll);
    for (int column = 0; column < 3; ++column)
    {
        out->rotation[0][column] = atZero[0][column];
        out->rotation[1][column] = c * atZero[1][column] - s * atZero[2][column];
        out->rotation[2][column] = s * atZero[1][column] + c * atZero[2][column];
    }
    const float minusPoint[3] = {-axisPoint[0], -axisPoint[1], -axisPoint[2]};
    RotateArmToBar(out->rotation, minusPoint, out->wrist);
    for (float& value : out->wrist)
    {
        value /= BAR_ARM_SCALE;
    }
    const float straight[3] = {0, 1, 0};
    float natural[3];
    RotateArmToBar(out->rotation, straight, natural);
    // two-bone arm from the shoulder to the wrist, the elbow toward BAR_POLE
    float toWrist[3] = {out->wrist[0] - shoulder[0], out->wrist[1] - shoulder[1], out->wrist[2] - shoulder[2]};
    const float upper = BAR_UPPER_ARM_METERS;
    float distance = sqrtf(Dot3(toWrist, toWrist));
    distance = fminf(fmaxf(distance, fabsf(upper - forearmMeters) + 0.01f), upper + forearmMeters - 0.001f);
    Normalize3(toWrist);
    const float toCentre = (upper * upper - forearmMeters * forearmMeters + distance * distance) / (2.0f * distance);
    const float radius = sqrtf(fmaxf(upper * upper - toCentre * toCentre, 0.0f));
    float pole[3];
    const float along = Dot3(BAR_POLE, toWrist);
    for (int axis = 0; axis < 3; ++axis)
    {
        pole[axis] = BAR_POLE[axis] - toWrist[axis] * along;
    }
    Normalize3(pole);
    for (int axis = 0; axis < 3; ++axis)
    {
        const float elbow = shoulder[axis] + toWrist[axis] * toCentre + pole[axis] * radius;
        out->want[axis] = out->wrist[axis] - elbow;
    }
    Normalize3(out->want);
    out->score = Dot3(natural, out->want);
}

} // namespace

void DrawOnBar(void* device, const float* barWorld, const float* shoulderInBar, float twistRadians, bool isLeft)
{
    if (!HAS_DATA || !_isOn || !device || !barWorld || !shoulderInBar)
    {
        return;
    }
    Prepare();
    const int skeleton = CurrentSkeleton();
    if (skeleton < 0 || skeleton >= SKELETON_COUNT)
    {
        return;
    }
    const BarAxis* axis = nullptr;
    for (const BarAxis& candidate : BAR_AXES)
    {
        axis = !strcmp(candidate.skeleton, BODY_ARM_SKELETONS[skeleton].name) ? &candidate : axis;
    }
    int pose = FindSkeletonPose(skeleton, BAR_POSE);
    pose = pose >= 0 ? pose : FindSkeletonPose(skeleton, BAR_POSE_FALLBACK);
    if (!axis || pose < 0)
    {
        return;
    }
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    if (_device != d3dDevice)
    {
        OnReset();
        ReleaseBuffers();
        _device = d3dDevice;
    }
    if (!CreateBuffers(d3dDevice))
    {
        return;
    }
    FpGuard fp;
    // roll 0: the bar's axis along x, the back of the hand up
    float atZero[3][3];
    memcpy(atZero[0], axis->direction, sizeof(atZero[0]));
    Normalize3(atZero[0]);
    const float back[3] = {0, 0, 1};
    const float onAxis = Dot3(back, atZero[0]);
    for (int i = 0; i < 3; ++i)
    {
        atZero[1][i] = back[i] - atZero[0][i] * onAxis;
    }
    Normalize3(atZero[1]);
    Cross3(atZero[0], atZero[1], atZero[2]);
    const float* elbowRest = BODY_ARM_BONES[BODY_ARM_SKELETONS[skeleton].firstBone].rest + 12; // the forearm's head
    const float forearmMeters = sqrtf(Dot3(elbowRest, elbowRest)) / BAR_ARM_SCALE;
    // the roll about the bar that points the straight forearm at the elbow: 5 degree steps, then half degrees
    constexpr float DEGREE = 0.01745329f;
    BarRoll best{}, trial{};
    float bestRoll = 0;
    best.score = -2;
    for (int step = 0; step < 72; ++step)
    {
        SolveRoll(atZero, axis->point, shoulderInBar, forearmMeters, step * 5.0f * DEGREE, &trial);
        if (trial.score > best.score)
        {
            best = trial;
            bestRoll = step * 5.0f * DEGREE;
        }
    }
    const float coarse = bestRoll;
    for (int step = -10; step <= 10; ++step)
    {
        SolveRoll(atZero, axis->point, shoulderInBar, forearmMeters, coarse + step * 0.5f * DEGREE, &trial);
        if (trial.score > best.score)
        {
            best = trial;
            bestRoll = coarse + step * 0.5f * DEGREE;
        }
    }
    // the twist grip turns the hand about the bar (rolled back: the top of the hand toward the rider)
    SolveRoll(atZero, axis->point, shoulderInBar, forearmMeters, bestRoll + twistRadians, &best);
    // the forearm's direction in arm space (the transpose), the wrist's bend capped
    float toward[3];
    for (int column = 0; column < 3; ++column)
    {
        toward[column] = best.rotation[0][column] * best.want[0] + best.rotation[1][column] * best.want[1] +
                         best.rotation[2][column] * best.want[2];
    }
    const float straight[3] = {0, 1, 0};
    const float bend = acosf(fminf(fmaxf(Dot3(toward, straight), -1.0f), 1.0f));
    if (bend > MAX_BAR_WRIST_DEGREES * DEGREE)
    {
        // slerp from straight toward `toward` by the allowed share
        const float share = MAX_BAR_WRIST_DEGREES * DEGREE / bend, sine = sinf(bend);
        const float a = sinf((1.0f - share) * bend) / sine, b = sinf(share * bend) / sine;
        for (int i = 0; i < 3; ++i)
        {
            toward[i] = a * straight[i] + b * toward[i];
        }
        Normalize3(toward);
    }
    static float skin[MAXIMUM_BONES][16];
    ComputeSkin(skin, pose, 0.0f, toward);
    // arm space -> bar frame (rows: each arm axis's image, scaled to the game's size; the wrist as the origin), then
    // the bar frame -> world (a mirrored frame for the left hand: the left arm is the right one's mirror image)
    float armToBar[16] = {};
    for (int row = 0; row < 3; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            armToBar[row * 4 + column] = best.rotation[column][row] / BAR_ARM_SCALE;
        }
        armToBar[12 + row] = best.wrist[row];
    }
    armToBar[15] = 1;
    float world[16];
    MultiplyRows(world, armToBar, barWorld);
    Remember(isLeft ? 1 : 0, world, skin);
    _isDrawingLeft = isLeft;
    const bool isSkinned = !PlayerBody::IsDrawingHands() && (UpdateShownParts(), Skin(skin));
    _isDrawingLeft = false;
    if (isSkinned)
    {
        Render(d3dDevice, world, start, BODY_ARM_POSE_NAMES[pose]);
    }
}

bool GetHandPose(int hand, float* armToWorld, const float (**skin)[16])
{
    if (!HAS_DATA || hand < 0 || hand > 1 || !_lastTick[hand] || GetTickCount() - _lastTick[hand] > 250)
    {
        return false;
    }
    memcpy(armToWorld, _lastArmToWorld[hand], sizeof(_lastArmToWorld[hand]));
    *skin = _lastSkin[hand];
    return true;
}

int BoneCount()
{
    return HAS_DATA ? BODY_ARM_SKELETONS[0].boneCount : 0; // the human skeleton's (PlayerBody's hands)
}

const char* BoneName(int bone)
{
    return bone >= 0 && bone < BONE_COUNT ? BODY_ARM_BONES[bone].name : "";
}

bool IsLeftWanted()
{
    return HAS_DATA && _isOn && _isLeftOn && CurrentSkeleton() >= 0;
}

bool GetItemWorld(uintptr_t weaponClass, const float* gripWorld, float* itemWorld)
{
    // (not the grenade: its hold was right, headset run 19)
    const char* itemGrip = Holster::HeldItemGrip(weaponClass);
    if (!HAS_DATA || !_isOn || !gripWorld || !itemGrip || strncmp(itemGrip, "item_", 5))
    {
        return false;
    }
    Prepare();
    const int grip = FindGrip(weaponClass);
    if (grip < 0 || !IsItemGrip(BODY_ARM_GRIPS[grip].name))
    {
        return false;
    }
    FpGuard fp;
    // The hand stays on the controller as the empty hand sits there (ArmToGripWorld, the rifle handle's fit) and the
    // item goes where the fitted grip puts it in that hand (headset run 19, idren2023: the item turns to fit the hand,
    // not the arm to fit the item): the game's model space -> msh (AXES) -> arm (the grip) -> world. The grip's scale
    // (our 0.712, the item's size) is taken out of the rotation: Holster draws the item at its own scale.
    float armWorld[16], toModel[16] = {}, inMsh[16], world[16];
    ArmToGripWorld(gripWorld, false, armWorld);
    const int axes = _axes & 3;
    toModel[0] = AXES[axes][0];
    toModel[5] = AXES[axes][1];
    toModel[10] = AXES[axes][2];
    toModel[15] = 1;
    MultiplyRows(inMsh, toModel, BODY_ARM_GRIPS[grip].matrix); // columns read as rows: the same map for row vectors
    MultiplyRows(world, inMsh, armWorld);
    for (int row = 0; row < 3; ++row)
    {
        const float length = sqrtf(world[row * 4] * world[row * 4] + world[row * 4 + 1] * world[row * 4 + 1] +
                                   world[row * 4 + 2] * world[row * 4 + 2]);
        for (int column = 0; column < 3 && length > 1e-6f; ++column)
        {
            world[row * 4 + column] /= length;
        }
    }
    memcpy(itemWorld, world, sizeof(world));
    return true;
}

bool GetCanisterWorld(const float* leftGripWorld, float* canisterWorld)
{
    if (!IsLeftWanted() || !leftGripWorld)
    {
        return false;
    }
    int canister = -1;
    for (int grip = 0; grip < GRIP_COUNT; ++grip)
    {
        canister = BODY_ARM_GRIPS[grip].name && !strcmp(BODY_ARM_GRIPS[grip].name, "ammocanister") ? grip : canister;
    }
    if (canister < 0)
    {
        return false;
    }
    FpGuard fp;
    // msh -> arm space (the fitted grip: the canister where tools/body_tunegrip.py turned the hand on it) -> world as
    // the left arm; the mirror undone on the model by flipping its x about its middle (x 0.0135), so the game draws
    // it with a proper rotation (a mirrored matrix would turn it inside out).
    float armWorld[16], inMsh[16], flipped[16];
    ArmToGripWorld(leftGripWorld, true, armWorld);
    const float* columns = BODY_ARM_GRIPS[canister].matrix; // columns read as rows: the same map for row vectors
    MultiplyRows(inMsh, columns, armWorld);
    const float flip[16] = {-1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2 * CANISTER_MIDDLE_X, 0, 0, 1};
    MultiplyRows(flipped, flip, inMsh);
    memcpy(canisterWorld, flipped, sizeof(flipped));
    return true;
}

void Configure(const char* arguments)
{
    if (!arguments)
    {
        return;
    }
    if (!strncmp(arguments, "axes ", 5))
    {
        _axes = atoi(arguments + 5) & 3;
        Log::Info("bodyarm", "weapon msh -> model axes %d", _axes);
    }
    else if (!strncmp(arguments, "pose ", 5))
    {
        // a pose of the library by (part of) its name, "auto" back to the trigger
        _forcedPose = -1;
        for (int pose = 0; pose < static_cast<int>(sizeof(BODY_ARM_POSE_NAMES) / sizeof(BODY_ARM_POSE_NAMES[0]));
             ++pose)
        {
            if (_forcedPose < 0 && BODY_ARM_POSE_NAMES[pose] && strstr(BODY_ARM_POSE_NAMES[pose], arguments + 5))
            {
                _forcedPose = pose;
            }
        }
        Log::Info("bodyarm", "empty hand pose: %s", _forcedPose >= 0 ? BODY_ARM_POSE_NAMES[_forcedPose] : "by the trigger");
    }
    else if (!strncmp(arguments, "tilt ", 5))
    {
        _gripTiltDegrees = static_cast<float>(atof(arguments + 5));
        Log::Info("bodyarm", "hand tilted %.1f degrees about the controller's handle", static_cast<double>(_gripTiltDegrees));
    }
    else if (!strncmp(arguments, "wrist ", 6))
    {
        _isWristStraight = atoi(arguments + 6) != 0;
        Log::Info("bodyarm", "forearm in line with the barrel: %s", _isWristStraight ? "on" : "off");
    }
    else if (!strncmp(arguments, "left ", 5))
    {
        _isLeftOn = atoi(arguments + 5) != 0;
        Log::Info("bodyarm", "our left arm: %s", _isLeftOn ? "on" : "off (the stock one)");
    }
    else if (!strncmp(arguments, "forward ", 8))
    {
        _isForwardMinusZ = atoi(arguments + 8) != 0;
        Log::Info("bodyarm", "the barrel is the msh's %sz", _isForwardMinusZ ? "-" : "+");
    }
    else if (!strcmp(arguments, "log"))
    {
        _shouldLog = true;
    }
    else
    {
        _isOn = atoi(arguments) != 0;
        Log::Info("bodyarm", "our arm %s%s", _isOn ? "on" : "off", HAS_DATA ? "" : " (this build has no arm data)");
    }
}

void OnReset()
{
    if (_savedState)
    {
        _savedState->lpVtbl->Release(_savedState);
        _savedState = nullptr;
    }
}

} // namespace BodyArm

} // namespace SwbfVr
