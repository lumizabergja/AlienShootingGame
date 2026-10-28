#pragma once

#include <cmath>
#include <cstdint>
#include <stdexcept>

// Event-driven frame selection for a Vulkan vkQueuePresentKHR interceptor.
// This class never duplicates a frame and never waits for a timer. It only
// decides whether a *new, real* submitted frame should be copied to a bridge.
namespace gpu_frame_bridge {

class PresentSampler final {
public:
    // Zero means uncapped. Positive values are a maximum sample rate.
    explicit PresentSampler(double capture_fps = 60.0) { setCaptureFps(capture_fps); }

    void setCaptureFps(double capture_fps) {
        if (!std::isfinite(capture_fps) || capture_fps < 0.0 || capture_fps > 1000.0) {
            throw std::invalid_argument("Capture FPS must be between 0 and 1000");
        }
        period_ns_ = capture_fps == 0.0
            ? 0
            : static_cast<std::uint64_t>(std::llround(1'000'000'000.0 / capture_fps));
        reset();
    }

    // Timestamp is from a monotonically increasing clock (QPC converted to ns).
    // Call once for each *distinct* game Present, after ensuring the frame is ready.
    bool shouldCopy(std::uint64_t present_time_ns) noexcept {
        if (period_ns_ == 0) return true;
        if (!initialized_ || present_time_ns < last_present_ns_) {
            initialized_ = true;
            last_present_ns_ = present_time_ns;
            next_deadline_ns_ = present_time_ns + period_ns_;
            return true;
        }

        last_present_ns_ = present_time_ns;
        // 1/8 period tolerance handles small vsync/present timestamp jitter.
        const auto early_tolerance = period_ns_ / 8;
        if (present_time_ns < next_deadline_ns_ &&
            next_deadline_ns_ - present_time_ns > early_tolerance) {
            return false;
        }

        // Advance past missed deadlines without generating or copying duplicates.
        const auto threshold = present_time_ns + early_tolerance;
        if (next_deadline_ns_ <= threshold) {
            const auto steps = 1 + (threshold - next_deadline_ns_) / period_ns_;
            next_deadline_ns_ += steps * period_ns_;
        } else {
            next_deadline_ns_ += period_ns_;
        }
        return true;
    }

    void reset() noexcept {
        initialized_ = false;
        last_present_ns_ = 0;
        next_deadline_ns_ = 0;
    }

    std::uint64_t periodNs() const noexcept { return period_ns_; }

private:
    std::uint64_t period_ns_ = 0;
    std::uint64_t last_present_ns_ = 0;
    std::uint64_t next_deadline_ns_ = 0;
    bool initialized_ = false;
};

} // namespace gpu_frame_bridge
