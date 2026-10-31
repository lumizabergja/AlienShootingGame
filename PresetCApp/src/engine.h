#pragma once
#include "common.h"
#include <functional>

struct Options {
    UINT multiplier = 2;
    UINT centerRadius = 0; // Center-hole bypass removed in this Independent Flip test build.
    bool vsync = false;
    bool gpuProfile = false;
    bool opticalFlow = false; // Experimental NVOFA 4x4 FAST input; zero-motion fallback.
    UINT opticalFlowPercent = 50; // NVOFA input scale: 50, 25 or 10 percent of captured dimensions.
};

struct PerformanceSnapshot {
    std::atomic<uint64_t> version{0};
    std::atomic<uint32_t> baseFpsMilli{0}, presentFpsMilli{0};
    std::atomic<uint32_t> ageUs{0}, submitUs{0}, presentUs{0};
    std::atomic<uint64_t> dropped{0};
    std::atomic<uint32_t> dlssStatus{0}, outputWidth{0}, outputHeight{0}, centerRadius{0};
    std::atomic<bool> bridgeDirect{false};
};

struct SessionControl {
    std::atomic<bool> stop{false}, focused{false}, hasFrame{false}, perfActive{false}, resolutionChanged{false};
    std::mutex statusMutex;
    std::wstring status;
    PerformanceSnapshot perf;
    HMONITOR expectedMonitor{};
    RECT expectedMonitorRect{};
    void report(const std::wstring &s) {
        perfActive.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> l(statusMutex);
        status = s;
    }
};

void runPresenter(HWND target, HWND output, const Options &, SessionControl &);
