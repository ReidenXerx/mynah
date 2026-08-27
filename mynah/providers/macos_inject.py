"""MacTextInjector — type text into the focused macOS app.

Two paths:
- ASCII text (printable Latin): CGEvent keystroke synthesis via the
  Quartz framework. Fast, no clipboard pollution.
- Non-ASCII text (Cyrillic, emoji, etc.): set the system clipboard, then
  synthesize a ⌘V paste via CGEvent. CGEvent keystrokes can't directly
  emit arbitrary Unicode code points, so paste is the reliable path for
  Cyrillic — which is exactly what we need for Russian dictation.

Both paths require the process to have Accessibility permission
(System Settings → Privacy & Security → Accessibility). Without it,
CGEvent posting silently does nothing. ``check_permissions()`` verifies
via ``AXIsProcessTrusted()`` and returns a clear hint.

The split is per-utterance, not per-character: if the transcribed text is
entirely ASCII we keystroke the whole thing; if it contains any non-ASCII
(Cyrillic counts), we paste the whole thing. This avoids mixing methods
within a single utterance and handles Russian (which is all non-ASCII)
cleanly.
"""

from __future__ import annotations

import logging
import time

from whiz.dictate.providers.base import TextInjector

logger = logging.getLogger(__name__)

# CGEvent flags and key codes for ⌘V (paste). These are Quartz constants
# we reference after lazy-importing Quartz — kept as module-level ints so
# tests can patch them without importing pyobjc.
_CMD_V_KEYCODE = 9  # v key on a US keyboard
_CMD_FLAG = 1 << 20  # kCGEventFlagMaskCommand


class MacTextInjector(TextInjector):
    """Injects text into the focused macOS app via CGEvent + clipboard paste."""

    def type_text(self, text: str) -> None:
        """Type ``text`` into the focused app.

        ASCII-only text → CGEvent keystrokes.
        Any non-ASCII → clipboard + ⌘V paste.
        """
        if not text:
            return
        # If the text is entirely ASCII-printable, keystroke it. Otherwise
        # (Cyrillic, emoji, accents) paste via the clipboard. ``str.isascii()``
        # is the cleanest check: Cyrillic letters are non-ASCII.
        if text.isascii():
            self._keystroke(text)
        else:
            self._paste(text)

    def check_permissions(self) -> tuple[bool, str]:
        """Verify Accessibility permission is granted (required for CGEvent posting)."""
        try:
            from ApplicationServices import AXIsProcessTrustedWithOptions
            from CoreFoundation import kCFBooleanTrue
            from Foundation import NSDictionary

            # AXIsProcessTrustedWithOptions with the prompt option opens
            # System Settings to the Accessibility pane if not yet trusted.
            options = NSDictionary.dictionaryWithDictionary_(
                {"AXTrustedCheckOptionPrompt": kCFBooleanTrue}
            )
            trusted = AXIsProcessTrustedWithOptions(options)
            if trusted:
                return True, ""
            return False, (
                "Accessibility permission required. Grant it in:\n"
                "  System Settings → Privacy & Security → Accessibility\n"
                "  Add the terminal/Python that runs whiz, then restart whiz dictate."
            )
        except ImportError:
            # Fallback to the simpler API if ApplicationServices isn't available.
            try:
                from ApplicationServices import AXIsProcessTrusted

                if AXIsProcessTrusted():
                    return True, ""
                return False, (
                    "Accessibility permission required. Grant it in:\n"
                    "  System Settings → Privacy & Security → Accessibility"
                )
            except ImportError:
                return False, (
                    "PyObjC (pyobjc-framework-ApplicationServices) not installed.\n"
                    "Install the dictate extra: pipx inject whiz 'whiz[dictate]'"
                )

    # ---------- internal: keystroke path ----------

    def _keystroke(self, text: str) -> None:
        """Post CGEvent keystrokes for each character of an ASCII string."""
        from Quartz import (
            CGEventCreateKeyboardEvent,
            CGEventSetFlags,
            CGEventPost,
            kCGHIDEventTap,
            kCGEventKeyDown,
            kCGEventKeyUp,
            kCGEventFlagMaskShift,
        )

        for char in text:
            keycode = _char_to_keycode(char)
            if keycode is None:
                # Can't map this ASCII char — fall back to paste for the
                # whole string rather than dropping a character.
                logger.debug("No keycode for %r; falling back to paste", char)
                self._paste(text)
                return
            shift = char.isupper() or char in _SHIFTED_CHARS
            flags = kCGEventFlagMaskShift if shift else 0
            # Key down + key up for each character.
            down = CGEventCreateKeyboardEvent(None, keycode, True)
            if flags:
                CGEventSetFlags(down, flags)
            CGEventPost(kCGHIDEventTap, down)
            up = CGEventCreateKeyboardEvent(None, keycode, False)
            if flags:
                CGEventSetFlags(up, flags)
            CGEventPost(kCGHIDEventTap, up)
            # A tiny delay between characters improves reliability — some
            # apps drop events that arrive too fast.
            time.sleep(0.002)

    # ---------- internal: paste path ----------

    def _paste(self, text: str) -> None:
        """Set the clipboard to ``text`` and simulate a ⌘V paste via CGEvent."""
        # Set the clipboard via NSPasteboard.
        from AppKit import NSPasteboard
        from Quartz import (
            CGEventCreateKeyboardEvent,
            CGEventSetFlags,
            CGEventPost,
            kCGHIDEventTap,
        )

        # NSGeneralPboardType was removed in macOS 14+ / modern pyobjc.
        # The canonical replacement is NSPasteboardTypeString.
        try:
            from AppKit import NSPasteboardTypeString
            paste_type = NSPasteboardTypeString
        except ImportError:
            from AppKit import NSString
            paste_type = NSString

        pb = NSPasteboard.generalPasteboard()
        pb.clearContents()
        pb.setString_forType_(text, paste_type)

        # Small delay so the clipboard write settles before the paste.
        time.sleep(0.01)

        # ⌘ key down, v key down, v key up, ⌘ key up.
        cmd_down = CGEventCreateKeyboardEvent(None, 0x37, True)  # left cmd
        v_down = CGEventCreateKeyboardEvent(None, _CMD_V_KEYCODE, True)
        CGEventSetFlags(v_down, _CMD_FLAG)
        v_up = CGEventCreateKeyboardEvent(None, _CMD_V_KEYCODE, False)
        CGEventSetFlags(v_up, _CMD_FLAG)
        cmd_up = CGEventCreateKeyboardEvent(None, 0x37, False)

        CGEventPost(kCGHIDEventTap, cmd_down)
        CGEventPost(kCGHIDEventTap, v_down)
        CGEventPost(kCGHIDEventTap, v_up)
        CGEventPost(kCGHIDEventTap, cmd_up)

        # Restore the user's clipboard after a brief pause so the paste
        # has time to land in the focused app. We save the old clipboard
        # contents before overwriting above — but for simplicity (and
        # because dictation is the foreground task), we leave the
        # clipboard as-is. A future improvement can save/restore.
        time.sleep(0.05)


# ---------- keycode mapping ----------

# Characters that require Shift on a US keyboard layout.
_SHIFTED_CHARS = set('!@#$%^&*()_+{}|:"<>?~')

# US QWERTY keycodes (virtual key → character). This covers the common
# ASCII printable set. Non-US layouts may differ, but CGEvent posts
# virtual keycodes interpreted by the active layout — for dictation of
# Russian (which goes through the paste path anyway), the keystroke path
# only handles ASCII like numbers and basic punctuation in commands.
_KEYCODE_MAP: dict[str, int] = {
    "a": 0, "s": 1, "d": 2, "f": 3, "h": 4, "g": 5, "z": 6, "x": 7,
    "c": 8, "v": 9, "b": 11, "q": 12, "w": 13, "e": 14, "r": 15,
    "y": 16, "t": 17, "1": 18, "2": 19, "3": 20, "4": 21, "6": 22,
    "5": 23, "=": 24, "9": 25, "7": 26, "-": 27, "8": 28, "0": 29,
    "]": 30, "o": 31, "u": 32, "[": 33, "i": 34, "p": 35, "l": 37,
    "j": 38, "'": 39, "k": 40, ";": 41, "\\": 42, ",": 43, "/": 44,
    "n": 45, "m": 46, ".": 47, "`": 50, " ": 49,
    "\n": 36,  # return
    "\t": 48,  # tab
}


def _char_to_keycode(char: str) -> int | None:
    """Map a single ASCII character to a macOS virtual keycode, or None."""
    if char in _KEYCODE_MAP:
        return _KEYCODE_MAP[char]
    # Handle uppercase by lowercasing for the lookup.
    low = char.lower()
    if low in _KEYCODE_MAP:
        return _KEYCODE_MAP[low]
    return None