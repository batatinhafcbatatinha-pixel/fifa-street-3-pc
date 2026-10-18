# FIFA Street 3 — Native PC Port | No Emulated GPU or Audio Hardware

> [!WARNING]
> This repository does **not** include the game, an ISO/XEX, EA assets, generated game code or a precompiled executable.
> You need your own Xbox 360 USA disc. See [the full notice](#notice) at the end.

An experimental Xbox 360 FIFA Street 3 port built through static recompilation with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), with its own Direct3D 11 renderer and its own XMA audio pipeline. **This public repository is a source and local-build kit, not a ready-to-play game download.** The game's PowerPC code is translated locally from your own verified disc; no game executable or generated game code is distributed here.

## Status

- Tested on Windows 11 x64: boot, menus, profile/save, and a full match at 55–60 fps.
- Graphics: the game's Direct3D library is intercepted at the API level and drawn by a native Direct3D 11 renderer. Xenos shader microcode is translated to HLSL. No GPU command processor, register file or emulation plugin is used.
- Audio: the XMA and XAudio exports are reimplemented on the host (FFmpeg's XMA decoder plus a native mixer). No audio hardware model. A loopback capture through menus and a match measured continuous stereo output with no clipping, dropouts or decode failures; it has not yet been reviewed by ear.
- The Xbox 360 kernel and XAM services (files, threads, memory, profiles, input) come from the ReXGlue runtime as an operating-system compatibility layer.
- Windows x64 only for now. Online features are not supported.

## Why a native port instead of an emulator

An emulator such as [Xenia](https://xenia.jp) recreates the Xbox 360: it JIT-translates the PowerPC code at run time and emulates the Xenos GPU by processing the console's command buffers, and the audio hardware by modelling its XMA contexts. That is the right approach for the whole library. This port takes a different route for one game:

- **Ahead-of-time code.** The game's PowerPC code becomes C++ compiled by a modern optimizing compiler before you play: no JIT, no warm-up.
- **No GPU emulation in the hot path.** Draw calls go from the game's D3D library straight to Direct3D 11, instead of being turned into command packets and register writes that are then emulated again.
- **Native audio.** XMA streams are decoded and mixed directly on the PC.
- **Readable and fixable.** Every hook, renderer path and audio routine is plain C++ in `project/src`.

The trade-off: it covers only this game and revision, and it is experimental. An emulator is the more general and better-tested option.

## Your game files

Only the **Xbox 360 USA** revision is accepted (`default.xex` SHA-256 below). Extract your own disc image into a new folder:

```sh
python tools/extract_iso.py "FIFA Street 3 (USA).iso" --output game_files
python tools/validate_game.py game_files
```

```text
default.xex  1b4cd0fd2973fdea5f4f8b4907c6db67fb061c4173193c22144dd027b0a5d792
```

The validator checks the hash and that the `data` folder is present. Do not upload the ISO, extracted files, generated code or the built executable.

## Build on Windows

Requirements: Python 3.10+, Visual Studio 2022 Build Tools (C++ workload, Windows SDK), LLVM/Clang 20 or newer (a complete install that includes the clang resource headers), CMake 3.25+ and Ninja.

```powershell
python tools/build_local.py --game-files game_files --llvm-dir "C:\Program Files\LLVM"
python tools/play.py --game-files game_files
```

`build_local.py`:

1. validates your files;
2. downloads the pinned ReXGlue SDK nightly (`0.10.0.24-dev.gbd833a2`, SHA-256 checked) and applies the small FPSCR patch in `patches/`;
3. downloads the public FFmpeg headers matching the SDK's prebuilt `libavcodec` (LGPL-2.1);
4. translates your `default.xex` into private C++ under `.local/`;
5. compiles the port and removes any GPU emulation plugin from the output folder.

Everything stays in `.local/`, which is excluded from version control. `--reuse-generated` skips the translation on later builds; `--sdk-dir` uses an SDK you already have.

## Controls and settings

Xbox controllers work as on the console. **F2** opens the PC settings (window size, fullscreen, FPS counter); **Alt+Enter** toggles fullscreen. Runtime options of the SDK can be passed after `play.py`, for example `python tools/play.py --game-files game_files --fullscreen=true`.

## Credits and licenses

- Port integration, native renderer and audio: [batatinhafcbatatinha-pixel](https://github.com/batatinhafcbatatinha-pixel).
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk): Tom Clay and contributors, BSD-3-Clause; derived from [Xenia](https://github.com/xenia-project/xenia) (Ben Vanik and contributors). External dependency, downloaded by the build script; license in `licenses/`. Shader-microcode semantics and XMA field layouts follow the SDK's Xenia-derived headers.
- [FFmpeg](https://ffmpeg.org) (Xenia fork by wmarti), LGPL-2.1: the SDK's prebuilt `libavcodec`/`libavutil` and their public headers, downloaded at build time.
- Microsoft Visual C++ runtime, Windows SDK and Direct3D: Microsoft; obtained separately.
- FIFA Street 3, its game code, trademarks and assets belong to Electronic Arts and the respective rights holders. This project is not affiliated with or endorsed by EA or Microsoft.

See [NOTICE.md](NOTICE.md) for the publication boundary and [LICENSE](LICENSE) for the license of the port's own code.

---

<a id="notice"></a>

**⚠️ THIS PROJECT DOES NOT INCLUDE THE GAME, AN ISO/XEX, EA GAME ASSETS, GENERATED GAME CODE OR A PRECOMPILED GAME EXECUTABLE. YOU MUST PROVIDE YOUR OWN LAWFULLY OBTAINED XBOX 360 USA GAME FILES AND HAVE THE RIGHTS REQUIRED FOR YOUR USE. THIS PROJECT DOES NOT AUTHORIZE COPYRIGHT INFRINGEMENT. ALL GAME RIGHTS BELONG TO THEIR RESPECTIVE OWNERS. ⚠️**
