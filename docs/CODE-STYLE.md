# Code style for swbf-vr

Agreed with idren2023 on 2026-10-09. The goal is code idren2023 can read and navigate (VS Code, IntelliSense) coming
from C# and JavaScript. **Status: migration steps 1 to 5 done (2026-10-12), section 7 built (2026-10-12, game run pending):** logging goes through `src/Log.h`; `.clang-format` and `.clang-tidy` are live
in the repo root and `src/` is formatted; every subsystem is renamed to the style (`Name.h`/`Name.cpp`, `namespace SwbfVr::Name`).
clang-tidy is clean (section 8). New code follows it.

## 1. Naming

No abbreviations, in any kind of name. A name should say what the thing is or does without a comment.

| Kind | Style | Example |
|---|---|---|
| Types (class, struct, enum, type alias) | PascalCase | `WeaponAimer`, `HandPose` |
| Functions and methods | PascalCase | `ComputeReticlePosition()` |
| Local variables and parameters | camelCase | `headPitch`, `eyeIndex` |
| Private and protected member variables | `_` + camelCase | `_frameCount` |
| File-level variables (anonymous namespace in a `.cpp`; like private static fields) | `_` + camelCase | `_hasShoulder` |
| Public fields of plain data structs | camelCase, no prefix | `pose.position`, `vector.x` |
| Constants (`constexpr`, `const` globals) | SCREAMING_SNAKE_CASE | `MAXIMUM_HAND_COUNT` |
| Engine addresses | `ADDRESS_` + camelCase | `ADDRESS_registerHit` |
| Engine struct offsets | `OFFSET_` + camelCase | `OFFSET_soldierHealth` |
| Enum types and their values | PascalCase | `ZoomLevel::Second` |
| Namespace | PascalCase | `SwbfVr` |

- **Acronyms** follow the C# rule: two letters stay upper case (`IO`), longer ones are written as a word (`Xr`,
  `Hud`, `D3d`: `XrSession`, `HudPanel`). Acronyms are fine where they are the common name of the thing
  (XR, HUD, FOV, IPD, DLL, D3D, Lua); invented short forms are not (`pos`, `cnt`, `mgr`, `idx`, `tmp`, `cb`).
- **Units go in the name** when a number has one: `distanceMeters`, `angleRadians`, `delayMilliseconds`.
- **Single letters** only for the coordinates `x`, `y`, `z`, `w`. Loop counters are named after what they count
  (`for (int eye = 0; eye < 2; ++eye)`).
- **Booleans** read as a question: `isZoomed`, `hasWeapon`, `shouldLog`.
- Names from outside our code (Windows, OpenXR, Direct3D, the game) keep their own spelling.

## 2. Layout

Enforced by clang-format (`.clang-format`).

- 4 spaces, no tabs.
- Braces on their own line (Allman, as in C#), for every block, including one-line `if`, `for` and `while`.
- About 120 characters per line. Not strict, but no long lines for the sake of it.
- When a call or a declaration does not fit, the `(` stays on the first line and each parameter or argument goes
  on its own line, one level indented:

  ```cpp
  bool CreateSwapchain(
      XrSession session,
      int64_t format,
      uint32_t width,
      uint32_t height,
      XrSwapchain& swapchain);
  ```
- One statement per line, and one blank line between functions.

## 3. Files and structure

- **Header and source files.** C++ compiles each `.cpp` file on its own, and a file can only use what has been
  declared before it. A header (`.h`) is the public face of a subsystem (roughly a C# interface: types and
  function signatures), and other files `#include` it, which pastes its text in. The `.cpp` file holds the bodies.
  Rule: one `Name.h` / `Name.cpp` pair per subsystem, named after it (`Stereo.h`, `Stereo.cpp`). The header holds
  only what other files need. Helpers and state used by one file stay in that `.cpp`, inside an anonymous
  namespace (`namespace { ... }`, the equivalent of `private` at file level).
- Every header starts with `#pragma once`, which stops it from being pasted twice.
- **Include order:** the file's own header first, then other project headers, then Windows SDK headers
  (`<windows.h>`, `<d3d9.h>`, ...; OpenXR's platform header needs them), then libraries (OpenXR, MinHook), then
  standard headers (`<cstdint>`, `<cmath>`). clang-format sorts and groups them.
- **Namespace:** all our code is inside `namespace SwbfVr`. The functions the game or Windows calls by name
  (the `dinput8` exports, hook thunks with a fixed signature) are the exception and say so in a comment.

## 4. Language features

- **`enum class`** for every enum. It works like a C# enum (`ZoomLevel::Second`). Plain `enum` is not used,
  because its values leak into the surrounding scope.
- **`auto`** only when the type is visible on the same line (`auto aimer = reinterpret_cast<Aimer*>(address);`)
  or is an iterator. Everywhere else, write the type.
- **References and pointers.** A reference (`Pose& pose`) is an alias that cannot be null; it is used like a
  normal variable (`pose.position`). A pointer (`Pose* pose`) is an address that can be null and is used with
  `->`. Parameters that must exist are references (`const` when they are only read). Pointers are for "may be
  absent" and for game memory.
- **Ownership.** C++ has no garbage collector: something created with `new` must be freed with `delete`.
  - Objects we create and own are held in a `std::unique_ptr<T>` (made with `std::make_unique<T>()`), which frees
    them automatically when it goes out of scope, like a `using` block in C#.
  - A raw pointer (`T*`) never owns anything. It only means "I am looking at this". All game memory is reached
    through raw pointers, and we never free it.
  - There is no `new` or `delete` outside a smart pointer.
  - The project rule still applies: our data must not live in the game's heap. Before the first `unique_ptr` is
    added, check which heap our `operator new` uses with this toolchain. Until then, long-lived state stays in
    file-level variables, as today.
- **`const`, strictly.** Everything that is not changed is `const`: local variables, reference and pointer
  parameters that are only read, and methods that do not change their object (`float Length() const`). A reader
  can then tell from the signature which functions only read. `constexpr` is used for compile-time constants
  (the C# `const`).
- **No exceptions** in our code. Our code runs inside the game's threads, called from the game's own functions.
  An exception that escapes into game code crashes the game with an unreadable dump. Functions that can fail
  return `bool` or `std::optional<T>` (like C# `T?`) and log the reason where it happens. Every entry point the
  game or a library calls (hooks, thunks, callbacks) is a boundary where any exception thrown by a library is
  caught and logged.
- **No macros** (`#define`), except where nothing else can do the job (compiler or platform switches, things that
  need `__FILE__`/`__LINE__`). Each one has a comment saying why it has to be a macro. Constants use `constexpr`
  and code uses functions.
- **Casts** are written as C++ casts so they can be found and say what they do: `static_cast` for ordinary
  conversions, `reinterpret_cast` for treating an address as a typed pointer (game memory). No C-style
  `(Type)value` casts.
- **Type aliases** use `using` rather than `typedef`, also for function pointer types:
  `using RegisterHitFunction = void(__fastcall*)(void* self, void* edx, int damage);`
- **`nullptr`**, never `NULL` or `0`, for pointers.

## 5. Comments

- **Doc comments** (`///`) on every function and type declared in a header, so they show up in IntelliSense:
  what it does, its parameters, its return value, and on which thread it may be called if that matters.
- **Engine hooks and game memory get full explanations, not a fixed form.** For each hook or engine access, say:
  what the game function or structure does, as far as it is known; what we change and why; how it was found
  (which run, which tool) and which entry of `docs/offsets.md` it uses; the calling convention and which thread
  calls it; and what was observed versus only inferred.
- **Why-comments** on any block whose purpose is not clear from its names, especially the "business" reason
  (what the player experiences, which headset report it fixes).
- **C++ for a C# reader.** Where the code uses a language feature with no clear C# equivalent, add a short
  comment explaining it: inline assembly, calling conventions (`__fastcall`, `__thiscall`), pointer arithmetic,
  `reinterpret_cast` over game memory, RAII guards (objects that do their work in the destructor), templates,
  `std::atomic`, lambdas that capture by reference.
- **No TODO comments.** Open work goes into `docs/PLAN.md`, which may name the file and function where the
  change belongs.
- No commented-out code; git history keeps old versions.

## 6. Logging

People will send their logs to diagnose their setup, so a log must be readable without access to their PC.

- One logging interface, with four levels: `Log::Debug`, `Log::Info`, `Log::Warning`, `Log::Error`. Debug lines
  are off in release builds unless switched on in the ini.
- Every line has a timestamp, the level and the subsystem: `12345 INFO  Stereo: swapchain 2064x2272 created`.
- The first lines of every log: mod version and build id, the hash of `Battlefront.exe` and which game build
  was detected, Windows version, OpenXR runtime and headset name, and the ini settings that differ from their
  defaults.
- Errors say what failed, what the mod did about it (feature turned off, flat mode) and, where it helps, what
  the user can do.
- No log line in a per-frame path except under Debug or as a rate-limited summary.

## 7. Game builds and failed engine checks

Only the Steam build can be tested. Other builds (GOG, the original disc, the Classic Collection) may have
different addresses, so the mod has to fail safely and say why.

1. **Identify the build at startup** from the hash of `Battlefront.exe`. A known build uses its address table.
   All addresses for one build live in one place, matching `docs/offsets.md`.
2. **Unknown build:** look each needed function up by a byte pattern around it (a signature) instead of a fixed
   address, and log what was found where.
3. **Check every feature on its own:** if its addresses are missing or fail their check, that feature is turned
   off with one clear log line (which feature, which check). The rest of the mod keeps working.
4. **If the core (rendering or OpenXR) cannot be set up,** VR is turned off and the game runs flat, as if the mod
   were not installed. The log explains why and asks the user to send it.
5. The mod never crashes the game on purpose because of a failed check.

Steps 1, 3 and 4 can be built and tested on the Steam build. Step 2 can be tested by deliberately using a wrong
hash.

**Done 2026-10-12 (`src/GameBuild.h/.cpp`, ARCHITECTURE section 12; built, not yet run in the game):**
- [x] 1. `GameBuild::Identify` hashes the exe at start (the hash moved there from the log header); the Steam build
  uses `STEAM_ADDRESSES`. All 61 engine addresses are in `GameBuild.cpp`, with their signatures; the per-file
  `ADDRESS_` tables, `engine.h`'s two addresses and the `..._CODE` first-bytes arrays are gone. Struct offsets
  (`OFFSET_`) stay `constexpr` (not build-specific, see ARCHITECTURE section 12). Diagnostic tools (Probe, Watch,
  EngineSurvey's vtable list, checked by RTTI) are outside the table.
- [x] 2. Unknown build: each signature is searched in `.text`, must match once, one log line per address.
  `tools/signatures.py` replays search and known-build check over `re/battlefront-image.bin` with the patterns read
  from `GameBuild.cpp`: 61 of 61 unique and at the Steam addresses. The C++ itself was also run offline over the
  dump (rebased by its relocation table): known, forced-unknown (61 of 61 equal the table, 0.1 s), broken check.
- [x] 3. Ten features (`GameBuild::Feature`), each switched off alone with one line `feature off: <feature>. Check
  <address>: <reason>`.
- [x] 4. Without the core (Stereo, Limits) `Xr` refuses to start and the log asks for the file.
- [x] 5. Checks only read memory; nothing crashes on a failed check.
- Test switches in swbf-vr.ini: `test_unknown_build = 1`, `test_break_check = <Address name>`.
- [ ] Game run (PLAN "Next session"): normal start like b97, forced-unknown start, one broken check.

## 8. Tools

- **clang-format** applies sections 2 and 3 (layout, include order, braces). Config: `.clang-format`.
- **clang-tidy** checks most of section 1 (naming, except abbreviations, which need review) and parts of
  section 4 (`nullptr`, `using`, C-style casts, braces, macros). Config: `.clang-tidy`.
- Both come with the toolchain (`re/toolchain/bin/`, version 19, from `tools/setup-toolchain.sh`), as does `clangd`.
  Format: `re/toolchain/bin/clang-format -i src/*.cpp src/*.h`. Check one file:
  `re/toolchain/bin/clang-tidy src/Aim.cpp -- --target=i686-w64-mingw32 -std=c++20 -Iexternal/minhook/include -Iexternal/openxr/include`.
  `-Isrc` must not be added: Windows file names ignore case, so `<limits.h>` would find `src/Limits.h`.
  clang-tidy is not part of the build. Pass of 2026-10-12: 498 warnings before (475 naming, almost all the `_` prefix of
  file-level variables, which `.clang-tidy` now allows as `GlobalVariablePrefix`), none after; same bytes as b97.
  Fixed: static local constants renamed to SCREAMING_SNAKE_CASE, `memcmp`/`strcmp` results compared with `!= 0`, a
  `default:` in `Crash`'s switch, an explicit `void**` cast, an increment moved out of a condition in `Xr`. Suppressed
  with `// NOLINT` and a reason: external names (OpenXR function fields in `Xr`, the C-linkage stub names in `Feedback`,
  `Proxy_DirectInput8Create`, `__ImageBase`) and the macros that must stay (`BUILD_ID`, `DIRECTINPUT_VERSION`, the
  X-macros). Off in `.clang-tidy`: `bugprone-casting-through-void` (the `GetProcAddress` casts) and
  `performance-enum-size`.

## 9. Migration (steps 1 to 5 done)

The current code (about 7,400 lines before formatting, 13600 after, in 22 `.cpp` files and `common.h`) is written in a C-like style:
`Subsystem_Function()` names, one shared header (`common.h`), `typedef` function pointer types, `float*` math,
printf-style `Log()`, short names such as `g_reticleV` and `dik`. It uses no smart pointers and no `new` or
`delete` at all: state lives in file-level variables, partly because of the game heap rule.

Order, one step per session, each shipped and checked so that behaviour is unchanged:

1. [x] (2026-10-11) Configs live, `src/` formatted in a commit that only changes formatting. The formatted sources
   build to the same bytes as the deployed b96 except the PE timestamps, so nothing was redeployed and no game run
   was needed. Change to the draft: Windows SDK headers form their own include group ahead of the libraries,
   because `openxr_platform.h` needs `<d3d11.h>` first.
2. [x] (2026-10-11, b97) Logging: `src/log.h`, all calls moved over by script; subsystem from the old `xr:` prefixes
   (kept lower case so existing greps still match); levels from the wording, Debug unused so far. Startup header as
   in section 6, the ini values in the `settings` lines when the ini is read (first frame), runtime and headset when VR starts.
3. [x] (2026-10-11) `common.h` split into one header per `.cpp` (`stereo.h` for `stereo.cpp`, ...; file names stay
   lower case until the files are renamed in step 5), plus `fp_guard.h`; every file includes what it uses, and all
   code is in `namespace SwbfVr` except `DllMain` (called by its global name). The old one-line comments became
   `///` doc comments as they were; proper doc comments come with step 5. `.clang-format` puts `<windows.h>` ahead
   of the other SDK headers (`mmsystem.h` needs it). The build is the same bytes as b97 except the timestamps, so
   nothing was redeployed.
4. [x] (2026-10-11) Engine addresses: `src/engine.h` holds the addresses and offsets several subsystems use (first-person
   object, soldier, Aimer, weapon, camera manager, RedCamera matrix, vtable range); each `.cpp` that touches the
   engine has its own table near the top (`// ---- Engine addresses ...`), all as `constexpr uintptr_t`
   `ADDRESS_`/`OFFSET_` with a `///` comment saying what it is and where it comes from (offsets.md section, run).
   The expected first bytes stay next to their address (`k...Code`, renamed in step 5). Not converted: the vtable
   list in `engine_survey.cpp` (a name table from tools/rtti.py) and the diagnostic tools (probe, watch).
   Same bytes as b97 except the timestamps, so no game run and no redeploy. Groundwork for section 7: one build's
   addresses are now all named, but still spread over the subsystems' files.
5. [x] (2026-10-12) Rename and comment one subsystem at a time, starting with the ones that will be changed next for features.
   Each rename is its own commit, without behaviour changes. Pattern (set by `Handling`, 2026-10-11): files
   `Name.h` / `Name.cpp`; the public functions in `namespace SwbfVr::Name` (`Handling::ApplyWeight`, as `Log::Info`)
   with `///` doc comments and `@param` lines in the header; helpers, constants and state in an anonymous namespace
   in the `.cpp`; numbers with a meaning become named `constexpr` constants. Matrices stay `float*` to 16 floats until
   the mod has a matrix type. `tools/build.sh` links the sources in case-insensitive order, so a rename alone keeps
   the code layout: check a rename by building with the deployed build's id and comparing the sections (only
   `.buildid` may differ); then no game run is needed. For pure arithmetic also compare old and new natively on random
   inputs (done for `Handling`: 200,000 calls, all bit-identical).
   - [x] `Handling` (2026-10-11; `aim.cpp`'s `Handling()` became `ApplyWeaponHandling()` to free the name).
   - [x] `Stereo` (2026-10-11; public names: `Stereo::OnPresent`, `GetBaseCamera`, `IsZoomed`, `ComputePointerUV`,
     ...; test switches and HUD, lift and zoom numbers as constants). Same bytes as b97 except the build id. Kept
     out of the anonymous namespace: the grab test code (`GrabBackBuffer`, `WriteGrabImages` and their state), which
     had external linkage before; inside it the compiler inlines them and the layout changes. It stays there (see the end of this section).
   - [x] `Aim` (2026-10-12; public names: `Aim::OnPresent`, `BeforeRender`, `GetHandWorld`, `GetHandYaw`,
     `GetFireLine`, `IsTwoHanded`, `Kick`, `ForgetRestingPoses`, `SetWeaponHandling`, ...; hooks `AimerSetHook`,
     `FirstPersonRenderHook`; grip, kick, lag and test numbers as constants). Same bytes as b97 except the build id.
   - [x] `Xr` (2026-10-12; public names: `Xr::Service`, `IsStarted`, `IsRunning`, `BeginStereoFrame`, `GetViews`,
     `SubmitEye`, `EndStereoFrame`, `SubmitFlatFrame`, `GetHandPose`, `GetControllerInput` (`XrPad` became
     `Xr::ControllerInput`, button fields `isLowerPressed`, ...), `Vibrate`, `SimulateHeadset`, ...; OpenXR's own names
     unchanged; the function pointer table is `_xr`, its X-macro list `XR_FUNCTIONS` one entry per line inside
     `clang-format off`). Same bytes as b97 except the build id. Kept out of the anonymous namespace: the simulated
     headset's state and helpers (`SimulateViews`, `ApplySimulationRequest`, ...), which had external linkage before.
   - [x] `Hud` (2026-10-12; public names: `Hud::Composite`, `OnProjection`, `OnReset`, `IsCompositing`, `DrawVignette`,
     `SetVignette`, `SetLayout`, `SetCrosshair`, `SetScopeReticle`, `SetMode`, `CurrentMode`, `DumpNext`, `Accumulate`;
     element indexes `ELEMENT_AMMO`, ... as constants instead of a plain enum; vignette, wrist panel and clip numbers as
     constants). Same bytes as b97 except the build id; everything private was already file-local, nothing kept outside.
   - [x] `Controls` (2026-10-12; public names: `Controls::OnPresent`, `CurrentMotion`, `SetTurn`, `SetMove`, `SetGestures`,
     `SetCrouchByHead`, `SetSecondaryKey`, `SetCountsPerDegree`; hooks `SetStrafeHook`, `SetMoveHook`; key codes `KEY_W`, ...,
     stick, trigger, crouch and timing numbers as constants). Same bytes as b97 except the build id; nothing kept outside.
   - [x] `Settings` (2026-10-12; public names: `Settings::Load`, `OnPresent`, `IsMenuOpen`; the table is `_settings`, applied
     by `ApplySettings`, the Lua screen `LUA_SCREEN`; ini keys and menu labels unchanged). Same bytes as b97 except the
     build id; nothing kept outside.
   - [x] `Script` (2026-10-12; public names: `Script::Install`, `IsAutorunPending`; command words and file names
     unchanged; poll, foreground, autorun and percent numbers as constants). Same bytes as b97 except the build id.
   - [x] `Input` (2026-10-12; public names: `Input::OnDirectInputCreated`, `IsReady`, `SetActive`, `Key`, `MouseMove`,
     `MouseButton`, `MouseWheel`, `LogStats`; hooks `GetDeviceStateHook`, ..., originals `_original...`; `Kind` became
     `enum class DeviceKind`). Same bytes as b97 except the build id; nothing kept outside.
   - [x] `D3dSurvey` (2026-10-12; public names: `D3dSurvey::Install`, `FrameCount`, `PresentingDevice`, `StartDrawLog`,
     `RequestTrace`, `RequestPresentStack`; hooks `PresentHook`, ..., trampolines in `_original`; the X-macro list
     `DEVICE_HOOKS` one entry per line inside `clang-format off`). Same bytes as b97 except the build id; nothing kept outside.
   - [x] `EngineSurvey` (2026-10-12; public names: `EngineSurvey::Resolve`, `Install`, `OnPresent`; the rtti.py table is
     `VTABLE_HOOKS` of `VtableHook`, data unchanged; `Cls` became `ClassEntry`, `IntSet` `KeySet`; walk, scan, snapshot
     limits and timers as constants). Same bytes as b97 except the build id; nothing kept outside.
   - [x] `Feedback` (2026-10-12; public names: `Feedback::OnPresent`, `SetOptions`, `Pulse`, `SetDumpHitKinds`; hooks
     `RegisterHitHook`, `HitValueHook`; the assembly stub's C-linkage names `Feedback_FireStub`, `Feedback_FireNotify`,
     `g_feedbackFireOriginal` unchanged; pulse, colour, marker, log and thud numbers as constants). Same bytes as b97
     except the build id; nothing kept outside.
   - [x] `Limits` (2026-10-12; public names: `Limits::Install`, `Apply`; hook `FlushHook`; the game's queue structs
     `SortedQueue`, `ListQueue`; table sizes, entry sizes and log thresholds as constants). Same bytes as b97 except the
     build id; nothing kept outside.
   - [x] `Gesture` (2026-10-12; public names: `Gesture::OnPresent`, `CurrentHand`, `ZoneName`, `SetLogging` (had external
     linkage without a header declaration; now declared); `GestureZone` became `enum class Gesture::Zone`, `GestureHand`
     `Gesture::Hand` with `isTracked`, `hasEntered`, `isPalmUp`, `isGripped`; hysteresis and angle numbers as constants).
     Same bytes as b97 except the build id; nothing kept outside.
   - [x] `Pointer` (2026-10-12; public names: `Pointer::OnPresent`, `IsActive`, `SetEnabled`, `SetScreenCounts`,
     `SetTestTarget`; the phase became `enum class Phase`; step, corner push, margin and smoothing numbers as constants).
     Same bytes as b97 except the build id; nothing kept outside.
   - [x] `Sight` (2026-10-12; public names: `Sight::Flash`, `Draw`, `OnReset`, `SetEnabled`; flash, width and point-table
     numbers as constants). Same bytes as b97 except the build id; nothing kept outside.
   - [x] `Log` (2026-10-12; public names unchanged, `Log::Info`, ...; state `_file`, `_lock`, `_isDebugEnabled` and the
     helpers now inside `namespace Log`; kept-log count, name table and hash sizes as constants; output format unchanged).
     Same bytes as b97 except the build id; nothing kept outside.
   - [x] `Crash` (2026-10-12; public names: `Crash::Install`, `Heartbeat`; the no-frame watcher `WatchForHang`, handlers
     `OnFirstChance`, `OnUnhandled`; exception codes, stack, backtrace and watcher timings as constants; crash log format
     unchanged). Same bytes as b97 except the build id; nothing kept outside.
   - [x] `Lua` (2026-10-12; public names: `Lua::Run`, `Install`, `OnPresent`, `CurrentState`, `Execute`, `Register`,
     `ToString`; hook `ConsolePrintfHook`, original `_originalConsolePrintf`; statuses, wait steps and buffer sizes as
     constants). Same bytes as b97 except the build id; nothing kept outside.
   - [x] `Probe` (2026-10-12; public names: `Probe::Add`, `Report`, `LogStack` (was `Stack_Log`); the stub's callback
     `OnProbeHit`, the record `InstalledProbe`; scan, entry and stub sizes as constants). Same bytes as b97 except the
     build id; nothing kept outside.
   - [x] `Watch` (2026-10-12; public names: `Watch::NoteMainThread`, `Set`, `ClearAndReport`; the record `AddressWatch`,
     `Site` with `codeAddress` and `hitCount`; slot, site, hit, scan and line limits as constants). Same bytes as b97
     except the build id; nothing kept outside.
   - [x] `Main` (2026-10-12; public names: `Main::OpenModFile`, `ModDirectory` (was `ModDir`), `PatchPointer`; the frame
     timer is `Profiling::Scope`, `Add`, `Report` with `enum class Profiling::Stage` (was `ProfScope`, `kProf...`,
     `Prof_Report`), also in `Main.h`. `DllMain` and `Proxy_DirectInput8Create` (named in `dinput8.def`) keep their global
     names). Same bytes as b97 except the build id; nothing kept outside.
   - [x] `FpGuard.h` (2026-10-12; was `fp_guard.h`; header only, file renamed, `FpGuard` already in the style).

   The `Stereo` grab test code and the `Xr` simulated headset's state stay outside their anonymous namespaces (idren2023
   decided 2026-10-12 not to move them in).

Steps 1 to 3 are mechanical and cheap. Steps 4 and 5 need care and a game run each, because of the render-thread
and heap rules (our data never in the game's heap; render-thread state changed only between frames).
