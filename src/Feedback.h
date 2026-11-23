#pragma once

/// Feedback: what the player feels and sees when a shot leaves the gun and when it hits: controller vibration, the
/// recoil kick of the gun model, a flash of the aiming line and a thud (Feedback.cpp).
///
/// All functions run on the game's main thread (the Present hook, Settings and the script commands call in from it).

namespace SwbfVr
{

namespace Feedback
{

/// Once per presented frame (called from the Present hook in D3dSurvey.cpp): installs the fire and hit hooks when VR
/// first runs.
void OnPresent();

/// Switches the parts of the feedback on or off (swbf-vr.ini: hit_feedback, haptics, recoil, hit_sound).
/// @param isHitFeedbackOn a hit flashes the aiming line and gives a pulse in the right controller.
/// @param isHapticsOn controller vibration at all (shots, hits, and the pulses of Pulse).
/// @param isRecoilOn a shot kicks the gun model back.
/// @param isHitSoundOn a hit plays a short thud.
void SetOptions(bool isHitFeedbackOn, bool isHapticsOn, bool isRecoilOn, bool isHitSoundOn);

/// A vibration pulse for other parts of the mod, given only if vibration is switched on.
/// @param hand 0 right, 1 left.
/// @param seconds how long.
/// @param amplitude strength, 0 to 1.
void Pulse(int hand, float seconds, float amplitude);

/// What the player's weapon is aimed at, as the game's reticle shows it (red / green): read from the player's
/// controlled Controllable (+0x30, the primary channel's target relation; docs/offsets.md "Reticule"). Logs changes.
/// @return -1 an enemy, 1 a friend, 0 nothing (or not known yet).
int AimedRelation();

/// Test ("hud hitdump <0|1>"): the first hit of each kind writes the HUD texture to a file.
/// @param isOn true switches the test on.
void SetDumpHitKinds(bool isOn);

} // namespace Feedback

} // namespace SwbfVr
