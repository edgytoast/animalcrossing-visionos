# Working on Animal Crossing VR

How the port fits together, how to build and test it without a headset, and the tools that check
it. Installing on a headset is in [AVP-INSTALL.md](../AVP-INSTALL.md).

## How it works

- **The game** is ACreTeam's decompilation with flyngmt's PC port layer (`pc/`) and zwaetschge's
  64-bit work, compiled with Apple Clang for visionOS into `ACVRVisionGame.framework`
  (`pc/cmake/visionos_framework.cmake`). The game is linked into one relocatable object first, so
  its own `operator new` stays its own, and the framework exports only the bridge
  (`visionos/exports.txt`). The SwiftUI app (`visionos/App`) runs it on its own thread through the
  host (`visionos/host/game_host.mm`).
- **Rendering:** the PC port's GX-to-OpenGL renderer (`pc/src/pc_gx.c`) draws through a small
  OpenGL 3.3-on-Metal layer written for this port (`pc/src/pc_gl_metal.mm`).
- **Full and Progressive** (stereo): LiquidAzir's VR mod (`pc/src/pc_vr.cpp`) applies the head pose
  to the game's view, routes the 2D screens to a panel, widens the game's culling, keeps the whole
  town loaded and fills in the backs of buildings. First person is the default, at life size (your
  eyes at the character's). When the game takes over the camera (the train ride, events), your
  eyes stay where the shot put them and your head looks around; they move on when the camera cuts
  or has gone 1.5 m or 40 degrees from where you are.
- **Single-pass stereo:** one image with both eyes side by side. The display list is read once and
  each world draw is drawn instanced, instance 1 with the right eye's matrices, placed and clipped
  into its half (`pc/include/pc_metal_stereo.h`); the sky is drawn once per eye. That halves the
  draws of a pass per eye (`ACVR_STEREO_PASSES=2`, the mod's own way). The progressive space draws a
  pass per eye for now: the provider's layered path showed nothing for one side-by-side image in
  the Simulator.
- **The OpenXR provider** (`visionos/openxr-provider`, GPL-3.0-or-later) is OpenXR on
  CompositorServices and ARKit, from WiiCompiled Vision by way of Twilight Princess VR. It presents
  PS VR2 Sense controllers and your bare hands (pinches) as an OpenXR Touch controller, and does
  the SMAA and FXAA.
- **The Window view** runs the game flat, with its own camera (`--no-vr`), paced by the window.
  `pc/src/pc_mirror.cpp` captures every perspective draw until the HUD begins: vertices in the
  camera's view space, the lighting and the TEV stages folded into "texture × multiply + add" per
  vertex, the blend, cut-out, depth, culling and wrap state, decals and a sky dome; textures are
  read back from the Metal layer and named by their pixels. `visionos/host/scene_mirror.cpp`
  finishes each frame on a worker thread, and `visionos/App/Sources/MirrorScene.swift` rebuilds it
  in RealityKit behind a portal. The host also snapshots the frame at the HUD and at its end
  (`visionos/host/window_frames.mm`), and the window lays the HUD, fades and letterbox bars on its
  glass. The window's scale holds through a shot (it changes at cuts and with the camera's own
  moves), and depth is never squashed: Animal Crossing's camera is narrow, and the picture is exact
  from about 1.6 window widths away.
- **Input:** Sense controllers and bare hands through the provider in Full and Progressive;
  gamepads through SDL 2; in the Window view the two Sense halves play as one gamepad
  (`visionos/host/sense_pad.mm`).
- **Lifecycle:** the Digital Crown closes the space, the game holds until **Resume** hands it a new
  layer, then opens its pause menu. With the headset off or visionOS on top, play holds.
- **Saves** are the game's own memory card files, in the app's `Documents/save/card_a`.

## Build for the Simulator

```bash
visionos/scripts/build-visionos.sh --simulator
```

It builds SDL 2 once (into `visionos/third_party/`), then the game framework, then the app,
unsigned, and prints where the app is (`.scratch/DerivedData-simulator/Products/...`).
`--game-only` rebuilds just the framework: copy
`.scratch/build-xrsimulator-RelWithDebInfo/ACVRVisionGame.framework/ACVRVisionGame` over the one in
the built app's `Frameworks`, then install again, in minutes instead of a full app build. Don't run
two builds in one checkout at once (both generate the Xcode project).

Install it and give it your disc:

```bash
xcrun simctl list devices available | grep -i vision    # pick an Apple Vision Pro simulator's UDID
xcrun simctl install <UDID> <path to ACVRVision.app>
xcrun simctl launch <UDID> dev.acvr.vision.simulator     # once, so its folders exist
cp "<your disc>.iso" "$(xcrun simctl get_app_container <UDID> dev.acvr.vision.simulator data)/Documents/"
```

Then launch it from the Simulator, or with test hooks: `xcrun simctl launch` passes each
`SIMCTL_CHILD_<NAME>=<value>` in its environment to the app as `<NAME>`.

## The macOS bench

The same game, with the Metal layer, as a Mac command-line program: fast to build, and it runs
headless with scripted input, for checking the game, the stereo and the Window view's capture
without a Simulator.

```bash
brew install sdl2
cmake -S pc -B build-macos-metal -G Ninja -DPC_METAL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-macos-metal
```

Put your disc in `build-macos-metal/bin/rom/` as `GAFE01.iso` and run it from
`build-macos-metal/bin` (its `shaders/`, settings and saves are relative to the folder it runs in):

```bash
cd build-macos-metal/bin
ACVR_HEADLESS=1 ACVR_TEST_ACTIONS='A@8~0.1*155/0.4' ACVR_TEST_SHOTS='20-60/5' ACVR_TEST_QUIT=62 ./AnimalCrossing -v
```

`ACVR_FAKE_VR=1 ... --vr` renders both eyes side by side, from a still head
(`ACVR_FAKE_VR_YAW`, `ACVR_FAKE_VR_PITCH` turn it). A new game to its first save, about three
minutes with `skip_first_job = 1` in `settings.ini`:

```
A@8~0.1*155/0.4,TEXT=Trev@10,TEXT=Bench@10,STICK=0,-100@70~4,A@75~0.1*52/0.4,SPEED=0@97~3,HOUSE@100~20,STICK=0,-100@124~2.5,SPEED=1@128~1,A@128~0.1*100/0.4,GYROID@168~50
```

## Test hooks

Environment variables, read by the game (`pc/src/pc_test_hooks.c`, `pc/src/pc_vr.cpp`,
`pc/src/pc_mirror.cpp`), the host and the app. Unset, each does nothing.

| Variable | Does |
|---|---|
| `ACVR_AUTO_PLAY=1`, `ACVR_IMMERSION=full\|progressive\|window` | Presses the launcher's Play, in that view |
| `ACVR_ARGS` | Extra arguments for the game, such as `--time 14:00:00`, `--date 10/10/2026` or `--rain` |
| `ACVR_TEST_ACTIONS` | Scripted pad input: `NAME[=x,y]@start[~duration][*count/period]`, comma separated. Buttons, `STICK`, `CSTICK`, `TEXT=word`, `SPEED=1\|0`, and `HOUSE`, `GYROID` and `GIVE=n` (1 net, 2 axe, 3 shovel, 4 rod), which walk a new game to a save and put a tool in your pockets |
| `ACVR_TEST_SHOTS`, `ACVR_TEST_SHOT_DIR`, `ACVR_TEST_QUIT` | Saves the frame at those seconds (`10,20` or `100-200/5`), and quits |
| `ACVR_TEST_SPEEDHACK=1` | The game's own speed-up, for long scripted runs |
| `ACVR_TEST_CAMLOG=1` | Logs the game camera, the window's scale and the VR anchor every frame |
| `ACVR_TEST_XR_ACTIONS` | Scripted Sense controllers (the provider's syntax) |
| `ACVR_TEST_HAND=hold\|swing` | A scripted tool hand |
| `ACVR_TEST_CLOSE_SPACE=<s>`, `ACVR_TEST_PAUSED=resume@<s>` | The Digital Crown pause, and Resume |
| `ACVR_SNAP_TURN=<degrees>` | Turning, overriding the launcher's |
| `ACVR_STEREO_PASSES=2` | A pass per eye instead of single-pass stereo |
| `ACVR_VR_FOLLOW_CAMERA=1` | The game's scripted cameras turn and move you again, for comparison |
| `ACVR_TEST_RIGHT_EYE=1` | The Simulator, which shows one view, shows the right eye's half |
| `ACVR_GPU_TIMING=1` | Logs the compositor's GPU time |
| `ACVR_TEST_MIRROR_DUMP=<frame>[,<frame>...]` | The bench writes those frames' Window view capture into `mirror-dump/` |
| `ACVR_TEST_WINDOW_TILT=<degrees>` | Turns the window about its vertical axis, to see its depth without moving the Simulator's camera |
| `ACVR_TEST_WINDOW_DUMP=<frame>` | The app writes that window frame's images into its `Documents` |
| `ACVR_TEST_WINDOW_MIRROR=0`, `ACVR_TEST_WINDOW_EFFECTS=1\|ghost` | The window without its scene mirror, or with the screen effects' layer |
| `ACVR_TEST_SENSE_NAMES=1` | Logs which controller names count as a PS VR2 Sense controller |
| `ACVR_TEST_TAB`, `ACVR_TEST_CONTROLS` | Opens the launcher on that tab, or that Controls page |

## Tools

| Script | Does |
|---|---|
| `visionos/scripts/render-mirror-dump.py` | Renders a mirror dump from the game camera, as the game would draw it: checks the capture |
| `visionos/scripts/render-window-view.py` | Renders a mirror dump as the window lays it out, from anywhere in the room: checks the window's shape and depth |
| `visionos/scripts/window-dump.py` | Turns a window frame dump into PNGs and splits out the screen effects |
| `visionos/scripts/gen-mirror-materials.py` | Writes the window's RealityKit materials (`visionos/App/Resources/MirrorMaterials*.usda`) |
| `visionos/scripts/make-app-icon-art.py`, `make-app-icon.py` | Draws the app icon (original art: a fruit tree at sunset and a bug net) and packages it |
| `visionos/scripts/make-readme-hands.py` | The README's hand drawings, from TrevorbiltKit's |
| `visionos/scripts/sync-trevorbilt-kit.sh` | Vendors TrevorbiltKit from its source repository |

## Known limits

- The Window view draws each surface's main texture, so a dirt path blended over grass by a second
  texture can come out as grass.
- The Window view's town has its full depth: a 1.2 m window shows your character about 0.35 m
  behind the glass, and the far edge of the town a few metres back. Nearer than about 1.6 window
  widths, you can see a little past the sides of what the game drew (it culls to its own view).
- The progressive space draws a pass per eye, so it costs a little more than Full.
