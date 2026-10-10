import SwiftUI
import TrevorbiltKit

/// Everything in the launcher that's this port's own: its name, the ways it plays and what each
/// plays with, what pausing can do, its game files, Advanced options, the controls, credits and
/// licences. The layout and scene plumbing (LauncherView, ControlsView, GameModel) and
/// TrevorbiltKit are shared by every Trevorbilt port: change them only to fix them for all.
///
/// Fill every TODO. Copy: plain, warm, low pressure; short direct verbs ("Play", "Resume"); no em
/// dashes; errors say what to do. Run new copy past the trevorbilt-critic agent.
enum Port {
    // MARK: Who and where

    /// The port's name, its last word bold in the header.
    static let title = "Animal Crossing "
    static let boldTitle = "VR"
    /// The game on Apple Vision Pro (not "in VR": Window isn't VR).
    static let subtitle = "Animal Crossing on Apple Vision Pro"
    /// The public release, "owner/name", and its branch: the update check, Report a problem (its
    /// issues), the GPL source offer. The "Build commit" phase in project.yml names it too.
    static let repository = "edgytoast/animalcrossing-visionos"
    static let releaseBranch = "main"
    /// The port's id in the AVP Ports Index once listed (the Ports tab marks its own card).
    static let indexID = "animal-crossing-vr"
    /// How a player updates once the update check finds a newer version.
    static let updateInstructions = "git pull, then visionos/scripts/build-visionos.sh --team <your team> --install (AVP-INSTALL.md has the details). Your game files and saves stay."

    // MARK: Modes

    /// The ways this port plays, in the order shown.
    static let modes: [GameModel.Immersion] = [.full, .progressive, .window]

    /// One line on each mode's card.
    static func line(_ mode: GameModel.Immersion) -> String {
        switch mode {
        case .full: "Life size in your town, in first person."
        case .progressive: "A portal in your room. The Digital Crown widens it."
        case .window: "Beside your other apps. Plays with a controller."
        }
    }

    /// One plain sentence under the cards on what the selected mode (and its option) does.
    static func note(_ mode: GameModel.Immersion, roomBehindMenus: Bool) -> String {
        switch mode {
        case .full where roomBehindMenus:
            "Menus and loading screens float in your room. Walk about 1.2 m from where you started and the game fades into the room."
        case .full:
            "The game stays around you in menus too, with visionOS's own boundary."
        case .progressive:
            "Turn the Digital Crown to widen or narrow the portal. Press it to pause."
        case .window:
            windowClosePauses ? "With depth you can see as you move. Close the window to pause."
                              : "With depth you can see as you move. Close the window to quit."
        }
    }

    // MARK: Pause capabilities

    /// The Digital Crown closing Full or Progressive pauses the game, and the launcher comes back
    /// with Resume. Needs a game that holds its session and takes a new layer (acvr_visionos_can_resume).
    static let crownPauses = true
    /// Quit beside Resume: the game keeps everything up to your last save.
    static let quitsFromPause = true
    /// Closing the game's window ends the game and the app (it can quit from anywhere).
    static let windowClosePauses = false
    /// What Quit keeps, said beside Resume.
    static let pausedNote = "Resume carries on where you were. Quit keeps your progress up to your last save."

    // MARK: Inputs (mode → inputs)

    /// Which inputs each mode plays with, and whether they're here.
    @MainActor static func verdict(_ mode: GameModel.Immersion, _ inputs: InputMonitor) -> InputVerdict {
        if mode == .window {
            // Two Sense controllers make one gamepad in the window (host/sense_pad).
            if inputs.senseLeft != nil && inputs.senseRight != nil {
                return InputVerdict("Ready to play with your Sense controllers, as a gamepad.", ready: true)
            }
            if inputs.anySense {
                return InputVerdict("Only one Sense controller is connected. Turn on the other, or turn this one off to play with a gamepad.", ready: false)
            }
            if inputs.gamepad != nil { return InputVerdict("Ready to play with your gamepad.", ready: true) }
            return InputVerdict("The Window view plays with a controller. Connect your Sense controllers or a gamepad.", ready: false)
        }
        // Full and Progressive: each hand is a Sense controller if one's in it, else the bare hand;
        // a gamepad plays alongside either.
        let handsPlay = inputs.hands == .allowed || inputs.hands == .notAsked
        if inputs.senseLeft != nil && inputs.senseRight != nil {
            if inputs.accessories == .denied && !handsPlay {
                return InputVerdict("Accessory tracking is off for Animal Crossing VR, so the game can't follow your controllers. Turn it on in Settings.",
                                    ready: false, settingsNeeded: true)
            }
            return InputVerdict("Ready to play with your Sense controllers. Have fun!", ready: true)
        }
        if inputs.gamepad != nil { return InputVerdict("Ready to play with your gamepad.", ready: true) }
        if inputs.anySense {
            return handsPlay
                ? InputVerdict("Ready with one Sense controller. Your other hand plays bare.", ready: true)
                : InputVerdict("Only one Sense controller is connected and hand tracking is off. Turn on the other controller, or turn hand tracking on in Settings.",
                               ready: false, settingsNeeded: true)
        }
        switch inputs.hands {
        case .allowed:
            return InputVerdict("Ready to play with your hands. The Controls tab shows how.", ready: true)
        case .notAsked:
            return InputVerdict("Your hands play. visionOS asks to track them when the game starts.", ready: true)
        case .denied:
            return InputVerdict("Hand tracking is off for Animal Crossing VR. Turn it on in Settings, or connect a controller.",
                                ready: false, handsNeeded: true)
        case .unavailable:
            return InputVerdict("No controller is connected, and hand tracking isn't available here.", ready: false)
        }
    }

    // MARK: Game files

    /// What the player calls the game's files, as the card says it.
    static let filesNoun = "disc"
    static let filesSymbol = "opticaldisc.fill"
    /// Which files, which releases, and the ways in.
    static let filesHelp = "Your own Animal Crossing disc (GameCube, USA, GAFE01) as an .iso (NKit too), .gcm or .ciso. AirDrop it to this Vision Pro, put it in Files › On My Apple Vision Pro › Animal Crossing VR, or import it here."

    /// Whether `file` is a disc the game plays, from its header: nil when it is, else what's wrong
    /// and what to do. ISO/GCM: the game ID at offset 0 and the GameCube magic at 0x1C. CISO: its
    /// "CISO" header (the game checks the disc inside when it boots).
    nonisolated static func validate(_ file: URL) -> String? {
        guard let handle = try? FileHandle(forReadingFrom: file) else { return "The disc can't be read. Import it again." }
        defer { try? handle.close() }
        guard let head = try? handle.read(upToCount: 0x204), head.count >= 0x20 else {
            return "The file is too short to be a disc. Import it again."
        }
        let bytes = [UInt8](head)
        if bytes[0..<4] == [0x43, 0x49, 0x53, 0x4F] { return nil }  // "CISO"
        // NKit images ("NKIT" at 0x200) play: they keep the GameCube header and the file table,
        // and the game finds its files through the table (a 27 MB NKit v01 GAFE01 boots, plays and
        // saves on the macOS bench and in the Simulator).
        let magic = UInt32(bytes[0x1C]) << 24 | UInt32(bytes[0x1D]) << 16 | UInt32(bytes[0x1E]) << 8 | UInt32(bytes[0x1F])
        guard magic == 0xC233_9F3D else { return "This isn't a GameCube disc. Animal Crossing VR needs your Animal Crossing disc." }
        let id = String(bytes: bytes[0..<6], encoding: .ascii) ?? ""
        if id == "GAFE01" { return nil }
        if id.hasPrefix("GAF") { return "This is \(id), another region. Animal Crossing VR plays the USA disc (GAFE01)." }
        return "This is \(id), not Animal Crossing. Animal Crossing VR needs your Animal Crossing disc (GAFE01)."
    }

    // MARK: Advanced options

    /// Options that must be set before Play and that most players never touch.
    struct AdvancedOptions: View {
        @ObservedObject var model: GameModel
        /// Read by the game at Play (game_host.mm): 0 turns smoothly, else the snap angle. The game
        /// writes it back when its pause menu's Turning changes, so the two never disagree.
        @AppStorage("snapTurnDegrees") private var snapTurnDegrees = 0
        @AppStorage("comfortVignette") private var comfortVignette = true

        var body: some View {
            TrevorbiltCard {
                VStack(alignment: .leading, spacing: 8) {
                    Text("Turning").font(.tbBody(14, weight: .bold))
                    Picker("Turning", selection: $snapTurnDegrees) {
                        Text("Smooth").tag(0)
                        Text("Snap 30°").tag(30)
                        Text("Snap 45°").tag(45)
                        Text("Snap 90°").tag(90)
                    }
                    .pickerStyle(.segmented)
                    Text("How the right stick and the turn pinch turn you in first person. Snap turns in steps, which many people find easier on the stomach.")
                        .font(.tbBody(12)).foregroundStyle(.white.opacity(0.8))
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            TrevorbiltCard {
                Toggle(isOn: $comfortVignette) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Comfort vignette").font(.tbBody(14, weight: .bold))
                        Text("Darkens the edges of your view while the game moves you: walking, turning, and the view sliding to the next part of town.")
                            .font(.tbBody(12)).foregroundStyle(.white.opacity(0.8))
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .tint(Trevorbilt.orange)
            }
            TrevorbiltCard {
                Toggle(isOn: $model.foveated) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Foveated rendering").font(.tbBody(14, weight: .bold))
                        Text("In Full and Progressive: sharper where you look. The game renders larger eye images for it, which costs GPU time, so lower the game's render resolution if it stutters.")
                            .font(.tbBody(12)).foregroundStyle(.white.opacity(0.8))
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                .tint(Trevorbilt.orange)
            }
        }
    }

    // MARK: Controls

    /// Pages of controls. One page.
    static let controlPages: [String] = []

    /// The controls, read from the game (include/m_submenu.h: Start or Y opens your pockets, X the map;
    /// m_kankyo.c: Z the light switch indoors; m_player_lib.c: B, L or R held runs), the PC port's
    /// gamepad bindings (pc_keybindings.c; its d-pad switches tools), the VR mod's Touch mapping as
    /// pc_vr.cpp binds it for the Sense controllers, the provider's bare-hand pinches, and pc_pad.c's
    /// Window layout for the Sense halves as one pad.
    static func controls(_ input: ControlsView.Input, window: Bool, page: Int) -> [ControlItem] {
        func button(_ n: Int, _ action: String, _ how: String, _ anchor: String?, also: [String: String] = [:]) -> ControlItem {
            ControlItem(n, action, how, anchor: anchor, also: also)
        }
        func hand(_ n: Int, _ action: String, _ how: String, _ anchor: String, _ pose: HandPose) -> ControlItem {
            ControlItem(n, action, how, anchor: anchor, pose: .hand(pose))
        }
        switch (input, window) {
        case (.hands, _):
            return [hand(1, "Walk", "Pinch your left thumb and middle finger, hold, and move your hand.", "leftMiddle", .pinchMiddleLeftMove),
                    hand(2, "Turn", "Pinch your right thumb and little finger, hold, and move it sideways.", "rightLittle", .pinchLittleRightTurn),
                    hand(3, "Talk, pick up, use", "Pinch your right thumb and index finger.", "rightIndex", .pinchIndexRight),
                    hand(4, "Use your tool", "Swing your right hand while you hold a net, axe, shovel or rod. Left-handed? Pick Tool hand: Left in the VR settings.", "rightPalm", .swingRight),
                    hand(5, "Run, cancel", "Pinch your left thumb and index finger.", "leftIndex", .pinchIndexLeft),
                    hand(6, "Map", "Tap your left thumb and middle finger together.", "leftThumb", .pinchMiddleLeft),
                    hand(7, "Pockets", "Pinch your left thumb and ring finger.", "leftRing", .pinchRingLeft),
                    hand(8, "Menu", "Pinch your left thumb and little finger.", "leftLittle", .pinchLittleLeft)]
        case (.sense, false):
            return [button(1, "Walk", "Left stick", "leftStick", also: ["leftStick": "Pockets (click)"]),
                    button(2, "Turn", "Right stick", "rightStick", also: ["rightStick": "Z (click)"]),
                    button(3, "Talk, pick up, use", "Right trigger or Cross, or a swing", "rightTrigger", also: ["a": "Talk, pick up, use"]),
                    button(4, "Run, cancel", "Left trigger or Circle", "leftTrigger", also: ["b": "Run, cancel"]),
                    button(5, "Map", "Square", "x"),
                    button(6, "Pockets", "Triangle or Create", "y", also: ["view": "Pockets"]),
                    button(7, "Switch tools", "Hold the left grip and push the left stick", "leftShoulder"),
                    button(8, "Run", "Right grip", "rightShoulder"),
                    ControlItem(9, "Use your tool", "Swing your right controller, or your left with Tool hand: Left", anchor: "swing")]
        case (.sense, true):
            return [button(1, "Walk", "Left stick", "leftStick"),
                    button(2, "Camera", "Right stick", "rightStick"),
                    button(3, "Talk, pick up, use", "Cross", "a"),
                    button(4, "Run, cancel", "Circle, or hold either trigger", "b", also: ["leftTrigger": "Run", "rightTrigger": "Run"]),
                    button(5, "Map", "Square", "x"),
                    button(6, "Pockets", "Triangle or Options", "y", also: ["menu": "Pockets"]),
                    button(7, "Switch tools", "Hold the left grip and push the left stick", "leftShoulder"),
                    button(8, "Light switch", "Right grip, indoors", "rightShoulder")]
        case (.gamepad, _):
            return [button(1, "Walk", "Left stick", "leftStick"),
                    button(2, window ? "Camera" : "Look", "Right stick", "rightStick"),
                    button(3, "Talk, pick up, use", "A", "a"),
                    button(4, "Run, cancel", "B, or hold either trigger", "b", also: ["leftTrigger": "Run", "rightTrigger": "Run"]),
                    button(5, "Map", "X", "x"),
                    button(6, "Pockets", "Y or Menu", "y", also: ["menu": "Pockets"]),
                    button(7, "Switch tools", "D-pad", "dpad"),
                    button(8, "Light switch", "Right shoulder, indoors", "rightShoulder")]
        }
    }

    /// One note under each page.
    static func controlsNote(_ input: ControlsView.Input, window: Bool) -> String {
        switch (input, window) {
        case (.hands, _): "Hands play in Full and Progressive. A Sense controller takes over the hand holding it."
        case (.sense, false): "Your net, axe, shovel and rod follow your right hand, or your left: Options and a right stick click open the VR settings, where Tool hand switches it. Square and Triangle together recenter you; the left grip and Triangle switch between first person and the game's camera."
        case (.gamepad, false): "In Full and Progressive a gamepad plays the original game's buttons while you look around."
        case (.sense, true): "In the Window view your Sense controllers act as one gamepad. Look at the window to give it your controller."
        case (.gamepad, true): "The Window view needs a controller: visionOS gives apps no hand tracking outside Full and Progressive. Look at the window to give it your controller."
        }
    }

    // MARK: About

    /// The app links the vendored OpenXR provider (GPL-3.0-or-later) from M2 on.
    static let appIsGPL = true

    /// The repository at the commit this build came from (a development build's isn't public, so
    /// the release branch): the GPL source offer.
    static var sourceOfThisBuild: URL? {
        let build = BuildInfo.current
        return URL(string: "https://github.com/\(repository)/tree/\(build.dirty ? releaseBranch : build.commit ?? releaseBranch)")
    }

    static var about: AboutContent {
        var notices: [AboutContent.Notice] = [
            .init("This repository: CC0 and MIT", "The decompilation (ACreTeam/ac-decomp) is CC0. The PC port layer (flyngmt), its 64-bit and Android work (zwaetschge), the VR additions (LiquidAzir) and Trevorbilt's visionOS code are MIT.",
                  URL(string: "https://github.com/\(repository)/blob/\(releaseBranch)/LICENSE")),
        ]
        if appIsGPL {
            notices.append(.init("This app: GPL-3.0", BuildInfo.current.dirty
                ? "It links the visionOS OpenXR provider, which is GPL-3.0-or-later, so the app is GPL-3.0. This is a development build: its source is the copy it was built from, and the repository has the released ones."
                : "It links the visionOS OpenXR provider, which is GPL-3.0-or-later, so the app is GPL-3.0. Its source is the repository at the commit this build came from.",
                sourceOfThisBuild))
        }
        notices += [
            .init("The launcher's shared code: MIT", "TrevorbiltKit, in visionos/TrevorbiltKit.",
                  URL(string: "https://github.com/\(repository)/blob/\(releaseBranch)/visionos/TrevorbiltKit/LICENSE")),
            .init("The launcher's fonts", "Space Mono and Roboto: SIL Open Font License 1.1.",
                  URL(string: "https://github.com/\(repository)/tree/\(releaseBranch)/visionos/TrevorbiltKit/Sources/TrevorbiltKit/Resources/Fonts")),
            .init("SDL 2", "Audio, timers and controllers: zlib licence.", URL(string: "https://github.com/libsdl-org/SDL/blob/release-2.32.10/LICENSE.txt")),
            .init("FixNES", "The in-game NES: MIT, by FIX94.", URL(string: "https://github.com/FIX94/fixNES")),
            .init("Trevorbilt's name, badge and app icons", "All rights reserved; not covered by the repository's licences."),
        ]
        return AboutContent(
            appName: "Animal Crossing VR",
            repository: repository,
            releaseBranch: releaseBranch,
            updateInstructions: updateInstructions,
            // Trevorbilt first, then the lineage by name and link, as the README lists it.
            credits: [
                .init("Trevorbilt", "The Vision Pro port: the visionOS app, its immersive spaces and window, the input and the build.", "https://trevorbilt.com"),
                .init("ACreTeam", "ac-decomp, the decompilation of Animal Crossing the port is built on.", "https://github.com/ACreTeam/ac-decomp"),
                .init("flyngmt", "ACGC-PC-Port, the native PC port: GX on OpenGL, saves, the game's PC layer.", "https://github.com/flyngmt/ACGC-PC-Port"),
                .init("zwaetschge", "ACGC-Android-Port, the 64-bit ARM port this one starts from.", "https://github.com/zwaetschge/ACGC-Android-Port"),
                .init("LiquidAzir", "animal-crossing-vr: per-eye rendering, first person, the tools in your hand.", "https://github.com/LiquidAzir/animal-crossing-vr"),
                .init("Twilight Princess VR", "The OpenXR provider for visionOS, from WiiCompiled Vision through TPVR.", "https://github.com/edgytoast/tpvr-visionos"),
            ],
            notices: notices,
            disclaimer: "Animal Crossing VR is a fan project. It isn't affiliated with or endorsed by Nintendo, who own the game, its characters and its art. Your copy of the game is yours to bring.",
            otherPorts: [
                (name: "Twilight Princess VR", url: URL(string: "https://github.com/edgytoast/avp-ports-index/blob/main/ports/twilight-princess-vr.md")!),
                (name: "SHAR VR", url: URL(string: "https://github.com/edgytoast/avp-ports-index/blob/main/ports/shar-visionos.md")!),
            ],
            // Stays nil until Trevor gives a support link: never a placeholder.
            supportURL: nil)
    }
}
