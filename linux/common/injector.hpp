// mynah::inject — typing into the focused window on Wayland.
//
// The port of linux_inject.py + linux_clipboard.py (Phase 3). `wtype`
// speaks the virtual-keyboard protocol, the compositor-blessed way to
// synthesize input. X11 is not supported here — see docs/LINUX-APP.md for
// why we do not ship a path where every client can already do this to
// every other client.
//
// The text goes in on **stdin**, never in argv: argv is world-readable
// through /proc, so passing a transcript as an argument would publish
// every dictated sentence to every process on the machine for as long as
// the command ran.
//
// Apps that ignore a virtual keyboard render its keystrokes as digits
// (they read the keycodes with the real keyboard's layout), so the
// clipboard injector pastes instead: one chord, arbitrary Unicode, no
// keymap involved — and the clipboard is borrowed for under a second and
// put back. SmartInjector picks per focused window.

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace mynah::inject {

// Where the binaries are; every path is resolved up front so the tests can
// point at fakes and the error paths can name what is missing. An empty
// optional means "not installed".
struct Tools {
    std::optional<std::string> wtype;
    std::optional<std::string> wl_copy;
    std::optional<std::string> wl_paste;
    std::optional<std::string> hyprctl;

    // PATH lookup + the MYNAH_WTYPE override, exactly like the Python
    // side resolved them.
    static Tools discover();
};

// Whether this is a Wayland session — the virtual-keyboard path only
// exists there.
bool is_wayland();

// The paste shortcut for a window class, as (mods, key): terminals paste
// with Ctrl+Shift+V, because Ctrl+V is a control character to the program
// inside them. Matched as substrings of the lowercased window class.
std::pair<std::string, std::string> paste_chord(const std::string& window_class);

// The class of the focused window, lowercased, or "" when we cannot ask
// (not Hyprland, no hyprctl, or a wedged call).
std::string focused_class(const std::optional<std::string>& hyprctl);

class Injector {
public:
    virtual ~Injector() = default;

    // Type text into the focused window. Returns true when the text
    // landed; false means a named tool was missing, the session is wrong,
    // or the attempt failed — and the caller logs the remedy.
    virtual bool type_text(const std::string& text) = 0;

    // (ok, remedy): what a human must do to make typing work here. The
    // messages are the Python CLI's word-for-word — the pinned Omarchy
    // plugin greps stderr for fragments of them.
    virtual std::pair<bool, std::string> check() const = 0;
};

// wtype: the virtual keyboard.
std::unique_ptr<Injector> make_wtype(Tools tools);

// clipboard: paste through the compositor, borrow the clipboard, give it back.
std::unique_ptr<Injector> make_clipboard(Tools tools);

// smart: type, except into the apps where typing does not work.
std::unique_ptr<Injector> make_smart(Tools tools);

// What the session should use: KWin's paste typer on KWin (kwin_backend.hpp;
// wtype cannot work there), smart everywhere else. Connects to the Wayland
// display to find out — so the tests, which must never type into the
// developer's focused window, use the explicit factories above instead.
std::unique_ptr<Injector> make_auto(Tools tools);

} // namespace mynah::inject