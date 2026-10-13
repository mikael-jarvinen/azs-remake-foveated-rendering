# Changelog

## 1.2.0
- Eye tracking on more headsets ([#1](https://github.com/mikael-jarvinen/azs-remake-foveated-rendering/issues/1)): the
  eye tracking binding is now merged into every headset binding the game ships (Index, Vive, Vive Pro, Vive Cosmos,
  Rift, Windows Mixed Reality). Those override SteamVR's generic fallback, so any eye tracker reported through one of
  those headset types was never delivered before. Every other headset uses the mod's `generic_hmd` binding. Headset
  types from installed SteamVR drivers (SteamVR's own, external drivers, drivers installed as Steam apps) are bound too.
  The game's files are backed up as `*.original` before their first change.
- Gaze diagnostics ([#2](https://github.com/mikael-jarvinen/azs-remake-foveated-rendering/issues/2)): the log names the
  connected headset and its type, whether it advertises eye gaze and what SteamVR bound the gaze action to, and after
  25 seconds without gaze explains the most likely cause.
- PSVR2 with [PSVR2Toolkit](https://github.com/BnuuySolutions/PSVR2Toolkit) is expected to work: its driver publishes
  gaze through SteamVR's standard eye tracking input.
- Build: the managed project references only BepInEx and Unity assemblies (no game assemblies), the native build finds
  Visual Studio through `vswhere`, release builds embed no local paths.

## 1.1.0
- Angular foveation: each shading tile knows its view direction from SteamVR's own projection, so the full-quality
  area is a true circle of `InnerDegrees` around the gaze, also on asymmetric lenses.
- Gaze matched to the frame: SteamVR's eye data for the next frame is paired with the compositor's render pose for
  that frame (previously latest sample + current pose, which lagged during head movement).
- `ShowOutline` debug rings drawn by the game camera along the gaze, independent of the shading code.
- Scene buffers without MSAA are recognised; fixed a VRAM leak; redundant VRS calls skipped; lock-free lookups;
  `BenchmarkSeconds` measures GPU time per frame with foveation on and off.
- Eye tracking binding for generic HMDs in addition to the Steam Frame.

## 1.0.1
- Vertical gaze axis was mirrored; only the gaze direction is used (the fixation distance is noisy); one
  shading-rate image per scene buffer size (the game switches between 1080 and 2160 on the Steam Frame).

## 1.0.0
- First version: NVIDIA VRS on the scene passes, gaze from SteamVR eye tracking.
