// mynah::inject::PasteTyper — typing by pasting, over any selection backend.
//
// On KWin there is no virtual keyboard to type through (wtype's protocol does
// not exist there), and turning text into key presses breaks on Cyrillic
// whenever the active layout is `us` — the default language is Russian. So
// text is pasted: it goes on the clipboard AND the primary selection, one
// chord pastes it (Shift+Insert: no letter key, so no layout can remap it;
// it pastes the clipboard in Qt, GTK, Chromium, Firefox and Electron, and
// the primary selection in terminals), and what the user had there before
// comes back a moment later.
//
// This class is the policy; SelectionBackend is the compositor. The policy
// is the part with the subtle rules, so it is the part the tests drive:
//   - the user's content is saved once per burst: a second utterance inside
//     the restore window finds OUR text on the clipboard, and saving that
//     would lose what the user had;
//   - every paste pushes the restore back, so no restore lands while an app
//     is still reading the paste;
//   - a selection that could not be saved (too large, unreadable) is left
//     with our text rather than "restored" to something else;
//   - an empty selection is restored to empty, not left holding dictation.

#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "injector.hpp"

namespace mynah::inject {

enum class Selection { Clipboard, Primary };

// A selection's content, every format the owner offered: (mime type, bytes).
// No formats means the selection was empty.
struct SelectionSnapshot {
    std::vector<std::pair<std::string, std::string>> formats;
    bool operator==(const SelectionSnapshot&) const = default;
};

class SelectionBackend {
public:
    virtual ~SelectionBackend() = default;

    // What the selection holds now; nullopt when it cannot be saved
    // faithfully (larger than the backend keeps, or the owner did not
    // answer) — the policy then leaves it alone.
    virtual std::optional<SelectionSnapshot> snapshot(Selection which) = 0;
    // Make `text` the selection, served as UTF-8 text.
    virtual bool set_text(Selection which, const std::string& text) = 0;
    // Put a snapshot back; an empty one clears the selection.
    virtual bool restore(Selection which, const SelectionSnapshot& content) = 0;
    // Press the paste chord into the focused window.
    virtual bool paste_chord() = 0;
    // (ok, remedy), as Injector::check.
    virtual std::pair<bool, std::string> check() const = 0;
};

class PasteTyper final : public Injector {
public:
    explicit PasteTyper(std::unique_ptr<SelectionBackend> backend,
                        std::chrono::milliseconds restore_after = std::chrono::milliseconds(800));
    // A restore still pending runs before this returns: quitting right after
    // dictating must not leave the dictated text where the user's was.
    ~PasteTyper() override;

    PasteTyper(const PasteTyper&) = delete;
    PasteTyper& operator=(const PasteTyper&) = delete;

    bool type_text(const std::string& text) override;
    std::pair<bool, std::string> check() const override { return backend_->check(); }

private:
    struct Saved {
        bool pending = false;                 // a restore is scheduled
        std::optional<SelectionSnapshot> content; // nullopt: leave it alone
    };

    void restore_loop();

    std::unique_ptr<SelectionBackend> backend_;
    const std::chrono::milliseconds restore_after_;

    std::mutex op_mutex_; // one paste or one restore at a time; taken before mutex_
    std::mutex mutex_;
    std::condition_variable wake_;
    Saved saved_[2]; // indexed by Selection
    std::chrono::steady_clock::time_point restore_at_;
    bool stopping_ = false;
    std::thread restorer_; // started on the first paste, joined by the destructor
};

} // namespace mynah::inject
