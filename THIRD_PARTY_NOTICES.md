# Third-party notices

Animal Crossing VR is built from other people's work. This is what's in this repository, what ends
up in the Apple Vision Pro app you build, where each part comes from, and the license it keeps.
[LICENSE](LICENSE) has the license texts for the code this repository is made of.

## The game's code

| Component | What it is here | License |
|---|---|---|
| [ac-decomp](https://github.com/ACreTeam/ac-decomp) by ACreTeam | The decompiled game: `src/`, `include/`, `config/`, `tools/` (with the PC port's and the VR mod's hooks in the game's files). | CC0 1.0 |
| [ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port) by flyngmt | The native PC port: the `pc/` layer (GX on OpenGL, saves, input, settings) and its `TARGET_PC` changes to the game's files. | MIT ([LICENSE](LICENSE)) |
| [ACGC-PC-Port fork](https://github.com/birabittoh/ACGC-PC-Port) by birabittoh | The European message loader (`pc/src/pc_msg_eur*`) and the translation tools. | CC0 1.0 and MIT, as upstream |
| [ACGC-Android-Port](https://github.com/zwaetschge/ACGC-Android-Port) by zwaetschge | The 64-bit (ARM64) work this port starts from, and the Android app in `app/`. | MIT ([LICENSE](LICENSE)) |
| [animal-crossing-vr](https://github.com/LiquidAzir/animal-crossing-vr) by LiquidAzir | The VR mod: `pc/src/pc_vr.cpp`, first person (`pc/src/pc_fp_camera.c`), the tools on your hand, the backs of buildings, the whole-town view, and their hooks in the game's files. | MIT ([LICENSE](LICENSE)) |
| [FixNES](https://github.com/FIX94/fixNES) by FIX94 | The in-game NES (`pc/lib/fixnes`). | MIT ([`pc/lib/fixnes/LICENSE`](pc/lib/fixnes/LICENSE)) |
| [glad](https://github.com/Dav1dde/glad) 2.0.8 | The generated OpenGL loader (`pc/lib/glad`), from the Khronos OpenGL registry. | (WTFPL or CC0 1.0) and Apache 2.0, as its header says |

## The Apple Vision Pro port

| Component | What it is here | License |
|---|---|---|
| Animal Crossing VR by [Trevorbilt](https://trevorbilt.com) | The visionOS app (`visionos/App`), the host that runs the game (`visionos/host`), the OpenGL-on-Metal layer and the Window view's capture in `pc/`, the build scripts, and the port's other changes. | MIT ([LICENSE](LICENSE)) |
| visionOS OpenXR provider, from [WiiCompiled Vision](https://github.com/iChris4/Wiicompiled_VR) by iChris4, by way of [Twilight Princess VR](https://github.com/edgytoast/tpvr-visionos) | `visionos/openxr-provider`: OpenXR on CompositorServices and ARKit, the runtime the immersive spaces run on. Its changes are listed in [its README](visionos/openxr-provider/README.md). | GPL-3.0-or-later ([`visionos/openxr-provider/LICENSE`](visionos/openxr-provider/LICENSE)) |
| [SMAA](https://github.com/iryoku/smaa) by Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez | The SMAA anti-aliasing option, in the provider (`visionos/openxr-provider/src/smaa`). | MIT ([`smaa/LICENSE.txt`](visionos/openxr-provider/src/smaa/LICENSE.txt)) |
| TrevorbiltKit by Trevorbilt | The launcher every Trevorbilt port shares, vendored in `visionos/TrevorbiltKit` from [SHAR VR](https://github.com/edgytoast/shar-visionos). | MIT ([`visionos/TrevorbiltKit/LICENSE`](visionos/TrevorbiltKit/LICENSE)), except its brand assets ([notice](visionos/TrevorbiltKit/Sources/TrevorbiltKit/Resources/Brand/NOTICE.md)) |
| [Space Mono](https://github.com/googlefonts/spacemono) and [Roboto](https://github.com/googlefonts/roboto-classic) | The launcher's fonts, in `visionos/TrevorbiltKit/Sources/TrevorbiltKit/Resources/Fonts/`. | SIL Open Font License 1.1 (the `OFL-*.txt` files beside them) |

The app links the provider, so **the Apple Vision Pro app you build is GPL-3.0**. Its source is this
repository; the About tab links the commit it was built from.

## Downloaded by the Apple Vision Pro build

| Component | What it does here | License |
|---|---|---|
| [SDL](https://www.libsdl.org) 2.32.10, by Sam Lantinga and contributors | Audio, timers, events and gamepads. `visionos/scripts/build-sdl2.sh` clones its `release-2.32.10` tag, applies `visionos/patches/sdl2/0001-visionos.patch` and builds it static, into `visionos/third_party/` (not committed). | zlib ([`pc/licenses/LICENSE-SDL2.txt`](pc/licenses/LICENSE-SDL2.txt)) |
| [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) 1.1.61 headers, by the Khronos Group | The OpenXR API the VR mod and the provider speak. CMake downloads the release archive; only its headers are used. | Apache 2.0 |

## From Apple's SDK

Metal, CompositorServices, ARKit, RealityKit, GameController, SwiftUI and zlib are the system's own.

## The Android app (`app/`), which the Apple Vision Pro build doesn't use

| Component | What it does there | License |
|---|---|---|
| pyjkernel | The archive reader and writer the translation tools use (`app/src/main/python/acport/l10n/pyjkernel`, `tools/pyjkernel`). Bundled in the Android app, so Android builds you share follow the GPL. | GPL-3.0 (its `LICENSE`) |
| SDL 2.30.12 | The Android app's platform layer: downloaded by its build; its Java glue is in `app/src/main/java/org/libsdl/app` (changed: the on-screen keyboard without suggestions). | zlib |
| [ARM astc-encoder](https://github.com/ARM-software/astc-encoder) 5.7.0 | HD texture conversion; downloaded by its build. | Apache 2.0 |
| [bcdec](https://github.com/iOrange/bcdec) | BC1, BC3 and BC7 decoding; downloaded by its build. | MIT or Unlicense |
| [Chaquopy](https://chaquo.com/chaquopy/) 16.1 | The Gradle plugin and Python 3.11 runtime in the Android app. | MIT |
| [jcifs-ng](https://github.com/AgNO3/jcifs-ng) 2.1.10 | Importing from network shares (a Maven dependency). | LGPL 2.1 |
| Gradle wrapper | Not committed (compiled code): `gradle wrapper` restores it ([docs/README-ANDROID-PORT.md](docs/README-ANDROID-PORT.md)). | Apache 2.0 |
| Animal Crossing HD Texture Pack, by its authors | **Not in this repository.** The Android app downloads it from its Dolphin forum thread when you ask it to. | Its authors' |

## The game

*Animal Crossing* was made by Nintendo and released for the GameCube in 2002 (in Japan in 2001, as
*Dōbutsu no Mori+*). The game, its characters and its art belong to Nintendo, including what shows in the
screenshots in `docs/images/`. None of the game's files are in this repository or produced by its
build: you bring your own disc.
