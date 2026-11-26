// Builds xenowerk.exe on the player's PC from their own copy of the game.
// The release ships only our code: the recompiler (recomp\lifter, Python) and the runtime (recomp\runtime, C).
// The game's code is translated to C and compiled here, from the Xenowerk binary inside the player's .ipa.
// Tools are downloaded once, at fixed versions checked by SHA-256, into <data>\cache\tools.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;

class BuildError : Exception
{
    public int Code;
    public BuildError(int code, string msg) : base(msg) { Code = code; }
}

static class Builder
{
    // Bump when the build steps or the compile flags change. Changes to recomp\ (lifter, runtime) are seen by themselves:
    // the stamp holds a hash of that folder.
    public const string BuildVersion = "1";

    class Tool
    {
        public string Name, Url, Sha256, Folder, Strip; public long Size;
        public Tool(string name, string url, string sha, long size, string folder, string strip) { Name = name; Url = url; Sha256 = sha; Size = size; Folder = folder; Strip = strip; }
    }
    static readonly Tool Python = new Tool("Python 3.14.8", "https://www.python.org/ftp/python/3.14.8/python-3.14.8-embed-amd64.zip",
        "a93abe456ab01bd96d7a085b3cdb6566b3063f4241360d114142fbdb07f0a310", 12601679, "python", null);
    static readonly Tool Capstone = new Tool("Capstone 5.0.7", "https://files.pythonhosted.org/packages/70/39/2138d890a8e827636b9de9924fbc8527fe83e38b6d26605b30ac55e30ebe/capstone-5.0.7-py3-none-win_amd64.whl",
        "4ab8bcb7da8f221ff45926ca168ca33e76f7237d06fbf3c10780002faa2670e1", 1272204, "python", null);
    static readonly Tool Zig = new Tool("Zig 0.17.1", "https://ziglang.org/download/0.17.1/zig-x86_64-windows-0.17.1.zip",
        "b03f477a1943673e02aca0f570e8030dffd536ce1f45cbc4d3326ec31159562e", 100343666, "zig", "zig-x86_64-windows-0.17.1/");

    static readonly string[] Libs = { "opengl32", "user32", "gdi32", "dbghelp", "winmm", "ole32", "dinput8", "dxguid" };

    static string T(string pt) { return I18n.T(pt); }
    public delegate void Progress(double frac, string text);

    // installDir: folder with Launcher.exe and recomp\ (the exe is written there).
    // dataDir: folder for the tools cache and the build scratch (the launcher's own folder when run alone).
    // appDir: the Xenowerk.app folder extracted from the player's .ipa.
    public static string ExePath(string installDir) { return Path.Combine(installDir, "xenowerk.exe"); }
    static string StampPath(string installDir) { return Path.Combine(installDir, "xenowerk.build"); }

    // What a build was made from: build steps version, the game's binary and our recompiler + runtime sources.
    public static string Stamp(string installDir, string appDir)
    {
        string recomp = Path.Combine(installDir, "recomp");
        var files = new List<string>();
        foreach (var f in Directory.GetFiles(recomp, "*", SearchOption.AllDirectories))
            if (f.EndsWith(".py", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".c", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".h", StringComparison.OrdinalIgnoreCase))
                files.Add(f.Substring(recomp.Length + 1).Replace('\\', '/'));
        files.Sort(StringComparer.Ordinal);
        using (var h = SHA256.Create())
        {
            foreach (var f in files)
            {
                var name = Encoding.UTF8.GetBytes(f + "\n"); h.TransformBlock(name, 0, name.Length, null, 0);
                var data = File.ReadAllBytes(Path.Combine(recomp, f.Replace('/', '\\'))); h.TransformBlock(data, 0, data.Length, null, 0);
            }
            h.TransformFinalBlock(new byte[0], 0, 0);
            string src = BitConverter.ToString(h.Hash).Replace("-", "").ToLowerInvariant().Substring(0, 16);
            return "v" + BuildVersion + " game=" + Sha256File(Path.Combine(appDir, "Xenowerk")) + " recomp=" + src;
        }
    }

    public static bool IsBuilt(string installDir, string appDir)
    {
        try
        {
            if (!File.Exists(ExePath(installDir)) || !File.Exists(StampPath(installDir))) return false;
            return File.ReadAllText(StampPath(installDir)).Trim() == Stamp(installDir, appDir);
        }
        catch { return false; }
    }

    public static void Build(string installDir, string dataDir, string appDir, Progress progress, Func<bool> cancelled)
    {
        string recomp = Path.Combine(installDir, "recomp");
        string lifter = Path.Combine(recomp, "lifter"), runtime = Path.Combine(recomp, "runtime");
        if (!File.Exists(Path.Combine(lifter, "lift_t2.py")) || !Directory.Exists(runtime))
            throw new BuildError(25, T("A pasta recomp não está ao lado do launcher. Extraia o zip do lançamento inteiro de novo."));
        string bin = Path.Combine(appDir, "Xenowerk");
        if (!File.Exists(bin)) throw new BuildError(3, T("O executável Xenowerk do jogo não foi encontrado. Escolha o .ipa do Xenowerk 2.0.7 de novo."));

        string cache = Path.Combine(dataDir, "cache"), tools = Path.Combine(cache, "tools"), work = Path.Combine(cache, "build");
        Directory.CreateDirectory(tools);
        CheckFreeSpace(cache, 1500L << 20);

        string stamp = Stamp(installDir, appDir);
        // remove the old build first, so a build that is cut short never looks finished
        TryDelete(StampPath(installDir));

        // 1. tools (0 .. 0.30)
        EnsureTool(Python, tools, 0.00, 0.04, progress, cancelled);
        EnsureTool(Capstone, tools, 0.04, 0.05, progress, cancelled);
        EnsureTool(Zig, tools, 0.05, 0.30, progress, cancelled);
        string py = Path.Combine(tools, "python"), zig = Path.Combine(tools, "zig", "zig.exe");
        // the embeddable Python only sees the folders in its ._pth file: add the lifter (the install folder may have moved)
        foreach (var pth in Directory.GetFiles(py, "python*._pth"))
            File.WriteAllText(pth, Path.GetFileNameWithoutExtension(pth) + ".zip\r\n.\r\n" + lifter + "\r\n");

        // 2. the armv7 slice of the game's binary
        if (Directory.Exists(work)) Directory.Delete(work, true);
        string gen = Path.Combine(work, "src"), obj = Path.Combine(work, "obj");
        Directory.CreateDirectory(gen); Directory.CreateDirectory(obj);
        string slice = Path.Combine(work, "xenowerk_armv7.bin");
        File.WriteAllBytes(slice, Armv7Slice(File.ReadAllBytes(bin)));

        // 3. translate the game's code to C (0.30 .. 0.45)
        progress(0.30, T("Traduzindo o código do jogo para C"));
        var log = new StringBuilder();
        var re = new Regex(@"^lifted (\d+) / (\d+)");
        int rc = RunTool(Path.Combine(py, "python.exe"), Q(Path.Combine(lifter, "lift_t2.py")) + " " + Q(slice) + " " + Q(gen), work,
            new Dictionary<string, string> { { "PYTHONHASHSEED", "0" }, { "PYTHONDONTWRITEBYTECODE", "1" } },
            line =>
            {
                lock (log) log.AppendLine(line);
                var m = re.Match(line);
                if (m.Success) progress(0.30 + 0.15 * double.Parse(m.Groups[1].Value) / Math.Max(1, double.Parse(m.Groups[2].Value)), T("Traduzindo o código do jogo para C"));
            }, cancelled);
        if (rc != 0 || !File.Exists(Path.Combine(gen, "func_table.c")))
            throw new BuildError(22, T("A tradução do código do jogo falhou (código ") + rc + ").\n" + Tail(log.ToString(), 6));
        if (log.ToString().Contains("lift failure"))
            throw new BuildError(22, T("A tradução do código do jogo falhou: algumas funções não puderam ser lidas. Este é o .ipa do Xenowerk 2.0.7?"));

        // 4. compile (0.45 .. 0.95): the generated code, then our runtime
        var env = new Dictionary<string, string> {
            { "ZIG_GLOBAL_CACHE_DIR", Path.Combine(cache, "zig-global") }, { "ZIG_LOCAL_CACHE_DIR", Path.Combine(cache, "zig-local") } };
        var jobs = new List<string[]>();     // { source, object, flags }
        const string common = "-target x86_64-windows-gnu -c -w -g0 -fwrapv -fno-strict-aliasing -fno-sanitize=all";
        foreach (var f in Directory.GetFiles(gen, "*.c"))
            jobs.Add(new[] { f, Path.Combine(obj, Path.GetFileNameWithoutExtension(f) + ".o"), common + " -O1 -I" + Q(gen) + " -I" + Q(runtime) });
        foreach (var f in Directory.GetFiles(runtime, "*.c"))
            jobs.Add(new[] { f, Path.Combine(obj, "rt_" + Path.GetFileNameWithoutExtension(f) + ".o"), common + " -O2 -I" + Q(gen) + " -I" + Q(runtime) });
        jobs.Sort((a, b) => new FileInfo(b[0]).Length.CompareTo(new FileInfo(a[0]).Length));   // biggest first: shorter total time

        // the first zig run builds its C library for Windows; do it once before starting the parallel jobs
        progress(0.45, T("Preparando o compilador (só na primeira vez)"));
        var first = new StringBuilder();
        string probe = Path.Combine(work, "probe.c"); File.WriteAllText(probe, "int main(void){return 0;}\n");
        if (RunTool(zig, "cc -target x86_64-windows-gnu -o " + Q(Path.Combine(work, "probe.exe")) + " " + Q(probe), work, env, l => { lock (first) first.AppendLine(l); }, cancelled) != 0)
            throw new BuildError(23, T("O compilador não iniciou.") + "\n" + Tail(first.ToString(), 6));

        int done = 0, failed = 0, next = -1; string firstError = null;
        int workers = Math.Max(1, Math.Min(Environment.ProcessorCount, 16));
        var threads = new List<Thread>();
        for (int w = 0; w < workers; w++)
        {
            var t = new Thread(() =>
            {
                for (;;)
                {
                    int i = Interlocked.Increment(ref next);
                    if (i >= jobs.Count || Volatile.Read(ref failed) != 0 || cancelled()) return;
                    var j = jobs[i]; var outp = new StringBuilder();
                    int r = RunTool(zig, "cc " + j[2] + " -o " + Q(j[1]) + " " + Q(j[0]), work, env, l => { lock (outp) outp.AppendLine(l); }, cancelled);
                    if (r != 0 || !File.Exists(j[1]))
                    {
                        if (Interlocked.Exchange(ref failed, 1) == 0) firstError = Path.GetFileName(j[0]) + ": " + Tail(outp.ToString(), 6);
                        return;
                    }
                    int d = Interlocked.Increment(ref done);
                    progress(0.45 + 0.50 * d / jobs.Count, T("Compilando") + " (" + d + "/" + jobs.Count + ")");
                }
            });
            t.IsBackground = true; t.Start(); threads.Add(t);
        }
        foreach (var t in threads) t.Join();
        if (cancelled()) throw new BuildError(26, T("Cancelado."));
        if (failed != 0) throw new BuildError(23, T("A compilação falhou.") + "\n" + firstError);

        // 5. link (0.95 .. 1)
        progress(0.95, T("Gerando o xenowerk.exe"));
        string tmpExe = Path.Combine(work, "xenowerk.exe");
        var args = new StringBuilder("cc -target x86_64-windows-gnu -o " + Q(tmpExe));
        foreach (var j in jobs) args.Append(' ').Append(Q(Path.GetFileName(j[1])));
        foreach (var l in Libs) args.Append(" -l").Append(l);
        args.Append(" -Wl,--stack,67108864");
        var lk = new StringBuilder();
        if (RunTool(zig, args.ToString(), obj, env, l => { lock (lk) lk.AppendLine(l); }, cancelled) != 0 || !File.Exists(tmpExe))
            throw new BuildError(23, T("A geração do xenowerk.exe falhou.") + "\n" + Tail(lk.ToString(), 6));

        string exe = ExePath(installDir);
        TryDelete(exe);
        File.Move(tmpExe, exe);
        File.WriteAllText(StampPath(installDir), stamp);
        try { Directory.Delete(work, true); } catch { }
        progress(1.0, T("Jogo pronto"));
    }

    // ------------------------------------------------------------------ helpers
    static string Q(string s) { return "\"" + s + "\""; }
    static void TryDelete(string f) { try { if (File.Exists(f)) File.Delete(f); } catch { } }
    static string Tail(string s, int n)
    {
        var l = s.Replace("\r", "").TrimEnd('\n').Split('\n');
        return string.Join("\n", l, Math.Max(0, l.Length - n), Math.Min(n, l.Length));
    }

    static void CheckFreeSpace(string dir, long need)
    {
        try
        {
            var root = Path.GetPathRoot(Path.GetFullPath(dir));
            var d = new DriveInfo(root);
            if (d.AvailableFreeSpace < need)
                throw new BuildError(24, T("Espaço livre insuficiente em ") + root + " (" + (need >> 20) + T(" MB necessários para preparar o jogo)."));
        }
        catch (BuildError) { throw; }
        catch { }   // network drive / unknown: just try
    }

    public static string Sha256File(string path)
    {
        using (var s = File.OpenRead(path)) using (var h = SHA256.Create())
            return BitConverter.ToString(h.ComputeHash(s)).Replace("-", "").ToLowerInvariant();
    }

    // The game's binary is a "fat" Mach-O; the code is in its armv7 slice (cputype 12).
    static byte[] Armv7Slice(byte[] f)
    {
        if (f.Length < 8) throw new BuildError(3, T("O executável Xenowerk do jogo está danificado."));
        if (f[0] == 0xCA && f[1] == 0xFE && f[2] == 0xBA && f[3] == 0xBE)
        {
            uint n = BE(f, 4);
            for (uint i = 0; i < n; i++)
            {
                int a = 8 + 20 * (int)i;
                if (BE(f, a) == 12)
                {
                    uint off = BE(f, a + 8), size = BE(f, a + 12);
                    if ((long)off + size > f.Length) break;
                    var r = new byte[size]; Buffer.BlockCopy(f, (int)off, r, 0, (int)size); return r;
                }
            }
            throw new BuildError(3, T("O executável do jogo não tem código armv7. Escolha o .ipa do Xenowerk 2.0.7."));
        }
        if (f[0] == 0xCE && f[1] == 0xFA && f[2] == 0xED && f[3] == 0xFE) return f;   // already thin (32-bit little-endian Mach-O)
        throw new BuildError(3, T("O executável Xenowerk do jogo não é um arquivo Mach-O. Escolha o .ipa do Xenowerk 2.0.7."));
    }
    static uint BE(byte[] b, int o) { return (uint)(b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3]); }

    static int RunTool(string exe, string args, string cwd, Dictionary<string, string> env, Action<string> line, Func<bool> cancelled)
    {
        var psi = new ProcessStartInfo(exe, args)
        {
            WorkingDirectory = cwd, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8
        };
        if (env != null) foreach (var kv in env) psi.EnvironmentVariables[kv.Key] = kv.Value;
        using (var p = new Process { StartInfo = psi })
        {
            p.OutputDataReceived += (s, e) => { if (e.Data != null) line(e.Data); };
            p.ErrorDataReceived += (s, e) => { if (e.Data != null) line(e.Data); };
            try { p.Start(); }
            catch (Exception ex) { line(ex.Message); return -1; }
            p.BeginOutputReadLine(); p.BeginErrorReadLine();
            while (!p.WaitForExit(250))
                if (cancelled()) { try { p.Kill(); } catch { } p.WaitForExit(); return -2; }
            p.WaitForExit();
            return p.ExitCode;
        }
    }

    // Downloads a tool once (resumed from scratch if cut), checks its SHA-256 and unpacks it into tools\<Folder>.
    static void EnsureTool(Tool t, string tools, double p0, double p1, Progress progress, Func<bool> cancelled)
    {
        string mark = Path.Combine(tools, t.Folder, "." + Path.GetFileName(new Uri(t.Url).AbsolutePath) + ".ok");
        if (File.Exists(mark) && File.ReadAllText(mark).Trim() == t.Sha256) return;

        string dl = Path.Combine(tools, "download"); Directory.CreateDirectory(dl);
        string file = Path.Combine(dl, Path.GetFileName(new Uri(t.Url).AbsolutePath)), part = file + ".part";
        if (!(File.Exists(file) && Sha256File(file) == t.Sha256))
        {
            TryDelete(file);
            ServicePointManager.SecurityProtocol = (SecurityProtocolType)3072;     // TLS 1.2
            try
            {
                var rq = (HttpWebRequest)WebRequest.Create(t.Url);
                rq.UserAgent = "XenoRecomp-Builder"; rq.Timeout = 30000; rq.ReadWriteTimeout = 60000;
                using (var rs = rq.GetResponse()) using (var s = rs.GetResponseStream()) using (var o = File.Create(part))
                {
                    long total = rs.ContentLength > 0 ? rs.ContentLength : t.Size, got = 0; var buf = new byte[1 << 16]; int n;
                    var sw = Stopwatch.StartNew(); long last = -1;
                    while ((n = s.Read(buf, 0, buf.Length)) > 0)
                    {
                        if (cancelled()) throw new BuildError(26, T("Cancelado."));
                        o.Write(buf, 0, n); got += n;
                        long sec = sw.ElapsedMilliseconds / 1000;
                        if (sec != last) { last = sec; progress(p0 + (p1 - p0) * 0.9 * got / Math.Max(1, total), T("Baixando ") + t.Name + " (" + (got >> 20) + "/" + (total >> 20) + " MB)"); }
                    }
                }
            }
            catch (BuildError) { TryDelete(part); throw; }
            catch (Exception ex)
            {
                TryDelete(part);
                throw new BuildError(20, T("Não consegui baixar ") + t.Name + " (" + ex.Message + T("). Verifique a conexão com a internet e tente de novo.") + "\n" + t.Url);
            }
            if (Sha256File(part) != t.Sha256)
            {
                TryDelete(part);
                throw new BuildError(21, T("O download de ") + t.Name + T(" veio danificado (o SHA-256 não confere). Tente de novo.") + "\n" + t.Url);
            }
            File.Move(part, file);
        }

        progress(p0 + (p1 - p0) * 0.9, T("Extraindo ") + t.Name);
        string dest = Path.Combine(tools, t.Folder);
        if (t.Folder == "zig" && Directory.Exists(dest)) Directory.Delete(dest, true);
        Directory.CreateDirectory(dest);
        string destSep = Path.GetFullPath(dest).TrimEnd('\\') + "\\";
        bool wheel = file.EndsWith(".whl", StringComparison.OrdinalIgnoreCase);
        using (var z = ZipFile.OpenRead(file))
            foreach (var e in z.Entries)
            {
                string name = e.FullName;
                if (t.Strip != null) { if (!name.StartsWith(t.Strip, StringComparison.Ordinal)) continue; name = name.Substring(t.Strip.Length); }
                if (wheel && name.Contains(".dist-info/")) continue;
                if (name == "" || name.EndsWith("/")) continue;
                string target = Path.GetFullPath(Path.Combine(dest, name.Replace('/', '\\')));
                if (!target.StartsWith(destSep, StringComparison.OrdinalIgnoreCase)) continue;    // never write outside the tool's folder
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                e.ExtractToFile(target, true);
            }
        File.WriteAllText(mark, t.Sha256);
        TryDelete(file);
        progress(p1, T("Extraído: ") + t.Name);
    }
}
