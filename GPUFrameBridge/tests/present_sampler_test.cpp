#include "present_sampler.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>

using gpu_frame_bridge::PresentSampler;

static int countSelected(double source_fps, double capture_fps, double seconds) {
    PresentSampler sampler(capture_fps);
    int selected = 0;
    const int total = static_cast<int>(source_fps * seconds);
    for (int i = 0; i < total; ++i) {
        // rounded timestamps emulate actual game Presents, rather than an
        // independent screenshot timer.
        const auto ts = static_cast<std::uint64_t>(
            static_cast<double>(i) * 1'000'000'000.0 / source_fps + 0.5);
        selected += sampler.shouldCopy(ts);
    }
    return selected;
}

int main() {
    assert(countSelected(60.0, 60.0, 1.0) == 60);
    assert(countSelected(120.0, 60.0, 1.0) == 60);
    assert(countSelected(120.0, 120.0, 1.0) == 120);
    assert(countSelected(240.0, 60.0, 1.0) == 60);
    assert(countSelected(30.0, 60.0, 1.0) == 30); // never invent frames
    assert(countSelected(240.0, 0.0, 1.0) == 240); // uncapped mode

    PresentSampler sampler(60.0);
    assert(sampler.shouldCopy(0));
    assert(!sampler.shouldCopy(8'000'000));
    assert(sampler.shouldCopy(17'000'000));
    assert(sampler.shouldCopy(1'000'000'000)); // no replay of missed slots
    assert(!sampler.shouldCopy(1'001'000'000));
    sampler.setCaptureFps(120.0);
    assert(sampler.shouldCopy(1'002'000'000)); // hot switching resets cadence

    bool rejected = false;
    try { sampler.setCaptureFps(-1.0); }
    catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);

    std::cout << "PresentSampler tests passed (60x4, 120x2, jitter/cadence)\n";
}
