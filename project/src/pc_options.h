#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <rex/ui/window_listener.h>

namespace rex::ui { class Window; class ImGuiDrawer; }

// Own on the UI thread; destroy before the SDK window and ImGui drawer.
class PcOptionsHost final : public rex::ui::WindowInputListener {
 public:
  using FpsProvider = std::function<double()>;
  PcOptionsHost(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer,
                const std::filesystem::path& user_data_root,
                FpsProvider fps_provider = {});
  ~PcOptionsHost() override;
  void Toggle();
  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;

 private:
  friend class PcOptionsDialog;
  friend class PcFpsDialog;
  void Apply();
  bool Save();
  void Load();
  rex::ui::Window* window_;
  rex::ui::ImGuiDrawer* drawer_;
  class PcOptionsDialog* dialog_ = nullptr;
  class PcFpsDialog* fps_dialog_ = nullptr;
  std::filesystem::path config_path_;
  FpsProvider fps_provider_;
  bool fullscreen_ = false;
  bool show_fps_ = false;
  int window_size_ = 0;
  bool save_failed_ = false;
};
