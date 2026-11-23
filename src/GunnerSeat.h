#pragma once

#include <cstdint>

/// GunnerSeat: the tanks' gunner seats in first person (PLAN 56, idren2023 2026-10-17/18). On the stock fighter tanks
/// (rep_hover_fightertank, imp_hover_fightertank) our own gunner's station (ShellDesigns "<odf>_gunner": a hatch the
/// gunner stands in, a chest shield, twin grips, the gun on trunnions) replaces the stock turret entirely: during the
/// hull's draws the turret's three bones (turret_y, turret_x, turret_barrel1) point at a collapsing matrix, so the
/// game's turret is not drawn, and their live matrices place our turret's three parts in the first-person pass (in
/// place of the game's targeting frame), so it turns, elevates and moves as the tank's own does. The view's resting
/// eye is the standing gunner's, on the turret's yaw axis. Other gunner seats are left to the game.
///
/// All functions run on the game's main thread (OnPresent from the Present hook, GetSeatEye from the camera hook
/// during Update, BeforeHullDraw / AfterHullDraw from the model draw hook and DrawTurret / GetHandPose from the
/// first-person draw, both inside Render), except SetTurretTest (script thread).

namespace SwbfVr
{

namespace GunnerSeat
{

/// Once per presented frame, after Vehicle::OnPresent: whether the player is in a gunner seat with a station design
/// (VR running), its hull's pose table, and the model draw hook it needs.
void OnPresent();

/// Model draw hook (Body), before the game's draw: when the draw uses the gunner seat's hull pose table, keeps the
/// turret bones' live matrices and the draw's world matrix, and (in first person) points those bones at a collapsing
/// matrix, so the stock turret is not drawn.
/// @param boneSet the draw's pose table.
/// @param matrix the draw's world matrix (16 floats, row vectors).
/// @return true when the table was changed: the caller draws, then calls AfterHullDraw.
bool BeforeHullDraw(uintptr_t boneSet, const float* matrix);

/// Model draw hook, after a draw BeforeHullDraw changed: the table's entries back as they were.
void AfterHullDraw();

/// Model draw hook, first: whether this draw is the hull of a cabin seat (the combat speeder's gunner) while its cabin
/// is shown in first person: the game draws the own hull in a gunner seat, which would show through the cabin's glass
/// (the cabin is wider than the hull), so it is left out, as in the pilot seat.
/// @param boneSet the draw's pose table.
/// @return true: the caller skips the draw.
bool ShouldSkipHullDraw(uintptr_t boneSet);

/// The resting eye of the view in a gunner seat with a station, in first person (Stereo, during Update): the standing
/// gunner's eye on the turret's yaw axis, through the hull's matrix.
/// @param gameCameraPosition the game camera's world position (3 floats), to tell first from third person.
/// @param eyeWorld set to the eye's world position (3 floats) when the result is true.
/// @return false outside such a seat, with VR off, in third person, or with `gunner turret 0`.
bool GetSeatEye(const float* gameCameraPosition, float* eyeWorld);

/// In a fighter tank's gunner station with VR running (PLAN 58 c): the view keeps its own heading there, as on foot
/// (Controls, Stereo), so the driver turning the hull does not turn the gunner.
bool IsStationSeat();

/// In a flyer's cabin seat (the TIE bomber's bombardier) shown in first person: the view takes the hull's whole
/// attitude there, as in a flyer's pilot seat (Stereo HoldHullHeading).
bool IsFlyerCabinSeat();

/// A cabin seat whose turret works aft (the snowspeeder's tail gunner, the AT-TE's rear guns): Stereo turns the
/// resting view round.
/// The drawn barrel's muzzle in the world (a cabin whose barrel is drawn apart from the game's gun: the AT-TE
/// cannon), where the aiming line starts (Sight); false otherwise.
bool GetDrawnMuzzle(float* position);
bool IsFacingAft();

/// A cabin seat whose view keeps its own heading as the fighter tanks' gunner station's (the AT-TE's open cannon).
bool IsHeldViewCabinSeat();

/// In a cabin seat whose gun aims below the head's line (the TIE bomber's bombardier, idren2023 2026-10-27: the level head
/// at the middle of the pitch limits), how far below (radians, down positive as the turret's pitch); else 0.
float AimDownRadians();

/// The combat speeder gunner's periscope (PLAN 57 i, idren2023 2026-10-19): a camera on the gun's muzzle looking along
/// the shot, its picture shown on a display ahead of the gunner (the stock gunner screen's bezel and glass) that rides
/// curved rails with the gun's pitch and turns with its yaw (ShellDesigns SPEEDER_PERISCOPE_*). Stereo renders the
/// camera's view as the scope pass; the shell draws its picture behind the display's glass.
struct Periscope
{
    float camera[16]; // the muzzle camera (world; camera convention: right, up, back, position)
    float screen[16]; // the display glass's frame (world; right, up, back toward the eye, position at its centre)
    float halfWidthMeters; // the glass's half width (the picture spans it)
    float fieldFactor = 1.0f; // the picture's field over the glass's angle as seen from the eye (above 1: wider)
};

/// The periscope this frame, while the cabin seat is shown in first person.
/// @param periscope set when the result is true.
/// @return false outside the speeder's gunner seat, in third person, with `gunner turret 0`, or without fire lines.
bool GetPeriscope(Periscope& periscope);

/// Draws our station in place of the seat's first-person model (Aim's vehicle first-person draw), each part with its
/// bone's matrix as the hull was drawn this frame; in a cabin seat (the combat speeder's gunner) the pilot cabin
/// instead: the game's pilot first-person model and its shell, as in the pilot seat.
/// @param flags the first-person draw's flags (passed on to the model draw).
/// @return false when nothing was drawn (no station for this seat, no hull draw seen this frame, test off): the caller
/// draws the game's model.
bool DrawTurret(unsigned flags);

/// A seat the game shows only from outside (the gunship's door passenger): his eye in the world. Update thread,
/// FpGuard held by the caller. @return false in any other seat.
bool GetForcedEye(float* eye);

/// Where a hand is drawn (the arms, Aim) while our station is shown: the controller's pose; while it holds a grip
/// (taken by a grip press within 20 cm), on the grip itself, which turns with the turret.
/// @param hand 0 right, 1 left.
/// @param restPose set to the pose in resting-pose space (16 floats) when the result is true.
/// @return false outside a station seat, in third person or while the hand is not tracked.
bool GetHandPose(int hand, float* restPose);

/// The building turret's gun along its model's +z (1, also outside one) or -z (-1: Mos Eisley's barge gun). Its yaw 0
/// then points the gun opposite to the building's forward: Vehicle aims and Stereo's view heading turn by half a turn.
float GunForwardZ();

/// In a building turret aimed by the right hand instead of the head (idren2023 2026-10-21: the Hoth dish and the
/// Geonosian turret, whose stock screens the head can then stay on; Vehicle StepTurretByHand), with our model shown.
bool IsAimedByHand();

/// Whether a hand holds its grip of the station now.
/// @param hand 0 right, 1 left.
bool IsHolding(int hand);

/// Test (script command `gunner turret <0|1>`): our station (1, the default) or the game's own turret and targeting
/// frame (0).
/// @param isOn true draws ours.
void SetTurretTest(bool isOn);

} // namespace GunnerSeat

} // namespace SwbfVr
