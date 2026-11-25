# Engine reference: offsets, layouts, functions

Everything known about the game's code and data. Addresses are RVAs (offset from the exe base); the exe has
ASLR, so code must add the live base. How each fact was found is noted, because some are inferences.
Static findings not yet confirmed in a run (2026-10-15/17) are in `docs/research/*.md`; move each here once a run confirms it.

Target: Steam `Battlefront.exe`, 5,258,816 bytes, PE timestamp 2020-07-31,
SHA-256 `deca98fb348310e5e3b6dd74cbffb026a612b0410442c7787d4589b9d845b772`.

The exe has ASLR enabled, so record addresses as RVAs (offset from the exe base). Every address the mod uses also has a
byte signature (section "Signatures"), so another build can be searched.

| Name | RVA | Signature | Notes |
|---|---|---|---|
| .text | 0x1000 | | size 0x40AC05, encrypted on disk |

## Signatures (src/GameBuild.cpp, 2026-10-12)

Every address the mod uses, with the signature that finds it in another build (CODE-STYLE section 7, ARCHITECTURE
section 12). `??` = any byte (call targets, absolute addresses). Find: `Code` = site + offset, `Pointer` = the 32-bit
absolute operand at site + offset, `CallTarget` = target of the call at site + offset (through jump thunks),
`Relative` = another address + offset. Site = where the pattern matches on Steam. All 61 match exactly once in
`.text` (`python tools/signatures.py`, which reads the patterns from `GameBuild.cpp`; that file is the source, this
table a copy).

| Address | Steam RVA | Feature | Find | Site + offset | Pattern |
|---|---|---|---|---|---|
| Render | 0x295340 | stereo | Code | 0x295340 +0 | `53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC B8 00 00 00 A1 ?? ?? ?? ?? 33 C5 89 45 FC 56 57 E8 ?? ?? ?? ??` |
| SetTransform | 0x2913C0 | stereo | Code | 0x2913C0 +0 | `55 8B EC 8B 45 08 0F 10 00 0F 11 41 10 0F 10 40 10 0F 11 41 20 0F 10 40 20 0F 11 41 30 0F 10 40 30 0F 11 41 40 E8 ?? ?? ?? ??` |
| SetFov | 0x2912F0 | stereo | Code | 0x2912F0 +0 | `55 8B EC F3 0F 10 45 08 F3 0F 59 05 ?? ?? ?? ?? 56 51 8B F1 F3 0F 11 04 24 E8 ?? ?? ?? ?? D9 5D 08` |
| SetNearFar | 0x291400 | stereo | Code | 0x291400 +0 | `55 8B EC F3 0F 10 45 08 F3 0F 11 81 90 00 00 00 F3 0F 10 45 0C` |
| ViewCameras | 0x6ED3C0 | stereo | Pointer | 0x2954B7 +10 | `8D 85 70 FF FF FF 50 8B 0C 8D ?? ?? ?? ??` |
| ViewCount | 0x6ED3D4 | stereo | Pointer | 0x292C20 +5 | `55 8B EC 51 A1 ?? ?? ?? ?? 56 33 F6 85 C0` |
| ViewIs3d | 0x6ED400 | stereo | Pointer | 0x292C9B +2 | `C6 86 ?? ?? ?? ?? 01 E8 ?? ?? ?? ?? 66 0F 6E C0 F3 0F E6 C0` |
| SceneNearFarReturn | 0x2954FA | camera callers | Code | 0x2954EB +15 | `D9 1C 24 8B 0C 8D ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? D8 8D 68 FF FF FF` |
| FirstPersonNearFarReturn | 0x295724 | camera callers | Code | 0x295714 +16 | `D9 5C 24 04 C7 04 24 9A 99 99 3E E8 ?? ?? ?? ??` |
| SpawnScreenCameraReturn | 0x13EED5 | camera callers | Code | 0x13EECB +10 | `75 23 8B 4B 08 E8 ?? ?? ?? ?? 51 8B 4B 08` |
| ChaseCameraUpdate | 0x13EEA0 | camera callers | Code | 0x13EEA0 +0 | `53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 08 01 00 00 56 57 8B F9 83 7F 0C 00` |
| ChaseCameraCodeStart | 0x13EE00 | camera callers | Relative | ChaseCameraUpdate - 0xA0 | |
| ChaseCameraCodeEnd | 0x13F800 | camera callers | Relative | ChaseCameraUpdate + 0x960 | |
| Flush | 0x27C1D0 | limits | Code | 0x27C1D0 +0 | `55 8B EC 53 E8 ?? ?? ?? ?? 8B 5D 08 85 DB` |
| SortedQueue1800 | 0x63B9C4 | limits | Pointer | 0x47BD0 +10 | `6A 00 6A FF 68 08 07 00 00 B9 ?? ?? ?? ??` |
| SortedQueue1024 | 0x657AB8 | limits | Pointer | 0x47C90 +10 | `6A 00 6A FF 68 00 04 00 00 B9 ?? ?? ?? ??` |
| SortedQueue2312 | 0x657AC8 | limits | Pointer | 0x47C00 +10 | `6A 00 6A FF 68 08 09 00 00 B9 ?? ?? ?? ??` |
| ListQueue800 | 0x657AD8 | limits | Pointer | 0x27D017 +3 | `7F 11 B9 ?? ?? ?? ?? F7 D6 8D 45 D0 56 50` |
| ListQueue512 | 0x63B9B0 | limits | Pointer | 0x27C32C +5 | `84 C0 74 30 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 84 C0 74 22` |
| ItemCount | 0x639E10 | limits | Pointer | 0x27C33E +2 | `C7 05 ?? ?? ?? ?? 00 00 00 00 85 DB 78 14` |
| MatrixCount | 0x639E0C | limits | Pointer | 0x275390 +9 | `55 8B EC 8B 55 08 56 8B 35 ?? ?? ?? ?? 8B C6` |
| RecordCount | 0x639E14 | limits | Pointer | 0x2753D0 +2 | `8B 15 ?? ?? ?? ?? 8D 04 D5 00 00 00 00 8D 4A 01` |
| Items | 0x688010 | limits | Pointer | 0x27C9F0 +11 | `8B 45 DC 43 C1 E7 04 8B C8 81 C7 ?? ?? ?? ??` |
| Matrices | 0x657BF0 | limits | Pointer | 0x2753A2 +1 | `05 ?? ?? ?? ?? 8D 0C 16 81 F9 F6 0B 00 00` |
| Records | 0x63B9D8 | limits | Pointer | 0x2753E2 +13 | `81 F9 00 08 00 00 1B C9 23 CA 8D 04 C5 ?? ?? ?? ??` |
| SubmitItemLimit | 0x27C9D8 | limits | Code | 0x27C9D0 +8 | `8B 75 0C 89 75 C0 81 FB 28 12 00 00` |
| SubmitItemTable | 0x27C9FB | limits | Code | 0x27C9F0 +11 | `8B 45 DC 43 C1 E7 04 8B C8 81 C7 ?? ?? ?? ??` |
| MatrixBlockTable | 0x2753A3 | limits | Code | 0x2753A2 +1 | `05 ?? ?? ?? ?? 8D 0C 16 81 F9 F6 0B 00 00` |
| MatrixBlockLimit | 0x2753AC | limits | Code | 0x2753AA +2 | `81 F9 F6 0B 00 00 1B C9 23 CE` |
| MatrixSingleTable | 0x275421 | limits | Code | 0x27541F +2 | `81 C6 ?? ?? ?? ?? 8D 41 01 3D F6 0B 00 00` |
| MatrixSingleLimit | 0x275429 | limits | Code | 0x275425 +4 | `8D 41 01 3D F6 0B 00 00 8B C6` |
| MatrixInPoolStart | 0x278E77 | limits | Code | 0x278E76 +1 | `3D ?? ?? ?? ?? 72 0B 3D ?? ?? ?? ?? 73 04 B0 01 5D C3` |
| MatrixInPoolEnd | 0x278E7E | limits | Code | 0x278E7D +1 | `3D ?? ?? ?? ?? 73 04 B0 01 5D C3 32 C0 5D` |
| RecordAllocatorLimit | 0x2753E4 | limits | Code | 0x2753E2 +2 | `81 F9 00 08 00 00 1B C9 23 CA` |
| RecordAllocatorTable | 0x2753EF | limits | Code | 0x2753E2 +13 | `81 F9 00 08 00 00 1B C9 23 CA 8D 04 C5 ?? ?? ?? ??` |
| RecordInPoolFirst | 0x278260 | limits | CallTarget | 0x29D55F +4 | `0F 11 45 8C E8 ?? ?? ?? ?? 8D 85 68 FF FF FF` |
| RecordInPoolFirstStart | 0x278269 | limits | Relative | RecordInPoolFirst + 0x9 | |
| RecordInPoolFirstEnd | 0x278271 | limits | Relative | RecordInPoolFirst + 0x11 | |
| RecordInPoolSecond | 0x278770 | limits | CallTarget | 0x28872E +0 | `E8 ?? ?? ?? ?? 8B 85 98 FD FF FF C1 E8 07` |
| RecordInPoolSecondStart | 0x278779 | limits | Relative | RecordInPoolSecond + 0x9 | |
| RecordInPoolSecondEnd | 0x278781 | limits | Relative | RecordInPoolSecond + 0x11 | |
| AimerSet | 0x8CE90 | aim | Code | 0x8CE90 +0 | `55 8B EC 8B 45 08 F3 0F 7E 00 66 0F D6 41 7C` |
| FirstPersonRender | 0x130320 | first person model | Code | 0x130320 +0 | `55 8B EC 56 8B F1 E8 ?? ?? ?? ?? 83 F8 01` |
| FirstPersonRenderables | 0x4E3008 | first person model, muzzle | Pointer | 0x12FA29 +3 | `8B 0C B5 ?? ?? ?? ?? 85 C9 74 5C 8B 01 53` |
| HashName | 0x3007F0 | muzzle | Code | 0x3007F0 +0 | `55 8B EC 56 57 8B 7D 08 8B CF 8D 51 01` |
| SetStrafe | 0xDA0A0 | analog move | Code | 0xDA0A0 +0 | `55 8B EC F3 0F 10 45 08 F3 0F 11 41 58` |
| SetMove | 0xDA0C0 | analog move | Code | 0xDA0C0 +0 | `55 8B EC F3 0F 10 45 08 F3 0F 11 41 54` |
| SetStrafeReturn | 0x1F750C | analog move | Code | 0x1F7507 +5 | `E8 ?? ?? ?? ?? F3 0F 10 45 C4 51 8B 4F 04` |
| SetMoveReturn | 0x1F751F | analog move | Code | 0x1F751A +5 | `E8 ?? ?? ?? ?? F3 0F 10 45 DC 51 8B 4F 04` |
| WeaponFire | 0x2461A0 | feedback | Code | 0x2461A0 +0 | `53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC A8 00 00 00 56 57 8B F9 E8 ?? ?? ?? ?? 8B 4F 6C` |
| RegisterHit | 0x9BBE0 | feedback | Code | 0x9BBE0 +0 | `55 8B EC 56 FF 75 08 8B F1 8B 8E A4 00 00 00` |
| HitValue | 0x9B710 | feedback | Code | 0x9B710 +0 | `55 8B EC 8B 45 08 83 EC 08 F3 0F 10 84 81 D8 00 00 00` |
| ReticuleUpdate | 0x208000 | feedback | Code | 0x208000 +0 | `55 8B EC 81 EC 88 00 00 00 53 8B D9 56` |
| ReticuleUpdateEnd | 0x208C70 | feedback | Relative | ReticuleUpdate + 0xC70 | |
| LuaDoBuffer | 0x264120 | lua | Code | 0x264120 +0 | `55 8B EC FF 75 14 FF 75 10 FF 75 0C FF 75 08 E8 ?? ?? ?? ?? 83 C4 10 85 C0` |
| LuaRegister | 0x18AF60 | lua | Code | 0x18AF60 +0 | `55 8B EC 6A 00 FF 75 10 FF 75 08` |
| LuaToString | 0xCB560 | lua | CallTarget | 0xCB073 +10 | `51 53 56 8B 75 08 57 6A 01 56 E8 ?? ?? ?? ?? 6A 02 56 8B D8 E8 ?? ?? ?? ?? 6A 03` |
| MissionScript | 0x4E6DCC | lua | Pointer | 0x141EF1 +14 | `83 C4 08 80 3D ?? ?? ?? ?? 00 74 6A 8B 0D ?? ?? ?? ??` |
| ShellScript | 0x62DE8C | lua | Pointer | 0x20E3DA +6 | `33 C0 56 8B C8 A3 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B F0 56` |
| ConsolePrintf | 0x61440 | console output | Code | 0x61440 +0 | `55 8B EC 81 EC 00 08 00 00 8D 45 10` |
| ConsolePuts | 0x27FF50 | console output | Code | 0x27FF50 +0 | `55 8B EC 83 EC 08 53 57 8B 7D 08 8B D9 89 5D FC` |

## Vtables (from RTTI, image dump 2026-10-05)

Full list: run `tools/rtti.py re/battlefront-image.bin <base>` (898 vtables). Method RVAs below 0x20000 are incremental-link jump thunks (`jmp rel32`); follow the jump for the real function.

| Class | Vtable RVA | Object offset | Methods | First method RVAs |
|---|---|---|---|---|
| CameraManager | 0x410bf8 | 0x0 | 5 | 104b5 b0f0 1c12f 16d6f f8df |
| GameCamera | 0x41811c | 0x0 | 6 | 377e f083 1180b 12134 11e0f 13124 |
| ChaseCamera | 0x418140 | 0x0 | 6 | 1f28f 56b9 11e96 16f3b 13110 117d4 |
| FreeCamera | 0x418188 | 0x0 | 6 | a588 1c0b2 1bd79 12134 3ee0 19ce0 |
| DeathCamera | 0x418164 | 0x0 | 6 | 68f2 2671 d328 12134 15217 13124 |
| CameraInterpolator | 0x410bc8 | 0x0 | 5 | 7734 a7f9 1c12f 16d6f f8df |
| RedCamera | 0x42c820 | 0x0 | 1 | 2c43 |
| Aimer | 0x41086c | 0x0 | 5 | 7ce3 179e0 1c12f 16d6f f8df |
| Controller | 0x412700 | 0x0 | 12 | c176 12c38 1b301 ae02 1c34b 318e |
| PlayerController | 0x422b6c | 0x0 | 12 | 5862 2a8b 73f6 136f6 1c34b 53e9 |
| Controllable | 0x4125ec | 0x0 | 39 | 8a99 144e3 114eb a1dc 10e1a 7919 |
| EntitySoldier | 0x416454 | 0x0 | 64 | 5d26 6a41 d7ba 9818 1beaf ea48 |
| EntitySoldier | 0x41658c | 0x8 | 8 | 138d1 1ce09 805d 8922 d6bb aa97 |
| EntitySoldier | 0x4165b8 | 0xb4 | 1 | f038 |
| EntitySoldier | 0x4165c4 | 0xf4 | 18 | 14b00 13705 1b383 1bff9 9241 180b1 |
| EntitySoldier | 0x416620 | 0x17c | 9 | 14209 a709 e642 4345 d099 b686 |
| EntitySoldier | 0x416650 | 0x250 | 39 | 1bbd5 1a627 16667 16e32 1679d 7a63 |
| EntitySoldier | 0x416710 | 0x434 | 5 | ae1b 21a8 1c12f 16d6f f8df |
| EntitySoldier | 0x41672c | 0x44c | 14 | 18200 138f4 d373 1d89f 17995 c00e |
| EntitySoldier | 0x416774 | 0x454 | 3 | 175ee c40f 10d2a |
| Weapon | 0x426fcc | 0x0 | 46 | 1ca58 206d 1c12f 16d6f f8df 3bcf |
| WeaponCannon | 0x4272fc | 0x0 | 48 | 17cfb 6c58 1c12f 16d6f f8df 17ee |
| FirstPersonRenderable | 0x417a60 | 0x0 | 7 | 18994 371a 14a01 8abc 4def 120da |

## Object layouts found by the survey (2026-10-05)

Offsets are from the start of the object. "M44" = 16 floats, row-major, rows are right / up / forward(back) / position.

### CameraManager (vtable RVA 0x410bf8), heap object, one per match
| Offset | What |
|---|---|
| +0x24..+0x48 | pointers to its embedded cameras (Chase, Red, Free, Death; two sets) |
| +0x260 | embedded RedCamera (the camera the renderer uses) |
| +0x390 | embedded ChaseCamera |
| +0x39c | = ChaseCamera+0xC: pointer to the followed EntitySoldier (+0x44c subobject) |
| +0x480 | embedded FreeCamera; +0x500 embedded DeathCamera; a second set follows from about +0x580 |

### RedCamera (vtable RVA 0x42c820)
| Offset | What |
|---|---|
| +0x10 | M44 camera-to-world (eye position in row 3) |
| +0x50 | M44 view (inverse of +0x10) |
| +0x90 | near plane (0.5) |
| +0x94 | far plane (280 on Bespin) |
| +0x98, +0x9C | tan(half FOV) horizontal, vertical (0.7, 0.4375) |
| +0xA0 | zoom factor (1.0; 2.5 when zoomed; Rebel Marksman sniper: first Z 1.23, second Z a stronger level, third Z off, run93) |
| +0xA4, +0xA8 | tan(half FOV) after zoom |
| +0xAC.. | derived frustum values |

### ChaseCamera (vtable RVA 0x418140)
| Offset | What |
|---|---|
| +0x4 | pointer to CameraManager |
| +0xC | pointer to target EntitySoldier+0x44c |
| +0x10 | M44 camera transform |

### PlayerController (vtable RVA 0x422b6c)
| Offset | What |
|---|---|
| +0x4 | pointer to the controlled EntitySoldier+0x250 (Controllable base); null while dead / on the spawn screen |

### EntitySoldier (main vtable RVA 0x416454; bases at +0x8, +0xb4, +0xf4, +0x17c, +0x250 Controllable, +0x434, +0x44c, +0x454)
| Offset | What |
|---|---|
| +0x1B0 | M44 world transform, position at +0x1E0 (feet) |
| +0xB00 | M44 second world transform (position +0xB30) |
| +0x8F0 | embedded Aimer (0x260 bytes) |
| +0xA60 | stance: 0 stand, 1 crouch, 2 prone |
| +0xA64 | action: 0 none, 1 crouch, 2 prone, 4 jump, 10 roll |
| +0xB50 | end of object (next pooled soldier starts here) |

### Aimer (vtable RVA 0x41086c), embedded at EntitySoldier+0x8F0
| Offset | What |
|---|---|
| +0x18 | pointer to another Aimer (list link) |
| +0x20 | pointer to owner EntitySoldier+0x250 |
| +0x48 | aim direction, unit vector (equals camera forward in 1st person) |
| +0x70, +0x7C | two positions 0.5-0.9 from the eye (fire origin candidates) |
| +0x128 | pointer to current weapon |
| +0x140..+0x14C | (= soldier+0xA30..+0xA3C: the first 4 entries of the soldier's weapon array, not Aimer fields; see "Soldier weapons") |
| +0x210 | M44 barrel/mount transform (forward row +0x230, position +0x240) |

### Weapon (WeaponCannon etc., 0x1B0 bytes, pooled)
| Offset | What |
|---|---|
| +0x20 | M44 weapon transform (position +0x50, about 1.0-1.2 from the eye) |
| +0x110 | pointer to owner EntitySoldier |
| +0x218 | pointer to owner EntitySoldier+0x250 |
| +0x21C | pointer to the Aimer |
| +0x210, +0x214 | pointers to the weapon class objects |

### Level texture loading (2026-11-24, runs 893-895, static reading)
- 0x283110 reads a `tex_` chunk: its INFO list of formats goes to 0x2832c0, which picks the format with
  `d3dx9_43!D3DXCheckTextureRequirements(device, 0, 0, 0, usage 0, &format, D3DPOOL_MANAGED)` (import slot
  RVA 0x21bc65c, thunk 0x3ec6ee); then 0x283810 reads the matching FMT_ chunk and makes the texture with
  CreateTexture (vtable +0x5c, pool MANAGED, call returns at +0x283a2a), cube (+0x64) or volume (+0x60).
- On a Direct3D 9Ex device that check fails (no managed pool), no format is chosen and the texture is skipped
  silently: the world draws white. b671 asks it for the default pool on 9Ex.
- The loader fills the textures with IDirect3DTexture9::LockRect on the loading thread = the render thread (run 899:
  4339 locks for 908 textures on Naboo, none during play).

### Direct3D per-frame facts
- Shader constants: c2-c5 view-projection (rows), c8-c10 another camera matrix, c15.. world matrices (3x4), c48.. bone/world data.
- Projections seen in gameplay at 16:10: x 1.428, y 2.285 (= 1/0.7, 1/0.4375); near -0.5 main, -2.0 pre-pass, -0.3 first-person weapon. HUD projection 1.732 / 2.309, near 1.0, identity view.

## Functions (RVA), found with memory watches (runs 6-7) and tools/sym.py

Names are mine, from what the code touches. "vt" gives the vtable slot where known.

| RVA | Name | Evidence |
|---|---|---|
| 0x99680 | CameraManager::Update (vt CameraManager[1]) | once per frame; contains "PlayerPos"/"CameraPos" strings; calls ChaseCamera::Update |
| 0x13eea0 | ChaseCamera::Update (vt ChaseCamera[2]) | calls RedCamera::SetTransform and SetFov once per frame |
| 0x2913c0 | RedCamera::SetTransform(matrix*) | writes +0x10 (15 call sites); calls 0x2920e0 |
| 0x2920e0 | RedCamera::UpdateView | writes the view matrix +0x50 |
| 0x2912f0 | RedCamera::SetFov | writes +0x98 |
| 0x118030 | EntitySoldier(+0x44c)[1] | writes ChaseCamera+0x10: the soldier computes the chase camera matrix |
| 0x295340 | scene render (large, multi-pass) | reads camera position; return sites 0x29554a-0x295d1a |
| 0x291400, 0x291600, 0x291700 | camera/frustum setup inside rendering | 0x291700 reads +0xa4 at many sites (frustum planes) |
| 0x27b190 | renderer set-camera | reads the view matrix +0x50 |
| 0x8ce90 | `Aimer::Set(const float origin[3], const float dir[3])`, thiscall, ret 8; sets flag +0x29; player call returns to 0x11D6D9 (other call sites 0x11D28F, 0x11D66B) | the only writer of Aimer+0x48, +0x70, +0x7c; called from 0x11cfd0 in the soldier update |
| 0x118f30 | EntitySoldier(+0x434)[1] | writes Aimer+0x210 barrel matrix from the aim direction |
| 0x2461a0 | weapon fire (called via WeaponCannon[28] 0x246fc0) | reads Aimer+0x7c (origin) and, through 0x8c620, Aimer+0x48 (direction) when a shot is fired |
| 0x243000 | Weapon[23] | writes the weapon transform +0x20; reads the aim direction |
| 0x23c720, 0x23cb20 | entity update dispatch | on the call chain of every soldier update |
| 0x1162e0 | EntitySoldier[15] | writes the soldier position |
| 0x2912f0 | RedCamera::SetFov(float fovRadians, float tanY_over_tanX) | thiscall, ret 8; one caller (0x13f54e, chase camera, every frame once the player has spawned); tanX = tan(fov/2) -> +0x98, tanY -> +0x9C |
| 0x291400 | RedCamera::SetNearFar(near, far) | called inside Render for the first-person weapon pass (near 0.3) and restored |
| 0x27c9a0 | submit draw item (93 callers) | fills the item table and pushes into one of five queues |
| 0x27c1d0 | flush queues (mask) | bit 0 opaque queue, 1, 2 other sorted queues, 3 transparent list (0x657AD8), 4 list 0x63B9B0 |
| 0x274b20 | draw one item of a list queue | crash site +0x276763 when the transparent queue overran |
| 0x2755f0 | begin view | resets the item, matrix and record counts |
| 0x2842b0 / 0x2842d0 | game allocator / free | heap is emptied on level load: never keep our data there |

### Render view list (used by Render 0x295340, one entry per split-screen view)
| RVA | What |
|---|---|
| 0x6ED3C0 | RedCamera* per view (equals CameraManager+0x260 for view 0) |
| 0x6ED3D0 | index of the view being drawn |
| 0x6ED3D4 | number of views |
| 0x6ED400 | per view: byte, non-zero when the 3D scene is drawn (zero in menus) |

### Renderer tables (per-frame draw collection; see `src/Limits.cpp`, which replaces all of them)
| RVA | What | Game's limit | Behaviour at the limit |
|---|---|---|---|
| 0x688010 | draw items, 48 bytes each; count at 0x639E10 | 4648 | further items are dropped |
| 0x657BF0 | matrix pool, 64 bytes each; count at 0x639E0C; allocators 0x275410 (one) and 0x275390 (block) | 3062 | wraps to the start; the block allocator then returns a block that runs past the end (bug) |
| 0x63B9D8 | light/state records, 56 bytes each; count at 0x639E14; allocator 0x2753D0 | 2048 | wraps to the start |
| 0x63B9C4, 0x657AB8, 0x657AC8 | sorted queues (binary heaps): {count, capacity, entries*, owns}; entry = {key, item*}, entry 0 is a guard with key 0xFFFFFFFF | 1800, 1024, 2312 | accepts one entry more than the array holds (bug) |
| 0x657AD8, 0x63B9B0 | list queues: {nodes*, free*, head*, count, capacity}; node = {item*, key, next*} | 800, 512 | the 800 one (transparent objects) overran with a wide view: crash at 0x276763 or hang |
| 0x639E08 | flush threshold for the first sorted queue (set by 0x27D670) | | |

Draw item (48 bytes): +0x0 and +0x4 object pointers (the second has the vtable whose slot 15 the flush calls),
+0x8 segment index, +0xC matrix pointer, +0x20 flags, +0x24 light record pointer, +0x28 pass info.

## Frame and rendering facts (from Direct3D traces and probes)

- Model draws are queued and rendered later in the pass: a first-person model's draw call returns before anything is
  drawn (runs 344-346: depth written before or a picture pasted right after the cockpit's draw call changed nothing;
  depth written after our own later draw did hide it).
- Model lighting (runs 356-357, `shaderdump`): lit models use vertex shaders; per lit draw one write of c11-c21: c15-c17
  the model's matrix (3x4 rows), c18 a colour scale (0.549 on Naboo's cockpits), c19 the ambient colour (Naboo
  0.39 0.47 0.39), c20 the sun's colour (0.82 0.78 0.75), c21 the direction the sunlight travels in the world
  (-0.63 -0.77 0.11: the same at any head yaw and hull heading); unlit draws write c11-c19 with c19 white. Per map:
  each world defines its own light. The look matches texture x c18 x (c19 + c20 x N.L).

- Direct3D 9, hardware vertex processing, 1920x1200 exclusive fullscreen, X8R8G8B8 + D24S8, no MSAA. The game
  creates 12 probe devices at start; the real one is the one that presents.
- Frame call tree: per-frame function 0x143440 -> `Update(float dt, int, int)` 0x226BC0 (twice) and `Render()`
  0x295340 (once, cdecl, no arguments). `Render()` ends by calling Present through 0x275D10; the game's device
  pointer is the global at 0x639638. `CameraManager::Update` 0x99680 -> `ChaseCamera::Update` 0x13EEA0 ->
  `RedCamera::SetTransform` runs inside Update.
- `Render()` loops over the view list (table above). Per view with the 3D flag set: begin view (0x2755F0), scene
  traversal (0x297AC0 and 0x2B2660 take the camera; that they are what submits the draw items is inferred), then queue flushes with masks 1, 3, 4,
  8, 0x10, then the first-person pass, then 2D. Without the flag (menus) only the 2D part runs.
- Passes of a gameplay frame: (1) pre-pass into a full-size offscreen texture, near plane 2.0; (2) main scene to
  the back buffer, near 0.5, with StretchRect copies for effects; (3) depth clear, then the first-person weapon,
  near 0.3; (4) EndScene; (5) HUD as its own scene with identity view and the fixed projection 1.732 / 2.309.
  The layout is the same in first and third person and when zoomed.
- Everything is drawn with vertex shaders. The view-projection goes to shader constants c2-c5 (written about 11
  times a frame); `SetTransform` VIEW and PROJECTION are also called, which is how the mod recognises the HUD pass.
- Projections at 16:10 without the mod: x 1.428, y 2.285 (= 1/0.7, 1/0.4375): the horizontal field of view is
  fixed at 70 degrees and the vertical one follows the aspect ratio. Third person uses tan 0.637.
- Calling `Render()` twice in one frame works without visible artifacts.
- Eye height above the feet: 1.72 standing, 1.22 crouched, 0.63 prone. The third-person camera sits about 4.2
  behind the soldier.
- The camera is set from chase-camera code both on the spawn screen (caller +0x13EED5) and in play (+0x13F4C2).
  `SetFov` is only called in play (inferred: on the spawn screen the camera never took a forced field of view).
- The game's allocator (0x2842B0 / 0x2842D0, heap handle at 0x6E55BC) is emptied when a level loads. Inferred:
  a queue array allocated there in the menu had lost its guard entry by the first frame of the match (b20).
- Uncapped frame rate: 400 to 1100 fps on the test PC.


## ODF property parsers (found by name hash, 2026-10-06)
Method: the BF1 property hash list (source file, line, name hash; from secretsociety.com). Image offsets of the 4-byte hash immediate
inside the `case` that parses the property; the code that follows stores the value into the class. Not yet
disassembled.

| Property | Hash | Steam image | SPTest VA | Source file |
|---|---|---|---|---|
| FirstPersonFOV | 0x4785d6b7 | +0xc3a47 | 0x484f29 | Controllable.cpp |
| ThirdPersonFOV | 0x20728c12 | +0xc38df | 0x484e61 | Controllable.cpp |
| EyePointOffset | 0x41568c97 | +0x22834f | 0x4351dd | Trackable.cpp |
| TrackOffset | 0xfd3d9507 | +0x2284ee | 0x4352c3 | Trackable.cpp |
| TiltValue | 0x359d5227 | +0x228363 | 0x4351d5 | Trackable.cpp |
| AimerPitchLimits | 0x2d6487bd | +0x8c7ed | 0x49eca8 | Aimer.cpp |
| AimerNodeName | 0xc182abc2 | +0x8ca0c | 0x49ef17 | Aimer.cpp |
| FirePointName | 0x4274fc96 | +0x8c94b, +0x2439f5 | 0x49ee1d, 0x4ab9e4 | Aimer.cpp, Weapon.cpp |
| CAMERASECTION | 0xbaca6bf1 | +0xe1dd2, +0x1172a8 | 0x448367, 0x46ab0a | EntityDroideka.cpp, EntitySoldier.cpp (order not checked) |
| ZoomMin | 0x1881b1a6 | +0x2436b4 | 0x4ab77e | Weapon.cpp |
| PitchLimits | 0x3403b139 | 6 places | 6 places | soldier, droid, droideka, hover, walker, turret |

## Lua (found 2026-10-06, used by `Lua.cpp`)
| RVA | What | How found |
|---|---|---|
| 0x264120 | `int lua_dobuffer(lua_State*, const char* text, size_t, const char* name)`, cdecl | called by the base library's `dostring` (0x2662B0, from the function table at 0x429D38) |
| 0x2632E0 | `lua_setglobal(lua_State*, const char* name)` | sets "gFinalBuild", "gDemoBuild" |
| 0x18A880 | pushes a number/bool for the next setglobal | same places |
| 0x4E6DCC | pointer to the mission's script object (`[obj+0]` = lua_State); null in the menus | 0x141F70 |
| 0x62DE8C | pointer to the menu's script object | 0x20E47E |
| 0x18AF60 | `Register(lua_State*, const char* name, lua_CFunction)`, cdecl: makes a C function a Lua global (pushes the closure, sets the global). `lua_CFunction` = `int __cdecl f(lua_State*)`, returns the number of results | called in a loop at 0x17CDE0 over the table of `ScriptCB_*` name/function pairs at 0x479018 (124 pairs there; other tables exist) |
| 0xCB560 | `const char* ToString(lua_State*, int index)`, cdecl (wraps 0x262260 with a third argument 0) | first calls of `ScriptCB_PushScreen` (0x17A350) and `ScriptCB_DoConsoleCmd` (0x17B2E0); 0x175680 = argument count, 0x1757B0 = is-string check |
| 0x18A100 | script object method: run a named script from the loaded levels ("game_interface", "shell_interface") | |
| 0x4776CC | byte: final build flag (when non-zero the game sets Lua `gFinalBuild` = 1) | 0x20E45F |
| 0x61440 | `ConsolePrintf(Console*, const char* fmt, ...)`, cdecl, 37 callers; console is null in retail | crash of run19 |
| 0x27FF50 | `Console::Puts(const char*)`, thiscall | |
| 0x160F40 | returns Aimer+0x7C (fire origin) | used by the fire code 0x2461A0 for the bolt's start |
| 0x8C620 | Aimer: builds the aim matrix (out pointer) from +0x48 direction | bolt direction before spread |
| 0x4E3008 | table of `FirstPersonRenderable*`, one per view | 0x131640 |
| 0x131660 | `FirstPersonRenderable::Update` dispatch by state (+0x160C): 0 -> 0x131890 (on foot), 0x131EA0 / 0x1328C0 (hp_camera) | |
| 0x130320 | `FirstPersonRenderable::Render(flags)`, vtable slot 4; arms matrix +0x1570, weapon matrix +0x15B0, arms model +0x15FC, weapon +0x1610, pose +0x830 | |
| 0x243000 | `Weapon::Render(matrix*, light*, flags, firstPerson)`, Weapon vtable slot 23 (+0x5C): copies the matrix to weapon+0x20, draws weapon+0xD0 (first-person model, if firstPerson and set) else +0xCC (third-person model) through the model's vtable slot 1 | b131 |
| - | Model draw = model vtable slot 1 (+4), thiscall `(matrix*, pose*, light*, flags, 0)`: world matrix 4x4 row vectors read during the call (the game passes a stack copy); pose 0 for weapons; light = global 0x6D939C (GameBuild ModelLight); flags = first-person draw's flags \| 0x80001. Called by the mod for extra weapon models (run137: works) | b131, run137 |
| - | Soldier weapons (sub-agent, disassembly, 2026-10-12; used by Holster.cpp b135+, run140-run142 confirm the layout): flat array of up to 8 Weapon* at soldier+0xA30 (Controllable+0x7E0), both channels; current index per channel at soldier+0xA50 (ch 0) / +0xA54 (ch 1), -1 none; soldier+0xA58 = channel the Aimer holds, +0xA5C its index (Aimer::SetWeapon 0x8CEE0 follows next frame). Channel per weapon from the soldier class ODF (EntitySoldierClass WeaponName +0x166C+4i, WeaponAmmo +0x168C+4i, WeaponChannel +0x16AC+4i, count +0x16CC); at run time weapon+0x70 = pointer to its channel's trigger Controllable+0x38+2*ch | 0x11ABB8, 0x10A7xx |
| 0xC3540 | `Controllable::RequestWeapon(int index)` thiscall: sets Controllable+0x12C, applied in Controllable::Update 0xC4510 (deselect old of that channel, vtbl+0x50; set index; select, vtbl+0x4C). **Did not switch the player's primary weapon (run140)**; worked for the secondary channel (run140-run142). Player next weapon: Controllable+0x64+4ch = +-1 (0xC3FF0); mouse wheel up = next primary (used by Holster) | 0xC3540 |
| - | Weapon readiness: +0x8C clip fraction (float, 1 full; rounds = int(WeaponClass+0x54 RoundsPerClip * it)), +0x84 reserve clips (FLT_MAX unlimited), +0xB2 selected, +0xB4 state (0 ready, 1-3 firing, 4 charging, 5 reloading, 8 empty), +0xB8 / +0xBC timer elapsed / duration; vtbl slot 42 0x241EE0 out of ammo, 44 clip empty, 45 timer done; Weapon::Update slot 1 0x244C80. WeaponClass +0x68 ReloadTime; WeaponCannonClass +0x1D4 ShotDelay. Holster's "ready" = state 0 and a round in the clip: a thrown grenade's place refilled after about 6 s (run140) | sub-agent, run140 |
| - | Soldier+0x830 = pointer to its EntitySoldierClass (RTTI name checked, run143). No ODF name strings inside the weapon or soldier classes (run143 probe): unit / weapon names must be matched by hash or by other fields. Weapon types seen (RTTI): Rebel pilot = 2 x WeaponCannon (ch 0), WeaponRepair (fusion cutter, no clip: RoundsPerClip * clip never >= 1), WeaponDispenser (ch 1); scout trooper's 2 secondaries share a model. ODF ClassLabel -> type: grenade (thermal / haywire detonators), dispenser (mines, remote charges, powerups), repair (fusion cutter), remote (recon droid), destruct (recon droid self-destruct), launcher / cannon (SBD wrist rocket / blaster) | run143 |
| - | Triggers: Controllable+0x38 primary, +0x3A secondary (2 bytes, frame count, >0 held; Trigger::Set 0xC4270 from PlayerController::Update 0x1F7150); the soldier update 0x118F30 moves the Aimer to the channel whose trigger is held | sub-agent |
| 0x3007F0 | `unsigned HashName(const char*)`, cdecl (case-insensitive): CRC-32 MSB-first over the lowercased name, poly 0x04C11DB7, init and final ~0 (table 0x9CE300; checked on all_1st_weap_inf_sniperrifle 0x01B41F2C, 2026-11-01) | WeaponClass property parser |
| 0x2882B0 | `unsigned Model::FindNode(unsigned hash, float outPos[3])`, thiscall: position relative to the parent node; returns the parent's hash, 0 = not found; node list at model+0xC0 | same |
| - | Runtime models keep only the root node (hash = HashName of the model name, e.g. `all_1st_weap_inf_sniperrifle` 0x01B41F2C) and hardpoints: the stock sniper rifle has 2 nodes (root, hp_fire), the pistol 3; msh geometry nodes such as SCOPE_LENS are gone (run110) | run110 |
| 0x243500 | `WeaponClass` property parser (vtable slot 6): class+0x3C third-person model, +0x40 first-person model, +0x24 fire point offset of the third-person model | |
| - | Weapon: +0x64 its WeaponClass, +0x6C its Aimer; `WeaponCannon` vtable slot 28 (0x246FC0) leads to the fire code | 0x2461A0 |
| - | FirstPersonRenderable+0x1610 = the soldier's Controllable part (vtable 0x416650), not a weapon; its virtual +0x90 / +0x34 return the first-person weapon model to draw | run29 dump |

### Clip distances and field of view (b59-b64)
| RVA | What |
|---|---|
| 0x291400 | `RedCamera::SetNearFar(float near, float far)` thiscall: camera +0x90 near, +0x94 far, then rebuilds |
| 0x2912F0 | `RedCamera::SetFov(float fov, float heightOverWidth)`: camera +0x98, +0x9C tan of half angles |
| 0x291250 | `RedCamera::GetFov()`: 2 * atan(camera +0x98) |
| 0xD8290 | `lerp(a, b, t)` cdecl |
| 0x2954F5 | call in Render(): SetNearFar for the scene pass |
| 0x29571B | the constant 0.3f: near distance of the pass that draws the first-person model (call at 0x29571F); its far is 165.2 m (run 858). Stereo sets 1 mm while VR runs (b654) |
| 0x295CB4 | the constant 2.0f: near distance of another pass (sky) |

### Player movement input (b68)
| RVA | What |
|---|---|
| 0xDA0A0 | `Controllable::SetStrafe(float)` thiscall: Controllable +0x58 (soldier +0x2A8), -1..1, D = +1 |
| 0xDA0C0 | `Controllable::SetMove(float)`: Controllable +0x54 (soldier +0x2A4), -1..1, W = +1 |
| 0xDA0E0 | a third setter of the same shape: Controllable +0x5C (not identified) |
| 0x1F74E0.. | PlayerController code that calls them every frame (return addresses 0x1F750C strafe, 0x1F751F move) |
| soldier +0x254 | position x, y, z (also +0x260, +0x330) |
| camera +0xA0 | zoom factor, 1 = not zoomed |

### Reticule and hit indicator (b83-b85, used by `Feedback.cpp`)
| RVA | What | How found |
|---|---|---|
| vtable 0x423800 | `ReticuleDisplay` (5 slots: 0x207B80, 0x208000 update, 0x226910, 0x226A30, 0x7CB60) | RTTI |
| 0x207E10 | sets up the hit indicator: the element at `ReticuleDisplay+0x700`, texture "hit_indicator" (string at 0x423818) | reference to the string |
| 0x208000 | `ReticuleDisplay` update (3184 bytes). At 0x2088FC: `obj = [ebp-0x24]`; `0x9B710(obj, 1)` -> `[ebp-0x24]`, `0x9B710(obj, 0)` -> `[ebp-0x2C]`, `0x9B710(obj, -1)` -> `[ebp-0x10]`; all zero: `0x7B540(this+0x700, 0)` at 0x208967 (hide); else `0x7B540(this+0x700, 1)` at 0x208B83 (return address 0x208B88) and a colour from the three (kind -1 red, 1 green, 0 blue, alpha = the largest) | disassembly; runs 77-79 |
| 0x9B710 | `float f(int kind)`, thiscall: `[this + kind*4 + 0xD8]` scaled; the fading strength of the last hit of that kind | |
| 0x7B540 | interface element show/hide `(bool)`, thiscall, many callers | |
| 0x2461A0 | weapon fire: `this` (ecx) is the weapon; equals soldier Aimer +0x128 for the player's shots | hook log, run77 |
| 0x9BBE0 | `RegisterHit(target)`, thiscall on the shooter's hit object; called from 0xC765E for every character's hits. Writes the strength to +0xD4 (enemy), +0xD8 (neither), +0xDC (friend) | b86, hook log |

### Scope reticle, spawn screen, jetpack gauge (b88-b92)
- Zoomed reticle position: the game camera's forward axis (no drop while zoomed: run99, within 0.001 rad; the
  0.0351 rad of runs 83/84 was wrong) projected through the drawing camera with its zoomed tanY (+0xA4);
  horizontally always the centre. Unzoomed it sits at v 0.5275. Size 79 x 73 px at 1920 x 1200.
- Rebel Marksman zoom: level 1 factor 2.00 (tan 0.689 x 0.641 with the headset's fov), level 2 factor 8.00
  (tan 0.172 x 0.160); the factor ramps over about 10 frames (it reads 1.23 early in the ramp).
- Spawn screen (C++, camera caller 0x13EED5): DirectInput keys Up 0xC8 / Down 0xD0 = class, Space = spawn,
  Left/Right = switch sides (run86).
- Jetpack gauge on the HUD: u 0.904-0.932, v 0.113-0.154, blue; needs no flight to show (run89).

## Prop generator and grass (2026-11-22, static reading + FoliageProbe run 868; full notes: the side-session notes foliage-re.md, summary here)

| RVA | What |
|---|---|
| 0x201ED0 | PropGenerator update (per game update; thread object 0x5AC24C, vtable 0x4231B4): per view (CameraManager 0x7C850, count 0x882E0, camera 0x88280, matrix 0x63D50) compares with the camera stored at 0x5AC500 + 0x40*view; moved > the limit at 0x4208D0 or forward dot < 0.7 (0x40FDC0) frees every cluster (jumps 0x201F5A / 0x201F83); then 0x201C90 orders the pool (free, not drawn since the last update, then far), 0x201880 clears the visibility bits, a frustum sweep (fov 0x291280) hands clusters to missing 32 m cells until the pool runs out |
| 0x201980 / 0x201B28 | pool size at level load: (100.0 at 0x410A80 / cell 32 + 5.0 at 0x40F814)^2 = 66, capped at 254 (cmp eax, 0xFE; jle at **0x201B52**, NOPed by Foliage b657: always 254); clusters 0x9C bytes, game heap; globals 0x5AC220-0x5AC248 (count 0x5AC240, array 0x5AC23C) |
| 0x200A10 | PropCluster render (per Render call): depth test against the current camera, builds a layer when in range (0x201040), sets the view's visibility bit (0x200C30), renders its props |
| 0x204AA0 / 0x6B210 / 0x6AFA0 | a prop's render (sphere test 0x290DC0) / grass class: fade over the last 20 m, far 0x294670 / the class's GrassPatchRenderable add (gate: renderable +0xE8 == instance flag bit 21, set by 0x6AF80: reflection pass 0x200000, main pass 0) |
| 0x6B450 | GrassPatchRenderable rebuild per Render call: blades sorted along the camera, a dynamic VB from the pool 0x28E1D0 (lock 0x28E990: DISCARD and start over when full; reset 0x28E550 from Present wrapper 0x275D10) |

In VR (run 868) the generator swapped between two cluster sets on every update while the pool was 66: the eyes draw a
wider view turned with the head than the game camera the sweep uses, so drawn and swept clusters disagree and evict
each other (head turned: 27 / 38 clusters, 117 / 528 grass patches on alternate frames). With 254 (run 869) the
counts are constant every frame.

## Sky domes (2026-11-22, static reading; full notes: the side-session notes farscene-re.md)

| RVA | What |
|---|---|
| 0x295CB1 / 0x295CB4, return 0x295CBD | the sky pass's SetNearFar: near the immediate 2.0f (signature SkyNearFarReturn; Stereo sets 0.5 while VR runs, b658), far from 0x2945A0; pass skipped when the per-view flag 0x6ED408 is clear (0x295C92) |
| 0x7A280 | dome draw: each `DomeModel` of the world's .sky placed at the current camera (identity matrix, scale 1.0f at 0x7A304, MovementScale height term); no frustum test, no clip plane |
| 0x20FB60 / 0x20F790 | DomeInfo parser (DomeModel hash 0x82681057) / SkyDome constructor (vtable 0x423F88, global 0x62DFC4) |

KAS2's `kas2_mountains` is a ring of radius 2.93 units (the sky dome 4.38): with near 2 it is clipped beyond acos(2/2.9) =
46 deg off the view axis, off screen in flat play (35 deg), at 88 % of the width with the headset's view (run 873). A dome
ring under about 3.4 units would show the same cut. The distant terrain pass (0x29E560, return +0x29E67D) culls with the
camera's own (forced) frustum: not involved.

## Zoom (found 2026-10-12, static; docs/ZOOM.md section 2)
| RVA / field | What |
|---|---|
| WeaponClass +0x48 / +0x4C | ReticuleTexture / ScopeTexture name hash (0 = none) |
| WeaponClass +0x6C, +0x70, +0x74 | ZoomMin, ZoomMax (0 -> 1.0), ZoomRate |
| WeaponClass +0x78, +0x7C | ZoomTurnDivisorMin / Max (default Min / Max) |
| WeaponClass +0x80 byte, +0xD4 byte | ZoomFirstPerson, SniperScope |
| Weapon +0xC0, +0xC4 | current zoom factor, turn divisor (getters 0xD81E0, 0xD81F0) |
| soldier +0x26C | zoomed (bool); +0x294 zoom button byte; active weapon `[soldier+0xA30 + 4*[soldier+0xA5C]]` |
| 0x10CF70 | EntitySoldier zoom update (needs ZoomMax > 1) |
| 0x241FA0 | Weapon::ZoomStep: off -> Min -> Max -> off |
| 0x2457D0 | Weapon::ZoomContinuous (ZoomRate != 0) |
| 0x118330 | soldier Trackable slot 2 GetZoom(float*): Weapon+0xC0 when zoomed, else 1 |
| 0x13F4C2 | ChaseCamera::Update ramp: lerp(target, current, exp(-25 dt)) -> SetZoom |
| 0x2914B0 | RedCamera::SetZoom(float), thiscall: +0xA0, then 0x291600 sets +0xA4/+0xA8 = +0x98/+0x9C / zoom |
| 0x245880 | Weapon::UsesFirstPersonZoom |
| 0x118520 | soldier camera slot 5: a zoomed check before 0x131640 (not on the update path, run103) |
| 0x1181A5 | `jne` (0F 85, 6 bytes) in the first-person camera (slot 1, 0x118030): skips the first-person update 0x1316B0 while soldier+0x26C is set (run103 probes); NOPed in VR by Zoom.cpp |
| 0x13072C | `jne` (0F 85, 6 bytes) in the first-person draw 0x1306E0: skips the draw when 0x1301D0 (returns byte [FirstPersonRenderable+0x1610]+0x1C = soldier+0x26C, zoomed) is set; NOPed in VR by Zoom.cpp (b105) |
| 0x130713 | call of 0x12FFC0(view) in the first-person draw 0x1306E0: "first person hidden"; true while soldier+0xA64 (camera slot, read by 0x1101F0) is 5 or 8, else a byte +0x18 of the controlled object's vfunc +0x58 result. Zooming sets slot 5 (run106). Redirected by Zoom.cpp (b107) |
| 0x6B680 | RedCamera current zoom getter (`fld [ecx+0xA0]`), read by the ChaseCamera ramp |
| 0x12FA10 | first-person renderable show / hide (view, bool); not called on zoom (run103) |
| 0x20D780 | ScopeDisplay update (vtable 0x423DD4); byte 0x20D9C2 = `push 1` that shows the overlay |
| 0x77AA0, 0x62DCF0 | ScopeBlurEffect render; blur alpha dword per view |
| 0x208000 | ReticuleDisplay update; byte 0x2084F8 = `push 1` that shows the reticle. Reticle element ReticuleDisplay+0xE0 (RedBitmapElement, vtable 0x42CEB0): vfunc +0x34 (0x2AFBA0) stores the hash, 0x2AF8E0 looks the texture up on every call, 0x2AFCC0 sizes the quad to the texture's pixels, centred; colour [elem+0x70] = 0x43A7F2; whole texture (no UV call) |
| 0x74290 | `Texture* __cdecl TextureByHash(unsigned)`: table at 0x6E2EF8 (+0 count, +4 0x400 hash keys then 0x400 values, slot hash & 0x3FF, probing backwards), 0 if not loaded. Hash: FNV-1a of the lower-cased name (0x3008F0). Insert 0x2830E0, remove 0x283040, clear all 0x282DC0 (from teardown 0x14AC10) |
| Texture object (0x3C, ctor 0x282B80, filled 0x2835E0) | +0 hash, +4 height, +8 width, +0xC depth, +0x10 levels, +0x14 format, +0x18 kind (1 2D, 2 cube, 3 volume), +0x1C IDirect3DBaseTexture9*, +0x20 pool, +0x24 usage. On the game heap (0x284260): one level only |
| Weapon +0x64 | WeaponClass (getter 0x9B620) |
| 0x3007F0 / 0x3008F0 | name hashes: HashName (nodes, bones) = CRC-32 MSB first, table 0x48E300, A-Z lower-cased; texture names = FNV-1a with each byte | 0x20. Both in `tools/texnames.py` |
| ScopeDisplay (ctor 0x20D0F0) | 4 RedBitmapElements at +0xD0/+0x1D0/+0x2D0/+0x3D0, each (screen w/2) x (screen h/2) from the global screen struct 0x6F17A0, around the screen centre; UVs (0,0,1,1), (1,0,0,1), (0,1,1,0), (1,1,0,0): one quarter texture mirrored, stretched (not aspect-correct); colour white; hash set only on change (+0x4D0). Blur: 0x20D670 / 0x20D590 write 0xFF / 0 to 0x62DCF0[view]. Sounds at +0x4DC |
| Texture table contents (run113, Naboo, Wookiee) | 532 slots used. HUD lookups all come from 0x2D32C5 / 0x2D32E4 (HUD bitmap draw): `hud1`, `HUD2` (atlases), `hud_health`, `naboo1_map`, `map_mask`, unit / vehicle / weapon icons, `gamefont_tiny_tex0` (font), the reticle. Model textures from 0x2D236E, particles from 0x2D47AE. `re/run113/textures-named.txt` |

### HUD element draws (2026-10-12, agent reading of the image, checked in run127 with `hud draws`, b124-b125)
| Address / field | Meaning |
|---|---|
| 0x298C60 | `RedInterfaceElement::Draw(parentMatrix*, parentColour*)`, thiscall, ret 8: per-element step of the interface tree (root loop 0x298D50, groups 0x2A67F0); skips unless `[elem+0x14] & 0x100`; world = local (elem+0x30) x parent, colour from elem+0x70, then vtable +0x28 Render(world*, colour*) |
| 0x2AF980 | `RedBitmapElement::Render(world*, colour*)`: nothing if [elem+0xF8] = 0; 4 vertices at elem+0x88 + i*0x1C (x, y, z, u, v, u2, v2), order (x0,y0) (x0,y1) (x1,y0) (x1,y1); texture hash [[elem+0xF8]+0x18] |
| 0x2A4D90 | `RedGlyphCacheElement::Render(world*, colour*)` (text; reached from RedTextElement::Render 0x2A5580): pages p: glyph count [[elem+0x8C]+4p], material [[elem+0x94]+4p], vertex chunks [[elem+0x90]+4p] (48 x 20-byte vertices, used count +0x3C0, next +0x3C4) |
| 0x2D3280 | pcInterfaceShader pass set-up (the 0x2D32C5 / 0x2D32E4 texture lookups): runs at queue flush, no element identity; draws are queued by 0x27C9A0 (93 callers) |
| HUD camera | screen u = (1 + 1.7320508 * X / -Z) / 2, v = (1 - 2.3094 * Y / -Z) / 2 with (X, Y, Z) = vertex x world (row vectors); factors are globals 0x6F3104 / 0x6F3100 (0x298D50 sets the camera at 0x6F1810). Checked: run127 boxes match the hand-measured Hud.cpp table within its 0.012 margin |
| vtables (RTTI) | RedInterfaceElement 0x42CB8C; RedBitmapElement 0x42CEB0, RedMaskedBitmapElement 0x42D710, RedTextElement 0x42CC08, RedGlyphCacheElement 0x42CBC8, RedVectorElement 0x42D758, Red3DModelElement 0x42CC84; groups RedGroupElement 0x42CC48, RedScreenGroupElement 0x42C980, HealthGroup 0x4192C8, AmmoGroup 0x4191D0 (its children sit in a group with vtable 0x419194), DirectionsGroup 0x419218, OutOfBoundsGroup 0x4259D4, SquadCommandGroup 0x425998 |
| Displays | StatusDisplay 0x425A10 (+0x130 HealthGroup, +0x16C0 AmmoGroup, +0x2840 OutOfBounds, +0x30D0 SquadCommand), MapDisplay 0x41F9E4, TeamDisplay 0x425B84, ReticuleDisplay 0x423800, ScopeDisplay 0x423DD4, MessageDisplay 0x41FB20 (sizes in the agent notes are inferred) |
| Slant | AmmoGroup ctor 0x154AA0: 30 deg about (0,1,0), then (0, 21, 40), x0.7 when 0x12FF80() > 1 (split screen, inferred); HealthGroup 0x1569E0: 30 deg about (0,-1,0), y 25. A real 3D rotation through the HUD camera |
| Text vertices (b126, disassembly of 0x2A4D90, drawn right in run128) | per page: glyph count x 6 vertices, triangle list, 20 bytes each (x y z u v; element space, drawn with the world matrix); chunks of 48 vertices, used count +0x3C0, next +0x3C4 (iterator 0x2A41C0 / 0x2A4340 / 0x2A4B40 / 0x2A4BB0); the glyph material, like the bitmap's, has the texture name hash at +0x18. Bitmaps are queued as a 4-vertex strip (primitive 5), text as a list (4), both through 0x27C9A0(vertices, material, world*, colour*, 0x200, 0, 0, 0) |
| Group matrix (b126) | ElementDraw computes world = local (elem+0x30) x parent with 0x8BDE0 and passes it to Render; a group's Render draws its children with that world, so the parentMatrix a direct child's ElementDraw gets is the group's world matrix. In the group's own plane (inverse applied): HealthGroup 118 x 33 units (x -118..0, y 0..33), 118 x 49 with the jetpack gauge (Dark Trooper: three more bitmaps, texture abb2e97d, in the HealthGroup); AmmoGroup 118 x 49 (x 0..118). Colour argument: pointer to an ARGB dword (alpha 0x66 normally, 0xFF just after spawning) |
| Group plane orientation (b128, run131) | with the group's inverse world matrix applied, plane x runs with screen u but plane y runs DOWN the screen (with v): drawn as y-up the boxes came out mirrored top to bottom (b126). Hud now takes both signs from the covariance of plane x / screen u and plane y / screen v per frame (`GroupDrawing::signX/signY`) |
| Group by RTTI (b126) | classes found at run time by name: vtable[-1] -> complete object locator, +0xC type descriptor, +8 the name `.?AVHealthGroup@@` / `.?AVAmmoGroup@@`: no vtable addresses needed per game build |
| run127 frame (Naboo, sim, on foot) | 45 draws: reticle (texture reticule_rifle, colour = target relation), HealthGroup: hud1 x2 + hud_health; AmmoGroup: hud1 boxes, weapon icon, grenade icon, 4 texts (counts); score HUD2 x3 + 2 texts (under RedScreenGroupElement); minimap icons (troop, turret, flag, player...) under RedScreenGroupElement. The minimap background (naboo1_map / map_mask) did not go through BitmapRender (another path, not found yet) |

### HUD element owners (2026-10-14, run188-run190 `hud draws`, b179)
- Reticle bitmap: ReticuleDisplay+0xE0 (+0x100 per channel); its screen group at +0x30.
- HealthGroup at StatusDisplay+0x130 (elements +0x200..+0x400); AmmoGroup at +0x1790 / +0x1FE0 (two subgroups).
- Target's bar: a RedGroupElement at StatusDisplay+0x1200 inside the HealthGroup: box and bar (HUD2 9891331c) and an
  icon (de5882cf) at its right end, in the reticle's red / green; plane x -96..0, y 32..65 (health plane).
- Score: RedScreenGroupElement at TeamDisplay+0x40 (elements +0xF0..+0x6B0). Minimap icons: the one at MapDisplay+0x30.
- Minimap background (2026-10-14, sub-agent static reading, used by b184, seen working in run192-run194):
  RedMaskedBitmapElement at MapDisplay+0xE0 (built in MapDisplay's constructor 0x18B730, added to the group at +0x30).
  `Render` 0x2CB9E0 (vtable 0x42D710 slot +0x28, thiscall, ret 8). Same 4 vertices as a bitmap (elem+0x88, x y z u v
  u2 v2): u/v the map, u2/v2 the mask (set once to 0..1 by 0x2CBB20). Map hash [[elem+0xF8]+0x18] (FNV of "%s_map",
  set by 0x18DB80); second material [elem+0x100], mask hash at its +0x1C (0xAC6A2D32 = `map_mask`, from 0x18D6E1).
  No second material or mask: falls back to RedBitmapElement::Render. Scrolling: 0x18E640 rewrites the map u/v
  (0x2AFB00); turning: the group's own matrix (0x18E320, sin / cos). In the group's drawing plane the background is
  143.4 units square (run193: half sides 71.7). How the game blends the mask (stage setup in 0x2D3280) not read;
  alpha times mask alpha looks right. Seen once 419.8 units (first draw at mission load, not the match HUD).

### Reticle target relation (2026-10-12, agent reading of 0x208000, used by b124 Feedback::AimedRelation, seen working in run127)
| Address / field | Meaning |
|---|---|
| player object P | the `this` of HitValue 0x9B710 called from the reticule update (Feedback's `_reticuleObject`), 0xE8 bytes (0x21F5D0) |
| Controllable X | first non-zero of [P+0xBC], [P+0xB8], [P+0xB4] (0x8C320); a soldier's is soldier+0x250 |
| [X+0x30+4i] | target relation per weapon channel i (0 primary): < 0 enemy (reticle 0xDF2020 from global 0x6D93B0), > 0 friend (0x20DF20, 0x6D931C), 0 none (0x43A7F2); written into the reticle element's colour [ReticuleDisplay+0xE0+0x100i+0x70] by 0x7D020 (alpha replaced per weapon type) |
| 0x9B5C0 | `Team::Relation(int otherTeam)`: movsx [team+other+0x8C], -1 / 0 / 1 |
| 0xC4040 | `Controllable::SetTarget(entity)`; stores the target handle also at [curWeapon+0x110] (lock-on), so "Weapon+0x110 = owner" in the weapon table is likely wrong |
| Target search (2026-10-13, sub-agent, static) | written each frame by the per-channel loop of `PlayerController::Update` 0x1F7150 (0x1F7770-0x1F8069; relation stored at 0x1F8056 from 0x146670(target) -> Team::Relation); `Controllable::ResetInput` 0xC1ED0 zeroes +0x20..+0x37 every frame (no hold timer). Ray: origin Controllable+4 = soldier+0x254, direction Controllable+0xEC = soldier+0x33C, both written by the soldier aim function 0x11CFD0 (0x11D123, 0x11D684) from the soldier's own pitch and body matrix, just before Aimer::Set (0x11D6D4): **not the Aimer, not the camera**. Candidates: sphere query 0xA2E10 radius WeaponClass+0xC4; cone test 0x2233B0 (slope WeaponClass+0xCC = AutoAimSize*0.072 or LockOnAngle; +0.785 while the trigger is held); line of sight 0xA41A0. Target handle at Controllable+0x20+8*ch (entity, generation). The direction is read through the getter 0x8FF00 (`lea eax,[ecx+0xEC]`), called at 0x1F77E0: b178 points that call at its own getter (hand direction for the player) |

### Lock-on (2026-10-20, static reading; used by b343 `LockOn.cpp`, the target's place not yet confirmed)
| RVA / offset | What |
|---|---|
| WeaponClass +0xC4 / +0xC8 | longest range kept (max of ranges) / LockOnRange (m) |
| WeaponClass +0xCC / +0xD0 | LockOnAngle / LockOffAngle, radians (ODF degrees x 0.01745329 at 0x24394E) |
| WeaponLauncherClass +0x234 | LockTime (s); +0x238 ClearLockOnFire; +0x239 ordnance homes (OrdnanceName's class, 0x24C348) |
| Weapon +0xB8 / +0xBC | charge seconds so far / needed: for a launcher the lock (slot 30 0x24C070 sets +0xBC = LockTime, or class+0x1D0); slot 34 0x245780 adds dt clamped; slot 45 0x242FE0 locked = +0xB8 >= +0xBC; slots 9 / 10 the HUD bar = ratio |
| Weapon +0x110 / +0x114 | target handle: object, serial (valid while object+0xB8 == serial: 0x8C6B0); weapon +0x140 / +0x144 last target |
| 0x24BE57 | WeaponLauncherClass constructor: +0x238 ClearLockOnFire = 1, +0x239 = 0 (word write); so the game clears a lock at a shot unless the ODF says ClearLockOnFire = 0 |
| 0x2233B0 | target score(from, dir, point, radius, slope, range, flag): t = dot(point - from, dir), reject t < 0 or t > range + radius; reject when the distance off the ray squared > (t x slope + radius)^2: a cone, the angle in radians as its slope; score = off^2 x k + t |
| 0x1F77F1.. | the search reads the weapon class's angle (LockOffAngle while locked, slot 0x2C), adds 0.785 (45 deg) when 0xC25E0 is true (not identified), range +0xC8 |

### Manual reload (2026-10-20, b355, run396)
- Weapon vtable slot 44 (0x241F20, thiscall, bool): clip empty; returning false for the player's weapon stops the
  game's own reload (the weapon stays in state 0 at 0 rounds); the reload control still reloads (slot 43 path, state 5,
  +0x84 -1). A full clip refuses the reload control (state stays 0).

### Super battle droid camera (2026-10-13, run177, run180)
- As the droid, the game's first-person camera (SetTransform from +0x13F4C2, the chase camera) alternates frame by
  frame between following soldier+0x33C (aim direction; pitched) and the body (level, other yaw, about 4 cm higher):
  with the hand's direction written to +0x33C the two differed by up to 12 degrees and 7 cm. Not seen for other
  classes. b171 no longer writes +0x33C for the droid; then its camera was steady (run181). run183 (`droidtest`, 40
  frames per phase): aim write on alternates with the mirrored left arm on (26/14) or off (21/19); write off is steady
  with the mirror on or off (40/40). The mirrored arm is not involved.
- For every soldier the first-person camera follows +0x33C: with the hand's direction written there the measured
  character heading equals the gun's (run184), which broke the character-follow loop (headset run 13). b175: no write.

### Jet packs (2026-10-13, sub-agent, static; used by b165 `Jet.cpp`, confirmed in run172-run173)
| RVA / offset | What | Status |
|---|---|---|
| 0x116720 | EntitySoldierClass ODF parser; class pointer at soldier+0x830. Class +0x16D8 JetJump (default 7), +0x16DC JetPush (14), +0x16E0 JetFuelRechargeRate, +0x16E4 JetFuelCost (0.2), +0x16E8 JetFuelInitialCost, +0x16EC JetFuelMinBorder (0.1), byte +0x170C = JetType "hover" (hash 0x70CEAB2D). No JetAcceleration | static |
| soldier+0xAF0 | jet fuel 0..1 (recharged at 0x119EDE) | static |
| soldier+0x8CC | velocity (vec3, +0x8D0 vertical) | used, works (run172-run173) |
| soldier+0xA64 | action; 7 = jetting, 6 = falling (switch in the soldier update 0x118F30, ecx = soldier+0x434, jump table 0x11B4F8) | 7 confirmed in run173 |
| 0x1103F0 | `bool __thiscall JetJump()` (ecx = soldier; thunk 0x1500F; callers 0x11A7BE, 0x11CD70): fuel check; velocity += JetJump * normalise((0,1.1,0) + F * max(move,-0.1) + R * strafe * -0.1), F = row +0x20 of the body matrix soldier+0x1B0, move / strafe = soldier+0x2A4 / +0x2A8; AI (Controllable+0xD4 < 0) uses soldier+0x35C instead; then 0x117D40(7) (inferred SetAction) | hooked b165; the game's angle logged as 90 deg standing still (run172) |
| 0x11A7EA-0x11A9C7 | hover (state 7, class+0x170C): vy += dt*(13 - 2.5*(vy-2)); vy += JetPush * aim.y * 4 * move * dt (aim = soldier+0x33C); forward sent on = 2*move*dot(aim, F); jump pressed again (soldier+0x28E == 1, via 0xC2620) or fuel < 0 = action 6. Dark trooper's state 7 at 0x11A945: inputs * 0.7, vy += JetPush*dt | static |
| 0x11A9C7 | the call of the air movement 0x110BE0 (thiscall, 5 floats: dt, forward, strafe, Controllable+0x5C?, airControl 0.3; 10 callers) in state 7 | redirected b165; vy set before and after holds the height exactly (run173: 0.00 m/s) |

### Squad commands (2026-10-14, static; used by b189 `Squad.cpp`, confirmed in run195-run196)
| Address / field | Meaning |
|---|---|
| 0x2208F0 | `SquadCommand(SquadMessage*)` cdecl (GameBuild SquadCommand). Returns at once when the mission is over (0x142500, byte 0x4E6DB6: not a network flag, corrected 2026-11-10) and, after the local feedback, in the client world of a net game (0x7CC30 at 0x2209E3; docs/research/netcode.md). Calls the feedback 0x2204B0 (voice line, `SquadCommandGroup::Show` 0x222250 via StatusDisplay+0x30D0), then loops over all controllers (count 0x1FB3B0, get 0x9B600): skips the issuer, humans (0x9A730 >= 0), other teams (0x146A80), dead; field-context Follow Me (3) skips AI in vehicles; distance from the issuer <= 45 m for commands 0-1 (0x425914), 30 m otherwise (0x40F934), x3 (0x40F80C) when the issuer is in a vehicle; sends to the AI agent 0x233660(msg). No follower limit in this function (any limit would be in 0x233660, not read). |
| SquadMessage (0x14 bytes) | +0 issuer = [Controllable+0xCC] (getter 0x8C310), +4 command, +8 game time (0x141A50), +0xC entity handle {entity, [entity+0xB8]} (Follow Me: the soldier itself). Constructors 0x10C0B0(cmd, issuer) (empty handle; GameBuild SquadMessage), 0x10C100(cmd, issuer, h0, h1) |
| Commands (field) | 0 Move Out, 1 At Ease, 2 Hold Position, 3 Follow Me (game.squadcommand.field.up/left/down/right; display index table 0x47AB00); 4-5 vehicle contexts. Soldier buttons (2 bytes each, 0xC2620 = pressed) at soldier+0x298 (cmd 0), +0x29A (2), +0x29C (3), +0x29E (1), read in the soldier update 0x11B15E-0x11B1F8; controls ifs.controls.squadup/down/left/right |
| Soldier vfunc 8 (vtable+0x20) | returns `this` (0x10FC80) |
| Result | run196: each command reached 5-7 friendly AI on Naboo (probe on 0x233660) |

### Soldier body draw (2026-10-14, static only; PLAN 51)
| Address / field | Meaning |
|---|---|
| 0x113690 | `EntitySoldier` render, vtable 0x416620 (object +0x17C) slot 3; `this` = soldier+0x17C. Early outs to 0x115668: the view's player in action ([soldier+0xA64]) 0xC / 0xD when 0x98F40 says so; flag byte arg [ebx+8] (or 1 if 0x10FFF0 is false) kept at [ebp-0x20], which skips the body draw at 0x113CF9 |
| Animator | [soldier+0xA70] (= [this+0x8F4]): pose evaluated into animator+0x1C20 (cached per frame by +0x1FFC), advance 0x307B10 / 0x307A70, evaluate 0x306050(anim, 1), blend 0x305580 |
| 0x216E50 | `SoldierAnimator::GetBones(out)` thiscall: updates the pose once per frame, then 0x2ADCA0 |
| 0x2ADCA0 | builds a per-model bone set (ctor 0x92F10, stack ebp-0x658 in 0x113690) from a pose: for each model bone (record +0x5C name hash, HashName) the pose matrix found by 0x8C230(hash) is copied (4x4); same path for the first-person arms (pose FirstPersonRenderable+0x830, 0x131F87) |
| 0x8C230 | bone set lookup by name hash, returns the matrix; 0x113690 uses it for hp_weapons (0x2B960099) and two unnamed hashes 0x2F06FA86, 0x1AF77461 |
| 0x114B5D, 0x114B91 | the body's model draws: model vtable slot 1 (matrix ebp-0x1D0 / -0x210, bones ebp-0x658, light, flags or 0x40000000, ...); more at 0x114D14 / 0x114D4D / 0x115602 / 0x11562D (not read) |
| 0x113705 | jne 0x113710 (75 09) after the visibility check (soldier vtable 0x41672C slot 11 0x115F50 -> 0x228270: false for the view's own player when the camera's 0x98F40 is true, i.e. first person). Falling through sets the draw alpha [ebp-0x1C] = 0 and [ebp-0x1] = 1; the model draw then skips every segment and draws only the shadow volume. EB 09 = body visible (run199). Alpha below 1 takes the translucent path at 0x113C19 |
| Bone set (run198, sub-agent) | +0x00 count, +0x04 0x80 name hashes, +0x204 0x80 pointers to 4x4 matrices (into the animator's pose), +0x404 skeleton (written by the draw 0x2ADE20 = [model+0xC0]). 31 bones for rep/imp infantry, **model space** (root about 0.9 m up, head 1.5 m; row 3 = translation). Swapping a slot's pointer after GetBones moves the drawn mesh (run198) |
| RedModel (vtable 0x42C6A0) slot 1 0x288680 | draw(matrix, bones, colour (b,b,b,alpha packed), flags, override), ret 0x14. Alpha byte 0 jumps to 0x288BF9 (shadow volume 0x2B1D60 only when (flags & 0x230000) == 0x10000). Bones looked up per draw: skeleton index -> hash (0xD49F0) -> 0x8C230; a miss gives identity 0x936CB0. Skinned segments: bone x inverse bind (0x2AE010 / 0x2ADE40) into ring buffer 0x657BF0; rigid segments: bone index at seg+0x8. The colour is a pointer to a dword (stock 0x6D939C = 0xFFFFFFFF); a top byte under 0xFF draws the model see-through (item_powerup_ammo at 0x39..0xA0, run 724, b618) |
| Soldier render draws | body 0x114B5D / 0x114D14 (class model [[edi+0x74]+0x20], matrix ebp-0x1D0; which one by [class+0x16D0]); 0x114B91 / 0x114D4D the same at ebp-0x210 (reflection, inferred); third-person weapon 0x114C34 / 0x114CB6 (slot 0x5C) at hp_weapons; low-res model [edi+0x8F8] at 0x115602 / 0x11562D when skipped; attachment [edi+0x958] |
| Return addresses (2026-10-14, used by b199-b205) | RedModel draw 0x288680 is called by the first-person arms at 0x130855 (return 0x130858) and by Weapon::Render 0x243000 at 0x243043 (return 0x243046; inside a soldier render = the third-person weapon). GameBuild FullBody holds all seven addresses |
| Bone frames (run198, used by the IK) | model space +y up, +z forward, the right side at -x; every bone points along its own +x (row 0); rep_inf upper arm 0.24 m, forearm 0.30 m; neck 0.30 m ahead of and 0.31 m below the first-person eye in the aiming pose (run202) |
| Render passes' SetNearFar (run206, Naboo, per eye in this order) | +0x295CBD sky 2 m / 1000; +0x29E67D distant terrain 150 / 2000; +0x295D98 the scene 0.5 / 200 (GameBuild SceneAfterFarNearFarReturn); +0x295724 first person 0.3; +0x2957DD 0.5 after the first-person pass. +0x2954FA (SceneNearFarReturn) did not run there. The player's soldier render runs in the eye passes, not the first-person pass |
| Models | rep_inf_* (third person): bone_root, pelvis, a_spine, b_spine, ribcage, neck, head, l/r clavicle, upperarm, forearm, hand, thigh, calf, foot, toe; hp_weapons. rep_1st_* arms: bone_shoulders, l/r forearm, l/r wrist; hands are rigid meshes. No finger bones in either |

### Death camera (2026-10-14, sub-agent, static; used by b180 Stereo, death view seen in run189-run190)
| Address / field | Meaning |
|---|---|
| CameraManager | global `[rva 0x4A1F80]` (getter 0x7C850); per view: +0x24 active camera, +0x2C RedCamera, +0x34 chase, +0x3C free, +0x44 death, +0x4C listener, +0x20 view count |
| 0x99630 | `CameraManager::StartDeathCamera(target, view, killer)` ret 0xC: only if active == chase; DeathCamera::Start 0x13E9A0(target, killer, &chase+0x10); active = death. Called from the soldier update (0x119341) via 0x229020 -> 0x229540 when the soldier is dead |
| DeathCamera (vtable 0x418164, 0x70 bytes) | +0x4 CameraManager, +0xC target (soldier+0x44C), +0x10 M44 (start = chase matrix of the last live frame), +0x5C look point, +0x68 timer, +0x6C killer (Character, 0 = self). Slot 1 Update 0x13FE40 (thiscall float, ret 4: turn to the body, rise, swing to the killer after 1.5 s), slot 2 Apply 0x13F710 (SetTransform return 0x13F71F). Constructor writes the vtable at 0x13DF61 (GameBuild DeathCameraTable) |
| Spawn screen | `CameraManager::SetChaseTarget` 0x98B70 makes the chase camera active again; ChaseCamera::Apply 0x13EEA0 with +0xC == 0 shows ChaseCamera+0x10 (return 0x13EED5), which nothing writes during death: the death spot (seen run189) |
| Killer | Character K = [soldier+0x28]; unit = [K+0xB8] (vehicle) else [K+0xB4] (0 while dead); position unit+0xE0. Soldier position +0x1E0 |

### Thrown ordnance (2026-10-13, sub-agent, static only; b180 uses the strength rows)
| Address / field | Meaning |
|---|---|
| WeaponGrenade vtable 0x427DB4 | slot 27 0x24AF20 = release (inferred): clears weapon+0x120 (fired), strength weapon+0x124 = timer / MaxPressedTime (1.0 when 0), state +0xB4 = 2/3 or ForceFireAnimation+1, timer +0xB8 = 0, duration +0xBC = InitialSalvoDelay + ShotDelay; slot 28 0x24B4A0 per frame: +0xB8 += dt, once >= class+0x1D4 and not fired: fire 0x24B040, ammo +0x8C -= class+0x58, heat +0x90 += class+0x5C, +0x120 = 1; 0x24B200 "done" |
| WeaponGrenadeClass (parser 0x24B330) | +0x1D0 ShotDelay, +0x1D4 InitialSalvoDelay (the release delay; 0.3 s on stock thermal detonators), +0x1D8 ShotElevate, +0x1DC MaxPressedTime, +0x1E0 HideOnFire, +0x1E1 ForceFireAnimation |
| 0x24B040 fire | descriptor D on the stack: +0x00 position = Aimer+0x7C, +0x0C direction = normalize(Aimer+0x48 + (0, ShotElevate, 0)), +0x18 strength, +0x1C..+0x38 owner, team, weapon; then `ordnanceClass->vfunc[2](&D)` (ordnance class = WeaponClass+0x50, call at 0x24B168). Calls 0x10D130 when InitialSalvoDelay > 0 (unknown) |
| Ordnance create | OrdnanceGrenadeClass vtable 0x421D6C slot 2 (create 0x1D8750, object 0xF8; ctor chain 0x1D8430 -> 0x1DAEF0 -> 0x1D45D0 -> 0x1D1BD0); OrdnanceStickyClass vtable 0x4222CC (stock thermal detonator: "sticky", Velocity 25, Gravity 1.25). Ordnance+0x40 position, +0xAC start, +0xB8 velocity = D.dir x Velocity (D.dir not normalised again: its length scales the speed), +0xE8 gravity; collision +0x80 and effect +0x74 / +0xE0 placed in the ctor (do not move after creation) |
| Hook plan | rewrite D+0x00 and D+0x0C in ordnance-class vfunc[2] (direction = hand direction x original length); or skip the delay per weapon by setting weapon+0xB8 = class+0x1D4 after the release (class shared with AI: never patch class+0x1D4) |
| WeaponDispenser | slot 28 0x24A850, delay class+0x1DC, fire = vtable slot 46 0x249FA0 (matrix from Aimer+0x7C / +0x48, inferred); velocity = Aimer+0x48 x Velocity (class+0x1D4) x lerp(MinStrength +0x1E4, MaxStrength +0x1E8, strength) + owner velocity |
| Network games | another create path 0x1A2610 |
| Strength (2026-10-14, sub-agent) | grenades: D+0x18 only picks a charge level (class chain +0x2C next, +0x28 threshold; stock: none); speed = lerp(class+0xAC Velocity, next+0xAC, t), ordnance+0xB8 = D.dir x speed at 0x1D4684, D.dir length kept. OrdnanceStickyClass create 0x1DB680. Hook: class vtable slot 2 (thiscall, &D, ret 4), D+0x38 = firing Weapon |
| Powerup effects (2026-10-24) | EntitySoldierClass +0x16F8 "Powerup_blue" (health), +0x16FC "Powerup_red" (ammo): effect names set by the class ctor 0x10B623 (ODF keys parsed at 0x116F0E / 0x117068). EntitySoldier 0x1189C0 (thiscall, no args) plays the red one on the soldier (effect by name 0x9B5A0, vtable +4 creates, 0x136070 attaches it to the entity, slot soldier+0xAE4); called by the ammo add 0x1FFF60 when ammo was added and vtable +0xCC == 1. 0x1100A0 (EntitySoldier+8 vtable slot 7, float amount) the blue one, slot +0xAD4. Holster calls 0x1189C0 after a manual reload (b389) |
| Dispensers (2026-10-14) | WeaponDispenser 0x427A9C, WeaponDetonator 0x42779C, WeaponRemote 0x428390: release slot 27 0x249ED0 sets weapon+0x124 = press time / MaxPressedTime (1 when 0); fire slot 46 (0x249FA0, thiscall, no args; Remote 0x24D620 calls it): speed = class+0x1D4 x lerp(+0x1E4 Min, +0x1E8 Max, +0x124), lerp unclamped. Class: +0x1D0 OrdnanceName (entity class: EntityMineClass, PowerupItemClass, EntityDroidClass), +0x1D8 ShotDelay, +0x1DC InitialSalvoDelay, +0x1E0 MaxPressedTime, +0x1EC MaxItems |
| 0x64570 | `ModelByHash(hash)` cdecl: model registry 0x6E59F0 (what WeaponClass+0x3C holds), keyed by the 0x3008F0 name hash (FNV-1a, bytes \| 0x20; the caller 0x243A83 runs 0x300730 -> 0x3008F0), **not** HashName's CRC (b180 used that: every lookup failed). b206: com_item_powerup_dual found (run212) |

### First-person weapon draw (2026-10-13, sub-agent, static; used by b159 for the droid's mirrored arm)
| Address / field | Meaning |
|---|---|
| 0x130320 | `FirstPersonRenderable::Render`: jump on +0x160C (table 0x130388): 0 on foot 0x1306E0, 1 / 3 arms only 0x1303C0, 2 0x130430. On foot: arms drawn at 0x130855 (model +0x15FC, matrix +0x1570), then `Weapon::Render` 0x243000 (vtbl+0x5C) on the Weapon at +0x1600 with the matrix from +0x15B0, light 0x6D939C, flags \| 0x80001 |
| 0x243000 | after the weapon model: skipped if byte [light+3] == 0; copies the matrix to weapon+0x20; if weapon+0xC8 - now() > 0 draws the muzzle flash (0x243260: class+0x190 model, +0x198 scale, +0x1A0 colour, fraction / class+0x188), immediate; state +0xB4 == 4 (charging): world effect at weapon+0xFC moved to +0x20 |
| side effects of a second call | weapon+0x20 overwritten; FPR+0x1628 smoothed again when 0x2B0CC0 is true |

### PLAN 53 findings (2026-10-17, b207-b209)
| Address / field | Meaning |
|---|---|
| 0x222250 | `SquadCommandGroup::Show(int context, int highlighted)` thiscall, ret 8 (GameBuild SquadDisplayShow): labels from 0x62EB90 + context x 16 (4 per context), the highlighted one alpha 0xFF, others 0x7F (via 0x7CF70); shows the group (0x7B540(1)); +0xB0 = 1.0 (fade). Group = StatusDisplay+0x30D0. Called each frame while armed: the display stays up (run213-run214) |
| StatusDisplay | found from its HealthGroup at +0x130 (HudDraws, RTTI `.?AVStatusDisplay@@`) |
| WeaponDispenser release | slot 27 0x249ED0: +0x120 fired = 0, +0x124 strength, timer +0xB8 = -1.0; slot 28 0x24A850: timer < 0 -> 0, else += dt; fires (vtable +0xB8 = slot 46) once timer >= class+0x1DC (InitialSalvoDelay) with fired clear, then +0xBC = class+0x1D8 and fired = 1. Mine dispensers: class+0x1DC = 0.30 s (default; the ODFs set none), powerup dispenser 0 |
| Soldier bone set | 31 slots on the Rebel pilot / troopers: the 24 named in Body.cpp, dummyroot (0x4446E9B8) and six unknown (2FE55C87, D1E1F308, 7238CCAE, C7C743D8, B4209B59, 1A14D260; not any eff_ / root_ name by the game's hash). Source meshes skin to eff_* bones (eff_head, eff_*_forearm, eff_*_hand); the Rebel pilot's head is its own skinned segment (bone_head + eff_head), the troopers' helmets rigid on bone_head |
| Pistol models | barrel axes in model space (source meshes, +x = the player's left): all_1st_weap_inf_pistol 4.5 deg toward +x and its hp_fire node at the origin (marker box at 0.03 0.06 0.35-0.39); cis_ and rep_ about 1.8 deg; imp_ about 1.9 deg (2026-10-18: its muzzle, top rail and scope nodes; an earlier whole-mesh reading said straight, and idren2023 saw it off in headset run 16) |

### Droideka (2026-10-17, sub-agent, static; used by b211, mode and pitch seen in run216)
E = EntityDroideka (Controllable at E+0x250, as the soldier). "Seen" = read in the disassembly.
| Where | What |
|---|---|
| E+0x434[1] 0xE2D30 | update. Pitch E+0x4C4 += Controllable+0x60 (mouse Y axis) * class+0x1674 * dt, clamped to PitchLimits (class+0x1698/+0x169C, radians), seen. Turning: Controllable+0x5C to movement vfuncs +0x108 / +0x10C turns the body matrix E+0x1B0: aim yaw = body heading. YawLimits only copied |
| vtbl slot 63 0xDFC90 | aim function (soldier's 0x11CFD0): `bool __thiscall (int)` on E (ret 4, returns `mov al, 1`; a hook that returns nothing broke the caller, runs 309-310). Controllable+0xEC (E+0x33C) = body * (0, sin p, cos p), written first, unconditionally; ray 0xA40C0 from Controllable+4 (E+0x254) along it, 512 m, args (start, dir, 512.0, &out (the caller keeps 8 bytes), 0, &{E, ?}, 1, mask 0x4E, 1); hit point -> target point Controllable+0x10 (E+0x260..0x268). No Aimer::Set. Hooked b281 (Vehicle, target moved to the head's line) |
| body turn rate | standing, turned by mouse counts toward a 40 or 60 deg error: a steady ~35 deg/s (run308 trace), whatever the counts; the arms' Aimers follow the body within 0.2 deg unless the target point is moved |
| E+0x4C8+4i | Aimer* (heap, 0x140 bytes, one per weapon; ctor 0x8B700 from init 0xDB370). Aimer::Update 0x8D210 (vtbl[1]): skipped if Aimer+0x29 (set by Aimer::Set); aims +0x48 at the target point, rate-limited, clamped to AimerYaw/PitchLimits (0x8BF10), +0x7C = fire point. Arm pose 0x8CF30 called from E+0x17C[3] 0xE05E0 |
| E+0x4D8+4i / E+0x4E8 | weapons / current index per channel |
| E+0x1E6C | mode: 0 standing, 11 rolling up, 12 ball, 13 standing up (run216); 1 / 2 seen briefly while turning. Toggle (slot 64 0xE3C20) on a press of Controllable+0x42 = input bit 7 = the enter / exit control (E in idren2023's profile; run216). Jump (+0x3E) unused by the droideka. No primary fire in 11-13 |


### Vehicles, turrets, seats (2026-10-17, sub-agent, static only; PLAN 54)
C = the player's Controllable (PlayerController+4), E = vehicle entity, T = MountedTurret. Corrects engine-misc-2 C:
the mapper 0x1F8500 is gamepad aim assist (returns at once with the mouse), not the source of +0x5C / +0x60.
| Where | What |
|---|---|
| 0x1F72D1-0x1F7540 | +0x5C (SetYaw 0xDA0E0) / +0x60 (0xD9A60) from the device getters 0x1F6FB0 / 0x1F6FA0 on PC+0x24: rates, clamped +-1 (pad) or +-16 (mouse) |
| seat by [C] vtable (RTTI) | Hover 0x415790, CommandHover 0x411BC0, Flyer 0x414DFC, Carrier 0x413E04, Walker 0x4171DC, CommandWalker 0x41239C: E = C-0x250. MountedTurret 0x4200D8, PassengerSlot 0x4225E8: object = C-8. All seats' Controllables in the list at E+0xBC (pilot first) |
| T (0x320 bytes, ctor 0x1951B0 from flyer / hover / walker inits and building turrets) | +0x1EC update 0x1974F0: filtered rates T+0x2BC / +0x2C4 = lerp(-(C+0x60 / +0x5C) * rate, filter*dt); pitch T+0x2B8, yaw T+0x2C0 += rate*dt (radians), clamped to YawLimits / PitchLimits of section S = T+0x2CC (S+0xE30..0xE3C; S+0xE40 / 0xE48 rates mouse / pad, +0xE44 / 0xE4C filters, +0xE50 full 360). T+0x2C8 parent, T+0x210 turret world matrix, T+0x290+4i Aimers, T+0x2A0+4i weapons, T+0x204 Trackable |
| class / turrets | Hover: class E+0x500, turrets E+0x520 (byte count E+0x540), sections class+0x21F8+4i. Flyer: class E+0x534, turrets E+0x554 (count E+0x574). Walker: class E+0x4E0, turrets E+0x4F4, sections class+0x2224+4i |
| Aimers | vehicle and turret weapons use Aimer (ctor 0x8B700); Aimer::Update 0x8D210 aims +0x48 at the owner's target point C+0x10 (owner Aimer+0x20) within AimerYaw / PitchLimits |
| aim per class | C+0xEC = body forward turned by the class's aim angles; third person: a camera ray. Then 512 m ray to C+0x10. Walker vtbl[63] 0x126C80 (W+0x520 / 0x524), Flyer 0xE88C0 (F+0x504), Hover 0xFB3D0 (H+0x554 / 0x558), turret inside 0x1974F0 |
| Flyer inputs (0xEC430; ODF 0xEA550) | +0x60 pitch rate (PitchRate class+0x158C, PitchFilter +0x1594), +0x5C turn rate (TurnRate +0x1590, TurnFilter +0x1598), +0x58 strafe (StrafeSpeed +0x1588) and roll (StrafeRollAngle +0x15A0), +0x54 throttle (class+0x159C) |
| Hover inputs (0xFEBD0; ODF 0xFC9E0) | +0x54 drive (ForwardSpeed +0x1588, ReverseSpeed +0x158C), +0x5C turns the hull (class+0x159C / 0x15A0, inferred Turn rates), +0x58 strafe (+0x1590), +0x60 aim pitch |
| Camera | ChaseCamera::Update 0x13EEA0 calls the target's Trackable (E+0x44C, T+0x204) slots 1-3; slot 1: Walker 0x12AB20, Flyer 0xEBB20, Hover 0xFE590, Turret 0x1970D0. TrackableClass (parser 0x228330): +0x0 EyePointOffset, +0xC TrackCenter, +0x18 TrackOffset, +0x4C TiltValue; per class: hover class+0x21A0, flyer +0x21A8, walker +0x21CC, turret S+0xDB4. First person (0x98F40 true, inferred) builds the camera from the aim angles; cockpit model = FirstPersonRenderable states 1 / 2 at hp_camera (0x131EA0, 0x1328C0) |
| FirstPersonRenderable +0x160C state | Render 0x130320 jump table 0x130388: 0 soldier (0x1306E0), 1 and 3 model at +0x1570 only (0x1303C0), 2 model plus weapon at +0x15B0 (0x130430). Seen: hover pilot 3, AAT gunner seat 1 (run223-225). The cockpit (state 1-3, update 0x131EA0) = local x inverse(hp_camera) x the camera handed to Trackable slot 5 (ChaseCamera): rigid in the game camera's frame (0, -1.5, -1.5 m in the AAT and STAP), one frame late at a change |
| Pose table (0x408 bytes, hash -> 4x4) | Init 0x92F10 (thiscall, returns this), Set 0x8D1E0 (thiscall: hash, const float* matrix; copies), lookup 0x8C230. The arms draw 0x1306E0 builds one on its stack (Init, then 0x2ADCA0 = Set for every bone of the skeleton at +0x830) and calls the arms model's vtbl[1](matrix +0x1570, table, light 0x6D939C, flags \| 0x80001, 0). A table of ours drew the copied arms correctly (run227) |
| Vehicle class name | the ODF class name's FNV-1a \| 0x20 hash (0x3008F0's) at class +0x18 (rep_hover_fightertank, hover class E+0x500; run228, found by searching the class for the hash) |
| Soldier class name | the same hash at EntitySoldierClass +0x18 (soldier+0x830 -> class): all_inf_soldier 3A514751 (run 820), all_inf_marksmansnow 75E6A734 (run 819, Hoth's first-person all_1st_hothmarksperson); used by Body.cpp HasFullBodyClass (b641) |
| First-person arms skeleton | 8 bones (run227-229, clone and droids): from the msh, dummyroot -> bone_shoulders -> bone_l/r_forearm (elbow) -> bone_l/r_wrist (0.271 m along z), hp_weapons under bone_r_wrist; no upper arm |
| First-person cockpit model (vehicles) | drawn with FirstPersonRenderable +0x1570 = local x inverse(hp_camera) x camera; for every stock tank the result puts the model's origin 1.5 m below and 1.5 m ahead of the game camera, axes aligned, scale 1 (runs 222-238): the eye sits at (0, 1.5, 1.5) of the msh's space, looking along -z (a render of cis_1st_cockpit_AAT.msh from there matched run225's picture). The cockpit msh files have no hp_camera node. 0x919A0 (with 3.2) calls a D3DX matrix helper (two calls, 0x2748C6 / 0x2748D2): a rotation by that angle, not a scale |
| Stock cockpit textures | by name: cis_1st_cockpit_aat, cis_1st_cockpit_mtt, rep_1st_cockpit_fightertank, imp_1st_cockpit_fightertank (512 x 512); effects cis_1st_cockpit_fx (128), rep_cockpit_fx, imp_1st_cockpit_fx (64); all loaded while in the vehicle (runs 235-238) |
| Stock vehicle models (data, not code) | the ODF's GeometryName names the drawn hull, not the ODF name: all_hover_combatspeeder draws all_hover_rebel_attack_speeder (tandem hp_pilot / hp_gunner on the middle line, gunner seat FirstPerson all_1st_cockpit_securitycam; its first-person cockpit all_1st_cockpit_combatspeeder is a two-seat cabin, node all_1st_cockpit_landspeeder1); the mod tools' all_hover_combatspeeder.msh is unused. all_hover_hovernaut (Rhen Var Harbor, GCW Rebels): hull only in GameData/Data/_lvl_pc/SIDE/all.lvl (modl chunk at byte 12437580, 179124 bytes: segm INFO (5, vertex count, primitive count), MTRL, RTYP, TNAM, IBUF = count + u16 triangle strip, VBUF x2 (count, stride, flags; stride 36 = position, normal, uv floats), BNAM), first person all_1st_cockpit_hovernaught.msh / .tga in BFBuilder ALL/MSH (2026-10-17, runs 240-242) |
| Bike / STAP first person (runs 261-262) | the game's first-person camera sits in the hull (level, model axes) at (0.16, 0.99, 0.07) on imp_hover_speederbike (ODF EyePointOffset 0 1 0) and at (-0.2..-0.33, 2.94-2.99, -0.06..0.52) on cis_hover_stap (EyePointOffset 0 3 0); FirstPersonRenderable state 3 there too. The hull models draw in the first-person pass by model vtable slot 1 at a world matrix; imp_hover_speederbike is skinned (bones bone_root, bone_l/r_handlebar, _foil, _rearflap, _footpedal, gun): a pose table of model-space bone matrices at their msh rest places (identity rotations) draws it whole and still (run261). Pilot animations place the rider relative to hp_active: human_speederbike_sit in bike model space (hp_active at 0), human_stap_ride offset by the STAP's hp_active (0, 0.36, 0). Hover EntityHover render: +0x17C vtable slot 3 = 0xFBE60; its first check (Trackable slot 11) skips the whole render, the pose table E+0x1B34 is filled from the animation at 0xFC030 only after it |
| Gunner seat camera (b249, runs 264-265) | the game camera in a MountedTurret seat = turret world matrix (+0x210, row 2 forward) x the seat's EyePointOffset: AAT (0, 2, -1.75), fighter tanks (0, 0.5, 3); the turret origin is not on its pivot (moves as it turns). FirstPersonRenderable state 1 draws the seat's `*_1st_cockpit_securitycam` frame at (0, -1.5, -1.5) of the game camera. The vehicle's hull and turret are drawn by the game in the scene while the player sits in its gunner seat |
| Hover hull matrix | the hover's entity matrix (+0x1B0) pitches with its hover motion (AAT standing: -9 to -21 deg, run224) while the first-person camera at zero aim stays level |
| Nodes | pose-override tables hash -> matrix* (lookup 0x8C230, insert 0x8D1E0): hover H+0x1B34, flyer F+0x1AF4, walker W+0x1C80; static node positions Model::FindNode 0x2882B0 |
| Walker render and visibility (static 2026-11-07, mounts) | EntityWalker ctor 0x121CD0 writes the vtables: E+0x17C 0x4171AC (render object; slot 3 = 0x1279B0, the walker's render), E+0x250 0x4171DC (Controllable), E+0x44C 0x4172B8 (Trackable; slot 11 = 0x128680). The render (this = E+0x17C) first calls the Trackable's slot 11 (visible(view), ret 4) and skips everything when false; then each turret's Trackable (E+0x378 list, count E+0x398), the Aimers' poses into the pose table E+0x1C80 (this+0x1B04), the turrets' 0x1964A0, the hull. 0x128680 returns true early when 0x90DE0(E+8) is false, else 0x228270 (false for the view's own object in first person: the same check that hides the player's soldier, 0x113705). Hover twin: render 0xFBE60, visible 0xFC8F0 (differs at +0x130 / +0x120 for +0x10C / +0xFC). Handlebars hooks 0x128680 (GameBuild WalkerVisible) to draw a ridden tauntaun / Kaadu in first person. The tauntaun is an EntityWalker (no EntityTauntaun class in the exe; the Lua pool name is a leftover) |
| Pose table layout (0x8C230 -> 0x300890, insert 0x3009F0; static, b250 reads it) | +0 entry count (insert increments it), +4 128 hash slots, +0x204 the 128 matrix pointers (slot i's at +0x204 + 4i); open addressing from slot hash & 127, probing down with wrap, an empty slot (hash 0) ends the lookup; insert never replaces (takes the first empty slot) |
| Hover hull draw (EntityHover render 0xFBE60, static 2026-10-18; runs 266-267) | edi = E+0x17C. Aimers E+0x55C[i] (class +0x218C count) pose 0x8CF30(&matrix, table, 1); mounted turrets E+0x520[i] (byte count E+0x540) pose 0x1964A0(matrix ebp-0xD0, ebp-0x120, table E+0x1B34, flags); then the hull model E+0x1F0 vtbl[0](flag, float, matrix ebp-0xD0, table E+0x1B34, light 0x6D939C, flags, 0 or ebp-0x1C) at 0xFC5C5 / 0xFC5FA. Observed: the RedModel draw 0x288680 is reached with bone set = E+0x1B34 (about 2 draws per eye render), its matrix equal to the entity matrix E+0x1B0 |
| MountedTurret pose 0x1964A0 (runs 266-267, fighter tank gunner seat in first person) | writes the turret's bones (turret_y, turret_x, turret_barrel1) into the hull's table with their live rotation but the translation (0, 0, -1000, 1) (0x1964E7 lookup, 0x6C0A0 sets the row; also at 0x1963FD / 0x196446 in 0x196390): the game's own way of keeping the turret out of the gunner's view (inferred). The rotations are model space (all three share the yaw bone's heading). For the first ~3 s in the seat the barrel bone's forward pointed up to 100 deg sideways (an activation animation, inferred) |
| MountedTurret update 0x1974F0 (static 2026-10-18, b253, runs 269-279) | `this` = T+0x1EC (vtable there). Angles relative to the parent's matrix (parent T+0x2C8, its vtable slot 14 = 0xB8010 for hovers: E+0x1B0): pitch T+0x2B8 (negative = up), its filtered rate T+0x2BC, yaw T+0x2C0 (atan2(x, z) of the model axes: left positive; run315, combat speeder: +79.7 at the left stop, -79.7 at the right), rate T+0x2C4. With a player in it (C+0xD4 >= 0, 0x8C710): rate = lerp(rate, -input * Rate, min(1, Filter * dt)) (Rate S+0xE40 mouse / S+0xE48 pad, times 1/zoom; Filter S+0xE4C pitch, +0xE44 yaw); angle = actual + (angle - actual) * \|input\| with actual = the angles of T+0x210's row 2 in the parent's frame (inverse 0x66950); angle += rate * dt; clamped to S+0xE30 / +0xE34 (yaw) and S+0xE38 / +0xE3C (pitch) when inside (-pi, pi), else wrapped (0x197480); T+0x210 = R(yaw, pitch) x turret node x parent matrix (0x198399-0x19844E); aim C+0xEC = T+0x210 row 2. So with no input the gun keeps its world direction while the hull moves; an input of +-1 keeps the written angle. |
| TurretHideChildren 0x196390 (cdecl: model, pose table, node; callers 0x1964A0's first-person branch) | walks the turret node's children, mapping each child's index to its hash through the table's skeleton (table +0x404, via 0x2AE140 -> 0xD49F0) and writing translation (0, 0, -1000); a table without a skeleton (+0x404 null) crashes it (run273: imp_hover_fightertank uses AnimationName rep_hover_fightertank, not loaded on RHN1i; a window at the seat change). Vehicle's hook skips it then (b257). |
| AAT (cis_hover_aat.msh) turret nodes | Gun_body (0, 3.621, -2.709) yaw, Main_gun +(0, 0.040, 1.877) pitch (Aimer), recoil_3 +(0, 0.003, 1.381); all rest rotations identity; gunner seat ODF: PitchLimits -40 20, YawLimits -180 180, EyePointOffset (0, 2, -1.75); in first person the game hides Gun_body and its children (-1000) itself (run275), and then writes recoil_3 with a rotation 82 deg off the gun's (run306). Skin weights (CIS_hover_aat_mesh): Gun_body 605 vertices x +-1.42, y 3.14-4.88, z -4.44..-0.65; Main_gun (breech) 159, x +-0.36, y 3.44-3.90, z -1.09..0.58; recoil_3 (barrel) 135, x +-0.10, y 3.59-3.80, z 0.46..4.12; hp_cannon_1 (muzzle) (0.004, 3.689, 4.153). Turret Aimer slots T+0x290: fighter tank and AAT one Aimer in slot 0, the other slots hold leftover floats (run304-305). Turret seat Controllable (T+8): +4 the aim ray's start (the game camera: 1.9-2.6 m from the AAT cabin eye, run313-314), +0x10 the point that ray hit (510 m when nothing; 13-57 m on ground ahead), which the turret's Aimers converge on. Combat speeder (all_hover_rebel_attack_speeder): turret_body (0, 1.187, 0.432), turret_aimer +0.078 / +0.277, turret_barrel +0.118; pilot EyePointOffset (0, 1, -2.2), gunner seat EyePointOffset (0, 2, 4), YawLimits -80 80. |
| First-person arms' fist (b256) | the hand mesh imp_inf_trooper_r_hand's centroid in bone_r_wrist's frame (-0.023, 0.015, -0.108) (rep_1st_arctrooper.msh); hp_weapons (-0.008, 0.078, -0.162) is the weapon's attach point, above and ahead of the fist, and its runtime matrix is not in the bones' space. In the hand pose's frame the fist sits about (0, -0.05, 0.06) (run275 grip presses; the first arms copy at the spawn gives a transient 0.5 m). |
| Fighter tank hull (rep / imp_hover_fightertank.msh, one mesh) | skinned, every vertex on one bone; the stock turret = 440 vertices on turret_y (0, 1.793, -2.858) / turret_x (+0.276 up, +0.540 ahead of it) / turret_barrel1 (-0.033, +0.476 from turret_x; hp_gun_1 +0.312 beyond), rest rotations identity; deck under it closed (platform y 1.80, x +-0.4..0.5, z -2.4..-3.2; rear spine 1.86; 1.66 at the sides and ahead). Gunner seat ODF: PitchLimits -40 15, YawLimits +-180, EyePointOffset 0 0.5 3. Same mesh and texture layout on both tanks |

### Flyer mouse (run518-519, 2026-10-31)
- In a flyer's pilot seat the mouse's vertical runs the other way round from a soldier's look: counts that raise a
  soldier's view lower the nose. Mouse X turns the ship (18 deg of stick at 120 deg/s-equivalent: ~16 deg/s); A / D
  give the yaw input +0x5C = +-1 (a sharp turn, ~28 deg/s in the X-wing).

### First-person arms model (run521-522, 2026-10-31)
- FirstPersonRenderable +0x15FC (the arms model) has 7 nodes: dummyroot (HashName 0x4446E9B8) and the bones; no
  node carries the model's name (unlike the weapons' root). The model registry (ModelByHash, FNV-1a with bytes | 0x20
  of the model name) returns the same pointer for the unit's name, e.g. all_1st_marksperson.

### Network games (2026-11-10, static traces; the flags, team managers and Character layout seen in run 679; details docs/research/netcode.md)
| Address | Meaning |
|---|---|
| byte 0x506BF2 | a network game is running (function 0x67DB0) |
| byte 0x506F60 | the client world is the active one (0x7CC30; switched on by 0x1A3A00, off by 0x1A75F0) |
| byte 0x506F61 | the turn being simulated is simulated for the first time, not a replay (0xA3CB0); used with 0x506F60 by `Net::IsLocalPlayerStep` (b595) |
| float 0x507064 | seconds per game turn (1 / turns per second); read by `Net::GetTurnSeconds` (b590) |
| EntitySoldier +0x1B0 | its world matrix: **rows left, up, forward**, position (runs 682, 683, 685: W moves along +row 2, stick right along -row 0) |
| soldier turn | heading and pitch change by rate x 2.0 x turn length per turn (run 685: the loop built on it settles without overshoot); strafe speed about 0.67 of running speed (run 682) |
| control state bits 8, 9, 10, 11 | squad commands 1, 3, 0, 2: pressed for one turn they call SquadCommand 0x2208F0 from the soldier update (+0x11B1FD) in both worlds (run 686) |
| Controllable +0x10 (soldier +0x260) | the point the soldier's own aim ray ends at, also for a soldier in a net game's client world (run 698: 5.7 m aiming at the ground, 55-65 m level) |
| Character +0xB4 (unit) | stays the soldier while the player sits in a vehicle; the soldier's position is then the vehicle's (run 688) |
| byte 0x503A10 | this machine hosts (0x7C9E0) |
| int[2] 0x505620 | network player id of local player 0 / 1 (-1 none) |
| 0xA40C0 | world ray cast, cdecl: `float (const float* start, const float* direction, float range, void** hitObject, int 0, void** leftOut, int leftOutCount, unsigned mask, int 1)`; returns the free part of the range (1.0 = nothing hit; hit distance = result x range); world object at 0x4A4C80 (inner 0xA36B0). The soldier's aim function calls it at 0x11D553 with range 60 (so Controllable +0x10 is at most 60 m away for a soldier; seats use 512), mask 0x4E and itself left out. Called by the mod since b603 (`Net::CastAimRay`), from the control-state hook (host world active) and from Holster's update: both gave ground hits at the expected distances (runs 703, 705-709) |
| Ordnance class +0xDC | gravity as an acceleration, negative (thermal detonator: -12.25 = ODF Gravity 1.25 x 9.8), copied to ordnance +0xE8; flight has no drag (run 704). Bounce of a thermal detonator (runs 705-706): speed off the surface x 0.33, speed along it reduced by 1.08 x (off before + off after), to zero at most. In a net game both worlds create their grenade through the class's create slot 2 (host world first, same tick on a listen server) |
| 0x62EA54 / 0x62EA58 / 0x62EA50 | TeamManager of the host world / of the client world (net games only) / the active one. +0x38 Character count, +0x3C Character array (0xE8 each; +0x9C player id, +0xB4 unit = the EntitySoldier) |
| 0x4E64F4 | the local control state, 20 bytes: +0 strafe, +4 move, +8 turn rate, +0xC pitch rate, +0x10 button bits (0 fire, 1 secondary, 2 jump, 3 crouch, 4 zoom, 5 view, 6 reload, 7 enter, 8-11 squad, 12-15 weapon next / prev, 16 prone); filled by the mapper 0x67DE0 |
| 0x1AF320 | cdecl (dst, src): copies a control state; AdvanceTurn 0x1AF5E0 uses it once per turn into the move ring 0x507440. Callers: 0x1AF751 (local player 0, return 0x1AF756, net game only), 0x1AF7A3 (local player 1), 0x1A6F5C (host turn advance 0x1A6F20, single player only: input state into player 0's control state `[0x506CF8]`). Hooked since b589 (`Net::SetLocalControlWriter`, run 682) |
| 0x1ADA70 / 0x1ABBF0 | client sends its moves (packet type 0x0B) / host stores them; 0x1A6F20 feeds them to the players' control states 0x506CF8[50] |
| 0x1AEC70 / 0x1ACB20 | host sends a state update (0x0C) / client reads it; soldier state write 0x1D0000, read 0x1CBBB0; replay of unacknowledged moves 0x1A7E80 |
| EntitySoldier +0x8D8 | pitch (radians, -0.8..1.1), integrated from the pitch rate; yaw is the body matrix |
| 0x142500 | "mission over" (byte 0x4E6DB6), not a network flag |

### Display size and frame sync (2026-11-10, read in the code; used by b623 / b624)
- Command line `/win` sets the byte at 0x581ec4, `/resolution W H` the ints at 0x581ebc / 0x581ec0 (parser 0xb8560,
  stores at 0xb92a5 / 0xb92b8). The video set-up 0x1f8d20 (this = the profile's video record: width word +2, height
  word +4): with the flag and both sizes set it calls 0x27d520(width, height, 1, record byte +7, 0, 0) and returns,
  else it takes the profile's mode. The mod's `render_size` writes these three before Direct3DCreate9 (run 738-740:
  the device's back buffer and all the game's own targets take the size; the window may be larger than the monitor).
- Frame sync at 0x275510: an IDirect3DQuery9 at 0x63963c; GetData(&value, 4, D3DGETDATA_FLUSH), and while that
  returns S_FALSE: Sleep(0) and again, without limit. Called at the start of the scene render (under 0x29536d). On
  D3D9On12 it never returns when nothing was drawn since the last submit (runs 732-734).
- Menu cursor: counts across the screen 1 022 at both 1920x1200 and 2400x2240, counts down 667 and about 890: they
  follow the picture's shape, not its size (run 739; the law is not known).
