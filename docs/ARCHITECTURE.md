# Architecture

How the mod works, as of build b22. Addresses are RVAs in the Steam exe linked 2020-07-31 (see `offsets.md`).
Read section 4 before changing `Stereo.cpp`: its rules were each learned from a crash or a hang.

## 1. Shape

- The mod is one 32-bit DLL named `dinput8.dll` in the game's `GameData` folder. The game loads it instead of the
  system DirectInput; the DLL forwards `DirectInput8Create` to the real one (`src/Main.cpp`, `src/dinput8.def`).
- Everything is set up on the game's first `DirectInput8Create` call. By then the Steam DRM wrapper has decrypted
  the game's code, and we are outside the loader lock. The Direct3D import hook alone is placed earlier, in `DllMain`.
- Start-up order (`Main::Initialize`): log header (mod build, Windows), `GameBuild::Identify` (exe hash, address
  table or signature search, one check per address, features switched off; section 12), crash handler, Direct3D
  device hooks, engine survey, script thread, Lua console hook. Everything that hooks the game asks `GameBuild`
  first, so `Identify` runs before any of it.
- Built with llvm-mingw (clang, i686, static runtime). Third-party code is vendored: MinHook (`external/minhook`)
  for inline hooks, the OpenXR headers (`external/openxr`). Nothing else ships: no loader DLL, no config file yet.
- The DLL is still a diagnostic build. The survey, trace, watch, probe and script modules will be left out of a
  release build; `Xr`, `Stereo`, `Limits`, `Main` and (for controller input) `Input` are the product.

## 2. Modules

| File | Role | Product or diagnostic |
|---|---|---|
| `Main.cpp` | proxy export, start-up order, `Main::OpenModFile`, `PatchPointer`, frame timer `Profiling::` | product |
| `Log.cpp` | `Log::` functions, `swbf-vr.log`, old logs kept in `swbf-vr-logs`, startup header | product |
| `Xr.cpp` | OpenXR (`Xr::`): finds and loads the runtime, D3D11 device, session, swapchains, frame timing, eye poses, flat screen, resting pose, simulated headset | product |
| `Stereo.cpp` | hooks the game's Render, camera SetTransform and SetFov; draws each frame once per eye; HUD panel; back-buffer read-back | product |
| `Limits.cpp` | replaces the renderer's fixed tables by larger ones; fill-level monitor | product (monitor is diagnostic) |
| `Foliage.cpp` | the prop generator's cluster pool always at its cap (254), so props do not swap sets every frame in VR (b657; offsets.md "Prop generator") | product |
| `Input.cpp` | synthetic keyboard and mouse merged into what the game reads from DirectInput | product later (controllers), used by scripts now |
| `Crash.cpp` | exception log and no-frame watcher (`swbf-vr-crash.log`) | diagnostic, worth keeping in releases |
| `Aim.cpp` | hook on `Aimer::Set`: replaces the aim direction of the player's Aimer (test: the game's direction turned by an offset) | product (controller aiming) |
| `Lua.cpp` | runs Lua text in the game's own interpreters on the main thread (from the Present hook); hook that sends the game's console output to our log | diagnostic now, product later (console commands, menu control) |
| `Hud.cpp` | the HUD pass redirected into a texture and drawn back as quads in the world; HUD layouts | product |
| `HudDraws.cpp` | hooks on the game's interface element draw, bitmap, masked bitmap and glyph render (installed by Stereo::Install): per interface frame each HUD group's draws (HealthGroup, AmmoGroup, target bar, score, minimap; found by RTTI name) as textured triangles in the group's own plane, slant removed; `SetTaken` makes the game skip its own draws of the groups the mod draws. Hud.cpp draws them straight from the game's textures on the gun, the wrist and in the compact layout (b126). `hud draws` logs one frame (offsets "HUD element draws") | product (health, ammunition), diagnostic |
| `Keyboard.cpp` | virtual keyboard for menus with typed text (b621): a KEYBOARD tab (bottom middle, while the pointer is on the screen) and a 12 x 5 key panel drawn into the flat menu frame in the present path (`Stereo`, before the frame is grabbed or sent to the headset); `Input::MouseButton` asks it first, so a click on it never reaches the menu; keys go out as DirectInput key codes through `Input::Key` (70 ms each), SHIFT holds for the next key; cursor place from `Pointer::GetCursor` | product |
| `Settings.cpp` | `swbf-vr.ini`, the table of settings, the VR Settings screen (Lua) in the pause menu | product |
| `dev/Script.cpp` | test scripts: End key or autorun file; src/dev is compiled in only where present (SWBF_VR_DEV; `PUBLIC=1` leaves it out) and is not in the public source | diagnostic |
| `D3dSurvey.cpp` | Direct3D 9 device hooks (owns the Present hook that drives `stereo`), frame tracer | hooks are product, tracer diagnostic |
| `EngineSurvey.cpp` | RTTI class table, vtable thunks, object graph, memory snapshots, `EngineSurvey::Resolve` | diagnostic; paused while VR is on |
| `GpuTransfer.cpp` | eye images on the graphics card (b268-b272): the game's IDirect3D9 from Direct3DCreate9On12 over our D3D12 device (D3dSurvey's Direct3DCreate9 hook asks first); per image Stage (StretchRect into our A8R8G8B8 target) and Transfer (Unwrap, copy into a shared texture on our queue, fence `written`, Return with it); Xr opens texture and fences in its D3D11 device | product |
| `Zoom.cpp` | zoom in VR: soldier GetZoom hook (camera kept at 1, real factor kept), first-person patches (update and draw kept while zoomed), weapon class zoom fields, lens table by first-person model name (`LENS_MODELS`: lens centres and radius in model space, `GetLenses` for B2; `IsFullView`: no lens = whole view magnified, done in Stereo's eye loop and SetNearFar hook), scope factor ramp and window size (docs/ZOOM.md section 6) | product |
| `Body.cpp` | the player's own body in first person (PLAN 51), diagnostic stage: hooks on the soldier render 0x113690 and GetBones 0x216E50 (Steam RVAs, known build only), script command `body`: alpha branch for the player, bone slots pointed at our matrices; model draw 0x288680 hook: placement (neck behind the eyes, scale about the feet; `Stereo::GetHeadWorld`), two-bone IK of both arms to `Aim::GetHandWorld` at the body's draw, skips the first-person arms' and the third-person weapon's draws; VR Settings `full_body` (`SetFullBody`), on only in VR on foot with controllers; GameBuild FullBody | product (b205), diagnostics kept |
| `BodyArm.cpp` | our own first-person right arm (b374-b377, PLAN 51 B): data generated by tools/body_export.py into re/body/BodyArm.inc, stripped by tools/build.sh into src/data/BodyArm.inc (in git; game parts made at start by GameTextures and GameMeshes), 19 bones, glove + sleeve + stud (9 612 tris), pose library, weapon grips (msh model space -> hand bone, our scale 0.712 inside); Aim's first-person hook collapses the stock right forearm, wrist and their parts (bone 3x3 zeroed, restored after) for a weapon with a grip (all_1st_weap_inf_{pistol,rifle,thermaldetonator} node hash) and draws the arm after the game's draw at weapon world x (arm -> msh by the inverse grip, msh -> game model space as they are: a model's +x is the shooter's left; run427-432); CPU skinning into managed buffers, fixed-function light, game textures by name; weapon placement (since 2026-10-23): the weapon turns with the controller's aim pose (AlignBarrel per model) and is only moved so the grip's axis point sits on the controller's grip pose (GetGripPointInModel), so a weapon tilted in the hand does not change the aim; GetWeaponInGrip (the b380 placement by the grip's axis) is no longer called; 0.07 ms per eye pass; script `bodyarm 0\|1\|axes n\|left 0\|1\|log`; b389 our left arm: the right one mirrored across the left grip pose frame's x (DrawLeft, on foot; Aim collapses bone_l_forearm / bone_l_wrist as the right), posed round the reload canister while held; GetCanisterWorld places item_powerup_ammo by the exported ammocanister grip (x flipped about its middle, so the game draws a proper rotation) | product (b381) |
| `PlayerBody.cpp` | our own model of the player's body, one mesh on one skeleton (b397, PLAN Next session): data from tools/body_export_player.py (re/body/player_stock.blend whole: body, our arms and hands, packs) into re/body/PlayerBody.inc, stripped into src/data/PlayerBody.inc (in git): vertices in msh model space with four bone weights, our 60 weighted bones (parent, rest, inverse rest; rows), the rest eye, BodyArm's arm space -> model (right arm; the left through the mirror across x). Body's model draw hook, with "place" on and the player's bone set, calls Draw once a render with the game's matrix, its foot/toe bones and Body's neck anchor, and skips the game's draws on that set. Solve (model space): scale = (configured height - 0.115) / rest eye height, the game's heading, the neck over the anchor, feet on the game's ground; hips lowered as the spine needs under the neck's target; upper body turned half the head's yaw (<= 45 deg); spine bent at its three joints and the hips to the neck's target; per arm a collarbone shrug toward a far wrist, two-bone IK to BodyArm's wrist (BodyArm::GetHandPose: its last pose of that hand), twist bones take the hand's roll, hand and fingers skinned as BodyArm posed them; legs by two-bone IK to the game's ankles, feet toward its toes. BodyArm draws nothing while PlayerBody::IsDrawingHands. Fixed-function as BodyArm; 0.18 ms per eye pass (run452); script `playerbody 0\|1\|log` | first version (b397) |
| `Squad.cpp` | squad commands by gesture (PLAN 33): either empty hand raised in front of the face + grip arms; jab = Move Out, beckon (quick pull back) = Follow Me, pull down = Hold Position, horizontal circle = At Ease; gives them through the game's `SquadCommand` (voice, HUD display and radius are the game's); called from the Present hook after Holster; script command `squad` | product |
| `Handlebars.cpp` | speeder bikes and the STAP (PLAN 55): the hull model drawn in first person instead of the game's stretched cockpit model, resting eye at the rider's eye, hands on the grips, lean steering and twist-grip throttle added by Vehicle; script commands `bike log`, `bike grip` | product |
| `Levers.cpp` | tank drive handles (PLAN 54 a2, slide handles PLAN 57 d): two handles sliding in tracks in the shell's front console in a hover pilot seat, taken by the grip, three notches 8 cm apart (reverse, neutral, forward; a released handle springs back to neutral, b341), head-yaw turning while a handle is held (b341, PLAN 58 b), drive / turn / strafe added to the seat's inputs by Vehicle, drawn by LeverModel after the cockpit and shell; script command `levers <log>` | product |
| `LeverModel.cpp` | the handles' look (b228-b231, b273): a slide handle (oval carriage, short ribbed boot, grooved grip with a trigger following the controller's, cap, thumb button) built once from lathe profiles (2946 triangles), kept in a managed vertex buffer grouped by part and material (style patch baked in), drawn with Direct3D fixed-function lighting (key light fixed to the hull, fill, ambient, highlights per material): 12 draws, 0.027 ms per eye pass (run234; the b228 per-vertex CPU path took 1.18 ms); textured with a plain patch of the stock tank's own cockpit texture (table `STYLES`; TextureByHash), neutral colours otherwise | product |
| `ShellKit.cpp` | modelling kit for the cockpit shells (no Direct3D, also built into tools/shellpreview): bevelled boxes, panels with rivets, grilles, sweeps (ribs, pipes, cables), lathes, lofted walls; every face mapped by real size onto a patch of the cockpit's texture and cut exactly on the patch's repeat lines (`MappedPolygon`) | product |
| `ShellDesigns.cpp` | one cabin shell per stock tank (AAT, Republic and Imperial fighter tanks, MTT, combat speeder), in the cockpit model's own space, from the game cockpits' rims; its pilot eye (`PilotEye`), seat and front console (handle tracks at the shared places, `LeverPlacement.h`; the HUD screens) | product |
| `CockpitShell.cpp` | draws the current stock tank's shell after the game's cockpit model with that model's matrix: managed vertex buffer per finish, fixed-function lighting, the cockpit's texture and its effects texture (`Textures::FindByName`) | product |
| `EarMenu.cpp` | radial menu of the ear gesture (PLAN 32): state from Controls, drawn by `Hud::Composite` in the head's frame, labels from a GDI texture (managed pool) | product |
| `Jet.cpp` | jet packs (PLAN 36): MinHook on JetJump 0x1103F0 (impulse turned to 45 deg + head pitch, 30-90), the hover's air-movement call 0x11A9C7 redirected (vertical speed from the right stick); player only, VR only; feature `JetPacks` | product |
| `GameBuild.cpp` | which `Battlefront.exe` this is (SHA-256), every engine address the mod uses (`GameBuild::Address`), their signatures and the Steam table, the per-feature checks (`GameBuild::IsAvailable(Feature)`); section 12 | product |
| `engine.h` | struct offsets shared by several modules (`OFFSET_`); offsets used by one module are at the top of its `.cpp`. Not build-specific (section 12) | product |
| `Watch.cpp`, `Probe.cpp` | hardware data breakpoints; call counters and call stacks for any game function | diagnostic |
| `<module>.h` | one header per `.cpp` with what other files use from it (since 2026-10-11); `FpGuard.h` holds `FpGuard`, `Main.h` the mod-folder files, `PatchPointer` and the frame timer. All code is in `namespace SwbfVr` except `DllMain` | |

## 3. Hooks

| What | Where | How | Owner |
|---|---|---|---|
| `Direct3DCreate9` | the exe's import table | pointer patch | D3dSurvey |
| `IDirect3D9::CreateDevice`, device methods (Present, Reset, SetTransform, SetViewport, BeginScene, ...) | d3d9.dll code | MinHook inline (the game creates 12 probe devices and each device object carries its own vtable, so vtable patching misses the real one) | D3dSurvey |
| `IDirectInput8::CreateDevice`, device `GetDeviceState` / `GetDeviceData` / `SetDataFormat` / `SetCooperativeLevel` | dinput8 code | MinHook inline | input |
| `Render()` 0x295340 | game | MinHook; installed when VR, side by side or `fov` is first used | stereo |
| `RedCamera::SetTransform` 0x2913C0, `RedCamera::SetFov` 0x2912F0 | game | MinHook; same moment | stereo |
| queue flush 0x27C1D0 | game | MinHook; only samples fill levels | limits |
| `Aimer::Set` 0x8CE90 | game | MinHook; installed at the first `aim` command | aim |
| `FirstPersonRenderable::Render` 0x130320 | game | MinHook; installed at the first `fp` command | aim |
| soldier GetZoom 0x118330; first-person zoom `jne` 0x1181A5 and 0x13072C; call 0x130713 (hidden check 0x12FFC0) | game | MinHook; 6 NOPs each while VR runs (written between frames by `Zoom::BeforeFrame`); the call rel32 pointed at `IsHiddenHook` once at install (passes the game's answer through unless VR zoom is on) | zoom |
| console printf 0x61440 | game | MinHook; with no console object (always, in the retail game) the text goes to our log as `console:` instead of crashing | lua |
| 14 constants in the renderer's code, 5 queue structs | game code and data | direct writes, once, between frames | limits |
| selected virtual methods of cameras and controllers | game vtables | generated thunks | EngineSurvey |

Our code runs in the middle of game code, so anything doing float maths inside a hook wraps it in `FpGuard`
(saves and restores the x87/SSE state). Functions that replace or sit in front of tiny game functions are
compiled with `general-regs-only` so they cannot touch a float register at all (`Limits.cpp`).

## 4. A frame in VR

The game's per-frame function (0x143440) calls `Update(dt)` twice and then `Render()` once. `Render()` draws every
pass from the engine camera (a `RedCamera`) and ends by calling Present.

1. **Update.** The game computes its camera and calls `RedCamera::SetFov` and `RedCamera::SetTransform`.
   - `SetFovHook`: if a forced field of view is set (the headset's, or a script's), it replaces the game's values.
   - `SetTransformHook`, when VR is running and we are not inside Render: keeps the game's matrix (`_gameCamera`),
     makes a level copy without pitch and roll (`_levelCamera`), and passes on `head pose x level camera`, the
     head pose being the latest known one. The game then finishes Update with the camera the player really has.
2. **Render hook** (`H_Render`), start of frame: `Xr::Service()` handles start, stop and runtime events;
   `Limits::Apply()` (acts once). Then it decides what this frame is:
   - *Stereo for the headset* if VR is running, the first render view shows the 3D scene (list at 0x6ED3C0, flag
     0x6ED400), this frame's Update went through step 1, and the camera's field of view equals the forced one.
     `Xr::BeginStereoFrame()` waits for the headset's frame slot and locates both eyes. For each eye:
     `SetTransform(eye pose x level camera)`, call the game's `Render()`. Its Present lands in `Stereo::OnPresent`,
     which copies the back buffer to system memory and hands it to `Xr::SubmitEye`; the left eye's real Present is
     skipped, the right eye's goes to the monitor. Afterwards the camera is put back and `Xr::EndStereoFrame`
     submits one projection layer.
   - *Side by side on the monitor* if the desktop test is on (`stereo <mm>`): same double render, eye images
     composed into the back buffer.
   - *Flat* otherwise: one normal `Render()`. If VR is running, its Present is read back and shown in the headset
     as a quad 2 m ahead of the resting pose (`Xr::SubmitFlatFrame`). This covers menus, loading screens, the spawn
     screen before the first spawn, and the one frame after VR switches on.
3. After any headset frame `RefreshHead()` stores the head pose and field of view for the next Update.

Rules that must hold:
- **Nothing about the camera changes between Update and Render** except the small eye offset. The first attempt
  set the field of view inside Render; do not go back to that.
- **Xr state changes only in `Xr::Service`, between frames.** A script thread switching the simulated headset off
  in the middle of an eye pass crashed b18.
- **Inside an eye pass, pass the game's own `SetTransform` calls through untouched.** Render re-sets the camera
  from the camera's own matrix in one path (0x2954C5, 0x2956FA); applying the eye offset again would double it.
- A stale camera pointer is never dereferenced: `g_camera` is re-read from the view list at every Render.

## 5. Camera maths

- Engine matrices are 4x4, row vectors (`p' = p * M`), rows = right, up, back, position. Engine space is
  right-handed with the same axes as OpenXR view space (x right, y up, -z forward), so an OpenXR pose converts
  to an engine matrix without any axis swap. Composition: `eye = eyePose * base` (`Multiply(out, a, b)`).
- `Xr::GetViews` returns each eye's pose relative to the resting pose, already in that matrix form.
- Held view heading (b141, replaces b129's body follow): while the player view is shown on foot (EntitySoldier or
  EntityDroideka, seen through the movement hooks) with controllers, `SetTransformHook` gives the level camera a heading
  of its own (`HoldViewHeading`) instead of the game's; `Controls.cpp` turns the character by mouse counts toward the
  left controller's heading (closed loop on `Stereo::GetCharacterYaw`, the character's yaw relative to the view), and
  snap / smooth turns turn the held heading (`Stereo::TurnView`). A recentre lets go for one Update. Elsewhere
  (vehicles, menus, death camera) the level camera takes the game's heading as before. The playing HUD panel hangs on
  the head camera; menus, the spawn screen and large overlays straight ahead (the pointer works on them).
- Resting pose (`Xr.cpp`): head position and heading at the first tracked frame, or when Home is pressed. The game
  camera sits exactly there. OpenXR's LOCAL space is used as is; only the difference to the resting pose matters.
- Field of view: the engine's projection is symmetric, so the image covers the union of both eyes
  (`tanX = max over eyes and sides`, same for `tanY`). `SetFov(fovRadians, tanY/tanX)` is fed `2*atan(tanX)`. The
  layer is submitted with the tangents the camera really used (camera +0xA4, +0xA8, after the game's zoom).
- The image is always the game's back buffer. Its aspect does not matter to correctness (the layer says which
  angles it covers) but pixels are not square in angle.

## 6. HUD panel

The game draws HUD and in-match menus in a last pass with its own fixed projection (m11 = 1.7320508,
m22 = 2.3094011) over the whole target. `D3dSurvey` reports every projection the game sets
(`Stereo::OnProjection`). During a headset eye pass, that projection switches the viewport to a panel; any other
projection switches it back, and the eye's Present restores it in any case.

The panel is a point 3 m along a direction, projected into each eye (which gives it depth), about 76 degrees wide
(`kHudHalfWidthTan`), clamped to stay inside the image. The direction is the game's aim (unlevelled game camera)
when the camera was set from chase-camera code (caller RVA 0x13EE00 to 0x13F800), else straight ahead.

Limits of this approach: the panel has the eye image's resolution (about 1090 x 460 pixels); markers the game
positions over world objects in the HUD pass are placed for the full screen and end up in the wrong spot;
full-screen effects drawn in that pass (damage flash, scope) shrink with it.

## 7. OpenXR details

- Runtime lookup: `XR_RUNTIME_JSON`, then `HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime` in the 32-bit registry
  view, then the 64-bit entry's sibling `steamxr_win32.json`, then SteamVR's folder from
  `%LOCALAPPDATA%\openvr\openvrpaths.vrpath`. On the test PC the 32-bit view is empty and the sibling file is what
  works. The library is loaded directly and negotiated with `xrNegotiateLoaderRuntimeInterface`; API layers are
  not supported.
- Only `XR_KHR_D3D11_enable` is used (SteamVR's 32-bit runtime also offers D3D12, Vulkan and OpenGL). The D3D11 device is ours, on the adapter the runtime names; if that adapter
  is not listed (SteamVR's simulated headset names 0:0), on the adapter the game renders on, else the first.
- Swapchains: one per eye and one for the flat screen, created at the back buffer's size on first use, format
  B8G8R8A8 sRGB preferred (byte-for-byte the game's back buffer). Pixels arrive with `UpdateSubresource`.
- Session states: READY begins the session, STOPPING ends it, EXITING or loss pending shuts VR down and the game
  continues flat. After a failed start VR stays off until `vr on`.
- Auto start: on the first presented frame, if `vrserver.exe` is running.
- All OpenXR calls are made on the game's render thread.
- Seen from SteamVR 2.17.10 with its simulated headset: runtime name "SteamVR/OpenXR in Meta compatibility
  mode", recommended eye size 1788x1988, three swapchain images, field of view 45 degrees each way.
- Simulated headset (`vr sim`): no OpenXR calls at all; fixed or swinging head pose, Quest-like asymmetric field
  of view, both eyes side by side on the monitor. Everything in `Stereo.cpp` runs exactly as with a headset.

## 8. Renderer limits

The renderer collects each frame's draw calls in fixed tables sized for a 70-degree view. With the headset's view
the transparent-object queue (a linked list of 800 nodes at 0x657AD8) overran and the game crashed in the queue
flush (0x276763) or hung. The exact failure inside the game's queue code was not traced; raising the limits
removed it, and the monitor confirms the old limit is exceeded (813 of 800 on Naboo: Plains).

`Limits::Apply` runs once, at a frame start when all queues are empty:
- item table 4648 -> 32768, matrix pool 3062 -> 32768, light records 2048 -> 16384: new memory, and the constants
  in the code that hold the table address and size are rewritten (each is checked first; on any mismatch nothing
  is changed and the log says "different game version");
- three sorted queues and two list queues, 8 times their size: new memory, queue structs repointed.

The new tables come from `VirtualAlloc`, never from the game's allocator (emptied at level load). Two real
overflow bugs in the game sit behind the larger tables: the sorted queues accept one entry more than their array
holds, and the matrix block allocator returns a block past the end when it wraps. `limits: peak use` lines in the
log show how close a map gets to the original limits. Other maps and modded maps are untested.

## 9. Scripts and input

`Input.cpp` merges synthetic key and mouse state into the game's DirectInput reads, so scripted input is exact
and independent of window focus quirks. While a script runs, the real devices are ignored.

A script is a text file, one command per line, `#` for comments. It runs when End is held for 0.3 s
(`GameData\swbf-vr-script.txt`) or once at start (`GameData\swbf-vr-autorun.txt`, renamed to `.running.txt`
when claimed, started 15 s after launch).

| Command | Meaning |
|---|---|
| `mark <text>` | writes `MARK <text>` to the log |
| `wait <ms>` | |
| `key <DIK hex> <ms>`, `keydown <hex>`, `keyup <hex>` | DirectInput scan codes (W 11, A 1E, S 1F, D 20, Space 39, Q 10, X 2D, Z 2C, LCtrl 1D) |
| `mouse <dx/s> <dy/s> <ms>` | steady mouse movement in counts per second; negative dy looks up; 400 counts/s for 8 s is about one full turn |
| `click L|R|M <ms>`, `wheel <notches>` | |
| `stereo <mm>` | desktop side by side with that eye distance; 0 = off |
| `fov <tanX*1000> [tanY*1000]` | force the engine field of view; 0 = off |
| `vr on`, `vr off`, `vr recenter` | start OpenXR now; stop VR and the simulated headset; take a new resting pose |
| `vr hands <0/1> [period ms]` | simulated controllers for `vr sim`: at rest, or moving with that period |
| `vr sim <yaw> <pitch> [period ms]` | simulated headset, head turned by those degrees (yaw left positive, pitch up positive); with a period the head swings between plus and minus |
| `hand <x> <y> <forward> <yaw> <pitch>`, `hand off` | weapon, arms and aim from one pose: centimetres from the eye (in VR: from the resting head), degrees left / up |
| `lhand <x> <y> <forward> <yaw> <pitch>`, `lhand off` | left forearm and hand from their own pose (position = wrist), same units and base as `hand` |
| `fp <x> <y> <z> <yaw> <pitch>`, `fp off` | move the first-person arms and weapon about the eye: centimetres right / up / back, degrees left / up |
| `aim <yaw> <pitch>`, `aim off` | turn the player's weapon aim away from the view by that many degrees (left and up positive) |
| `lua <text>`, `luashell <text>` | run one line of Lua (up to 500 characters) in the mission's or the menu's interpreter; waits for the next frame; the log line gives lua_dobuffer's status (0 = ran, 3 = Lua error, -1 = that interpreter does not exist now) |
| `trace` | trace the next frame's Direct3D calls to `swbf-vr-trace.txt` |
| `watch <slot 0-3> <root>+<hex offset> <w|rw>`, `unwatch` | data breakpoint on a live object field; roots: cam, red, chase, pc, soldier, aimer, weapon |
| `probe <rva hex> [name]`, `probes`, `stack` | count calls of a game function and capture call stacks; report; stack of the next Present |
| `textures`, `texsave <hash> ...` | log the game's texture table and one frame's lookups (`textures:` lines, `tools/texnames.py` names them); write textures to `swbf-vr-tex-<hash>.bin` (`tools/texview.py` makes BMPs) |
| `keyboard <0/1>` | hide or show the virtual keyboard (test: `pointer <u%> <v%>` then `click L 120` presses the key under the cursor; `tools/scripts/keyboard-test.txt`) |
| `exit` | end the game process |

### Aim
`Aimer::Set(origin, direction)` (thiscall, two pointers to three floats) stores the fire origin (+0x70 and +0x7C) and
the aim direction (+0x48). For the player it is called from the soldier update (return address 0x11D6D9) every
frame with the eye position and the camera's forward direction. `Aim.cpp` passes a different direction for the
player's Aimer only (found once per frame through the PlayerController). Seen in run21 (b26), desktop, by
screenshots: with 30 degrees left the bolts leave to the left of the crosshair; with 25 degrees up the
third-person soldier raises the rifle. idren2023, watching the same run: bolts went left and then up in both first and third person; the
third-person model appeared to follow the fire direction (he would want a second look to be sure); the crosshair
stayed at the view centre; and the bolts visibly started at the rifle's muzzle, which fits the weapon ODFs
(`FirePointName = "hp_fire"`, a node in the weapon model). So the origin given to `Aimer::Set` (the eye) is not
where bolts are drawn from: it is presumably the start of the aiming ray, and the visible bolt starts at the
weapon model's fire point. Consequence for controllers: placing the weapon model at the hand should move the
bolt's start with it; the Aimer origin then decides where the shot is aimed from. Still open: the first-person
weapon model did not visibly turn at 30 degrees left (a screenshot).

### First-person arms and weapon
`FirstPersonRenderable` (one per view; pointers in the table at 0x4E3008, indexed by view) owns what the first-person
pass draws: the arms model (pointer +0x15FC) with the matrix at +0x1570 and the weapon (pointer +0x1610) with the
matrix at +0x15B0. Both matrices are in world space, row vectors; on foot the arms origin sits 0.37 m ahead of the
eye and the weapon origin about 0.23 right, 0.3 down, 0.58 ahead. The skeleton pose is at +0x830. State +0x160C:
0 on foot (update 0x131890, uses "bone_root"), others use an "hp_camera" node (0x131EA0, 0x1328C0; vehicles and
turrets, not looked at). Update entry: 0x131640(view, arg) -> 0x131660 (dispatch by state). Render: vtable slot 4,
0x130320, dispatch by state to 0x1303C0 / 0x1306E0 / 0x130430; the last hands the weapon matrix to the weapon
object (its virtual calls +0x90 and +0x34) while drawing.
`Aim.cpp` hooks the render method and, for the duration of the call, replaces both matrices by ones moved and turned
about the eye (`fp` command). Seen in run22 (b27): yaw 30 puts the rifle left of centre pointing left, an offset of
25 cm left / 10 cm up puts it under the crosshair, pitch 20 raises it; the arms move with it and show their cut ends.
Open: whether bolts and the muzzle flash start at the moved muzzle (no bolt was caught in the screenshots).

### Hand pose (b28)
`hand <x> <y> <forward> <yaw> <pitch>` gives one pose, relative to a base frame, that drives everything a motion
controller will drive: the weapon matrix becomes `turn180y x hand x base` (the weapon model looks along its +z),
the arms keep their game-computed place relative to the weapon, and the Aimer direction becomes the hand's forward
axis. Base frame: in VR the level camera (the resting head, so the pose is in tracking space and independent of
the head); without VR the game's camera. It is cached once per presented frame (`Aim::OnPresent`), so the aim is
one frame late. The game's own look is about `hand 23 -30 58 0 0`.
Seen, by screenshots: run23 (desktop) the stock pose looks like the unmodified game; other poses put the rifle
where asked, and with the pose high and pitched down the bolts go downward. Run24 (simulated headset) the rifle
stays put in the world while the head turns left and right (it moves to the opposite side of the image) and turns
with the body when the mouse turns it. idren2023, watching run23: the muzzle flash always sat at the moved rifle's muzzle; the bolts did not: their start
moved independently, sometimes close to the muzzle, sometimes far from it; and at the end the left forearm looked
turned independently of the rest of the model. So the flash is drawn from the render-time weapon matrix, the bolt
start is computed elsewhere (not from the matrices as we change them at render time).
Bolt start (b29): the fire code (0x2461A0) takes the bolt's start from Aimer+0x7C, the origin given to
`Aimer::Set`, and its direction from the Aimer's aim matrix (0x8C620) plus spread. The origin the game passes is
not the eye and not the muzzle: it is a fixed point about 9 cm right and 16 cm above the weapon model's origin
(measured in the weapon's frame; it does not move with recoil). In hand mode the hook now moves that point
rigidly with the weapon (expressed in the game's weapon matrix, re-emitted from ours). Run25: a screenshot of the
low-left pose shows the flash at the muzzle and a bolt beyond it along the barrel. idren2023 on run25: bolts flew along the barrel but started slightly above and behind the
muzzle in the moved poses. That is the game's own origin: the stormtrooper rifle's `hp_fire` node is at
(0, 0.042, 0.408) in the weapon model (from the mod tools' `imp_1st_weap_inf_rifle.msh`, `tools/msh.py`), so the
game's point is 0.41 m behind and 0.12 m above the muzzle; in the stock view that is hidden by looking along the
barrel. b30 started the bolt at the muzzle with that rifle's value hard-coded (idren2023: bolts exactly from the muzzle
in all poses, correct direction; the stray left-forearm motion is the recoil animation).
b34 reads the node from the loaded model, with the game's own functions: FirstPersonRenderable+0x1610 is the
soldier's Controllable part (soldier+0x250, not the weapon); soldier+0x8F0 Aimer; Aimer+0x128 current weapon;
weapon+0x64 its WeaponClass; class+0x40 the first-person model (`HighResGeometry`; +0x3C is the third-person
one); the node is found by the hash of its name (`HashName`, 0x3007F0) in the model's node table (model+0xC0: +0x00
array of 4x4 matrices, +0x0C array of name hashes, +0x10 count) and its position is read from its model-space
matrix (b35), so a fire point that hangs under another node is handled. Cached per weapon class.
Run30/run31: rifle 0.000 0.042 0.408 (node 1), pistol 0.006 0.029 0.205, identical to the source models; idren2023:
bolts perfectly placed for both weapons in every pose.
Survey of all ODFs and source models in the mod tools (2026-10-06), infantry weapons only:
- 54 weapons have a first-person model (`HighResGeometry`). In every one that fires bolts the model's node is
  called `hp_fire`, also where the ODF says `FirePointName = "hp_fire_ALL_INF_BlasterRifle"` (6 weapons: that
  name belongs to the third-person model; the first-person model has plain `hp_fire`). So looking for "hp_fire"
  in the first-person model is right for the stock game. A mod that names it differently falls back.
- `hp_fire` under another node: the three fusion cutters (under `tip`). Not seen in game yet.
- First-person model without `hp_fire`: sonic blaster, wookiee bowcaster, mines, grenades (thrown, no bolt),
  rocket launcher `com_1st_weap_inf_torpedo` (not checked). These fall back to the game's own point carried with
  the weapon.
- 48 infantry weapons have no first-person model at all (heroes' powers, natives, remote droids, orbital strike):
  fallback as well; what the first-person pass draws for them is unknown.
- Vehicles, turrets and flyers use many names (`hp_cannon_1`, `hp_gun_2`, `hp_fire_lgun`, several per weapon) and
  a different first-person state (`hp_camera`): deferred.
A held trigger builds up spread; tests fire single shots (`click L 150`, `wait 700`).
Not done: recoil and other weapon
animation applied by the game to the weapon matrix is dropped; nothing checked in a real headset.

### First-person skeleton (from the mod tools' source models, e.g. `imp_1st_stormtrooper.msh`)
Six nodes matter: `dummyroot` -> `bone_shoulders` -> `bone_l_forearm` -> `bone_l_wrist` and `bone_r_forearm` ->
`bone_r_wrist` -> `hp_weapons`; plus `hp_camera` on `dummyroot` (the node the game aligns with the camera).
The weapon model hangs on `hp_weapons` of the right wrist. Nothing attaches the left hand to the weapon: the
first-person animation bank for the weapon type (`AnimationBank = "rifle"` in the weapon ODF) poses both arms so
that the left hand rests on it. There are no upper arms and no fingers: each arm is a forearm and a wrist.
For controllers this means: right controller = `bone_r_wrist`/`hp_weapons` (today done by moving the whole model
with the weapon), left controller = `bone_l_forearm` + `bone_l_wrist`, to be overridden in the pose at
FirstPersonRenderable+0x830 after the game has animated it.
In memory (b36): `S = *(FirstPersonRenderable+0x830)`; `[S+0]` definition (`[def+0]` bone records of 0x70 bytes,
name hash at +0x5C; `[def+4]` count); bone matrices at `S+0x10`, 0x40 bytes each, in the arms model's space
(row vectors, root at the origin). The stormtrooper's eight: 0 dummyroot, 1 bone_shoulders, 2 bone_l_forearm,
3 bone_l_wrist, 4 bone_r_forearm, 5 bone_r_wrist, 6 hp_weapons, 7 hp_camera (0, -0.22, -0.414, as in the source
model). A bone's +z points toward the hand; forearm length 0.271.
`lhand <x> <y> <forward> <yaw> <pitch>` (b36): in the render hook the left wrist is put at the pose's position and
the forearm trails behind along the pose's direction (`forearm = turn180y x pose`, moved back by the forearm's
length, converted into the arms model's space; the wrist keeps its animated place relative to the forearm); the
two matrices are restored after the draw. Run32: the forearm appears detached from the rifle at the left edge of
the view where asked; no crash. idren2023 on run32: recoil works (rifle and both hands kick); the left arm left the rifle and stayed put while
firing, but still showed the kick (the wrist keeps its animated offset from the forearm); the poses chosen put
the left arm at the edge of or outside the flat view.
Recoil (b36): with K = the weapon in the arms model's space as the game animates it and K0 = its resting value
(the last K unchanged for 20 draws), the arms model is placed at `K0^-1 x target` and the weapon at `K x arms`,
so the resting weapon sits exactly on the hand pose and recoil, reload and switch animations play around it.
Since b58 the weapon itself is put exactly on the hand and, once a resting pose is final (72 still draws, with all
bones), the bones are frozen too, so no game animation shows. Resting poses are kept per weapon class (8 slots,
`RestingPose`). Weapon swap (b122): the class changes when the swap begins and the lowered weapon holds still for
about 20 draws, so during the first 150 draws after a change a sample more than 30 cm from another weapon's
resting pose is refused; a class with no pose yet is placed with the most recent final pose of another class
(`FindOtherFinalPose`). `fp: swap draw` log lines show K, K0 and the bones for 160 draws after each change.
Two-handed with the virtual stock (b119, `ApplyTwoHandedGrip`): the right controller is ignored; the gun pivots on
a point kept at a fixed offset from the head position, starts from last frame's pose and is turned to the left hand.

### Controllers (b37)
`Xr.cpp` creates one OpenXR action set with a single pose action for both hands, bound to `input/aim/pose` for the
Touch, Index, Vive, WMR and simple-controller profiles; `LocateHands` syncs and locates both action spaces in the
LOCAL space every frame (in `LocateEyes`, between begin and end frame). `Xr::GetHandPose(hand, matrix)` returns the pose
relative to the resting pose in the camera's matrix convention (0 right, 1 left). `Aim::OnPresent` asks for both
every presented frame; a tracked hand takes over from the `hand` / `lhand` test values (log: `hand: right hand
follows its controller`), installs the hooks on first use, and lets go when tracking is lost.
The aim pose's origin becomes the weapon model's origin and its -z the barrel direction: no grip offset yet.
Simulated controllers for the desktop: `vr hands 1 [period]` with `vr sim` (right hand in front of the right
shoulder, swinging when a period is given; left hand circling in front of the left shoulder).
Run33 (simulated headset and controllers, screenshots): both hands at rest where expected, the rifle swinging
and the left forearm circling as separate pieces, both staying put when the head turns, stock look back after
`vr hands 0`. **The real OpenXR path (action set, bindings, xrSyncActions, xrLocateSpace) has never run**: it
needs a session with real or runtime-simulated controllers. Buttons, sticks and triggers are not read yet.

### Lua
The game has two Lua 4.0.1 interpreters: one for the menus, one created for each mission. Each belongs to a script
object whose first field is the `lua_State`; pointers to the two objects are globals (`offsets.md`, "Lua"). The
mission pointer is null in the menus. `Lua::Run` queues one chunk and waits; `Lua::OnPresent` runs it with the
game's `lua_dobuffer`. Globals defined by one chunk stay for later ones (until the mission ends).
Reading results back: Lua's own file functions work, so a chunk can append to a file in GameData:
`function vrlog(s) local f=openfile("swbf-vr-lua.txt","a") write(f,tostring(s).."\n") closefile(f) end`.
Seen working (b23-b25): menu globals (`gFinalBuild` = 1, `gPlatformStr` = PC); clearing `gFinalBuild` makes the
pause menu show Free Camera and Fake Console; `ScriptCB_GetConsoleCmds()` lists only six commands (skipplayer,
toggleholos, rendercollision, invincible, unlimitedammo, framerate); `ScriptCB_DoConsoleCmd("framerate")` shows the
frame counter (and crashed before the console hook existed); a mission started from the profile screen with
`ScriptCB_SetMissionNames` + `ScriptCB_SetTeamNames` + `ScriptCB_EnterMission`.

**Network games (b588-b598, `src/Net.cpp`; docs/research/netcode.md "Built").** A net game keeps a host world and a client world in one process, and a client tells the host only a 20-byte control state per game turn. `Net::IsNetGame / IsHost / IsClientWorldActive / IsLocalPlayerStep` read the game's flags (Steam build only). `Net::SetLocalControlWriter` hooks the game's control state copy 0x1AF320 and calls one writer per turn on the local player's state (`Net::ControlState`) before it is quantised, predicted and sent: the only place the mod's controls reach the host. Controls' writer (`WriteNetControlState`) puts in: the stick's movement, turned from the left hand's heading to the character's (`WriteNetControls`); on foot the turn and pitch rates that steer the soldier to the gun hand's fire line (`WriteNetAim`, closed loop on the client world's soldier from `Net::GetLocalSoldier`; aimed from the soldier's own fire point at the point of the hand's line where the soldier's aim ray ends, since the host's shots start at that fire point); in a seat the head aim's look rates and the levers' drive (`WriteNetSeat` from `Vehicle::GetNetSeatInputs`). `Net::PressButton` presses a game button for one turn (Squad). In a net game the hooks below PlayerController do not set anything (SetStrafe / SetMove, SetYaw / SetPitch, lever writes), and what has no channel is left to the game (manual reload, RequestWeapon, jet direction, throw strength, droideka arm target, flyer height, turret angles). Single player takes none of these paths. Script commands: `net` (flags, both worlds' soldier and Aimer), `net trace 1|0` (one line per turn, read by `tools/nettrace.py`; `tools/netaim.py` compares the two worlds' lines of fire), `start <folder> | <args>` (untested). Scripts: `net-host-test.txt`, `net-aim-test.txt`, `net-tank-test.txt`. Since b603 the soldier is aimed at the point the hand's line hits (`Net::CastAimRay`, the game's ray cast), and since b605 a grenade let go is solved in `Holster.cpp` (`SolveNetThrow`: the hand throw's resting place, then the soldier's aim whose full-speed throw rests there, held for 0.7 s through `Aim::SetNetThrowAim`).

## 10. Files the mod writes (all in GameData, overwritten at every launch)

| File | Content |
|---|---|
| `swbf-vr.log` | everything, written through `Log::Debug/Info/Warning/Error` (`src/Log.h`). Each line: GetTickCount, level (`DEBUG`, `INFO`, `WARN`, `ERROR`), subsystem, text, e.g. `123456 INFO  xr: session created`. Subsystems: `xr`, `stereo`, `limits`, `script` (with `MARK` lines), `d3d`, `input`, `survey`, ... Debug lines only with `log_debug = 1` in swbf-vr.ini (none use it yet). First lines (`init`): build, Battlefront.exe hash and known build, Windows version; `settings` lists the ini values that differ from the defaults; `xr` names runtime and headset when VR starts |
| `swbf-vr-crash.log` | fatal exceptions with registers and a rough backtrace (module+offset); "NO FRAME for 4 s" entries with three samples of where the render thread is |
| `swbf-vr-trace.txt`, `swbf-vr-snap.bin`, `swbf-vr-dump.bin` | frame traces, memory snapshots, the image dump (diagnostic; large) |

## 11. Known weak points

- Stereo needs the game to call SetFov every frame, which it only does once the player has spawned. After a
  death the camera keeps the forced field of view, so the respawn screen is expected to be drawn in stereo with
  its menu on the HUD panel, while the very first spawn screen is flat (seen). Dying has not been tested in VR.
- The head pose used during Update is one frame old, and both eyes use eye 0's orientation for it.
- VR starts only if SteamVR is already running at launch; there is no key to start or stop it.
- No handling yet for a device Reset (alt-tab) while VR is running beyond releasing our Direct3D 9 targets.
- Split screen: only the first view is handled.
- First-person arms are modelled for a narrow view and show their cut ends.
- The zoom key is expected to shrink the image to a window instead of magnifying, because the layer is submitted
  with the true angles. Confirmed in headset run 10 (a small box with black around it); redesign in `docs/ZOOM.md`.

### Controller buttons and sticks (b38, `src/Controls.cpp`)
`Xr::GetControllerInput(Xr::ControllerInput*)` gives trigger, stick and two buttons per hand (index 0 right, 1 left), from OpenXR actions (bindings for touch, index, vive, WMR, simple) or from the script command `vr pad`. `Controls::OnPresent()` runs each frame from `H_Present` and feeds the game's own input path:
- left stick -> W/A/S/D (on at 0.45, off at 0.30), right trigger -> mouse button 0, A -> Space, B -> left Ctrl. Held keys are sent again every 500 ms.
- right stick x -> turn through `Input::MouseMove`, so character facing and view turn together. Snap (default 30 degrees, arms at 0.70, re-arms at 0.35) or smooth (degrees per second). `_countsPerDegree` = 3200/360, measured with idren2023's mouse sensitivity; `turn counts` changes it.
- Script: `turn snap <deg>`, `turn smooth <deg/s>`, `turn counts <x100>`, `vr pad <lx> <ly> <rx> <ry> <trigger> [a] [b]` in percent.
Left wrist (b38): `T(0,0,forearm length) * forearm`, rigid, so the recoil animation no longer reaches the left hand.
The game clamps one frame's mouse movement to about 33 degrees of turn, so `Controls.cpp` queues turns and feeds at most 200 counts per frame (b40). Script command `heading` logs the base frame's heading for measuring turns.

### Test instruments in the render path (b41-b52)
Test clips (b628-b629): `record <name> <seconds> <fps> [eye]` writes the left (or right) eye pass or a flat frame at half size as swbf-vr-rec-<name>-NNNN.bmp as they come (names up to 31 characters); `vr glide <ms>` eases new `vr sim` / `vr handpose` targets over that time (yaw not wrapped: 0 -> 360 is one full turn); `vr life <0|1>` a slight sway of the simulated head and hands. tools/clip-run.sh turns the frames into re/clips/<name>.mp4.

All off unless a script asks. `grab <n> [mode]` (Stereo.cpp): images kept in memory, written as BMP when the last is in; mode 0 eye passes, cut of the lower middle at full resolution; 1 the presented image at quarter size; 2 eye passes, whole image at half size. `camlog <n>`: camera matrices the game sets, and the camera at Render start. `drawlog <n>` (D3dSurvey.cpp): draw calls and primitives per pass. `pace:` log lines every 2 s while VR runs: time between presents. `vr readback <0|1>`, `vr cap <fps>`, `vr mirror <0|1>` (simulated headset side by side or right eye alone), `vr test <bits>` (1 no level camera, 2 no HUD panel, 4 no head camera in Update, 8 forced fov on every camera). `SetFovHook` logs the first 24 different (camera, caller, fov) it sees.

### Settings and the VR Settings screen (b73, `src/Settings.cpp`)
One table, `_settings`: name in `swbf-vr.ini`, current value as text and, for settings shown in the menu, a label
and the values the menu steps through. `Settings::Load` (first presented frame) reads the file and hands every value
to its module (`ApplySettings`); a change from the menu applies that one setting and rewrites the whole file.
The screen is Lua text built by `BuildScreenChunk` (the table as `swbfvr_opts`, then `LUA_SCREEN`) and run in the
mission's interpreter with `Lua::Execute`: a normal `NewIFShellScreen` named `ifs_vr` with `AddVerticalButtons`, labels
set as raw text (`ScriptCB_tounicode` + `RoundIFButtonLabel_fnSetUString`). The pause menu's `Enter`,
`Input_Accept` and `Exit` are wrapped (the originals are kept in globals; the game looks these fields up at call
time): Enter unhides the stock "console" button under the label "VR Settings", Accept on it pushes `ifs_vr`.
Lua talks to the mod through `swbfvr_cmd("<text>")`, a C function registered with the game's own register helper:
`set <name> <value>`, `menu 1|0` (pause menu entered / left for good), `ping`, `loaded`, anything else is logged.
`Settings::OnPresent` runs a ping chunk once a second; without an answer (new mission = new interpreter) it
registers the function and runs the screen chunk, which does nothing until `ifs_pausemenu` exists and builds the
screen at most once per interpreter. Lua 4.0 rules apply (no closures over locals used; `getn`, `..`).
`Settings::IsMenuOpen()` switches `Controls.cpp` to menu input (stick = arrow keys, A = Enter, B = Esc) and lets
`Stereo::ComputePointerUV` use the HUD panel in a match. To add a setting: one line in `_settings`, one case in `ApplySettings`.
The Lua side can be tried outside the game with a Lua 4.0 build and stubs for the interface functions.

### HUD layer (b76, `src/Hud.cpp`; replaces section 6's viewport panel, which remains as fallback and as `hud mode 0`)
`Stereo::OnProjection` still recognises the HUD pass by its projection. In a headset eye pass it now calls
`Hud::OnProjection`: on the HUD projection the render target becomes a texture (A8R8G8B8, back buffer size, cleared
to transparent once per eye pass) and separate alpha blending is switched on (alpha: one, 1 - source alpha) so the
texture ends with premultiplied colour and true coverage; on any other projection the game's target, viewport and
blend setting come back. If the texture cannot be made (multisampled target, creation failure) the old viewport
panel is used. At the eye's Present (`Stereo::OnPresent`, before grabs and the read-back) `Hud::Composite` draws
into the eye image inside its own BeginScene/EndScene, fixed function, states saved and restored with a state
block, view = inverse of the eye camera, projection from the pass's tangents, blend (one, 1 - source alpha), no
depth test. `g_compositing` keeps our own SetTransform calls from being taken for the game's.
Geometry: `Quad(frame, distance, centre, half size, uv)` puts a piece of the texture on the plane `distance` ahead
in any frame (camera convention). Today every quad uses the panel's frame (the aim camera)
at 3 m with the panel's size, so `Stereo::ComputePointerUV` and the menus are unchanged. `isWhole` (menu open, spawn screen,
camera not the player's) draws one quad. While zoomed (b103) the game's camera stays unzoomed and the scope picture is drawn by Stereo's `DrawScope` on the weapon's lens (B2, b117: lens frame from `Aim::GetWeaponWorld`) or as a window of its own (docs/ZOOM.md section 6: an extra `Render()` per frame, `Hud::CaptureScope`, `Hud::DrawScopeWindow`). Otherwise: elements from `STOCK_ELEMENTS` / `_elements` (rectangles in screen
fractions) were drawn at `0.5 + (centre - 0.5) * spread`, scaled by `hud_size`, and "the rest" was the panel cut
along the element edges. **Since b186 (PLAN 11 d) there are no cut-outs:** compact and immersive take every HudDraws
group (`SetTaken`: health, ammunition, target, score, map) and draw it from the game's textures at its moved place
(`movedGroup`) or on the gun / wrist (`drawnGroup`); the panel is always drawn whole, once, and only holds what the game
still draws (messages, markers, vehicle displays, the reticle unless the hands aim). Without the HudDraws hooks the HUD
stays where the game puts it (classic). Wrist display, ammo on the gun and so on are further quads with
another frame. Script: `hud dump <n>` (texture to `swbf-vr-hud-NNN.bmp`, 32 bit with alpha), `hud mode <0|1>`,
`hud compact <spread % or 0> [size %]`, `hud crosshair <0|1>`.

### Holsters (b131-b137, `src/Holster.cpp`)
Always on for a soldier while VR runs (no setting; `holster <mode>` and `holster pose <place> x y z yaw pitch roll` are test commands). Weapon change by the right stick stays in Controls for what holsters do not cover (vehicle seats, the super battle droid).
- Places: right hip, left hip (channel 0 weapons in array order), left chest, right chest (channel 1). Pose per place in Gesture's torso frame (`Gesture::GetTorso`), world = TURN * pose * torso * base frame (`Aim::GetBaseFrame`), worked out in `Holster::OnPresent` (after Gesture).
- Drawing: `Holster::Draw` is called by Aim's first-person hook right after the game's draw, while the hand-placed matrices are in; it calls each model's draw (vtable slot 1) with the model light (GameBuild `ModelLight`). Feature `Holsters` (addresses ModelLight, RequestWeapon).
- Right hand (Gesture zones, grip): grip let go = empty hand (Aim takes FirstPersonRenderable+0x1600 out for the game's draw, Controls blocks primary fire; the weapon hangs in its place). Grip in a place takes its weapon: a primary by wheel notches (next primary) until soldier+0xA50 is the wanted index; a secondary by `Controllable::RequestWeapon`, drawn at the hand's weapon matrix. Letting go of a secondary in its own place puts it back; elsewhere it is thrown: Controls holds secondary fire 150 ms (`Holster::IsThrowing`), and `Aim::SetThrowDirection` replaces the aim direction for 250 ms with the hand's swing (rest-space motion over the last 0.1 s, at least 1 m/s, turned to world). Strength stays the game's (ODF). A secondary's model is back in its place once the weapon is idle with a round in the clip.
- Right grip no longer reloads (KEY_R) while holsters are on; reload is the left-hand gesture.
- Per-item place poses (b466, `Layout::poses`, made in MakeLayout from `_poses`): channel 1 items other than WeaponGrenade 10 cm lower (CHEST_ITEM_DROP_CENTIMETERS); a WeaponLauncher on the right hip at RIGHT_HIP_LAUNCHER_POSE. The health / ammo dispenser drawn at AMMO_MODEL_SCALE.
- b138-b140: only for an `EntitySoldier` (read from the game's RTTI: vtable[-1] -> CompleteObjectLocator +0xC -> TypeDescriptor name at +8; the droideka is `EntityDroideka` and gets none). Secondary kind from the weapon's RTTI type: `WeaponGrenade` / `WeaponDispenser` are thrown (released elsewhere than their place), everything else is a trigger item: while held and selected the right trigger is the secondary fire (Controls), letting go puts it back. A secondary taken is requested by RequestWeapon and, if soldier+0xA54 is not it after 250 ms, the "next secondary" key (G, idren2023's profile) is tapped until it is (run143-run144: one tap needed for the dispenser). Throwables are drawn only when ready (state 0, a round in the clip); trigger items always. A weapon without a first-person model is drawn with its third-person one. Right trigger with an empty hand does nothing.

### Gestures (b77, `src/Gesture.cpp`) and the immersive HUD (b78-b80, `src/Hud.cpp`)
`Gesture::OnPresent` (each presented frame, after `Aim::OnPresent`) takes the head pose (`Stereo::GetHeadPose`), both
controller poses (`Xr::GetHandPose`) and the pad, all in resting-pose space, and fills a `Gesture::Hand` per hand: zone,
`hasEntered` (true for the frame a zone is entered), palm up, grip, pose. The table `ZONES` at the top of the file
holds every zone (frame: head, torso or right hand; centre; radius; leave at 1.3 x radius). The torso frame sits
under the head and turns only when the head has turned more than 40 degrees away from it. Consumers:
`Controls.cpp` (ear menu, reload, zoom: the block marked "gestures" in `Controls::OnPresent`; the left trigger's use is decided when a pull starts and kept until release, `LeftTrigger`: secondary fire, or the magazine hold, where `Aim::SetLeftHandOnMagazine` locks the drawn left hand onto a fixed point under the gun in `ApplyMagazineHold`, after grip and weight) and `Hud.cpp` (wrist
panel while the left palm is up). A new motion control = a zone or a flag here plus a few lines in the consumer
and a line in `_settings`.
HUD layouts in `Hud::Composite`: elements are `STOCK_ELEMENTS` / `_elements` (ammunition, score, player health, target health, map,
crosshair). `placed(index, planeMatrix, ahead, centerX, centerY, width)` draws an element at a size in metres in any frame:
the gun sign uses the right controller's world pose (`Aim::GetHandWorld`), the wrist panel a frame built at the left
controller facing the head (`head` = the point between the eyes, so both eyes see the same panel). The wrist
pieces are drawn with a texture factor for the fade; its plate and frame are untextured triangles and lines.
`Probe` notices large overlays (big map, lists) by reading back a 64x40 copy of the middle of the HUD texture
every 8th HUD and switches to the whole panel while one is up.

### Feedback, two-handed grip, vignette, crouch by head (b83-b85)
`Feedback.cpp` installs two hooks when VR first runs: an assembly stub in front of the weapon fire function
(convention-free: saves all registers, calls `Feedback_FireNotify(ecx)`, jumps on) and a normal hook on the
interface element show function that only acts on calls returning to 0x208B88 (the reticule's hit indicator) and
reads the three hit strengths from the caller's stack frame (through its own saved frame pointer; hence
`-fno-omit-frame-pointer`). Outputs: `Xr::Vibrate` (Xr.cpp; a log line with the simulated headset), `Aim::Kick`
(Aim.cpp: a decaying local transform on the hand matrix inside the first-person render hook only),
`Sight::Flash` (Sight.cpp: colour and width override for 220 ms).
`ApplyTwoHandedGrip` (Aim.cpp) runs right after the controller poses are refreshed (both in `Aim::OnPresent` and
`Aim::BeforeRender`) and rewrites `_trackedPoses[0]`, so aim, model, muzzle and HUD sign all follow. It keeps the
direction to the left hand in the gun's frame from the moment of the grip and applies, each frame, the rotation
that takes that direction to where the left hand is now (row-vector matrix in front of the hand matrix).
`Hud::DrawVignette` (Hud.cpp) is called per eye from `Stereo::OnPresent` before the HUD: a ring of screen-space
triangles, strength from `Controls::CurrentMotion()`.
Crouch by head: `Controls.cpp` compares the head's height in resting-pose space with the soldier's stance and
presses the crouch key; `Stereo::SetLift` raises `_levelCamera` and `_gameCamera` (in `SetTransformHook`) while the
crouch is the mod's.


## Weapon handling (b94, `src/Handling.cpp`)
Pure arithmetic, no game calls. `Aim.cpp` calls it on the fresh controller poses, after `ApplyTwoHandedGrip`, in both places the poses are read (`Aim::BeforeRender`, `Aim::OnPresent`): `Handling::ComputeStock` gives the shouldered weight and the shoulder-to-left-hand direction that `ApplyTwoHandedGrip` blends in; `ApplyWeaponHandling()` then replaces `_trackedPoses[0]` by the lagging weapon pose (`Handling::ApplyWeight`) and re-locks the left hand. Everything downstream (arms, weapon, aim, aiming line) reads `_trackedPoses`, so it all follows the weapon, not the controller. Settings: `weapon_weight`, `virtual_stock`, `scope_reticle`.

## 12. Game builds and feature switches (`src/GameBuild.cpp`, CODE-STYLE section 7, 2026-10-12)

- **One table.** Every engine address (function, global, return address, constant in code) is a
  `GameBuild::Address`. `DEFINITIONS` gives each its features and its signature: a byte pattern of the code around
  it, with `??` over call targets and absolute addresses, and how the address follows from the match (`Code`: in
  the match; `Pointer`: a 32-bit absolute operand in the match; `CallTarget`: target of a call in the match, through
  up to 3 jump thunks; `Relative`: another address + an offset, for ranges and for constants inside a function found
  by a call). `STEAM_ADDRESSES` holds the Steam build's address and signature site per entry. Subsystems call
  `GameBuild::Absolute(Address::X)` (0 when missing) or `Rva`; hot hooks copy what they need into file variables when
  they are installed. Struct offsets (`OFFSET_`) stay `constexpr` in `engine.h` and the subsystems: no signature finds
  them, they are assumed equal across builds, and the code reading them checks pointers first.
- **Known build** (SHA-256 of `Battlefront.exe` in `KNOWN_GAME_BUILDS`): each address is checked, its signature must
  match at the table's site and lead to the table's address (61 compares, no search). Log: `game: Steam, Classic 2004,
  known build` and `all 61 engine addresses checked against the table, all features on`.
- **Unknown build:** each signature is searched in `.text` and must match exactly once; one log line per address
  (`Render at +0x295340 (signature at +0x295340)`). About 0.1 s for all 61 (measured offline on the image dump).
- **Features** (`GameBuild::Feature`), each with the addresses it needs; one address may serve several:

  | Feature | Addresses | Off means |
  |---|---|---|
  | Stereo (core) | Render, SetTransform, SetFov, SetNearFar, ViewCameras, ViewCount, ViewIs3d | VR stays off, game flat |
  | Limits (core) | Flush, the 5 queues, 3 counts, 3 tables, 10 code constants, the 2 record-pool checks | VR stays off, game flat (a wide view without them crashes) |
  | CameraCallers | SceneNearFarReturn, FirstPersonNearFarReturn, SpawnScreenCameraReturn, ChaseCameraUpdate and its range | no near-clip change per pass, HUD always as one flat panel |
  | Aim | AimerSet | the game aims (from the view) |
  | FirstPersonModel | FirstPersonRender, FirstPersonRenderables | arms and weapon stay where the game draws them |
  | Muzzle | HashName, FirstPersonRenderables | no muzzle point (aiming line, fire origin from the game) |
  | AnalogMove | SetStrafe, SetMove and their return addresses | stick movement through W/A/S/D |
  | Feedback | WeaponFire, RegisterHit, HitValue, ReticuleUpdate and its end | no vibration, recoil kick or hit flash |
  | Lua | LuaDoBuffer, LuaRegister, LuaToString, MissionScript, ShellScript | no VR Settings screen, script `lua` fails |
  | VehicleHands | PoseTableInit, PoseTableSet | no arms on the tank levers (levers still work) |
  | ConsoleOutput | ConsolePrintf, ConsolePuts | console text not logged (a console command that prints may crash, as without the mod) |

  A failed address switches off every feature it belongs to, with one line per feature:
  `feature off: shot and hit feedback. Check RegisterHit: <reason>`. Each subsystem checks
  `GameBuild::IsAvailable` before it hooks or calls (the old per-file first-bytes checks are gone; `Limits::Apply`
  still checks each constant's value before rewriting it).
- **Core:** without Stereo or Limits, `Xr::Service` refuses every start request (also `vr sim`) and logs
  `VR is off: ... The game runs flat, as without the mod. Please send GameData\swbf-vr.log with your report.`
- **Threads:** the table is written once in `Identify` on the main thread, before any hook that reads it exists,
  and never changes; nothing lives in the game's heap.
- **Test switches** (swbf-vr.ini, read by `Identify` at start; file-only keys of `Settings`): `test_unknown_build = 1`
  takes the signature path on the Steam build and logs `test: N of 61 addresses found by signature equal the Steam
  table`; `test_break_check = <Address name>` fails that address's check (for example `RegisterHit` switches off
  Feedback only, `Render` keeps VR off). Remove both lines after a test.
- **Changing a signature or adding an address:** add the enum value (`GameBuild.h`), a `DEFINITIONS` line and a
  `STEAM_ADDRESSES` line in the same position, then `python tools/signatures.py` (replays search and check on
  `re/battlefront-image.bin`, must report 0 failures). Patterns were chosen instruction-aligned with wildcards over
  call targets and absolute addresses; vtables (EngineSurvey) are checked by RTTI and stay outside this table.

### Projected scope and the super battle droid (b148-b164, 2026-10-13)
- Zoomed, `Zoom::GetLenses` returns one picture (centre, radius, up to two emitters) in the weapon model's frame:
  lens weapons: the picture floats above the lens toward the eye and the lens(es) emit (bowcaster: both scopes, one
  picture); lensless: a virtual lens at the rifles' eyepiece place with an emitter on the gun; the droid: emitter
  between its barrels (`Aim::GetBarrelEmitter`). `Hud::DrawScopeWindow` draws beam, emitter (a glowing lens or a dot),
  the picture tinted, light around it, scan lines; colours from `HoloColours` (red when `Holster::IsCis`, also the
  wrist hologram).
- The droid (`Holster::IsSuperBattleDroid`, by its weapon model's root node hash): no holsters; `Aim`'s first-person
  hook draws the model a second time mirrored (MIRROR_X) on the left controller, with the weapon's flash time
  (weapon+0xC8) cleared and weapon+0x20 restored for that call; `D3dSurvey`'s SetRenderState hook turns back-face
  culling off in the first-person pass (`Stereo::IsFirstPersonPass`) while it is drawn. Its barrels (arms model,
  forearm bone) give the fire origin and direction, the ammo sign place and the holoscope emitter; its weapon model
  (which draws the flash) is moved onto the barrels (`PlaceOnBarrels`).
- `Xr::SimulateHandPose` glides to a new pose in 150 ms (test hands).


### Ear menu, jet packs, aiming line start (b165-b166, 2026-10-13)
- Ear menu: Controls' ear block calls `EarMenu::Update(open, right ear, stick, choice taken)` every frame; up = `Xr::Recenter` (as Home). `EarMenu::Draw` runs at the end of `Hud::Composite` (not when `isWhole`), inside its state block: a disc of 10 cm radius, 42 cm ahead, 4 cm down and 11 cm toward the ear, facing the eyes, built from the eye camera's rotation and the head position, so it follows the headset; beam lines from the ear zone's centre. Linked with gdi32 (`tools/build.sh`).
- Jet: `Jet::OnPresent(rightStickY)` from Controls (main thread) installs the hooks once VR runs and keeps the player soldier (`EngineSurvey::Resolve("soldier")`); while hovering the right stick's up / down does not change weapons.
- Sight: the first 0.5 m of the aiming line is drawn without the scene's depth (like the first-person weapon), the rest with it. The droid's line starts at `Aim::GetBarrelOpeningWorld` (barrels from the weapon matrix the arms were drawn with), not at the update-time fire origin.
- `SetSoldierAimDirection` (b147) skips the droid: its camera follows soldier+0x33C on some frames (offsets "Super battle droid camera").

### Headset run 13 follow-ups (b175-b177, 2026-10-13/14)
- No write of soldier+0x33C for any soldier (`Aim::SetSoldierAimDirection`, test command `droidtest <write> <mirror>`): the game's first-person camera follows it, and `Stereo::HoldViewHeading` measures the character's heading from that camera.
- Virtual stock: pivot = right controller + `_stockBack` (taken at the grip, turned only by the right controller's heading); no leash.
- Scopes: `Zoom::GetLenses` returns lens weapons' pictures on the glass (`emitterCount` 0, up to 2 for the bowcasters; Hud draws them without the hologram); holoscopes' emitter behind the picture toward the eye.
- Hud: big map / lists / pause menu (`Settings::IsMenuOpen`) as holograms: scan lines, sweep, flicker, tint (`_panelFactor`), lines from `EarMenu::ProjectorPosition`. Big map at `OVERLAY_ONLY_SCALE` 1.2.
- Holster: `WeaponModel` falls back to the third-person model when the class's first-person model has no nodes (`Zoom::HasFirstPersonNodes`). Throws: Aim's fire origin is the hand while `_throwUntilTick` runs; swing over the last 0.05 s.
- Controls: in a jet hover the right stick turns only when more than twice as far across as up / down.

### Work list of 2026-10-14 (b178-b182)
- Aim: the target search's direction getter call (0x1F77E0) redirected to `TargetSearchDirectionHook` (hand direction
  for the player's Controllable while the Aimer hook is fresh); soldier+0x33C is never written.
- HudDraws: `SetReticleHidden` (ReticuleDisplay's bitmaps, found by the RTTI of the object 0xE0 / 0x1E0 before the
  element); `Group::Target` (HealthGroup draws in the reticle's exact red / green; same plane as the health group);
  `hud draws` logs each element's owning display (`OwningDisplay`). Hud draws Target on the gun sign under the ammo.
- HudDraws (b183-b188): `Group::Score` = the RedScreenGroupElement at TeamDisplay+0x40, `Group::Map` = the one at
  MapDisplay+0x30 (both found by RTTI of the object at element - offset; KindOf only follows vtable, locator and
  descriptor inside the exe image). The map's plane is the matrix its group is drawn with (the group turns with the
  player); a fourth hook on `RedMaskedBitmapElement::Render` 0x2CB9E0 catches its background (map texture + map_mask,
  `GroupBatch::d3dMask`, mask UVs in `GroupVertex`), whose square gives the group's bounds (`hasFrame`). Hud draws
  groups with FVF TEX2; a masked batch multiplies alpha by the mask's in stage 1.
- Stereo: `InstallDeathView` points DeathCamera vtable slot 1 at a hook that skips the update while VR runs;
  `GetKillerLine` reads CameraManager (scene camera - 0x260) -> death camera -> killer; Hud draws the line as glow.
  Statistics screen (`Controls::IsStatisticsScreen`) at 4.5 m, tan 0.66, in Composite and in the pointer mapping.
- Holster: throw strength (`SetThrowStrength`, vtable slots patched at the first throw of each class: ordnance class
  slot 2, dispenser weapon slot 46); `DispensedModel` for dispenser / remote weapons' holster models.

### Headset run 14 follow-ups (b206, 2026-10-14)
- Hud: `OnReset` also releases the vignette's state block (a held state block made the Reset fail). `MeasureLitArea`
  (every 8th HUD while a panel is a hologram): largest 4-connected covered piece of a 64x40 copy of the whole HUD
  texture -> `_litArea`, the hologram light's rectangle. The Map group is left untaken while drawn wider than 0.3 of
  the screen (the big map is MapDisplay's too).
- Holster: `ModelByName` hashes with the registry's FNV-1a; `ItemScale` per item kind; `DispensedKind`, `IsTimeBomb`.
  Throw strength: full at 10 m/s, underhand 12 m/s.
- Squad: sweep across = At Ease (circle removed), `Squad::IsArmed` keeps Controls' ear menu shut.
- Controls: the right trigger clicks while `Pointer::IsActive`, also with an empty hand.
- Body: full body = "arms 3" (first-person arms drawn; the body's forearm and hand matrices collapsed onto the elbow)
  and "spine 1" (`BendSpine`: the upper-body bones turned about bone_a_spine toward the neck target, which
  `ComputePlacement` puts in the head's own frame; the feet follow an anchor in the base frame beyond 25 cm).


### Headset run 15 follow-ups (b207-b209, 2026-10-17)
- Controls: scope zone = the lens (`Zoom::GetLensCentre`, any weapon in LENS_MODELS) within 22 cm (leave 28) of the
  point between the eyes (`Stereo::GetLastHeadWorld`), ahead of them, gun within 60 deg of the view; lensless weapons
  zoom only by the stick. A zoom the game ends while the same weapon is held is given back once
  `Holster::IsWeaponReady` (6 s at most). Ear menu: either hand (left wins; the right hand uses the right stick, which
  then neither turns nor changes weapons); a choice = stick past 0.85, back inside 0.3 within 150 ms. Head crouch is
  edge-triggered: the game's own stance changes win (prone stays ours, a jump to standing ends ours).
- Aim: `BARREL_CORRECTIONS` (pistols): the weapon model is turned on the hand (`AlignBarrel`, about the model origin)
  so its barrel lies along the controller; aim stays the controller's. `GetWeapon`, `GetWeaponClass`,
  `GetFirstPersonElbowWorld` (forearm bones' origins as drawn). Zoomed, the recoil kick only tilts (30 %), no move.
- Holster: places kept relative to the base frame and multiplied by Aim's base frame at the draw (no one-frame lag);
  dispensers' release delay skipped like the grenade's.
- Squad: two gesture sets (`squad_gestures` directions | immersive); `ShowSquadDisplay` calls the game's
  SquadCommandGroup::Show each frame while armed.
- Stereo: glass-lens scope pictures only for the eye nearer the lens axis. Sight: the line stays on zoomed, not in the
  scope pass. Hud: hologram pieces (`_litPieces`) with outlines and tracing beams, a bright source at the ear projector.
- Body: with the head hidden, unnamed bones within 30 cm of the head joint (above the neck) collapse too; "arms 3"
  points and stretches (0.7-1.5) the upper arm to the first-person forearm's elbow end.

### Headset run 16 follow-ups (b264-b267, 2026-10-18; PLAN 57)
- Controls: the scope check runs on foot only (b264: in a vehicle seat `Aim::GetWeaponClass` follows the soldier chain
  into garbage; `Zoom::ModelNodeHashes` also guards the class read). Crouch is pressed with `_crouchKey`, Right Ctrl by
  default (ini `crouch_key`, hex DIK code); head crouch stands a crouch up with jump (the crouch key cycles stand ->
  crouch -> prone -> stand), prone with the crouch key; a crouch press the game misses is repeated (3 at most).
  Droideka modes 11-13 (rolling up, ball, standing up): the view is not held (`SetViewFree(false)`: it takes the ball's
  heading), head yaw / pitch follow off, the left stick's sideways push becomes turn degrees (150 deg/s) sent as mouse
  counts with the right stick's turn; the stick's forward part still rolls.
- Aim: `GetWeaponWorld` returns the last placed weapon moved into the current base frame (W x B_then^-1 x B_now with
  the base frame kept at placement): the scope pass runs before this frame's first-person draw, so the lens lagged a
  frame behind the head when moving or turning and the scope field (lens radius over distance) pumped. Stereo logs
  `scope: lens half tan A to B` every 2 s while zoomed on a lens. `BARREL_CORRECTIONS` has the Imperial pistol.
- Hud: the pause hologram's per-item outlines and tracing beams are gone (source and corner beams stay).

### Headset run 16 follow-ups 2 (b275-b281, 2026-10-18; PLAN 57 f, g, h, b)
- Handlebars: the throttle is the lean by default: ahead = the neck's forward travel (resting-pose z) since the first
  grip of a ride minus the held hands' forward travel since each one's grab; 3-18 cm ahead = forward 0-1, 8-20 cm the
  other way = reverse. `bike_throttle` (Settings MOVEMENT) twist brings back the right grip's twist. STAP eye z -0.82.
- Seat aiming lines: `Vehicle::GetSeatFireLines` (turret seats: MountedTurret T+0x290 slots; droideka: E+0x4C8; each
  slot's Aimer and those linked by +0x18 with the same owner +0x20; checked with VirtualQuery and RTTI "Aimer", looked
  up again once a second) returns each Aimer's fire point +0x7C and direction +0x48. Sight draws one ribbon per line
  (up to 4) in turret seats and as a droideka, and `Sight::IsReplacingReticle` makes HudDraws leave the game's reticle
  out. `vehicle trace <ms>` logs head, seat aim, body and Aimer headings every 50 ms.
- GunnerSeat `ShowStockGun` (stations with `showsStockGun`: the AAT; b282-b284, PLAN 57 h2 / h3): in the hull's draw
  Main_gun and recoil_3 get matrices of ours, restored by AfterHullDraw: the breech where it lies relative to the eye at
  pitch 0, turned about the eye with the visor; breech and barrel pointing from there at the game's target point
  (`PointAt`). The visor part (`isVisorOnTarget`) points from the eye at that point too (`VisorRotation`), so barrel and
  reticle stay fixed in the window on where the shots land; the shots and the aiming line stay the game's (real muzzle;
  idren2023's rule: gameplay the same in single and multiplayer). The AAT cabin and its eye sit 0.70 m higher
  (AAT_GUNNER_EYE y 4.45) so the barrel is seen under the aim. Fighter tank gunner eye y 2.55.
- Vehicle `ParallaxAim` (b283, all turret seats): the game aims a seat's guns at the point its own ray hits, from
  Controllable+4 (the game's camera) along +0xEC, stored at +0x10; StepTurret aims the turret from +4 at the point on
  the head's line of sight as far from the head as +0x10 (3 m to 12 m guards), so the game's ray, and the shots, pass
  where the head looks. `GetSeatTarget` gives +0x10. `vehicle trace` logs both distances.
- Vehicle `DroidekaAimHook` (GameBuild feature DroidekaAim, 0xDFC90, bool thiscall, one stack argument): after the
  game's aim, the player's droideka's arms' target point E+0x260 is put on the head's line of sight at the game's own
  hit distance; the arms turn there within their +-40 deg while the body follows at the game's ~35 deg/s. The hook must
  return the game's bool (b279-b280 did not: the droideka's aim stopped, then crashed).
- Speeder gunner's turning seat (b285, PLAN 57 i): the cabin design has moving parts (ShellDesigns
  SPEEDER_CABIN_PARTS); GunnerSeat draws it with `CockpitShell::DrawParts` (cabin; seat turned about the gunner's eye
  by the turret's heading in the cabin frame; the stop lamps only at a stop, from Vehicle `GetTurretYaw` against the
  limits). A null part matrix leaves a part out; `CockpitShell::Draw` (pilot seat) draws parts below
  `Shell::restPartCount` at rest and leaves the others out. Its periscope (b287): `GunnerSeat::GetPeriscope` gives a
  muzzle camera and a screen frame; Stereo's scope pass renders that camera whenever it is there (before the zoom
  checks); since b288 the shell draws the picture itself (finish texture 6 = `Hud::ScopePicture`, behind the stock
  screen's glass, texture 5 = the design's `screenName`), and `DrawScope` draws nothing for it. In the periscope pass
  (b289-b291) HudDraws draws no HUD (and leaves the game's reticle out in every pass while it shows) and Sight draws
  the stock reticle on the first gun's line. Shell displays may sit on a moving part (Display `part`).

### Headset run 17 follow-ups (b341, 2026-10-20; PLAN 58 a-d)
- Fighter tank gunner station: the view keeps its own heading as on foot (Controls `SetViewFree` when
  `GunnerSeat::IsStationSeat`, Stereo `HoldViewHeading`; the right stick's turns go to `TurnView`, the character
  follow is off there). `HoldHullHeading` then only puts the eye at the station's. The turret keeps following the head
  in the world (Vehicle StepTurret), so the hull turns under the gunner.
- Speeder gunner periscope: `Cabin::isPeriscopeByHead`: the display's pitch is the head's over the cabin's level,
  clamped to ShellDesigns `SPEEDER_PERISCOPE_LOW / HIGH_DEGREES`, and its camera looks along the display's forward
  (GunnerSeat TurnCabinSeat, GetPeriscope); the AAT's still follows its gun.
- Turrets aimed by the hand (b371-b373, idren2023 2026-10-21): `BuildingTurret::isAimedByHand` (Hoth dish, Geonosian
  turret) -> `GunnerSeat::IsAimedByHand`. Vehicle `StepTurretByHand` replaces the head as StepTurret's input: the
  right controller's (Aim::GetHandWorld) pitch over the mount's level, and a steered yaw that moves while the
  controller's yaw is off the gun's (joystick: HAND_YAW_*), held at most HAND_YAW_MOST_LEAD_RADIANS ahead of the gun and
  stopped in the dead zone. Stereo `HoldHullHeading` takes the gun's heading (`Vehicle::GetTurretForward`) for the view
  there, so the stock screen on the turning part stays ahead; the controller's offset is then the same against the view.

### Headset run 17 follow-ups 2 (b342, 2026-10-20; PLAN 58 e-p)
- Droideka: Controls `IsDroidekaStanding` (mode, once a Present) gates Sight's droideka lines (the first two seat
  fire lines only); Hud draws the classic layout as a droideka, the panel times DROIDEKA_HUD_FACTOR over
  `DrawDroidekaVision` (a screen-space multiply and scan lines in the eye image, under the HUD). The ball's turn has a
  dead zone, a square curve and the ball's own count gain (DROIDEKA_BALL_COUNT_GAIN).
- Stance by head height (Controls, `crouch = head`): the head's stance (stand / crouch / prone by drop, hysteresis) is
  driven to step by step when it changes; the game's own changes stand until the next change.
- Respawn: Stereo keeps the last play camera matrix (`_lastPlayView`) and hands it to the spawn screen's caller
  (0x13EED5); cleared while no scene camera exists.
- Hologram: MeasureLitArea also traces the lit piece's outline (`_litOutline`); the projector's beams go there.
- Left trigger on foot: only the reload under the gun (LeftTrigger::Spent elsewhere); secondary fire in vehicles and
  as the droideka.

### Headset run 18 follow-ups (b447-b449, 2026-10-31; PLAN 59)
- Levers: one drive stick (right handle's place) tilting ahead / aside -> move / strafe (dead zone 3, full 25 deg).
  `Levers::IsShown` makes the tank pilot a station seat for Controls (view held, no character follow) and lets
  Vehicle keep the head-heading yaw input for the hover (SetYawHook), so the hull turns under the view to the head.
- Flight: GetDrive gives only the throttle; GetStickLook's shares go to Controls, which sends them as mouse counts
  (FLIGHT_STICK_DEGREES_PER_SECOND; a flyer's vertical is the soldier's reversed); the right stick's X presses A / D in
  a flyer. All three handles drawn (LeverModel's old `lever < 2` limit gone). Stick rumble by tilt.
- Stereo::GetSmoothedHeadWorld: the head's turn relative to the level camera followed with a 0.1 s lag (the droid's HUD
  on the head: CockpitShell HEAD_PART displays, ControllerHolo, which also scales its layout by HUD_DEPTH_SCALE 1.6).
- Spawn screen: Stereo latches `_spawnPanelFrame` (the head's level heading) when the spawn screen comes up; the HUD
  panel and the pointer use it while it lasts.
- Aim: `_fireStartInWeapon` (muzzle in the weapon's frame) -> GetDrawnFireLine for Sight (the line on the drawn gun);
  ForegripInPose for the two-handed grip (fixed foregrip, the hand model locked on it). The arms copy for vehicles is
  dropped only while no scene is shown (not at each spawn).
- Holster: IsHeldBack holds back the game's own reload for every clip weapon of the player's (layout weapons), the one
  in hand until the canister goes in.
- Xr: optional actions trigger_touch / thumb_touch (Touch, Index), bound last with a fallback; ControllerInput hasTouch,
  isTriggerTouched, isThumbTouched. BodyArm::ControllerPose picks fist / point / thumbs up / grip / trigger poses.
- GunnerSeat: Periscope::fieldFactor (cabin periscopeFieldFactor: bomber 1.8, Y-wing 1.4); building turrets first person
  with hysteresis (6 m in, 11 m out). Cabin "all_fly_ywing": YwingGunnerStation (SPEEDER_CABIN_PARTS parts); since b478 one tandem cockpit: the pilot's tub runs back to YWG_REAR with a low divider (YWG_DIVIDER_TOP) between the seats, under the stock canopy whose glass covers both.
- ShellDesigns: YWG_EYE (Y-wing pilot 12 cm higher); stockexterior.py CUT_BOXES (Jedi dash cowl).

### Per-unit arms (b450-b453, PLAN 60)
- BodyArm UNIT_SKINS: arms model name -> glove and sleeve texture names; CurrentUnit() looks each name up in the model
  registry and compares with FirstPersonRenderable +0x15FC (cached by pointer); SkinTexture() swaps the exported
  trooper textures per part. IsWanted / IsEmptyHandWanted / IsLeftWanted need a known unit (else the stock arms).
- Armoured forearms: UNIT_SKINS.forearm names a part forearm_<unit> (tools/body_armour.py, exported from
  re/body/r_arm_armour.blend); IsPartShown draws it instead of r_forearm_*; it takes the unit's arms texture;
  UpdateShownParts before each skinning skips the hidden parts.
- Grips: GRIP_POSES adds "launcher". tools/body_gripbox.py finds a grip box; body_weapons.py / body_export.py take
  `side:name` weapons (object weap_<side>_<name>). b456-b457: grips named <side>_<name> (imp_rifle, imp_pistol,
  imp_sniperrifle, rep_rifle, rep_pistol) match <side>_1st_weap_inf_<name>; FindGrip tries the root node's name plain,
  with _mesh, with _model (clone pistol) and as <side>_1st__weap_inf_<name> (clone rifle). Their grip axis point and
  direction are the Rebel rifle's / pistol's (re/body/grip-weap_*.json `measured_axis_*` keeps the measured ones):
  the axis sets where and how the hand sits on the controller (ArmToGrip, Aim's GetGripPointInModel), and the hand is
  the same for every weapon, so only the gun differs. Export source now re/body/r_arm_grips.blend (r_arm_armour.blend +
  its weapons; b458 adds the snipers, fusion cutters, Imperial launcher and thermal; a thermal of any side counts as thrown).

- Item grips (b467-b469): items Holster draws in the hand at their item size (ItemScale) have grips named com_ / item_
  (tools/body_hold.py; body_weapons `item_<name>=<msh>@<scale>`, body_export divides the scale out). Holster's Layout
  keeps each place's grip name (ItemGrip: grenade -> com_grenadethermal, landmine / time bomb / dispenser / recon droid
  -> item_*); HeldItemGrip(class) gives it while that secondary is held. FindGrip never matches com_ / item_ grips by
  model name; it takes an item_ grip by Holster's name, the grenade's by its node, and a grenade whose model matched a
  full-size thermal grip takes item_<that grip> (not cached). Aim draws our arm round a held secondary (armClass =
  Holster::HeldSecondaryClass: the game's weapon stays the primary until the secondary fires), also for the grip point;
  thrown items keep the fitted wrist and ignore the trigger.
- Wookiee (b470-b471, PLAN 60 step 3): UNIT_SKINS all_1st_wookie. Its stock hands are the trooper glove mesh with
  their own uvs in a quarter of all_1st_inf_wookie.tga: tools/body_unithands.py bakes them into our glove's layout
  (all_1st_inf_wookie_hands, a texture of ours: OwnTexture finds any BODY_ARM_TEXTURES name, not only game textures).
  body_armour.py also takes `_r_arm` (the fur forearm) and builds hair_<unit> from the hair skirts under
  bone_r_forearm (the msh names both arms' skirts l_hair): IsPartShown draws hair_x with forearm_x, alpha-tested
  (HAIR_ALPHA_REFERENCE) in our RGBA copy of the texture (all_1st_inf_wookie_hair; run539's solid skirts were upside-down
  uvs, fixed b473: Blender part uvs keep msh v as is, it runs up the image as Blender's; body_export writes 1 - v). Grips bowcaster and com_torpedo (the mortar launcher's
  com_1st_weap_inf_torpedo); IsItemGrip is now item_ and com_grenadethermal only. b472-b473: fur cards on the back of the
  hand (3) and each finger's base bone (tools/body_furcards.py, part hair_<unit>_hand, each card on one bone); IsPartShown takes hair_x and
  hair_x_hand with forearm_x.

- Per-unit skeletons (b479, PLAN 60 step 4a): BODY_ARM_BONES holds every skeleton, the human first and unchanged;
  BODY_ARM_SKELETONS gives each one's range (an .inc without it: one, the human). UNIT_SKINS.skeleton names a unit's
  (null: human); CurrentSkeleton() -1 when the unit or its skeleton is missing (stock arms). Parts and grips carry
  their skeleton (trailing field, 0 omitted); IsPartShown / FindGrip take only the player's skeleton's. Every bone is
  posed each draw (pose angles only touch their own skeleton's bones). Poses of a non-human skeleton are labelled
  "<name> <group>: <label>" (body_poses group "b1 fitted"), found by FindSkeletonPose with the human label, so
  GRIP_POSES, the controller poses and the canister pose need no second table. Each skeleton's root is "forearm" with
  "hand" its child (the wrist-straightening swing); trigger blending takes bones named index* or trigger*. BoneCount
  (PlayerBody) is the human skeleton's. body_export.py: armatures with a `skeleton` property after the human rig; a
  part or weap_ grip belongs to the armature it is parented to; its C names are the whole part name.
- The B1's hand in the game (b480-b481, PLAN 60 step 4d): UNIT_SKINS cis_1st_bdroid (the assault droid, droid pilot and
  assassin droid wear it; on NAB1c the CIS's first class is the super battle droid) with skeleton "b1", parts b1_hand
  (30k tris) and b1_arm (9k), texture cis_1st_bdroid_hands as it is. re/body/r_arm_grips.blend is built by
  `blender -b --factory-startup re/body/r_arm_grips-b479.blend -P tools/body_b1.py -- re/body/r_arm_grips.blend
  stock=1`: the CIS weapons at the stock hp_weapons, then each moved by re/body/b1-stock-tweaks.json (pivot / turn /
  align / move steps, an `after` chain; idren2023's approved placements), the grenade and the mine replaced by the model
  and size Holster draws (DRAWN: grenade 0.8, mine com_weap_inf_landmine_placed 0.6; put on the posed one's centre and
  flat-side normal, then the tweak's `drawn` steps). Poses: body_poses.py 'b1 fitted' (grip labels; CIS grips
  "fitted: E-5 rifle (stock hold)" etc. in GRIP_POSES, common items reuse the human labels) and 'b1 controller' (the
  human labels, so ControllerPose picks them). body_export.py also writes meshes parented to a skeleton armature with a
  `texture` property. FindGrip also tries <side>_1st_weap_<name> (cis_1st_weap_launcher has no "inf_"), and an item with
  no grip by its model falls back to the grip Holster names on the player's skeleton (b483: the common grenade's root
  node is imp_1st_weap_inf_thermaldetonator, whose grips only the human hand has). Geometry (b482, idren2023: a Quest
  build later, no wasted triangles): flat bars bevelled once (`segments=1`) and not subdivided (`levels=0`), the joint
  cylinders and the rod subdivided once (`round=1`), the wrist ball 10 x 20 unsubdivided: hand 2 518, rod 2 306
  triangles (were 30 008 and 9 224). `rawmesh=1` builds the stock pieces as they are (comparison renders). The B1 has no
  trigger-pulled pose (no "b1 primary: ..." label: its trigger finger does not curl on fire).

- Our models in git (2026-11-04): tools/data/models/<set>/ holds every exported object of re/body/r_arm_grips.blend
  (set arm) and re/body/player_stock.blend (set player) as one json each (tools/body_model_io.py: points, faces, uvs
  per face corner, attributes, weights, materials by image path; armatures by bone rest frames and pose; a weap_ object
  by its msh path and the affine msh -> object map). tools/body_save.py writes them, tools/body_rebuild.py builds the
  blends back, tools/body_textures.sh makes our textures from the game's, tools/body_bake.py the player body's baked
  textures from a recipe (player/bake.json: elbow strips' uv rings, glove uvs; written by body_assemble.py),
  tools/body_rebuild_all.sh runs everything (checked from a fresh clone).
- The super battle droid's arms in the game (b484-b492, PLAN 60 step 4e): tools/body_sbd.py builds them (forearm
  plate = re/body/sbd_plate.blend: idren2023's re/body/plate-source/sbd_plate_edit.blend smoothed by tools/sbd_plate_rebuild.py
  (b493: the limit surface of his mesh as a subdivision cage, his sharp edges as creases; crease points resampled along
  the smooth crease curves, the others relaxed onto the surface, all renumbered; check= fails below 1.5 mm from a
  comparison point), our base strips, capsule, guns from the stock cylinders, the
  hand machined); `export=1` writes them into re/body/r_arm_grips.blend (skeleton "sbd": sbd_arm, sbd_hand, sbd_arm_l =
  the right arm without guns, unmirrored) and the grip weap_sbd_wristblaster (property `direct`: body_export.py takes
  the object's matrix, no msh fit) that puts cis_weap_inf_sbdroidmuzzle's hp_fire 25 mm (3.5 cm at game scale) ahead of
  our barrels' muzzles, its +z along them. hp_fire is taken relative to the model's root node: the game draws a model
  from its root's frame (Aim's GetMuzzleInWeaponFrame), not from the msh's world coordinates (run557-558). Texture: ours,
  the stock 128 px upscaled 4x (re/body/hd/..-ultrasharp-4x.png, BODY_ARM_TEXTURES); uvs per piece (texture_arm,
  texture_hand, unwrap_plate with idren2023's pinned map tools/data/sbd_plate_uvmap.json; `shade=distort` heat map).
  BodyArm: UNIT_SKINS cis_1st_sbdroid skeleton "sbd"; FOREARM_WEAPON_GRIP "cis_wristblaster" is every weapon's grip
  for a skeleton that has it (the tri-shot and rocket have no model); its hand takes ControllerPose (no grip pose, no
  wrist swing); parts named *_l are drawn only for the left hand (_isDrawingLeft), the rest only for the right.
  DrawLeftForearmWeapon: the left arm from Aim's mirrored weapon placement (the left controller's aim pose, grip point
  on its grip pose, MIRROR_X), not ArmToGripWorld (the human rule put it upside down, run556). Aim: our arm drawn for
  the droid even with the hand empty; barrels in the weapon frame from hp_fire (opening 3.5 cm back along +z, emitter
  0.135 back, 0.017 up); fire origin from the weapon as last placed (GetWeaponWorldWithin 500 ms,
  BARREL_LIFETIME_MILLISECONDS); IsBarrelFresh with a signed tick age; the stock mirror off. Holster: the droid flag is
  set once per OnPresent (no false moment for the render thread: the ammo sign alternated, run562); ready = right grip
  held (Holding::Primary), lowered = released (HoldsNothing: no HUD, no line; Controls: no fire, next primary on the
  release); no manual reload, no reload-under-the-gun gesture, no two-handed grip for the droid; the left trigger fires
  the rocket (secondary) while ready. Poses: body_poses 'sbd controller' (grip_1 + opens the slab, grip_2 + its tip,
  thumb_1 x + the thumb out).

### Building turrets (b356, PLAN 58 u)
- Grips grabbed (b460, idren2023 2026-10-31: a real hand grabbing each handle): tools/body_grab.py places every weapon and
  writes its pose by the bones (not the glove mesh): the hand opened (fingers straight, thumb out); the grip's axis laid
  on the line from the web (between the index knuckle and the thumb's base joint) to the heel of the palm (25, 35 mm),
  its palm face on the palm's plane (z -24 mm), the frame 26 mm above the web; the weapon slid along its grip until its
  trigger is level with the index knuckle (a guard's opening found in the side view, else just in front of the grip's
  top); each finger the most wrapped pose (knuckle, middle joint, end joint k x middle) whose bone capsules (each
  bone's own thickness from the mesh) stay off the weapon's surface (1.5 mm press), out of its inside (ray parity), off
  the palm and the finger before; the index to the trigger; the thumb the other way round. The grips turn 13-33 deg in
  the hand (a real wrist cocks on a pistol grip); axis_point / axis_direction (the controller in the hand) are turned
  with the weapon, so each weapon sits on the controller exactly as before and only the hand turns on it. Settings per
  weapon: re/body/grasp/final.txt; tools/body_grasp.py is the earlier mesh-crossing variant. The Imperial and clone
  launchers are exported now (GRIP_POSES imp_launcher, rep_launcher).
- Grips with the web at the grip's top (b461, idren2023 2026-11-01: b460 put the web 16-38 mm down the grip): the web
  point is now fixed at (-36, 69, palm) mm, where the accepted b456 Rebel pistol and rifle had the backstrap's top;
  the grip's top (frame row) sits on it (top 0), the heel on the little finger's side (40, 42 mm), no slide to the
  trigger (the index bends to it; `slide` brings it back); palm -20 / -22 (-30 clone launcher). An upright grip (E-11,
  Imperial pistol, snipers, Imperial launcher) gets `turn` so its barrel lies along the hand's y like the Rebel guns'
  (long axis x 0.05-0.15); `measure` prints a placed grip's top / back / face relative to the web. Settings:
  re/body/grasp/final.txt (pistol, rifle, E-11) and final2.txt (the other 12).
- Vehicle FindStockVehicle: a turret seat's building has no class at the vehicles' offsets; the building is searched
  (first 0x800 bytes) for a pointer to a class holding a STOCK_TURRETS name hash: CurrentStockName is that ODF.
- GunnerSeat BuildingTurret (table BUILDING_TURRETS): DrawTurret draws the building's own model at the hull matrix
  with a pose table of ours (rest transforms from the msh, the yaw / pitch about their pivots from
  Vehicle::GetTurretAngles) in place of the game's first-person model; GetSeatEye gives the eye on the turning part.
- Grips (b357): BuildingTurret.grip / gripLeanDegrees on the pitching part; DrawBuildingTurret calls PlaceGrips with
  design space -> model (diag(-1, 1, -1)) x pitch delta x building, and counts as shown (IsShown), so StepGrip,
  GetHandPose and Aim's DrawArmsOnLevers work as in the fighter tanks' station.
- Other turrets (b359-b362): BuildingTurret.forwardZ (-1: the gun along the model's -z, Mos Eisley's barge gun:
  model-space pitch reversed, GunForwardZ turns Vehicle's head aim and Stereo's view heading); hasGrips /
  gripCentreX; screenDesign / isScreenOnGun / screen / screenHalfWidth / camera: a viewscreen, GetPeriscope ->
  GetBuildingPeriscope (screen frame from the turret frames of this frame, camera LookAlong the gun), the picture
  drawn by CockpitShell::DrawParts with ShellDesigns' `<odf>_screen` (one quad, finish texture 6, band 0.35-0.65);
  isEyeOnGun: GetSeatEye turns the eye with the pitch too (Bespin AAA). The screen design is drawn with two parts
  (0: the screen's part, 1: the gun), so a whole cabin can be one (the Hoth tower's, isGripOnTurning: grips on the
  head, not the gun).
- ShellKit Finish (b367): `textureTexels` (the texture's width; default 512) and `isDense` (the mesh's one density on
  any texture, not only slots 1 and 3): a design can mix textures of any size at one texel density. Vehicle's named enter: a building's class at
  +0x4.
- Aim `ConnectForearm` (b358): for a hand holding a GunnerSeat grip, the forearm bone is turned about the wrist by the
  least rotation from its drawn line onto elbow -> wrist, the elbow solved for a two-bone arm from a shoulder below the
  head (Stereo head, level heading; constants SHOULDER_* and UPPER_ARM_METERS). ArmsCopy keeps rightForearmBone.

### Manual reload (b355, PLAN 58 s)
- Holster StepAmmo (main thread, after UpdateHand): the canister's states (hidden, on the belt, in the left hand,
  put in, reloading) from the held primary's RoundsPerClip and spare clips; ClipEmptyHook on weapon slot 44 (PatchSlot)
  blocks the game's own reload for the player's weapon; the insert asks Controls::RequestReload (R at its next
  OnPresent) and waits for the game's state 5. Drawn in Holster::Draw (belt pose with the places; the left hand's
  palm). Gesture Zone::BeltLeft, left hand only. Setting manual_reload; the b118 reload gesture is off with it.

### Grab and spawn (b354, PLAN 58 q, r)
- Holster::IsSwapPending (render thread): a primary taken from a place while the first-person model's weapon object
  (FirstPersonRenderable+0x1600, engine.h) is still another: Aim's DrawGameFirstPerson leaves the game's weapon out,
  Holster::Draw draws the taken one at the hand's weapon matrix and the old one in its holster.
- UpdateHand: a new soldier starts Holding::Empty unless the grip is held (holsters on, hand tracked).

### Thrown items (b353, PLAN 58 k)
- Aim never takes a resting pose for WeaponGrenade / WeaponDispenser (the after-throw pose was being learned); they
  borrow another weapon's. Holster scales only grenades by the swing (ordnance create hook); dispensers (mines, time
  bombs, health / ammo) throw at the game's own strength.

### Lock-on display (b343, 2026-10-20; `src/LockOn.cpp`)
- In the lens's scope pass (Stereo's SetNearFar hook at the first-person pass, scope pass, not the periscope):
  LockOn::DrawInScope reads the homing launcher's lock (class +0x239, LockTime > 0; weapon +0xB8 / +0xBC, target
  handle +0x110) and draws, depth off, ribbons facing the scope camera: the cone's ring round the search ray
  (Controllable+4, and the hand's direction from Aim::GetTargetSearchDirection, as the game's search uses), the
  closing brackets and the locked diamond. LockOn::ScopeHalfTan widens a homing launcher's scope picture to 1.3 x its
  widest cone (Stereo passes the matching factor to RenderScopePass). Script `lockon test 1`. b348: Hud scales the
  scope reticle's alpha by LockOn::ReticleOpacity (fades with the lock, 0 locked); Feedback_FireNotify calls
  LockOn::OnShot (the display starts again).

### Droideka (b211, 2026-10-17)
- `Aim::IsDroideka` (player = EntityDroideka, checked per presented frame): no hand aim in AimerSetHook, the
  first-person model (its guns) drawn as the game places it, no Aimer looked up.
- Controls: the character-follow loop takes the head's heading (`Stereo::GetHeadYaw`) instead of the left
  controller's; pitch by vertical mouse counts toward `Stereo::GetHeadPitch`, closed loop on `Stereo::GetGamePitch`
  (the game camera's pitch before LevelCamera). The game then aims the guns itself (within PitchLimits -25..25).
  A presses E (the game's enter / exit control = roll toggle) instead of Space; no crouch by head height.

### Voice squad commands (b212, 2026-10-17)
- `src/Voice.cpp`, its own thread (COM multithreaded): SAPI in-process recognizer, English (US) engine, input = the
  SAPI audio input whose name contains `voice_device`, else "steam streaming", else Windows' default; a grammar of the
  four phrases (`AddWordTransition`). Loudness = the capture endpoint's peak meter (`IAudioMeterInformation`, matched
  by the SAPI name being the start of the endpoint's name), highest since the recognizer's SOUND_START. A result
  passes if `SREngineConfidence` >= `voice_confidence` % and the peak >= `voice_loudness` dB; then
  `Squad::Request` (main thread gives it). Settings rows in FEEDBACK (`voice_commands` off by default: it opens the
  microphone). Test: script command `voice file <wav>` (no loudness check), then back to the microphone if on.

### Vehicles (b213-b214, 2026-10-17, PLAN 54)
- `src/Vehicle.cpp`: the seat from the RTTI type of the Controllable PlayerController feeds (EntitySoldier /
  EntityDroideka on foot; EntityHover, EntityCommandHover; EntityFlyer, EntityCarrier; EntityWalker,
  EntityCommandWalker; MountedTurret; PassengerSlot), so modded vehicles of those classes are covered. Hooks on
  Controllable::SetYaw / SetPitch (PlayerController's calls only): in a seat with a gun (turret, walker, hover pitch
  only) the look rates become error / (0.15 s * gain), error = head forward vs the seat's aim Controllable+0xEC, the gain
  (aim radians per second per input unit) measured while the gun moves, sign fixed (+ = right / up).
- Stereo `HoldHullHeading`: in a seat the level camera's heading is the hull's (vehicle entity +0x1B0 row 2; a
  turret's parent), not the game camera's, which follows the aim in first person.
- Cockpit model (b217-b220, Aim `DrawVehicleFirstPerson`): in FirstPersonRenderable states 1-3 the hook places no
  hands. In a pilot seat the model (+0x1570, +0x15B0) is drawn x inverse(game camera, Stereo::GetGameCamera) x hull
  camera at the game camera's position; hull camera = the hull's level heading for hovers and walkers (the game's own
  camera is level at zero aim), its full matrix for flyers (not yet run). Gunner seats keep the game's placement.
  b273 (PLAN 57 e): a stock vehicle with a cockpit shell seats the pilot at the shell's own eye
  (`ShellDesigns::PilotEye`, cockpit model space; tanks (0, 1.5, 0.7), MTT and combat speeder (0, 1.5, 1.1) against the
  game's (0, 1.5, 1.5)): the model's matrix (and the weapon matrix) is first moved by (game eye - pilot eye) along the
  model's own axes, so the pilot's eye lands at the game camera; the shell is drawn with the moved matrix.
- Drive per vehicle (b225, idren2023 2026-10-17): sticks by default (modded vehicles, bikes, animal mounts); stock
  vehicles may override it in `STOCK_VEHICLES` at the top of Vehicle.cpp (ODF name -> Drive; now levers for
  cis_hover_aat, cis_hover_mtt, rep / imp_hover_fightertank, all_hover_combatspeeder). At a pilot-seat change the class object (hover E+0x500,
  flyer +0x534, walker +0x4E0) is searched for each table name's hash (HashName and the FNV-1a | 0x20 hash; run228:
  FNV at class +0x18). `Vehicle::CurrentDrive()`.
- Seat change in vehicles: right stick click = mouse wheel up (Controls, b225); the right grip and right stick down
  no longer change seats in a vehicle; the stick click no longer zooms there. Fire in vehicles (b230): right trigger =
  primary fire whatever the holster's hand state, left trigger = secondary fire (the reload gesture is off there).
- Tank handles (b221-b227 levers; slide handles b273, PLAN 57 d, `src/Levers.cpp`): with Drive::Levers two handles
  stand in resting-pose space, i.e. on the hull's level heading (LeverPlacement: pivot = carriage top 20 cm to each
  side, 45 cm under and 40 cm ahead of the resting eye; the grip leans 10 deg ahead, the hand closes 10 cm up it: palm
  at 35 cm under, 42 ahead). A new grip press with the hand's fist (Aim::GetPalmInHand in the hand pose) within 20 cm
  of a grip takes it; the handle then follows the fist's motion along the hull's forward axis (relative to where it
  was taken) between three notches 10 cm apart (reverse -1, neutral 0, forward 1): it moves to the next notch past the
  half way point + 1.5 cm (back only 1.5 cm short of it), the carriage follows the hand 1.2 cm either side of its
  notch, a pulse marks each notch; only the notch is the input. Let go, the carriage settles into its notch and stays
  (reset to neutral when the seat is entered or left). The hand is drawn closed round the grip: the grip's frame at
  the grip point minus the palm offset (Levers::GetHandPose, as GunnerSeat). Strafe (b227): with both handles held,
  both hands rolled the same way past 25 deg (roll = atan2(-right.y, up.y) of the controller, right positive) give full
  strafe (+0x58 = +-1) until either comes back under 20 deg; the grips lean 12 deg that way, a pulse marks the start.
  Vehicle's SetPitch hook adds move = (R + L) / 2 to +0x54, the strafe to +0x58 and turn = (L - R) / 2 to +0x5C after
  PlayerController set them (sticks keep working; no head yaw, idren2023 2026-10-18). Drawn from Aim's vehicle
  first-person draw after the cockpit and its shell by LeverModel (carriage frame with the hull's axes, grip frame
  leaning; fixed function, state block).
- Starfighters (b402-b410, PLAN starfighters, idren2023 2026-10-27): every flyer's pilot seat gets Drive::FlightStick
  (`src/Flight.cpp`; the five single-pilot stock fighters also in `STOCK_VEHICLES` for their names). Three handles in
  resting-pose space, taken by a new grip press within 20 cm (nearest of the hand's): the stick (right; pivot 16 cm
  right, 65 cm down, 30 cm ahead of the eye; tilt taken as a 25 cm lever from where it was grabbed, 20 deg = full,
  1.5 deg dead zone, curve 1.4, full = look input 1.5; back = pitch +, side = yaw +), the throttle (left; slides
  between notches 8 cm apart, -1 / 0 / 1 into move +0x54, stays where left) and the take-off lever (left, low; hinge
  36 cm left, 62 cm down, 10 cm ahead; pushed under 15 deg asks a take-off, pulled over 35 deg a landing, as the
  Land / Takeoff key (Space) held 0.3 s by Controls; the ship's state from the hull's speed, a request trusted 4 s).
  Vehicle's AddLeverDrive adds them to +0x54 / +0x5C / +0x60. Controls: A with the stick held = secondary fire, not
  Space. Drawn by LeverModel after the cockpit (Aim), the hands on them as on the levers. Stereo HoldHullHeading: in a
  flyer the level camera takes the hull's whole attitude (the view pitches and rolls with the ship).
- Starfighter cockpits (b403-b410): ShellDesigns `all_fly_xwing`, `imp_fly_tiefighter`, `rep_fly_jedifighter`,
  `cis_fly_droidfighter`, `cis_fly_geofighter` are whole cockpits (Design::isWholeCockpit, ShellDesigns::IsWholeCockpit):
  Aim does not draw the game's first-person model, the shell (our interior at the real size, eye at the game's
  (0, 1.5, 1.5)) is the cockpit, with the HUD groups on its dash screens (AddDisplay). Built from the stock cockpit
  and exterior textures; previews with `sh tools/shellpreview.sh <odf>` and tools/meshview.py.
  Since b412-b413 (idren2023: ~50k triangles each, accurate to the exterior and the films) every fighter's shell holds its
  own exterior model: tools/stockexterior.py (run by build.sh and shellpreview.sh) moves the mod tools' exterior msh
  into cockpit space (per ship the eye's place in the exterior; x and z turn round, the exterior's +z is ahead) and
  leaves out the exterior's own cockpit inside (a box: faces turned to the eye, or 'all'; a sphere for the TIE balls),
  cutting triangles that cross the region's edge; it writes build/gen/StarfighterExteriors.inc (not in git), which
  StockExterior draws on texture 3 at the model's own texture positions, the glass as the design's glass. The
  interiors use the starfighter kit in ShellDesigns: KitPanel (a panel filled row by row with seeded modules:
  toggles, push buttons, knobs, dials, small screens, slot lamps, vents, cover plates; about 25k triangles a square
  metre), KitPanelBetween, CorrugatedHose. The Y-wing and the TIE bomber have their own builders (the X-wing's and
  the TIE's interiors in their own hulls). Research: docs/research/starfighter-cockpits-film.md. Since b416 (idren2023: the
  stock first-person canopies have the better texture detail; use them at the player's scale) tools/stockcockpit.py
  fits pieces of the game's own first-person cockpits into ours (box onto box per axis, or a scale about the eye and a
  move) into build/gen/StarfighterStockCockpits.inc, drawn by StockPieces on textures 1, 2 and 4: the X-wing's and
  Y-wing's canopy frame, glass and wires on the hull's canopy (the exterior's glass then left out, StockExterior with
  glass -1) and their gauges, the TIE's gauge cluster, the Jedi's canopy glass and holo display, and the droid's
  stock shell as the front of an enclosed cockpit in its head (idren2023: its eyes are cameras; the stock window's red
  visor pane left out for our viewscreen). The exterior's texture v is turned top-down (b415).
- Bikes and the STAP (b245-b248, `src/Handlebars.cpp`, PLAN 55): Drive::Handlebars by the stock table. In the pilot
  seat in first person Aim's vehicle draw calls `Handlebars::DrawHull` instead of the game's first-person model: the
  hull model (`Holster::LoadedModel`, the ODF GeometryName) by the game's model draw (vtable slot 1) at the hull matrix
  with pitch and roll removed (rows model x, up, forward, the hull's position); a skinned hull gets a pose table of
  ours (PoseTableInit / Set, HashName of each bone) with every bone at its msh rest place, so the game's animation of
  handlebars and foils is not seen. Stereo `HoldHullHeading` moves the level and game cameras to the rider's eye
  (`Handlebars::GetSeatEye`: the bike's eye point through the same level hull matrix; only while the game camera is
  within 3 m of it, i.e. first person). Grips in resting-pose space = (-(m.x - eye.x), m.y - eye.y, -(m.z - eye.z));
  a new grip press within 20 cm takes one; `GetHandPose` gives the snapped pose to Aim's `DrawArmsOnLevers` (now via
  `GetSeatHandPose`: levers or grips). Drive: lean = neck x in resting-pose space (head pose + back 9 cm - up 8 cm),
  twist = right controller pitch minus its pitch at the grab; Vehicle's `AddLeverDrive` adds both to +0x54 / +0x5C.
  Test: `vehicle enter hover <odf name>` (named search: 1 km, 128 entities, only named candidates logged), `vr simside
  <cm>`, `bike log 1`, `bike grip <roll> <pitch>`.
- Mounts (2026-11-07, the vehicle proposal: tauntaun and Kaadu ridden as the bikes): Drive::Handlebars for
  all_walk_tauntaun and gun_walk_kaadu in STOCK_VEHICLES; Handlebars accepts a walker seat for a Bike row with
  `isMount`. The animal is not drawn by us: a hook on the walker's Trackable visibility check (GameBuild WalkerVisible
  0x128680) returns true for the ridden mount's entity (`_mountEntity`, set in OnPresent), so the game renders the
  animal itself, animated, in first person; Aim's vehicle draw then leaves the stock first-person model out
  (DrawHull returns true without drawing) and draws the arms on the reins (the Bike grips). Eye and grips from
  human_ride's pose on the ODF's AnimatedPilotPosition bone (Handlebars' table). Vehicle: no head aim in a mount
  (its look yaw would turn the animal), the rider's lean turn goes to the walker's strafe input (a walker turns on
  it). Without the hook the stock model stays. Not yet run.
- Arms on the levers (b223, Aim `CopyArms` / `DrawArmsOnLevers`, feature VehicleHands): every on-foot hand-placed
  draw copies the arms model (+0x15FC), its bones (with hashes) and the arms' place relative to the right hand; in the
  tank the model is drawn at that place relative to the right hand (on its handle while held, Levers::GetHandPose),
  the left wrist as the mirror image of the right wrist's place in the right hand (b229: X' = diag(1,1,-1) X mirror(x),
  the left skeleton being the right turned half way round y), the forearm its length behind, through a pose table of
  ours (PoseTableInit / Set). The first-person arms end at the elbow (msh: bone_shoulders ->
  bone_l_forearm -> bone_l_wrist, 0.271 m; no upper arm), so there is nothing to bend: the forearm follows the hand.
  The copy is dropped at the spawn screen; the model's vtable is checked before each use. No aiming line and no
  ammunition sign on the hand in vehicles (b222, b224).
- Map lighting (b333, PLAN 57 l): the game lights its models in vertex shaders with per-draw constants c18-c21 (scale, ambient, sun colour, sun direction in the world); D3dSurvey keeps them from the draws of a watched texture (WatchLightOf / GetWatchedLight) and CockpitShell lights its shells with them (ambient and one directional light, gain 255/128 so a 0x808080 finish matches the game's cockpit), the fixed lights only as a fallback. Script `shaderdump` logs one first-person pass's constants, textures and draws.
- Shell polish (b320-b321, PLAN 57 l): ShellKit maps every face of the cockpit's and exterior textures at one density (Mesh::texelsPerMeter, ShellDesigns TEXELS_PER_METER 160; Finish::isOwnScale keeps a finish's own `meters`); lofts by each point's place in a local frame of its band, lathes by each band's flat development, sweeps by the path plus each point's offset along its straight stretch (no shear, no stretch). ShellDesigns' detail kit (RoundedBar, CableLoom, GrabHandle, QuiltedPanel, TreadPlate, ToggleBank, KnobRow, Locker, Extinguisher, FlangedPipe, ValveWheel, CellRack, ControlPanel) and FighterTankFittings. Parts of the game's cockpit model replaced by the shell (Shell::maskVertices: the combat speeder's right half, rebuilt from the game's left half mirrored, which tools/mirrorstock.py generates at build time from the mod tools' msh into build/gen, never into git; without the msh the speeder builds without it): CockpitShell::CutOpenings (Aim, GunnerSeat: where the game's cockpit is drawn) marks the pass; the shell draw then ends with the mask triangles, depth only, 5 mm before the game's surfaces: the game queues its model's draw and renders it later in the pass, so its surface there fails the depth test (anything done before or right after its draw call comes too early: runs 344-346).
- Cockpit shells (b232-b234, PLAN 54; idren2023: closed cabins round the stock tanks, ~10-15k triangles each, in the
  tank's style, with a seat; stock vehicles only): every stock first-person cockpit model is drawn at scale 1 with the
  eye at (0, 1.5, 1.5) of its own space, axes as the camera's (x right, y up, z back). `ShellDesigns` builds a shell
  there from the cockpit's open rims (taken from the msh files with tools/mshgeo.py, pushed 1.5-4 % away from the eye
  and tucked behind the game's edges); `CockpitShell` draws it right after the cockpit, before the levers and arms.
  Rules each shell is checked against with tools/shellcheck.py (cube map from the eye): no direction may see nothing
  (holes < 0.1 %), and no shell triangle may lie behind the cockpit's glass on any ray through it (drawn see-through;
  a part before the glass, the front console's brow over the windows' lower edge, is counted apart). The rims are
  pushed from the game's eye; the seat, the front console and the faces turned to the eye are placed from the
  design's pilot eye (b273). Panels must never lie in the plane of the surface under them (z-fighting, b274). The
  Rebel combat speeder (b237-b238) keeps its first-person model's two-seat width and takes the rest from its real
  exterior (all_hover_rebel_attack_speeder): a long canopy whose glass sides are see-through finishes (drawn last,
  blended; shellcheck counts rays through them as a view out, whatever lies behind), a roof, the pilot's and
  passenger's seats, the gunner's raised seat behind, and outside the decks and gun pods seen through the glass.
  A design may use the vehicle's exterior texture as a third texture (Shell.exteriorName, Textures::FindByName)
  and the cockpit's own window texture for its glass (Shell.glassName, texture 4; in the glass pass the texture's
  alpha is multiplied in, so the panes look like the game's windshield: about 10 % in the middle, grimy edges).
  Lighting: ambient 0.85, key 0.32 from above-ahead fixed to the cockpit (b234; the game shows its cockpit textures
  at about their own brightness).
- HUD on the dashboards (b240-b244, idren2023 2026-10-17; front console b273, PLAN 57 e): every stock shell has a front
  console before the seat (ShellDesigns `FrontConsole`: a deck at the handles' height with their two tracks and a
  padded roll, a fascia leaning back toward the eye with three screens (`ScreenFacet`: each turned to the eye, moulded
  bezel, glass, hood; the map's middle 0.80 m from the eye, 26 deg down; health / score left, ammunition / target
  right), a brow, cheeks, open beneath for the legs; per design a `ConsoleLook` (finishes) and a `DashboardLook`
  (scale, finishes, decals of the cockpit's own artwork, optionally the map inside a picture of the cockpit's own gauge
  or display)) whose screens are `ShellDesigns::Display` slots (group, centre, right / up axes, room). CockpitShell
  draws the HUD groups HudDraws reports as taken (`HudDraws::IsTaken`) on them after the shell, in the same
  first-person pass (depth test on, no depth write, lighting off, the masked minimap through stage 1, alpha x2), so
  the levers and arms drawn after cover them; each group fills its slot's width or height. `CockpitShell::HasDisplays`
  (a shell with displays drawn in the last 250 ms) makes Hud leave those groups off the head panel and the wrist; the
  classic layout takes nothing, so the game's own HUD stays and the screens stay dark.
- Stock screens (b293-b294, PLAN 57 j): the HUD's screens are the side's own gunner screen texture
  (<side>_1st_cockpit_securitycam, shell texture 5 = the design's `screenName`) cut in nine pieces round any glass size
  (ShellDesigns `StockScreen`, `ScreenArt`), the glass a tiled plain square of the stock glass over a dark backing;
  Display `tint` makes CockpitShell draw the groups as monochrome readouts (`ReadoutColour`, reds kept; script
  `shell tint`). `CockpitShell::ShowsGroup`: Hud leaves only the groups a drawn screen shows off its panel. A Display
  may show part of a group (region: whole batches by their middle; the HealthGroup's player / vehicle halves) or,
  with width 0, leave the group out of the seat; StockScreen can be curved (curveRadius, CurvedFrame).
- AAT commander's seat (b298-b301, PLAN 57 k): the AAT is a GunnerSeat Cabin like the speeder (Cabin: design name,
  optional game first-person model, end stops or not); since b300 its design is the AAT's own cabin `cis_hover_aat`
  with the speeder's six-part layout (the commander on a loft over the rear ramp, under the turret's race ring);
  CockpitShell::GetPilotFrame gives the pilot seat's cockpit frame: the tank levers and the hands holding them are
  drawn in it (they tilt with the hull), no longer in the level camera frame;
  the turning gunner seats' controls are grips on the seat's armrests (ShellDesigns ArmrestsAndFrame,
  ARMREST_GRIP_*; GunnerSeat CABINS grip places), the periscope's rails box sections on struts from the turntable;
  CockpitShell logs `shell draw cost` (main thread ms per eye pass) every 1 000 draws; shells hold 60 000 triangles; the turntable's display pitches by the seat's first fire line (Vehicle::GetSeatFireLines). The station
  path (STATIONS) now serves only the fighter tanks.
- Trim (b240): ShellDesigns `SeamTrim` (a bolted round moulding along a cockpit rim, pushed 1.2 % from the eye like
  the shell's joints), `Strip` (bevelled skirting / cornice), `DeckSeam` (flat two-triangle line), `FloorHatch`.
  Cost lesson: long bevelled boxes and tubes are cut on every texture repeat; flat quads and sparse bolts are cheap.
- Gunner and passenger seats (b249, PLAN 56): Vehicle names the stock vehicle under a gunner seat too
  (`CurrentStockName`; drive stays sticks), `GetTurretMatrix` (MountedTurret +0x210), `CurrentHullEntity`; Aim's
  vehicle draw logs `fp: gunner seat` and `gunner frame 0` leaves the game's targeting frame out.
- Fighter tanks' gunner station (b250-b252, `src/GunnerSeat.cpp`, idren2023 2026-10-18: replace the stock turret
  entirely): in the gunner seat of rep / imp_hover_fightertank with VR on, Body's model draw hook (now installable on
  its own, `Body::InstallModelDrawHook`) calls `GunnerSeat::BeforeHullDraw` first: a draw whose bone set is the hull's
  pose table (E+0x1B34) has its turret_y / turret_x / turret_barrel1 slots copied (live rotations) and, while our
  station was drawn last frame (first person), pointed at a matrix with no rotation part (the 440 turret vertices
  collapse into one point inside the hull), restored right after (`AfterHullDraw`). In the first-person pass Aim
  calls `GunnerSeat::DrawTurret` instead of the targeting frame: CockpitShell::DrawParts draws ShellDesigns'
  `<odf>_gunner` (three parts: base on the yaw bone, cradle on the pitch bone, barrel also on the pitch bone) with
  part matrix = turned design space x minus rest place x (bone rotation at its chained place) x the hull draw matrix;
  places chained from the rest pivots because the game writes these bones with translation (0, 0, -1000). Stereo
  `HoldHullHeading` takes `GunnerSeat::GetSeatEye` (hull model (0, 2.70, -2.858), on the yaw axis, through the entity
  matrix) as Handlebars'. In third person nothing changes (the station and the hiding need a first-person draw in the
  last frame; DrawTurret also checks the game camera is within 6 m of the eye). ShellKit vertices carry a `part`;
  CockpitShell groups by part and finish. Test: `gunner turret 0|1`, `tools/scripts/gunner-station-test.txt`.
  b253-b261: a `Station` names per part the bone whose rotation turns it and a pivot (rest place; the AAT's visor turns
  about the eye), places always chained from the pivots; `hidesStockTurret` (fighter tanks yes, AAT no: the game hides
  its turret in first person itself), a collapse point per station. Grips (`PlaceGrips`, on the part they sit on,
  right grip in design space, the left mirrored about an x): a grip press within 20 cm of the hand's fist
  (`Aim::GetPalmInHand`: the arms' hand mesh centroid in bone_r_wrist's frame, rep_1st_arctrooper.msh, carried into
  the right hand's frame by the copied arms) takes it; `GetHandPose` then returns the grip pose moved so the fist
  closes on the grip's axis (Aim's `GetSeatHandPose` asks Levers, Handlebars, GunnerSeat; the arms are drawn after
  the station). AAT: `cis_hover_aat_gunner` (cabin on Gun_body, visor with the targeting frame on Main_gun). `Cabin`
  seats (the combat speeder's gunner): no station; the game's pilot first-person model (by name, Holster::
  LoadedModel) and the pilot shell (`CockpitShell::Draw`) placed as in the pilot seat (shell's pilot eye on the ODF's
  pilot EyePointOffset, hull's level heading), the eye at the shell's gunner seat, `ShouldSkipHullDraw` leaves the
  real hull out while shown (Body's model draw hook asks first). `DrawTurret(flags)`.
- Gunship door passenger (2026-11-07): TURRET4 is a MountedTurret seat (FLYERSECTION, YawLimits -95..-60, no
  weapon, no FirstPerson key) whose turret reads at the hull's origin like the co-pilot's: GunnerSeat `Cabin::
  yawMiddleDegrees` picks a cabin by its turret's yaw limits (|middle| within 15 deg) before the turretAhead rows.
  Design `rep_fly_gunship_door` (ShellDesigns BuildGunshipDoor): the ship's own exterior from his eye
  (stockexterior.py GUNSHIP_DOOR; the exterior models the troop bay), a rail with two hand straps (the grips) and a
  grab bar at the door's front edge; standing at hp_passenger1 facing ahead. Shows only if the game gives the seat a
  first-person pass (unknown). His own weapon: research (docs/research/vehicle-controls-proposal.md).
- Turret seats' head aim (b253, Vehicle `StepTurret`): the turret's own angles (MountedTurret +0x2B8 pitch, +0x2C0
  yaw, relative to the parent's matrix, its vtable slot 14) are written each update from the head's forward in the
  parent's frame (the update's own formulas: yaw atan2(x, z), pitch -asin(y)), smoothed (0.15 s) and held within the
  section's limits (+0xE30 yaw, +0xE38 pitch pair); the inputs sent are +-1, which make the update keep the written
  angle (it blends toward the turret's last direction by 1 - |input|), the filtered rates (+0x2BC, +0x2C4) get our
  speed and the step the update adds is measured and taken off the next write. Pilot seats keep the rate loop.
  Vehicle also hooks the game's first-person turret hiding (`TurretHideChildren` 0x196390) and skips it for a hull
  pose table without a skeleton (+0x404 null: the Imperial fighter tank on Rhen Var, run273 crash).
- Test: `vehicle <head aim 0|1> [log 0|1]`, `vehicle enter [kind][+]` (EntityQuery + CanEnter / Enter, vtbl 16 / 17;
  nearest first; `+` = with a gunner seat, hover / flyer turret count bytes E+0x540 / E+0x574).

### Eye images on the graphics card (b268-b272, 2026-10-18; PLAN M2)
- `GpuTransfer::CreateDirect3D` (from D3dSurvey's Direct3DCreate9 hook): with ini `gpu_transfer` 1 (default: only
  when vrserver.exe runs at game start) or 2, our D3D12 device on DXGI adapter 0 and the game's IDirect3D9 from
  `Direct3DCreate9On12` on it (D3D9On12 keeps its own queue: on ours the game's draws were slower). The game creates
  its 15 devices on it; our device hooks work unchanged. Settings::ReadEarly reads the ini value (before Load).
- Per eye in Stereo::OnPresent, when `Xr::TakesSharedImages()` (GpuTransfer active; with a headset: Xr's D3D11
  device on the same adapter LUID, BGRA swapchain, nothing refused): `Stage` (StretchRect into the slot's render
  target), `Transfer` (Unwrap onto our queue, which waits on the card for D3D9On12's work; CopyResource into the
  slot's shared texture; Signal `written`; Return with that fence) and `Xr::SubmitEyeShared`: acquire, D3D11 Wait on
  `written`, CopyResource into the swapchain image, Signal `read`, Flush, release. Before writing a slot again,
  Transfer checks `read` on the CPU (capped at 20 ms, so a missing signal cannot hang the card). Slots: 0 / 1 eyes,
  2 the flat screen (`Xr::SubmitFlatShared`). Simulated headset: GpuTransfer signals `read` itself.
- Each Return with a fence makes the next Returns of that resource slower, so a slot's target is made again every
  2000 transfers. Unwrap costs about 1 ms per eye (it submits the eye's draws).
- Any failure turns GpuTransfer off for the session (logged once) and Stereo uses the system-memory path: left eye
  copied on the card, both read back after the right eye's render (one GPU wait per frame), HUD probe every 32nd HUD.
  The read-back is slower on D3D9On12 (probe: 2.2 against 1.9 ms per eye).
- After the left eye's transfer one pixel of `_eyeSurfaces[0]` is filled (b623): its Present is skipped, and with an
  empty D3D9On12 command list the game's frame-sync query (offsets "Display size and frame sync") never completes
  (the b337 freeze). `GpuTransfer::LogState` logs the fences for the first eye images and from the hang watchdog.
- `render_size = <width>x<height>` (ini, read at Direct3DCreate9 by D3dSurvey `ApplyRenderSize`, b624): sets the game's
  own forced window size, so the back buffer, the game's targets, our HUD texture and the eye swapchains all take it.
  Keep the monitor's shape (16:10): the menu cursor's counts down the screen change with the shape (Pointer).
- Profiling stage `gpucopy`; `gpu: ms per transfer` every 500 transfers (StretchRect, Unwrap, consumer wait, record
  and execute, Return).

### Vehicle cockpits and stations (2026-11-04, b506-b533)
- Since b630 the AT-AT and AT-ST cabs are whole cockpits: `tools/stockcockpit.py` ('mirror', plane, kept once,
  spanned) cuts the game's pilot model at the cab's middle and mirrors it for the other seat (winding reversed), the
  AT-AT's window one quad spanning both; the stock model is not drawn (GunnerSeat's cabins have no cockpitModelName).
  AT-ST co-pilot at x 2.60 (ATST_MIRROR_X 1.30). `CabinCornerTrims` covers box cabins' inside corners.
- `tools/stockexterior.py` without_twins: triangles lying on another of the same kind in its plane are left out (the
  shells draw without culling, so back-to-back faces fought for depth).
- ShellDesigns: a design per vehicle seat. Pilot shells round the stock first-person cockpit (AT-ST, AT-AT, AT-TE,
  gunship, MAF: `PilotEye` moves the stock model so the design's eye meets the game camera) or whole cockpits
  (`isWholeCockpit`: spider, hailfire, snowspeeder). `DroidPod` (an ellipsoid room) + `PortWall` (the stock frame
  texture laid over the pod's front at its 1.22 aspect, its port glazed, a lip) for the droids. `ThrottleConsole` under
  WalkerDrive's lever.
- Gunner stations: GunnerSeat `Cabin` rows. Swivel stations use SPEEDER_CABIN_PARTS (1 chair/column/grips/rails turned
  with the turret's yaw about the gunner's eye, 2 the display pitched, 3/4 stop lamps, 5 the scope picture). A vehicle
  with several turrets: `turretAhead` / `turretRight` (the turret origin in the hull's frame; a turret without its own
  node reads at the hull's origin: `NO_TURRET_NODE` -1000). `isFacingAft` (Stereo turns the resting view round: the
  snowspeeder's tail gunner, the AT-TE's rear guns). `isViewHeld` (Controls treats it as a station seat: the view keeps
  its heading, the hull turns under it: the AT-TE's open-air cannon). The MAF uses one design per seat (its other
  station still).
- tools/stockexterior.py: ships' exteriors round a seat (eye in the exterior's space); `'only'` boxes keep just a
  moving part (the AT-TE's cannon: ATTE_GUN) while the hull table cuts it (ATTE_CANNON); CUT_BOXES per table; previews
  `--obj DIR` write ext_<table>.obj with v the model's way up.
- Recon droid (b653): the player's Controllable an `EntityDroid` (Seat::Other) sets `Vehicle::IsRemoteDroid`: the look
  loop runs as for a gun seat (head aims yaw and pitch), Controls holds the view (SetViewFree) so the droid turns under it;
  the triggers are the vehicle seats' (right primary, left secondary = its orbital strike), X the use key (back to the
  soldier). Holster: WeaponRemote is a Throwable (b652), launched by a throw like the grenade.
- `vehicle_controls` (Settings, Vehicle::IsSticksMode): the lever modules ignore the grip; Flight's remote take-off on
  A; walkers turn after the view (Controls' stick turn) by head steering.

### Our hands on the bike grips; hands on entering a seat (2026-11-07, PLAN 61 j; not yet deployed)
- Bikes and the STAP draw our hands (BodyArm) for every unit that has them, instead of the stock arms copy: Aim
  DrawOurBikeHands after Handlebars::DrawHull. A held grip: Handlebars::GetBarFrame (the bar's frame in the world: x out
  along the bar, y up, z back, origin on the bar's axis; the left grip's frame mirrored; Bike barCentre / barOutward
  measured on the hulls' grip vertices; the rider's shoulder from the head) -> BodyArm::DrawOnBar: the skeleton's
  "vehicle: bike grip (fitted)" pose (human, "b1 vehicle", "sbd vehicle"; tools/body_bikegrip.py fit), the bar through
  the fist on BAR_AXES, the hand rolled about the bar so the straight forearm points at the elbow of a two-bone arm
  from the shoulder, the wrist bending the rest (ComputeSkin's forearmToward, at most 30 deg); the twist grip turns the
  hand further. A free hand: DrawEmptyHand / DrawLeft on the controller as on foot. BodyArm::CurrentUnit keeps the unit
  last seen on foot while in a vehicle (the renderable then holds the seat's model).
- Free hands in every seat: Aim GetSeatHandPose replaces the seat modules' Gesture copy (taken at the last Present) by
  this frame's Xr pose: the copy was a frame old and, on the first frame in a seat, in the resting space from before
  Vehicle's recentre on entry. Handlebars::DrawHull takes the bike from Vehicle at once (the entry frame drew the game's
  stretched first-person model and no hands).

### Shell texturing rules (2026-11-09)

- `ShellKit::Quad` without explicit s/t keeps the whole patch only when the quad has about the patch's proportions (within 1.6x); otherwise it maps by real size (PlanarQuad).
- `ShellDesigns::Textured`: every unlit, opaque plain-colour finish of a design takes the patch of its plate (`Design::plateFinish`, else the first plain textured finish of texture 1) in its own colour.
- `StockExterior(..., liningFinish, liningReach, liningAbove)`: hull faces near the eye turned away from it (or above `liningAbove`) get an inner lining in one plating direction; caps (texture 5) map by real size.
- `tools/uvaudit.py` measures flat and stretched area per design and finish (`--where` locates); `tools/allpositions.py` renders every seat.
