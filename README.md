# swbf-vr

A VR mod for Star Wars Battlefront (2004), the Steam "Classic 2004" build: a single `dinput8.dll` placed next to
`Battlefront.exe`. It renders the game in stereo through OpenXR (developed with SteamVR and a Quest 3 over Steam
Link) with motion controls: weapons in your hands, holsters on your body, scopes on the weapons, thrown grenades,
vehicles flown and driven with your hands.

**Status:** early test release (0.x). Expect rough edges and the odd crash; keep a backup of anything in the game
folder you care about. Bug reports are welcome as GitHub issues; please attach `swbf-vr.log` and, after a crash,
`swbf-vr-crash.log` from the game folder.

## Trying it

1. Download `dinput8.dll` from the latest release.
2. Put it in the game's `GameData` folder, next to `Battlefront.exe` (Steam: right-click the game, Manage, Browse
   local files, then `GameData`).
3. Start SteamVR, then start the game from Steam. The mod starts VR by itself when SteamVR is running.

To remove the mod, delete `dinput8.dll` (and `swbf-vr.ini` and the `swbf-vr*.log` files it writes).

## Controls

Quest 3 / Touch layout: A and B are on the right controller, X and Y on the left; "grip" is the side button. Most motion controls can be switched off or changed in **VR Settings** (pause menu).

### On foot

| Input | Action |
|---|---|
| Left stick | Walk, in the direction your left controller points (or your head: VR Settings) |
| Right stick left / right | Turn: smooth by default, snap turn in VR Settings |
| A | Jump (with a jet pack: A again in the air jets / starts the hover) |
| B | Crouch (the game's toggle: stand, crouch, prone) |
| Duck your head | Crouch; get right down to go prone (stand up to rise). VR Settings: "Crouch" |
| Y (short press) | First / third person |
| Left stick held down 1 s | Recentre the view |
| Jet trooper hover | Left stick flies where the left controller points; right stick up / down climbs or sinks |

### Weapons and items

You start empty-handed. Weapons hang on your body and are taken with the **right** hand.

| Input | Action |
|---|---|
| Reach to your right (or left) hip and hold right grip | Take that primary weapon; keep holding grip to hold it |
| Let go of right grip | The weapon goes back to its holster |
| Right trigger | Fire |
| Reach to your chest (left or right) and hold right grip | Take a grenade, mine, health / ammo pack or recon droid |
| Swing and let go of grip | Throw it (a faster swing throws further) |
| Fusion cutter, binoculars, detonator | Take it the same way (cutter: front of the belt, left side); right trigger uses it; let go to put it back |
| Hold left grip with the left hand on the barrel | Two-handed grip (steadier aim). Optional virtual stock in VR Settings |
| Reload | An ammo canister hangs at the front left of your belt: grab it with left grip, bring it to the gun and let go. It only appears when you have a spare clip |
| Reload (canister off) | With "Manual reload" off: left hand under the gun, pull left trigger, pull the hand away |

### Zoom and scopes

| Input | Action |
|---|---|
| Right stick click | Zoom in / out (on scoped weapons, the picture shows on the scope's lens) |
| Raise the scope to your eye | Zooms while it is there. Off by default: VR Settings "Zoom: weapon to the eye" |

Letting go of the weapon ends the zoom.

### Menus, map, squad and settings

| Input | Action |
|---|---|
| Hand at your ear + hold grip | Ear menu, picked with that hand's stick (push to the edge and let it spring back): up = recentre, left = pause menu, down = map, right = player list (map and list stay until picked again) |
| Y held 1 s | Pause menu |
| Pointing the right controller + right trigger | Click in menus, the spawn screen (command posts) and end-of-match screens |
| In menus: left stick / A / B | Move / select / back. Left hand at your ear + trigger or grip also resumes |
| Spawn screen | Left stick up / down picks the class, A spawns |
| End-of-match statistics | A = next / details, B = done |
| Empty hand raised in front of your face + grip, then a quick jab | Squad commands: forward = Move Out, back toward you = Hold Position, right = Follow Me, left = At Ease |
| Voice (off by default) | Say "move out", "follow me", "hold position" or "at ease" |

**VR Settings**: pause menu, then **VR Settings**. Changes apply at once and are saved to `GameData\swbf-vr.ini`. Includes turning style and speed, movement direction, crouch by head, comfort vignette, HUD layout, vehicle controls, bike throttle, reload style, two-handed grip and weapon weight, scope options, squad gesture style, voice commands, vibration and hit feedback.

### Vehicles

| Input | Action |
|---|---|
| X | Enter / leave a vehicle or turret (flyers: only on the ground) |
| Right trigger / left trigger | Primary / secondary weapon |
| Right stick click | Next seat |
| Right stick up | Next secondary weapon |
| Your head | Aims the guns in any seat with a gun |

- **Tanks and the combat speeder**: grab the drive stick at your right with right grip; tilt forward / back to drive, sideways to strafe. While you hold it the hull turns toward where you look.
- **Speeder bikes, STAP, tauntaun, kaadu**: grab both handlebars (reins) with the grips. Lean to steer, lean forward over your hands to go, push your hands far out to reverse (or twist the right grip as a throttle: VR Settings "Bike throttle").
- **Walkers (AT-ST, AT-AT, AT-TE, spider walker) and the hailfire droid**: grab the lever at your right with right grip; it has three notches (reverse, stop, forward) and stays where you leave it. The walker turns toward where you look.
- **Starfighters**: right hand takes the flight stick (tilt back = nose up, sideways = turn; its trigger fires, A = secondary weapon). Right thumbstick: up / down pitches, full left / right is a sharp turn. Left hand takes the throttle at your left: three notches, slow / cruise / fast. The lever low on your left: push down to take off, pull up to land.
- **Gunship and MAF**: the stick flies forward / back and strafes, twist it to turn; the left lever raised climbs, lowered sinks. Without the stick in hand the right thumbstick turns and climbs.
- **Droid starfighter**: grab anywhere (no handles drawn); X takes off / lands; hold X 1 s on the ground to get out.
- **Gunner seats and turrets**: look to aim, right trigger fires.
- **Sticks instead of motion** (VR Settings "Vehicle controls" = sticks): the handles are not used; left stick drives, right stick turns, A takes off / lands.

### Special classes

- **Droideka**: A rolls up / stands up. Standing, it aims where you look; triggers fire the primary / secondary. Rolled up, the left stick steers the ball. Right stick up / down or right grip changes weapons.
- **Super battle droid**: hold right grip to raise the arm blasters; right trigger fires, left trigger fires the wrist rocket. Letting go of the grip lowers them and switches to the next weapon. It reloads by itself.
- **Recon droid** (Rebel marksman, CIS assassin droid): take it from your chest and throw it to launch it. Then your head aims it, left stick moves, right trigger fires its blaster, left trigger calls the orbital strike, X returns to your soldier.

The mod drives the game through its standard key bindings (crouch Ctrl, enter E, next secondary weapon G, squad
commands F1-F4). If you have changed crouch or next secondary weapon, set `crouch_key` / `secondary_weapon_key` in
`swbf-vr.ini` to your key (a DirectInput key code in hex).

## Legal

- You need your own, legitimately bought copy of the game. This project does not include or distribute the game.
- This repository and the release DLL contain no files or data from the game or from the LucasArts mod tools. The
  parts of the mod that are made from game assets (cockpit pieces, the arms' and body's meshes and textures) are
  built when the game starts, from the player's own installed game files, and kept in memory only.
- Not affiliated with or endorsed by Lucasfilm, Disney, LucasArts, Pandemic Studios, Aspyr or Electronic Arts.
  Star Wars and Star Wars Battlefront are trademarks of their owners.
- Licence: GNU General Public License v3.0 (`LICENSE`). Copyright (C) 2026 idren2023.

## What the DLL does

It is loaded by the game in place of Windows' DirectInput 8 library, forwards DirectInput to the real system DLL, and
hooks the game's rendering, camera and input in memory (with [MinHook](external/minhook)). Every engine address it
uses is found by a byte signature and checked against a table for the known build (`src/GameBuild.cpp`); a feature
whose addresses are missing is switched off rather than guessed. It writes logs and reads its settings
(`swbf-vr.ini`) in the game folder. It makes no network connections of its own.

## Reading the code

The history is cut into commits by subsystem, in the order that reads best:

1. Project, build and third-party code.
2. The proxy and engine addresses: `Main`, `GameBuild`, `Log`, `Settings`, `Crash`.
3. Stereo rendering: `Xr` (OpenXR), `Stereo` (one game Render per eye, cameras, near clipping, the scope pass),
   `Limits` (the renderer's tables enlarged for the wide view), `Foliage`.
4. Input and movement: `Input`, `Controls`, `Gesture`, `Pointer`.
5. Hands and weapons: `Aim`, `Sight`, `Zoom`, `Holster`, `Feedback`, `Handling`.
6. The first-person arms and body: `BodyArm`, `Body`, `PlayerBody`.
7. HUD and menus: `Hud`, `EarMenu`.
8. Vehicles and cockpits: `Vehicle`, `Flight`, `Levers`, `WalkerDrive`, `GunnerSeat`, `CockpitShell`, `ShellDesigns`.
9. Game assets made at start from the player's own files: `LvlFile`, `GameMeshes`, `GameTextures`, `StockTables`.
10. Network play: `Net`.
11. Tools and documentation.

`docs/ARCHITECTURE.md` explains how the pieces fit together, `docs/offsets.md` lists what is known about the engine,
and `docs/CODE-STYLE.md` the conventions. Comments in the code refer to the developer's own notes (plan items, numbered
test runs, headset sessions, a few design notes); those are kept privately and are not part of this repository.

## Building

Windows, Git Bash and Python 3. No game files or mod tools are needed to build.

```sh
sh tools/setup-toolchain.sh                      # once: fetches the llvm-mingw compiler into re/toolchain
PUBLIC=1 TOOLCHAIN=$PWD/re/toolchain/bin sh tools/build.sh
```

The result is `build/dinput8.dll` (and `build/dinput8.debug.dll` with symbols, for reading crash logs).

## Third-party code

- MinHook, `external/minhook` (BSD 2-clause, see its LICENSE.txt)
- OpenXR headers, `external/openxr` (Apache 2.0, see its LICENSE)
