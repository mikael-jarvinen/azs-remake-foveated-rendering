# Changelog

## Unreleased
- Repository build: the managed project references only BepInEx and Unity assemblies (no game assemblies), the native
  build finds Visual Studio through `vswhere`, release builds embed no local paths.

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
