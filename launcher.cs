// Xenowerk launcher (WinForms). Build: csc /target:winexe /out:Launcher.exe launcher.cs
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Net;
using System.Threading;
using System.Text.RegularExpressions;
using System.IO.Compression;
using System.Text;
using System.Windows.Forms;

class Launcher : Form
{
    string dir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
    string cfgFile, saveRoot, bakRoot;
    const string Default = "(padrão)";
    const int MaxName = 13;                       // longest player name the in-game text node can hold
    Dictionary<string, string> cfg = new Dictionary<string, string>();

    class Aspect { public string Name; public int W; public Aspect(string n, int w) { Name = n; W = w; } }
    // the game's height is fixed at 576 points; the width follows the aspect ratio
    static readonly Aspect[] Aspects = {
        new Aspect(T("16:9 - nativo (recomendado)"), 1024), new Aspect(T("4:3 - iPad (corta as laterais)"), 768),
        new Aspect("16:10", 922), new Aspect("21:9 - ultrawide", 1344), new Aspect("32:9 - super ultrawide", 2048) };
    static readonly string[] ResNames = { T("Automática (% da tela)"), "1280 x 720", "1366 x 768", "1600 x 900", "1920 x 1080", "2560 x 1440", "3440 x 1440", "3840 x 2160", T("Personalizada...") };
    static readonly int[,] ResSizes = { { 0, 0 }, { 1280, 720 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3440, 1440 }, { 3840, 2160 }, { 0, 0 } };

    // Jogo tab
    // Controles tab
    CheckBox ckKbm; TextBox[,] txKey; ComboBox[] cbBtn;
    static readonly string[] ActId = { "UP", "DOWN", "LEFT", "RIGHT", "FIRE", "NEXT", "PREV", "RELOAD", "USE", "POWER", "BACK", "PAUSE", "CAM", "TOGGLE" };
    static readonly string[] ActLbl = { T("Mover para cima"), T("Mover para baixo"), T("Mover para a esquerda"), T("Mover para a direita"), T("Atirar / mirar (segurar)"), T("Próxima arma"), T("Arma anterior"), T("Recarregar"), T("Usar / interagir"), T("Poder especial"), T("Voltar / cancelar"), T("Pausa"), T("Resetar câmera"), T("Mouse: mira / menu (alternar)") };
    static readonly string[,] ActKey = { { "W", "NONE" }, { "S", "NONE" }, { "A", "NONE" }, { "D", "NONE" }, { "MOUSE1", "NONE" }, { "WHEELDOWN", "E" }, { "WHEELUP", "Q" }, { "R", "NONE" }, { "F", "NONE" }, { "SPACE", "NONE" }, { "C", "NONE" }, { "P", "ESC" }, { "V", "NONE" }, { "TAB", "NONE" } };
    static readonly string[] ActBtn = { "", "", "", "", "RT", "RB", "LB", "X", "A", "Y", "B", "START", "RS", "" };
    static readonly string[] BtnList = { "A", "B", "X", "Y", "LB", "RB", "LT", "RT", "START", "BACK", "LS", "RS" };
    ComboBox cbProf; CheckBox ckShowName, ckFull, ckAudio, ckPad; Label lbSt, lbApp;
    // Avançado tab
    ComboBox cbRes, cbAspect, cbScale, cbFilter; NumericUpDown nudW, nudH; TrackBar tb, tbDead; Label lbPct, lbDead, lbInfo; CheckBox ckVsync, ckProf; ComboBox cbFps, cbLang, cbGLang; CheckBox ckUpd;
    static readonly string[] GLangCode = { "", "en", "pt", "es", "fr", "de", "it" };

    const string AppVersion = "0.2";
    const string RepoSlug = "defaultdotcfg/XenoRecomp";          // "owner/repo" of the GitHub project that publishes the releases; empty = update check disabled
    static string T(string pt) { return I18n.T(pt); }

    [STAThread]
    static void Main() { Application.EnableVisualStyles(); Application.Run(new Launcher()); }

    string Get(string k, string d) { string v; return cfg.TryGetValue(k, out v) ? v : d; }
    int GetInt(string k, int d) { int v; return int.TryParse(Get(k, d.ToString()), out v) ? v : d; }
    static string Clean(string n) { foreach (char c in "\\/:*?\"<>|;") n = n.Replace(c, '_'); return n.Trim(); }
    string SaveDir(string p) { return string.IsNullOrEmpty(p) ? saveRoot : Path.Combine(saveRoot, p); }
    string CurProfile() { string p = Clean(cbProf.Text); return p == Default ? "" : p; }
    static Label L(string t, int x, int y) { return new Label { Text = t, Location = new Point(x, y), AutoSize = true }; }

    Launcher()
    {
        cfgFile = Path.Combine(dir, "launcher.ini"); saveRoot = Path.Combine(dir, "save"); bakRoot = Path.Combine(dir, "save_backups");
        if (File.Exists(cfgFile))
            foreach (var l in File.ReadAllLines(cfgFile)) { int i = l.IndexOf('='); if (i > 0) cfg[l.Substring(0, i)] = l.Substring(i + 1); }

        Text = T("Xenowerk - Launcher"); ClientSize = new Size(440, 562); StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedDialog; MaximizeBox = false; Font = new Font("Segoe UI", 9f);

        var tabs = new TabControl { Location = new Point(8, 8), Size = new Size(424, 464) };
        var tJogo = new TabPage(T("Jogo")); var tAdv = new TabPage(T("Avançado")); var tCtl = new TabPage(T("Controles"));
        tabs.TabPages.Add(tJogo); tabs.TabPages.Add(tAdv); tabs.TabPages.Add(tCtl); Controls.Add(tabs);

        BuildJogo(tJogo); BuildAdvanced(tAdv); BuildControls(tCtl);

        string defApp = Path.GetFullPath(Path.Combine(dir, @"..\..\..\Payload\Xenowerk.app"));
        lbApp = new Label { Text = Get("app", defApp), Location = new Point(12, 484), Size = new Size(220, 18), AutoEllipsis = true };
        var btApp = new Button { Text = T("Pasta..."), Location = new Point(354, 480), Width = 76 };
        var btIpa = new Button { Text = T("Importar IPA..."), Location = new Point(236, 480), Width = 114 };
        btIpa.Click += (s, e) => ImportIpa();
        var btPlay = new Button { Text = T("JOGAR"), Location = new Point(10, 512), Size = new Size(420, 40), Font = new Font("Segoe UI", 12f, FontStyle.Bold) };
        Controls.AddRange(new Control[] { lbApp, btApp, btIpa, btPlay });

        FormClosing += (s, e) => SaveCfg();
        Shown += (s, e) => { if (ckUpd.Checked) CheckUpdate(); };
        btApp.Click += (s, e) => { using (var dlg = new FolderBrowserDialog { Description = T("Selecione a pasta Xenowerk.app") }) if (dlg.ShowDialog() == DialogResult.OK) lbApp.Text = dlg.SelectedPath; };
        btPlay.Click += (s, e) => Play();
        UpdateAdvancedState();
    }

    // ------------------------------------------------------------------ tab "Jogo"
    void BuildJogo(TabPage t)
    {
        var gp = new GroupBox { Text = T("Perfil"), Bounds = new Rectangle(8, 8, 396, 150) }; t.Controls.Add(gp);
        gp.Controls.Add(L(T("Perfil:"), 12, 28));
        cbProf = new ComboBox { Location = new Point(62, 24), Width = 200, DropDownStyle = ComboBoxStyle.DropDown }; gp.Controls.Add(cbProf);
        RefreshProfiles(Get("profile", ""));
        var btNew = new Button { Text = T("Novo perfil..."), Location = new Point(272, 23), Width = 110 }; gp.Controls.Add(btNew);
        ckShowName = new CheckBox { Text = T("Mostrar o nome do perfil no jogo"), Location = new Point(14, 58), AutoSize = true, Checked = Get("showname", "1") == "1" }; gp.Controls.Add(ckShowName);
        gp.Controls.Add(new Label { Text = T("O nome substitui \"Jogador\" no menu (até ") + MaxName + T(" caracteres).\nCada perfil tem o seu próprio save."), Location = new Point(14, 84), Size = new Size(370, 36), ForeColor = Color.DimGray });
        var btOpen = new Button { Text = T("Abrir pasta do save"), Location = new Point(232, 114), Width = 150 }; gp.Controls.Add(btOpen);

        var gb = new GroupBox { Text = T("Backup do save"), Bounds = new Rectangle(8, 164, 396, 84) }; t.Controls.Add(gb);
        var btBak = new Button { Text = T("Fazer backup"), Location = new Point(12, 24), Width = 180 };
        var btRes = new Button { Text = T("Restaurar backup..."), Location = new Point(200, 24), Width = 182 };
        lbSt = new Label { Location = new Point(12, 58), AutoSize = true, ForeColor = Color.DimGray };
        gb.Controls.AddRange(new Control[] { btBak, btRes, lbSt });

        var go = new GroupBox { Text = T("Opções"), Bounds = new Rectangle(8, 254, 396, 84) }; t.Controls.Add(go);
        ckFull = new CheckBox { Text = T("Tela cheia (F11 alterna)"), Location = new Point(14, 24), AutoSize = true, Checked = Get("fullscreen", "0") == "1" };
        ckAudio = new CheckBox { Text = T("Áudio"), Location = new Point(220, 24), AutoSize = true, Checked = Get("audio", "1") == "1" };
        ckUpd = new CheckBox { Text = T("Verificar atualizações ao abrir"), Location = new Point(14, 54), AutoSize = true, Checked = Get("upd", "1") == "1" };
        go.Controls.AddRange(new Control[] { ckFull, ckAudio, ckUpd });

        var gl = new GroupBox { Text = T("Idiomas"), Bounds = new Rectangle(8, 344, 396, 88) }; t.Controls.Add(gl);
        gl.Controls.Add(L(T("Idioma do launcher:"), 12, 26));
        cbLang = new ComboBox { Location = new Point(190, 22), Width = 190, DropDownStyle = ComboBoxStyle.DropDownList };
        cbLang.Items.AddRange(I18n.Names); cbLang.SelectedIndex = I18n.Lang;
        cbLang.SelectionChangeCommitted += (s, e) => { SaveCfg(); Process.Start(Application.ExecutablePath); Close(); };
        gl.Controls.Add(cbLang);
        gl.Controls.Add(L(T("Idioma do jogo:"), 12, 56));
        cbGLang = new ComboBox { Location = new Point(190, 52), Width = 190, DropDownStyle = ComboBoxStyle.DropDownList };
        cbGLang.Items.Add(T("Automático")); cbGLang.Items.AddRange(new object[] { "English", "Português", "Español", "Français", "Deutsch", "Italiano" });
        { int gi = Array.IndexOf(GLangCode, Get("glang", "")); cbGLang.SelectedIndex = gi < 0 ? 0 : gi; }
        gl.Controls.Add(cbGLang);

        btNew.Click += (s, e) => NewProfile();
        btOpen.Click += (s, e) => { var d = SaveDir(CurProfile()); Directory.CreateDirectory(d); Process.Start("explorer.exe", "\"" + d + "\""); };
        btBak.Click += (s, e) => Backup();
        btRes.Click += (s, e) => Restore();
    }

    // Looks for a newer release on GitHub (one anonymous HTTPS GET; nothing about the user or the game is sent) and offers to open the download page.
    void CheckUpdate()
    {
        if (RepoSlug == "") return;
        ThreadPool.QueueUserWorkItem(_ =>
        {
            try
            {
                ServicePointManager.SecurityProtocol = (SecurityProtocolType)3072;     // TLS 1.2
                var rq = (HttpWebRequest)WebRequest.Create("https://api.github.com/repos/" + RepoSlug + "/releases/latest");
                rq.UserAgent = "Xenowerk-Launcher"; rq.Accept = "application/vnd.github+json"; rq.Timeout = 6000; rq.ReadWriteTimeout = 6000;
                string js; using (var rs = rq.GetResponse()) using (var sr = new StreamReader(rs.GetResponseStream())) js = sr.ReadToEnd();
                var tag = Regex.Match(js, "\"tag_name\"\\s*:\\s*\"([^\"]+)\"");
                var url = Regex.Match(js, "\"html_url\"\\s*:\\s*\"([^\"]+)\"");
                if (!tag.Success || !url.Success) return;
                Version nv, cv; if (!Version.TryParse(tag.Groups[1].Value.TrimStart('v', 'V'), out nv) || !Version.TryParse(AppVersion, out cv) || nv <= cv) return;
                string link = url.Groups[1].Value; if (!link.StartsWith("https://github.com/", StringComparison.Ordinal)) return;
                if (Get("skipver", "") == tag.Groups[1].Value) return;
                BeginInvoke((MethodInvoker)(() => ShowUpdate(tag.Groups[1].Value, link)));
            }
            catch { }      // offline / rate-limited: stay silent
        });
    }
    void ShowUpdate(string tag, string link)
    {
        using (var f = new Form { Text = T("Atualização disponível"), ClientSize = new Size(380, 150), StartPosition = FormStartPosition.CenterParent, FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false, Font = Font })
        {
            f.Controls.Add(new Label { Text = T("Nova versão do launcher disponível: ") + tag + Environment.NewLine + T("Sua versão: ") + "v" + AppVersion, Location = new Point(14, 14), Size = new Size(352, 40) });
            var ck = new CheckBox { Text = T("Não avisar novamente sobre esta versão"), Location = new Point(14, 62), AutoSize = true }; f.Controls.Add(ck);
            var go = new Button { Text = T("Abrir página de download"), Location = new Point(14, 100), Width = 190, DialogResult = DialogResult.OK }; f.Controls.Add(go);
            var later = new Button { Text = T("Mais tarde"), Location = new Point(212, 100), Width = 154, DialogResult = DialogResult.Cancel }; f.Controls.Add(later);
            f.AcceptButton = go; f.CancelButton = later;
            var r = f.ShowDialog(this);
            if (ck.Checked) { cfg["skipver"] = tag; SaveCfg(); }
            if (r == DialogResult.OK) { try { Process.Start(link); } catch { } }
        }
    }

    void RefreshProfiles(string sel)
    {
        cbProf.Items.Clear(); cbProf.Items.Add(Default);
        if (Directory.Exists(saveRoot)) foreach (var d in Directory.GetDirectories(saveRoot)) cbProf.Items.Add(Path.GetFileName(d));
        cbProf.Text = string.IsNullOrEmpty(sel) ? Default : sel;
    }

    void NewProfile()
    {
        using (var f = new Form { Text = T("Novo perfil"), ClientSize = new Size(320, 120), StartPosition = FormStartPosition.CenterParent, FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false, Font = Font })
        {
            f.Controls.Add(L(T("Nome do perfil (aparece no jogo):"), 12, 12));
            var tx = new TextBox { Location = new Point(12, 36), Width = 296, MaxLength = MaxName }; f.Controls.Add(tx);
            var ok = new Button { Text = T("Criar"), Location = new Point(150, 78), Width = 76, DialogResult = DialogResult.OK };
            var cn = new Button { Text = T("Cancelar"), Location = new Point(232, 78), Width = 76, DialogResult = DialogResult.Cancel };
            f.Controls.AddRange(new Control[] { ok, cn }); f.AcceptButton = ok; f.CancelButton = cn;
            if (f.ShowDialog(this) != DialogResult.OK) return;
            string n = Clean(tx.Text);
            if (n.Length == 0 || n == Default) { MessageBox.Show(T("Informe um nome válido."), T("Novo perfil"), MessageBoxButtons.OK, MessageBoxIcon.Information); return; }
            if (Encoding.UTF8.GetByteCount(n) > MaxName) { MessageBox.Show(T("O nome é longo demais para aparecer no jogo (máximo ") + MaxName + T(" caracteres)."), T("Novo perfil"), MessageBoxButtons.OK, MessageBoxIcon.Information); return; }
            Directory.CreateDirectory(SaveDir(n)); RefreshProfiles(n); lbSt.Text = T("Perfil criado: ") + n; ckShowName.Checked = true;
        }
    }

    // ------------------------------------------------------------------ tab "Avançado"
    void BuildControls(TabPage t)
    {
        var gp = new GroupBox { Text = T("Controle (gamepad)"), Bounds = new Rectangle(8, 6, 396, 76) }; t.Controls.Add(gp);
        ckPad = new CheckBox { Text = T("Ativar suporte (XInput / DirectInput)"), Location = new Point(14, 24), AutoSize = true, Checked = Get("pad", "1") == "1" };
        gp.Controls.Add(ckPad);
        gp.Controls.Add(L(T("Zona morta:"), 14, 52));
        tbDead = new TrackBar { Location = new Point(130, 42), Width = 190, Minimum = 0, Maximum = 50, TickFrequency = 10, Value = Math.Max(0, Math.Min(50, GetInt("padDead", 18))) };
        lbDead = new Label { Location = new Point(326, 52), AutoSize = true };
        tbDead.ValueChanged += (s, e) => lbDead.Text = tbDead.Value + "%"; lbDead.Text = tbDead.Value + "%";
        gp.Controls.Add(tbDead); gp.Controls.Add(lbDead);

        var gk = new GroupBox { Text = T("Teclado e mouse (emulam um controle)"), Bounds = new Rectangle(8, 86, 396, 296) }; t.Controls.Add(gk);
        ckKbm = new CheckBox { Text = T("Ativar teclado e mouse"), Location = new Point(14, 22), AutoSize = true, Checked = Get("kbm", "1") == "1" };
        gk.Controls.Add(ckKbm);
        var pn = new Panel { Location = new Point(6, 46), Size = new Size(384, 212), AutoScroll = true, BorderStyle = BorderStyle.FixedSingle }; gk.Controls.Add(pn);
        pn.Controls.Add(new Label { Text = T("Ação"), Location = new Point(4, 4), AutoSize = true, Font = new Font(Font, FontStyle.Bold) });
        pn.Controls.Add(new Label { Text = T("Tecla 1"), Location = new Point(150, 4), AutoSize = true, Font = new Font(Font, FontStyle.Bold) });
        pn.Controls.Add(new Label { Text = T("Tecla 2"), Location = new Point(220, 4), AutoSize = true, Font = new Font(Font, FontStyle.Bold) });
        pn.Controls.Add(new Label { Text = T("Botão"), Location = new Point(292, 4), AutoSize = true, Font = new Font(Font, FontStyle.Bold) });
        txKey = new TextBox[ActId.Length, 2]; cbBtn = new ComboBox[ActId.Length];
        for (int i = 0; i < ActId.Length; i++)
        {
            int y = 24 + i * 26;
            pn.Controls.Add(new Label { Text = ActLbl[i], Location = new Point(4, y + 3), Size = new Size(144, 18) });
            for (int k = 0; k < 2; k++)
            {
                var tx = new TextBox { Location = new Point(150 + k * 70, y), Width = 66, ReadOnly = true, TextAlign = HorizontalAlignment.Center, BackColor = SystemColors.Window, Cursor = Cursors.Hand };
                tx.Tag = ""; txKey[i, k] = tx; SetKey(tx, Get("bind_" + ActId[i] + (k == 1 ? "2" : ""), ActKey[i, k]));
                tx.Enter += (s, e) => { var b = (TextBox)s; b.BackColor = Color.LightYellow; };
                tx.Leave += (s, e) => { var b = (TextBox)s; b.BackColor = SystemColors.Window; };
                tx.PreviewKeyDown += (s, e) => e.IsInputKey = true;
                tx.KeyDown += (s, e) => { e.SuppressKeyPress = true; e.Handled = true; var b = (TextBox)s;
                    if (e.KeyCode == Keys.Delete) { SetKey(b, "NONE"); return; }
                    string n = KeyName(e.KeyCode); if (n != null) SetKey(b, n); };
                tx.MouseDown += (s, e) => { var b = (TextBox)s; b.Focus();
                    if (e.Button == MouseButtons.Right) SetKey(b, "MOUSE2"); else if (e.Button == MouseButtons.Middle) SetKey(b, "MOUSE3");
                    else if (e.Button == MouseButtons.XButton1) SetKey(b, "MOUSE4"); else if (e.Button == MouseButtons.XButton2) SetKey(b, "MOUSE5"); };
                tx.DoubleClick += (s, e) => SetKey((TextBox)s, "MOUSE1");
                tx.MouseWheel += (s, e) => SetKey((TextBox)s, e.Delta > 0 ? "WHEELUP" : "WHEELDOWN");
                pn.Controls.Add(tx);
            }
            if (ActBtn[i] != "")
            {
                var cb = new ComboBox { Location = new Point(292, y), Width = 64, DropDownStyle = ComboBoxStyle.DropDownList };
                cb.Items.AddRange(BtnList); string cur = Get("btn_" + ActId[i], ActBtn[i]); int ix = Array.IndexOf(BtnList, cur); cb.SelectedIndex = ix < 0 ? Array.IndexOf(BtnList, ActBtn[i]) : ix;
                cbBtn[i] = cb; pn.Controls.Add(cb);
            }
        }
        gk.Controls.Add(new Label { Text = T("Clique na caixa e pressione a tecla; botão direito/do meio, roda ou duplo clique (Mouse1) também valem. Del limpa."), Location = new Point(10, 262), Size = new Size(380, 30), ForeColor = Color.DimGray, Font = new Font("Segoe UI", 7.5f) });
        var btDef = new Button { Text = T("Padrão"), Location = new Point(310, 18), Width = 76 }; gk.Controls.Add(btDef);
        btDef.Click += (s, e) => { for (int i = 0; i < ActId.Length; i++) { for (int k = 0; k < 2; k++) SetKey(txKey[i, k], ActKey[i, k]); if (cbBtn[i] != null) cbBtn[i].SelectedIndex = Array.IndexOf(BtnList, ActBtn[i]); } };
        t.Controls.Add(new Label { Text = T("Menus: mouse clica. Em partida, aperte a tecla de alternar (Tab): o mouse vira mira (cursor em cruz) e não toca nos botões da tela. Pausa volta ao modo menu."), Location = new Point(12, 384), Size = new Size(404, 44), ForeColor = Color.DimGray, Font = new Font("Segoe UI", 7.5f) });
    }
    static void SetKey(TextBox tx, string name) { tx.Tag = name; tx.Text = name == "NONE" ? "-" : name == "WHEELUP" ? "Scroll " + (char)0x25B2 : name == "WHEELDOWN" ? "Scroll " + (char)0x25BC : name; }
    static string KeyName(Keys k)
    {
        if (k >= Keys.A && k <= Keys.Z) return ((char)k).ToString();
        if (k >= Keys.D0 && k <= Keys.D9) return ((char)k).ToString();
        if (k >= Keys.F1 && k <= Keys.F12) return "F" + (k - Keys.F1 + 1);
        switch (k)
        {
            case Keys.Space: return "SPACE"; case Keys.ShiftKey: return "SHIFT"; case Keys.ControlKey: return "CTRL"; case Keys.Menu: return "ALT";
            case Keys.Tab: return "TAB"; case Keys.Escape: return "ESC"; case Keys.Return: return "ENTER"; case Keys.Back: return "BACKSPACE";
            case Keys.Up: return "UP"; case Keys.Down: return "DOWN"; case Keys.Left: return "LEFT"; case Keys.Right: return "RIGHT";
            case Keys.Insert: return "INSERT"; case Keys.Home: return "HOME"; case Keys.End: return "END"; case Keys.PageUp: return "PAGEUP"; case Keys.PageDown: return "PAGEDOWN";
        }
        return null;
    }

    void BuildAdvanced(TabPage t)
    {
        var gw = new GroupBox { Text = T("Janela e resolução"), Bounds = new Rectangle(8, 8, 396, 136) }; t.Controls.Add(gw);
        gw.Controls.Add(L(T("Resolução da janela:"), 12, 28));
        cbRes = new ComboBox { Location = new Point(150, 24), Width = 232, DropDownStyle = ComboBoxStyle.DropDownList }; cbRes.Items.AddRange(ResNames);
        cbRes.SelectedIndex = Math.Max(0, Math.Min(ResNames.Length - 1, GetInt("winres", 0))); gw.Controls.Add(cbRes);
        gw.Controls.Add(L(T("Personalizada:"), 12, 60));
        nudW = new NumericUpDown { Location = new Point(150, 56), Width = 70, Minimum = 320, Maximum = 16384, Value = Math.Max(320, Math.Min(16384, GetInt("winw", 1280))) };
        nudH = new NumericUpDown { Location = new Point(240, 56), Width = 70, Minimum = 240, Maximum = 16384, Value = Math.Max(240, Math.Min(16384, GetInt("winh", 720))) };
        gw.Controls.Add(nudW); gw.Controls.Add(L("x", 225, 60)); gw.Controls.Add(nudH);
        gw.Controls.Add(L(T("Tamanho da janela:"), 12, 96));
        tb = new TrackBar { Location = new Point(146, 88), Width = 130, Minimum = 30, Maximum = 100, TickFrequency = 10, Value = Math.Max(30, Math.Min(100, GetInt("winpct", 70))) };
        lbPct = new Label { Location = new Point(282, 96), AutoSize = true };
        tb.ValueChanged += (s, e) => lbPct.Text = tb.Value + T("% da tela"); lbPct.Text = tb.Value + T("% da tela");
        gw.Controls.Add(tb); gw.Controls.Add(lbPct);

        var gr = new GroupBox { Text = T("Renderização"), Bounds = new Rectangle(8, 150, 396, 134) }; t.Controls.Add(gr);
        gr.Controls.Add(L(T("Proporção da tela:"), 12, 28));
        cbAspect = new ComboBox { Location = new Point(150, 24), Width = 232, DropDownStyle = ComboBoxStyle.DropDownList };
        foreach (var a in Aspects) cbAspect.Items.Add(a.Name);
        cbAspect.SelectedIndex = Math.Max(0, Math.Min(Aspects.Length - 1, GetInt("aspect", 0))); gr.Controls.Add(cbAspect);
        gr.Controls.Add(L(T("Qualidade (supersampling):"), 12, 60));
        cbScale = new ComboBox { Location = new Point(190, 56), Width = 192, DropDownStyle = ComboBoxStyle.DropDownList };
        cbScale.Items.AddRange(new object[] { T("1x - nativo (mais rápido)"), T("2x (recomendado)"), "3x", T("4x (pesado)") });
        cbScale.SelectedIndex = Math.Max(1, Math.Min(4, GetInt("scale", 2))) - 1; gr.Controls.Add(cbScale);
        gr.Controls.Add(L(T("Filtro de imagem:"), 12, 92));
        cbFilter = new ComboBox { Location = new Point(150, 88), Width = 232, DropDownStyle = ComboBoxStyle.DropDownList };
        cbFilter.Items.Add(T("Suave (linear)")); cbFilter.Items.Add(T("Pixelado (nearest)")); cbFilter.SelectedIndex = Get("filter", "linear") == "nearest" ? 1 : 0; gr.Controls.Add(cbFilter);

        var go = new GroupBox { Text = T("Outros"), Bounds = new Rectangle(8, 290, 396, 98) }; t.Controls.Add(go);
        ckVsync = new CheckBox { Text = "VSync", Location = new Point(14, 24), AutoSize = true, Checked = Get("vsync", "1") == "1" }; go.Controls.Add(ckVsync);
        go.Controls.Add(L(T("Limite de FPS:"), 14, 56));
        cbFps = new ComboBox { Location = new Point(110, 52), Width = 150, DropDownStyle = ComboBoxStyle.DropDownList };
        cbFps.Items.AddRange(new object[] { "30", T("60 (original)"), "90", "120", "144", "165", "240", T("Sem limite") });
        { string fv = Get("fps", "60"); int fi = Array.IndexOf(new[] { "30", "60", "90", "120", "144", "165", "240", "0" }, fv); cbFps.SelectedIndex = fi < 0 ? 1 : fi; }
        go.Controls.Add(cbFps);
        ckProf = new CheckBox { Text = T("Diagnóstico (perf_log.txt)"), Location = new Point(200, 24), AutoSize = true, Checked = Get("prof", "0") == "1" }; go.Controls.Add(ckProf);
        go.Controls.Add(new Label { Text = T("Acima de 60 altera o passo da lógica (experimental)"), Location = new Point(14, 72), AutoSize = true, ForeColor = Color.DimGray, Font = new Font("Segoe UI", 7.5f) });

        lbInfo = new Label { Location = new Point(12, 394), AutoSize = true, ForeColor = Color.DimGray }; t.Controls.Add(lbInfo);

        cbRes.SelectedIndexChanged += (s, e) => UpdateAdvancedState();
        cbAspect.SelectedIndexChanged += (s, e) => UpdateAdvancedState();
        cbScale.SelectedIndexChanged += (s, e) => UpdateAdvancedState();
    }

    void UpdateAdvancedState()
    {
        bool custom = cbRes.SelectedIndex == ResNames.Length - 1, auto = cbRes.SelectedIndex == 0;
        nudW.Enabled = nudH.Enabled = custom; tb.Enabled = auto;
        int gw = Aspects[cbAspect.SelectedIndex].W, sc = cbScale.SelectedIndex + 1;
        lbInfo.Text = T("Renderização interna: ") + (gw * sc) + " x " + (576 * sc) + " px";
    }

    // ------------------------------------------------------------------ settings / backup / launch
    void SaveCfg()
    {
        var l = new List<string> {
            "app=" + lbApp.Text, "profile=" + CurProfile(), "showname=" + (ckShowName.Checked ? 1 : 0),
            "scale=" + (cbScale.SelectedIndex + 1), "winpct=" + tb.Value, "aspect=" + cbAspect.SelectedIndex,
            "winres=" + cbRes.SelectedIndex, "winw=" + (int)nudW.Value, "winh=" + (int)nudH.Value,
            "fullscreen=" + (ckFull.Checked ? 1 : 0), "vsync=" + (ckVsync.Checked ? 1 : 0), "filter=" + (cbFilter.SelectedIndex == 1 ? "nearest" : "linear"),
            "audio=" + (ckAudio.Checked ? 1 : 0), "pad=" + (ckPad.Checked ? 1 : 0), "padDead=" + tbDead.Value, "fps=" + FpsVal(), "kbm=" + (ckKbm.Checked ? 1 : 0), "prof=" + (ckProf.Checked ? 1 : 0), "upd=" + (ckUpd.Checked ? 1 : 0), "skipver=" + Get("skipver", ""), "lang=" + I18n.Codes[Math.Max(0, cbLang.SelectedIndex)], "glang=" + GLangCode[Math.Max(0, cbGLang.SelectedIndex)] };
        for (int i = 0; i < ActId.Length; i++) { l.Add("bind_" + ActId[i] + "=" + txKey[i, 0].Tag); l.Add("bind_" + ActId[i] + "2=" + txKey[i, 1].Tag); if (cbBtn[i] != null) l.Add("btn_" + ActId[i] + "=" + cbBtn[i].SelectedItem); }
        try { File.WriteAllLines(cfgFile, l); } catch { }
    }

    static void CopyDir(string src, string dst)
    {
        Directory.CreateDirectory(dst);
        foreach (var f in Directory.GetFiles(src)) if (!f.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)) File.Copy(f, Path.Combine(dst, Path.GetFileName(f)), true);
        foreach (var d in Directory.GetDirectories(src)) CopyDir(d, Path.Combine(dst, Path.GetFileName(d)));
    }

    void Backup()
    {
        string p = CurProfile(), src = SaveDir(p);
        if (!Directory.Exists(src) || Directory.GetFiles(src, "*", SearchOption.AllDirectories).Length == 0) { lbSt.Text = T("Nada para salvar: o perfil ainda não tem arquivos."); return; }
        string dst = Path.Combine(bakRoot, (p == "" ? "padrao" : p) + "_" + DateTime.Now.ToString("yyyyMMdd_HHmmss"));
        Directory.CreateDirectory(dst);
        foreach (var f in Directory.GetFiles(src)) if (!f.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)) File.Copy(f, Path.Combine(dst, Path.GetFileName(f)), true);
        if (p != "") foreach (var d in Directory.GetDirectories(src)) CopyDir(d, Path.Combine(dst, Path.GetFileName(d)));   // the default profile's folder holds the named profiles: skip them
        lbSt.Text = T("Backup criado: ") + Path.GetFileName(dst);
    }

    void Restore()
    {
        Directory.CreateDirectory(bakRoot);
        using (var dlg = new FolderBrowserDialog { SelectedPath = bakRoot, Description = T("Escolha a pasta de backup a restaurar no perfil atual") })
        {
            if (dlg.ShowDialog() != DialogResult.OK) return;
            if (MessageBox.Show(T("Substituir o save do perfil '") + cbProf.Text + T("' por este backup?"), T("Restaurar"), MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
            string dst = SaveDir(CurProfile()); Directory.CreateDirectory(dst);
            foreach (var f in Directory.GetFiles(dst)) File.Delete(f);
            CopyDir(dlg.SelectedPath, dst);
            lbSt.Text = T("Restaurado de: ") + Path.GetFileName(dlg.SelectedPath);
        }
    }

    string FpsVal() { return new[] { "30", "60", "90", "120", "144", "165", "240", "0" }[Math.Max(0, cbFps.SelectedIndex)]; }
    // Extracts Payload/<name>.app from an .ipa (a zip) into <launcher dir>\game and points the launcher at it.
    void ImportIpa()
    {
        string ipa;
        using (var dlg = new OpenFileDialog { Title = T("Selecione o IPA do Xenowerk 2.0.7"), Filter = T("IPA (*.ipa)|*.ipa|Todos os arquivos|*.*") })
        { if (dlg.ShowDialog() != DialogResult.OK) return; ipa = dlg.FileName; }
        string old = lbApp.Text;
        try
        {
            Cursor = Cursors.WaitCursor; Enabled = false;
            using (var zip = ZipFile.OpenRead(ipa))
            {
                string appPrefix = null;
                foreach (var en in zip.Entries)
                {
                    var n = en.FullName; if (!n.StartsWith("Payload/", StringComparison.OrdinalIgnoreCase)) continue;
                    int i = n.IndexOf('/', 8); if (i < 0) continue;
                    string top = n.Substring(0, i + 1);
                    if (top.EndsWith(".app/", StringComparison.OrdinalIgnoreCase) && zip.GetEntry(top + "Info.plist") != null) { appPrefix = top; break; }
                }
                if (appPrefix == null) { MessageBox.Show(T("Este arquivo não parece um IPA válido (não há Payload/*.app com Info.plist)."), "Xenowerk", MessageBoxButtons.OK, MessageBoxIcon.Error); return; }
                string appName = appPrefix.Substring(8, appPrefix.Length - 9);
                var plist = zip.GetEntry(appPrefix + "Info.plist"); byte[] pb;
                using (var ms = new MemoryStream()) { using (var st = plist.Open()) st.CopyTo(ms); pb = ms.ToArray(); }
                string txt = Encoding.ASCII.GetString(pb);
                bool known = txt.IndexOf("com.", StringComparison.Ordinal) >= 0 && txt.IndexOf("2.0.7", StringComparison.Ordinal) >= 0 && txt.IndexOf("enowerk", StringComparison.Ordinal) >= 0;
                if (!known && MessageBox.Show(T("Não consegui confirmar que este IPA é o Xenowerk 2.0.7 (o recompilado só funciona com essa versão). Extrair mesmo assim?"), "Xenowerk", MessageBoxButtons.YesNo, MessageBoxIcon.Warning) != DialogResult.Yes) return;
                string root = Path.Combine(dir, "game"), dest = Path.GetFullPath(Path.Combine(root, appName));
                if (Directory.Exists(dest))
                {
                    if (MessageBox.Show(T("Já existe um jogo extraído em:\n") + dest + T("\n\nSubstituir? (os saves em save\\ não são afetados)"), "Xenowerk", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
                    Directory.Delete(dest, true);
                }
                int total = 0, done = 0; foreach (var en in zip.Entries) if (en.FullName.StartsWith(appPrefix, StringComparison.Ordinal) && en.Name != "") total++;
                string destSep = dest.TrimEnd('\\') + "\\";
                foreach (var en in zip.Entries)
                {
                    if (!en.FullName.StartsWith(appPrefix, StringComparison.Ordinal) || en.Name == "") continue;
                    string target = Path.GetFullPath(Path.Combine(dest, en.FullName.Substring(appPrefix.Length).Replace('/', '\\')));
                    if (!target.StartsWith(destSep, StringComparison.OrdinalIgnoreCase)) continue;     // never write outside the game folder
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    en.ExtractToFile(target, true);
                    if (++done % 25 == 0) { lbApp.Text = T("Extraindo... ") + (done * 100 / Math.Max(1, total)) + "%"; Application.DoEvents(); }
                }
                if (!File.Exists(Path.Combine(dest, "Xenowerk")))
                { MessageBox.Show(T("Extração concluída, mas o executável 'Xenowerk' não está no IPA. Verifique se é o IPA correto."), "Xenowerk", MessageBoxButtons.OK, MessageBoxIcon.Warning); lbApp.Text = old; return; }
                lbApp.Text = dest; SaveCfg();
                MessageBox.Show(T("Jogo extraído em:\n") + dest, "Xenowerk", MessageBoxButtons.OK, MessageBoxIcon.Information);
                old = null;
            }
        }
        catch (Exception ex) { MessageBox.Show(T("Falha ao importar o IPA:\n") + ex.Message, "Xenowerk", MessageBoxButtons.OK, MessageBoxIcon.Error); }
        finally { Enabled = true; Cursor = Cursors.Default; if (old != null) lbApp.Text = old; }
    }

    void Play()
    {
        string exe = Path.Combine(dir, "xenowerk.exe"), app = lbApp.Text;
        if (!File.Exists(Path.Combine(app, "Xenowerk"))) { MessageBox.Show(T("Jogo não encontrado. Use \"Importar IPA...\" (o .ipa do Xenowerk 2.0.7) ou \"Pasta...\" (pasta Xenowerk.app já extraída)."), "Xenowerk", MessageBoxButtons.OK, MessageBoxIcon.Error); return; }
        if (!File.Exists(exe)) { MessageBox.Show("xenowerk.exe não encontrado ao lado do launcher.", "Xenowerk", MessageBoxButtons.OK, MessageBoxIcon.Error); return; }
        SaveCfg();
        var psi = new ProcessStartInfo(exe, "\"" + app + "\"") { WorkingDirectory = dir, UseShellExecute = false, CreateNoWindow = true };
        var e = psi.EnvironmentVariables;
        e["XW_W"] = Aspects[cbAspect.SelectedIndex].W.ToString(); e["XW_H"] = "576";
        e["XW_SCALE"] = (cbScale.SelectedIndex + 1).ToString(); e["XW_FILTER"] = cbFilter.SelectedIndex == 1 ? "nearest" : "linear";
        e["XW_FULLSCREEN"] = ckFull.Checked ? "1" : "0"; e["XW_VSYNC"] = ckVsync.Checked ? "1" : "0"; e["XW_FPS"] = FpsVal(); e["XW_QUIET"] = "1"; if (GLangCode[Math.Max(0, cbGLang.SelectedIndex)] != "") e["XW_LANG"] = GLangCode[cbGLang.SelectedIndex]; if (ckProf.Checked) e["XW_PROF"] = "1";
        int ri = cbRes.SelectedIndex;
        if (ri == 0) e["XW_WINPCT"] = tb.Value.ToString();
        else if (ri == ResNames.Length - 1) { e["XW_WINW"] = ((int)nudW.Value).ToString(); e["XW_WINH"] = ((int)nudH.Value).ToString(); }
        else { e["XW_WINW"] = ResSizes[ri, 0].ToString(); e["XW_WINH"] = ResSizes[ri, 1].ToString(); }
        if (!ckAudio.Checked) e["XW_NOAUDIO"] = "1";
        e["XW_KBM"] = ckKbm.Checked ? "1" : "0";
        for (int i = 0; i < ActId.Length; i++) { e["XW_BIND_" + ActId[i]] = (string)txKey[i, 0].Tag; e["XW_BIND_" + ActId[i] + "2"] = (string)txKey[i, 1].Tag; if (cbBtn[i] != null) e["XW_BTN_" + ActId[i]] = (string)cbBtn[i].SelectedItem; }
        e["XW_PAD"] = ckPad.Checked ? "1" : "0"; e["XW_PADDEAD"] = (tbDead.Value / 100.0).ToString(CultureInfo.InvariantCulture);
        string p = CurProfile();
        if (p != "") { e["XW_PROFILE"] = p; if (ckShowName.Checked) e["XW_PLAYER"] = p; }
        Process.Start(psi); Close();
    }
}
