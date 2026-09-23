#include "kwin_backend.hpp"

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <wayland-client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "ext-data-control-v1-client-protocol.h"
#include "fake-input-client-protocol.h"

namespace mynah::inject {

namespace {

// What a saved selection may hold: a copied screenshot fits, a copied
// video file's bytes do not — that one is left alone instead (the policy's
// nullopt).
constexpr std::size_t kMaxSnapshotBytes = 64u << 20;
// How long an owner gets to answer one format's read.
constexpr int kReadTimeoutMs = 1000;
// And a reader to take one of ours.
constexpr int kWriteTimeoutMs = 2000;

// Offered for dictated text: the forms Wayland, GTK, Qt and XWayland
// clients ask for.
const char* const kTextFormats[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING",
                                    "TEXT", "STRING"};

// Formats worth saving: MIME types, and the X11 names XWayland apps offer
// text under. Pseudo-targets like TARGETS or SAVE_TARGETS are not content.
bool worth_saving(const std::string& mime) {
    return mime.find('/') != std::string::npos || mime == "UTF8_STRING" || mime == "STRING" ||
           mime == "TEXT" || mime == "COMPOUND_TEXT";
}

std::string executable_path() {
    std::error_code ec;
    std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string("/usr/bin/mynah") : self.string();
}

class KWinBackend final : public SelectionBackend {
public:
    // Connects and reads the globals; nullptr-equivalent state is reported
    // by usable()/on_kwin() so the factory can decide.
    KWinBackend() {
        display_ = wl_display_connect(nullptr);
        if (!display_) return;
        registry_ = wl_display_get_registry(display_);
        wl_registry_add_listener(registry_, &kRegistryListener, this);
        wl_display_roundtrip(display_);
        if (fake_input_) org_kde_kwin_fake_input_authenticate(fake_input_, "mynah",
                                                                "type what you dictate");
        if (manager_ && seat_) {
            device_ = ext_data_control_manager_v1_get_data_device(manager_, seat_);
            ext_data_control_device_v1_add_listener(device_, &kDeviceListener, this);
        }
        wl_display_roundtrip(display_); // the current selections arrive
        wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        running_ = true;
        thread_ = std::thread([this] { loop(); });
    }

    ~KWinBackend() override {
        if (thread_.joinable()) {
            stop_ = true;
            poke();
            thread_.join();
        }
        for (auto& [source, owned] : sources_) ext_data_control_source_v1_destroy(source);
        for (auto& [offer, mimes] : offers_) ext_data_control_offer_v1_destroy(offer);
        if (device_) ext_data_control_device_v1_destroy(device_);
        if (manager_) ext_data_control_manager_v1_destroy(manager_);
        if (fake_input_) org_kde_kwin_fake_input_destroy(fake_input_);
        if (seat_) wl_seat_destroy(seat_);
        if (registry_) wl_registry_destroy(registry_);
        if (display_) wl_display_disconnect(display_);
        if (wake_fd_ >= 0) ::close(wake_fd_);
    }

    bool on_kwin() const { return on_kwin_; }
    bool connected() const { return display_ != nullptr; }

    std::optional<SelectionSnapshot> snapshot(Selection which) override {
        return call<std::optional<SelectionSnapshot>>(
            [&]() -> std::optional<SelectionSnapshot> { return snapshot_now(which); },
            std::nullopt);
    }

    bool set_text(Selection which, const std::string& text) override {
        SelectionSnapshot content;
        for (const char* format : kTextFormats) content.formats.emplace_back(format, text);
        return call<bool>([&] { return offer_now(which, content); }, false);
    }

    bool restore(Selection which, const SelectionSnapshot& content) override {
        return call<bool>([&] { return offer_now(which, content); }, false);
    }

    bool paste_chord() override {
        return call<bool>(
            [&] {
                if (!fake_input_) return false;
                // Shift+Insert: no letter key, so no layout remaps it.
                key(KEY_LEFTSHIFT, true);
                key(KEY_INSERT, true);
                key(KEY_INSERT, false);
                key(KEY_LEFTSHIFT, false);
                return wl_display_roundtrip(display_) >= 0;
            },
            false);
    }

    std::pair<bool, std::string> check() const override {
        if (!display_) return {false, "No Wayland display — mynah types only in a Wayland session."};
        if (!manager_)
            return {false,
                    "This KWin has no ext_data_control_v1, which mynah pastes through.\n"
                    "  It arrived in Plasma 6.3; update Plasma."};
        if (!fake_input_) {
            const std::string exe = executable_path();
            return {false,
                    "KWin has not let mynah press keys. It does that for programs whose\n"
                    "  .desktop file asks for it; the package installs one for /usr/bin/mynah.\n"
                    "  For " + exe + ":\n"
                    "  printf '[Desktop Entry]\\nType=Application\\nName=mynah\\nExec=" + exe +
                        "\\nNoDisplay=true\\nX-KDE-Wayland-Interfaces=org_kde_kwin_fake_input\\n' \\\n"
                        "    > ~/.local/share/applications/mynah-dev.desktop && kbuildsycoca6\n"
                        "  then restart mynah."};
        }
        return {true, ""};
    }

private:
    struct Owned {
        Selection which;
        SelectionSnapshot content;
    };

    // --- the event thread -------------------------------------------------------------

    // Run `job` on the event thread and wait for its result; `failed` when
    // the connection is gone.
    template <class R>
    R call(std::function<R()> job, R failed) {
        std::packaged_task<R()> task(std::move(job));
        std::future<R> result = task.get_future();
        {
            // Checked under the lock the loop clears it under: a job is
            // either queued before the loop's last run_jobs() or refused.
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            if (!running_) return failed;
            jobs_.push_back([&task] { task(); });
        }
        poke();
        return result.get();
    }

    void poke() {
        const std::uint64_t one = 1;
        [[maybe_unused]] ssize_t n = ::write(wake_fd_, &one, sizeof one);
    }

    void run_jobs() {
        std::deque<std::function<void()>> jobs;
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            jobs.swap(jobs_);
        }
        for (auto& job : jobs) job();
    }

    void loop() {
        const int display_fd = wl_display_get_fd(display_);
        while (!stop_) {
            while (wl_display_prepare_read(display_) != 0) wl_display_dispatch_pending(display_);
            wl_display_flush(display_);
            pollfd fds[2] = {{display_fd, POLLIN, 0}, {wake_fd_, POLLIN, 0}};
            if (::poll(fds, 2, -1) < 0) {
                wl_display_cancel_read(display_);
                continue;
            }
            if (fds[0].revents & POLLIN) {
                if (wl_display_read_events(display_) < 0) break; // the compositor is gone
            } else {
                wl_display_cancel_read(display_);
            }
            if (wl_display_dispatch_pending(display_) < 0) break;
            if (fds[1].revents & POLLIN) {
                std::uint64_t count;
                [[maybe_unused]] ssize_t n = ::read(wake_fd_, &count, sizeof count);
            }
            run_jobs();
        }
        {
            std::lock_guard<std::mutex> lock(jobs_mutex_);
            running_ = false;
        }
        run_jobs(); // what was queued runs; its Wayland calls fail harmlessly
    }

    // --- selections ---------------------------------------------------------------------

    ext_data_control_offer_v1* current(Selection which) const {
        return which == Selection::Clipboard ? clipboard_offer_ : primary_offer_;
    }

    std::optional<SelectionSnapshot> snapshot_now(Selection which) {
        // Ours: answer from memory. Reading our own offer over Wayland would
        // wait on a send() this very thread has to serve.
        if (auto* mine = owned_source(which)) return sources_[mine].content;
        ext_data_control_offer_v1* offer = current(which);
        SelectionSnapshot snap;
        if (!offer) return snap; // empty
        std::size_t total = 0;
        for (const std::string& mime : offers_[offer]) {
            if (!worth_saving(mime)) continue;
            std::optional<std::string> bytes = receive(offer, mime);
            if (!bytes) return std::nullopt; // the owner did not answer: leave it alone
            total += bytes->size();
            if (total > kMaxSnapshotBytes) return std::nullopt;
            snap.formats.emplace_back(mime, std::move(*bytes));
        }
        return snap;
    }

    std::optional<std::string> receive(ext_data_control_offer_v1* offer, const std::string& mime) {
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) != 0) return std::nullopt;
        ext_data_control_offer_v1_receive(offer, mime.c_str(), fds[1]);
        ::close(fds[1]);
        wl_display_flush(display_);
        std::string bytes;
        char buffer[65536];
        for (;;) {
            pollfd p{fds[0], POLLIN, 0};
            if (::poll(&p, 1, kReadTimeoutMs) <= 0) {
                ::close(fds[0]);
                return std::nullopt;
            }
            ssize_t n = ::read(fds[0], buffer, sizeof buffer);
            if (n < 0) {
                ::close(fds[0]);
                return std::nullopt;
            }
            if (n == 0) break;
            bytes.append(buffer, std::size_t(n));
            if (bytes.size() > kMaxSnapshotBytes) {
                ::close(fds[0]);
                return std::nullopt;
            }
        }
        ::close(fds[0]);
        return bytes;
    }

    ext_data_control_source_v1* owned_source(Selection which) {
        return which == Selection::Clipboard ? owned_clipboard_ : owned_primary_;
    }

    bool offer_now(Selection which, const SelectionSnapshot& content) {
        if (!device_) return false;
        ext_data_control_source_v1* source = nullptr;
        if (!content.formats.empty()) {
            source = ext_data_control_manager_v1_create_data_source(manager_);
            for (const auto& [mime, bytes] : content.formats)
                ext_data_control_source_v1_offer(source, mime.c_str());
            ext_data_control_source_v1_add_listener(source, &kSourceListener, this);
            sources_[source] = Owned{which, content};
        }
        if (which == Selection::Clipboard) {
            ext_data_control_device_v1_set_selection(device_, source); // null clears
            owned_clipboard_ = source;
        } else {
            ext_data_control_device_v1_set_primary_selection(device_, source);
            owned_primary_ = source;
        }
        // Processed before the chord: the app must see OUR offer when it pastes.
        return wl_display_roundtrip(display_) >= 0;
    }

    void serve(ext_data_control_source_v1* source, const char* mime, int fd) {
        auto it = sources_.find(source);
        if (it != sources_.end()) {
            for (const auto& [format, bytes] : it->second.content.formats) {
                if (format != mime) continue;
                std::size_t off = 0;
                while (off < bytes.size()) {
                    pollfd p{fd, POLLOUT, 0};
                    if (::poll(&p, 1, kWriteTimeoutMs) <= 0) break; // a reader that stalled
                    ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
                    if (n <= 0) break;
                    off += std::size_t(n);
                }
                break;
            }
        }
        ::close(fd);
    }

    void cancelled(ext_data_control_source_v1* source) {
        // Someone else owns the selection now; this source is done.
        if (owned_clipboard_ == source) owned_clipboard_ = nullptr;
        if (owned_primary_ == source) owned_primary_ = nullptr;
        sources_.erase(source);
        ext_data_control_source_v1_destroy(source);
    }

    void forget_offer(ext_data_control_offer_v1* offer) {
        if (!offer || offer == clipboard_offer_ || offer == primary_offer_) return;
        offers_.erase(offer);
        ext_data_control_offer_v1_destroy(offer);
    }

    void key(std::uint32_t code, bool down) {
        org_kde_kwin_fake_input_keyboard_key(fake_input_, code, down ? 1 : 0);
        wl_display_flush(display_);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    // --- listeners ----------------------------------------------------------------------

    static void on_global(void* data, wl_registry* registry, std::uint32_t name,
                          const char* interface, std::uint32_t version) {
        auto* self = static_cast<KWinBackend*>(data);
        const std::string iface = interface;
        // KWin-only globals. org_kde_* alone is not enough: Hyprland serves
        // org_kde_kwin_server_decoration_manager too.
        if (iface == "org_kde_plasma_shell" || iface == "kde_output_management_v2" ||
            iface == "org_kde_kwin_appmenu_manager")
            self->on_kwin_ = true;
        if (iface == "wl_seat" && !self->seat_) {
            self->seat_ = static_cast<wl_seat*>(
                wl_registry_bind(registry, name, &wl_seat_interface, 1));
        } else if (iface == "org_kde_kwin_fake_input" && version >= 4) { // keyboard_key: v4
            self->fake_input_ = static_cast<org_kde_kwin_fake_input*>(wl_registry_bind(
                registry, name, &org_kde_kwin_fake_input_interface, std::min(version, 4u)));
        } else if (iface == "ext_data_control_manager_v1") {
            self->manager_ = static_cast<ext_data_control_manager_v1*>(
                wl_registry_bind(registry, name, &ext_data_control_manager_v1_interface, 1));
        }
    }
    static void on_global_remove(void*, wl_registry*, std::uint32_t) {}

    static void on_offer_mime(void* data, ext_data_control_offer_v1* offer, const char* mime) {
        static_cast<KWinBackend*>(data)->offers_[offer].push_back(mime);
    }
    static void on_data_offer(void* data, ext_data_control_device_v1*,
                              ext_data_control_offer_v1* offer) {
        auto* self = static_cast<KWinBackend*>(data);
        self->offers_[offer]; // known, no formats yet
        ext_data_control_offer_v1_add_listener(offer, &kOfferListener, self);
    }
    static void on_selection(void* data, ext_data_control_device_v1*,
                             ext_data_control_offer_v1* offer) {
        auto* self = static_cast<KWinBackend*>(data);
        ext_data_control_offer_v1* previous = self->clipboard_offer_;
        self->clipboard_offer_ = offer;
        if (previous != offer) self->forget_offer(previous);
    }
    static void on_primary(void* data, ext_data_control_device_v1*,
                           ext_data_control_offer_v1* offer) {
        auto* self = static_cast<KWinBackend*>(data);
        ext_data_control_offer_v1* previous = self->primary_offer_;
        self->primary_offer_ = offer;
        if (previous != offer) self->forget_offer(previous);
    }
    static void on_finished(void* data, ext_data_control_device_v1* device) {
        auto* self = static_cast<KWinBackend*>(data);
        ext_data_control_device_v1_destroy(device);
        self->device_ = nullptr; // the seat went away
    }
    static void on_send(void* data, ext_data_control_source_v1* source, const char* mime,
                        std::int32_t fd) {
        static_cast<KWinBackend*>(data)->serve(source, mime, fd);
    }
    static void on_cancelled(void* data, ext_data_control_source_v1* source) {
        static_cast<KWinBackend*>(data)->cancelled(source);
    }

    static constexpr wl_registry_listener kRegistryListener = {on_global, on_global_remove};
    static constexpr ext_data_control_offer_v1_listener kOfferListener = {on_offer_mime};
    static constexpr ext_data_control_device_v1_listener kDeviceListener = {
        on_data_offer, on_selection, on_finished, on_primary};
    static constexpr ext_data_control_source_v1_listener kSourceListener = {on_send,
                                                                            on_cancelled};

    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_seat* seat_ = nullptr;
    org_kde_kwin_fake_input* fake_input_ = nullptr;
    ext_data_control_manager_v1* manager_ = nullptr;
    ext_data_control_device_v1* device_ = nullptr;
    bool on_kwin_ = false;

    // Event-thread state (and the constructor's, before the thread exists).
    std::map<ext_data_control_offer_v1*, std::vector<std::string>> offers_;
    ext_data_control_offer_v1* clipboard_offer_ = nullptr;
    ext_data_control_offer_v1* primary_offer_ = nullptr;
    std::map<ext_data_control_source_v1*, Owned> sources_;
    ext_data_control_source_v1* owned_clipboard_ = nullptr;
    ext_data_control_source_v1* owned_primary_ = nullptr;

    int wake_fd_ = -1;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::mutex jobs_mutex_;
    std::deque<std::function<void()>> jobs_;
    std::thread thread_;
};

} // namespace

std::unique_ptr<SelectionBackend> make_kwin_backend(std::string* why) {
    auto backend = std::make_unique<KWinBackend>();
    if (!backend->connected()) {
        if (why) *why = "no Wayland display";
        return nullptr;
    }
    if (!backend->on_kwin()) {
        if (why) *why = "the compositor is not KWin";
        return nullptr;
    }
    return backend;
}

std::unique_ptr<Injector> make_kwin(std::string* why) {
    std::unique_ptr<SelectionBackend> backend = make_kwin_backend(why);
    if (!backend) return nullptr;
    return std::make_unique<PasteTyper>(std::move(backend));
}

} // namespace mynah::inject
