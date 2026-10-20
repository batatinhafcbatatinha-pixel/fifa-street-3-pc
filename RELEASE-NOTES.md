Source kit only: no game, no XEX, no generated code and no executable. Bring your own Xbox 360 USA disc.

- **Keyboard play:** the keyboard plays as an Xbox controller with EA SPORTS FC 27's default WASD layout (WASD move, L pass, ; shoot, K lob/cross, O through ball, P sprint, I change player, Esc pause; Enter passes PRESS START).
- Native Direct3D 11 renderer and native XMA audio; no GPU or audio-hardware emulation.
- `tools/extract_iso.py` + `tools/validate_game.py`: extract your disc and verify the supported revision by SHA-256.
- `tools/build_local.py`: downloads the pinned ReXGlue SDK, translates your XEX locally and builds the port.
- Tested on Windows 11 x64: boot, menus, saves and full matches at 55-60 fps. Audio measured clean (no clipping, dropouts or decode failures) but not yet reviewed by ear.
