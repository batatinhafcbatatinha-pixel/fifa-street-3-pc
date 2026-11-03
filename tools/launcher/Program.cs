using System;
using System.IO;
using System.Net;
using System.Windows.Forms;

namespace PortLauncher {

static class Program {
  [STAThread]
  static void Main(string[] args) {
    ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
    Application.EnableVisualStyles();
    Application.SetCompatibleTextRenderingDefault(false);
    string dir = AppDomain.CurrentDomain.BaseDirectory;
    var config = GameConfig.Load(dir);
    if (config == null) {
      MessageBox.Show(L.T("Este launcher não tem a configuração do jogo (launcher.json).",
                          "This launcher has no game configuration (launcher.json)."), "Launcher",
                      MessageBoxButtons.OK, MessageBoxIcon.Error);
      return;
    }
    var form = new MainForm(config, dir);
    // Test aid: --shot <file.png> [tab index] draws the window into a PNG and exits (no clicks needed).
    int shot = Array.IndexOf(args, "--shot");
    if (shot >= 0 && shot + 1 < args.Length)
      form.ShotOnLoad(args[shot + 1], shot + 2 < args.Length && int.TryParse(args[shot + 2], out int tab) ? tab : 0);
    Application.Run(form);
  }
}

}
