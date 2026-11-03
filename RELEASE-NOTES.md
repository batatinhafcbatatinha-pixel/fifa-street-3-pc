Source kit and `Setup.exe` (our installer and launcher): no game, no XEX, no generated code, no game executable. Bring your own Xbox 360 USA disc.

**v0.2.5**

- **`Setup.exe`**: download it alone and run it. It fetches this source kit, shows on its *Dependencies* tab what your PC has and what is missing (Python, C++ Build Tools, LLVM/CMake/Ninja, your game files, the built game) with the space each part needs, installs each part or everything, and builds the game. Afterwards it is the launcher: *Play*, and *PC settings* with the same options as the F1 menu, saved before the game starts. It is built by GitHub from `tools/launcher`; nothing from the game is in it. Windows may warn because it is not signed.
- `Setup.cmd` still works as before; `tools/setup.ps1` gained `-Only python|buildtools|tools|game`.

`Setup.exe` was tested with a stand-in kit (install steps, Play, settings file) and with the real kit download up to the build tools; a full build through it and the game started from it have not been tried yet.

**v0.2.4**

- **One-click `Setup.cmd`:** asks before installing anything, installs Python and Microsoft's C++ Build Tools through winget if missing, downloads pinned, SHA-256-checked LLVM/Clang, CMake and Ninja into `.local/tools` only, fetches missing parts, opens the game-file installer and builds the port; `Play-<game>.cmd` starts it. Run it again after a failure: finished steps are skipped.
- **README:** a *How to play* guide for people new to this, and the same section layout as the other ports.
- The source-kit ZIP now includes the game-file installer (`tools/port-installer`); `Install-Files.cmd` did not work from the ZIP before.

`Setup.cmd` was run from a fresh copy of this kit on Windows 11 up to the built executable (about 15 minutes with the tools already installed); the game itself was not started in that test.

**v0.2.3**

- **Controller vibration** on/off in the F1 PC settings.
- The game-data installer launcher is now `Install-Files.cmd` (English name).
- The shared game-data installer (`tools/port-installer`) ships in the repository.

Vibration has been compiled but not felt on a real controller yet. Nobody has played this exact build through.

**v0.2.2**

- Measured in a match at 4K, 8x MSAA, 16x anisotropic filtering, no frame limit: about 148 fps, minimum 120 (Ryzen 5 3600, RTX 3050 Laptop).
- Native render-list sort comparator and memcpy; converted index buffers cached while their pages are untouched; audio clock on a fixed schedule.
- PC settings menu reads every SDL pad; controller mappings sit next to the executable.
- The game keeps running when the audio device stops consuming.
- Experimental macOS build files (not compiled or run yet) and a native Linux build path (not built for this release).

Nobody has played this exact build through.

**Earlier**

- **Keyboard play:** the keyboard plays as an Xbox controller with EA SPORTS FC 27's default WASD layout (WASD move, L pass, ; shoot, K lob/cross, O through ball, P sprint, I change player, Esc pause; Enter passes PRESS START).
- Native Direct3D 11 renderer and native XMA audio; no GPU or audio-hardware emulation.
- `tools/extract_iso.py` + `tools/validate_game.py`: extract your disc and verify the supported revision by SHA-256.
- `tools/build_local.py`: downloads the pinned ReXGlue SDK, translates your XEX locally and builds the port.
- Tested on Windows 11 x64: boot, menus, saves and full matches. Audio measured clean (no clipping, dropouts or decode failures) but not yet reviewed by ear.
