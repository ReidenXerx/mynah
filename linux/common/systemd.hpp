// mynah::systemd — the login service, as `mynah service ...` drives it.
//
// The packaged unit lives at /usr/lib/systemd/user/mynah.service (the
// package installs it); this wraps `systemctl --user` on it, with the
// same semantics and the same stderr wording mynah/systemd.py had — the
// Omarchy plugin's menu items call these and grep the output.
//
// The unit's own two details are not decoration: PartOf/WantedBy
// graphical-session.target ties the daemon to the graphical session
// rather than to login (mynah types through the compositor; started
// before there is one it can only fail, and left running after the
// session ends it holds a microphone for nobody), and StartLimitBurst
// gives up after three failures in a minute — a service that cannot
// start is a service that needs a person, not a retry loop filling the
// journal.

#pragma once

#include <string>

namespace mynah::systemd {

inline constexpr const char* kUnit = "mynah.service";

// install / uninstall / status: 0 on success, 1 with a message on stderr.
int install();
int uninstall();
int status();

} // namespace mynah::systemd