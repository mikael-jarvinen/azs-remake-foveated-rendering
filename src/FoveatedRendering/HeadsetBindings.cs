// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Mikael Järvinen
// Foveated Rendering for Arizona Sunshine Remake - https://github.com/mikael-jarvinen/azs-remake-foveated-rendering
// Licensed under GPL-3.0 with the additional terms in ADDITIONAL-TERMS.md (attribution, permission to run with the game).

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace AZS1HD.FoveatedRendering;

/// <summary>
/// Makes SteamVR deliver eye tracking to the game on any headset whose driver publishes it.
///
/// SteamVR only feeds an input action to an app through a binding for the connected device's controller type.
/// Eye tracking is an input on the headset itself (/user/head/eyetracking), which drivers usually add at runtime
/// without listing it in their input profile. SteamVR uses the app's binding for the headset's type, and falls
/// back to the app's generic_hmd binding when there is none. So:
///  - generic_hmd and frame_hmd get an eye tracking binding (covers every headset the game has no binding for),
///  - every headset type the game ships its own binding for (Index, Vive, Rift, WMR, ...) gets the eye tracking
///    entry merged into the game's file, because those override the generic fallback,
///  - headset types found in installed SteamVR drivers (SteamVR's own, registered external drivers, drivers
///    installed as Steam apps) are bound too, in case a future game update adds bindings for them.
/// SteamVR reads the manifest when the game starts VR, so this runs at plugin load. Originals are kept as
/// *.original. Bindings on headsets without eye tracking are ignored by SteamVR.
/// </summary>
internal static class HeadsetBindings
{
    internal static readonly string[] AlwaysBound = { "frame_hmd", "generic_hmd" };
    /// <summary>Headset controller types (as opposed to controllers and trackers) the game may ship bindings for.</summary>
    internal static readonly string[] KnownHeadsetTypes = { "frame_hmd", "generic_hmd", "indexhmd", "vive", "vive_pro", "vive_cosmos", "rift", "holographic_hmd" };
    internal static HashSet<string> BoundTypes = new(StringComparer.OrdinalIgnoreCase);
    private static readonly JsonSerializerOptions Indented = new() { WriteIndented = true };
    private const string OwnMarker = "Foveated Rendering mod";

    // ---------------------------------------------------------------- discovery

    /// <summary>Headset controller types from every SteamVR driver input profile that can be found.</summary>
    internal static SortedSet<string> DiscoverHeadsetTypes(List<string> log)
    {
        var types = new SortedSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var root in DriverRoots(log))
        {
            string input = Path.Combine(root, "resources", "input");
            if (!Directory.Exists(input)) continue;
            foreach (var file in SafeFiles(input, "*.json", SearchOption.AllDirectories))
            {
                string type = HeadsetTypeOf(file);
                if (type != null && types.Add(type)) log.Add($"headset type '{type}' from {file}");
            }
        }
        return types;
    }

    private static IEnumerable<string> SafeFiles(string dir, string pattern, SearchOption opt)
    {
        try { return Directory.GetFiles(dir, pattern, opt); } catch { return Array.Empty<string>(); }
    }

    private static IEnumerable<string> SafeDirs(string dir)
    {
        try { return Directory.GetDirectories(dir); } catch { return Array.Empty<string>(); }
    }

    /// <summary>controller_type of an input profile that describes a headset, otherwise null.</summary>
    internal static string HeadsetTypeOf(string file)
    {
        try
        {
            var info = new FileInfo(file);
            if (info.Length > 512 * 1024) return null;
            string text = Regex.Replace(File.ReadAllText(file), @"(?m)^\s*//.*$", "");
            if (!text.Contains("controller_type") || !text.Contains("input_source")) return null;
            var o = JsonNode.Parse(text, documentOptions: new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip }) as JsonObject;
            if (o == null) return null;
            string type = (string)o["controller_type"];
            string mode = (string)o["input_bindingui_mode"];
            string cls = (string)o["device_class"];
            bool hmd = string.Equals(mode, "hmd", StringComparison.OrdinalIgnoreCase) || string.Equals(cls, "TrackedDeviceClass_HMD", StringComparison.OrdinalIgnoreCase);
            return hmd && !string.IsNullOrWhiteSpace(type) ? type.Trim() : null;
        }
        catch { return null; }
    }

    private static IEnumerable<string> DriverRoots(List<string> log)
    {
        var roots = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var runtimes = new List<string>();
        try
        {
            string reg = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "openvr", "openvrpaths.vrpath");
            if (File.Exists(reg))
            {
                var o = JsonNode.Parse(File.ReadAllText(reg)) as JsonObject;
                if (o?["runtime"] is JsonArray rt) foreach (var r in rt) if (r != null) runtimes.Add((string)r);
                if (o?["external_drivers"] is JsonArray ext) foreach (var d in ext) if (d != null) roots.Add((string)d);
            }
            else log.Add($"{reg} not found");
        }
        catch (Exception e) { log.Add($"openvrpaths.vrpath unreadable: {e.Message}"); }

        var libraries = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var rt in runtimes)
        {
            foreach (var d in SafeDirs(Path.Combine(rt, "drivers"))) roots.Add(d);
            // ...\Steam\steamapps\common\SteamVR -> Steam library folders, for drivers shipped as Steam apps
            var steamapps = Directory.GetParent(rt)?.Parent;
            if (steamapps == null) continue;
            libraries.Add(steamapps.FullName);
            string vdf = Path.Combine(steamapps.Parent?.FullName ?? "", "steamapps", "libraryfolders.vdf");
            if (!File.Exists(vdf)) vdf = Path.Combine(steamapps.FullName, "libraryfolders.vdf");
            try
            {
                foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
                    libraries.Add(Path.Combine(m.Groups[1].Value.Replace("\\\\", "\\"), "steamapps"));
            }
            catch { }
        }
        foreach (var lib in libraries)
            foreach (var app in SafeDirs(Path.Combine(lib, "common")))
                FindDriverManifests(app, 3, roots);
        return roots;
    }

    private static void FindDriverManifests(string dir, int depth, HashSet<string> roots)
    {
        if (File.Exists(Path.Combine(dir, "driver.vrdrivermanifest"))) { roots.Add(dir); return; }
        if (depth <= 0) return;
        foreach (var d in SafeDirs(dir)) FindDriverManifests(d, depth - 1, roots);
    }

    // ---------------------------------------------------------------- manifest

    /// <summary>
    /// Adds the eye tracking action and a binding for each headset type. Returns the types that were newly bound.
    /// </summary>
    internal static List<string> Apply(string steamVrDir, string action, IEnumerable<string> headsetTypes, List<string> log)
    {
        var added = new List<string>();
        string manifestPath = Path.Combine(steamVrDir, "actions.json");
        if (!File.Exists(manifestPath)) { log.Add("actions.json not found, gaze unavailable"); return added; }
        string setName = action.Substring(0, action.IndexOf("/in/", StringComparison.Ordinal));

        var root = JsonNode.Parse(File.ReadAllText(manifestPath))!.AsObject();
        bool manifestChanged = false;

        var actions = root["actions"]!.AsArray();
        if (!actions.Any(a => (string)a!["name"] == action))
        {
            actions.Add(new JsonObject { ["name"] = action, ["type"] = "eyetracking", ["requirement"] = "optional" });
            manifestChanged = true;
        }

        var bindings = root["default_bindings"]!.AsArray();
        foreach (var type in headsetTypes.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            var existing = bindings.FirstOrDefault(b => string.Equals((string)b!["controller_type"], type, StringComparison.OrdinalIgnoreCase));
            string file = existing != null ? (string)existing["binding_url"] : $"binding_{SafeName(type)}.json";
            string bindingPath = Path.Combine(steamVrDir, file);
            if (existing == null)
            {
                bindings.Add(new JsonObject { ["controller_type"] = type, ["binding_url"] = file });
                manifestChanged = true;
            }
            try
            {
                if (EnsureEyeBinding(bindingPath, type, setName, action, existing != null))
                {
                    added.Add(type);
                    log.Add(existing != null ? $"eye tracking merged into the game's {file} ({type})" : $"eye tracking binding {file} written ({type})");
                }
            }
            catch (Exception e) { log.Add($"binding for {type} failed: {e.Message}"); }
            BoundTypes.Add(type);
        }

        // Everything the manifest binds the gaze action for, including types bound by earlier runs.
        foreach (var b in bindings)
        {
            string t = (string)b!["controller_type"], f = (string)b["binding_url"];
            try { if (t != null && f != null && File.ReadAllText(Path.Combine(steamVrDir, f)).Contains(action)) BoundTypes.Add(t); } catch { }
        }

        if (manifestChanged)
        {
            string backup = manifestPath + ".original";
            if (!File.Exists(backup)) File.Copy(manifestPath, backup);
            File.WriteAllText(manifestPath, root.ToJsonString(Indented));
            log.Add($"actions.json updated ({bindings.Count} default bindings)");
        }
        return added;
    }

    private static string SafeName(string type) => Regex.Replace(type, "[^A-Za-z0-9_.-]", "_");

    /// <summary>Makes sure the binding file binds the eye tracking action. Returns true when the file was changed.</summary>
    private static bool EnsureEyeBinding(string path, string type, string setName, string action, bool gameFile)
    {
        JsonObject doc;
        bool ours = false;
        if (File.Exists(path))
        {
            doc = JsonNode.Parse(File.ReadAllText(path))!.AsObject();
            ours = ((string)doc["description"] ?? "").Contains(OwnMarker);
        }
        else
        {
            doc = new JsonObject
            {
                ["action_manifest_version"] = 0,
                ["alias_info"] = new JsonObject(),
                ["category"] = "steamvr_input",
                ["controller_type"] = type,
                ["description"] = "Eye gaze for foveated rendering (" + OwnMarker + ")",
                ["name"] = "Eye gaze",
                ["options"] = new JsonObject(),
                ["simulated_actions"] = new JsonArray(),
                ["bindings"] = new JsonObject()
            };
        }
        if (doc["bindings"] is not JsonObject sets) { sets = new JsonObject(); doc["bindings"] = sets; }
        if (sets[setName] is not JsonObject set) { set = new JsonObject(); sets[setName] = set; }
        if (set["eyetracking"] is not JsonArray eye) { eye = new JsonArray(); set["eyetracking"] = eye; }
        if (eye.Any(e => (string)e!["output"] == action)) return false;
        // back up a file of the game (never one of ours) right before its first modification
        if (gameFile && !ours && File.Exists(path) && !File.Exists(path + ".original")) File.Copy(path, path + ".original");
        eye.Add(new JsonObject { ["output"] = action, ["path"] = "/user/head/eyetracking" });
        File.WriteAllText(path, doc.ToJsonString(Indented));
        return true;
    }
}
