#pragma once
#include "common.h"
#include "gpu_timing.h"

// Optional bounded SPSC recorder used only when a diagnostic build enables it.
class CsvRecorder {
    struct Row {
        uint64_t frame, sequence, dropped;
        double age, submit, present;
        UINT presents, status;
        GpuSample gpu;
    };
    static constexpr uint64_t Capacity = 8192;
    std::array<Row, Capacity> rows{};
    alignas(64) std::atomic<uint64_t> written{0};
    alignas(64) std::atomic<uint64_t> read{0};
    std::atomic<bool> stop{false};
    uint64_t lost{};
    Event wake;
    std::ofstream file;
    std::thread thread;

  public:
    CsvRecorder() {
        wchar_t p[32768];
        GetModuleFileNameW(nullptr, p, 32768);
        file.open(std::filesystem::path(p).parent_path() / "FrameGeneration.csv");
        file << "base_frame,capture_sequence,dropped_captures,grab_return_to_select_ms,"
                "grab_return_to_submit_ms,present_cpu_ms,sdk_present_count,sdk_status,"
                "gpu_sample_frame,gpu_color_ms,gpu_join_gap_ms,gpu_resolve_ms\n";
        thread = std::thread([this] {
            for (;;) {
                auto end = written.load(std::memory_order_acquire);
                auto begin = read.load(std::memory_order_relaxed);
                while (begin < end) {
                    const auto &r = rows[begin % Capacity];
                    file << r.frame << ',' << r.sequence << ',' << r.dropped << ',' << r.age << ','
                         << r.submit << ',' << r.present << ',' << r.presents << ',' << r.status << ','
                         << r.gpu.frame << ',' << r.gpu.color << ',' << r.gpu.joinGap << ','
                         << r.gpu.resolve << '\n';
                    ++begin;
                }
                read.store(begin, std::memory_order_release);
                file.flush();
                if (stop.load(std::memory_order_acquire) &&
                    begin == written.load(std::memory_order_acquire))
                    break;
                WaitForSingleObject(wake.h, 100);
            }
        });
    }

    void push(uint64_t frame, uint64_t sequence, uint64_t dropped, double age, double submit,
              double present, UINT presents, UINT status, const GpuSample &gpu) {
        auto w = written.load(std::memory_order_relaxed);
        if (w - read.load(std::memory_order_acquire) >= Capacity) {
            ++lost;
            return;
        }
        rows[w % Capacity] = {frame, sequence, dropped, age, submit, present, presents, status, gpu};
        written.store(w + 1, std::memory_order_release);
    }

    ~CsvRecorder() {
        stop.store(true, std::memory_order_release);
        SetEvent(wake.h);
        if (thread.joinable())
            thread.join();
        if (lost)
            file << "# dropped_telemetry_rows=" << lost << '\n';
    }
};
