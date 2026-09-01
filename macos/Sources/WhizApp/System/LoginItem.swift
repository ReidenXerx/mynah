import ServiceManagement

/// Start-at-login, via `SMAppService`.
///
/// This is the whole replacement for `whiz/dictate/service.py` — 343 lines that
/// hand-wrote LaunchAgent plist XML, shelled out to `launchctl`, and copied
/// Apple's framework Python binary to `~/.local/share/whiz/whiz` so Activity
/// Monitor would show "whiz" instead of "Python" and so the TCC grant would
/// survive `pipx install --force`.
///
/// An app bundle has a name and a stable identity by construction, so all of
/// that collapses to `register()` / `unregister()`.
enum LoginItem {

    static var isEnabled: Bool {
        SMAppService.mainApp.status == .enabled
    }

    static func enable() throws {
        try SMAppService.mainApp.register()
    }

    static func disable() throws {
        try SMAppService.mainApp.unregister()
    }
}
