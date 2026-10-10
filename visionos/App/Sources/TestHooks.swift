import Foundation

/// The environment variables that drive the launcher's headless Simulator runs
/// (SIMCTL_CHILD_ACVR_TEST_…). They exist only in Simulator builds: an app installed on a headset
/// ignores them. Set but empty counts as not set, so a test script can pass every hook and leave the
/// ones it doesn't need blank. (ACVR_AUTO_PLAY and ACVR_IMMERSION read the environment directly,
/// so devicectl launches can use them too.)
///
///   ACVR_TEST_TAB=play|controls|ports|about            the tab to open on
///   ACVR_TEST_SHEET=manage|advanced|credits|diagnostics|port:<index id>
///   ACVR_TEST_INPUTS=hands:denied,sense:R,gamepad:none  what InputMonitor reports
///   ACVR_TEST_CONTROLS=hands|sense|gamepad[,window][,page=<n>]
///   ACVR_TEST_PAD=none|xbox                            the gamepad drawing, as if none (an Xbox kind) were connected
///   ACVR_TEST_FEED=<host path>  ACVR_TEST_OFFLINE=1     the Ports tab's feed, or no network
///   ACVR_TEST_IMPORT=<host path>                       imports that file as if picked
///   ACVR_TEST_PAUSED=resume@<s>|quit@<s>               presses Resume or Quit after a Crown pause
///   ACVR_TEST_CLOSE_SPACE=<s>                          closes the immersive space after Play, as the Crown does
enum TestHooks {
    static func value(_ name: String) -> String? {
        #if targetEnvironment(simulator)
        ProcessInfo.processInfo.environment[name].flatMap { $0.isEmpty ? nil : $0 }
        #else
        nil
        #endif
    }
}
