// Autostart — "Start at Login", as an XDG autostart entry.
//
// ~/.config/autostart/mynah-kde.desktop, pointing at this very binary, so a
// build-tree mynah-kde starts itself and an installed one starts
// /usr/bin/mynah-kde. The systemd unit is the headless engine's way in;
// Plasma starts desktop apps from autostart entries.

#pragma once

namespace autostart {

bool enabled();
// false when the entry could not be written or removed.
bool setEnabled(bool on);

} // namespace autostart
