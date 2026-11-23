#pragma once

#include <cstdint>

/// Zoom in VR (Zoom.cpp, docs/ZOOM.md): the game's zoom is kept off the main camera, so the headset always gets its
/// own field of view, and the zoom the game wants is kept here for the mod's own scope view.
///
/// The game's camera asks the player's soldier for its zoom (soldier GetZoom) every Update and ramps toward it. While
/// VR runs the hook answers 1.0, so the camera, its field of view and everything that depends on the camera being
/// zoomed (the scope overlay, its blur) stay as unzoomed; the real value is kept here. The first-person weapon,
/// whose update the game stops while zoomed, stays drawn in VR.
///
/// Main thread only (the game's Update and Render).

namespace SwbfVr
{

namespace Zoom
{

/// The zoom values of the weapon in hand, read from its WeaponClass (the ODF) and the weapon itself.
struct WeaponZoom
{
    float minimumFactor;    // ZoomMin: first level
    float maximumFactor;    // ZoomMax: second level (equal to the first when there is one level)
    float rate;             // ZoomRate: 0 = levels, otherwise a continuous zoom
    bool isFirstPersonZoom; // ZoomFirstPerson: zoom forces the first-person view
    bool isSniperScope;     // SniperScope
    unsigned reticleHash;   // ReticuleTexture name hash, 0 = none
    unsigned scopeHash;     // ScopeTexture name hash, 0 = none
    bool hasLens;           // the first-person weapon model has a lens node (docs/ZOOM.md section 2 table 5)
};

/// Installs the GetZoom hook (feature Zoom of GameBuild). Called by Stereo::Install.
void Install();

/// At the start of each Render, between frames: puts the patch that keeps the first-person weapon drawn while zoomed
/// in place while VR runs, and takes it out when VR stops.
/// @param isVrRunning whether VR runs this frame.
void BeforeFrame(bool isVrRunning);

/// @return true in VR while the game's zoom is on (the soldier asked for a factor above 1 in the last 0.2 s).
bool IsZoomed();

/// @return the zoom factor the game wants this frame (1 when not zoomed); the game ramps toward it over about 40 ms.
float TargetFactor();

/// Once per headset frame: the zoom factor of the scope view, ramped toward TargetFactor as the game ramps its camera
/// (time constant 40 ms), starting from 1 when the zoom comes on.
/// @return the factor for this frame (1 when not zoomed).
float StepFactor();

/// The scope window's angular size ("scope_window" in swbf-vr.ini, VR Settings).
/// @param degrees the full angle; 10 to 60, anything else keeps the current one (default 50).
void SetWindowDegrees(int degrees);

/// @return tan of half the scope window's angle.
float WindowHalfTan();

/// @param weaponZoom set to the weapon's zoom values when the result is true.
/// @return true while zoomed and the weapon's values could be read.
bool GetWeaponZoom(WeaponZoom& weaponZoom);
/// @return true while zoomed with a weapon whose first-person model has no lens: the whole view is magnified
/// (docs/ZOOM.md B3) instead of the scope window.
bool IsFullView();

/// The most lenses a weapon model has (the bowcasters: two scopes).
constexpr int MAXIMUM_LENS_COUNT = 2; // the bowcasters' two scopes
constexpr int MAXIMUM_EMITTER_COUNT = 2;
/// The most corners of a lens outline (the CIS sniper rifle's screen: 10).
constexpr int MAXIMUM_OUTLINE_POINTS = 12;

/// A lens of the first-person weapon model, in the model's space (metres, z forward, the lens facing -z).
/// The projected scope picture (b155): where it floats and where its light comes from, in the weapon model's space
/// (metres, z forward).
struct Lens
{
    float centre[3];                          // the picture's centre
    float radiusMeters;                       // its radius
    int emitterCount;                         // 0: the picture on the glass itself (lens weapons, b176); else projected
    float emitters[MAXIMUM_EMITTER_COUNT][3]; // the light's origins: the real lenses, or a point on the gun
    float emitterRadiusMeters;                // a real lens's radius (it glows as a whole), or 0 (a point)
    /// 0: a round picture. Else the glass's convex outline (the CIS sniper rifle's and launcher's screens, idren2023
    /// 2026-11-22): its corners in the picture's frame (x right and y up seen from behind), in radii; the radius is
    /// then the outline's largest reach along x or y, so the square scope picture covers it.
    int outlineCount;
    float outline[MAXIMUM_OUTLINE_POINTS][2];
    /// With an outline: the weapon's own scope design is stretched over the outline's extent (idren2023 2026-11-22: our
    /// line designs fell far short of the stock art), along axes turned by this angle (degrees, counter-clockwise in
    /// the picture's frame) to follow a tilted screen; the reticle is turned with it.
    float tiltDegrees;
    /// A display rather than a glass (the CIS sniper rifle's and launcher's screens): its picture is drawn for both
    /// eyes, as a screen shows its image to both, and covers the model's own animated screen in each (idren2023
    /// 2026-11-22: the other eye saw the stock screen and its animation, distracting). A glass's picture is drawn for
    /// the eye behind it only (headset run 15).
    bool isScreen;
};

/// Scope picture on the weapon's lens (docs/ZOOM.md B2) or in the floating window ("scope_view" in swbf-vr.ini).
/// @param isOn true: on the lens, for weapons whose lens is known.
void SetLensView(bool isOn);
/// @param lenses set to the projected picture of the weapon in hand (room for MAXIMUM_LENS_COUNT).
/// @return how many: 0 when not zoomed or the lens view is off (the window is used), else 1 (2 for the bowcasters).
int GetLenses(Lens* lenses);

/// The lens of a weapon class's first-person model, zoomed or not (for the scope zone, headset run 15).
/// @param weaponClass the class (Aim::GetWeaponClass).
/// @param centre set to the first lens's centre in the weapon model's space when the result is true.
/// @return false for weapons without a known lens (LENS_MODELS).
bool GetLensCentre(uintptr_t weaponClass, float* centre);

/// True if a weapon class's first-person model has a node with this name hash (the root node carries the model's
/// name).
bool HasFirstPersonNode(uintptr_t weaponClass, unsigned nodeHash);

/// True if a model (a runtime model object: a weapon's, or the first-person arms, FirstPersonRenderable +0x15FC) has a
/// node with this name hash (its root carries the model's name).
bool ModelHasNode(uintptr_t model, unsigned nodeHash);

/// A runtime model's node name hashes (their array, as an address, and count; 0 when it cannot be read).
unsigned ModelNodeList(uintptr_t model, uintptr_t* hashes);

/// True if a weapon class's first-person model has any nodes (the CIS mine's has none and draws nothing, headset run
/// 13).
bool HasFirstPersonNodes(uintptr_t weaponClass);

/// True if a weapon class's first-person model is one of the CIS side's (cis_1st_weap_inf_*, the super battle droid's
/// arm blaster): the holograms are drawn red for that side.
bool IsCisWeapon(uintptr_t weaponClass);

/// Logs the node name hashes of a weapon class's first-person model (to name them offline).
void LogFirstPersonModel(uintptr_t weaponClass);
/// The weapon's own reticle (its ReticuleTexture) while zoomed, looked up in the game's texture table each call, as
/// the game's reticle display does (the table is emptied at level load, so the pointer is only good for this frame).
/// @param width set to the texture's width in pixels.
/// @param height set to its height.
/// @return the IDirect3DTexture9*, or nullptr when not zoomed, the weapon has none or it is not loaded.
void* ReticleTexture(unsigned& width, unsigned& height);

/// The weapon's scope overlay (its ScopeTexture) while zoomed, looked up as ReticleTexture. The texture is the top
/// left quarter of the overlay, the scope's centre at its bottom right corner; the game mirrors it into four quads
/// over the whole screen (ScopeDisplay, docs/offsets.md "Zoom").
/// @param width set to the texture's width in pixels.
/// @param height set to its height.
/// @return the IDirect3DTexture9*, or nullptr when not zoomed, the weapon has none or it is not loaded.
void* ScopeTexture(unsigned& width, unsigned& height);

} // namespace Zoom

} // namespace SwbfVr
