#pragma once

/// LeverModel: how a tank's drive handle looks (Levers.cpp places and moves it). A slide handle built as if it had
/// always been part of the vehicle (idren2023 2026-10-17, slide handles 2026-10-18): an oval carriage riding in the
/// console's track, a short ribbed rubber boot, a rubber pistol grip with finger grooves and a trigger, a cap and a
/// thumb button. Its metal takes a plain patch of the stock vehicle's own cockpit texture and colours (the table at
/// the top of LeverModel.cpp), lit by a light fixed to the hull, with highlights; other vehicles get neutral colours
/// untextured.
///
/// Draw and OnReset run on the game's main thread, inside the first-person pass (Aim's vehicle draw via Levers::Draw).

namespace SwbfVr
{

namespace LeverModel
{

/// Which handle a frame draws (idren2023 2026-10-27: the starfighters' controls as a fighter jet's).
enum class Model
{
    Tank,     // a tank's slide handle: a short grip on a carriage (Levers, the take-off lever)
    JetStick, // a fighter's centre stick: a floor plate, a gaiter, a long shaft and a jet grip with its trigger, hat and
              // pickle buttons; the pivot on the floor between the legs (JET_STICK_GRIP_METERS up to the hand)
    Throttle, // a boat-style throttle: a quadrant hub on its hinge, an arm swinging ahead and back, a crosswise knob
              // (THROTTLE_GRIP_METERS up to the hand)
    Count
};

/// From each model's pivot up its grip to where the hand closes (the tank's: LeverPlacement::GRIP_HEIGHT_METERS).
constexpr float JET_STICK_GRIP_METERS = 0.33f, THROTTLE_GRIP_METERS = 0.137f;

/// Draws the handles into the current eye image: a dozen draws from a vertex buffer, lit by Direct3D's fixed-function
/// lighting; every device state touched is put back through a state block.
/// @param device the game's IDirect3DDevice9.
/// @param carriages each handle's carriage frame (16 floats: rows right, up, back, pivot position; world space; the
/// hull's axes, at the handle's place along its track).
/// @param grips each handle's grip frame (16 floats: rows right, up along the grip, back, pivot position; world space;
/// leaning ahead and, while strafing, to the side).
/// @param triggerPulls each handle's trigger, 0 (out) to 1 (pulled): the controller's trigger of the hand holding it.
/// @param count how many handles (frames in each array).
/// @param stockName the stock vehicle's ODF name (Vehicle::CurrentStockName), or null: picks texture and colours.
/// @param models each handle's model, or null: all tank handles.
void Draw(
    void* device,
    const float (*carriages)[16],
    const float (*grips)[16],
    const float* triggerPulls,
    int count,
    const char* stockName,
    const Model* models = nullptr);

/// A model's triangles in handle space for tools/leverpreview.cpp (the render tool): positions, and each vertex's part
/// * 4 + material (part 0 carriage, 1 grip, 2 trigger; material 0 metal, 1 polished, 2 rubber, 3 button).
/// @return the vertices written (three per triangle), at most `most`.
int PreviewMesh(Model model, float (*positions)[3], int* kinds, int most);

/// Before the device is reset: releases the state block (default pool).
void OnReset();

} // namespace LeverModel

} // namespace SwbfVr
