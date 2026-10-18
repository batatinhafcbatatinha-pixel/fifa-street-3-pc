// fifastreet3 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <filesystem>

#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/rex_app.h>

#include "script_input.h"
#include "pc_options.h"
#include "native/audio_system.h"
#include "native/gfx_system.h"

namespace fs3_performance { double FramesPerSecond(); }

class Fifastreet3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Fifastreet3App>(new Fifastreet3App(ctx, "fifastreet3",
        PPCImageConfig));
  }

 protected:
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    pc_options_ = std::make_unique<PcOptionsHost>(window(), drawer, user_data_root(),
        [] { return fs3_performance::FramesPerSecond(); });
  }

  void OnShutdown() override { pc_options_.reset(); }

  // Game files: --game_data_root wins; otherwise <exe>/assets (SDK default),
  // <exe>/game, then the extracted disc on D:.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    auto has_xex = [](const std::filesystem::path& p) {
      std::error_code ec;
      return !p.empty() && std::filesystem::is_regular_file(p / "default.xex", ec);
    };
    if (has_xex(paths.game_data_root)) return;
    auto exe_dir = rex::filesystem::GetExecutableFolder();
    for (auto candidate : {exe_dir / "game", std::filesystem::path("D:/FIFA-Street3-game")}) {
      if (has_xex(candidate)) {
        paths.game_data_root = candidate;
        return;
      }
    }
  }

  // Native renderer: the game's Direct3D library is intercepted at the API level
  // (native/native_hooks.cpp); no GPU emulation plugin is loaded. Same for audio
  // (native/audio_exports.cpp): no APU model.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    config.gpu_plugin.clear();
    config.graphics = std::make_unique<fs3gfx::NativeGraphicsSystem>();
    // Native audio: XMA decoded by fs3audio::XmaEngine at the API level, own render driver.
    config.audio_factory = [](rex::runtime::FunctionDispatcher* dispatcher)
        -> std::unique_ptr<rex::system::IAudioSystem> {
      return std::make_unique<fs3audio::NativeAudioSystem>(dispatcher);
    };
    // FS3_INPUT=<file>: add a scripted pad (see script_input.h) next to the real ones.
    if (const char* script = std::getenv("FS3_INPUT"); script && *script) {
      config.input_factory = [path = std::string(script)](bool tool_mode)
          -> std::unique_ptr<rex::system::IInputSystem> {
        auto input = rex::input::CreateDefaultInputSystem(tool_mode);
        auto driver = std::make_unique<ScriptInputDriver>(path);
        driver->Setup();
        input->AddDriver(std::move(driver));
        return input;
      };
    }
  }

 private:
  std::unique_ptr<PcOptionsHost> pc_options_;
};
