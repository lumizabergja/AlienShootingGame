#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
// CPU ownership only; caller supplies GPU readiness. Never hold this mutex across an API call.
template <unsigned N> class LatestMailbox {
  public:
    enum class State { Free, Writing, Ready, Reading, History };
    struct Slot {
        State state{State::Free};
        uint64_t sequence{}, ready{}, release{}, epoch{};
        int64_t acquired{};
    };
    std::array<Slot, N> slots{};
    std::mutex mutex;
    std::atomic<bool> readyHint{false};
    std::atomic<uint64_t> dropped{0};

  private:
    void refreshReadyHintLocked() {
        bool ready = false;
        for (const auto &s : slots)
            if (s.state == State::Ready) {
                ready = true;
                break;
            }
        readyHint.store(ready, std::memory_order_release);
    }

  public:
    template <class Reusable> int reserve(Reusable reusable) {
        std::lock_guard<std::mutex> lock(mutex);
        int candidate = -1;
        for (unsigned i = 0; i < N; ++i)
            if (slots[i].state == State::Free && reusable(i, slots[i])) {
                candidate = int(i);
                break;
            }
        if (candidate < 0)
            for (unsigned i = 0; i < N; ++i)
                if (slots[i].state == State::Ready && reusable(i, slots[i]) &&
                    (candidate < 0 || slots[i].sequence < slots[candidate].sequence))
                    candidate = int(i);
        if (candidate >= 0) {
            if (slots[candidate].state == State::Ready)
                dropped.fetch_add(1, std::memory_order_relaxed);
            slots[candidate].state = State::Writing;
            refreshReadyHintLocked();
        } else
            dropped.fetch_add(1, std::memory_order_relaxed);
        return candidate;
    }
    void cancel(unsigned i) {
        std::lock_guard<std::mutex> lock(mutex);
        if(slots[i].state==State::Writing)slots[i].state=State::Free;
        refreshReadyHintLocked();
    }
    void publish(unsigned i, uint64_t sequence, uint64_t ready, uint64_t epoch, int64_t acquired) {
        std::lock_guard<std::mutex> lock(mutex);
        auto &s = slots[i];
        s.sequence = sequence;
        s.ready = ready;
        s.epoch = epoch;
        s.acquired = acquired;
        s.state = State::Ready;
        readyHint.store(true, std::memory_order_release);
    }
    bool available() const {
        return readyHint.load(std::memory_order_acquire);
    }
    int take(Slot *selected = nullptr) {
        std::lock_guard<std::mutex> lock(mutex);
        int best = -1;
        for (unsigned i = 0; i < N; ++i)
            if (slots[i].state == State::Ready && (best < 0 || slots[i].sequence > slots[best].sequence))
                best = int(i);
        if (best >= 0) {
            slots[best].state = State::Reading;
            if (selected)
                *selected = slots[best];
            for (unsigned i = 0; i < N; ++i)
                if (slots[i].state == State::Ready) {
                    slots[i].state = State::Free;
                    dropped.fetch_add(1, std::memory_order_relaxed);
                }
            readyHint.store(false, std::memory_order_release);
        }
        return best;
    }
    // Last-chance latch for latency-sensitive consumers.  The current Reading
    // slot has not been submitted to the GPU yet, so if the producer published a
    // newer frame while per-frame CPU setup was running we can discard the old
    // selection and switch ownership atomically.  This never touches History slots.
    bool retakeLatest(int &current, Slot *selected = nullptr) {
        std::lock_guard<std::mutex> lock(mutex);
        if (current < 0 || current >= int(N) || slots[current].state != State::Reading)
            return false;
        int best = -1;
        const uint64_t currentSequence = slots[current].sequence;
        for (unsigned i = 0; i < N; ++i)
            if (slots[i].state == State::Ready && slots[i].sequence > currentSequence &&
                (best < 0 || slots[i].sequence > slots[best].sequence))
                best = int(i);
        if (best < 0)
            return false;

        // The old Reading slot was never referenced by a submitted command list.
        slots[current].state = State::Free;
        dropped.fetch_add(1, std::memory_order_relaxed);
        slots[best].state = State::Reading;
        current = best;
        if (selected)
            *selected = slots[best];
        for (unsigned i = 0; i < N; ++i)
            if (slots[i].state == State::Ready) {
                slots[i].state = State::Free;
                dropped.fetch_add(1, std::memory_order_relaxed);
            }
        refreshReadyHintLocked();
        return true;
    }
    Slot snapshot(unsigned i) {
        std::lock_guard<std::mutex> lock(mutex);
        return slots[i];
    }
    void commit(unsigned current, int previous, uint64_t done) {
        std::lock_guard<std::mutex> lock(mutex);
        slots[current].state = State::History;
        slots[current].release = done;
        if (previous >= 0) {
            slots[previous].release = done;
            slots[previous].state = State::Free;
        }
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto &s : slots)
            if (s.state == State::Ready || s.state == State::History)
                s.state = State::Free;
        readyHint.store(false, std::memory_order_release);
    }
    uint64_t dropCount() const {
        return dropped.load(std::memory_order_relaxed);
    }
};
