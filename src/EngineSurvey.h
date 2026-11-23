#pragma once

/// EngineSurvey: diagnostic survey of the game's objects through MSVC RTTI: a table of every class with a vtable,
/// thunks on a few camera and controller vtables that collect live objects ("roots"), an object graph and memory
/// snapshots, and the lookup of live objects by short name for the other subsystems and the script (EngineSurvey.cpp).
///
/// Paused while VR runs (its work causes small hitches); Resolve keeps working then.

#include <cstdint>

namespace SwbfVr
{

namespace EngineSurvey
{

/// Finds a live game object by short name, from the newest CameraManager and PlayerController seen by the thunks.
/// @param name "cam" (CameraManager), "red" (its RedCamera), "chase" (its chase camera), "pc" (PlayerController),
///        "soldier" (the player's soldier), "aimer" (the soldier's Aimer) or "weapon" (the Aimer's weapon).
/// @return the object's address, or 0 if it is absent, the name is unknown or the survey is not installed.
uintptr_t Resolve(const char* name);

/// Builds the class table and installs the vtable thunks; opens swbf-vr-snap.bin. Called once, at the first
/// DirectInput8Create.
void Install();

/// Called from the Present hook after every presented frame: drives the graph walk, the scans of .data and the
/// snapshots on their timers.
/// @param frame the number of the frame just presented.
/// @param forceSnapshot true to write a snapshot now (the frame was traced by D3dSurvey).
void OnPresent(uint32_t frame, bool forceSnapshot);

} // namespace EngineSurvey

} // namespace SwbfVr
