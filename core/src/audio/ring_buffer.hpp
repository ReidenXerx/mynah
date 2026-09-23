// mynah::audio — the audio-side plumbing every session runs on.
//
// The C API's threading contract starts here: mynah_push_audio is called
// from the front end's real-time capture thread and must never block and
// never allocate. It lands in the SPSC ring buffer below; the session's
// worker thread pops from it and runs everything that costs time.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace mynah::audio {

// A single-producer, single-consumer ring buffer of float samples.
//
// One writer thread (the capture thread via push), one reader thread (the
// session worker). Head/tail are atomics with acquire/release ordering —
// no locks on either side. Capacity is rounded up to a power of two so the
// wrap is a mask. If the writer laps the reader, the push drops what does
// not fit and counts it: a stalled worker must cost audio, never the
// capture thread.
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity_samples)
        : mask_(next_capacity(capacity_samples) - 1), data_(mask_ + 1) {}

    // Writer thread only. Returns how many samples were accepted; the rest
    // are dropped. Never blocks, never allocates.
    std::size_t push(const float* samples, std::size_t count) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t space =
            capacity() - readable_relaxed(head, tail_.load(std::memory_order_acquire));
        const std::size_t accepted = count < space ? count : space;
        if (accepted == 0) {
            dropped_.fetch_add(count, std::memory_order_relaxed);
            return 0;
        }
        const std::size_t start = head & mask_;
        const std::size_t first = mask_ + 1 - start < accepted ? mask_ + 1 - start : accepted;
        std::copy(samples, samples + first, data_.data() + start);
        std::copy(samples + first, samples + accepted, data_.data());
        head_.store(head + accepted, std::memory_order_release);
        if (accepted < count)
            dropped_.fetch_add(count - accepted, std::memory_order_relaxed);
        return accepted;
    }

    // Reader thread only. Pops up to count samples; returns how many.
    std::size_t pop(float* out, std::size_t count) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t available =
            readable_relaxed(head_.load(std::memory_order_acquire), tail);
        const std::size_t taken = count < available ? count : available;
        const std::size_t start = tail & mask_;
        const std::size_t first = mask_ + 1 - start < taken ? mask_ + 1 - start : taken;
        std::copy(data_.data() + start, data_.data() + start + first, out);
        std::copy(data_.data(), data_.data() + taken - first, out + first);
        tail_.store(tail + taken, std::memory_order_release);
        return taken;
    }

    // Where the writer is: the index the next pushed sample gets. Any
    // thread. A session records it when capture is armed, so its reader
    // can later skip whatever was written before (skip_to).
    std::size_t written() const { return head_.load(std::memory_order_acquire); }

    // Reader thread only. Discards everything before `position` (a value
    // written() returned) and nothing after what has been written; a
    // position already read past is a no-op. Returns how many samples were
    // discarded. The indices only grow (64-bit), so they compare directly.
    std::size_t skip_to(std::size_t position) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (position > head) position = head;
        if (position <= tail) return 0;
        tail_.store(position, std::memory_order_release);
        return position - tail;
    }

    std::size_t readable() const {
        return readable_relaxed(head_.load(std::memory_order_acquire),
                                tail_.load(std::memory_order_acquire));
    }

    std::size_t capacity() const { return mask_ + 1; }

    // Total samples dropped since construction (diagnostics; both threads).
    std::uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    static std::size_t next_capacity(std::size_t requested) {
        if (requested < 2) requested = 2;
        std::size_t cap = 1;
        while (cap < requested) cap <<= 1;
        return cap;
    }

    static std::size_t readable_relaxed(std::size_t head, std::size_t tail) {
        return head - tail; // power-of-two indices: wrap-safe
    }

    std::size_t mask_;
    std::vector<float> data_;
    // Unbounded monotonically increasing indices; the mask maps them into
    // the buffer, so head - tail is always the readable count.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

} // namespace mynah::audio