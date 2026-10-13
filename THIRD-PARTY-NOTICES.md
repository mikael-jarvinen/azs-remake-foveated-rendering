# Third-party components

| Component | Where | License |
|---|---|---|
| [NVIDIA NVAPI SDK](https://github.com/NVIDIA/nvapi) headers and `nvapi64.lib` | `native/third_party/nvapi` | MIT, see `native/third_party/nvapi/License.txt` |
| [OpenVR SDK](https://github.com/ValveSoftware/openvr) header `openvr.h` | `native/third_party/openvr` | BSD-3-Clause, see `native/third_party/openvr/LICENSE` |

These files are included unmodified under their own licenses; the GPL of this project does not apply to them.
`AZS1HD.Foveated.Native.dll` statically links `nvapi64.lib`, so binary distributions must include both notices.

At runtime the mod uses [BepInEx](https://github.com/BepInEx/BepInEx) (LGPL-2.1),
[Il2CppInterop](https://github.com/BepInEx/Il2CppInterop) (LGPL-3.0) and [HarmonyX](https://github.com/BepInEx/HarmonyX) (MIT),
which are not part of this repository.
