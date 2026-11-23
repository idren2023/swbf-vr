// HudDraws.cpp: the game's HUD element draws, caught as they are made (see HudDraws.h; engine facts in
// docs/offsets.md, "HUD element draws").
//
// RedInterfaceElement::Draw (ElementDraw, thiscall: parent matrix*, parent colour*) is the per-element step of the
// interface tree: our hook keeps a stack of the elements being drawn, so each leaf draw knows its group.
// RedBitmapElement::Render (BitmapRender) and RedGlyphCacheElement::Render (GlyphRender) get the element's final
// world matrix and colour. A bitmap's four corners are at element+0x88 + i * 0x1C (x, y, z, u, v, u2, v2; drawn as a
// strip); its material is [element+0xF8], whose +0x18 is the texture's name hash. Text: per page p, the glyph count
// [[element+0x8C]+4p], the material [[element+0x94]+4p] and a chain of vertex chunks from [[element+0x90]+4p]
// (48 vertices of x y z u v, used count at +0x3C0, next chunk at +0x3C4), 6 vertices per glyph (triangle list).
//
// A group (HealthGroup, AmmoGroup) is recognised by its class name (the vtable's RTTI), so no vtable addresses are
// needed. The matrix its children are drawn with is the group's world matrix; every vertex of the group is taken
// through its element's world matrix and back through the inverse of the group's: that removes the slant the game
// gives the two boxes (a real 3D rotation through the HUD camera) and leaves the group flat in its own plane.
//
// Projected with the HUD camera (horizontal factor 1.7320508, vertical 2.3094, looking down -z) the world positions
// give the place on screen as fractions (u across, v down, like the table in Hud.cpp).
#include "HudDraws.h"

#include "GameBuild.h"
#include "Log.h"
#include "Stereo.h"

#include <windows.h>

#include <MinHook.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace SwbfVr
{

namespace HudDraws
{

namespace
{

/// The HUD camera's projection factors (globals 0x6F3104 / 0x6F3100, set up in 0x298D50).
constexpr float HUD_PROJECTION_X = 1.7320508f;
constexpr float HUD_PROJECTION_Y = 2.3094f;
constexpr int STACK_DEPTH = 32;
constexpr int RECORD_COUNT = 600;
constexpr int ANCESTOR_COUNT = 3;
constexpr int GROUP_COUNT = static_cast<int>(Group::Count);
constexpr int BATCH_CAPACITY = 64;    // per group
constexpr int VERTEX_CAPACITY = 1536; // per group
constexpr int CLASS_CACHE_SIZE = 64;
/// KindOf's answers besides a Group.
constexpr int KIND_OTHER = -1;
/// The reticle's enemy and friend colours (offsets "Reticle target relation"), which the target's bar is drawn in.
constexpr unsigned TARGET_ENEMY_RGB = 0xDF2020;
constexpr unsigned TARGET_FRIEND_RGB = 0x20DF20;
constexpr int KIND_RETICLE_DISPLAY = -2;
constexpr int KIND_TEAM_DISPLAY = -3;
constexpr int KIND_SCREEN_GROUP = -4;
/// TeamDisplay: the screen group holding the score's bars and texts (offsets "HUD element owners", run188).
constexpr uintptr_t OFFSET_teamDisplayScoreGroup = 0x40;
constexpr int KIND_MAP_DISPLAY = -5;
constexpr int KIND_STATUS_DISPLAY = -6;
/// StatusDisplay: its HealthGroup (offsets "Displays"); the display is found from the group (for Squad's display).
constexpr uintptr_t OFFSET_statusDisplayHealthGroup = 0x130;
/// MapDisplay: the screen group holding the minimap's background and icons (offsets "HUD element owners").
constexpr uintptr_t OFFSET_mapDisplayGroup = 0x30;
/// RedMaskedBitmapElement (the minimap's background): its second material, whose +0x1C is the mask's texture hash.
constexpr uintptr_t OFFSET_maskedMaterial = 0x100;
constexpr uintptr_t OFFSET_materialMaskHash = 0x1C;
/// ReticuleDisplay: its reticle elements, one per weapon channel (primary, secondary).
constexpr uintptr_t OFFSET_reticleDisplayElements = 0xE0;
constexpr uintptr_t RETICLE_ELEMENT_STRIDE = 0x100;
constexpr int RETICLE_CHANNEL_COUNT = 2;
/// Beyond this offset from the exe's base an address is not in its image (SizeOfImage 0x2224000).
constexpr uintptr_t MAXIMUM_IMAGE_OFFSET = 0x2224000;
/// An interface frame older than this is stale (the tree is not drawn: menus, loading).
constexpr DWORD FRESH_MILLISECONDS = 250;

/// Element and material fields.
constexpr uintptr_t OFFSET_bitmapVertices = 0x88;
constexpr uintptr_t BITMAP_VERTEX_SIZE = 0x1C;
constexpr uintptr_t OFFSET_bitmapMaterial = 0xF8;
constexpr uintptr_t OFFSET_glyphCounts = 0x8C;
constexpr uintptr_t OFFSET_glyphChunks = 0x90;
constexpr uintptr_t OFFSET_glyphMaterials = 0x94;
constexpr uintptr_t OFFSET_glyphRenderer = 0x80;
constexpr uintptr_t CHUNK_VERTEX_COUNT = 48;
constexpr uintptr_t GLYPH_VERTEX_SIZE = 0x14;
constexpr uintptr_t OFFSET_chunkUsed = 0x3C0;
constexpr uintptr_t OFFSET_chunkNext = 0x3C4;
constexpr uintptr_t OFFSET_materialTextureHash = 0x18;
/// Texture object (docs/offsets.md "Zoom"): +0x18 kind (1 = 2D), +0x1C the Direct3D texture.
constexpr uintptr_t OFFSET_textureKind = 0x18;
constexpr uintptr_t OFFSET_textureD3d = 0x1C;
constexpr int TEXTURE_KIND_2D = 1;

struct Record
{
    bool isText;
    uintptr_t element;
    uintptr_t elementTable;              // vtable, as an offset in the exe
    uintptr_t ancestors[ANCESTOR_COUNT]; // nearest first: vtable offsets
    uintptr_t parent;                    // the nearest ancestor element (address)
    unsigned texture;
    unsigned colour;
    float left, top, right, bottom; // fractions of the screen; for text: the element's origin only
};

/// One group's drawing, built during the interface frame.
struct GroupState
{
    GroupBatch batches[BATCH_CAPACITY];
    GroupVertex vertices[VERTEX_CAPACITY];
    int batchCount;
    int vertexCount;
    bool hasWorld;
    float inverseWorld[16]; // the group's world matrix inverted
    float minX, minY, maxX, maxY;
    float screenLeft, screenTop, screenRight, screenBottom;
    // Sums for how the plane's axes follow the screen: x against u, y against v.
    double sumX, sumY, sumU, sumV, sumXU, sumYV;
    // The minimap's background, which gives the group its size: centre and half sides in the plane (its edges'
    // lengths, so a turning map keeps its size).
    bool hasFrame;
    float frameCenterX, frameCenterY, frameHalfWidth, frameHalfHeight;
};

using DrawFunction = void(__fastcall*)(void* self, void* edx, const float* matrix, const void* colour);
DrawFunction _originalElementDraw = nullptr, _originalBitmapRender = nullptr, _originalGlyphRender = nullptr,
             _originalMaskedBitmapRender = nullptr;
using TextureByHashFunction = uintptr_t(__cdecl*)(unsigned);
TextureByHashFunction _textureByHash = nullptr;

uintptr_t _exeBase = 0;
bool _isInstalled = false;
bool _hasInstallFailed = false;

// The element stack and the group each level belongs to (-1 none).
uintptr_t _stack[STACK_DEPTH];
int _stackGroup[STACK_DEPTH];
int _depth = 0;

// The interface frame: it starts again when the first root of the last frame comes round again.
uintptr_t _firstRoot = 0;
DWORD _frameTick = 0;
GroupState _groups[GROUP_COUNT];
unsigned _takenMask = 0;
bool _isReticleHidden = false; // SetReticleHidden

// Class of a vtable: -1 not a group, else the Group. Looked up once per vtable.
struct ClassEntry
{
    uintptr_t table;
    int group;
};

ClassEntry _classCache[CLASS_CACHE_SIZE];
int _classCacheCount = 0;
uintptr_t _statusDisplay = 0; // the StatusDisplay its HealthGroup was last drawn from

// The diagnostic dump.
volatile LONG _isRequested = 0;
bool _isCapturing = false;
Record _records[RECORD_COUNT];
int _recordCount = 0;

bool IsReadable(uintptr_t address, unsigned size)
{
    return address && !IsBadReadPtr(reinterpret_cast<const void*>(address), size);
}

uintptr_t TableOf(uintptr_t object)
{
    if (!IsReadable(object, 4))
    {
        return 0;
    }
    const uintptr_t table = *reinterpret_cast<uintptr_t*>(object);
    return table > _exeBase ? table - _exeBase : table;
}

/// The kind of an object's class, by its RTTI name (vtable[-1] = complete object locator, +0xC its type
/// descriptor, whose +8 is the decorated name): a Group, KIND_RETICLE_DISPLAY or KIND_OTHER.
int KindOf(uintptr_t element)
{
    if (!IsReadable(element, 4))
    {
        return -1;
    }
    const uintptr_t table = *reinterpret_cast<uintptr_t*>(element);
    for (int index = 0; index < _classCacheCount; ++index)
    {
        if (_classCache[index].table == table)
        {
            return _classCache[index].group;
        }
    }
    int group = KIND_OTHER;
    // Locator and descriptor are only followed inside the exe's image: a word there that was no vtable gave a
    // locator of 9, and IsBadReadPtr's fault on it went to the crash log (first-chance, caught; run191).
    const auto isInImage = [](uintptr_t address)
    {
        return address >= _exeBase && address - _exeBase <= MAXIMUM_IMAGE_OFFSET;
    };
    if (isInImage(table) && IsReadable(table - 4, 4))
    {
        const uintptr_t locator = *reinterpret_cast<uintptr_t*>(table - 4);
        if (isInImage(locator) && IsReadable(locator + 0xC, 4))
        {
            const uintptr_t descriptor = *reinterpret_cast<uintptr_t*>(locator + 0xC);
            if (isInImage(descriptor) && IsReadable(descriptor + 8, 32))
            {
                const char* name = reinterpret_cast<const char*>(descriptor + 8);
                if (strncmp(name, ".?AVHealthGroup@@", 17) == 0)
                {
                    group = static_cast<int>(Group::Health);
                }
                else if (strncmp(name, ".?AVAmmoGroup@@", 15) == 0)
                {
                    group = static_cast<int>(Group::Ammunition);
                }
                else if (strncmp(name, ".?AVReticuleDisplay@@", 21) == 0)
                {
                    group = KIND_RETICLE_DISPLAY;
                }
                else if (strncmp(name, ".?AVTeamDisplay@@", 17) == 0)
                {
                    group = KIND_TEAM_DISPLAY;
                }
                else if (strncmp(name, ".?AVMapDisplay@@", 16) == 0)
                {
                    group = KIND_MAP_DISPLAY;
                }
                else if (strncmp(name, ".?AVStatusDisplay@@", 19) == 0)
                {
                    group = KIND_STATUS_DISPLAY;
                }
                else if (strncmp(name, ".?AVRedScreenGroupElement@@", 27) == 0)
                {
                    group = KIND_SCREEN_GROUP;
                }
            }
        }
    }
    if (_classCacheCount < CLASS_CACHE_SIZE)
    {
        _classCache[_classCacheCount++] = ClassEntry{table, group};
    }
    return group;
}

/// True when an object's vtable lies in the exe's image: only such words are followed (other words there made
/// IsBadReadPtr fault, first-chance, caught, but written to the crash log; run187).
bool HasImageTable(uintptr_t object)
{
    const uintptr_t table = IsReadable(object, 4) ? *reinterpret_cast<uintptr_t*>(object) : 0;
    return table >= _exeBase && table - _exeBase <= MAXIMUM_IMAGE_OFFSET;
}

/// The Group an element is the root of, or -1: HealthGroup and AmmoGroup by their class, the score and the minimap by
/// their place (the screen groups TeamDisplay holds at +0x40 and MapDisplay at +0x30; others hold the reticle).
int GroupOf(uintptr_t element)
{
    const int kind = KindOf(element);
    if (kind == static_cast<int>(Group::Health) && element > OFFSET_statusDisplayHealthGroup &&
        HasImageTable(element - OFFSET_statusDisplayHealthGroup) &&
        KindOf(element - OFFSET_statusDisplayHealthGroup) == KIND_STATUS_DISPLAY)
    {
        _statusDisplay = element - OFFSET_statusDisplayHealthGroup;
    }
    if (kind >= 0)
    {
        return kind;
    }
    if (kind == KIND_SCREEN_GROUP && HasImageTable(element - OFFSET_teamDisplayScoreGroup) &&
        KindOf(element - OFFSET_teamDisplayScoreGroup) == KIND_TEAM_DISPLAY)
    {
        return static_cast<int>(Group::Score);
    }
    if (kind == KIND_SCREEN_GROUP && HasImageTable(element - OFFSET_mapDisplayGroup) &&
        KindOf(element - OFFSET_mapDisplayGroup) == KIND_MAP_DISPLAY)
    {
        return static_cast<int>(Group::Map);
    }
    return -1;
}

/// True for the weapon reticle's bitmap: ReticuleDisplay (0x423800) holds one per weapon channel, embedded at
/// +0xE0 + 0x100 * channel (offsets "Reticle target relation": the target colour is written to that element's +0x70).
/// Seen in run127: the first draw of the frame, texture reticule_rifle, colour = target relation.
bool IsReticleElement(uintptr_t element)
{
    for (int channel = 0; channel < RETICLE_CHANNEL_COUNT; ++channel)
    {
        const uintptr_t display = element - OFFSET_reticleDisplayElements - channel * RETICLE_ELEMENT_STRIDE;
        if (HasImageTable(display) && KindOf(display) == KIND_RETICLE_DISPLAY)
        {
            return true;
        }
    }
    return false;
}

/// Point through a row-vector matrix (16 floats).
void Transform(const float* matrix, float x, float y, float z, float* out)
{
    out[0] = x * matrix[0] + y * matrix[4] + z * matrix[8] + matrix[12];
    out[1] = x * matrix[1] + y * matrix[5] + z * matrix[9] + matrix[13];
    out[2] = x * matrix[2] + y * matrix[6] + z * matrix[10] + matrix[14];
}

/// Inverse of an affine row-vector matrix (3x3 part general, last column 0 0 0 1).
bool InvertAffine(const float* matrix, float* inverse)
{
    const float a = matrix[0], b = matrix[1], c = matrix[2], d = matrix[4], e = matrix[5], f = matrix[6], g = matrix[8],
                h = matrix[9], i = matrix[10];
    const float determinant = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (determinant > -1e-12f && determinant < 1e-12f)
    {
        return false;
    }
    const float scale = 1.0f / determinant;
    inverse[0] = (e * i - f * h) * scale;
    inverse[1] = (c * h - b * i) * scale;
    inverse[2] = (b * f - c * e) * scale;
    inverse[4] = (f * g - d * i) * scale;
    inverse[5] = (a * i - c * g) * scale;
    inverse[6] = (c * d - a * f) * scale;
    inverse[8] = (d * h - e * g) * scale;
    inverse[9] = (b * g - a * h) * scale;
    inverse[10] = (a * e - b * d) * scale;
    inverse[3] = inverse[7] = inverse[11] = 0;
    for (int column = 0; column < 3; ++column)
    {
        inverse[12 + column] =
            -(matrix[12] * inverse[column] + matrix[13] * inverse[4 + column] + matrix[14] * inverse[8 + column]);
    }
    inverse[15] = 1;
    return true;
}

/// A world point through the HUD camera, as fractions of the screen.
void ProjectWorld(const float* point, float* u, float* v)
{
    // The HUD camera looks down -z (run127: with +z every box came out mirrored on both axes).
    const float depth = point[2] < -1e-4f || point[2] > 1e-4f ? -point[2] : 1e-4f;
    *u = (HUD_PROJECTION_X * point[0] / depth + 1.0f) * 0.5f;
    *v = (1.0f - HUD_PROJECTION_Y * point[1] / depth) * 0.5f;
}

/// One point of the element through its world matrix and the HUD camera.
void Project(const float* world, float x, float y, float z, float* u, float* v)
{
    float point[3];
    Transform(world, x, y, z, point);
    ProjectWorld(point, u, v);
}

/// The Direct3D texture of a texture name hash (looked up in the game's table), or null.
void* TextureOfHash(unsigned hash)
{
    if (!_textureByHash || hash == 0)
    {
        return nullptr;
    }
    const uintptr_t texture = _textureByHash(hash);
    if (!IsReadable(texture, 0x20) || *reinterpret_cast<int*>(texture + OFFSET_textureKind) != TEXTURE_KIND_2D)
    {
        return nullptr;
    }
    return *reinterpret_cast<void**>(texture + OFFSET_textureD3d);
}

/// The Direct3D texture behind a material (its texture name hash looked up in the game's table), or null.
void* TextureOfMaterial(uintptr_t material, unsigned* hash)
{
    *hash = 0;
    if (!_textureByHash || !IsReadable(material + OFFSET_materialTextureHash, 4))
    {
        return nullptr;
    }
    *hash = *reinterpret_cast<unsigned*>(material + OFFSET_materialTextureHash);
    const uintptr_t texture = _textureByHash(*hash);
    if (!IsReadable(texture, 0x20) || *reinterpret_cast<int*>(texture + OFFSET_textureKind) != TEXTURE_KIND_2D)
    {
        return nullptr;
    }
    return *reinterpret_cast<void**>(texture + OFFSET_textureD3d);
}

void StartFrame(uintptr_t root)
{
    _firstRoot = root;
    _frameTick = GetTickCount();
    for (GroupState& state : _groups)
    {
        state.batchCount = 0;
        state.vertexCount = 0;
        state.hasWorld = false;
        state.hasFrame = false;
        state.minX = state.minY = state.screenLeft = state.screenTop = 1e9f;
        state.sumX = state.sumY = state.sumU = state.sumV = state.sumXU = state.sumYV = 0;
        state.maxX = state.maxY = state.screenRight = state.screenBottom = -1e9f;
    }
}

/// The target's health bar goes through the HealthGroup too and ended up in the wrist hologram (headset run 12, PLAN
/// 28). The player's own health draws are white or blue (0x0098FF; every dump so far); a clearly red or green colour
/// (exactly the reticle's enemy 0xDF2020 / friend 0x20DF20) is taken to be the target's bar. Inferred, not yet seen in
/// a dump: the bar did not show while aiming at a friend on the desktop (run152).
bool IsTargetColour(const void* colour)
{
    if (!IsReadable(reinterpret_cast<uintptr_t>(colour), 4))
    {
        return false;
    }
    const unsigned argb = *reinterpret_cast<const unsigned*>(colour);
    // Exactly the reticle's enemy / friend colour (alpha aside): the player's own health turns pink-red when hurt
    // (0xCB7A87, run189), which a looser test took for the target's bar.
    const unsigned rgb = argb & 0xFFFFFF;
    const bool isTarget = rgb == TARGET_ENEMY_RGB || rgb == TARGET_FRIEND_RGB;
    static bool hasSaid = false;
    if (isTarget && !hasSaid)
    {
        hasSaid = true;
        Log::Info("huddraws", "health group draw in colour %08X: taken as the target's bar (group Target)", argb);
    }
    return isTarget;
}

/// Starts a batch of the current element's group; null when the element is in no group or there is no room.
GroupState* GroupOfCurrent(int* groupIndex)
{
    *groupIndex = _depth > 0 && _depth <= STACK_DEPTH ? _stackGroup[_depth - 1] : -1;
    return *groupIndex >= 0 && _groups[*groupIndex].hasWorld ? &_groups[*groupIndex] : nullptr;
}

/// GroupOfCurrent, with the target's bar split off the HealthGroup into Group::Target (same plane: the health group's
/// matrix). The first frame with it is logged whole (`hud draws`), to see its pieces (PLAN 11, 28).
GroupState* GroupOfDraw(int* groupIndex, const void* colour)
{
    GroupState* state = GroupOfCurrent(groupIndex);
    if (*groupIndex != static_cast<int>(Group::Health) || !IsTargetColour(colour))
    {
        return state;
    }
    static bool hasAskedForDump = false;
    if (!hasAskedForDump)
    {
        hasAskedForDump = true;
        InterlockedExchange(&_isRequested, 1);
    }
    *groupIndex = static_cast<int>(Group::Target);
    GroupState& target = _groups[*groupIndex];
    if (!state)
    {
        return nullptr;
    }
    if (!target.hasWorld)
    {
        memcpy(target.inverseWorld, state->inverseWorld, sizeof(target.inverseWorld));
        target.hasWorld = true;
    }
    return &target;
}

/// Adds one vertex (element space) to the group's open batch.
void AddVertex(
    GroupState& state,
    const float* world,
    float x,
    float y,
    float z,
    float u,
    float v,
    float maskU = 0,
    float maskV = 0)
{
    if (state.vertexCount >= VERTEX_CAPACITY)
    {
        return;
    }
    float point[3], local[3];
    Transform(world, x, y, z, point);
    float screenU = 0, screenV = 0;
    ProjectWorld(point, &screenU, &screenV);
    Transform(state.inverseWorld, point[0], point[1], point[2], local);
    state.sumX += local[0];
    state.sumY += local[1];
    state.sumU += screenU;
    state.sumV += screenV;
    state.sumXU += static_cast<double>(local[0]) * screenU;
    state.sumYV += static_cast<double>(local[1]) * screenV;
    state.vertices[state.vertexCount++] = GroupVertex{local[0], local[1], u, v, maskU, maskV};
    state.minX = local[0] < state.minX ? local[0] : state.minX;
    state.maxX = local[0] > state.maxX ? local[0] : state.maxX;
    state.minY = local[1] < state.minY ? local[1] : state.minY;
    state.maxY = local[1] > state.maxY ? local[1] : state.maxY;
    state.screenLeft = screenU < state.screenLeft ? screenU : state.screenLeft;
    state.screenRight = screenU > state.screenRight ? screenU : state.screenRight;
    state.screenTop = screenV < state.screenTop ? screenV : state.screenTop;
    state.screenBottom = screenV > state.screenBottom ? screenV : state.screenBottom;
}

GroupBatch* OpenBatch(GroupState& state, void* d3dTexture, const void* colour)
{
    if (!d3dTexture || state.batchCount >= BATCH_CAPACITY)
    {
        return nullptr;
    }
    GroupBatch& batch = state.batches[state.batchCount++];
    batch.d3dTexture = d3dTexture;
    batch.d3dMask = nullptr;
    batch.colour = IsReadable(reinterpret_cast<uintptr_t>(colour), 4) ? *static_cast<const unsigned*>(colour) : 0;
    batch.firstVertex = state.vertexCount;
    batch.vertexCount = 0;
    return &batch;
}

void CloseBatch(GroupState& state, GroupBatch* batch)
{
    batch->vertexCount = state.vertexCount - batch->firstVertex;
    batch->vertexCount -= batch->vertexCount % 3;
    if (batch->vertexCount == 0)
    {
        --state.batchCount;
    }
}

Record* NewRecord(void* self, const float* world, const void* colour, bool isText)
{
    if (!_isCapturing || _recordCount >= RECORD_COUNT || !IsReadable(reinterpret_cast<uintptr_t>(world), 64))
    {
        return nullptr;
    }
    Record& record = _records[_recordCount++];
    record = Record{};
    record.isText = isText;
    record.element = reinterpret_cast<uintptr_t>(self);
    record.elementTable = TableOf(record.element);
    int found = 0;
    for (int level = (_depth < STACK_DEPTH ? _depth : STACK_DEPTH) - 1; level >= 0 && found < ANCESTOR_COUNT; --level)
    {
        if (_stack[level] != record.element)
        {
            record.parent = found == 0 ? _stack[level] : record.parent;
            record.ancestors[found++] = TableOf(_stack[level]);
        }
    }
    record.colour = IsReadable(reinterpret_cast<uintptr_t>(colour), 4) ? *static_cast<const unsigned*>(colour) : 0;
    Project(world, 0, 0, 0, &record.left, &record.top);
    record.right = record.left;
    record.bottom = record.top;
    return &record;
}

/// Diagnostic (PLAN 11 a, b): the object an element is embedded in, found by scanning back from it for a word that is
/// a vtable whose RTTI name ends in "Display" (StatusDisplay, TeamDisplay, MapDisplay, ReticuleDisplay...). Returns
/// the offset of the element in it and the class name, or 0.
uintptr_t OwningDisplay(uintptr_t element, const char** name)
{
    constexpr uintptr_t SCAN_BYTES = 0x4000;
    for (uintptr_t offset = 4; offset <= SCAN_BYTES && element > offset; offset += 4)
    {
        const uintptr_t candidate = element - offset;
        if ((candidate & 0xFFF) == 0xFFC && !IsReadable(candidate, 4))
        {
            return 0; // the page before is not ours to read
        }
        const uintptr_t table = *reinterpret_cast<uintptr_t*>(candidate);
        if (table < _exeBase || table - _exeBase > MAXIMUM_IMAGE_OFFSET || !IsReadable(table - 4, 4))
        {
            continue;
        }
        const uintptr_t locator = *reinterpret_cast<uintptr_t*>(table - 4);
        if (locator < _exeBase || locator - _exeBase > MAXIMUM_IMAGE_OFFSET || !IsReadable(locator + 0xC, 4))
        {
            continue;
        }
        const uintptr_t descriptor = *reinterpret_cast<uintptr_t*>(locator + 0xC);
        if (descriptor < _exeBase || descriptor - _exeBase > MAXIMUM_IMAGE_OFFSET || !IsReadable(descriptor + 8, 40))
        {
            continue;
        }
        const char* decorated = reinterpret_cast<const char*>(descriptor + 8);
        const char* end = static_cast<const char*>(memchr(decorated, '@', 40));
        if (end && end - decorated > 11 && strncmp(end - 7, "Display", 7) == 0)
        {
            *name = decorated;
            return offset;
        }
    }
    return 0;
}

void LogFrame()
{
    Log::Info("huddraws", "one interface frame: %d draws (vtables as exe offsets, textures as hashes)", _recordCount);
    for (int index = 0; index < _recordCount; ++index)
    {
        const Record& record = _records[index];
        Log::Info(
            "huddraws",
            "%3d %s el %08x vt %06x up %06x %06x %06x tex %08x col %08x u %.3f-%.3f v %.3f-%.3f",
            index,
            record.isText ? "text" : "bmp ",
            static_cast<unsigned>(record.element),
            static_cast<unsigned>(record.elementTable),
            static_cast<unsigned>(record.ancestors[0]),
            static_cast<unsigned>(record.ancestors[1]),
            static_cast<unsigned>(record.ancestors[2]),
            record.texture,
            record.colour,
            static_cast<double>(record.left),
            static_cast<double>(record.right),
            static_cast<double>(record.top),
            static_cast<double>(record.bottom));
        const char* ownerName = "";
        const char* parentOwnerName = "";
        const uintptr_t ownerOffset = OwningDisplay(record.element, &ownerName);
        const uintptr_t parentOwnerOffset = record.parent ? OwningDisplay(record.parent, &parentOwnerName) : 0;
        Log::Info(
            "huddraws",
            "    parent %08x; element at +%x in %.32s; parent at +%x in %.32s",
            static_cast<unsigned>(record.parent),
            static_cast<unsigned>(ownerOffset),
            ownerName,
            static_cast<unsigned>(parentOwnerOffset),
            parentOwnerName);
    }
    for (int group = 0; group < GROUP_COUNT; ++group)
    {
        const GroupState& state = _groups[group];
        if (state.hasFrame)
        {
            Log::Info(
                "huddraws",
                "group %d background: centre %.1f %.1f, half sides %.1f x %.1f",
                group,
                static_cast<double>(state.frameCenterX),
                static_cast<double>(state.frameCenterY),
                static_cast<double>(state.frameHalfWidth),
                static_cast<double>(state.frameHalfHeight));
        }
        Log::Info(
            "huddraws",
            "group %d: %d batches, %d vertices, plane x %.1f..%.1f y %.1f..%.1f, screen u %.3f-%.3f v %.3f-%.3f, "
            "x with u %+.1f, y with v %+.1f",
            group,
            state.batchCount,
            state.vertexCount,
            static_cast<double>(state.minX),
            static_cast<double>(state.maxX),
            static_cast<double>(state.minY),
            static_cast<double>(state.maxY),
            static_cast<double>(state.screenLeft),
            static_cast<double>(state.screenRight),
            static_cast<double>(state.screenTop),
            static_cast<double>(state.screenBottom),
            state.sumXU - state.sumX * state.sumU / (state.vertexCount ? state.vertexCount : 1),
            state.sumYV - state.sumY * state.sumV / (state.vertexCount ? state.vertexCount : 1));
    }
}

void __fastcall ElementDrawHook(void* self, void* edx, const float* parentMatrix, const void* parentColour)
{
    const uintptr_t element = reinterpret_cast<uintptr_t>(self);
    if (_depth == 0 && (element == _firstRoot || GetTickCount() - _frameTick > FRESH_MILLISECONDS))
    { // the tree starts again: one whole frame has been seen
        if (_isCapturing)
        {
            _isCapturing = false;
            LogFrame();
        }
        else if (InterlockedExchange(&_isRequested, 0))
        {
            _isCapturing = true;
            _recordCount = 0;
        }
        StartFrame(element);
    }
    // The group of this level: the element's own if it is a group, else its parent's. The matrix a group's children
    // are drawn with is the group's world matrix.
    const int parentGroup = _depth > 0 && _depth <= STACK_DEPTH ? _stackGroup[_depth - 1] : -1;
    const int ownGroup = GroupOf(element);
    if (parentGroup >= 0 && _depth > 0 && _depth <= STACK_DEPTH && GroupOf(_stack[_depth - 1]) == parentGroup &&
        !_groups[parentGroup].hasWorld && IsReadable(reinterpret_cast<uintptr_t>(parentMatrix), 64))
    {
        _groups[parentGroup].hasWorld = InvertAffine(parentMatrix, _groups[parentGroup].inverseWorld);
    }
    // The minimap's group turns with the player (its own matrix, 0x18E320): its plane is the one it is drawn in, so the
    // map turns on the wrist as it does on screen and the axis signs stay put.
    if (ownGroup == static_cast<int>(Group::Map) && !_groups[ownGroup].hasWorld &&
        IsReadable(reinterpret_cast<uintptr_t>(parentMatrix), 64))
    {
        _groups[ownGroup].hasWorld = InvertAffine(parentMatrix, _groups[ownGroup].inverseWorld);
    }
    if (_depth < STACK_DEPTH)
    {
        _stack[_depth] = element;
        _stackGroup[_depth] = ownGroup >= 0 ? ownGroup : parentGroup;
    }
    ++_depth;
    _originalElementDraw(self, edx, parentMatrix, parentColour);
    --_depth;
}

void __fastcall BitmapRenderHook(void* self, void* edx, const float* world, const void* colour)
{
    const uintptr_t element = reinterpret_cast<uintptr_t>(self);
    const bool isReadable =
        IsReadable(element + OFFSET_bitmapVertices, 0x74) && IsReadable(reinterpret_cast<uintptr_t>(world), 64);
    int groupIndex = -1;
    GroupState* state = GroupOfDraw(&groupIndex, colour); // the target's bar: its own group, not on the wrist
    if (isReadable && state)
    {
        unsigned hash = 0;
        void* const d3dTexture =
            TextureOfMaterial(*reinterpret_cast<uintptr_t*>(element + OFFSET_bitmapMaterial), &hash);
        if (GroupBatch* batch = OpenBatch(*state, d3dTexture, colour))
        {
            // The strip (0, 1, 2, 3) as two triangles: (0, 1, 2) and (2, 1, 3).
            static const int ORDER[6] = {0, 1, 2, 2, 1, 3};
            for (const int corner : ORDER)
            {
                const float* vertex =
                    reinterpret_cast<const float*>(element + OFFSET_bitmapVertices + corner * BITMAP_VERTEX_SIZE);
                AddVertex(*state, world, vertex[0], vertex[1], vertex[2], vertex[3], vertex[4]);
            }
            CloseBatch(*state, batch);
        }
    }
    Record* record = isReadable ? NewRecord(self, world, colour, false) : nullptr;
    if (record)
    {
        const uintptr_t material = *reinterpret_cast<uintptr_t*>(element + OFFSET_bitmapMaterial);
        record->texture = IsReadable(material + OFFSET_materialTextureHash, 4)
                              ? *reinterpret_cast<unsigned*>(material + OFFSET_materialTextureHash)
                              : 0;
        record->left = record->top = 2.0f;
        record->right = record->bottom = -1.0f;
        for (int corner = 0; corner < 4; ++corner)
        {
            const float* vertex =
                reinterpret_cast<const float*>(element + OFFSET_bitmapVertices + corner * BITMAP_VERTEX_SIZE);
            float u = 0, v = 0;
            Project(world, vertex[0], vertex[1], vertex[2], &u, &v);
            record->left = u < record->left ? u : record->left;
            record->right = u > record->right ? u : record->right;
            record->top = v < record->top ? v : record->top;
            record->bottom = v > record->bottom ? v : record->bottom;
        }
    }
    if (groupIndex >= 0 && (_takenMask & (1u << groupIndex)))
    {
        return; // the mod draws this group itself
    }
    if (_isReticleHidden && groupIndex < 0 && IsReticleElement(element))
    {
        return; // the hands aim: the aiming line replaces the game's reticle
    }
    if (Stereo::IsPeriscopePass())
    {
        return; // no HUD in the periscope's picture (run317: the game's reticle there on some frames, off its aim)
    }
    if (Stereo::IsPeriscopeShown() && groupIndex < 0 && IsReticleElement(element))
    {
        // The periscope has its own reticle on the guns' line (Sight): the game's is left out in every pass (run319:
        // it still showed in the display on some frames).
        static unsigned skipped = 0;
        if (++skipped == 1)
        {
            Log::Info("hud", "game reticle left out in the periscope seat");
        }
        return;
    }
    _originalBitmapRender(self, edx, world, colour);
}

void __fastcall GlyphRenderHook(void* self, void* edx, const float* world, const void* colour)
{    if (Stereo::IsPeriscopePass())
    {
        return; // no HUD text in the periscope's picture
    }

    const uintptr_t element = reinterpret_cast<uintptr_t>(self);
    int groupIndex = -1;
    GroupState* state = GroupOfDraw(&groupIndex, colour);
    Record* record = NewRecord(self, world, colour, true);
    if (state && IsReadable(element + OFFSET_glyphRenderer, 0x18) && IsReadable(reinterpret_cast<uintptr_t>(world), 64))
    {
        const uintptr_t counts = *reinterpret_cast<uintptr_t*>(element + OFFSET_glyphCounts),
                        chunks = *reinterpret_cast<uintptr_t*>(element + OFFSET_glyphChunks),
                        materials = *reinterpret_cast<uintptr_t*>(element + OFFSET_glyphMaterials);
        // The page count is asked of the font (renderer +0x80, virtual +4) by the game; one page is all the HUD's
        // fonts have been seen to use, so up to four are read while their counts are readable.
        for (int page = 0; page < 4 && IsReadable(counts + 4 * page, 4) && IsReadable(chunks + 4 * page, 4) &&
                           IsReadable(materials + 4 * page, 4);
             ++page)
        {
            const unsigned glyphCount = *reinterpret_cast<unsigned*>(counts + 4 * page);
            if (glyphCount == 0 || glyphCount > 256)
            {
                continue;
            }
            unsigned hash = 0;
            void* const d3dTexture = TextureOfMaterial(*reinterpret_cast<uintptr_t*>(materials + 4 * page), &hash);
            if (record && !record->texture)
            {
                record->texture = hash;
            }
            GroupBatch* batch = OpenBatch(*state, d3dTexture, colour);
            if (!batch)
            {
                continue;
            }
            uintptr_t chunk = *reinterpret_cast<uintptr_t*>(chunks + 4 * page);
            for (int guard = 0; guard < 64 && IsReadable(chunk, OFFSET_chunkNext + 4); ++guard)
            {
                unsigned used = *reinterpret_cast<unsigned*>(chunk + OFFSET_chunkUsed);
                used = used > CHUNK_VERTEX_COUNT ? CHUNK_VERTEX_COUNT : used;
                for (unsigned index = 0; index < used; ++index)
                {
                    const float* vertex = reinterpret_cast<const float*>(chunk + index * GLYPH_VERTEX_SIZE);
                    AddVertex(*state, world, vertex[0], vertex[1], vertex[2], vertex[3], vertex[4]);
                }
                chunk = *reinterpret_cast<uintptr_t*>(chunk + OFFSET_chunkNext);
            }
            CloseBatch(*state, batch);
        }
    }
    if (groupIndex >= 0 && (_takenMask & (1u << groupIndex)))
    {
        return; // the mod draws this group itself
    }
    _originalGlyphRender(self, edx, world, colour);
}

/// RedMaskedBitmapElement::Render (0x2CB9E0): the minimap's background, the map texture (UV set 1) cut out by
/// map_mask (UV set 2); the same four vertices as a bitmap (docs/offsets.md "HUD element draws"). Without a mask
/// the game draws it as a plain bitmap, through BitmapRenderHook.
void __fastcall MaskedBitmapRenderHook(void* self, void* edx, const float* world, const void* colour)
{
    const uintptr_t element = reinterpret_cast<uintptr_t>(self);
    int groupIndex = -1;
    GroupState* state = GroupOfCurrent(&groupIndex);
    if (state && IsReadable(element + OFFSET_bitmapVertices, 0x7C) &&
        IsReadable(reinterpret_cast<uintptr_t>(world), 64))
    {
        const uintptr_t maskMaterial = *reinterpret_cast<uintptr_t*>(element + OFFSET_maskedMaterial);
        unsigned hash = 0;
        void* const d3dTexture =
            TextureOfMaterial(*reinterpret_cast<uintptr_t*>(element + OFFSET_bitmapMaterial), &hash);
        void* const d3dMask = IsReadable(maskMaterial + OFFSET_materialMaskHash, 4)
                                  ? TextureOfHash(*reinterpret_cast<unsigned*>(maskMaterial + OFFSET_materialMaskHash))
                                  : nullptr;
        if (GroupBatch* batch = OpenBatch(*state, d3dTexture, colour))
        {
            batch->d3dMask = d3dMask;
            static const int ORDER[6] = {0, 1, 2, 2, 1, 3};
            for (const int corner : ORDER)
            {
                const float* vertex =
                    reinterpret_cast<const float*>(element + OFFSET_bitmapVertices + corner * BITMAP_VERTEX_SIZE);
                AddVertex(*state, world, vertex[0], vertex[1], vertex[2], vertex[3], vertex[4], vertex[5], vertex[6]);
            }
            // The first triangle just added is corners 0, 1, 2 of the strip: (x0, y0), (x0, y1), (x1, y0).
            const GroupVertex* added = state->vertices + batch->firstVertex;
            const float sideX = sqrtf(
                            (added[2].x - added[0].x) * (added[2].x - added[0].x) +
                            (added[2].y - added[0].y) * (added[2].y - added[0].y)),
                        sideY = sqrtf(
                            (added[1].x - added[0].x) * (added[1].x - added[0].x) +
                            (added[1].y - added[0].y) * (added[1].y - added[0].y));
            state->hasFrame = true;
            state->frameCenterX = (added[1].x + added[2].x) * 0.5f;
            state->frameCenterY = (added[1].y + added[2].y) * 0.5f;
            state->frameHalfWidth = sideX * 0.5f;
            state->frameHalfHeight = sideY * 0.5f;
            CloseBatch(*state, batch);
            static bool hasSaid = false;
            if (!hasSaid)
            {
                hasSaid = true;
                Log::Info(
                    "huddraws",
                    "minimap background: texture %p mask %p, %.1f x %.1f units",
                    d3dTexture,
                    d3dMask,
                    static_cast<double>(sideX),
                    static_cast<double>(sideY));
            }
        }
    }
    if (groupIndex >= 0 && (_takenMask & (1u << groupIndex)))
    {
        return; // the mod draws this group itself
    }
    _originalMaskedBitmapRender(self, edx, world, colour);
}

bool Hook(GameBuild::Address address, void* detour, DrawFunction* original)
{
    void* target = reinterpret_cast<void*>(GameBuild::Absolute(address));
    MH_STATUS status = MH_CreateHook(target, detour, reinterpret_cast<void**>(original));
    if (status == MH_OK)
    {
        status = MH_EnableHook(target);
    }
    if (status != MH_OK)
    {
        Log::Error("huddraws", "hook at %p failed: %s", target, MH_StatusToString(status));
    }
    return status == MH_OK;
}

} // namespace

void Install()
{
    if (_isInstalled || _hasInstallFailed)
    {
        return;
    }
    if (!GameBuild::IsAvailable(GameBuild::Feature::HudDraws))
    {
        Log::Warning("huddraws", "HUD element draws not found for this game build: the HUD is cut from its picture");
        _hasInstallFailed = true;
        return;
    }
    _exeBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
    if (GameBuild::IsAvailable(GameBuild::Feature::Zoom))
    {
        _textureByHash =
            reinterpret_cast<TextureByHashFunction>(GameBuild::Absolute(GameBuild::Address::TextureByHash));
    }
    MH_Initialize();
    _isInstalled =
        Hook(GameBuild::Address::ElementDraw, reinterpret_cast<void*>(&ElementDrawHook), &_originalElementDraw) &&
        Hook(GameBuild::Address::BitmapRender, reinterpret_cast<void*>(&BitmapRenderHook), &_originalBitmapRender) &&
        Hook(GameBuild::Address::GlyphRender, reinterpret_cast<void*>(&GlyphRenderHook), &_originalGlyphRender) &&
        Hook(
            GameBuild::Address::MaskedBitmapRender,
            reinterpret_cast<void*>(&MaskedBitmapRenderHook),
            &_originalMaskedBitmapRender);
    _hasInstallFailed = !_isInstalled;
    Log::Info(
        "huddraws",
        "hooks %s (textures %s)",
        _isInstalled ? "installed" : "failed",
        _textureByHash ? "found" : "missing");
}

bool GetGroup(Group group, GroupDrawing& drawing)
{
    const int index = static_cast<int>(group);
    if (!_isInstalled || index < 0 || index >= GROUP_COUNT || GetTickCount() - _frameTick > FRESH_MILLISECONDS)
    {
        return false;
    }
    const GroupState& state = _groups[index];
    if (state.batchCount == 0 || state.maxX - state.minX < 1e-3f || state.maxY - state.minY < 1e-3f)
    {
        return false;
    }
    drawing.batches = state.batches;
    drawing.batchCount = state.batchCount;
    drawing.vertices = state.vertices;
    drawing.minX = state.hasFrame ? state.frameCenterX - state.frameHalfWidth : state.minX;
    drawing.minY = state.hasFrame ? state.frameCenterY - state.frameHalfHeight : state.minY;
    drawing.maxX = state.hasFrame ? state.frameCenterX + state.frameHalfWidth : state.maxX;
    drawing.maxY = state.hasFrame ? state.frameCenterY + state.frameHalfHeight : state.maxY;
    drawing.screenLeft = state.screenLeft;
    drawing.screenTop = state.screenTop;
    drawing.screenRight = state.screenRight;
    drawing.screenBottom = state.screenBottom;
    // Which way the plane's axes run is taken from the game's own picture: x must grow to the right (with u) and y
    // upward (against v), as the pieces lie on screen. b126 trusted the group matrix and came out flipped, b127
    // turned it half round and was upside down (idren2023: icons of health on the right, ammunition on the left,
    // health / primary on top).
    const double count = state.vertexCount;
    drawing.signX = state.sumXU - state.sumX * state.sumU / count >= 0 ? 1.0f : -1.0f;
    drawing.signY = state.sumYV - state.sumY * state.sumV / count <= 0 ? 1.0f : -1.0f;
    return true;
}

void SetReticleHidden(bool isHidden)
{
    _isReticleHidden = _isInstalled && isHidden;
}

void SetTaken(unsigned groupMask)
{
    _takenMask = _isInstalled ? groupMask : 0;
}

bool IsTaken(Group group)
{
    return (_takenMask & (1u << static_cast<int>(group))) != 0;
}

uintptr_t GetStatusDisplay()
{
    return _statusDisplay;
}

void DumpNextFrame()
{
    Install();
    if (_isInstalled)
    {
        InterlockedExchange(&_isRequested, 1);
    }
}

} // namespace HudDraws

} // namespace SwbfVr
