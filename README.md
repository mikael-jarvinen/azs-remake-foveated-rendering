# Foveated Rendering for Arizona Sunshine Remake

![Foveation rings](docs/images/foveation-rings.png)

Eye-tracked (dynamic) foveated rendering built into the game's own renderer. The area you look at stays at full
quality while the GPU shades your peripheral vision at a quarter or a sixteenth of the pixels. No injector and no
overlay: the mod runs inside the game and only touches the 3D scene passes, so post-processing, UI and text stay clean.

**Download:** [Nexus Mods](https://www.nexusmods.com/arizonasunshineremake/mods/5)

![Quality and performance](docs/images/quality-and-performance.png)

Measured in the main menu: the game's GPU time per frame went from 3.42 ms to 1.86 ms.

## Requirements

- NVIDIA RTX 20xx or newer, or GTX 16xx (Variable Rate Shading on Direct3D 11 through NVAPI). On other GPUs the mod
  stays off.
- An eye-tracked headset that shares gaze with SteamVR. Tested with the Steam Frame: SteamVR > Settings > Steam Link >
  enable "Share eye tracking data to other apps on this PC".
- [BepInEx 6 for IL2CPP](https://builds.bepinex.dev/projects/bepinex_be) (build 6.0.0-be.788, win-x64).

Without eye tracking data the game renders at full quality everywhere.

## Installation

Put `AZS1HD.FoveatedRendering.dll` and `AZS1HD.Foveated.Native.dll` in `BepInEx\plugins\AZS1HD.FoveatedRendering\`
and start the game. In VR, `BepInEx\LogOutput.log` shows `receiving eye tracking data`. Set `ShowOutline = true` to
see the foveation rings: red dot = gaze, green = edge of full quality, yellow = edge of 2x2 shading.

## Settings

`BepInEx\config\azs1hd.foveatedrendering.cfg`, created on the first launch:

```ini
[General]
Enabled = true
GazeSource = SteamVR            # SteamVR | Center | Simulated | TestDirection
InnerDegrees = 20               # full quality within this angle of the gaze
MidDegrees = 32                 # 2x2 shading up to this angle, 4x4 beyond
MidRate = 1                     # 1 = 2x2, 3 = 2x1, 4 = 1x2
OuterRate = 2                   # 2 = 4x4, 1 = 2x2
WithoutGaze = FullQuality       # FullQuality | Center
GazeHoldSeconds = 0.4
[Debug]
ShowOutline = false
BenchmarkSeconds = 0            # >0: toggle foveation every N seconds, log GPU ms per frame for each state
VerboseLogging = false
DumpIntervalSeconds = 0         # write BepInEx\foveation_dump.ppm (both eyes, upright) every N seconds
FlipRows = false
TestDirection = -0.35,0.35,-1
GazeActionPath = /actions/Vertigo/in/EyeGaze
```

## How it works

- **Native module** (`native/foveated.cpp`): hooks `ID3D11DeviceContext::OMSetRenderTargets` on the immediate and
  deferred contexts. A bind with depth and an eye-shaped colour array that is multisampled or HDR is the game's 3D
  scene pass; it gets NVIDIA VRS through NVAPI with a per-eye shading-rate image (16x16 pixel tiles). Every other
  bind turns VRS off, and redundant off-calls are skipped.
- **Angular foveation**: each tile's view direction is precomputed from OpenVR's raw projection, the one Unity renders
  with. A tile is full rate within `InnerDegrees` of the gaze direction, 2x2 within `MidDegrees`, 4x4 beyond.
- **Gaze**: `IVRInput::GetEyeTrackingDataForNextFrame` combined with the compositor's render pose for the same frame
  (`IVRCompositor::GetLastPoses`), converted to a head-space direction. Only the direction is used; the fixation
  distance is noisy. The plugin adds the `eyetracking` action and HMD bindings to the game's SteamVR action manifest.
- **Managed plugin** (`src/FoveatedRendering`): drives the native side every frame from `LateUpdate`, issues a Unity
  render-thread plugin event to refresh the shading-rate images, draws the debug outline as meshes on the head camera,
  and runs the benchmark from SteamVR's `GetFrameTiming`.
- Unity eye textures are bottom-up, OpenVR's raw projection is y-down; the tile mapping accounts for both.

Testing without a headset: enable SteamVR's null driver (`steamvr.vrsettings`: `driver_null.enable = true`,
`steamvr.requireHmd = false`, `steamvr.forcedDriver = "null"`, `steamvr.activateMultipleDrivers = true`), set
`GazeSource = TestDirection` or `Simulated`, `ShowOutline = true` and `DumpIntervalSeconds = 5`, and look at the dump.

## Building

Requirements: Windows, .NET SDK 8 or newer, Visual Studio 2019+ or the Build Tools with the C++ x64 workload, and the
game with BepInEx 6 IL2CPP installed and launched once (the managed project references the generated
`BepInEx\interop` assemblies, which are not part of this repository).

```bat
native\build.bat
cd src\FoveatedRendering
dotnet build -c Release -p:GameDir="C:\Program Files (x86)\Steam\steamapps\common\Arizona Sunshine Remake"
```

Outputs: `native\build\AZS1HD.Foveated.Native.dll` and `src\FoveatedRendering\bin\Release\net6.0\AZS1HD.FoveatedRendering.dll`.
`GameDir` can also come from the `AZS_GAME_DIR` environment variable.

## License

Copyright (C) 2026 Mikael Järvinen.

Licensed under the [GNU General Public License v3.0](LICENSE) with the [additional terms](ADDITIONAL-TERMS.md):
you may modify and redistribute this mod, and versions you distribute must be released under the same license with
their source code, and must keep the credit to the original author. The additional terms also allow the mod to be
used with the game and its proprietary libraries. Third-party components keep their own licenses, see
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Credits

Approach informed by fholger's [vrperfkit](https://github.com/fholger/vrperfkit) /
[openvr_foveated](https://github.com/fholger/openvr_foveated), mbucchia's PimaxMagic4All and FarhanTheTinkerer's
[FrameEyePointer](https://github.com/FarhanTheTinkerer/FrameEyePointer).

Unofficial fan mod, not affiliated with or endorsed by Vertigo Games, Valve or NVIDIA.
