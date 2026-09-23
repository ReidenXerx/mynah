#include "paste_typer.hpp"

namespace mynah::inject {

namespace {

constexpr Selection kBoth[] = {Selection::Clipboard, Selection::Primary};

} // namespace

PasteTyper::PasteTyper(std::unique_ptr<SelectionBackend> backend,
                       std::chrono::milliseconds restore_after)
    : backend_(std::move(backend)), restore_after_(restore_after) {}

PasteTyper::~PasteTyper() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    if (restorer_.joinable()) restorer_.join();
}

bool PasteTyper::type_text(const std::string& text) {
    if (text.empty()) return true;
    // One paste at a time, and never across a restore: a restore half done
    // when the next utterance saves "the user's content" would save ours.
    std::lock_guard<std::mutex> op(op_mutex_);

    // Save what the user has — once per burst (see the header).
    std::optional<SelectionSnapshot> snapshots[2];
    bool taken[2] = {false, false};
    for (Selection which : kBoth) {
        const int i = int(which);
        bool pending;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending = saved_[i].pending;
        }
        if (!pending) {
            snapshots[i] = backend_->snapshot(which);
            taken[i] = true;
        }
    }

    // The clipboard is the one that must work; the primary selection is for
    // terminals and is best-effort (not every compositor has one).
    if (!backend_->set_text(Selection::Clipboard, text)) return false;
    const bool primary_set = backend_->set_text(Selection::Primary, text);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Selection which : kBoth) {
            const int i = int(which);
            if (which == Selection::Primary && !primary_set) continue;
            if (taken[i] && !saved_[i].pending) {
                saved_[i].pending = true;
                saved_[i].content = std::move(snapshots[i]);
            }
        }
        restore_at_ = std::chrono::steady_clock::now() + restore_after_;
        if (!restorer_.joinable()) restorer_ = std::thread([this] { restore_loop(); });
    }
    wake_.notify_all();

    // The text is on the clipboard whether or not the chord lands; the
    // restore is scheduled either way.
    return backend_->paste_chord();
}

void PasteTyper::restore_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        const bool any = saved_[0].pending || saved_[1].pending;
        if (!any) {
            if (stopping_) return;
            wake_.wait(lock);
            continue;
        }
        // Even when stopping: an app may still be reading the paste.
        if (std::chrono::steady_clock::now() < restore_at_) {
            wake_.wait_until(lock, restore_at_);
            continue;
        }
        // Lock order is op_mutex_ then mutex_, as in type_text.
        lock.unlock();
        std::lock_guard<std::mutex> op(op_mutex_);
        lock.lock();
        if (std::chrono::steady_clock::now() < restore_at_) continue; // a paste moved it
        Saved due[2] = {std::move(saved_[0]), std::move(saved_[1])};
        saved_[0] = Saved{};
        saved_[1] = Saved{};
        lock.unlock();
        for (Selection which : kBoth) {
            const Saved& one = due[int(which)];
            if (one.pending && one.content) backend_->restore(which, *one.content);
        }
        lock.lock();
    }
}

} // namespace mynah::inject
