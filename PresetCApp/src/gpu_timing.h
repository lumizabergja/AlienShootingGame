#pragma once
#include <cstdint>
struct GpuSample {
    uint64_t frame{};
    double color{}, joinGap{}, resolve{};
};
inline GpuSample decodeGpuTiming(const uint64_t *t, uint64_t frequency, uint64_t frame, bool motion) {
    if (!frame || !frequency || t[1] < t[0] ||
        (motion && (t[2] < t[1] || t[3] < t[2])))
        return {};
    double scale = 1000.0 / double(frequency);
    return {frame, double(t[1] - t[0]) * scale,
            motion ? double(t[2] - t[1]) * scale : 0,
            motion ? double(t[3] - t[2]) * scale : 0};
}
