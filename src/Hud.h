#pragma once

/// Hud: the game's HUD pass drawn into a texture and put back into each eye as quads in the world (layouts classic,
/// compact, immersive with the ammunition on the gun and the wrist panel), and the comfort vignette (Hud.cpp).
///
/// All functions run on the game's render thread unless they say otherwise (Stereo.cpp's Present and projection
/// hooks call in; the settings and script commands set the options). Matrices are the engine's 4x4 row-vector
/// matrices passed as pointers to 16 floats: rows 0..2 the frame's right, up and back axes, row 3 its position.

namespace SwbfVr
{

namespace Hud
{

// ---- Options (swbf-vr.ini and script commands) ----

/// The comfort vignette: the edge of the view darkens while the sticks turn or move the player.
/// @param level 0 off, 1 light, 2 strong; anything else counts as off.
void SetVignette(int level);

/// Zoomed with the whole view magnified (ZOOM.md B3): a fixed round vignette in place of the game's scope overlay.
/// Set once per headset frame, before the eye passes.
/// @param isOn whether this frame is drawn magnified.
void SetZoomVignette(bool isOn);

/// Draws the comfort vignette into the eye image; once per eye, before the HUD is drawn into it.
/// @param device the game's IDirect3DDevice9.
void DrawVignette(void* device);

/// Test ("hud accum"): collects the union of every HUD drawn in that time and writes it to swbf-vr-hud-accum.bmp.
/// @param milliseconds how long to collect.
void Accumulate(unsigned milliseconds);

// ---- Calls from Stereo.cpp ----

/// During a headset eye pass the game set a projection: the HUD pass's sends drawing into the HUD texture, any
/// other ends that.
/// @param device the game's IDirect3DDevice9.
/// @param isHudPass true when it is the HUD pass's projection.
/// @return false when the texture cannot be used (mode 0 or no texture); the caller then uses the viewport panel.
bool OnProjection(void* device, bool isHudPass);

/// At the end of an eye pass, before the eye image is taken: draws the HUD from the texture into the eye image as
/// quads in the world, in the chosen layout.
/// @param device the game's IDirect3DDevice9.
/// @param eyeIndex 0 left, 1 right (the wrist panel decides once per frame, on the left eye).
/// @param eyeMatrix the eye's camera matrix (world, 16 floats).
/// @param headPosition the point between the eyes (world, x y z): panels that face the player face it in both eyes.
/// @param tanX tan(half horizontal field of view) of the eye's projection.
/// @param tanY tan(half vertical field of view) of the eye's projection.
/// @param frameMatrix the frame the HUD panel hangs in (the aim camera; 16 floats).
/// @param distanceMeters distance of the HUD panel ahead in that frame.
/// @param halfWidthTan half width of the HUD panel as tan(angle).
/// @param isWhole true shows the whole HUD as one panel (menus, spawn screen) whatever the layout.
void Composite(
    void* device,
    int eyeIndex,
    const float* eyeMatrix,
    const float* headPosition,
    float tanX,
    float tanY,
    const float* frameMatrix,
    float distanceMeters,
    float halfWidthTan,
    bool isWhole);

/// Before the device is reset: releases the HUD texture and the probe surfaces (all in the default pool).
void OnReset();

/// Test ("hud mode"): how the HUD is put into the eye.
/// @param mode 0 the old viewport panel (Stereo.cpp), anything else through the HUD texture (default).
void SetMode(int mode);

/// The HUD layout ("hud layout", swbf-vr.ini).
/// @param layout 0 classic, 1 compact, 2 immersive; -1 or anything else keeps the current one.
/// @param spread compact: elements moved toward the centre, to this fraction of their distance (0.2 to 1; else
///        kept).
/// @param scale compact: size of the moved elements (0.5 to 3; 0 or anything else keeps it).
void SetLayout(int layout, float spread, float scale);

/// The centre crosshair ("hud crosshair", swbf-vr.ini: crosshair).
/// @param shouldHideWithController true = hidden while a controller aims the weapon (default), false = always on.
void SetCrosshair(bool shouldHideWithController);

/// @return the mode set by SetMode: 0 viewport panel, 1 through the HUD texture.
int CurrentMode();

/// Test ("hud dump"): writes the next HUD textures to swbf-vr-hud-NNN.bmp. Any thread.
/// @param count how many.
void DumpNext(unsigned count);

/// Script `hud scopesize <percent>`: the scope overlay's half width on a lens, in percent of the lens radius (50-400).
void SetScopeOverlaySize(unsigned percent);

/// Scope pass (Stereo, while zoomed): copies the picture drawn so far (the scene from the scope's camera, before the
/// HUD) into the square scope texture. Render thread.
/// @param device the game's Direct3D 9 device.
/// @return false if the texture cannot be made or the copy failed.
bool CaptureScope(void* device);

/// The texture CaptureScope copies the scope pass's picture into (IDirect3DTexture9*; null before the first), for a
/// shell that shows it (the speeder gunner's periscope display). Render thread.
void* ScopePicture();

/// At the end of an eye pass while zoomed: draws the scope texture as a round disc with a black ring, the weapon's
/// scope overlay and its own reticle (a thin cross when it has none): either a window at infinity in front of the aim,
/// or on the weapon's lens (B2). Render thread.
/// @param device the game's Direct3D 9 device.
/// @param eyeMatrix the eye's camera matrix (world, 16 floats).
/// @param tanX tan(half horizontal field of view) of the eye's projection.
/// @param tanY tan(half vertical field of view) of the eye's projection.
/// @param windowFrame the disc's frame (16 floats, camera convention: right, up, back, position). For the window its
///        position is the eye's own, so the window lies at infinity; for a lens it is the lens centre.
/// @param radiusMeters the disc's radius.
/// @param distanceMeters how far along the frame's forward axis the disc lies (0 for a lens).
/// @param isLens true on a lens: a thin rim instead of the wide ring, the overlay cut at the disc's edge.
/// @param emitters on a lens, the world positions of the picture's light sources (3 floats each), or null.
/// @param emitterCount how many; 0 = the picture on the lens's glass, no hologram (b176).
/// @param emitterRadiusMeters a lens emitter's radius (the lens glows as a whole), or 0 for a small dot.
/// @param outline on a lens, its convex outline (2 floats a corner, in radii, Zoom::Lens), or null for a round disc.
/// @param outlineCount its corners.
/// @param tiltDegrees with an outline, the screen's tilt: the scope design is stretched over the outline's extent
///        along axes turned by it (Zoom::Lens).
void DrawScopeWindow(
    void* device,
    const float* eyeMatrix,
    float tanX,
    float tanY,
    const float* windowFrame,
    float radiusMeters,
    float distanceMeters,
    bool isLens,
    const float* emitters = nullptr,
    int emitterCount = 0,
    float emitterRadiusMeters = 0,
    const float* outline = nullptr,
    int outlineCount = 0,
    float tiltDegrees = 0);

/// @return true while the HUD or the vignette is being drawn into the eye image (the projection hook then leaves
///         the draw calls alone).
bool IsCompositing();

/// True while something large covers the middle of the HUD (end-of-match statistics, the big map, the player list):
/// the HUD is then shown whole, and the pointer may click on it.
bool HasOverlay();

/// Size of the whole-HUD panel's picture relative to the panel: 1.6 for an overlay over the playing HUD, else 1.
float OverlayScale();

} // namespace Hud

} // namespace SwbfVr
