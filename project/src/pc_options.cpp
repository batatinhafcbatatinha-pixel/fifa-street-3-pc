#include "pc_options.h"

#include <algorithm>
#include <fstream>
#include <string>
#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#ifdef _WIN32
#include <windows.h>
#endif

namespace {
constexpr int kSizes[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
constexpr const char* kSizeNames[] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440"};
}

class PcFpsDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit PcFpsDialog(PcOptionsHost& host) : ImGuiDialog(host.drawer_), host_(host) {}
 protected:
  void OnDraw(ImGuiIO&) override {
    if (!host_.show_fps_ || !host_.fps_provider_) return;
    ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(.7f);
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::Begin("##street3-game-fps", nullptr, flags)) {
      const double fps = host_.fps_provider_();
      if (fps > 0.0) ImGui::Text("Jogo: %.1f FPS", fps);
      else ImGui::TextUnformatted("Jogo: -- FPS");
    }
    ImGui::End();
  }
 private:
  PcOptionsHost& host_;
};

class PcOptionsDialog final : public rex::ui::ImGuiDialog {
 public:
  explicit PcOptionsDialog(PcOptionsHost& host) : ImGuiDialog(host.drawer_), host_(host) {}
  ~PcOptionsDialog() override { host_.dialog_ = nullptr; }
  void RequestClose() { Close(); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(.035f, .055f, .08f, .97f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.12f, .24f, .28f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.23f, .45f, .43f, 1));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, ImVec4(.84f, 1, .13f, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(40, 30));
    constexpr auto flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                           ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("PC settings / FIFA Street 3", nullptr, flags)) {
      ImGui::TextColored(ImVec4(.84f, 1, .13f, 1), "FIFA STREET 3  /  CONFIGURACOES DO PC");
      ImGui::TextDisabled("F2 abre/fecha  |  O jogo continua em execucao");
      ImGui::Separator();
      if (ImGui::BeginTabBar("settings-tabs")) {
        if (ImGui::BeginTabItem("Tela")) {
          bool fullscreen = host_.window_->IsFullscreen();
          if (ImGui::Checkbox("Tela cheia", &fullscreen)) {
            host_.fullscreen_ = fullscreen;
            host_.Apply();
          }
          ImGui::BeginDisabled(fullscreen);
          if (ImGui::Combo("Tamanho da janela", &host_.window_size_, kSizeNames, 4)) host_.Apply();
          ImGui::EndDisabled();
          ImGui::TextWrapped("O tamanho da janela altera a apresentacao. Nao altera a resolucao interna do jogo.");
          if (host_.fps_provider_) ImGui::Checkbox("Mostrar contador de FPS", &host_.show_fps_);
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controles")) {
          ImGui::TextUnformatted("F2: configuracoes do PC\nAlt+Enter: alternar tela cheia\nAlt+F4: fechar\nMouse: selecionar as opcoes deste menu");
          ImGui::Spacing();
          ImGui::TextWrapped("Controle: use o controle conectado para jogar. As configuracoes do PC usam teclado e mouse.");
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Configuracao")) {
          ImGui::TextWrapped("As preferencias de tela sao salvas na pasta de dados do usuario.");
          if (ImGui::Button("Salvar preferencias")) host_.Save();
          ImGui::SameLine();
          if (ImGui::Button("Recarregar preferencias")) { host_.Load(); host_.Apply(); }
          if (ImGui::Button("Restaurar padrao")) {
            host_.fullscreen_ = false;
            host_.window_size_ = 0;
            host_.show_fps_ = false;
            host_.Apply();
          }
          if (host_.save_failed_) ImGui::TextWrapped("Nao foi possivel salvar. Verifique o acesso a pasta de dados do usuario.");
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }
      ImGui::Spacing();
      if (ImGui::Button("Salvar e voltar ao jogo", ImVec2(260, 42))) {
        if (host_.Save()) Close();
      }
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
  }

 private:
  PcOptionsHost& host_;
};

PcOptionsHost::PcOptionsHost(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer,
                           const std::filesystem::path& user_data_root,
                           FpsProvider fps_provider)
    : window_(window), drawer_(drawer), config_path_(user_data_root / "street3-pc-options.ini"),
      fps_provider_(std::move(fps_provider)) {
  Load();
  Apply();
  if (fps_provider_) fps_dialog_ = new PcFpsDialog(*this);
  window_->AddInputListener(this, 10000);
}

PcOptionsHost::~PcOptionsHost() {
  window_->RemoveInputListener(this);
  delete dialog_;
  delete fps_dialog_;
}

void PcOptionsHost::Toggle() {
  if (dialog_) { Save(); dialog_->RequestClose(); }
  else { fullscreen_ = window_->IsFullscreen(); dialog_ = new PcOptionsDialog(*this); }
}

void PcOptionsHost::OnKeyDown(rex::ui::KeyEvent& e) {
  if (e.virtual_key() == rex::ui::VirtualKey::kF2) {
    if (!e.prev_state()) Toggle();
    e.set_handled(true);
  }
}

void PcOptionsHost::OnKeyUp(rex::ui::KeyEvent& e) {
  if (e.virtual_key() == rex::ui::VirtualKey::kF2) e.set_handled(true);
}

void PcOptionsHost::Apply() {
  window_size_ = std::clamp(window_size_, 0, 3);
  window_->SetDesiredLogicalSize(kSizes[window_size_][0], kSizes[window_size_][1]);
  window_->SetFullscreen(fullscreen_);
}

bool PcOptionsHost::Save() {
  fullscreen_ = window_->IsFullscreen();
  std::error_code ec;
  std::filesystem::create_directories(config_path_.parent_path(), ec);
  if (ec) { save_failed_ = true; return false; }
  auto temporary = config_path_;
  temporary += ".tmp";
  std::ofstream out(temporary, std::ios::trunc);
  out << "fullscreen " << fullscreen_ << "\nwindow_size " << window_size_
      << "\nshow_fps " << show_fps_ << '\n';
  out.close();
  save_failed_ = !out;
  if (save_failed_) return false;
#ifdef _WIN32
  save_failed_ = !MoveFileExW(temporary.c_str(), config_path_.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
  std::filesystem::rename(temporary, config_path_, ec);
  save_failed_ = bool(ec);
#endif
  return !save_failed_;
}

void PcOptionsHost::Load() {
  fullscreen_ = false;
  window_size_ = 0;
  show_fps_ = false;
  save_failed_ = false;
  std::ifstream in(config_path_);
  std::string key;
  int value;
  while (in >> key >> value) {
    if (key == "fullscreen" && (value == 0 || value == 1)) fullscreen_ = value != 0;
    if (key == "window_size" && value >= 0 && value < 4) window_size_ = value;
    if (key == "show_fps" && (value == 0 || value == 1)) show_fps_ = value != 0;
  }
}
