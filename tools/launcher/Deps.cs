// The Dependencies tab: everything the build needs, what is already on this PC, what is missing,
// how much space each part takes, and a button to install just that part (setup.ps1 -Only).
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace PortLauncher {

sealed partial class MainForm {
  sealed class Dep {
    public string Name, Detail, Only;   // Only: the setup.ps1 -Only part that installs it (null: full setup)
    public double Gb;
    public bool OnSystemDrive;          // installs on C: whatever the game folder is
    public bool? Ok;                    // null while checking
    public string Found;                // where it was found / why it is missing
  }

  readonly TableLayoutPanel depsTable = new TableLayoutPanel();
  readonly Label depsSummary = new Label();
  readonly Button depsAllButton = new Button(), depsRecheckButton = new Button();
  readonly List<Button> depButtons = new List<Button>();
  List<Dep> deps = new List<Dep>();
  bool depsChecking;

  void BuildDepsPage() {
    var intro = new Label {
      Dock = DockStyle.Top, Height = 46, ForeColor = Muted,
      Text = L.T("O que a montagem do jogo precisa neste PC. O que faltar pode ser instalado item por item ou tudo de uma vez.",
                 "What building the game needs on this PC. Anything missing can be installed one by one or all at once."),
    };
    var scroller = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
    depsTable.Dock = DockStyle.Top; depsTable.AutoSize = true; depsTable.ColumnCount = 4;
    depsTable.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 330));
    depsTable.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 150));
    depsTable.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
    depsTable.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 140));
    scroller.Controls.Add(depsTable);
    depsSummary.Dock = DockStyle.Bottom; depsSummary.Height = 70; depsSummary.Padding = new Padding(4, 10, 4, 0);
    var bottom = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 52, FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(0, 8, 0, 0) };
    MakeButton(depsAllButton, L.T("Instalar tudo o que falta", "Install everything missing"), Accent, 260, 36);
    depsAllButton.Click += (s, e) => Install();
    MakeButton(depsRecheckButton, L.T("Verificar de novo", "Check again"), Panel2, 170, 36);
    depsRecheckButton.Click += (s, e) => CheckDeps();
    bottom.Controls.Add(depsAllButton);
    bottom.Controls.Add(depsRecheckButton);
    pageDeps.Controls.Add(scroller);
    pageDeps.Controls.Add(intro);
    pageDeps.Controls.Add(depsSummary);
    pageDeps.Controls.Add(bottom);
  }

  List<Dep> DepList() {
    var list = new List<Dep> {
      new Dep { Name = "Python 3.11+", Detail = L.T("roda os scripts de montagem e o instalador dos arquivos do jogo", "runs the build scripts and the game file installer"),
                Only = "python", Gb = cfg.PythonGb, OnSystemDrive = true },
      new Dep { Name = "Microsoft C++ Build Tools", Detail = L.T("bibliotecas C++ e o Windows SDK (pede administrador)", "C++ libraries and the Windows SDK (asks for administrator)"),
                Only = "buildtools", Gb = cfg.BuildToolsGb, OnSystemDrive = true },
    };
    foreach (var t in cfg.Tools)
      list.Add(new Dep { Name = t.Name, Detail = L.T("fica dentro da pasta do jogo (.local\\tools)", "kept inside the game folder (.local\\tools)"), Only = "tools", Gb = t.Gb });
    list.Add(new Dep { Name = L.T("Seus arquivos do jogo", "Your game files"), Detail = cfg.Disc + L.T(" (do seu disco ou .iso)", " (from your disc or .iso)"),
                       Only = "game", Gb = 0 });
    list.Add(new Dep { Name = L.T("Versão para PC (montada)", "PC version (built)"), Detail = L.T($"tradução e compilação, cerca de {cfg.Minutes} min", $"translation and compile, about {cfg.Minutes} min"),
                       Only = null, Gb = cfg.DiskGb });
    return list;
  }

  async void CheckDeps() {
    if (depsChecking) return;
    depsChecking = true;
    deps = DepList();
    RenderDeps();
    string kit = root;
    var results = await Task.Run(() => {
      var r = new List<KeyValuePair<bool, string>>();
      r.Add(FindPython());
      r.Add(FindBuildTools());
      string systemLlvm = SystemLlvm();
      foreach (var t in cfg.Tools) {
        if (t.Id == "llvm" && systemLlvm != null) { r.Add(new KeyValuePair<bool, string>(true, systemLlvm)); continue; }
        bool ok = kit != null && File.Exists(Path.Combine(kit, t.Check));
        r.Add(new KeyValuePair<bool, string>(ok, ok ? L.T("na pasta do jogo", "in the game folder") : ""));
      }
      r.Add(GameFilesOk(kit));
      r.Add(new KeyValuePair<bool, string>(Installed, Installed ? Path.GetFileName(GameExe) : ""));
      return r;
    });
    for (int i = 0; i < deps.Count && i < results.Count; ++i) { deps[i].Ok = results[i].Key; deps[i].Found = results[i].Value; }
    depsChecking = false;
    RenderDeps();
  }

  void RenderDeps() {
    depsTable.SuspendLayout();
    depsTable.Controls.Clear();
    depsTable.RowStyles.Clear();
    depsTable.RowCount = 0;
    depButtons.Clear();
    foreach (var d in deps) {
      var name = new Label { AutoSize = true, MaximumSize = new Size(320, 0), Margin = new Padding(3, 10, 3, 6),
                             Text = d.Name + "\n" + d.Detail };
      name.Font = Font;
      var state = new Label { AutoSize = true, Margin = new Padding(3, 10, 3, 6), MaximumSize = new Size(145, 0) };
      if (d.Ok == null) { state.Text = L.T("verificando...", "checking..."); state.ForeColor = Muted; }
      else if (d.Ok == true) { state.Text = "✔ " + L.T("instalado", "installed") + (string.IsNullOrEmpty(d.Found) ? "" : "\n" + d.Found); state.ForeColor = Accent; }
      else { state.Text = "✖ " + L.T("falta", "missing") + (string.IsNullOrEmpty(d.Found) ? "" : "\n" + d.Found); state.ForeColor = Danger; }
      var size = new Label { AutoSize = true, Margin = new Padding(3, 10, 3, 6), ForeColor = Muted,
                             Text = d.Gb <= 0 ? "-" : (d.Gb < 1 ? $"~{Math.Max(1, (int)Math.Round(d.Gb * 1024))} MB" : $"~{d.Gb:0.#} GB") + (d.OnSystemDrive ? L.T("\nno C:", "\non C:") : "") };
      var button = new Button();
      MakeButton(button, d.Only == null ? L.T("Montar", "Build") : L.T("Instalar", "Install"), Color.FromArgb(60, 96, 160), 120, 32);
      button.Font = Font;
      button.Margin = new Padding(3, 8, 3, 6);
      button.Visible = d.Ok == false && root != null;  // before the kit is downloaded only "install everything" works
      button.Tag = d;
      button.Click += (s, e) => InstallDep((Dep)((Button)s).Tag);
      depButtons.Add(button);
      // Fixed cells: a hidden button must not shift the next row's cells.
      int row = depsTable.RowCount++;
      depsTable.RowStyles.Add(new RowStyle(SizeType.AutoSize));
      depsTable.Controls.Add(name, 0, row);
      depsTable.Controls.Add(state, 1, row);
      depsTable.Controls.Add(size, 2, row);
      depsTable.Controls.Add(button, 3, row);
    }
    depsTable.ResumeLayout();
    UpdateDepsSummary();
    UpdateDepButtons();
  }

  void UpdateDepsSummary() {
    var missing = deps.Where(d => d.Ok == false).ToList();
    double onC = missing.Where(d => d.OnSystemDrive).Sum(d => d.Gb);
    double here = missing.Where(d => !d.OnSystemDrive).Sum(d => d.Gb);
    string folder = root ?? folderBox.Text;
    string driveName = Path.GetPathRoot(folder);
    double freeHere = FreeGb(driveName), freeC = FreeGb(Path.GetPathRoot(Environment.SystemDirectory));
    bool sameDrive = string.Equals(driveName, Path.GetPathRoot(Environment.SystemDirectory), StringComparison.OrdinalIgnoreCase);
    string text;
    if (deps.Any(d => d.Ok == null)) text = L.T("Verificando...", "Checking...");
    else if (missing.Count == 0) text = L.T("Tudo instalado.", "Everything is installed.");
    else {
      text = L.T($"Faltam {missing.Count} itens. Espaço necessário: ~{here:0.#} GB em {driveName} (livre: {freeHere:0.#} GB)",
                 $"{missing.Count} items missing. Space needed: ~{here:0.#} GB on {driveName} (free: {freeHere:0.#} GB)");
      if (onC > 0) text += sameDrive ? L.T($" + ~{onC:0.#} GB do sistema = ~{here + onC:0.#} GB", $" + ~{onC:0.#} GB of system parts = ~{here + onC:0.#} GB")
                                     : L.T($"; ~{onC:0.#} GB no C: (livre: {freeC:0.#} GB)", $"; ~{onC:0.#} GB on C: (free: {freeC:0.#} GB)");
      text += ".";
      bool short1 = sameDrive ? here + onC > freeHere : here > freeHere || onC > freeC;
      if (short1) text += L.T("\nAtenção: falta espaço em disco.", "\nWarning: not enough disk space.");
    }
    depsSummary.Text = text;
    depsSummary.ForeColor = text.Contains("\n") ? Danger : Text1;
  }

  static double FreeGb(string drive) {
    try { return new DriveInfo(drive).AvailableFreeSpace / (1024.0 * 1024 * 1024); } catch (Exception) { return 0; }
  }

  void UpdateDepButtons() {
    bool busy = Busy;
    foreach (var b in depButtons) b.Enabled = !busy && root != null;
    depsAllButton.Enabled = !busy && deps.Any(d => d.Ok == false);
    depsRecheckButton.Enabled = !busy && !depsChecking;
  }

  void InstallDep(Dep d) {
    if (Busy || root == null) return;
    if (d.Only == null) { Install(); return; }
    tabs.SelectedTab = pageInstall;
    log.Clear();
    progress.Value = 0;
    RunSetup(d.Only);
  }

  // ------------------------------------------------------------------ checks (as setup.ps1 does them)

  static string Run(string exe, string args) {
    try {
      var psi = new ProcessStartInfo(exe, args) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
      using (var p = Process.Start(psi)) {
        string output = p.StandardOutput.ReadToEnd();
        if (!p.WaitForExit(30000)) { try { p.Kill(); } catch (Exception) { } return null; }
        return p.ExitCode == 0 ? output.Trim() : null;
      }
    } catch (Exception) { return null; }
  }

  static KeyValuePair<bool, string> FindPython() {
    const string probe = "-c \"import sys, tkinter; assert sys.version_info >= (3, 11); print('%d.%d' % sys.version_info[:2])\"";
    foreach (var c in new[] { new[] { "py", "-3 " }, new[] { "python", "-B " } }) {
      string v = Run(c[0], c[1] + probe);
      if (!string.IsNullOrEmpty(v)) return new KeyValuePair<bool, string>(true, "Python " + v);
    }
    string local = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "Python", "Python312", "python.exe");
    if (File.Exists(local)) return new KeyValuePair<bool, string>(true, "Python 3.12");
    return new KeyValuePair<bool, string>(false, "");
  }

  static KeyValuePair<bool, string> FindBuildTools() {
    string x86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
    string vswhere = Path.Combine(x86, "Microsoft Visual Studio", "Installer", "vswhere.exe");
    bool sdk = Directory.Exists(Path.Combine(x86, "Windows Kits", "10", "Include"));
    string vs = File.Exists(vswhere)
        ? Run(vswhere, "-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property displayName") : null;
    if (!string.IsNullOrEmpty(vs) && sdk) return new KeyValuePair<bool, string>(true, vs.Split('\n')[0].Trim());
    if (!string.IsNullOrEmpty(vs)) return new KeyValuePair<bool, string>(false, L.T("falta o Windows SDK", "Windows SDK missing"));
    return new KeyValuePair<bool, string>(false, "");
  }

  // A complete Clang 20+ already installed is used instead of the download (setup_tools.py).
  static string SystemLlvm() {
    string bin = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "LLVM", "bin");
    string[] needed = { "clang.exe", "clang++.exe", "clang-cl.exe", "lld-link.exe", "llvm-ar.exe", "llvm-rc.exe" };
    if (!needed.All(n => File.Exists(Path.Combine(bin, n)))) return null;
    string v = Run(Path.Combine(bin, "clang++.exe"), "--version");
    var m = v == null ? null : System.Text.RegularExpressions.Regex.Match(v, @"clang version (\d+)");
    return m != null && m.Success && int.Parse(m.Groups[1].Value) >= 20 ? "Clang " + m.Groups[1].Value + L.T(" do sistema", " (system)") : null;
  }

  KeyValuePair<bool, string> GameFilesOk(string kit) {
    if (kit == null || cfg.GameFiles.Count == 0) return new KeyValuePair<bool, string>(false, "");
    foreach (var kv in cfg.GameFiles) {
      string path = Path.Combine(kit, kv.Key);
      if (!File.Exists(path)) return new KeyValuePair<bool, string>(false, "");
      using (var sha = SHA256.Create())
      using (var f = File.OpenRead(path)) {
        string hash = BitConverter.ToString(sha.ComputeHash(f)).Replace("-", "");
        if (!string.Equals(hash, kv.Value, StringComparison.OrdinalIgnoreCase))
          return new KeyValuePair<bool, string>(false, L.T("versão diferente", "different version"));
      }
    }
    return new KeyValuePair<bool, string>(true, L.T("versão conferida", "version verified"));
  }
}

}
