// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Mikael Järvinen
// Foveated Rendering for Arizona Sunshine Remake - https://github.com/mikael-jarvinen/azs-remake-foveated-rendering
// Licensed under GPL-3.0 with the additional terms in ADDITIONAL-TERMS.md (attribution, permission to run with the game).

using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.Json.Nodes;
using BepInEx.Configuration;
using Il2CppInterop.Runtime.Injection;
using UnityEngine;

namespace AZS1HD.FoveatedRendering;

/// <summary>
/// Eye-tracked foveated rendering (Variable Rate Shading through NVAPI, D3D11).
/// The heavy lifting is in AZS1HD.Foveated.Native.dll (see src/native/foveated.cpp). This side:
///  - adds an "eyetracking" action to the game's SteamVR action manifest so SteamVR hands us Steam Frame gaze,
///  - every frame tells the native side which texture is URP's scene color buffer and where the gaze is,
///  - issues a Unity render-thread plugin event so the shading-rate image is refreshed before the frame renders.
/// </summary>
internal static class FovNative
{
    const string Lib = "AZS1HD.Foveated.Native";
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int FovInit([MarshalAs(UnmanagedType.LPStr)] string logPath, [MarshalAs(UnmanagedType.LPStr)] string dumpPath);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int FovGazeInit([MarshalAs(UnmanagedType.LPStr)] string actionPath);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern void FovSetParams(int enabled, float innerDeg, float midDeg, int innerRate, int midRate, int outerRate, int flipRows, int noGazeMode, int dumpEnabled);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern void FovSetGazeDir(float x, float y, float z, int valid);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int FovAttachDevice(IntPtr anyTex, int eyeW, int eyeH, int eyeSlices);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int FovProjectHeadDir(float x, float y, float z, [Out] float[] uv);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern IntPtr FovGetRenderEventFunc();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern void FovRequestDump();
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern void FovGetStats(out int frames, out int enables, out int disables, out int skipped, out int vrsSupported, out int hookedContexts, out int descMatches);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int FovGazeUpdate([Out] float[] dir, out int status, out int mode);
    [DllImport(Lib, CallingConvention = CallingConvention.Cdecl)] public static extern int FovGetGpuTiming(int frames, out float preSubmitMs, out float totalRenderMs);

    public static string PluginDir;
    public static void Register()
    {
        NativeLibrary.SetDllImportResolver(typeof(FovNative).Assembly, (name, asm, path) =>
        {
            if (name != Lib) return IntPtr.Zero;
            return NativeLibrary.Load(Path.Combine(PluginDir, Lib + ".dll"));
        });
    }
}

internal static class Foveated
{
    internal enum GazeSourceKind { SteamVR, Simulated, Center, TestDirection }
    internal enum NoGazeKind { FullQuality, Center }

    internal static ConfigEntry<bool> Enabled;
    internal static ConfigEntry<GazeSourceKind> GazeSource;
    internal static ConfigEntry<float> InnerDegrees;
    internal static ConfigEntry<float> MidDegrees;
    internal static ConfigEntry<int> MidRate;
    internal static ConfigEntry<int> OuterRate;
    internal static ConfigEntry<NoGazeKind> NoGaze;
    internal static ConfigEntry<float> GazeHoldSeconds;
    internal static ConfigEntry<bool> ShowOutline;
    internal static ConfigEntry<bool> FlipRows;
    internal static ConfigEntry<string> TestDirection;
    internal static ConfigEntry<string> ActionPath;
    internal static ConfigEntry<float> DebugDumpInterval;
    internal static ConfigEntry<float> BenchmarkSeconds;
    internal static ConfigEntry<bool> Verbose;

    internal static bool nativeReady;
    internal static IntPtr renderEvent;
    internal static string logPath, dumpPath;
    internal static bool benchmarkVrsOn = true;

    internal static void Bind(ConfigFile config)
    {
        Enabled = config.Bind("General", "Enabled", true,
            "Eye-tracked foveated rendering via Variable Rate Shading. Needs an NVIDIA RTX / GTX 16xx GPU.");
        GazeSource = config.Bind("General", "GazeSource", GazeSourceKind.SteamVR,
            "SteamVR = real eye tracking (Steam Frame: SteamVR > Settings > Steam Link > share eye tracking data). " +
            "Simulated = moving test pattern. Center = straight ahead (fixed foveation). TestDirection = fixed direction below.");
        InnerDegrees = config.Bind("General", "InnerDegrees", 20f,
            new ConfigDescription("Full-resolution area: everything within this many degrees of where you look.", new AcceptableValueRange<float>(3f, 80f)));
        MidDegrees = config.Bind("General", "MidDegrees", 32f,
            new ConfigDescription("Up to this angle the middle shading rate is used; beyond it the outer rate.", new AcceptableValueRange<float>(3f, 120f)));
        MidRate = config.Bind("General", "MidRate", 1,
            new ConfigDescription("Shading rate between InnerDegrees and MidDegrees: 1 = 2x2 (quarter), 3 = 2x1, 4 = 1x2 (half).", new AcceptableValueList<int>(1, 3, 4)));
        OuterRate = config.Bind("General", "OuterRate", 2,
            new ConfigDescription("Shading rate beyond MidDegrees: 2 = 4x4 (1/16), 1 = 2x2.", new AcceptableValueList<int>(1, 2)));
        NoGaze = config.Bind("General", "WithoutGaze", NoGazeKind.FullQuality,
            "While no valid eye tracking is available: FullQuality = no foveation, Center = foveate around straight ahead.");
        GazeHoldSeconds = config.Bind("General", "GazeHoldSeconds", 0.4f,
            new ConfigDescription("Keep the last valid gaze this long when tracking drops out (blinks).", new AcceptableValueRange<float>(0f, 3f)));
        ShowOutline = config.Bind("Debug", "ShowOutline", false,
            "Draw the foveation boundaries in the headset: green ring = edge of the full-resolution area, yellow = edge of the 2x2 area, dot = gaze point. " +
            "The ring is placed by the game's own camera along the gaze direction, so it is an independent check of the mapping.");
        FlipRows = config.Bind("Debug", "FlipRows", false,
            "Troubleshooting only: treat eye texture rows as top-down. Use if the sharp area is mirrored vertically against the outline.");
        TestDirection = config.Bind("Debug", "TestDirection", "-0.35,0.35,-1",
            "For GazeSource = TestDirection: fixed gaze direction in head space (x right, y up, -z forward).");
        ActionPath = config.Bind("Debug", "GazeActionPath", "/actions/Vertigo/in/EyeGaze",
            "SteamVR action the mod adds to the game's action manifest for eye tracking.");
        DebugDumpInterval = config.Bind("Debug", "DumpIntervalSeconds", 0f,
            "Write BepInEx/foveation_dump.ppm (both eyes, upright, gaze marked) every N seconds. 0 = off.");
        BenchmarkSeconds = config.Bind("Debug", "BenchmarkSeconds", 0f,
            "If > 0, switch VRS on and off every N seconds and log the game's GPU time per frame (from SteamVR) for each state.");
        Verbose = config.Bind("Debug", "VerboseLogging", false, "Log statistics and gaze status every 5 seconds.");
    }

    internal static void Init(Plugin plugin, string pluginDir)
    {
        if (!Enabled.Value) { Plugin.Logger.LogInfo("Foveated rendering disabled in config."); return; }
        try
        {
            FovNative.PluginDir = pluginDir;
            FovNative.Register();
            string bepDir = Path.GetFullPath(Path.Combine(pluginDir, "..", ".."));
            logPath = Path.Combine(bepDir, "Foveated.log");
            dumpPath = Path.Combine(bepDir, "foveation_dump.ppm");
            int st = FovNative.FovInit(logPath, dumpPath);
            if (st != 0) { Plugin.Logger.LogWarning($"Foveated: NVAPI init failed ({st}); no NVIDIA GPU/driver? Feature off."); return; }
            renderEvent = FovNative.FovGetRenderEventFunc();
            PushParams();
            PatchActionManifest();
            nativeReady = true;
            ClassInjector.RegisterTypeInIl2Cpp<FoveatedBehaviour>();
            plugin.AddComponent<FoveatedBehaviour>();
            Plugin.Logger.LogInfo($"Foveated rendering armed: gaze {GazeSource.Value}, full rate within {InnerDegrees.Value:0}°, 2x2 to {MidDegrees.Value:0}°, outline {(ShowOutline.Value ? "on" : "off")}. Native log: {logPath}");
        }
        catch (Exception e)
        {
            Plugin.Logger.LogError($"Foveated init failed: {e}");
        }
    }

    internal static void PushParams()
    {
        float mid = Math.Max(MidDegrees.Value, InnerDegrees.Value);
        FovNative.FovSetParams(Enabled.Value && benchmarkVrsOn ? 1 : 0, InnerDegrees.Value, mid, 0, MidRate.Value, OuterRate.Value,
            FlipRows.Value ? 1 : 0, NoGaze.Value == NoGazeKind.Center ? 1 : 0, DebugDumpInterval.Value > 0 ? 1 : 0);
    }

    /// <summary>Adds an eye-tracking action and a Steam Frame HMD binding to the game's SteamVR action manifest.</summary>
    private static void PatchActionManifest()
    {
        try
        {
            string dir = Path.Combine(Application.streamingAssetsPath, "SteamVR");
            string manifestPath = Path.Combine(dir, "actions.json");
            if (!File.Exists(manifestPath)) { Plugin.Logger.LogWarning("Foveated: SteamVR actions.json not found, gaze unavailable."); return; }
            var root = JsonNode.Parse(File.ReadAllText(manifestPath))!.AsObject();
            string action = ActionPath.Value;
            string setName = action.Substring(0, action.IndexOf("/in/"));
            bool changed = false;

            var actions = root["actions"]!.AsArray();
            bool hasAction = false;
            foreach (var a in actions) if ((string)a!["name"]! == action) hasAction = true;
            if (!hasAction)
            {
                actions.Add(new JsonObject { ["name"] = action, ["type"] = "eyetracking", ["requirement"] = "optional" });
                changed = true;
            }
            var bindings = root["default_bindings"]!.AsArray();
            foreach (var type in new[] { "frame_hmd", "generic_hmd" })
            {
                bool has = false;
                foreach (var b in bindings) if ((string)b!["controller_type"]! == type) has = true;
                if (!has) { bindings.Add(new JsonObject { ["controller_type"] = type, ["binding_url"] = $"binding_{type}.json" }); changed = true; }
            }
            if (changed)
            {
                string backup = manifestPath + ".original";
                if (!File.Exists(backup)) File.Copy(manifestPath, backup);
                File.WriteAllText(manifestPath, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                Plugin.Logger.LogInfo($"Foveated: updated actions.json (eye tracking action {action} and HMD bindings)");
            }
            foreach (var type in new[] { "frame_hmd", "generic_hmd" })
            {
                string bindingPath = Path.Combine(dir, $"binding_{type}.json");
                if (File.Exists(bindingPath)) continue;
                var binding = new JsonObject
                {
                    ["action_manifest_version"] = 0,
                    ["alias_info"] = new JsonObject(),
                    ["category"] = "steamvr_input",
                    ["controller_type"] = type,
                    ["description"] = "Eye gaze for foveated rendering (Foveated Rendering mod)",
                    ["name"] = "Eye gaze",
                    ["options"] = new JsonObject(),
                    ["simulated_actions"] = new JsonArray(),
                    ["bindings"] = new JsonObject
                    {
                        [setName] = new JsonObject
                        {
                            ["eyetracking"] = new JsonArray(new JsonObject { ["output"] = action, ["path"] = "/user/head/eyetracking" })
                        }
                    }
                };
                File.WriteAllText(bindingPath, binding.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                Plugin.Logger.LogInfo($"Foveated: wrote binding_{type}.json");
            }
        }
        catch (Exception e)
        {
            Plugin.Logger.LogError($"Foveated: action manifest patch failed: {e}");
        }
    }
}

/// <summary>Per-frame driver. Runs in LateUpdate, i.e. after game logic and before this frame's cameras render.</summary>
internal class FoveatedBehaviour : MonoBehaviour
{
    public FoveatedBehaviour(IntPtr ptr) : base(ptr) { }

    private bool attached;
    private RenderTexture probe;
    private Camera xrCamera;
    private float nextStats, nextDump, nextGazeInit, nextBenchSample, benchPhaseEnd;
    private bool gazeInit;
    private int lastGazeStatus = int.MinValue, lastGazeMode;
    private readonly float[] dir = new float[3];
    private readonly float[] uv = new float[4];
    private Vector3 gazeHead = new Vector3(0, 0, -1);   // OpenVR head space
    private bool gazeValid;
    private Vector3 lastGood = new Vector3(0, 0, -1);
    private float lastGoodTime = -100f;
    private bool reportedValid;
    private float simT;
    private double benchOnSum, benchOffSum; private int benchOnN, benchOffN;
    private float phaseSkipUntil;

    // outline
    private GameObject outline;
    private Transform outlineParent;
    private float builtInner = -1, builtMid = -1;
    private const float OutlineDistance = 4f;

    private Camera FindXrCamera()
    {
        if (xrCamera != null && xrCamera.enabled && xrCamera.stereoEnabled) return xrCamera;
        xrCamera = null;
        var cams = Camera.allCameras;
        for (int i = 0; i < cams.Length; i++)
            if (cams[i] != null && cams[i].enabled && cams[i].stereoEnabled && cams[i].targetTexture == null) { xrCamera = cams[i]; break; }
        return xrCamera;
    }

    private void LateUpdate()
    {
        if (!Foveated.nativeReady) return;
        try
        {
            var cam = FindXrCamera();
            if (cam == null) return;

            if (!attached)
            {
                probe = new RenderTexture(8, 8, 0);
                probe.Create();
                var desc = UnityEngine.XR.XRSettings.eyeTextureDesc;
                int r = FovNative.FovAttachDevice(probe.GetNativeTexturePtr(), desc.width, desc.height, Math.Max(1, desc.volumeDepth));
                Plugin.Logger.LogInfo($"Foveated: device attach -> {r}; eye texture {desc.width}x{desc.height}, slices {desc.volumeDepth}, msaa {desc.msaaSamples}, {desc.graphicsFormat}");
                probe.Release(); probe = null;
                attached = true;
            }

            UpdateGaze();
            FovNative.FovSetGazeDir(gazeHead.x, gazeHead.y, gazeHead.z, gazeValid ? 1 : 0);
            GL.IssuePluginEvent(Foveated.renderEvent, 1);
            UpdateOutline(cam);

            float now = Time.unscaledTime;
            if (Foveated.DebugDumpInterval.Value > 0 && now >= nextDump)
            {
                nextDump = now + Foveated.DebugDumpInterval.Value;
                FovNative.FovRequestDump();
            }
            Benchmark(now);
            if (Foveated.Verbose.Value && now >= nextStats)
            {
                nextStats = now + 5f;
                FovNative.FovGetStats(out int frames, out int en, out int dis, out int skip, out int vrs, out int ctx, out int descM);
                FovNative.FovProjectHeadDir(gazeHead.x, gazeHead.y, gazeHead.z, uv);
                Plugin.Logger.LogInfo($"Foveated stats: frames {frames}, VRS on {en} / off {dis} (skipped {skip}) per 5s, scene binds {descM}, supported {vrs}, contexts {ctx}, " +
                    $"gaze {(gazeValid ? "valid" : "none")} status {lastGazeStatus} mode {lastGazeMode}, {Deg(gazeHead.x, gazeHead.z):0.0}° right {Deg(gazeHead.y, gazeHead.z):0.0}° up, uv L({uv[0]:0.00},{uv[1]:0.00}) R({uv[2]:0.00},{uv[3]:0.00})");
            }
        }
        catch (Exception e)
        {
            Plugin.Logger.LogError($"Foveated frame update failed: {e}");
        }
    }

    private static float Deg(float a, float z) => Mathf.Atan2(a, -z) * Mathf.Rad2Deg;

    private void Benchmark(float now)
    {
        float period = Foveated.BenchmarkSeconds.Value;
        if (period <= 0) return;
        if (benchPhaseEnd == 0) { benchPhaseEnd = now + period; phaseSkipUntil = now + 2f; Plugin.Logger.LogInfo($"Foveated benchmark: alternating VRS every {period:0}s"); }
        if (now >= nextBenchSample)
        {
            nextBenchSample = now + 1f;
            if (now >= phaseSkipUntil && FovNative.FovGetGpuTiming(60, out float pre, out float total) > 0)
            {
                if (Foveated.benchmarkVrsOn) { benchOnSum += pre; benchOnN++; } else { benchOffSum += pre; benchOffN++; }
            }
        }
        if (now >= benchPhaseEnd)
        {
            Plugin.Logger.LogInfo($"Foveated benchmark: app GPU time per frame with VRS ON {(benchOnN > 0 ? benchOnSum / benchOnN : 0):0.00} ms ({benchOnN} s), OFF {(benchOffN > 0 ? benchOffSum / benchOffN : 0):0.00} ms ({benchOffN} s)");
            Foveated.benchmarkVrsOn = !Foveated.benchmarkVrsOn;
            Foveated.PushParams();
            benchPhaseEnd = now + period;
            phaseSkipUntil = now + 2f;
        }
    }

    private void UpdateGaze()
    {
        float now = Time.unscaledTime;
        switch (Foveated.GazeSource.Value)
        {
            case Foveated.GazeSourceKind.Center:
                gazeHead = new Vector3(0, 0, -1); gazeValid = true; return;
            case Foveated.GazeSourceKind.TestDirection:
            {
                if (!gazeInit && now >= nextGazeInit) { nextGazeInit = now + 5f; FovNative.FovGazeInit(Foveated.ActionPath.Value); gazeInit = true; }
                var p = Foveated.TestDirection.Value.Split(',');
                var ci = System.Globalization.CultureInfo.InvariantCulture;
                gazeHead = new Vector3(float.Parse(p[0], ci), float.Parse(p[1], ci), float.Parse(p[2], ci)).normalized;
                gazeValid = true; return;
            }
            case Foveated.GazeSourceKind.Simulated:
            {
                simT += Time.unscaledDeltaTime;
                float yaw = 25f * Mathf.Sin(simT * 0.9f) * Mathf.Deg2Rad, pitch = 18f * Mathf.Sin(simT * 1.7f) * Mathf.Deg2Rad;
                gazeHead = new Vector3(Mathf.Sin(yaw) * Mathf.Cos(pitch), Mathf.Sin(pitch), -Mathf.Cos(yaw) * Mathf.Cos(pitch));
                gazeValid = true; return;
            }
            default:
            {
                if (!gazeInit && now >= nextGazeInit)
                {
                    nextGazeInit = now + 5f;   // retry until SteamVR input is up
                    gazeInit = FovNative.FovGazeInit(Foveated.ActionPath.Value) != 0;
                    Plugin.Logger.LogInfo($"Foveated: SteamVR eye tracking action init -> {(gazeInit ? "ok" : "not yet")}");
                }
                if (!gazeInit) { gazeValid = false; return; }
                int ok = FovNative.FovGazeUpdate(dir, out int status, out int mode);
                if (status != lastGazeStatus || mode != lastGazeMode)
                {
                    lastGazeStatus = status; lastGazeMode = mode;
                    Plugin.Logger.LogInfo($"Foveated: gaze status {status} (err*10 + active4 + valid2 + tracked1), timing mode {mode} (1 = matched to frame pose, 2 = latest sample), data {(ok != 0 ? "ok" : "none")}");
                }
                if (ok != 0)
                {
                    lastGood = new Vector3(dir[0], dir[1], dir[2]);
                    lastGoodTime = now;
                    if (!reportedValid) { reportedValid = true; Plugin.Logger.LogInfo($"Foveated: receiving eye tracking data, first sample {Deg(dir[0], dir[2]):0.0}° right {Deg(dir[1], dir[2]):0.0}° up"); }
                }
                gazeValid = ok != 0 || (now - lastGoodTime) <= Foveated.GazeHoldSeconds.Value;
                gazeHead = gazeValid ? lastGood : new Vector3(0, 0, -1);
                return;
            }
        }
    }

    // ---- outline ------------------------------------------------------------------------------------------------

    private static Material MakeMaterial(Color c)
    {
        string[] names = { "Hidden/Internal-Colored", "Unlit/Color", "UI/Default", "Sprites/Default" };
        foreach (var n in names)
        {
            var sh = Shader.Find(n);
            if (sh == null) continue;
            var m = new Material(sh);
            m.color = c;
            if (m.HasProperty("_ZTest")) m.SetInt("_ZTest", 8);        // always on top
            if (m.HasProperty("_ZWrite")) m.SetInt("_ZWrite", 0);
            if (m.HasProperty("_Cull")) m.SetInt("_Cull", 0);
            if (m.HasProperty("_SrcBlend")) m.SetInt("_SrcBlend", 5);
            if (m.HasProperty("_DstBlend")) m.SetInt("_DstBlend", 10);
            m.renderQueue = 4999;
            return m;
        }
        Plugin.Logger.LogWarning("Foveated: no usable shader for the outline");
        return null;
    }

    // Annulus at distance 1 whose inner and outer edges are at (deg - w) and (deg + w) from the +Z axis.
    private static Mesh RingMesh(float deg, float widthDeg, int seg = 128)
    {
        float r0 = Mathf.Tan((deg - widthDeg) * Mathf.Deg2Rad), r1 = Mathf.Tan((deg + widthDeg) * Mathf.Deg2Rad);
        var v = new Vector3[seg * 2];
        var t = new int[seg * 6];
        for (int i = 0; i < seg; i++)
        {
            float a = i * Mathf.PI * 2f / seg, c = Mathf.Cos(a), s = Mathf.Sin(a);
            v[i * 2] = new Vector3(c * r0, s * r0, 1f);
            v[i * 2 + 1] = new Vector3(c * r1, s * r1, 1f);
            int j = (i + 1) % seg;
            t[i * 6 + 0] = i * 2; t[i * 6 + 1] = j * 2; t[i * 6 + 2] = i * 2 + 1;
            t[i * 6 + 3] = i * 2 + 1; t[i * 6 + 4] = j * 2; t[i * 6 + 5] = j * 2 + 1;
        }
        var m = new Mesh();
        m.vertices = v; m.triangles = t;
        m.bounds = new Bounds(Vector3.zero, Vector3.one * 100f);
        return m;
    }

    private static void AddRing(GameObject parent, string name, Mesh mesh, Material mat, int layer)
    {
        var go = new GameObject(name);
        go.layer = layer;
        go.transform.SetParent(parent.transform, false);
        go.AddComponent<MeshFilter>().sharedMesh = mesh;
        var r = go.AddComponent<MeshRenderer>();
        r.sharedMaterial = mat;
        r.shadowCastingMode = UnityEngine.Rendering.ShadowCastingMode.Off;
        r.receiveShadows = false;
    }

    private void UpdateOutline(Camera cam)
    {
        if (!Foveated.ShowOutline.Value)
        {
            if (outline != null) outline.SetActive(false);
            return;
        }
        float inner = Foveated.InnerDegrees.Value, mid = Math.Max(Foveated.MidDegrees.Value, inner);
        if (outline == null || outlineParent != cam.transform || builtInner != inner || builtMid != mid)
        {
            if (outline != null) Destroy(outline);
            int layer = 0;
            int mask = cam.cullingMask;
            if ((mask & 1) == 0) for (int i = 0; i < 32; i++) if ((mask & (1 << i)) != 0) { layer = i; break; }
            outline = new GameObject("FoveationOutline");
            outline.layer = layer;
            outline.transform.SetParent(cam.transform, false);
            AddRing(outline, "Inner", RingMesh(inner, 0.25f), MakeMaterial(new Color(0.1f, 1f, 0.2f, 0.9f)), layer);
            AddRing(outline, "Mid", RingMesh(mid, 0.25f), MakeMaterial(new Color(1f, 0.85f, 0.1f, 0.8f)), layer);
            AddRing(outline, "Dot", RingMesh(0.6f, 0.6f, 24), MakeMaterial(new Color(1f, 0.1f, 0.1f, 0.95f)), layer);
            outlineParent = cam.transform; builtInner = inner; builtMid = mid;
            Plugin.Logger.LogInfo($"Foveated: outline created on layer {layer} under camera '{cam.name}'");
        }
        bool show = gazeValid || Foveated.NoGaze.Value == Foveated.NoGazeKind.Center;
        outline.SetActive(show);
        if (!show) return;
        // OpenVR head space (x right, y up, -z forward) -> Unity camera space (x right, y up, +z forward)
        var d = new Vector3(gazeHead.x, gazeHead.y, -gazeHead.z);
        outline.transform.localPosition = Vector3.zero;
        outline.transform.localRotation = Quaternion.LookRotation(d, Vector3.up);
        outline.transform.localScale = Vector3.one * OutlineDistance;
    }
}
