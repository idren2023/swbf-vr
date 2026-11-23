#pragma once

/// Body: the player's own third-person soldier in first person (PLAN 51, idren2023 2026-10-14): VR Settings "full_body"
/// and a diagnostic (Body.cpp): hooks the soldier render and its bone-set build, logs for the player's soldier whether
/// its body is drawn and what the bone set holds, and can hide the head and move the right hand for a picture. Script
/// command `body`. Needs GameBuild feature FullBody.
///
/// All functions run on the game's main thread, except Configure (script thread).

namespace SwbfVr
{

namespace Body
{

/// Script command `body <arguments>`: "log" switches the hooks and the once-a-second log on; "draw 0|1" makes the
/// player's body visible in first person (draw 1; the game draws only its shadow there) or not; "head 0|1" hides (0) or
/// shows the head; "hand <x> <y> <z>" moves the right hand by that many centimetres in the bone set's own space (0 0 0
/// = off); "place [<back cm> <down cm> <height 0|1|2>]" moves the player's drawn body so its neck sits that far behind
/// the point between the eyes and, with height 1 (lifted) or 2 (scaled about the feet), that far below them (defaults
/// 10, 12, 0), "place off" stops it; "full 0|1" as the VR Settings row full_body; "arms 0|1|2"
/// bends the body's arms to the controllers (1 also hides the first-person arms, 2 keeps them). Takes effect
/// with the next presented frame.
/// @param arguments the text after `body`.
void Configure(const char* arguments);

/// VR Settings "full_body": while on, in VR and on foot with controllers, the player's own body is drawn, placed with
/// its neck behind the eyes and scaled to the eye height, its arms bent to the controllers, the first-person arms and
/// the third-person weapon not drawn (the switches of Configure, set and cleared as that state comes and goes).
/// Main thread.
/// @param isOn the setting's value.
void SetFullBody(bool isOn);

/// VR Settings "height": the player's height, which sizes the full body (scaled about its feet so its standing neck is
/// where the player's is). Main thread.
/// @param centimeters the player's height (clamped to 120 .. 230).
void SetPlayerHeight(int centimeters);

/// Installs the hook of the game's model draw (RedModel draw 0x288680) on its own, once: full body uses it, and so
/// does GunnerSeat (the stock turret hidden in the hull's draw) with full body off. Main thread.
/// @return whether the hook is on (false on a game build without feature FullBody, or if MinHook failed).
bool InstallModelDrawHook();

/// Installs the hooks once they are asked for and writes the summary line once a second. Once per presented frame
/// (called from the Present hook in D3dSurvey.cpp).
void OnPresent();

} // namespace Body

} // namespace SwbfVr
