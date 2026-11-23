#pragma once

/// Squad: squad commands by hand gesture (PLAN 33, idren2023 2026-10-13). An empty hand raised to head height in front
/// of the face arms the listener with its grip; while the grip is held, a jab gives a command (Squad.cpp). Two sets
/// (VR Settings): by the HUD's directions (default, headset run 15: jab forward Move Out, left At Ease, right Follow
/// Me, back toward the face Hold Position), or immersive (jab forward Move Out, beckon Follow Me, pull down Hold
/// Position, slow horizontal circle At Ease). While a hand is armed the game's squad display shows. Either hand: the
/// right one while the holsters leave it empty, the left one while it is not on the gun.
///
/// The command goes through the game's own squad command, as the squad buttons do: every friendly AI soldier on foot
/// within the game's radius (45 m for Move Out and At Ease, 30 m for the others) takes it, with the game's voice line
/// and HUD display.
///
/// All functions run on the game's main thread.

namespace SwbfVr
{

namespace Squad
{

/// The game's squad commands (the soldier's squad buttons; game.squadcommand.field.* in the localisation).
enum class Command
{
    MoveOut = 0,
    AtEase = 1,
    HoldPosition = 2,
    FollowMe = 3
};

/// Reads both hands' gestures and gives a recognised command to the game. Once per presented frame, after
/// Gesture::OnPresent and Holster::OnPresent (called from the Present hook in D3dSurvey.cpp).
void OnPresent();

/// True while a hand is armed for a squad gesture and has not given its command (the ear menu stays shut meanwhile).
/// @param hand 0 right, 1 left.
bool IsArmed(int hand);

/// Chooses the gesture set (swbf-vr.ini `squad_gestures`).
/// @param isImmersive true: jab, beckon, pull down, circle; false: jabs in the HUD's four directions.
void SetGestureSet(bool isImmersive);

/// Gives a command with the next OnPresent, without a gesture (script command `squad`, tests).
/// @param command the command.
void Request(Command command);

} // namespace Squad

} // namespace SwbfVr
