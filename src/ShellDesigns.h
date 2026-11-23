#pragma once

/// ShellDesigns: one cockpit shell per stock tank (PLAN 54; idren2023 2026-10-17: about 10 000 triangles each, in the
/// tank's own style, with a seat for the player, joining the game's first-person cockpit model so that it looks as
/// if it had always been there; stock vehicles only). Each design is built with ShellKit in the cockpit model's own
/// space and closes everything the game's cockpit leaves open (floor, sides, back, roof) without covering its window;
/// it seats the pilot closer to the window than the game's eye (PilotEye) and carries the pilot's seat and the front
/// console with the slide handles' tracks and the HUD's screens.

#include "ShellKit.h"

namespace SwbfVr
{

namespace ShellDesigns
{

/// Display::part for a display fixed to the head rather than the hull (idren2023 2026-10-27, the droid starfighter: its HUD
/// is the head's own view): its place is in cockpit space as seen from the pilot's eye, carried with the head.
constexpr int HEAD_PART = 100;

/// Display::tint flag: only the minimap's background takes the tint, everything else keeps its own colour (who is
/// friend, enemy, third party or the player; idren2023 2026-10-27, the droid's red HUD).
constexpr uint32_t TINT_KEEPS_RELATIONS = 0x01000000;

/// A screen in a shell's dashboard that shows one of the game's HUD groups (idren2023 2026-10-17: the HUD on the
/// dashboards); CockpitShell draws the group there, Hud then leaves it out of the panel and the wrist.
struct Display
{
    int group;                        // HudDraws::Group
    ShellKit::Vec3 center, right, up; // the group's middle on the screen's face, its axes (cockpit model space)
    float width, height;              // room on the face (metres): the group fills the width or the height; 0: the
                                      // group is left out of the seat altogether (not drawn, not on the head's panel)
    int part;                         // the moving part it is on (ShellKit::Vertex::part; 0 for a cockpit's)
    uint32_t tint; // 0xRRGGBB: the screen's readout colour the group is drawn in (PLAN 57 j), 0: the game's colours
    /// The part of the group shown, as fractions of the group's drawn bounds (left, bottom, right, top, as it appears:
    /// 0 left / bottom, 1 right / top): only its batches whose middle lies inside, scaled to fill the room (PLAN 57
    /// j: the HealthGroup's player and vehicle bars in separate places). 0, 0, 1, 1: the whole group.
    float regionLeft, regionBottom, regionRight, regionTop;
};

/// A stock screen's glass (PLAN 57 j, idren2023 2026-10-19: scan lines over the whole screen, as on the gunner screens):
/// its middle and axes on the glass (cockpit model space), its size (metres), the moving part it is on, the screen's
/// glass finish (texture 5: a plain square of the stock glass, whose alpha carries its scan lines) and the alpha
/// between those lines. CockpitShell draws the lines over it after the HUD.
struct ScanArea
{
    ShellKit::Vec3 center, right, up;
    float width, height;
    int part;
    int glassFinish;
    float glassAlphaFloor;
};

/// Most vertices (three a triangle) of a shell's depth mask (Shell::maskVertices).
constexpr int MOST_MASK_VERTICES = 900;

/// Most scan areas a shell has (a curved screen gives one per 3 cm strip).
constexpr int MOST_SCAN_AREAS = 64;

/// Most displays a shell has.
constexpr int MOST_DISPLAYS = 12;

/// A built shell: its textures (the cockpit's own and its effects texture, by the names the game loads them), its
/// finishes and how many vertices it wrote.
struct Shell
{
    const char* textureName;  // finishes with texture 1
    const char* effectsName;  // finishes with texture 2
    const char* exteriorName; // finishes with texture 3 (null: none)
    const char* glassName;    // finishes with texture 4: the cockpit's own window texture (null: none)
    const char* screenName;   // finishes with texture 5: a stock targeting frame's texture (null: none)
    const ShellKit::Finish* finishes;
    int finishCount;
    int vertexCount;
    bool isFull;
    const Display* displays;
    int displayCount;
    const ScanArea* scanAreas;
    int scanAreaCount;
    /// Triangles (three points each, cockpit model space) a few millimetres before faces of the game's cockpit model
    /// that the shell replaces (idren2023 2026-10-19: the combat speeder's right half, rebuilt as the mirror of its left):
    /// CockpitShell draws them depth only, so the game's model, drawn later in the pass, fails the depth test there.
    const ShellKit::Vec3* maskVertices;
    int maskVertexCount;
    int partCount;     // moving parts (ShellKit::Vertex::part): 1 for a cockpit, 3 for a gunner's turret
    int restPartCount; // the parts CockpitShell::Draw draws at the cockpit's matrix; the others only DrawParts
};

/// The combat speeder's cabin (design "all_hover_combatspeeder") has six parts (PLAN 57 i; the AAT's "cis_hover_aat"
/// the same since PLAN 57 k, its commander on a deck over the rear hatchway): 0 the cabin, 1 the gunner's turning seat
/// (turned about the vertical through his eye by the turret's yaw relative to the hull), 2 the periscope's display
/// housing (on the seat's curved rails, also pitched with the gun about the horizontal through the eye), 3 and 4 the
/// lit stop lamps of the right and left end stop on the housing, 5 the periscope's picture behind the display's glass
/// (3 to 5 drawn with part 2's matrix: the lamps only at that stop, the picture only in the gunner seat). In the pilot
/// seat (CockpitShell::Draw) parts 0 to 2 are drawn at rest.
constexpr int SPEEDER_CABIN_PARTS = 6;
/// The periscope's display (idren2023 2026-10-19: the view of a camera on the gun's muzzle, moving up and down with the
/// gun on a curved rail at a constant distance from the head; a camera display in the stock gunner seat's own screen,
/// all_1st_cockpit_securitycam, not a scope): its glass's centre this far ahead of the gunner's eye along the gun's
/// line, and the glass's half width and half height (the stock glass's 2.5 : 1).
/// Headset run 19 (idren2023: too close to the face, larger, the TIE bomber's sight taller): 0.52 m away, 42 x 19 cm
/// (was 0.38 m, 26 x 10 cm): 44 x 20 deg of view (was 38 x 16).
constexpr float SPEEDER_PERISCOPE_DISTANCE_METERS = 0.52f;
constexpr float SPEEDER_PERISCOPE_HALF_WIDTH_METERS = 0.21f;
constexpr float SPEEDER_PERISCOPE_HALF_HEIGHT_METERS = 0.095f;
/// The display's own pitch limits (degrees, up positive; headset run 17, PLAN 58 d: it follows the head's pitch as
/// the AAT's does, beyond the gun's -15..10; GunnerSeat), and the rails it rides reach past them (ShellDesigns).
constexpr float SPEEDER_PERISCOPE_LOW_DEGREES = -25.0f, SPEEDER_PERISCOPE_HIGH_DEGREES = 15.0f;
/// The building turrets' sliding viewscreens (idren2023 2026-10-24, GunnerSeat BuildingTurret.screenLowDegrees): on a
/// rail about the gunner's eye, pitched by the head. The Hoth dish's as the speeder's periscope; the Geonosian one
/// small (18 x 14 deg) in the gap between the yoke's handles, offset toward the gap's middle (model x, left).
constexpr float DISH_SCREEN_RAIL_METERS = 0.38f;
constexpr float DISH_SCREEN_HALF_WIDTH_METERS = 0.13f, DISH_SCREEN_HALF_HEIGHT_METERS = 0.052f;
constexpr float GEO_SCREEN_RAIL_METERS = 0.30f;
constexpr float GEO_SCREEN_HALF_WIDTH_METERS = 0.0475f, GEO_SCREEN_HALF_HEIGHT_METERS = 0.037f;
constexpr float GEO_SCREEN_OFFSET_X_METERS = 0.027f;

/// The fighter tanks' gunner position (PLAN 56, idren2023 2026-10-17/18; design "<odf>_gunner"): it replaces the stock
/// turret, in three parts that ride the hull's own bones: 0 on turret_y (yaw), 1 on turret_x (the gun's pitch), 2 on
/// turret_barrel1 (the barrel). Its space is the hull model's turned half way round (x right, y up, z back, as the
/// cockpits'; hull model x = -x, z = -z). The standing gunner's eye, on the turret's yaw axis 0.85 m above the deck,
/// in the hull model's own space (x left, y up, z forward): GunnerSeat moves the view there. 0.9 m until PLAN 57 g
/// (headset run 16: about 15 cm too high), 0.75 m until PLAN 58 c (headset run 17: 10 cm higher).
constexpr float FIGHTER_TANK_GUNNER_EYE[3] = {0.0f, 2.65f, -2.858f};
/// The station's right grip (on the yaw part), in the hull model's space: where a hand closes on it (0.6 of the way up
/// its 15 cm), and how far its top leans ahead; the left grip is its mirror in x. GunnerSeat snaps the hands there.
constexpr float FIGHTER_TANK_GUNNER_GRIP[3] = {-0.17f, 2.24f, -2.547f};
constexpr float FIGHTER_TANK_GUNNER_GRIP_LEAN_DEGREES = 13.1f;

/// The AAT's commander (gunner) seat (PLAN 57 k, idren2023 2026-10-19, after the DK cross-section): in the AAT's own
/// cabin shell ("cis_hover_aat", cockpit model space: x right, y up, z back), on a deck over the rear hatchway under
/// the hood's crown, laid out as the combat speeder gunner's (SPEEDER_CABIN_PARTS: the seat turns with the turret, here
/// all the way round; the periscope display on rails with the gun's pitch; no stop lamps; the pilot seat draws parts 0
/// to 2 at rest). The commander's eye on the turning axis, the right grip's hand place on the yoke (the left mirrored
/// about the eye's x) and its lean. GunnerSeat places the eye on the turret's yaw axis in the hull.
constexpr float AAT_CABIN_EYE[3] = {1.35f, 3.0f, 4.45f};
constexpr float AAT_CABIN_GRIP[3] = {1.62f, 2.50f, 4.18f}; // the right armrest's grip (ARMREST_GRIP_*)
/// The AT-TE main cannon's station (design rep_walk_atte_cannon, its eye at (0, 1.5, 1.5)): the barrel's trunnion
/// (aimer_maincannon_x, the pitching part turns about it), its muzzle (hp_fire_maincannon) and the gun camera's
/// viewscreen (centre, then the picture's half width; on the turning part, turned to the eye).
constexpr float ATTE_CANNON_TRUNNION[3] = {0.0f, 0.30f, -1.12f};
constexpr float ATTE_CANNON_MUZZLE[3] = {0.0f, 0.36f, -9.62f};
constexpr float ATTE_CANNON_VIEWSCREEN[4] = {0.0f, 1.06f, 0.86f, 0.19f};
constexpr float ATTE_BAY_VIEWSCREEN[4] = {0.0f, 1.07f, 4.15f, 0.19f}; // the rear gunner's (centre, half width)
constexpr float AAT_CABIN_GRIP_LEAN_DEGREES = 9.5f;

/// The TIE bomber's bombardier (idren2023 2026-10-27: the second seat, ODF TURRET1 "bomber", pitch -10..40, yaw -25..25;
/// design "imp_fly_tiebomber", SPEEDER_CABIN_PARTS): in a turning chair in the pilot pod's tube behind the ball,
/// cockpit model space (x right, y up, z back). The eye on the chair's turning axis, the right grip's hand place (the
/// left mirrored about the eye's x) and its lean; the bomb-sight display on rails about the eye: its distance, half
/// width and half height (GunnerSeat's periscope, as the speeder gunner's).
constexpr float TIE_BOMBER_GUNNER_EYE[3] = {0.0f, 1.45f, 4.40f};
constexpr float TIE_BOMBER_GUNNER_GRIP[3] = {0.20f, 1.13f, 4.08f};
constexpr float TIE_BOMBER_GUNNER_GRIP_LEAN_DEGREES = 10.0f;
/// The Y-wing's gunner (headset run 18, idren2023: the second seat, ODF TURRET1, the dorsal ion cannon turning all the
/// way round, its barrels -10..90; design "all_fly_ywing", SPEEDER_CABIN_PARTS): in a turning chair in the compartment
/// behind the pilot's bulkhead (the hull's roof stays 1.9 m up to 2.8 m back), cockpit model space. The eye on the
/// chair's turning axis, the right grip's hand place (the left mirrored about x 0) and its lean; the display rides its
/// rails with the gun's pitch less YWING_GUNNER_AIM_UP_DEGREES (GunnerSeat aimDownDegrees: a level head aims it up).
/// (2026-11-09, the canon canopy kept whole: its gunner compartment runs from the frame band at z 1.50 to the end plate
/// at 2.82 under a ceiling at 1.74, the pilot's seat back reaching to 2.05: the eye in the middle of what is left, the
/// sight close and small (its whole sweep within 0.35 m of the eye), tilting at most 12 deg up.)
constexpr float YWING_GUNNER_EYE[3] = {0.0f, 1.55f, 2.43f};
constexpr float YWING_GUNNER_GRIP[3] = {0.20f, 1.23f, 2.15f};
constexpr float YWING_GUNNER_SIGHT_DISTANCE = 0.27f;
constexpr float YWING_GUNNER_SIGHT_HALF_WIDTH = 0.11f;
constexpr float YWING_GUNNER_SIGHT_HALF_HEIGHT = 0.06f;
constexpr float YWING_GUNNER_SIGHT_HIGH_DEGREES = 12.0f;
constexpr float YWING_GUNNER_GRIP_LEAN_DEGREES = 10.0f;
constexpr float YWING_GUNNER_AIM_UP_DEGREES = 35.0f;
constexpr float TIE_BOMBER_SIGHT_DISTANCE = 0.38f;
constexpr float TIE_BOMBER_SIGHT_HALF_WIDTH = 0.13f, TIE_BOMBER_SIGHT_HALF_HEIGHT = 0.09f;

/// Where a stock vehicle's shell seats its pilot, in the cockpit model's space (PLAN 57 e: closer to the front window
/// than the game's eye, LeverPlacement::COCKPIT_EYE); Aim draws the cockpit model and the shell so that this point is
/// at the game camera.
/// @param odfName the vehicle's ODF name (Vehicle::CurrentStockName).
/// @param eye set to the eye (3 floats, metres) when the result is true.
/// @return false for a vehicle without a cockpit shell (gunner designs included).
bool PilotEye(const char* odfName, float* eye);

/// The eye a design is built for (any design, gunner designs included), cockpit model space: for previews and tools.
/// @return false for an unknown design.
bool DesignEye(const char* odfName, float* eye);

/// Whether this stock vehicle has a shell.
/// @param odfName the vehicle's ODF name (Vehicle::CurrentStockName).
bool HasShell(const char* odfName);

/// Whether a stock vehicle's shell is its whole cockpit (the starfighters: our own interior at the real size), so
/// the game's first-person model is not drawn.
/// @param odfName the vehicle's ODF name (Vehicle::CurrentStockName).
bool IsWholeCockpit(const char* odfName);
/// Whether a gunner station's display (parts 2 to 5) is drawn over everything else (no depth test).
bool IsDisplayOnTop(const char* odfName);

/// Builds the shell of this stock vehicle into the vertex buffer.
/// @param odfName the vehicle's ODF name.
/// @param vertices where the vertices go (three per triangle).
/// @param capacity how many fit.
/// @param shell set to the result.
/// @return false for a vehicle without a shell.
bool Build(const char* odfName, ShellKit::Vertex* vertices, int capacity, Shell* shell);

} // namespace ShellDesigns

} // namespace SwbfVr
