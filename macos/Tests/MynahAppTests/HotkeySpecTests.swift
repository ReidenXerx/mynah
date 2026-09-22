// The hotkey spec parser: pynput syntax, shared with the config file the
// Python CLI and the Linux side write. A spec that parses wrongly registers
// the wrong key — or none, and the app looks dead.

import Carbon.HIToolbox
import Testing

@testable import MynahApp

@Suite("HotkeySpec")
struct HotkeySpecTests {

    @Test("the default spec parses, with '.' as a literal character")
    func defaultSpec() throws {
        // '.' is NOT a named key: pynput's own parser rejects <period>, so
        // the config carries a bare dot and this has to accept it.
        let combo = try #require(HotkeySpec.parse("<cmd>+<shift>+."))
        #expect(combo.modifiers == UInt32(cmdKey) | UInt32(shiftKey))
        #expect(combo.keyCode == 47) // kVK_ANSI_Period
    }

    @Test("function keys, named keys and modifier spellings")
    func variations() throws {
        #expect(try #require(HotkeySpec.parse("<f8>")).keyCode == 100)
        #expect(try #require(HotkeySpec.parse("<ctrl>+<space>")).keyCode == 49)
        // Both spellings of every modifier reach the same mask.
        let long = try #require(HotkeySpec.parse("<command>+<control>+<option>+a"))
        let short = try #require(HotkeySpec.parse("<cmd>+<ctrl>+<alt>+a"))
        #expect(long.modifiers == short.modifiers)
        #expect(long.keyCode == short.keyCode)
        // Case and spacing are not significant.
        #expect(try #require(HotkeySpec.parse(" <CMD> + <Shift> + . ")).keyCode == 47)
    }

    @Test("a spec with no key, or an unknown key, is refused")
    func refusals() {
        #expect(HotkeySpec.parse("") == nil)
        #expect(HotkeySpec.parse("<cmd>+<shift>") == nil)   // modifiers only
        #expect(HotkeySpec.parse("<cmd>+<nosuchkey>") == nil)
    }

    @Test("<period> is accepted here, though pynput only takes a bare '.'")
    func namedPeriodIsLenient() throws {
        // Worth pinning rather than assuming: the config file is shared with
        // the Python CLI, whose parser rejects <period>. This one takes both
        // spellings and maps them to the same key, so a spec written here
        // still has to be written as "." to work on the other side.
        let named = try #require(HotkeySpec.parse("<cmd>+<period>"))
        let bare = try #require(HotkeySpec.parse("<cmd>+."))
        #expect(named.keyCode == bare.keyCode)
        #expect(named.modifiers == bare.modifiers)
    }
}
