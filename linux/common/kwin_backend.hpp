// mynah::inject — KWin's selection backend for PasteTyper.
//
// One Wayland connection, three globals:
//   - ext_data_control_manager_v1: the clipboard and the primary selection,
//     read and set without a window or focus (KWin offers it to every
//     client);
//   - org_kde_kwin_fake_input: the paste chord. KWin grants it only to an
//     executable whose installed .desktop file lists it in
//     X-KDE-Wayland-Interfaces (linux/packaging/mynah.desktop) — no consent
//     dialog, which is why this and not the RemoteDesktop portal;
//   - wl_seat, for the data device.
// There is no zwp_virtual_keyboard in KWin at all, so wtype cannot work
// there (docs/LINUX-KDE-PLAN.md, Stage 3).
//
// Every Wayland call runs on the backend's own event thread: the paste is
// served from there after the chord (the app asks for the text only once
// it sees Shift+Insert), and so are the restored contents afterwards, for
// as long as mynah owns the selection.

#pragma once

#include <memory>
#include <string>

#include "paste_typer.hpp"

namespace mynah::inject {

// The backend when this is a KWin session, else nullptr with `why` set:
// no Wayland display, or a compositor that is not KWin. A KWin backend is
// returned even when fake_input was not granted — its check() then says
// how to grant it, which is the remedy the user needs.
std::unique_ptr<SelectionBackend> make_kwin_backend(std::string* why = nullptr);

// PasteTyper over make_kwin_backend(), or nullptr off KWin.
std::unique_ptr<Injector> make_kwin(std::string* why = nullptr);

} // namespace mynah::inject
