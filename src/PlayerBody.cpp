// PlayerBody: our own model of the player's body (PlayerBody.h). The data (tools/body_export_player.py ->
// build/gen/PlayerBody.inc): vertices in model space (the msh's: y up, +z forward, the model's left +x) with four bone
// weights, our whole skeleton (parent, rest frame and its inverse, row vectors), the rest eye, the map from BodyArm's
// arm space to ours, parts per texture, our textures as A8R8G8B8 rows.
//
// Each draw: the body's place and size (scale from the configured height), then every bone posed by us (Solve, model
// space): the hips from the headset, the spine to the neck below the eyes, the arms to BodyArm's wrists, the hands and
// fingers as BodyArm posed them, the legs to the game's feet; skin matrix per bone = inverse rest x posed (row
// vectors), every vertex skinned on the CPU into a managed vertex buffer (linear blend of four), drawn by
// fixed-function lighting (as BodyArm).

// Makes d3d9.h declare the Direct3D interfaces as C structs with an explicit vtable (device->lpVtbl->Method(device,
// ...)), the form the rest of the mod uses. Has to be a macro: d3d9.h reads it.
#define CINTERFACE

#include "PlayerBody.h"

#include "BodyArm.h"
#include "DrawState.h"
#include "GameMeshes.h"
#include "GameTextures.h"
#include "Log.h"
#include "Stereo.h"
#include "Textures.h"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace SwbfVr
{

namespace PlayerBody
{

namespace
{

struct PlayerBodyBone
{
    const char* name;
    int parent;            // index of the parent bone (always earlier), -1 for a root
    float rest[16];        // the bone's frame at rest -> model space (row vectors; Blender's axes, y along the bone)
    float inverseRest[16]; // model space at rest -> the bone's frame
};

struct PlayerBodyVertex
{
    float position[3];
    float normal[3];
    float uv[2];
    unsigned char bones[4];
    float weights[4]; // summing to 1
};

struct PlayerBodyTexture
{
    int width, height;
    const unsigned* pixels; // A8R8G8B8, rows from the top
};

struct PlayerBodyPart
{
    const char* name;
    int ownTexture;          // PLAYER_BODY_TEXTURES index, -1: the game's texture by name
    const char* gameTexture; // Textures::FindByName
    const PlayerBodyVertex* vertices;
    unsigned vertexCount;
    const unsigned short* indices;
    unsigned indexCount;
};

#if __has_include("PlayerBody.inc")
#include "PlayerBody.inc"
constexpr bool HAS_DATA = true;
#else
constexpr PlayerBodyBone PLAYER_BODY_BONES[1] = {};
constexpr float PLAYER_BODY_EYE[3] = {};
constexpr float PLAYER_BODY_ARM_TO_MODEL[16] = {};
constexpr PlayerBodyTexture PLAYER_BODY_TEXTURES[1] = {};
constexpr PlayerBodyPart PLAYER_BODY_PARTS[1] = {};
constexpr bool HAS_DATA = false;
#endif

constexpr int BONE_COUNT = static_cast<int>(sizeof(PLAYER_BODY_BONES) / sizeof(PLAYER_BODY_BONES[0]));
constexpr int PART_COUNT = static_cast<int>(sizeof(PLAYER_BODY_PARTS) / sizeof(PLAYER_BODY_PARTS[0]));
constexpr int TEXTURE_COUNT = static_cast<int>(sizeof(PLAYER_BODY_TEXTURES) / sizeof(PLAYER_BODY_TEXTURES[0]));

constexpr DWORD VERTEX_FORMAT = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
struct Vertex
{
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

volatile bool _isOn = true;
volatile bool _shouldLog = false;
IDirect3DVertexBuffer9* _vertexBuffers[PART_COUNT] = {};
IDirect3DIndexBuffer9* _indexBuffers[PART_COUNT] = {};
IDirect3DTexture9* _textures[TEXTURE_COUNT] = {};
IDirect3DStateBlock9* _savedState = nullptr;
bool _hasFailed = false;
double _costMilliseconds = 0, _costMaximumMilliseconds = 0;
int _costDraws = 0;
constexpr int COST_LOG_DRAWS = 600;
DWORD _lastDrawTick = 0; // the last Draw that drew (IsDrawingHands)

// The solve's limits (model units: the msh's metres, before the height's scale).
constexpr float EYE_HEIGHT_SHARE = 0.936f;     // a person's eye height over their height (anthropometry)
constexpr float LOWEST_HIPS = 0.45f;           // the hips go no lower (a deep crouch)
constexpr float MAXIMUM_YAW = 0.785f;          // the upper body turns at most 45 deg after the head
constexpr float MAXIMUM_SHRUG = 0.524f;        // a collarbone turns at most 30 deg toward a hand out of reach
constexpr float REACH_SHARE = 0.97f;           // the collarbone turns once a hand is beyond this of the arm's length
constexpr int RIGHT = 0, LEFT = 1;             // sides as Aim numbers the hands
constexpr float SIDE_X[2] = {-1.0f, 1.0f};     // the model's left is +x
constexpr char SIDE_PREFIX[2] = {'r', 'l'};
constexpr int MAXIMUM_ARM_BONES = 32;

// Bones the solve moves (PLAYER_BODY_BONES indices, -1 when missing), found once.
struct Skeleton
{
    bool isFound;
    int spine[3], neck, pelvis; // a_spine, b_spine, ribcage
    int clavicle[2], upperArm[2], forearm[2], forearmTwist[2], wristTwist[2], hand[2];
    int thigh[2], calf[2], foot[2], toe[2];
    int fromArm[2][MAXIMUM_ARM_BONES]; // per BodyArm bone, our bone it poses (the hand and fingers), else -1
    float modelToArm[2][16]; // model -> BodyArm's arm space (rows), the left arm's through the mirror across x
};
Skeleton _skeleton = {};
bool _isSkeletonLooked = false;
float _posed[BONE_COUNT][16]; // bone -> model, this draw
float _skin[BONE_COUNT][16];  // model at rest -> model posed
float _handSkin[2][MAXIMUM_ARM_BONES][16];
// What the last solve did (the log line).
float _lastScale = 0, _lastHipsDrop = 0, _lastYawDegrees = 0, _lastLeanDegrees = 0, _lastReach[2] = {},
      _lastShrugDegrees[2] = {}, _lastHandScale[2] = {};
bool _lastHasHand[2] = {}, _lastHasFoot[2] = {};

void Release()
{
    for (IDirect3DTexture9*& texture : _textures)
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
    if (_savedState)
    {
        _savedState->lpVtbl->Release(_savedState);
        _savedState = nullptr;
    }
}

// Our texture on the device with a box-filtered mip chain (managed: kept over resets).
IDirect3DTexture9* MakeTexture(IDirect3DDevice9* device, const PlayerBodyTexture& data, int index)
{
    // the pixels made at start from the player's own game texture (GameTextures; not compiled in)
    char table[32];
    snprintf(table, sizeof(table), "PLAYER_BODY_TEXELS_%d", index);
    int madeWidth = 0, madeHeight = 0;
    const unsigned* pixels = GameTextures::Pixels(table, madeWidth, madeHeight);
    if (!pixels || madeWidth != data.width || madeHeight != data.height)
    {
        return nullptr;
    }
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->lpVtbl->CreateTexture(
            device, data.width, data.height, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr)))
    {
        return nullptr;
    }
    int width = data.width, height = data.height;
    std::vector<unsigned> level(pixels, pixels + width * height), next;
    const DWORD levels = texture->lpVtbl->GetLevelCount(texture);
    for (DWORD mip = 0; mip < levels; ++mip)
    {
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(texture->lpVtbl->LockRect(texture, mip, &locked, nullptr, 0)))
        {
            for (int row = 0; row < height; ++row)
            {
                memcpy(static_cast<char*>(locked.pBits) + row * locked.Pitch, level.data() + row * width, width * 4);
            }
            texture->lpVtbl->UnlockRect(texture, mip);
        }
        const int nextWidth = width > 1 ? width / 2 : 1, nextHeight = height > 1 ? height / 2 : 1;
        next.assign(nextWidth * nextHeight, 0);
        for (int row = 0; row < nextHeight; ++row)
        {
            for (int column = 0; column < nextWidth; ++column)
            {
                unsigned sum[4] = {};
                for (int dy = 0; dy < 2; ++dy)
                {
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        const int y = row * 2 + dy < height ? row * 2 + dy : height - 1;
                        const int x = column * 2 + dx < width ? column * 2 + dx : width - 1;
                        const unsigned texel = level[y * width + x];
                        for (int channel = 0; channel < 4; ++channel)
                        {
                            sum[channel] += (texel >> (channel * 8)) & 0xFF;
                        }
                    }
                }
                unsigned texel = 0;
                for (int channel = 0; channel < 4; ++channel)
                {
                    texel |= ((sum[channel] + 2) / 4) << (channel * 8);
                }
                next[row * nextWidth + column] = texel;
            }
        }
        level.swap(next);
        width = nextWidth;
        height = nextHeight;
    }
    return texture;
}

bool Prepare(IDirect3DDevice9* device)
{
    if (_vertexBuffers[0])
    {
        return true;
    }
    for (int part = 0; part < PART_COUNT; ++part)
    {
        const PlayerBodyPart& data = PLAYER_BODY_PARTS[part];
        void* indices = nullptr;
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
                nullptr)) ||
            FAILED(_indexBuffers[part]->lpVtbl->Lock(_indexBuffers[part], 0, 0, &indices, 0)))
        {
            Release();
            return false;
        }
        memcpy(indices, data.indices, data.indexCount * sizeof(unsigned short));
        _indexBuffers[part]->lpVtbl->Unlock(_indexBuffers[part]);
    }
    for (int index = 0; index < TEXTURE_COUNT; ++index)
    {
        _textures[index] = MakeTexture(device, PLAYER_BODY_TEXTURES[index], index);
    }
    unsigned vertices = 0, triangles = 0;
    for (const PlayerBodyPart& data : PLAYER_BODY_PARTS)
    {
        vertices += data.vertexCount;
        triangles += data.indexCount / 3;
    }
    Log::Info(
        "playerbody",
        "our body ready: %d parts, %u vertices, %u triangles, %d bones, %d textures",
        PART_COUNT,
        vertices,
        triangles,
        BONE_COUNT,
        TEXTURE_COUNT);
    return true;
}

// Row-vector product: first then second (product may be either of them).
void Multiply(float* product, const float* first, const float* second)
{
    float result[16];
    for (int row = 0; row < 4; ++row)
    {
        for (int column = 0; column < 4; ++column)
        {
            result[row * 4 + column] = first[row * 4] * second[column] + first[row * 4 + 1] * second[4 + column] +
                                       first[row * 4 + 2] * second[8 + column] +
                                       first[row * 4 + 3] * second[12 + column];
        }
    }
    memcpy(product, result, sizeof(result));
}

// Row-vector inverse of an affine map (rotation and scale, then translation).
void InvertAffine(float* inverse, const float* m)
{
    const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    const float k = fabsf(determinant) > 1e-12f ? 1.0f / determinant : 0.0f;
    float r[16] = {};
    r[0] = (e * i - f * h) * k;
    r[1] = (c * h - b * i) * k;
    r[2] = (b * f - c * e) * k;
    r[4] = (f * g - d * i) * k;
    r[5] = (a * i - c * g) * k;
    r[6] = (c * d - a * f) * k;
    r[8] = (d * h - e * g) * k;
    r[9] = (b * g - a * h) * k;
    r[10] = (a * e - b * d) * k;
    for (int column = 0; column < 3; ++column)
    {
        r[12 + column] = -(m[12] * r[column] + m[13] * r[4 + column] + m[14] * r[8 + column]);
    }
    r[15] = 1.0f;
    memcpy(inverse, r, sizeof(r));
}

float Dot(const float* a, const float* b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool Normalize(float* v)
{
    const float length = sqrtf(Dot(v, v));
    if (length < 1e-6f)
    {
        return false;
    }
    v[0] /= length;
    v[1] /= length;
    v[2] /= length;
    return true;
}

void Cross(const float* a, const float* b, float* out)
{
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    out[0] = x;
    out[1] = y;
    out[2] = z;
}

// A point through a row matrix (p x m).
void Transform(const float* p, const float* m, float* out)
{
    float r[3];
    for (int column = 0; column < 3; ++column)
    {
        r[column] = p[0] * m[column] + p[1] * m[4 + column] + p[2] * m[8 + column] + m[12 + column];
    }
    memcpy(out, r, sizeof(r));
}

// A direction through a row matrix's 3x3.
void TransformDirection(const float* d, const float* m, float* out)
{
    float r[3];
    for (int column = 0; column < 3; ++column)
    {
        r[column] = d[0] * m[column] + d[1] * m[4 + column] + d[2] * m[8 + column];
    }
    memcpy(out, r, sizeof(r));
}

float Distance(const float* a, const float* b)
{
    const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    return sqrtf(Dot(d, d));
}

// The rotation by angle about a unit axis through pivot, for row vectors (v x out).
void RotationAbout(const float* axis, float angle, const float* pivot, float* out)
{
    const float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
    const float x = axis[0], y = axis[1], z = axis[2];
    const float rows[9] = {// the column form's transpose
                           t * x * x + c,
                           t * x * y + s * z,
                           t * x * z - s * y,
                           t * x * y - s * z,
                           t * y * y + c,
                           t * y * z + s * x,
                           t * x * z + s * y,
                           t * y * z - s * x,
                           t * z * z + c};
    memset(out, 0, 16 * sizeof(float));
    for (int row = 0; row < 3; ++row)
    {
        memcpy(out + row * 4, rows + row * 3, 3 * sizeof(float));
    }
    out[15] = 1.0f;
    float turned[3];
    TransformDirection(pivot, out, turned);
    for (int column = 0; column < 3; ++column)
    {
        out[12 + column] = pivot[column] - turned[column];
    }
}

const float* Position(int bone)
{
    return _posed[bone] + 12;
}

// Every bone from root down (the bones are ordered parents first) moved by m (posed x m).
void MoveSubtree(int root, const float* m)
{
    bool isInside[BONE_COUNT] = {};
    for (int bone = root; bone < BONE_COUNT; ++bone)
    {
        const int parent = PLAYER_BODY_BONES[bone].parent;
        isInside[bone] = bone == root || (parent >= root && isInside[parent]);
        if (isInside[bone])
        {
            Multiply(_posed[bone], _posed[bone], m);
        }
    }
}

// Turns the subtree about pivot so that direction from comes along to, by share of the angle between them.
// @return the angle turned (radians).
float TurnSubtree(int root, const float* pivot, const float* from, const float* to, float share = 1.0f)
{
    float a[3] = {from[0], from[1], from[2]}, b[3] = {to[0], to[1], to[2]}, axis[3];
    if (!Normalize(a) || !Normalize(b))
    {
        return 0.0f;
    }
    Cross(a, b, axis);
    if (!Normalize(axis))
    {
        return 0.0f;
    }
    const float cosine = Dot(a, b);
    const float angle = acosf(cosine > 1.0f ? 1.0f : (cosine < -1.0f ? -1.0f : cosine)) * share;
    float m[16], at[3];
    memcpy(at, pivot, sizeof(at)); // pivot may be a bone's position, which the move changes
    RotationAbout(axis, angle, at, m);
    MoveSubtree(root, m);
    return angle;
}

float RestDistance(int a, int b)
{
    return Distance(PLAYER_BODY_BONES[a].rest + 12, PLAYER_BODY_BONES[b].rest + 12);
}

int FindBone(const char* name)
{
    for (int bone = 0; bone < BONE_COUNT; ++bone)
    {
        if (!strcmp(PLAYER_BODY_BONES[bone].name, name))
        {
            return bone;
        }
    }
    return -1;
}

bool FindSkeleton()
{
    Skeleton& k = _skeleton;
    k.spine[0] = FindBone("bone_a_spine");
    k.spine[1] = FindBone("bone_b_spine");
    k.spine[2] = FindBone("bone_ribcage");
    k.neck = FindBone("bone_neck");
    k.pelvis = FindBone("bone_pelvis");
    bool isFound = k.spine[0] >= 0 && k.spine[1] >= 0 && k.spine[2] >= 0 && k.neck >= 0 && k.pelvis >= 0 &&
                   BodyArm::BoneCount() <= MAXIMUM_ARM_BONES;
    float modelToArmRight[16];
    InvertAffine(modelToArmRight, PLAYER_BODY_ARM_TO_MODEL);
    for (int side = 0; side < 2; ++side)
    {
        const char p = SIDE_PREFIX[side];
        char name[40];
        const auto find = [&](const char* format)
        {
            snprintf(name, sizeof(name), format, p);
            return FindBone(name);
        };
        k.clavicle[side] = find("bone_%c_clavicle");
        k.upperArm[side] = find("bone_%c_upperarm");
        k.forearm[side] = find("%c_forearm");
        k.forearmTwist[side] = find("%c_forearm_twist");
        k.wristTwist[side] = find("%c_wrist_twist");
        k.hand[side] = find("%c_hand");
        k.thigh[side] = find("bone_%c_thigh");
        k.calf[side] = find("bone_%c_calf");
        k.foot[side] = find("bone_%c_foot");
        k.toe[side] = find("bone_%c_toe");
        isFound = isFound && k.clavicle[side] >= 0 && k.upperArm[side] >= 0 && k.forearm[side] >= 0 &&
                  k.hand[side] >= 0 && k.thigh[side] >= 0 && k.calf[side] >= 0 && k.foot[side] >= 0 &&
                  k.toe[side] >= 0;
        for (int arm = 0; arm < MAXIMUM_ARM_BONES; ++arm)
        {
            k.fromArm[side][arm] = -1;
            const char* armName = arm < BodyArm::BoneCount() ? BodyArm::BoneName(arm) : "";
            if (*armName && strcmp(armName, "forearm") && !strstr(armName, "twist"))
            {
                snprintf(name, sizeof(name), "%c_%s", p, armName);
                k.fromArm[side][arm] = FindBone(name);
            }
        }
        // our left arm is the right one's mirror across x: model -> mirrored -> the right arm's arm space
        const float mirror[16] = {side == LEFT ? -1.0f : 1.0f, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        Multiply(k.modelToArm[side], mirror, modelToArmRight);
    }
    for (int bone = 0; bone < BONE_COUNT; ++bone)
    {
        isFound = isFound && PLAYER_BODY_BONES[bone].parent < bone;
    }
    k.isFound = isFound;
    Log::Info("playerbody", "skeleton %s (%d bones)", isFound ? "found" : "NOT complete: our body not drawn", BONE_COUNT);
    return isFound;
}

// Two-bone IK: the middle joint and the end of a chain from root (lengths first, second) toward target, bent toward
// pole. @return root to target over the chain's length.
float SolveTwoBones(
    const float* root, const float* target, float first, float second, const float* pole, float* middle, float* end)
{
    float toTarget[3] = {target[0] - root[0], target[1] - root[1], target[2] - root[2]};
    float distance = sqrtf(Dot(toTarget, toTarget));
    const float reach = distance / (first + second);
    if (!Normalize(toTarget))
    {
        toTarget[1] = -1.0f;
    }
    const float longest = (first + second) * 0.999f, shortest = fabsf(first - second) + 0.01f;
    distance = distance > longest ? longest : (distance < shortest ? shortest : distance);
    const float a = (first * first - second * second + distance * distance) / (2.0f * distance);
    const float h = sqrtf(fmaxf(0.0f, first * first - a * a));
    float bend[3] = {pole[0], pole[1], pole[2]};
    const float along = Dot(bend, toTarget);
    for (int axis = 0; axis < 3; ++axis)
    {
        bend[axis] -= toTarget[axis] * along;
    }
    if (!Normalize(bend))
    {
        bend[0] = 0.0f;
        bend[1] = -1.0f;
        bend[2] = 0.0f;
    }
    for (int axis = 0; axis < 3; ++axis)
    {
        middle[axis] = root[axis] + toTarget[axis] * a + bend[axis] * h;
        end[axis] = root[axis] + toTarget[axis] * distance;
    }
    return reach;
}

// Turns bone's subtree about the bone's head so that the direction from its head to child's comes along to - head.
void AimBone(int bone, int child, const float* to)
{
    float at[3], from[3], toward[3];
    memcpy(at, Position(bone), sizeof(at));
    for (int axis = 0; axis < 3; ++axis)
    {
        from[axis] = Position(child)[axis] - at[axis];
        toward[axis] = to[axis] - at[axis];
    }
    TurnSubtree(bone, at, from, toward);
}

// One arm: the collarbone toward a far wrist, the upper arm and forearm by two-bone IK, the twist bones sharing the
// hand's roll about the forearm (handPosed: the hand's frame as BodyArm posed it, or null).
void SolveArm(int side, const float* wrist, const float* handPosed)
{
    const Skeleton& k = _skeleton;
    const float upper = RestDistance(k.upperArm[side], k.forearm[side]);
    const float lower = RestDistance(k.forearm[side], k.hand[side]);
    _lastShrugDegrees[side] = 0.0f;
    const float missing = Distance(wrist, Position(k.upperArm[side])) - REACH_SHARE * (upper + lower);
    const float radius = Distance(Position(k.upperArm[side]), Position(k.clavicle[side]));
    if (missing > 0.0f && radius > 0.03f)
    {
        float at[3], from[3], to[3];
        memcpy(at, Position(k.clavicle[side]), sizeof(at));
        for (int axis = 0; axis < 3; ++axis)
        {
            from[axis] = Position(k.upperArm[side])[axis] - at[axis];
            to[axis] = wrist[axis] - at[axis];
        }
        float a[3] = {from[0], from[1], from[2]}, b[3] = {to[0], to[1], to[2]};
        if (Normalize(a) && Normalize(b))
        {
            const float full = acosf(fmaxf(-1.0f, fminf(1.0f, Dot(a, b))));
            if (full > 0.01f)
            {
                const float wanted = fminf(missing / radius, MAXIMUM_SHRUG);
                _lastShrugDegrees[side] =
                    TurnSubtree(k.clavicle[side], at, from, to, fminf(1.0f, wanted / full)) * 57.2958f;
            }
        }
    }
    float shoulder[3], elbow[3], end[3];
    memcpy(shoulder, Position(k.upperArm[side]), sizeof(shoulder));
    const float pole[3] = {SIDE_X[side] * 0.5f, -1.0f, -0.3f}; // down, outward, a little back
    _lastReach[side] = SolveTwoBones(shoulder, wrist, upper, lower, pole, elbow, end);
    AimBone(k.upperArm[side], k.forearm[side], elbow);
    AimBone(k.forearm[side], k.hand[side], end);
    if (!handPosed)
    {
        return;
    }
    // the hand's roll about the forearm against where the forearm left it: the twist bones take their share
    float along[3] = {end[0] - elbow[0], end[1] - elbow[1], end[2] - elbow[2]};
    if (!Normalize(along))
    {
        return;
    }
    float ours[3] = {_posed[k.hand[side]][0], _posed[k.hand[side]][1], _posed[k.hand[side]][2]};
    float theirs[3] = {handPosed[0], handPosed[1], handPosed[2]};
    const float oursAlong = Dot(ours, along), theirsAlong = Dot(theirs, along);
    for (int axis = 0; axis < 3; ++axis)
    {
        ours[axis] -= along[axis] * oursAlong;
        theirs[axis] -= along[axis] * theirsAlong;
    }
    if (!Normalize(ours) || !Normalize(theirs))
    {
        return;
    }
    float cross[3];
    Cross(ours, theirs, cross);
    const float roll = atan2f(Dot(cross, along), Dot(ours, theirs));
    const int twists[2] = {k.forearmTwist[side], k.wristTwist[side]};
    const float shares[2] = {0.5f, 1.0f};
    for (int twist = 0; twist < 2; ++twist)
    {
        if (twists[twist] >= 0)
        {
            float m[16], at[3];
            memcpy(at, Position(twists[twist]), sizeof(at));
            RotationAbout(along, roll * shares[twist], at, m);
            MoveSubtree(twists[twist], m);
        }
    }
}

// One leg: the thigh and calf by two-bone IK to the ankle's target, the knee forward; the foot toward the toe's.
void SolveLeg(int side, const float* ankle, const float* toe)
{
    const Skeleton& k = _skeleton;
    const float upper = RestDistance(k.thigh[side], k.calf[side]);
    const float lower = RestDistance(k.calf[side], k.foot[side]);
    float hip[3], knee[3], end[3];
    memcpy(hip, Position(k.thigh[side]), sizeof(hip));
    const float pole[3] = {SIDE_X[side] * 0.15f, 0.0f, 1.0f};
    SolveTwoBones(hip, ankle, upper, lower, pole, knee, end);
    AimBone(k.thigh[side], k.calf[side], knee);
    AimBone(k.calf[side], k.foot[side], end);
    AimBone(k.foot[side], k.toe[side], toe);
}

// Places and sizes the body (modelToWorld, rows) and poses every bone; the skin matrices into _skin.
bool Solve(const Inputs& inputs, float* modelToWorld)
{
    const Skeleton& k = _skeleton;
    float head[16];
    if (!k.isFound || !inputs.gameMatrix || !Stereo::GetHeadWorld(head))
    {
        return false;
    }
    // place and size: the game's heading, our scale from the configured height (only), the neck over the anchor, the
    // feet on the game's ground
    float scale = (inputs.heightCentimeters * 0.01f * EYE_HEIGHT_SHARE) / PLAYER_BODY_EYE[1];
    scale = scale < 0.6f ? 0.6f : (scale > 1.3f ? 1.3f : scale);
    _lastScale = scale;
    memset(modelToWorld, 0, 16 * sizeof(float));
    for (int row = 0; row < 3; ++row)
    {
        float axis[3] = {inputs.gameMatrix[row * 4], inputs.gameMatrix[row * 4 + 1], inputs.gameMatrix[row * 4 + 2]};
        Normalize(axis);
        for (int column = 0; column < 3; ++column)
        {
            modelToWorld[row * 4 + column] = axis[column] * scale;
        }
    }
    modelToWorld[15] = 1.0f;
    const float* restNeck = PLAYER_BODY_BONES[k.neck].rest + 12;
    float neckOffset[3];
    TransformDirection(restNeck, modelToWorld, neckOffset);
    modelToWorld[12] = inputs.anchorWorld[0] - neckOffset[0];
    modelToWorld[13] = inputs.gameMatrix[13];
    modelToWorld[14] = inputs.anchorWorld[2] - neckOffset[2];
    float worldToModel[16];
    InvertAffine(worldToModel, modelToWorld);
    // the eyes and the head's axes in model space; the neck's target as far below and behind the eyes as at rest
    float eye[3], right[3], up[3], back[3];
    Transform(head + 12, worldToModel, eye);
    TransformDirection(head, worldToModel, right);
    TransformDirection(head + 4, worldToModel, up);
    TransformDirection(head + 8, worldToModel, back);
    Normalize(right);
    Normalize(up);
    Normalize(back);
    float neckTarget[3];
    for (int axis = 0; axis < 3; ++axis)
    { // at rest the head looks along +z: its right is -x, up +y, back -z
        neckTarget[axis] = eye[axis] - right[axis] * (restNeck[0] - PLAYER_BODY_EYE[0]) +
                           up[axis] * (restNeck[1] - PLAYER_BODY_EYE[1]) -
                           back[axis] * (restNeck[2] - PLAYER_BODY_EYE[2]);
    }
    for (int bone = 0; bone < BONE_COUNT; ++bone)
    {
        memcpy(_posed[bone], PLAYER_BODY_BONES[bone].rest, sizeof(_posed[bone]));
    }
    // the hips: as high as the spine leaves them under the neck's target, never above standing
    const float* restHips = PLAYER_BODY_BONES[k.spine[0]].rest + 12;
    const float spineLength = Distance(restNeck, restHips);
    const float dx = neckTarget[0] - restHips[0], dz = neckTarget[2] - restHips[2];
    const float across = sqrtf(dx * dx + dz * dz);
    float hips =
        across < spineLength ? neckTarget[1] - sqrtf(spineLength * spineLength - across * across) : neckTarget[1];
    hips = hips > restHips[1] ? restHips[1] : (hips < LOWEST_HIPS ? LOWEST_HIPS : hips);
    _lastHipsDrop = restHips[1] - hips;
    {
        const float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, hips - restHips[1], 0, 1};
        MoveSubtree(k.spine[0], m);
        MoveSubtree(k.pelvis, m);
    }
    float hipsAt[3];
    memcpy(hipsAt, Position(k.spine[0]), sizeof(hipsAt));
    { // the upper body turned after the head, half way, at most MAXIMUM_YAW
        float forward[3] = {-back[0], 0.0f, -back[2]};
        float yaw = 0.0f;
        if (Normalize(forward))
        {
            yaw = atan2f(forward[0], forward[2]) * 0.5f;
            yaw = yaw > MAXIMUM_YAW ? MAXIMUM_YAW : (yaw < -MAXIMUM_YAW ? -MAXIMUM_YAW : yaw);
        }
        const float vertical[3] = {0, 1, 0};
        float m[16];
        RotationAbout(vertical, yaw, hipsAt, m);
        MoveSubtree(k.spine[0], m);
        _lastYawDegrees = yaw * 57.2958f;
    }
    { // the spine bent so the neck reaches its target: shares at the three spine joints, the rest at the hips
        const float shares[4] = {0.4f, 0.5f, 1.0f, 1.0f};
        float lean = 0.0f;
        for (int joint = 0; joint < 4; ++joint)
        {
            const int bone = joint < 3 ? k.spine[joint] : k.spine[0];
            float at[3], from[3], to[3];
            memcpy(at, Position(bone), sizeof(at));
            for (int axis = 0; axis < 3; ++axis)
            {
                from[axis] = Position(k.neck)[axis] - at[axis];
                to[axis] = neckTarget[axis] - at[axis];
            }
            lean += TurnSubtree(bone, at, from, to, shares[joint]);
        }
        _lastLeanDegrees = lean * 57.2958f;
    }
    // the arms to BodyArm's wrists (the hand and fingers as BodyArm posed them), or hanging without a pose
    for (int side = 0; side < 2; ++side)
    {
        float armToWorld[16];
        const float(*armSkin)[16] = nullptr;
        _lastHasHand[side] = BodyArm::GetHandPose(side, armToWorld, &armSkin);
        float wrist[3], handFrame[16];
        const float* handPosed = nullptr;
        if (_lastHasHand[side])
        {
            float armToModel[16];
            Multiply(armToModel, armToWorld, worldToModel);
            int handArm = 0;
            for (int arm = 0; arm < BodyArm::BoneCount(); ++arm)
            {
                const int ours = k.fromArm[side][arm];
                if (ours >= 0)
                { // our rest -> arm space at rest -> BodyArm's pose -> world -> model
                    Multiply(_handSkin[side][arm], k.modelToArm[side], armSkin[arm]);
                    Multiply(_handSkin[side][arm], _handSkin[side][arm], armToModel);
                    if (ours == k.hand[side])
                    {
                        handArm = arm;
                        handPosed = handFrame;
                    }
                }
            }
            const float origin[3] = {};
            Transform(origin, armToModel, wrist); // arm space's origin is the wrist
            // BodyArm's pose carries its own size (the game's first-person hands); ours come from the height alone, so
            // the hand and fingers are scaled back to the body's about the wrist, which stays on the controller
            const float* handSkin = _handSkin[side][handArm];
            const float size = sqrtf(Dot(handSkin, handSkin));
            _lastHandScale[side] = size;
            for (int arm = 0; size > 0.0f && arm < BodyArm::BoneCount(); ++arm)
            {
                if (k.fromArm[side][arm] >= 0)
                {
                    float* skin = _handSkin[side][arm];
                    for (int cell = 0; cell < 12; ++cell)
                    {
                        skin[cell] /= size;
                    }
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        skin[12 + axis] = (skin[12 + axis] - wrist[axis]) / size + wrist[axis];
                    }
                }
            }
            Multiply(handFrame, PLAYER_BODY_BONES[k.hand[side]].rest, _handSkin[side][handArm]);
        }
        else
        {
            const float* shoulder = Position(k.upperArm[side]);
            wrist[0] = shoulder[0] + SIDE_X[side] * 0.08f;
            wrist[1] = shoulder[1] - 0.6f;
            wrist[2] = shoulder[2] + 0.05f;
        }
        SolveArm(side, wrist, handPosed);
    }
    // the legs to the game's feet (its walk), else as at rest
    for (int side = 0; side < 2; ++side)
    {
        _lastHasFoot[side] = inputs.gameFoot[side] && inputs.gameToe[side];
        float ankle[3], toe[3];
        if (_lastHasFoot[side])
        {
            float world[3];
            Transform(inputs.gameFoot[side] + 12, inputs.gameMatrix, world);
            Transform(world, worldToModel, ankle);
            Transform(inputs.gameToe[side] + 12, inputs.gameMatrix, world);
            Transform(world, worldToModel, toe);
        }
        else
        {
            memcpy(ankle, PLAYER_BODY_BONES[k.foot[side]].rest + 12, sizeof(ankle));
            memcpy(toe, PLAYER_BODY_BONES[k.toe[side]].rest + 12, sizeof(toe));
        }
        SolveLeg(side, ankle, toe);
    }
    for (int bone = 0; bone < BONE_COUNT; ++bone)
    {
        Multiply(_skin[bone], PLAYER_BODY_BONES[bone].inverseRest, _posed[bone]);
    }
    for (int side = 0; side < 2; ++side)
    {
        for (int arm = 0; _lastHasHand[side] && arm < BodyArm::BoneCount(); ++arm)
        {
            if (k.fromArm[side][arm] >= 0)
            {
                memcpy(_skin[k.fromArm[side][arm]], _handSkin[side][arm], sizeof(_skin[0]));
            }
        }
    }
    return true;
}

bool Skin()
{
    const float(*skin)[16] = _skin;
    for (int part = 0; part < PART_COUNT; ++part)
    {
        const PlayerBodyPart& data = PLAYER_BODY_PARTS[part];
        Vertex* out = nullptr;
        if (FAILED(_vertexBuffers[part]->lpVtbl->Lock(_vertexBuffers[part], 0, 0, reinterpret_cast<void**>(&out), 0)))
        {
            return false;
        }
        for (unsigned index = 0; index < data.vertexCount; ++index)
        {
            const PlayerBodyVertex& vertex = data.vertices[index];
            float position[3] = {}, normal[3] = {};
            for (int influence = 0; influence < 4; ++influence)
            {
                const float weight = vertex.weights[influence];
                if (weight <= 0)
                {
                    continue;
                }
                const float* matrix = skin[vertex.bones[influence]];
                for (int column = 0; column < 3; ++column)
                {
                    position[column] +=
                        weight * (vertex.position[0] * matrix[column] + vertex.position[1] * matrix[4 + column] +
                                  vertex.position[2] * matrix[8 + column] + matrix[12 + column]);
                    normal[column] += weight * (vertex.normal[0] * matrix[column] + vertex.normal[1] * matrix[4 + column] +
                                                vertex.normal[2] * matrix[8 + column]);
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

} // namespace

bool IsWanted()
{
    return HAS_DATA && _isOn && !_hasFailed;
}

bool IsDrawingHands()
{
    return IsWanted() && _lastDrawTick && GetTickCount() - _lastDrawTick < 250;
}

bool Draw(void* device, const Inputs& inputs)
{
    if (!IsWanted() || !device)
    {
        return false;
    }
    LARGE_INTEGER start, end, frequency;
    QueryPerformanceCounter(&start);
    IDirect3DDevice9* d3dDevice = static_cast<IDirect3DDevice9*>(device);
    if (!_isSkeletonLooked)
    {
        _isSkeletonLooked = true;
        FindSkeleton();
    }
    if (!_skeleton.isFound || !Prepare(d3dDevice))
    {
        _hasFailed = true;
        Log::Warning("playerbody", "skeleton or buffers missing: our body not drawn");
        return false;
    }
    float modelToWorld[16];
    if (!Solve(inputs, modelToWorld) || !Skin())
    {
        return false;
    }
    _lastDrawTick = GetTickCount();
    if (!_savedState && FAILED(d3dDevice->lpVtbl->CreateStateBlock(d3dDevice, D3DSBT_ALL, &_savedState)))
    {
        _savedState = nullptr;
        return false;
    }
    if (FAILED(_savedState->lpVtbl->Capture(_savedState)))
    {
        return false;
    }
    DrawState::Reset(d3dDevice); // a known state, nothing inherited from the game's last draw
    d3dDevice->lpVtbl->SetVertexShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetPixelShader(d3dDevice, nullptr);
    d3dDevice->lpVtbl->SetFVF(d3dDevice, VERTEX_FORMAT);
    D3DMATRIX worldMatrix;
    memcpy(&worldMatrix, modelToWorld, sizeof(worldMatrix));
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
    // As BodyArm: one light from above and in front of the world plus the ambient (the game's sun is not read yet).
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
    for (int part = 0; part < PART_COUNT; ++part)
    {
        const PlayerBodyPart& data = PLAYER_BODY_PARTS[part];
        IDirect3DTexture9* texture = data.ownTexture >= 0
                                         ? _textures[data.ownTexture]
                                         : static_cast<IDirect3DTexture9*>(Textures::FindByName(data.gameTexture));
        d3dDevice->lpVtbl->SetTexture(d3dDevice, 0, reinterpret_cast<IDirect3DBaseTexture9*>(texture));
        d3dDevice->lpVtbl
            ->SetTextureStageState(d3dDevice, 0, D3DTSS_COLOROP, texture ? D3DTOP_MODULATE : D3DTOP_SELECTARG2);
        d3dDevice->lpVtbl->SetStreamSource(d3dDevice, 0, _vertexBuffers[part], 0, sizeof(Vertex));
        d3dDevice->lpVtbl->SetIndices(d3dDevice, _indexBuffers[part]);
        d3dDevice->lpVtbl->DrawIndexedPrimitive(
            d3dDevice, D3DPT_TRIANGLELIST, 0, 0, data.vertexCount, 0, data.indexCount / 3);
    }
    _savedState->lpVtbl->Apply(_savedState);
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);
    const double milliseconds =
        static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    _costMilliseconds += milliseconds;
    _costMaximumMilliseconds = milliseconds > _costMaximumMilliseconds ? milliseconds : _costMaximumMilliseconds;
    if (++_costDraws >= COST_LOG_DRAWS || _shouldLog)
    {
        _shouldLog = false;
        Log::Info(
            "playerbody",
            "body draw cost: %.3f ms per eye pass on average, %.3f at most; scale %.3f, hips down %.2f, upper body "
            "turned %.0f deg, leaning %.0f deg; reach right %.2f left %.2f, collarbones right %.0f left %.0f deg; "
            "hands from BodyArm right %d left %d (BodyArm's size undone: %.3f %.3f), feet from the game right %d left %d; origin (%.1f %.1f %.1f)",
            _costMilliseconds / _costDraws,
            _costMaximumMilliseconds,
            static_cast<double>(_lastScale),
            static_cast<double>(_lastHipsDrop),
            static_cast<double>(_lastYawDegrees),
            static_cast<double>(_lastLeanDegrees),
            static_cast<double>(_lastReach[RIGHT]),
            static_cast<double>(_lastReach[LEFT]),
            static_cast<double>(_lastShrugDegrees[RIGHT]),
            static_cast<double>(_lastShrugDegrees[LEFT]),
            _lastHasHand[RIGHT] ? 1 : 0,
            _lastHasHand[LEFT] ? 1 : 0,
            static_cast<double>(_lastHandScale[RIGHT]),
            static_cast<double>(_lastHandScale[LEFT]),
            _lastHasFoot[RIGHT] ? 1 : 0,
            _lastHasFoot[LEFT] ? 1 : 0,
            static_cast<double>(modelToWorld[12]),
            static_cast<double>(modelToWorld[13]),
            static_cast<double>(modelToWorld[14]));
        _costMilliseconds = _costMaximumMilliseconds = 0;
        _costDraws = 0;
    }
    return true;
}

void Configure(const char* arguments)
{
    if (!strncmp(arguments, "log", 3))
    {
        _shouldLog = true;
        return;
    }
    _isOn = atoi(arguments) != 0;
    _hasFailed = false;
    Log::Info("playerbody", "our body %s%s", _isOn ? "on" : "off", HAS_DATA ? "" : " (not built in: no data)");
}

void OnReset()
{
    if (_savedState)
    {
        _savedState->lpVtbl->Release(_savedState);
        _savedState = nullptr;
    }
}

} // namespace PlayerBody

} // namespace SwbfVr
