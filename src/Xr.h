#pragma once

/// Xr: OpenXR headset output, motion controllers and the simulated headset (Xr.cpp).
///
/// The runtime is loaded directly (no OpenXR loader library); the game's eye images arrive as BGRA pixels in system
/// memory and are copied into OpenXR swapchains through a Direct3D 11 device of our own. Unless a function says
/// otherwise it runs on the game's render thread, and callers wrap it in FpGuard. Poses are 4x4 row-vector matrices
/// passed as pointers to 16 floats, in the game's camera convention (rows: right, up, back, position; x right, y up,
/// -z ahead, metres), measured from the resting pose: where the head was when VR started or was last recentred.
/// Hands are indexed 0 right, 1 left.

namespace SwbfVr
{

namespace GpuTransfer
{
struct SharedImage;
}

namespace Xr
{

// ---- Start, stop and the frame loop ----

/// Remembers the PCI ids of the game's graphics adapter: the fallback choice for the headset's Direct3D 11 device
/// when the runtime names no adapter that is present.
/// @param vendorId the adapter's PCI vendor id.
/// @param deviceId the adapter's PCI device id.
void SetGameAdapter(unsigned vendorId, unsigned deviceId);

/// Asks for VR to start; Service starts it between frames. Clears an earlier failure. Any thread (script).
void RequestStart();

/// Asks for VR to stop; Service stops it between frames. Any thread (script).
void RequestStop();

/// Once per frame on the render thread, between frames: applies start, stop and simulated-headset requests, handles
/// the runtime's events and the Home key (recentre). On the first call it starts VR by itself if SteamVR is running
/// and no test script is pending.
void Service();

/// @return true while an OpenXR session exists (or the simulated headset is on).
bool IsStarted();

/// @return true while the session exists and the headset is accepting frames (or the simulated headset is on).
bool IsRunning();

/// Stereo frame, step 1: waits for the headset's frame timing and locates both eyes (and the controllers).
/// @return 0 VR not running; 1 VR running but nothing should be drawn this frame (the frame is already ended);
///         2 draw both eyes, then call SubmitEye for each and EndStereoFrame.
int BeginStereoFrame();

/// The latest eye poses and the symmetric field of view that covers both eyes.
/// @param leftEyeMatrix set to the left eye's pose (16 floats) when the result is true.
/// @param rightEyeMatrix set to the right eye's pose (16 floats) when the result is true.
/// @param tanX set to tan(half horizontal field of view) when the result is true.
/// @param tanY set to tan(half vertical field of view) when the result is true.
/// @return false while the headset has not reported a pose yet, or its field of view makes no sense.
bool GetViews(float* leftEyeMatrix, float* rightEyeMatrix, float* tanX, float* tanY);

/// Takes the current head pose as the resting pose at the next tracked frame. Any thread.
void Recenter();

/// How many recentres have been asked for (Home, left stick, script) since the start. Any thread.
unsigned RecenterCount();

/// Copies one eye's finished image into its swapchain (between BeginStereoFrame and EndStereoFrame).
/// @param eye 0 left, 1 right.
/// @param bgraPixels the image, 4 bytes per pixel in the back buffer's BGRA order.
/// @param pitch bytes from one row to the next.
/// @param width image width in pixels.
/// @param height image height in pixels.
/// @return true when the image was handed over (with the simulated headset: when there are pixels).
bool SubmitEye(int eye, const void* bgraPixels, int pitch, int width, int height);

/// @return true when SubmitEyeShared and SubmitFlatShared can be used: GpuTransfer is active, and (with a headset) our
///         Direct3D 11 device is on its adapter, the swapchains have the back buffer's byte order and opening its
///         shared objects has not failed. Always true with the simulated headset while GpuTransfer is active.
bool TakesSharedImages();

/// Like SubmitEye, for an image on the graphics card (GpuTransfer::Transfer): copied on the card, no CPU wait.
/// @param eye 0 left, 1 right.
/// @param image the shared image; its `read` fence is signalled in any case.
/// @return true when the image was handed over (with the simulated headset: always).
bool SubmitEyeShared(int eye, const GpuTransfer::SharedImage& image);

/// Stereo frame, last step: hands both eye images to the headset as one projection layer.
/// @param tanX tan(half horizontal field of view) the images were drawn with (symmetric).
/// @param tanY tan(half vertical field of view) the images were drawn with (symmetric).
/// @param hasImages false ends the frame without a layer (an eye image is missing).
void EndStereoFrame(float tanX, float tanY, bool hasImages);

/// Shows an image that is not the 3D scene (menus, loading screens) on a flat virtual screen 2 m ahead of the resting
/// pose, as a whole frame of its own.
/// @param bgraPixels the image, 4 bytes per pixel in BGRA order.
/// @param pitch bytes from one row to the next.
/// @param width image width in pixels.
/// @param height image height in pixels.
void SubmitFlatFrame(const void* bgraPixels, int pitch, int width, int height);

/// Like SubmitFlatFrame, for an image on the graphics card (GpuTransfer::Transfer); its `read` fence is signalled in
/// any case.
void SubmitFlatShared(const GpuTransfer::SharedImage& image);

/// @return true when SteamVR's server (vrserver.exe) is running. Any thread.
bool IsSteamVrRunning();

// ---- Controllers ----

/// A controller's aim pose (origin near its front, -z pointing where it points).
/// @param hand 0 right, 1 left.
/// @param matrix set to the pose (16 floats) when the result is true.
/// @return false while that controller is not tracked (or no resting pose is known yet).
bool GetHandPose(int hand, float* matrix);

/// A controller's grip pose (origin in the middle of its handle, in the palm; -z along the handle toward the
/// thumb, +x into the right palm), as GetHandPose. With the simulated hands (or no grip pose from the runtime): the
/// aim pose moved by a typical handle offset. Main thread.
/// @param hand 0 right, 1 left.
/// @param matrix set to the pose (16 floats, rows) when the result is true.
/// @return false while that controller is not tracked.
bool GetGripPose(int hand, float* matrix);

/// Buttons, triggers and sticks of both controllers as of this frame. Arrays are indexed 0 right hand, 1 left hand.
struct ControllerInput
{
    bool isValid; // false: no controller input now
    float trigger[2];
    float stickX[2], stickY[2];
    bool isLowerPressed[2], isUpperPressed[2]; // lower = A / X, upper = B / Y
    float grip[2];
    bool isStickPressed[2];
    bool isMenuPressed; // the left controller's menu button
    // Touch sensors (Touch and Index controllers; headset run 18: the open hand's poses), only where hasTouch:
    bool hasTouch[2];
    bool isTriggerTouched[2]; // the index finger on the trigger
    bool isThumbTouched[2];   // the thumb on the face: the stick, a button or the thumb rest
};

/// This frame's controller input (from OpenXR, or from SimulateControllerInput with the simulated headset).
/// @param input set to the input; its isValid is false when there is none.
void GetControllerInput(ControllerInput* input);

/// A vibration pulse in one controller. Main thread. With the simulated headset it is only logged.
/// @param hand 0 right, 1 left.
/// @param seconds how long.
/// @param amplitude strength, 0 to 1.
/// @param hertz frequency; 0 = the runtime's default.
void Vibrate(int hand, float seconds, float amplitude, float hertz);

// ---- Simulated headset (desktop tests, script commands "vr sim", "vr hands", "vr handpose", "vr pad") ----

/// Switches the simulated headset on or off: a fixed or swinging head pose and a Quest-like field of view, so the
/// whole stereo path except the OpenXR calls can be tested on the desktop. Takes effect at the next Service.
/// @param isOn true switches it on.
/// @param yawDegrees head heading, left positive (the swing's amplitude when a period is given).
/// @param pitchDegrees head pitch, up positive (likewise).
/// @param periodMilliseconds 0 = fixed pose, otherwise the head swings between -pose and +pose in this period.
void SimulateHeadset(bool isOn, float yawDegrees, float pitchDegrees, unsigned periodMilliseconds);

/// @return true while the simulated headset is on.
bool IsSimulated();

/// Test input for the simulated controllers ("vr pad").
/// @param input the buttons, triggers and sticks to report; isValid is set to true.
void SimulateControllerInput(const ControllerInput& input);

/// Simulated controllers for the simulated headset: the right one swings in front of the right shoulder, the left
/// one circles in front of the left shoulder.
/// @param isOn true switches them on.
/// @param periodMilliseconds period of the movement; 0 = both at rest.
void SimulateHands(bool isOn, unsigned periodMilliseconds);

/// Holds one simulated controller at a fixed pose ("vr handpose"); used while the simulated controllers are on.
/// @param hand 0 right, 1 left.
/// @param isOn false releases the controller to the simulated movement again.
/// @param x metres from the resting head, right.
/// @param y metres from the resting head, up.
/// @param z metres from the resting head, back.
/// @param yawDegrees heading, left positive.
/// @param pitchDegrees pitch, up positive.
/// @param rollDegrees roll, applied last.
void SimulateHandPose(
    int hand,
    bool isOn,
    float x,
    float y,
    float z,
    float yawDegrees,
    float pitchDegrees,
    float rollDegrees);

/// Test ("vr simheight"): puts the simulated head above (or, negative, below) its resting height.
/// @param meters the height difference.
void SetSimulatedHeight(float meters);

/// Test clips, `vr glide <ms>`: new simulated head and hand poses are reached over that time, eased (0 = the head
/// jumps, the hands glide 150 ms).
void SetSimulatedGlide(unsigned milliseconds);

/// Test clips, `vr life <0|1>`: a slight slow sway of the simulated head and hands.
void SetSimulatedLife(bool isOn);

/// Test ("vr simside"): puts the simulated head to the right (or, negative, left) of its resting place, and ahead of
/// it.
/// @param meters the sideways difference.
/// @param aheadMeters the forward difference (negative: back).
void SetSimulatedSide(float meters, float aheadMeters);

} // namespace Xr

} // namespace SwbfVr
