# Installing Animal Crossing VR on Apple Vision Pro

Animal Crossing VR is [Trevorbilt](https://trevorbilt.com)'s native visionOS port, built on
LiquidAzir's [animal-crossing-vr](https://github.com/LiquidAzir/animal-crossing-vr) mod and the
[ac-decomp](https://github.com/ACreTeam/ac-decomp) decompilation; the
[README](README.md#credits) credits everyone it builds on.

*Animal Crossing* (GameCube) runs natively on Apple Vision Pro once you build this project with
Xcode and install it on your own headset. Play it in **Full** immersion (your town around you, in
first person at life size), **Progressive** (the same through a portal the Digital Crown widens),
or in a **Window** beside your other apps, with real depth behind the glass. There's no ready-made
app to download, and none of the game's files are here: you bring your own disc.

## Requirements

- **Apple Vision Pro** on visionOS 26 or later (tested on visionOS 27).
- **A Mac with Apple silicon** and **Xcode** at `/Applications/Xcode.app`, not a path with a space
  in it (CMake can't handle one). Built and tested with Xcode 27 on macOS 27. Open Xcode once so it
  finishes installing, then add its visionOS platform in Xcode › Settings › Components.
- **An Apple Account** signed in to Xcode (Xcode › Settings › Accounts). The app asks for the
  increased memory limit. It's been built with a paid Apple Developer Program team; whether a free
  account can sign it is untested, and apps a free account signs stop opening after 7 days until
  you install them again.
- **Homebrew** ([brew.sh](https://brew.sh)) and three tools from it:

  ```bash
  brew install cmake ninja xcodegen
  ```

- **About 2 GB of free disk space.** The first build compiles the whole game and takes 10 to 30
  minutes (11 on an M4 Mac); later builds take minutes.
- **A controller is optional in Full and Progressive:** PS VR2 Sense controllers, a gamepad, or
  your bare hands. The Window view needs a gamepad, or both Sense controllers.

## Game files

You supply the game yourself; this repository contains none, and the build never downloads any.

1. **Dump your own disc** as `.iso`, `.gcm` or `.ciso` (an NKit `.iso` works too). Dolphin's
   [guide](https://wiki.dolphin-emu.org/index.php?title=Ripping_Games) explains how.
2. **Check it's the supported release:** *Animal Crossing*, USA, game ID **GAFE01, revision 0**.
   Dolphin's game list shows a disc's game ID and revision. The app tells you if a disc is another
   region or game.
3. **Keep it handy.** It goes onto the headset after the app is installed (step 5 below).

## Get the code

If you came from the AVP Ports Index and already cloned the commit it lists, skip this step and
build from that folder.

```bash
git clone https://github.com/edgytoast/animalcrossing-visionos.git animalcrossing-visionos
cd animalcrossing-visionos
```

## Build and install

1. **Connect the headset.** If you haven't run your own apps on it before, turn on Developer Mode
   (Settings › Privacy & Security › Developer Mode; the headset restarts), then pair it with your
   Mac: open Xcode › Window › Devices and Simulators, and on the headset open Settings › General ›
   Remote Devices and pick your Mac. Keep the headset on and unlocked while it installs.
2. **Find your team ID:** the 10-character ID shown for your team in Xcode › Settings › Accounts.
3. **Build and install,** from the project's folder:

   ```bash
   visionos/scripts/build-visionos.sh --team YOURTEAMID --install
   ```

   With more than one headset paired, add `--device <UDID>` (`xcrun devicectl list devices` lists
   them). Here's everything it does on your Mac. It never uses `sudo`, and apart from Xcode's and
   CMake's own caches it writes only inside this folder:

   1. **Checks** for Xcode, `cmake`, `ninja` and `xcodegen`, and says what's missing.
   2. **Builds SDL 2 for visionOS, once.** It clones SDL's `release-2.32.10` tag from
      [libsdl-org/SDL](https://github.com/libsdl-org/SDL) into `visionos/third_party/`, applies
      this project's visionOS fixes (`visionos/patches/sdl2/`), and builds it static.
   3. **Builds the game** as `ACVRVisionGame.framework`, in `.scratch/`. CMake downloads the
      [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) 1.1.61's headers for it.
   4. **Generates the Xcode project** (`visionos/App/ACVRVision.xcodeproj`) and builds the app,
      signed with your team. The bundle ID is `dev.acvr.vision.YOURTEAMID`: keep the team suffix,
      since a bare ID can already belong to someone else's team, and signing then fails on the
      memory entitlement.
   5. **Installs it** on your paired Apple Vision Pro with `xcrun devicectl`.

4. **Trust it,** if visionOS says the developer isn't trusted: Settings › General › VPN & Device
   Management.
5. **Add your disc**, any one of these ways:
   - AirDrop the disc image to your Vision Pro and choose **Animal Crossing VR** to open it with.
   - Put it anywhere Files can reach, then tap **Import** in the app.
   - Copy it into the app's own folder in Files (On My Apple Vision Pro › Animal Crossing VR).
6. **Play.** Pick a view (Full, Progressive or Window) and tap **Play**. The launcher's
   **Controls** tab shows every button and gesture for what you're holding.

## Update

Get the new code: if you came from the AVP Ports Index, run `git fetch`, then `git checkout` the
commit Animal Crossing VR's page there lists now (that's the code it scanned; `git pull` doesn't
work on it). If you cloned `main`, `git pull`. Then build and install again:

```bash
visionos/scripts/build-visionos.sh --team YOURTEAMID --install
```

Your disc and saves stay on the headset. The app's About tab says when a newer version is out.

## Saving

Save the way the game always did: talk to the gyroid beside your house. In Full or Progressive,
press the Digital Crown to pause: the game holds where you are, and the launcher offers **Resume**
or **Quit** (if it doesn't appear, open the app from Home). In the Window view, close the window to
quit. Quit without saving and Mr. Resetti will have words with you next time, just like on the
GameCube.

Saves are the game's own memory card files, in Files › On My Apple Vision Pro › Animal Crossing VR
› save. iCloud Backup includes them. Copy them out to keep your own backup: deleting the app deletes
them.

## Controls and known issues

- **Full and Progressive:** PS VR2 Sense controllers play the VR mod's layout (left stick walks,
  right stick turns, the right trigger or Cross talks and picks up, the left trigger or Circle runs,
  Square opens the map, Triangle the pockets, and a swing of your tool hand uses your tool). With no
  controller, your bare hands play: pinches are the buttons, and a held pinch, moved, walks and
  turns. A gamepad plays the original game's buttons while you look around. The
  [README](README.md#play) has every control, with drawings of the hand gestures.
- **Window:** it plays like the original game, so it needs a gamepad, or both Sense controllers
  held as one. Look at the game window to give it your controller: visionOS sends a controller's
  input to the window you're looking at.
- **Settings:** in Full and Progressive, Options and a right stick click open the port's VR
  settings: Tool hand (right or left), anti-aliasing (Off, FXAA, SMAA), render scale (50 to 150%),
  turning and hand visuals. Turning, the comfort vignette and foveated rendering are also under
  **Advanced** in the launcher.

Known issues:

- The Window view recreates the game's scene in visionOS's own renderer and draws each surface's
  main texture, so a dirt path blended over grass by a second texture can come out as grass.
- The Window view's town has its full depth. Nearer than about one and a half window widths, you
  can see a little past the edges of what the game drew.
- Progressive draws a pass per eye (Full draws both eyes in one), so it costs a little more.

Troubleshooting is in the [README](README.md#if-something-goes-sideways), and building for the
visionOS Simulator in [docs/DEVELOPING.md](docs/DEVELOPING.md).

## Credits and licenses

- The Apple Vision Pro port by [Trevorbilt](https://trevorbilt.com), MIT.
- [ac-decomp](https://github.com/ACreTeam/ac-decomp), the decompilation, by ACreTeam (CC0-1.0).
- [ACGC-PC-Port](https://github.com/flyngmt/ACGC-PC-Port), the PC port, by flyngmt (MIT), and its
  64-bit [Android port](https://github.com/zwaetschge/ACGC-Android-Port) by zwaetschge (MIT).
- [animal-crossing-vr](https://github.com/LiquidAzir/animal-crossing-vr), the VR mod, by LiquidAzir
  (MIT).
- [FixNES](https://github.com/FIX94/fixNES), the in-game NES, by FIX94 (MIT).
- The visionOS OpenXR provider from [WiiCompiled Vision](https://github.com/iChris4/Wiicompiled_VR)
  (GPL-3.0-or-later), so the app you build is GPL-3.0.
- TrevorbiltKit, the shared launcher (MIT; its fonts under the SIL Open Font License 1.1).

[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) lists every component and its license. *Animal
Crossing* is Nintendo's. This project isn't affiliated with or endorsed by Nintendo, and none of
the game's files are in the app or the repository.
