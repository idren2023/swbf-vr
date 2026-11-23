#pragma once
/// Voice: squad commands by voice (idren2023, 2026-10-17). The Windows speech recognizer (SAPI 5, in process, offline)
/// listens for four phrases, "move out", "follow me", "hold position", "at ease", on its own thread; a phrase counts
/// only if the recognizer's confidence and the microphone's loudness while it was said both reach their thresholds
/// (VR Settings, FEEDBACK). A command goes to the game through Squad::Request, as the gestures do.
///
/// The microphone: the capture device whose name contains swbf-vr.ini `voice_device`, else the one named "Steam
/// Streaming Microphone" (the headset's microphone over Steam Link), else Windows' default.
namespace SwbfVr
{
namespace Voice
{
/// Switches listening on or off and sets the thresholds; listening starts or stops at once (its own thread). Any
/// thread.
/// @param isOn true: listen.
/// @param minimumConfidencePercent the recognizer's confidence a phrase needs, 0 to 100.
/// @param minimumDecibels the loudest moment while the phrase was said must reach this level, dB of full scale
/// (0 = the loudest the microphone gives, -60 = very quiet).
/// @param deviceName part of the capture device's name; empty = the Steam Link microphone, else the default.
void SetOptions(bool isOn, int minimumConfidencePercent, int minimumDecibels, const char* deviceName);

/// Test (script command `voice file <path>`): recognises the speech in a WAV file instead of the microphone, once;
/// the loudness threshold is not applied. Listening to the microphone resumes afterwards if it is on.
/// @param path the WAV file.
void ListenToFile(const char* path);
} // namespace Voice
} // namespace SwbfVr
