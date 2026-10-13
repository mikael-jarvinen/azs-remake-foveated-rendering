// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Mikael Järvinen
// Foveated Rendering for Arizona Sunshine Remake - https://github.com/mikael-jarvinen/azs-remake-foveated-rendering
// Licensed under GPL-3.0 with the additional terms in ADDITIONAL-TERMS.md (attribution, permission to run with the game).

using BepInEx;
using BepInEx.Logging;
using BepInEx.Unity.IL2CPP;

namespace AZS1HD.FoveatedRendering;

/// <summary>
/// Eye-tracked foveated rendering for Arizona Sunshine Remake: NVIDIA Variable Rate Shading (D3D11, NVAPI)
/// driven by SteamVR eye tracking (Steam Frame). See Foveated.cs and src/native/foveated.cpp.
/// </summary>
[BepInPlugin(Guid, Name, Version)]
public class Plugin : BasePlugin
{
    public const string Guid = "azs1hd.foveatedrendering";
    public const string Name = "Foveated Rendering";
    public const string Version = "1.1.0";

    internal static ManualLogSource Logger;

    public override void Load()
    {
        Logger = Log;
        Foveated.Bind(Config);
        Foveated.Init(this, System.IO.Path.GetDirectoryName(typeof(Plugin).Assembly.Location));
        Log.LogInfo($"{Name} v{Version} loaded.");
    }
}
