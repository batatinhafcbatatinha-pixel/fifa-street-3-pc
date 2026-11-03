// The launcher window: Play (status, install / repair, play), Install (the setup log) and
// PC settings (the game's ini, the same file the in-game F1 menu uses).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PortLauncher {

sealed partial class MainForm : Form {
  static readonly Color Back = Color.FromArgb(24, 26, 30), Panel2 = Color.FromArgb(34, 37, 43),
      Text1 = Color.FromArgb(230, 232, 236), Muted = Color.FromArgb(150, 156, 166),
      Accent = Color.FromArgb(78, 160, 98), Danger = Color.FromArgb(200, 90, 80);

  readonly GameConfig cfg;
  string root;                // the kit folder (setup.json, tools\setup.ps1); null before download
  readonly string launcherPath = Application.ExecutablePath;

  readonly TabControl tabs = new TabControl();
  readonly TabPage pagePlay = new TabPage(), pageInstall = new TabPage(), pageSettings = new TabPage(), pageDeps = new TabPage();
  readonly Label status = new Label(), statusDetail = new Label(), stepLabel = new Label(), help = new Label();
  readonly Button playButton = new Button(), installButton = new Button(), folderButton = new Button();
  readonly Button saveButton = new Button(), defaultsButton = new Button(), cancelButton = new Button();
  readonly TextBox log = new TextBox(), folderBox = new TextBox();
  readonly CheckBox shortcutBox = new CheckBox();
  readonly ProgressBar progress = new ProgressBar();
  readonly TableLayoutPanel settingsTable = new TableLayoutPanel();
  readonly Label settingsNote = new Label();
  readonly Dictionary<Setting, Control> editors = new Dictionary<Setting, Control>();
  readonly Dictionary<Setting, Label> labels = new Dictionary<Setting, Label>();
  ComboBox monitorCombo, refreshCombo;
  Process running;
  bool dirty;

  public MainForm(GameConfig config, string exeDir) {
    cfg = config;
    root = IsKit(exeDir) ? exeDir : null;
    Text = cfg.Game + " - " + L.T("PC", "PC");
    try { Icon = Icon.ExtractAssociatedIcon(launcherPath); } catch (Exception) { }
    Font = new Font("Segoe UI", 10f);
    AutoScaleMode = AutoScaleMode.Dpi;
    ClientSize = new Size(920, 660);
    MinimumSize = new Size(760, 560);
    StartPosition = FormStartPosition.CenterScreen;
    BackColor = Back;
    ForeColor = Text1;

    var header = new Panel { Dock = DockStyle.Top, Height = 86, BackColor = Panel2, Padding = new Padding(24, 14, 24, 10) };
    var title = new Label { Text = cfg.Game.ToUpperInvariant(), Font = new Font("Segoe UI Semibold", 22f), AutoSize = true, Location = new Point(22, 10) };
    var sub = new Label {
      Text = L.T("Port para PC, montado a partir do seu próprio disco", "PC port, built from your own disc") +
             (string.IsNullOrEmpty(cfg.Disc) ? "" : "  ·  " + cfg.Disc),
      ForeColor = Muted, AutoSize = true, Location = new Point(26, 56) };
    header.Controls.Add(title);
    header.Controls.Add(sub);

    tabs.Dock = DockStyle.Fill;
    tabs.Padding = new Point(18, 6);
    pagePlay.Text = L.T("Jogar", "Play");
    pageInstall.Text = L.T("Instalação", "Install");
    pageSettings.Text = L.T("Configurações do PC", "PC settings");
    pageDeps.Text = L.T("Dependências", "Dependencies");
    foreach (var p in new[] { pagePlay, pageDeps, pageInstall, pageSettings }) {
      p.BackColor = Back; p.ForeColor = Text1; p.Padding = new Padding(24);
      tabs.TabPages.Add(p);
    }
    BuildPlayPage();
    BuildInstallPage();
    BuildSettingsPage();
    BuildDepsPage();
    // Our own tab bar (the Windows tab strip cannot be themed dark): the pages' headers are hidden.
    tabs.Appearance = TabAppearance.FlatButtons; tabs.ItemSize = new Size(0, 1); tabs.SizeMode = TabSizeMode.Fixed;
    var nav = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 46, BackColor = Panel2, Padding = new Padding(18, 0, 0, 0), WrapContents = false };
    foreach (TabPage page in tabs.TabPages) {
      var b = new Button { Text = page.Text, FlatStyle = FlatStyle.Flat, AutoSize = true, Height = 42, Tag = page,
                           ForeColor = Muted, BackColor = Panel2, Margin = new Padding(0, 4, 4, 0), Padding = new Padding(10, 0, 10, 0),
                           Font = new Font("Segoe UI Semibold", 10.5f), Cursor = Cursors.Hand };
      b.FlatAppearance.BorderSize = 0;
      b.FlatAppearance.MouseOverBackColor = Color.FromArgb(44, 48, 56);
      b.Click += (o, e) => tabs.SelectedTab = (TabPage)((Button)o).Tag;
      navButtons.Add(b);
      nav.Controls.Add(b);
    }
    tabs.SelectedIndexChanged += (o, e) => PaintNav();
    PaintNav();
    Controls.Add(tabs);
    Controls.Add(nav);
    Controls.Add(header);
    tabs.SelectedIndexChanged += (s, e) => {
      if (tabs.SelectedTab == pageSettings) LoadSettings();
      if (tabs.SelectedTab == pageDeps) CheckDeps();
    };
    FormClosing += OnClosing;
    RefreshStatus();
  }

  readonly List<Button> navButtons = new List<Button>();

  public void ShotOnLoad(string file, int tab) {
    Shown += (o, e) => {
      tabs.SelectedIndex = Math.Max(0, Math.Min(tabs.TabCount - 1, tab));
      var timer = new Timer { Interval = 5000 };
      timer.Tick += (t, a) => {
        timer.Stop();
        using (var bmp = new Bitmap(Width, Height)) {
          DrawToBitmap(bmp, new Rectangle(0, 0, Width, Height));
          bmp.Save(file, System.Drawing.Imaging.ImageFormat.Png);
        }
        Close();
      };
      timer.Start();
    };
  }

  void PaintNav() {
    foreach (var b in navButtons) {
      bool on = b.Tag == tabs.SelectedTab;
      b.ForeColor = on ? Text1 : Muted;
      b.BackColor = on ? Back : Panel2;
    }
  }

  // "owner/fifa-street-3-pc" -> "fifa-street-3-pc": the folder name.
  string RepoName => string.IsNullOrEmpty(cfg.Repo) ? "game" : cfg.Repo.Substring(cfg.Repo.LastIndexOf('/') + 1);

  static bool IsKit(string dir) => File.Exists(Path.Combine(dir, "setup.json")) && File.Exists(Path.Combine(dir, "tools", "setup.ps1"));
  string GameExe => root == null || cfg.Exe == null ? null : Path.Combine(root, cfg.Exe);
  string IniPath => root == null || cfg.Ini == null ? null : Path.Combine(root, cfg.Ini);
  bool Installed => GameExe != null && File.Exists(GameExe);
  bool Busy => running != null && !running.HasExited;

  static Button MakeButton(Button b, string text, Color back, int width = 220, int height = 44) {
    b.Text = text; b.FlatStyle = FlatStyle.Flat; b.BackColor = back; b.ForeColor = Color.White;
    b.FlatAppearance.BorderSize = 0; b.Size = new Size(width, height); b.Cursor = Cursors.Hand;
    b.Font = new Font("Segoe UI Semibold", 11f);
    return b;
  }

  // ------------------------------------------------------------------ Play page

  void BuildPlayPage() {
    var flow = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true };
    status.AutoSize = true; status.Font = new Font("Segoe UI Semibold", 14f);
    statusDetail.AutoSize = true; statusDetail.ForeColor = Muted; statusDetail.MaximumSize = new Size(800, 0);
    statusDetail.Margin = new Padding(3, 6, 3, 18);
    flow.Controls.Add(status);
    flow.Controls.Add(statusDetail);

    var folderRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 0, 0, 10) };
    var folderLabel = new Label { Text = L.T("Pasta de instalação:", "Install folder:"), AutoSize = true, Margin = new Padding(3, 8, 6, 0) };
    folderBox.Width = 460; folderBox.BackColor = Panel2; folderBox.ForeColor = Text1; folderBox.BorderStyle = BorderStyle.FixedSingle;
    folderBox.Margin = new Padding(3, 5, 6, 0);
    MakeButton(folderButton, L.T("Escolher...", "Browse..."), Panel2, 110, 32);
    folderButton.Font = Font;
    folderButton.Click += (s, e) => {
      using (var d = new FolderBrowserDialog { Description = L.T("Onde instalar o jogo", "Where to install the game"), SelectedPath = folderBox.Text }) {
        if (d.ShowDialog(this) == DialogResult.OK) folderBox.Text = Path.Combine(d.SelectedPath, RepoName);
      }
    };
    folderRow.Controls.AddRange(new Control[] { folderLabel, folderBox, folderButton });
    folderRow.Tag = "folder";
    flow.Controls.Add(folderRow);

    var buttons = new FlowLayoutPanel { AutoSize = true, WrapContents = false };
    MakeButton(playButton, L.T("JOGAR", "PLAY"), Accent);
    playButton.Font = new Font("Segoe UI Semibold", 14f);
    playButton.Click += (s, e) => Play();
    MakeButton(installButton, L.T("INSTALAR", "INSTALL"), Color.FromArgb(60, 96, 160));
    installButton.Click += (s, e) => Install();
    buttons.Controls.Add(playButton);
    buttons.Controls.Add(installButton);
    flow.Controls.Add(buttons);

    shortcutBox.Text = L.T("Criar atalho na área de trabalho", "Create a desktop shortcut");
    shortcutBox.Checked = true; shortcutBox.AutoSize = true; shortcutBox.Margin = new Padding(3, 12, 3, 3);
    flow.Controls.Add(shortcutBox);

    var links = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 18, 0, 0) };
    links.Controls.Add(MakeLink(L.T("Abrir a pasta do jogo", "Open the game folder"), () => { if (root != null) Process.Start("explorer.exe", "\"" + root + "\""); }));
    links.Controls.Add(MakeLink(L.T("Ver o log da instalação", "Show the install log"), () => tabs.SelectedTab = pageInstall));
    links.Controls.Add(MakeLink(L.T("Configurações do PC", "PC settings"), () => tabs.SelectedTab = pageSettings));
    flow.Controls.Add(links);

    var note = new Label {
      AutoSize = true, ForeColor = Muted, MaximumSize = new Size(800, 0), Margin = new Padding(3, 24, 3, 3),
      Text = L.T("Seu jogo nunca é enviado para lugar nenhum: tudo é montado neste PC a partir do seu disco. " +
                 "No jogo, F1 (ou Back+Start no controle) abre as mesmas configurações.",
                 "Your game is never uploaded anywhere: everything is built on this PC from your disc. " +
                 "In the game, F1 (or Back+Start on a controller) opens the same settings."),
    };
    flow.Controls.Add(note);
    pagePlay.Controls.Add(flow);
    folderBox.Text = DefaultInstallFolder();
  }

  LinkLabel MakeLink(string text, Action action) {
    var l = new LinkLabel { Text = text, AutoSize = true, LinkColor = Color.FromArgb(120, 170, 240), ActiveLinkColor = Text1, Margin = new Padding(3, 3, 24, 3) };
    l.LinkClicked += (s, e) => action();
    return l;
  }

  // The fixed drive with the most free space (the build needs several GB).
  string DefaultInstallFolder() {
    var best = DriveInfo.GetDrives().Where(d => d.DriveType == DriveType.Fixed && d.IsReady)
        .OrderByDescending(d => d.AvailableFreeSpace).FirstOrDefault();
    return Path.Combine(best?.RootDirectory.FullName ?? "C:\\", "Games", RepoName);
  }

  void RefreshStatus() {
    bool busy = Busy;
    foreach (Control c in ((FlowLayoutPanel)pagePlay.Controls[0]).Controls)
      if ((c.Tag as string) == "folder") c.Visible = root == null;
    if (busy && running.StartInfo.FileName.EndsWith("powershell.exe", StringComparison.OrdinalIgnoreCase)) {
      status.Text = L.T("Instalando...", "Installing..."); status.ForeColor = Text1;
      statusDetail.Text = L.T("Acompanhe na aba Instalação. Pode demorar; deixe esta janela aberta.", "Follow it on the Install tab. It takes a while; keep this window open.");
    } else if (busy) {
      status.Text = L.T("Em jogo", "Playing"); status.ForeColor = Accent;
      statusDetail.Text = L.T("O launcher volta quando o jogo fechar.", "The launcher comes back when the game closes.");
    } else if (root == null) {
      status.Text = L.T("Não instalado", "Not installed"); status.ForeColor = Text1;
      statusDetail.Text = L.T(
          $"O instalador baixa o kit do port (codigo-fonte, sem nada do jogo), instala as ferramentas de compilacao, pede o seu disco ({cfg.Disc}) e monta a versao para PC neste computador. " +
          $"Precisa de cerca de {cfg.DiskGb} GB livres e {cfg.Minutes} minutos.",
          $"The installer downloads the port kit (source code, nothing from the game), installs the build tools, asks for your disc ({cfg.Disc}) and builds the PC version on this computer. " +
          $"About {cfg.DiskGb} GB free and {cfg.Minutes} minutes are needed.");
    } else if (Installed) {
      status.Text = L.T("Pronto para jogar", "Ready to play"); status.ForeColor = Accent;
      statusDetail.Text = root;
    } else {
      status.Text = L.T("Instalação incompleta", "Not built yet"); status.ForeColor = Text1;
      statusDetail.Text = L.T($"O kit está em {root}. Clique em INSTALAR para instalar as ferramentas, escolher o seu disco e montar o jogo (cerca de {cfg.Minutes} minutos).",
                              $"The kit is in {root}. Click INSTALL to get the build tools, choose your disc and build the game (about {cfg.Minutes} minutes).");
    }
    playButton.Enabled = Installed && !busy;
    playButton.BackColor = playButton.Enabled ? Accent : Color.FromArgb(60, 64, 70);
    installButton.Enabled = !busy;
    installButton.Text = Installed ? L.T("REPARAR / ATUALIZAR", "REPAIR / UPDATE") : L.T("INSTALAR", "INSTALL");
    shortcutBox.Visible = !Installed;
    cancelButton.Enabled = busy;
    UpdateDepButtons();
    if (!busy && deps.Count > 0 && tabs.SelectedTab == pageDeps) CheckDeps();
  }

  // ------------------------------------------------------------------ Install page

  void BuildInstallPage() {
    stepLabel.Dock = DockStyle.Top; stepLabel.Height = 30; stepLabel.Font = new Font("Segoe UI Semibold", 11f);
    stepLabel.Text = L.T("Nada em andamento.", "Nothing running.");
    progress.Dock = DockStyle.Top; progress.Height = 8; progress.Style = ProgressBarStyle.Continuous; progress.Maximum = 5;
    log.Dock = DockStyle.Fill; log.Multiline = true; log.ReadOnly = true; log.ScrollBars = ScrollBars.Vertical;
    log.BackColor = Color.FromArgb(14, 15, 18); log.ForeColor = Color.FromArgb(200, 204, 210); log.BorderStyle = BorderStyle.None;
    log.Font = new Font("Consolas", 9.5f);
    var bottom = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 52, FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(0, 8, 0, 0) };
    MakeButton(cancelButton, L.T("Parar", "Stop"), Danger, 120, 36);
    cancelButton.Click += (s, e) => StopInstall();
    bottom.Controls.Add(cancelButton);
    var spacer = new Panel { Dock = DockStyle.Top, Height = 10 };
    pageInstall.Controls.Add(log);
    pageInstall.Controls.Add(spacer);
    pageInstall.Controls.Add(progress);
    pageInstall.Controls.Add(stepLabel);
    pageInstall.Controls.Add(bottom);
  }

  void AppendLog(string line) {
    if (line == null) return;
    if (InvokeRequired) { BeginInvoke(new Action<string>(AppendLog), line); return; }
    log.AppendText(line + Environment.NewLine);
    var m = Regex.Match(line, @"^== (\d)/(\d) (.*)$");
    if (m.Success) {
      progress.Maximum = int.Parse(m.Groups[2].Value);
      progress.Value = Math.Min(progress.Maximum, int.Parse(m.Groups[1].Value) - 1);
      stepLabel.Text = m.Groups[1].Value + "/" + m.Groups[2].Value + "  " + m.Groups[3].Value;
    }
  }

  async void Install() {
    if (Busy) return;
    string target = root;
    if (target == null) {
      target = folderBox.Text.Trim();
      if (target.Length == 0 || !Path.IsPathRooted(target)) {
        MessageBox.Show(this, L.T("Escolha uma pasta de instalação.", "Choose an install folder."), Text); return;
      }
    }
    string msg = L.T(
        "O instalador vai:\n\n" +
        (root == null ? "- baixar o kit do port do GitHub (só código-fonte, cerca de 1 MB)\n" : "") +
        "- instalar o Python e o Microsoft C++ Build Tools se faltarem (o Build Tools tem cerca de 7 GB e pede permissão de administrador)\n" +
        "- baixar as ferramentas de compilação para dentro da pasta do jogo\n" +
        $"- pedir o seu disco ({cfg.Disc}) ou a imagem .iso\n" +
        $"- montar a versão para PC (cerca de {cfg.Minutes} minutos)\n\n" +
        $"Pasta: {target}\n\nContinuar?",
        "The installer will:\n\n" +
        (root == null ? "- download the port kit from GitHub (source code only, about 1 MB)\n" : "") +
        "- install Python and the Microsoft C++ Build Tools if missing (the Build Tools are about 7 GB and ask for administrator permission)\n" +
        "- download the build tools into the game folder\n" +
        $"- ask for your disc ({cfg.Disc}) or its .iso image\n" +
        $"- build the PC version (about {cfg.Minutes} minutes)\n\n" +
        $"Folder: {target}\n\nContinue?");
    if (MessageBox.Show(this, msg, Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;

    tabs.SelectedTab = pageInstall;
    log.Clear();
    progress.Value = 0;
    installButton.Enabled = false;
    try {
      if (root == null) {
        stepLabel.Text = L.T("Baixando o kit do port...", "Downloading the port kit...");
        string kit = await Task.Run(() => KitDownload.Fetch(cfg, target, AppendLog));
        root = kit;
        Text = cfg.Game + " - PC";
      }
    } catch (Exception ex) {
      AppendLog(ex.Message);
      stepLabel.Text = L.T("O download do kit falhou.", "Downloading the kit failed.");
      RefreshStatus();
      return;
    }
    RunSetup();
  }

  static readonly Encoding Oem = Encoding.GetEncoding(System.Globalization.CultureInfo.CurrentCulture.TextInfo.OEMCodePage);

  void RunSetup(string only = null) {
    var psi = new ProcessStartInfo {
      FileName = Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe"),
      Arguments = $"-NoProfile -ExecutionPolicy Bypass -File \"{Path.Combine(root, cfg.Setup)}\" -Yes -NoShortcut" + (only != null ? " -Only " + only : ""),
      WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true,
      RedirectStandardOutput = true, RedirectStandardError = true,
      StandardOutputEncoding = Oem, StandardErrorEncoding = Oem,
    };
    psi.EnvironmentVariables["PORT_LAUNCHER"] = "1";
    // PowerShell writes the console's OEM code page into the pipe; Python is told to do the same.
    psi.EnvironmentVariables["PYTHONIOENCODING"] = "cp" + Oem.CodePage + ":replace";
    var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
    p.OutputDataReceived += (s, e) => AppendLog(e.Data);
    p.ErrorDataReceived += (s, e) => AppendLog(e.Data);
    p.Exited += (s, e) => BeginInvoke(new Action(() => SetupFinished(p.ExitCode, only)));
    running = p;
    stepLabel.Text = L.T("Preparando...", "Starting...");
    p.Start();
    p.BeginOutputReadLine();
    p.BeginErrorReadLine();
    RefreshStatus();
  }

  void SetupFinished(int code, string only) {
    running = null;
    if (only != null) {
      stepLabel.Text = code == 0 ? L.T("Pronto.", "Done.") : L.T("Parou com erro. Veja o log abaixo (também em setup-log.txt).", "Stopped with an error. See the log below (also in setup-log.txt).");
      tabs.SelectedTab = pageDeps;
      RefreshStatus();
      return;
    }
    if (code == 0 && Installed) {
      progress.Value = progress.Maximum;
      stepLabel.Text = L.T("Pronto! O jogo está instalado.", "Done! The game is installed.");
      if (shortcutBox.Checked) CreateShortcut();
      tabs.SelectedTab = pagePlay;
    } else {
      stepLabel.Text = L.T("A instalação parou. Veja o log abaixo (também em setup-log.txt) e clique em INSTALAR de novo: as etapas prontas são puladas.",
                           "Setup stopped. See the log below (also in setup-log.txt) and click INSTALL again: finished steps are skipped.");
    }
    RefreshStatus();
  }

  void StopInstall() {
    if (!Busy) return;
    if (MessageBox.Show(this, L.T("Parar agora? Dá para continuar depois: as etapas prontas são puladas.", "Stop now? You can continue later: finished steps are skipped."),
                        Text, MessageBoxButtons.YesNo) != DialogResult.Yes) return;
    KillTree(running.Id);
  }

  static void KillTree(int pid) {
    try { Process.Start(new ProcessStartInfo("taskkill", $"/T /F /PID {pid}") { CreateNoWindow = true, UseShellExecute = false })?.WaitForExit(); }
    catch (Exception) { }
  }

  // The launcher itself is the shortcut target: it plays and keeps the settings at hand.
  void CreateShortcut() {
    try {
      string exe = Path.Combine(root, Path.GetFileName(launcherPath));
      if (!File.Exists(exe)) File.Copy(launcherPath, exe);
      var shellType = Type.GetTypeFromProgID("WScript.Shell");
      dynamic shell = Activator.CreateInstance(shellType);
      dynamic link = shell.CreateShortcut(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), cfg.Game + ".lnk"));
      link.TargetPath = exe;
      link.WorkingDirectory = root;
      link.IconLocation = exe + ",0";
      link.Save();
    } catch (Exception ex) {
      AppendLog("Shortcut: " + ex.Message);
    }
  }

  void OnClosing(object sender, FormClosingEventArgs e) {
    if (dirty && root != null) SaveSettings();
    if (Busy && running.StartInfo.FileName.EndsWith("powershell.exe", StringComparison.OrdinalIgnoreCase)) {
      if (MessageBox.Show(this, L.T("A instalação ainda está rodando. Parar e fechar?", "Setup is still running. Stop it and close?"),
                          Text, MessageBoxButtons.YesNo) != DialogResult.Yes) { e.Cancel = true; return; }
      KillTree(running.Id);
    }
  }

  // ------------------------------------------------------------------ Play

  void Play() {
    if (!Installed || Busy) return;
    if (dirty) SaveSettings();
    var output = new StringBuilder();
    var psi = new ProcessStartInfo {
      FileName = Path.Combine(Environment.SystemDirectory, "cmd.exe"),
      Arguments = $"/c \"\"{Path.Combine(root, cfg.Play)}\" < nul\"",
      WorkingDirectory = root, UseShellExecute = false, CreateNoWindow = true,
      RedirectStandardOutput = true, RedirectStandardError = true,
    };
    var p = new Process { StartInfo = psi, EnableRaisingEvents = true };
    p.OutputDataReceived += (s, e) => { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
    p.ErrorDataReceived += (s, e) => { if (e.Data != null) lock (output) output.AppendLine(e.Data); };
    p.Exited += (s, e) => BeginInvoke(new Action(() => {
      running = null;
      WindowState = FormWindowState.Normal;
      Activate();
      if (p.ExitCode != 0) {
        string text;
        lock (output) text = output.ToString();
        if (text.Length > 3000) text = "..." + text.Substring(text.Length - 3000);
        MessageBox.Show(this, L.T("O jogo fechou com erro (código ", "The game closed with an error (code ") + p.ExitCode + ").\n\n" + text,
                        Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
      }
      RefreshStatus();
    }));
    running = p;
    p.Start();
    p.BeginOutputReadLine();
    p.BeginErrorReadLine();
    WindowState = FormWindowState.Minimized;
    RefreshStatus();
  }

  // ------------------------------------------------------------------ PC settings page

  void BuildSettingsPage() {
    help.Dock = DockStyle.Bottom; help.Height = 64; help.ForeColor = Muted; help.Padding = new Padding(4, 10, 4, 0);
    var bottom = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 52, FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(0, 8, 0, 0) };
    MakeButton(saveButton, L.T("Salvar", "Save"), Accent, 140, 36);
    saveButton.Click += (s, e) => SaveSettings();
    MakeButton(defaultsButton, L.T("Restaurar padrões", "Restore defaults"), Panel2, 180, 36);
    defaultsButton.Click += (s, e) => { foreach (var kv in editors) SetEditor(kv.Key, kv.Key.Default); dirty = true; };
    bottom.Controls.Add(saveButton);
    bottom.Controls.Add(defaultsButton);
    settingsNote.Dock = DockStyle.Top; settingsNote.Height = 40; settingsNote.ForeColor = Muted;
    var scroller = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
    settingsTable.AutoSize = true; settingsTable.ColumnCount = 2; settingsTable.Dock = DockStyle.Top;
    settingsTable.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 300));
    settingsTable.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 440));
    scroller.Controls.Add(settingsTable);
    pageSettings.Controls.Add(scroller);
    pageSettings.Controls.Add(settingsNote);
    pageSettings.Controls.Add(help);
    pageSettings.Controls.Add(bottom);

    var monitors = Displays.Monitors();
    var gpus = Gpus.List();
    foreach (var g in cfg.Groups) {
      var head = new Label { Text = L.T(g).ToUpperInvariant(), Font = new Font("Segoe UI Semibold", 10.5f), ForeColor = Accent, AutoSize = true, Margin = new Padding(3, 16, 3, 6) };
      settingsTable.Controls.Add(head);
      settingsTable.SetColumnSpan(head, 2);
      foreach (var s in g.Items) {
        var label = new Label { Text = L.T(s) + (s.Restart ? "" : ""), AutoSize = true, Margin = new Padding(3, 9, 3, 3) };
        Control editor;
        if (s.Max > s.Min) {
          var bar = new TrackBar { Minimum = s.Min, Maximum = s.Max, SmallChange = s.Step, LargeChange = s.Step * 2, TickFrequency = s.Step * 2, Width = 300, BackColor = Back };
          var value = new Label { AutoSize = true, Margin = new Padding(6, 9, 3, 3) };
          bar.ValueChanged += (o, e) => { value.Text = bar.Value + "%"; dirty = true; };
          var row = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = Padding.Empty };
          row.Controls.Add(bar); row.Controls.Add(value);
          row.Tag = bar;
          editor = row;
          bar.Enter += (o, e) => ShowHelp(s);
          bar.MouseEnter += (o, e) => ShowHelp(s);
        } else {
          var combo = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 420, FlatStyle = FlatStyle.Flat, BackColor = Panel2, ForeColor = Text1, Margin = new Padding(3, 5, 3, 3) };
          combo.DisplayMember = "Text";
          FillChoices(s, combo, monitors, gpus);
          combo.SelectedIndexChanged += (o, e) => { dirty = true; if (combo == monitorCombo) RefillRefresh(); };
          combo.Enter += (o, e) => ShowHelp(s);
          combo.MouseEnter += (o, e) => ShowHelp(s);
          editor = combo;
        }
        label.MouseEnter += (o, e) => ShowHelp(s);
        editors[s] = editor;
        labels[s] = label;
        settingsTable.Controls.Add(label);
        settingsTable.Controls.Add(editor);
      }
    }
  }

  sealed class Item {
    public int Value; public string Text;
    public override string ToString() => Text;
  }

  void FillChoices(Setting s, ComboBox combo, List<MonitorInfo> monitors, List<KeyValuePair<int, string>> gpus) {
    combo.Items.Clear();
    switch (s.Source) {
      case "monitors":
        monitorCombo = combo;
        for (int i = 0; i < Math.Max(1, monitors.Count); ++i)
          combo.Items.Add(new Item { Value = i, Text = (i + 1) + ": " + (i < monitors.Count ? monitors[i].Name : "?") });
        combo.Tag = monitors;
        return;
      case "refresh":
        refreshCombo = combo;
        RefillRefresh();
        return;
      case "gpus":
        combo.Items.Add(new Item { Value = -1, Text = L.T("Automática (padrão do Windows)", "Automatic (Windows default)") });
        foreach (var g in gpus) combo.Items.Add(new Item { Value = g.Key, Text = g.Value });
        return;
    }
    foreach (var c in s.Choices) combo.Items.Add(new Item { Value = c.Value, Text = L.T(c) });
  }

  void RefillRefresh() {
    if (refreshCombo == null) return;
    var monitors = monitorCombo?.Tag as List<MonitorInfo>;
    int index = monitorCombo != null && monitorCombo.SelectedItem is Item m ? m.Value : 0;
    string device = monitors != null && index < monitors.Count ? monitors[index].Device : null;
    int keep = refreshCombo.SelectedItem is Item r ? r.Value : 0, current = 60;
    var rates = device != null ? Displays.RefreshRates(device, out current) : new List<int>();
    refreshCombo.Items.Clear();
    refreshCombo.Items.Add(new Item { Value = 0, Text = L.T($"A do monitor ({current} Hz)", $"Monitor refresh rate ({current} Hz)") });
    foreach (int hz in rates) refreshCombo.Items.Add(new Item { Value = hz, Text = hz + " Hz" });
    SelectValue(refreshCombo, keep, 0);
  }

  static void SelectValue(ComboBox combo, int value, int fallback) {
    int pick = -1;
    for (int i = 0; i < combo.Items.Count; ++i) if (((Item)combo.Items[i]).Value == value) pick = i;
    if (pick < 0) for (int i = 0; i < combo.Items.Count; ++i) if (((Item)combo.Items[i]).Value == fallback) pick = i;
    combo.SelectedIndex = pick >= 0 ? pick : (combo.Items.Count > 0 ? 0 : -1);
  }

  void SetEditor(Setting s, int value) {
    var editor = editors[s];
    if (editor is ComboBox combo) SelectValue(combo, value, s.Default);
    else if (editor.Tag is TrackBar bar) bar.Value = Math.Max(bar.Minimum, Math.Min(bar.Maximum, value));
  }

  int GetEditor(Setting s) {
    var editor = editors[s];
    if (editor is ComboBox combo) return combo.SelectedItem is Item i ? i.Value : s.Default;
    return editor.Tag is TrackBar bar ? bar.Value : s.Default;
  }

  void ShowHelp(Setting s) {
    help.Text = (L.Pt ? s.HelpPt : s.HelpEn) +
                (s.Restart ? L.T("  (Vale na próxima vez que o jogo abrir.)", "  (Applies the next time the game starts.)") : "");
  }

  bool SettingVisible(Setting s) =>
      s.Requires != "vulkan" || (root != null && cfg.VulkanDir != null && Directory.Exists(Path.Combine(root, cfg.VulkanDir)));

  void LoadSettings() {
    string ini = IniPath;
    bool ready = ini != null && Directory.Exists(Path.GetDirectoryName(ini));
    settingsNote.Text = ready
        ? L.T("Estas são as mesmas opções do menu F1 do jogo. São salvas em ", "These are the same options as the game's F1 menu. Saved in ") + Path.GetFileName(ini) + "."
        : L.T("Instale o jogo primeiro: as configurações ficam ao lado dele.", "Install the game first: the settings are kept next to it.");
    settingsTable.Enabled = ready; saveButton.Enabled = ready; defaultsButton.Enabled = ready;
    settingsTable.SuspendLayout();
    foreach (var kv in editors) {
      bool visible = SettingVisible(kv.Key);
      kv.Value.Visible = visible;
      labels[kv.Key].Visible = visible;
    }
    // Monitor first: the refresh list depends on it.
    foreach (var s in editors.Keys.OrderBy(k => k.Source == "monitors" ? 0 : 1))
      SetEditor(s, ready ? Ini.Get(ini, cfg.Section, s.Key, s.Default) : s.Default);
    settingsTable.ResumeLayout();
    dirty = false;
  }

  void SaveSettings() {
    string ini = IniPath;
    if (ini == null || !Directory.Exists(Path.GetDirectoryName(ini))) return;
    foreach (var s in editors.Keys) if (SettingVisible(s)) Ini.Put(ini, cfg.Section, s.Key, GetEditor(s));
    dirty = false;
    help.Text = L.T("Salvo. O jogo usa estas opções na próxima vez que abrir.", "Saved. The game uses these options the next time it starts.");
  }
}

}
