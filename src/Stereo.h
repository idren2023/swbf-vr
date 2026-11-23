#pragma once

/// Stereo: renders the game twice per frame, once per eye, for the headset (OpenXR, Xr) or side by side on the
/// monitor (desktop test); head camera, headset field of view, zoom, HUD panel and menus in VR (Stereo.cpp).
///
/// All functions run on the game's render thread (Update and Render both run there; the Direct3D 9 hooks in
/// D3dSurvey.cpp and the script commands call in from it). Matrices are the engine's 4x4 row-vector matrices passed
/// as pointers to 16 floats (the convention of the whole mod until it has a matrix type): rows 0..2 are the frame's
/// right, up and back axes, row 3 its position; x right, y up, -z ahead, metres.

namespace SwbfVr
{

namespace Stereo
{

// ---- Calls from the Direct3D 9 hooks (D3dSurvey.cpp) ----

/// At the top of the Present hook: puts the HUD into the eye image, hands each finished eye image (or a flat frame
/// for menus) to the headset, composes the desktop side-by-side picture, and installs the hooks once VR has started.
/// @param device the game's IDirect3DDevice9.
/// @return true when the real Present must be skipped (the left-eye pass: the monitor shows the right eye).
bool OnPresent(void* device);

/// Before the device is reset: releases the eye render targets (default pool), also those of Sight and Hud.
void OnReset();

/// The game set a projection matrix. During an eye pass this tells the 3D scene from the HUD pass, which is then
/// drawn into the HUD texture or onto the HUD panel.
/// @param device the game's IDirect3DDevice9.
/// @param projectionMatrix the projection matrix (16 floats).
void OnProjection(void* device, const float* projectionMatrix);

/// Just before the real Present: the frame rate cap (test) and the time between presents, logged every 2 s while VR
/// runs or a cap is set.
void PaceBeforePresent();

/// Just before the real Present: keeps the presented image when a grab of presented images (mode 1) is running.
/// @param device the game's IDirect3DDevice9.
void GrabPresented(void* device);

/// Which eye is being drawn.
/// @return 0 left, 1 right while an eye pass is drawn, -1 otherwise.
int CurrentEye();

// ---- State for the other subsystems ----

/// The frame tracked poses are relative to in VR: the level camera (where the resting head is, pitch and roll
/// removed, crouch lift added).
/// @param matrix set to the level camera (16 floats) when the result is true.
/// @return false when VR is not running or no 3D scene is shown; the caller then uses the game's camera.
bool GetBaseCamera(float* matrix);

/// The camera the game asked for in this frame's Update (its player camera: in a vehicle it follows the seat's aim),
/// before the head is put on it.
/// @param matrix set to that camera-to-world matrix (16 floats) when the result is true.
/// @return false when VR is not running or this frame's Update did not set the player's view camera.
bool GetGameCamera(float* matrix);

/// While the game's zoom is on in VR (Zoom::IsZoomed): the head's camera of this frame's Update.
/// @param headCamera set to the head's camera-to-world matrix (16 floats) when the result is true.
/// @return true while zoomed in VR.
bool GetZoomedHeadCamera(float* headCamera);

/// Whether the view may keep a heading of its own while the player view is shown (on foot with controllers): the
/// character then turns under it and only TurnView turns the view. Read at the next Update. Main thread.
void SetViewFree(bool isFree);

/// Turns the held view heading (snap / smooth turn) at the next Update; nothing while the view is not held.
/// @param rightRadians to the right positive.
void TurnView(float rightRadians);

/// The character's heading relative to the held view, as of the last Update.
/// @param rightRadians set to the angle, to the right positive, when the result is true.
/// @return false while the view is not held (the view then turns with the character).
bool GetCharacterYaw(float* rightRadians);

/// Heading of the head relative to the character's facing.
/// @param yawRadians set to the heading, positive to the right, when the result is true.
/// @return false outside VR or while the head looks nearly straight up or down.
bool GetHeadYaw(float* yawRadians);

/// Pitch of the head relative to its resting pose (level), up positive.
/// @return false outside VR.
bool GetHeadPitch(float* pitchRadians);

/// Pitch of the game's own player camera at the last Update, before the view is levelled (mouse up / down), up
/// positive.
/// @return false while the view is not held (not the player's view on foot with controllers).
bool GetGamePitch(float* pitchRadians);

/// True between the first-person pass's start (its SetNearFar) and the HUD pass or the next scene pass.
bool IsFirstPersonPass();

/// The head (between the eyes) relative to its resting pose, camera convention.
/// @param matrix set to the latest head pose (16 floats) when the result is true.
/// @return false outside VR.
bool GetHeadPose(float* matrix);

/// World position of the eye being drawn (for the aiming line); in the periscope's scope pass, its camera's.
/// @param position set to x, y, z when the result is true.
/// @return false outside an eye pass and the periscope pass.
bool GetEyePosition(float* position);

/// The head in the world while an eye pass is drawn: the eyes' axes (camera convention, -z forward) at the point
/// between both eyes, the same for both passes of a frame (for the body's placement).
/// @param matrix set to the head's camera-to-world matrix (16 floats) when the result is true.
/// @return false outside an eye pass.
bool GetHeadWorld(float* matrix);
/// The head in the world as of the last stereo frame (GetHeadWorld's matrix), for code running after the frame.
/// @param matrix set to 16 floats when the result is true.
/// @return false outside VR or before the first stereo frame.
bool GetLastHeadWorld(float* matrix);

/// As GetLastHeadWorld, the head's turn (relative to the level camera, so the ship's own turns are not lagged)
/// followed with a 0.1 s lag: for a HUD on the head (the droid's), which then does not judder under the compositor's
/// reprojection (headset run 18). Same thread and validity as GetLastHeadWorld.
bool GetSmoothedHeadWorld(float* matrix);
/// While the view is held after the player's death (the game's death camera active, VR running): the player's body
/// and the killer, for the line between them (PLAN 35). No line once the killer has died or respawned, for a death
/// without a killer, or after the spawn screen. Render thread.
/// @param body set to a point on the body (world) when the result is true.
/// @param killer set to a point on the killer (world) when the result is true.
bool GetKillerLine(float* body, float* killer);

/// @return true inside the extra Render() that draws the scope view while zoomed (docs/ZOOM.md, B1).
bool IsScopePass();

/// @return true inside the scope pass when it draws the speeder gunner's periscope (GunnerSeat::GetPeriscope).
bool IsPeriscopePass();

/// @return true while this frame shows the speeder gunner's periscope (its scope pass ran), in every pass.
bool IsPeriscopeShown();

/// The scope pass's camera (world, camera convention: right, up, back, position), for what is drawn into it.
/// @param matrix set to it (16 floats) when the result is true.
/// @return false outside the scope pass.
bool GetScopeCamera(float* matrix);

/// Whether the game shows its 3D scene (a match: playing or the spawn screen); false in menus and on loading screens.
bool IsSceneShown();

/// @return true in VR while the spawn screen (class and command post choice) is showing.
bool IsSpawnScreen();

/// Where a controller ray meets the menu screen: the flat screen 2 m ahead when no 3D scene is shown (main menu,
/// loading), the HUD panel on the spawn screen and while the VR Settings screen is open.
/// @param handMatrix the controller's pose in resting-pose space (16 floats).
/// @param u set to the fraction across from the left edge when the result is true.
/// @param v set to the fraction down from the top edge when the result is true.
/// @return false while playing or when the ray points away from the screen.
bool ComputePointerUV(const float* handMatrix, float* u, float* v);

/// Raises the view, the hands and the HUD frame (crouch by head height); eased at 2.5 m/s.
/// @param meters how far to raise them.
void SetLift(float meters);

/// Near clip distances while VR runs.
/// @param sceneMillimeters the scene's; 0 = the game's (0.5 m).
/// @param firstPersonMillimeters the first-person arms' and weapon's; 0 = unchanged.
void SetNearClip(unsigned sceneMillimeters, unsigned firstPersonMillimeters);

/// Logs the next count calls of RedCamera::SetNearFar (caller, distances as passed on, eye), script "near log <n>".
void LogNearFarCalls(unsigned count);

// ---- Script commands (tests) ----

/// Desktop side-by-side test ("stereo <mm>").
/// @param eyeDistanceMillimeters distance between the eyes; 0 switches the test off.
void SetDesktopEyeDistance(unsigned eyeDistanceMillimeters);

/// Forces the engine camera's field of view ("fov").
/// @param tanXThousandths tan(half horizontal angle) * 1000; 0 = back to the game's own (or the headset's).
/// @param tanYThousandths tan(half vertical angle) * 1000; 0 = the same as the horizontal.
void ForceFov(unsigned tanXThousandths, unsigned tanYThousandths);

/// Saves the next images as BMP files in GameData ("grab <n> [mode]").
/// @param count how many images, at most 128.
/// @param mode 0 eye passes, a full-resolution cut of the lower middle; 1 the presented image at quarter size;
///        2 eye passes, whole image at half size; 3 eye passes, 800x500 at full resolution around the point set by
///        SetGrabCut.
void GrabImages(unsigned count, unsigned mode);

/// Test clips (script `record <name> <seconds> <fps> [eye 0|1]`): the left (or right) eye pass or flat frame at half size, at most fps a
/// second for that long, written as swbf-vr-rec-<name>-<frame>.bmp. Seconds 0 stops a clip. Main thread reads it.
void Record(const char* name, unsigned seconds, unsigned framesPerSecond, int eye);

/// Where grab mode 3 cuts its 800x500 full-resolution piece.
/// @param xPercent centre of the piece across the image, percent; over 100 = the middle.
/// @param yPercent centre of the piece down the image, percent; over 100 = the middle.
void SetGrabCut(unsigned xPercent, unsigned yPercent);

/// Headset path test settings ("vr readback", "vr cap").
/// @param simulatedReadback 1 = read back the eye images with the simulated headset (default), 0 = skip it,
///        -1 = leave as is.
/// @param frameRateCap frames per second, 0 = no cap, -1 = leave as is.
void SetTestOptions(int simulatedReadback, int frameRateCap);

/// Logs the next camera matrices the game sets, and the camera at Render start ("camlog <n>").
/// @param count how many.
void StartCameraLog(unsigned count);

/// How the simulated headset is shown on the monitor ("vr mirror").
/// @param isSideBySide true = both eyes side by side (default), false = the right eye alone, like a real headset.
void SetSimulatedSideBySide(bool isSideBySide);

/// Switches parts of the headset path off ("vr test <bits>").
/// @param bits 1 camera not levelled, 2 no HUD panel, 4 no head camera in Update, 8 forced field of view on every
///        camera.
void SetTestPartsOff(unsigned bits);

} // namespace Stereo

} // namespace SwbfVr
