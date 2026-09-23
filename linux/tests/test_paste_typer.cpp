// PasteTyper — the paste-and-restore policy behind typing on KWin — driven
// with a fake backend that records every operation and plays a clipboard.
// The Wayland half (kwin_backend.cpp) needs a live KWin and is checked by
// hand (docs/LINUX-KDE-PLAN.md, Stage 3).

#include "vendor/doctest.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "paste_typer.hpp"

using mynah::inject::PasteTyper;
using mynah::inject::Selection;
using mynah::inject::SelectionBackend;
using mynah::inject::SelectionSnapshot;
using namespace std::chrono_literals;

namespace {

SelectionSnapshot text_snapshot(const std::string& text) {
    return SelectionSnapshot{{{"text/plain;charset=utf-8", text}}};
}

// A clipboard and a primary selection, plus a log of what was done to them.
struct FakeState {
    std::mutex mutex;
    SelectionSnapshot selection[2];
    bool unsaveable[2] = {false, false}; // snapshot() answers nullopt
    bool set_fails[2] = {false, false};
    bool chord_fails = false;
    std::vector<std::string> log;

    std::string text(Selection which) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto& formats = selection[int(which)].formats;
        return formats.empty() ? std::string() : formats.front().second;
    }
    std::vector<std::string> take_log() {
        std::lock_guard<std::mutex> lock(mutex);
        return log;
    }
};

class FakeBackend final : public SelectionBackend {
public:
    explicit FakeBackend(std::shared_ptr<FakeState> state) : state_(std::move(state)) {}

    std::optional<SelectionSnapshot> snapshot(Selection which) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->log.push_back(std::string("snapshot:") + name(which));
        if (state_->unsaveable[int(which)]) return std::nullopt;
        return state_->selection[int(which)];
    }
    bool set_text(Selection which, const std::string& text) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->log.push_back(std::string("set:") + name(which) + ":" + text);
        if (state_->set_fails[int(which)]) return false;
        state_->selection[int(which)] = text_snapshot(text);
        return true;
    }
    bool restore(Selection which, const SelectionSnapshot& content) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->log.push_back(std::string("restore:") + name(which));
        state_->selection[int(which)] = content;
        return true;
    }
    bool paste_chord() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->log.push_back("chord");
        return !state_->chord_fails;
    }
    std::pair<bool, std::string> check() const override { return {true, ""}; }

private:
    static const char* name(Selection which) {
        return which == Selection::Clipboard ? "clipboard" : "primary";
    }
    std::shared_ptr<FakeState> state_;
};

struct Rig {
    std::shared_ptr<FakeState> state = std::make_shared<FakeState>();
    std::unique_ptr<PasteTyper> typer =
        std::make_unique<PasteTyper>(std::make_unique<FakeBackend>(state), 100ms);

    // Waits for the restore (100 ms here) to have happened.
    bool restored_to(Selection which, const std::string& text) {
        for (int i = 0; i < 100; ++i) {
            if (state->text(which) == text) return true;
            std::this_thread::sleep_for(10ms);
        }
        return false;
    }
};

int count(const std::vector<std::string>& log, const std::string& entry) {
    int n = 0;
    for (const auto& line : log) n += line == entry;
    return n;
}

} // namespace

TEST_CASE("a paste puts the text on both selections, presses the chord, then gives both back") {
    Rig rig;
    rig.state->selection[0] = text_snapshot("the user's clipboard");
    rig.state->selection[1] = text_snapshot("the user's selection");

    REQUIRE(rig.typer->type_text("привет"));
    auto log = rig.state->take_log();
    // Saved first, then ours on both, then the chord — in that order.
    REQUIRE(log.size() >= 5);
    CHECK(log[0] == "snapshot:clipboard");
    CHECK(log[1] == "snapshot:primary");
    CHECK(log[2] == "set:clipboard:привет");
    CHECK(log[3] == "set:primary:привет");
    CHECK(log[4] == "chord");

    CHECK(rig.restored_to(Selection::Clipboard, "the user's clipboard"));
    CHECK(rig.restored_to(Selection::Primary, "the user's selection"));
}

TEST_CASE("two utterances in the window: the user's content is saved once, restored once") {
    Rig rig;
    rig.state->selection[0] = text_snapshot("the user's clipboard");
    REQUIRE(rig.typer->type_text("первое"));
    REQUIRE(rig.typer->type_text("второе")); // finds OUR text on the clipboard
    CHECK(rig.state->text(Selection::Clipboard) == "второе");
    CHECK(rig.restored_to(Selection::Clipboard, "the user's clipboard"));
    std::this_thread::sleep_for(150ms);
    auto log = rig.state->take_log();
    CHECK(count(log, "snapshot:clipboard") == 1);
    CHECK(count(log, "restore:clipboard") == 1);
}

TEST_CASE("an empty selection is restored to empty, not left holding dictation") {
    Rig rig; // both selections start empty
    REQUIRE(rig.typer->type_text("secret words"));
    CHECK(rig.restored_to(Selection::Clipboard, ""));
    CHECK(rig.restored_to(Selection::Primary, ""));
}

TEST_CASE("a selection that cannot be saved is left alone, never restored to something else") {
    Rig rig;
    rig.state->selection[0] = text_snapshot("a 200 MB copied file");
    rig.state->unsaveable[0] = true;
    rig.state->selection[1] = text_snapshot("the user's selection");
    REQUIRE(rig.typer->type_text("привет"));
    CHECK(rig.restored_to(Selection::Primary, "the user's selection"));
    std::this_thread::sleep_for(150ms);
    CHECK(count(rig.state->take_log(), "restore:clipboard") == 0);
    CHECK(rig.state->text(Selection::Clipboard) == "привет");
}

TEST_CASE("every format of the user's content comes back, not just the text") {
    Rig rig;
    SelectionSnapshot image{{{"image/png", std::string("\x89PNG\r\n", 6)},
                             {"text/uri-list", "file:///tmp/shot.png"}}};
    rig.state->selection[0] = image;
    REQUIRE(rig.typer->type_text("привет"));
    // Read under the fake's lock: the restore writes it from the typer's thread.
    auto holds_image = [&] {
        std::lock_guard<std::mutex> lock(rig.state->mutex);
        return rig.state->selection[0] == image;
    };
    for (int i = 0; i < 100 && !holds_image(); ++i) std::this_thread::sleep_for(10ms);
    CHECK(holds_image());
}

TEST_CASE("no clipboard, no paste: a failed set types nothing and reports it") {
    Rig rig;
    rig.state->set_fails[0] = true;
    CHECK_FALSE(rig.typer->type_text("привет"));
    CHECK(count(rig.state->take_log(), "chord") == 0);
}

TEST_CASE("no primary selection is not fatal: the clipboard alone still pastes") {
    Rig rig;
    rig.state->set_fails[1] = true;
    rig.state->selection[0] = text_snapshot("the user's clipboard");
    REQUIRE(rig.typer->type_text("привет"));
    CHECK(rig.restored_to(Selection::Clipboard, "the user's clipboard"));
    std::this_thread::sleep_for(150ms);
    CHECK(count(rig.state->take_log(), "restore:primary") == 0);
}

TEST_CASE("a failed chord is reported, and the clipboard still comes back") {
    Rig rig;
    rig.state->chord_fails = true;
    rig.state->selection[0] = text_snapshot("the user's clipboard");
    CHECK_FALSE(rig.typer->type_text("привет"));
    CHECK(rig.restored_to(Selection::Clipboard, "the user's clipboard"));
}

TEST_CASE("quitting right after a paste still gives the clipboard back") {
    Rig rig;
    rig.state->selection[0] = text_snapshot("the user's clipboard");
    REQUIRE(rig.typer->type_text("привет"));
    rig.typer.reset(); // waits for the pending restore
    CHECK(rig.state->text(Selection::Clipboard) == "the user's clipboard");
}

TEST_CASE("an empty text touches nothing") {
    Rig rig;
    CHECK(rig.typer->type_text(""));
    CHECK(rig.state->take_log().empty());
}
