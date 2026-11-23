#pragma once

/// LeverPlacement: where the tanks' slide handles stand, in resting-pose space (metres; x right, y up, z back; origin
/// at the resting head), shared by the handles (Levers.cpp, LeverModel.cpp) and the cockpit shells' front console
/// that carries their tracks (ShellDesigns.cpp). In a stock tank's cockpit model space the same numbers hold, offset by
/// the pilot's eye of that shell (ShellDesigns::PilotEye): the cockpit model is drawn on the hull's level heading with
/// that eye at the game camera (Aim DrawVehicleFirstPerson).

namespace SwbfVr
{

namespace LeverPlacement
{

/// A handle's pivot (the top of its carriage, where the grip stands and leans for strafe) in the middle (neutral)
/// notch: this far to each side of the head, below it and ahead of it (PLAN 57 d, idren2023 2026-10-18: slide handles on
/// the console ahead, held in the palms). The hand closes GRIP_HEIGHT_METERS up the grip, which leans
/// GRIP_LEAN_DEGREES ahead at the top: the palm at 0.45 m under and 0.39 m ahead of the eye. A seated player's hands
/// at rest sit about 20 cm to the side, 25 cm under and 45 cm ahead of the eyes (the simulated controllers' rest).
/// Headset run 17 (PLAN 58 a): 10 cm lower than b273's 0.45, and the forward notch closer (0.45 m ahead, was 0.50).
constexpr float HANDLE_SIDE_METERS = 0.20f, PIVOT_DOWN_METERS = 0.55f, PIVOT_AHEAD_METERS = 0.37f;
constexpr float GRIP_HEIGHT_METERS = 0.10f, GRIP_LEAN_DEGREES = 10.0f;
/// The handles slide straight ahead and back along the hull, between three notches: reverse, neutral, forward, this
/// far apart (PLAN 58 a: 8 cm, were 10).
constexpr float NOTCH_SPACING_METERS = 0.08f;
/// From the pivot down to the console's deck the carriage rides on.
constexpr float CARRIAGE_HEIGHT_METERS = 0.026f;
/// The carriage's half length along the track and half width (LeverModel); the track's slot is cut for it.
constexpr float CARRIAGE_HALF_LENGTH_METERS = 0.051f, CARRIAGE_HALF_WIDTH_METERS = 0.034f;
/// The eye in every stock first-person cockpit model as the game draws it (measured: the model is drawn 1.5 m below
/// and 1.5 m ahead of the game camera, axes aligned, scale 1; runs 222-234 and a render of the msh against run225's
/// picture). The shells may seat the pilot elsewhere (ShellDesigns::PilotEye).
constexpr float COCKPIT_EYE[3] = {0.0f, 1.5f, 1.5f};

} // namespace LeverPlacement

} // namespace SwbfVr
