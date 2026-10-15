// NgParity: builds small test scenes from code, exports them with the Unity
// SplashEdit 2.4.0 exporter (twice, from freshly built scenes) and dumps the
// same scene in the splashedit-ng text format (docs/scene-format.md).
//
// Copy into <project>/Assets/Editor/ and run via
//   unity build <project> --target StandaloneLinux64 --execute-method NgParity.Run \
//     --args "-ngParityOut <out> [-ngParityCases c01_cube_flat,c03_textures]"
// (or set NGPARITY_OUT / NGPARITY_CASES in the environment).
//
// Output per case:  <out>/<case>/unity/scene.{splashpack,vram,spu}
//                   <out>/<case>/unity-run2/...
//                   <out>/<case>/ir/scene.scene, ir/assets/*.mesh, ir/assets/*.png
//                   <out>/<case>/log-<run>.txt (warnings/errors logged during export)
// plus <out>/summary.txt.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using SplashEdit.RuntimeCode;
using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

public static class NgParity
{
    const string TexDir = "Assets/Textures";
    static readonly CultureInfo Inv = CultureInfo.InvariantCulture;

    // ------------------------------------------------------------------ entry

    public static void Run()
    {
        int code = 0;
        var summary = new StringBuilder();
        try
        {
            string outRoot = Arg("-ngParityOut") ?? Environment.GetEnvironmentVariable("NGPARITY_OUT")
                             ?? Path.Combine(Directory.GetCurrentDirectory(), "ng-parity-out");
            string filter = Arg("-ngParityCases") ?? Environment.GetEnvironmentVariable("NGPARITY_CASES");
            var only = string.IsNullOrEmpty(filter) ? null : new HashSet<string>(filter.Split(','));
            Directory.CreateDirectory(outRoot);
            Debug.Log("NGPARITY out=" + outRoot + " cases=" + (filter ?? "all"));

            GenerateTextures();

            foreach (var (name, build) in Cases())
            {
                if (only != null && !only.Contains(name)) continue;
                string caseDir = Path.Combine(outRoot, name);
                if (Directory.Exists(caseDir)) Directory.Delete(caseDir, true);
                Directory.CreateDirectory(caseDir);
                summary.Append(name);
                foreach (string run in new[] { "unity", "unity-run2" })
                {
                    string result = RunOnce(caseDir, run, build, dumpIr: run == "unity");
                    summary.Append("  " + run + ": " + result);
                }
                summary.AppendLine();
            }
            File.WriteAllText(Path.Combine(outRoot, "summary.txt"), summary.ToString());
            Debug.Log("NGPARITY summary:\n" + summary);
        }
        catch (Exception e)
        {
            Debug.LogError("NGPARITY fatal: " + e);
            code = 1;
        }
        EditorApplication.Exit(code);
    }

    static string Arg(string key)
    {
        var a = Environment.GetCommandLineArgs();
        for (int i = 0; i < a.Length - 1; i++) if (a[i] == key) return a[i + 1];
        return null;
    }

    static string RunOnce(string caseDir, string run, Action build, bool dumpIr)
    {
        string runDir = Path.Combine(caseDir, run);
        Directory.CreateDirectory(runDir);
        var log = new StringBuilder();
        Application.LogCallback cb = (msg, st, type) =>
        {
            if (type != LogType.Log) log.AppendLine(type + ": " + msg + (type == LogType.Exception ? "\n" + st : ""));
        };
        Application.logMessageReceived += cb;
        string result;
        try
        {
            var scene = EditorSceneManager.NewScene(NewSceneSetup.EmptyScene, NewSceneMode.Single);
            var exporterGo = new GameObject("SceneExporter");
            var sceneExp = exporterGo.AddComponent<PSXSceneExporter>();
            build();

            string irDir = Path.Combine(caseDir, "ir");
            List<Texture2D> textures = null;
            if (dumpIr) textures = DumpScene(scene, sceneExp, irDir); // before export: meshes as authored

            string pack = Path.Combine(runDir, "scene.splashpack");
            sceneExp.ExportToPath(pack);

            if (dumpIr) DumpTextures(textures, Path.Combine(irDir, "assets")); // after export: importer forced
            result = File.Exists(pack) ? "ok " + new FileInfo(pack).Length + "B" : "FAILED (no splashpack written)";
        }
        catch (Exception e)
        {
            result = "FAILED " + e.GetType().Name + ": " + e.Message.Split('\n')[0];
            log.AppendLine("Harness exception: " + e);
        }
        finally
        {
            Application.logMessageReceived -= cb;
        }
        File.WriteAllText(Path.Combine(caseDir, "log-" + run + ".txt"), log.ToString());
        Debug.Log("NGPARITY " + Path.GetFileName(caseDir) + "/" + run + ": " + result);
        return result;
    }

    // ------------------------------------------------------------------ cases

    static IEnumerable<(string, Action)> Cases()
    {
        yield return ("c01_cube_flat", () =>
        {
            var go = Prim(PrimitiveType.Cube, "Cube01", new Vector3(1, 2, 3), Quaternion.identity, Vector3.one);
            Exp(go, colorMode: VertexColorMode.FlatColor, flat: new Color32(200, 100, 50, 255));
        });

        yield return ("c02_transforms", () =>
        {
            var box = ColorBox();
            var a = Obj("BoxA", box, new Vector3(-1, 0.5f, 2), Quaternion.Euler(30, 45, 60), new Vector3(2, 1, 0.5f), DefaultMat());
            Exp(a, colorMode: VertexColorMode.MeshVertexColors);
            var b = Prim(PrimitiveType.Cube, "CubeB", new Vector3(0.25f, -0.75f, 1.5f), Quaternion.Euler(0, 90, 0), new Vector3(1, 3, 1));
            Exp(b, colorMode: VertexColorMode.FlatColor, flat: new Color32(10, 220, 90, 255));
            var c = Obj("BoxC", box, new Vector3(1.5f, 1, -0.5f), Quaternion.Euler(-10, 200, 5), new Vector3(0.5f, 0.5f, 2), DefaultMat());
            Exp(c, colorMode: VertexColorMode.MeshVertexColors, active: false);
        });

        yield return ("c03_textures", () =>
        {
            var q4 = Prim(PrimitiveType.Quad, "Quad4", new Vector3(-2, 0, 0), Quaternion.identity, Vector3.one, TexMat("t03_pal7_16x16"));
            Exp(q4, colorMode: VertexColorMode.FlatColor, bpp: PSXBPP.TEX_4BIT);
            var q8 = Prim(PrimitiveType.Quad, "Quad8", new Vector3(0, 0, 0), Quaternion.identity, Vector3.one, TexMat("t03_pal100_32x32"));
            Exp(q8, colorMode: VertexColorMode.FlatColor, bpp: PSXBPP.TEX_8BIT);
            var q16 = Prim(PrimitiveType.Quad, "Quad16", new Vector3(2, 0, 0), Quaternion.identity, new Vector3(2, 1, 1), TexMat("t03_rgb_32x16"));
            Exp(q16, colorMode: VertexColorMode.FlatColor, bpp: PSXBPP.TEX_16BIT);
        });

        yield return ("c04_kmeans", () =>
        {
            var k8 = Prim(PrimitiveType.Quad, "QuadK8", new Vector3(-1, 0, 0), Quaternion.identity, Vector3.one, TexMat("t04_grad_64x64"));
            Exp(k8, colorMode: VertexColorMode.FlatColor, bpp: PSXBPP.TEX_8BIT);
            var k4 = Prim(PrimitiveType.Quad, "QuadK4", new Vector3(1, 0, 0), Quaternion.identity, Vector3.one, TexMat("t04_many_32x32"));
            Exp(k4, colorMode: VertexColorMode.FlatColor, bpp: PSXBPP.TEX_4BIT);
        });

        yield return ("c05_colliders", () =>
        {
            var d = Prim(PrimitiveType.Cube, "CubeDynamic", new Vector3(-2, 0, 0), Quaternion.Euler(0, 30, 0), Vector3.one);
            Exp(d, colorMode: VertexColorMode.FlatColor, collision: PSXCollisionType.Dynamic);
            var s = Prim(PrimitiveType.Cube, "CubeStatic", new Vector3(0, 0, 0), Quaternion.identity, new Vector3(3, 0.5f, 3));
            Exp(s, colorMode: VertexColorMode.FlatColor, collision: PSXCollisionType.Static);
            var n = Prim(PrimitiveType.Cube, "CubeNone", new Vector3(2, 1, 0), Quaternion.identity, Vector3.one);
            Exp(n, colorMode: VertexColorMode.FlatColor, collision: PSXCollisionType.None);
        });

        yield return ("c06_baked_light", () =>
        {
            var plane = Prim(PrimitiveType.Plane, "Ground", Vector3.zero, Quaternion.identity, Vector3.one);
            Exp(plane);
            var sphere = Prim(PrimitiveType.Sphere, "Ball", new Vector3(0, 1, 0), Quaternion.identity, Vector3.one);
            Exp(sphere);
            var sun = new GameObject("Sun");
            sun.transform.rotation = Quaternion.Euler(50, -30, 0);
            var sl = sun.AddComponent<Light>();
            sl.type = LightType.Directional; sl.intensity = 1f; sl.color = new Color(1f, 0.9f, 0.8f);
            var lamp = new GameObject("Lamp");
            lamp.transform.position = new Vector3(1.5f, 2.5f, -1f);
            var pl = lamp.AddComponent<Light>();
            pl.type = LightType.Point; pl.range = 10f; pl.intensity = 2f; pl.color = Color.white;
        });

        yield return ("c07_multimat", () =>
        {
            var mesh = TwoMatBox();
            var tint = NewMat();
            SetColor(tint, new Color(0.2f, 0.6f, 1f, 1f));
            var go = Obj("TwoMat", mesh, new Vector3(0, 1, 0), Quaternion.Euler(0, 20, 0), Vector3.one, TexMat("t03_pal7_16x16"), tint);
            Exp(go, colorMode: VertexColorMode.FlatColor, bpp: PSXBPP.TEX_8BIT);
        });
    }

    // ------------------------------------------------------------ scene help

    static GameObject Prim(PrimitiveType t, string name, Vector3 pos, Quaternion rot, Vector3 scale, params Material[] mats)
    {
        var go = GameObject.CreatePrimitive(t);
        go.name = name;
        var col = go.GetComponent<Collider>();
        if (col != null) UnityEngine.Object.DestroyImmediate(col);
        go.transform.SetPositionAndRotation(pos, rot);
        go.transform.localScale = scale;
        if (mats.Length > 0) go.GetComponent<MeshRenderer>().sharedMaterials = mats;
        return go;
    }

    static GameObject Obj(string name, Mesh mesh, Vector3 pos, Quaternion rot, Vector3 scale, params Material[] mats)
    {
        var go = new GameObject(name);
        go.AddComponent<MeshFilter>().sharedMesh = mesh;
        go.AddComponent<MeshRenderer>().sharedMaterials = mats;
        go.transform.SetPositionAndRotation(pos, rot);
        go.transform.localScale = scale;
        return go;
    }

    static void Exp(GameObject go, VertexColorMode colorMode = VertexColorMode.BakedLighting, Color32? flat = null,
        PSXBPP bpp = PSXBPP.TEX_8BIT, PSXCollisionType collision = PSXCollisionType.None, bool active = true)
    {
        var exp = go.AddComponent<PSXObjectExporter>();
        var so = new SerializedObject(exp);
        so.FindProperty("isActive").boolValue = active;
        so.FindProperty("bitDepth").intValue = (int)bpp;
        so.FindProperty("collisionType").intValue = (int)collision;
        so.FindProperty("vertexColorMode").intValue = (int)colorMode;
        if (flat.HasValue) so.FindProperty("flatVertexColor").colorValue = flat.Value;
        so.ApplyModifiedPropertiesWithoutUndo();
    }

    static Shader LitShader()
    {
        return Shader.Find("Universal Render Pipeline/Lit") ?? Shader.Find("Standard");
    }

    static Material NewMat() => new Material(LitShader());

    static Material DefaultMat()
    {
        var m = NewMat();
        SetColor(m, Color.white);
        return m;
    }

    static void SetColor(Material m, Color c)
    {
        if (m.HasProperty("_BaseColor")) m.SetColor("_BaseColor", c);
        if (m.HasProperty("_Color")) m.SetColor("_Color", c);
    }

    static Material TexMat(string texName)
    {
        var tex = AssetDatabase.LoadAssetAtPath<Texture2D>(TexDir + "/" + texName + ".png");
        if (tex == null) throw new Exception("texture missing: " + texName);
        var m = NewMat();
        m.name = "M_" + texName;
        m.mainTexture = tex;
        SetColor(m, Color.white);
        return m;
    }

    // 24-vertex box, per-face normals, uv, per-vertex colours.
    static void BoxData(out Vector3[] v, out Vector3[] n, out Vector2[] uv, out Color[] col, out int[][] faceTris)
    {
        var faces = new[]
        {
            (Vector3.right, Vector3.forward, Vector3.up), (Vector3.left, Vector3.back, Vector3.up),
            (Vector3.up, Vector3.right, Vector3.forward), (Vector3.down, Vector3.right, Vector3.back),
            (Vector3.forward, Vector3.left, Vector3.up), (Vector3.back, Vector3.right, Vector3.up),
        };
        v = new Vector3[24]; n = new Vector3[24]; uv = new Vector2[24]; col = new Color[24];
        faceTris = new int[6][];
        var corners = new[] { new Vector2(-1, -1), new Vector2(1, -1), new Vector2(1, 1), new Vector2(-1, 1) };
        for (int f = 0; f < 6; f++)
        {
            var (nrm, tu, tv) = faces[f];
            for (int k = 0; k < 4; k++)
            {
                int i = f * 4 + k;
                // Winding is not normalised here on purpose: the exporter flips any
                // triangle whose face normal disagrees with normals[v0].
                v[i] = 0.5f * nrm + 0.5f * corners[k].x * tu + 0.5f * corners[k].y * tv;
                n[i] = nrm;
                uv[i] = new Vector2((corners[k].x + 1) * 0.5f, (corners[k].y + 1) * 0.5f);
                col[i] = new Color((f * 4 + k) / 23f, 1f - f / 5f, (k % 2) * 0.75f + 0.1f, 1f);
            }
            int b = f * 4;
            faceTris[f] = new[] { b, b + 2, b + 1, b, b + 3, b + 2 };
        }
    }

    static Mesh ColorBox()
    {
        BoxData(out var v, out var n, out var uv, out var col, out var ft);
        var m = new Mesh { name = "ColorBox" };
        m.vertices = v; m.normals = n; m.uv = uv; m.colors = col;
        m.triangles = ft.SelectMany(x => x).ToArray();
        m.RecalculateBounds();
        return m;
    }

    static Mesh TwoMatBox()
    {
        BoxData(out var v, out var n, out var uv, out _, out var ft);
        var m = new Mesh { name = "TwoMatBox" };
        m.vertices = v; m.normals = n; m.uv = uv;
        m.subMeshCount = 2;
        m.SetTriangles(ft.Take(3).SelectMany(x => x).ToArray(), 0);
        m.SetTriangles(ft.Skip(3).SelectMany(x => x).ToArray(), 1);
        m.RecalculateBounds();
        return m;
    }

    // --------------------------------------------------------------- textures

    static void GenerateTextures()
    {
        // Regenerated every invocation (meta deleted too) so the exporter always
        // starts from Unity's default importer settings and forces its own.
        if (AssetDatabase.IsValidFolder(TexDir)) AssetDatabase.DeleteAsset(TexDir);
        AssetDatabase.CreateFolder("Assets", "Textures");

        var pal7 = new[]
        {
            new Color32(255, 0, 0, 255), new Color32(0, 255, 0, 255), new Color32(0, 0, 255, 255),
            new Color32(255, 255, 0, 255), new Color32(0, 255, 255, 255), new Color32(255, 0, 255, 255),
            new Color32(40, 40, 40, 255),
        };
        WritePng("t03_pal7_16x16", 16, 16, (x, y) => pal7[((x / 3) + (y / 5)) % 7]);
        WritePng("t03_pal100_32x32", 32, 32, (x, y) =>
        {
            int i = (x * 7 + y * 13) % 100;
            return new Color32((byte)(20 + (i % 10) * 23), (byte)(20 + (i / 10) * 23), (byte)((i * 53) % 256), 255);
        });
        WritePng("t03_rgb_32x16", 32, 16, (x, y) =>
        {
            if (x == 0 && y == 0) return new Color32(0, 0, 0, 255);      // opaque black -> 0x8421 rule
            if (x == 1 && y == 0) return new Color32(255, 0, 255, 0);    // transparent magenta
            if (x == 2 && y == 0) return new Color32(0, 0, 0, 0);        // transparent black
            return new Color32((byte)(x * 8), (byte)(y * 16), 128, 255);
        });
        WritePng("t04_grad_64x64", 64, 64, (x, y) => new Color32((byte)(x * 4), (byte)(y * 4), (byte)(((x ^ y) & 63) * 4), 255));
        WritePng("t04_many_32x32", 32, 32, (x, y) => new Color32((byte)(x * 8), (byte)(y * 8), 64, 255));
        AssetDatabase.Refresh();
    }

    static void WritePng(string name, int w, int h, Func<int, int, Color32> px)
    {
        var t = new Texture2D(w, h, TextureFormat.RGBA32, false);
        var p = new Color32[w * h];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                p[y * w + x] = px(x, y); // y = 0 is the bottom row in Unity
        t.SetPixels32(p);
        t.Apply();
        string path = TexDir + "/" + name + ".png";
        File.WriteAllBytes(path, t.EncodeToPNG());
        UnityEngine.Object.DestroyImmediate(t);
        AssetDatabase.ImportAsset(path, ImportAssetOptions.ForceUpdate);
    }

    // --------------------------------------------------------------- IR dump

    static List<Texture2D> DumpScene(UnityEngine.SceneManagement.Scene scene, PSXSceneExporter se, string irDir)
    {
        string assets = Path.Combine(irDir, "assets");
        Directory.CreateDirectory(assets);
        var meshNames = new Dictionary<Mesh, string>();
        var texNames = new Dictionary<Texture2D, string>();

        var settings = new J.Obj()
            .Add("gteScaling", se.GTEScaling)
            .Add("sceneType", se.SceneType == PSXSceneType.Interior ? "interior" : "exterior")
            .Add("fog", new J.Obj().Add("enabled", se.FogEnabled)
                .Add("color", J.Arr(se.FogColor.r, se.FogColor.g, se.FogColor.b))
                .Add("density", se.FogDensity))
            .Add("networkId", se.SceneNetworkId ?? "")
            .Add("script", se.SceneLuaFile != null ? (object)("scripts/" + se.SceneLuaFile.name + ".lua") : null);

        var objects = new List<object>();
        foreach (var go in scene.GetRootGameObjects())
        {
            if (go.GetComponent<PSXSceneExporter>() != null) continue;
            objects.Add(DumpObject(go.transform, true, assets, meshNames, texNames));
        }

        var root = new J.Obj()
            .Add("format", "splashedit-ng/scene")
            .Add("version", 1)
            .Add("settings", settings)
            .Add("objects", objects);
        File.WriteAllText(Path.Combine(irDir, "scene.scene"), J.Write(root) + "\n");
        return texNames.Keys.ToList();
    }

    static J.Obj DumpObject(Transform t, bool isRoot, string assets, Dictionary<Mesh, string> meshNames, Dictionary<Texture2D, string> texNames)
    {
        var exp = t.GetComponent<PSXObjectExporter>();
        // ng's "active" is the GameObject's own flag (off: not exported at all);
        // PSXObjectExporter.IsActive is the start-active flag (flag bit 0).
        bool active = t.gameObject.activeSelf;
        bool startActive = exp == null || exp.IsActive;
        Vector3 p = isRoot ? t.position : t.localPosition;
        Quaternion q = isRoot ? t.rotation : t.localRotation;
        Vector3 s = isRoot ? t.lossyScale : t.localScale;

        var comps = new List<object>();
        var mf = t.GetComponent<MeshFilter>();
        var mr = t.GetComponent<MeshRenderer>();
        if (exp != null && mf != null && mf.sharedMesh != null)
        {
            var mats = new List<object>();
            var shared = mr != null ? mr.sharedMaterials : new Material[0];
            foreach (var m in shared)
            {
                Texture2D tex = m != null ? m.mainTexture as Texture2D : null;
                Color c = Color.white;
                if (m != null)
                {
                    if (m.HasProperty("_BaseColor")) c = m.GetColor("_BaseColor");
                    else if (m.HasProperty("_Color")) c = m.color;
                }
                mats.Add(new J.Obj()
                    .Add("texture", tex != null ? (object)("assets/" + TexName(tex, texNames) + ".png") : null)
                    .Add("color", J.Arr(c.r, c.g, c.b, c.a)));
            }
            var so = new SerializedObject(exp);
            int bpp = so.FindProperty("bitDepth").intValue;
            var fc = exp.FlatVertexColor;
            comps.Add(new J.Obj()
                .Add("type", "mesh")
                .Add("mesh", "assets/" + MeshName(mf.sharedMesh, meshNames, assets) + ".mesh")
                .Add("materials", mats)
                .Add("bitDepth", bpp == (int)PSXBPP.TEX_16BIT ? 16 : bpp)
                .Add("vertexColors", exp.ColorMode == VertexColorMode.FlatColor ? "flat"
                                   : exp.ColorMode == VertexColorMode.MeshVertexColors ? "mesh" : "baked")
                .Add("flatColor", J.Arr((int)fc.r, (int)fc.g, (int)fc.b))
                .Add("smoothNormals", exp.SmoothNormals)
                .Add("uvOffsetMaterial", exp.UVOffsetMaterial));
        }
        if (exp != null)
        {
            comps.Add(new J.Obj()
                .Add("type", "collider")
                .Add("kind", exp.CollisionType == PSXCollisionType.Dynamic ? "dynamic"
                           : exp.CollisionType == PSXCollisionType.Static ? "static" : "none")
                .Add("platform", exp.IsPlatform));
            if (exp.LuaFile != null)
                comps.Add(new J.Obj().Add("type", "script").Add("lua", "scripts/" + exp.LuaFile.name + ".lua"));
        }
        var light = t.GetComponent<Light>();
        if (light != null)
        {
            comps.Add(new J.Obj()
                .Add("type", "light")
                .Add("kind", light.type.ToString().ToLowerInvariant())
                .Add("color", J.Arr(light.color.r, light.color.g, light.color.b))
                .Add("intensity", light.intensity)
                .Add("range", light.range)
                .Add("spotAngle", light.spotAngle)
                .Add("innerSpotAngle", light.innerSpotAngle));
        }

        var children = new List<object>();
        for (int i = 0; i < t.childCount; i++)
            children.Add(DumpObject(t.GetChild(i), false, assets, meshNames, texNames));

        var o = new J.Obj()
            .Add("name", t.gameObject.name)
            .Add("active", active);
        // ng's scene writer leaves startActive out when it is on.
        if (!startActive) o.Add("startActive", false);
        return o
            .Add("transform", new J.Obj()
                .Add("position", J.Arr(p.x, p.y, p.z))
                .Add("rotation", J.Arr(q.x, q.y, q.z, q.w))
                .Add("scale", J.Arr(s.x, s.y, s.z)))
            .Add("components", comps)
            .Add("children", children);
    }

    static string Safe(string n)
    {
        var sb = new StringBuilder();
        foreach (char c in n) sb.Append(char.IsLetterOrDigit(c) || c == '_' || c == '-' ? c : '_');
        return sb.Length == 0 ? "unnamed" : sb.ToString();
    }

    static string Unique(string baseName, ICollection<string> used)
    {
        string n = baseName; int k = 2;
        while (used.Contains(n)) n = baseName + "_" + k++;
        return n;
    }

    static string TexName(Texture2D tex, Dictionary<Texture2D, string> names)
    {
        if (!names.TryGetValue(tex, out var n))
            names[tex] = n = Unique(Safe(tex.name), names.Values);
        return n;
    }

    static string MeshName(Mesh mesh, Dictionary<Mesh, string> names, string assets)
    {
        if (names.TryGetValue(mesh, out var n)) return n;
        names[mesh] = n = Unique(Safe(mesh.name), names.Values);

        var o = new J.Obj().Add("format", "splashedit-ng/mesh").Add("version", 1);
        var pos = new List<object>();
        foreach (var v in mesh.vertices) { pos.Add(v.x); pos.Add(v.y); pos.Add(v.z); }
        o.Add("positions", pos);
        var nr = mesh.normals;
        if (nr != null && nr.Length > 0)
        {
            var l = new List<object>();
            foreach (var v in nr) { l.Add(v.x); l.Add(v.y); l.Add(v.z); }
            o.Add("normals", l);
        }
        var uv = mesh.uv;
        if (uv != null && uv.Length > 0)
        {
            var l = new List<object>();
            foreach (var v in uv) { l.Add(v.x); l.Add(v.y); }
            o.Add("uv", l);
        }
        var col = mesh.colors;
        if (col != null && col.Length > 0)
        {
            var l = new List<object>();
            foreach (var c in col) { l.Add(c.r); l.Add(c.g); l.Add(c.b); l.Add(c.a); }
            o.Add("colors", l);
        }
        var subs = new List<object>();
        for (int i = 0; i < mesh.subMeshCount; i++)
            subs.Add(mesh.GetTriangles(i).Cast<object>().ToList());
        o.Add("submeshes", subs);
        File.WriteAllText(Path.Combine(assets, n + ".mesh"), J.Write(o) + "\n");
        return n;
    }

    static void DumpTextures(List<Texture2D> textures, string assets)
    {
        var names = new Dictionary<Texture2D, string>();
        foreach (var tex in textures)
        {
            // Re-encode as 8-bit RGBA regardless of the imported format.
            var copy = new Texture2D(tex.width, tex.height, TextureFormat.RGBA32, false);
            copy.SetPixels32(tex.GetPixels32());
            copy.Apply();
            File.WriteAllBytes(Path.Combine(assets, TexName(tex, names) + ".png"), copy.EncodeToPNG());
            UnityEngine.Object.DestroyImmediate(copy);
        }
    }

    // ------------------------------------------------------------ JSON writer

    static class J
    {
        public class Obj
        {
            public readonly List<KeyValuePair<string, object>> Items = new List<KeyValuePair<string, object>>();
            public Obj Add(string k, object v) { Items.Add(new KeyValuePair<string, object>(k, v)); return this; }
        }

        public static List<object> Arr(params object[] xs) => xs.ToList();

        public static string Write(object v)
        {
            var sb = new StringBuilder();
            Emit(sb, v, 0);
            return sb.ToString();
        }

        static bool IsScalar(object v) => !(v is Obj) && !(v is List<object>);

        static void Emit(StringBuilder sb, object v, int ind)
        {
            switch (v)
            {
                case null: sb.Append("null"); break;
                case bool b: sb.Append(b ? "true" : "false"); break;
                case string s: Str(sb, s); break;
                case float f: sb.Append(f.ToString("R", Inv)); break;
                case int i: sb.Append(i.ToString(Inv)); break;
                case Obj o:
                    if (o.Items.Count == 0) { sb.Append("{}"); break; }
                    sb.Append("{\n");
                    for (int k = 0; k < o.Items.Count; k++)
                    {
                        sb.Append(' ', (ind + 1) * 2);
                        Str(sb, o.Items[k].Key);
                        sb.Append(": ");
                        Emit(sb, o.Items[k].Value, ind + 1);
                        if (k < o.Items.Count - 1) sb.Append(',');
                        sb.Append('\n');
                    }
                    sb.Append(' ', ind * 2).Append('}');
                    break;
                case List<object> l:
                    if (l.Count == 0) { sb.Append("[]"); break; }
                    if (l.All(IsScalar))
                    {
                        sb.Append('[');
                        for (int k = 0; k < l.Count; k++) { if (k > 0) sb.Append(", "); Emit(sb, l[k], ind); }
                        sb.Append(']');
                        break;
                    }
                    sb.Append("[\n");
                    for (int k = 0; k < l.Count; k++)
                    {
                        sb.Append(' ', (ind + 1) * 2);
                        Emit(sb, l[k], ind + 1);
                        if (k < l.Count - 1) sb.Append(',');
                        sb.Append('\n');
                    }
                    sb.Append(' ', ind * 2).Append(']');
                    break;
                default: throw new Exception("J: unsupported " + v.GetType());
            }
        }

        static void Str(StringBuilder sb, string s)
        {
            sb.Append('"');
            foreach (char c in s)
            {
                if (c == '"') sb.Append("\\\"");
                else if (c == '\\') sb.Append("\\\\");
                else if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                else sb.Append(c);
            }
            sb.Append('"');
        }
    }
}
