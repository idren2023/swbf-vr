#pragma once

#include <cstdint>

/// HudDraws: the game's own HUD element draws, caught as they are made (HudDraws.cpp). The game's interface is a tree
/// of elements (RedInterfaceElement); every element is drawn through one function with its parent's matrix, and
/// bitmaps and text through their own render functions with the final world matrix. Hooking those three gives, per
/// draw, the element, the group it belongs to (HealthGroup, AmmoGroup), its texture, its colour and its geometry.
///
/// Two uses:
/// - the mod's HUD (Hud.cpp) draws whole groups itself from the game's textures, straight (the game slants the
///   health and ammunition boxes) and wherever it likes: on the gun, on the wrist, moved in. A group the mod draws is
///   "taken": the game's own draws of it are left out, so it is not in the HUD picture at all (no cut-outs).
/// - a diagnostic: `hud draws` logs every draw of one interface frame.
///
/// Everything runs on the game's main thread, which is also its render thread (the tree is drawn there, Hud reads the
/// result there).

namespace SwbfVr
{

namespace HudDraws
{

/// The groups the mod can take from the game.
enum class Group
{
    Health,     ///< HealthGroup: the player's health bar (and the jetpack gauge, if it is in the group)
    Ammunition, ///< AmmoGroup: ammunition counts, weapon and grenade icons
    Target,     ///< the health of what the player aims at: HealthGroup draws in the reticle's red / green (b179)
    Score,      ///< reinforcements and score bars: TeamDisplay's screen group (TeamDisplay+0x40, b183)
    Map,        ///< the minimap: MapDisplay's screen group (+0x30), its masked background (+0xE0) and the icons (b184)
    Count
};

/// A vertex of a group: x and y in the group's own plane (right and up after GroupDrawing's signs) (the game's HUD
/// units, slant removed); texture coordinates of the element's texture.
struct GroupVertex
{
    float x, y, u, v;
    float maskU, maskV; ///< the mask's texture coordinates (a masked batch only)
};

/// One draw of a group: a triangle list with one texture and one colour, in the order the game drew them.
struct GroupBatch
{
    void* d3dTexture; ///< IDirect3DTexture9* of the game's texture (valid for this frame)
    void* d3dMask;    ///< IDirect3DTexture9* whose alpha cuts the texture out (the minimap's map_mask), or null
    unsigned colour;  ///< ARGB as the game drew it (its HUD alpha included)
    int firstVertex;  ///< into GroupDrawing::vertices
    int vertexCount;  ///< a multiple of 3
};

/// What the game drew of one group in the latest interface frame.
struct GroupDrawing
{
    const GroupBatch* batches;
    int batchCount;
    const GroupVertex* vertices;
    float minX, minY, maxX, maxY; ///< bounds of the vertices (the minimap: of its background, unturned)
    float screenLeft, screenTop, screenRight, screenBottom; ///< where the game puts it on screen (fractions, v down)
    float signX, signY; ///< multiply x and y by these to get right and up as the game shows the group on screen
};

/// Installs the three hooks (feature HudDraws of GameBuild). Called by Stereo::Install; harmless when unavailable.
void Install();

/// The latest interface frame's drawing of a group. Render thread.
/// @return false when the group was not drawn lately (menus, spawn screen, hooks missing): fall back.
bool GetGroup(Group group, GroupDrawing& drawing);

/// Which groups the mod draws itself: the game's own draws of them are left out from the next interface frame on.
/// Render thread, between the HUD pass and the next one.
/// @param groupMask bit (1 << Group) per taken group.
void SetTaken(unsigned groupMask);

/// Whether the mod draws this group itself (SetTaken): the cockpit shell's displays show only taken groups, so the
/// classic layout (nothing taken) keeps the game's own HUD and nothing is shown twice. Render thread.
bool IsTaken(Group group);

/// Leaves out the game's weapon reticle (ReticuleDisplay's bitmaps) from the next interface frame on, while the hands
/// aim: it moved up and down with the hand's pitch, away from the aiming line (PLAN 47). Render thread,
/// as SetTaken.
void SetReticleHidden(bool isHidden);
/// The game's StatusDisplay (health, ammunition, squad commands), as last seen drawing its HealthGroup, or 0. It
/// lives as long as the level; callers check its class before use.
uintptr_t GetStatusDisplay();

/// Logs every HUD element draw of the next whole interface frame ("hud draws"). Main thread.
void DumpNextFrame();

} // namespace HudDraws

} // namespace SwbfVr
