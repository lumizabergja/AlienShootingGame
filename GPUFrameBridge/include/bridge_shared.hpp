#pragma once

// Win32 shared-memory ABI used by the controller and Vulkan-layer process.
// These values are configuration requests, not evidence that FG is active.
#if !defined(_WIN32)
#error bridge_shared.hpp is only supported on Windows.
#endif

#include <windows.h>

namespace gpu_frame_bridge {

inline constexpr wchar_t kSettingsName[] = L"Local\\VkFrameBridge.Settings.v1";
inline constexpr LONG kSettingsMagic = 0x56464231; // VFB1

struct alignas(4) SharedSettings {
    volatile LONG magic;
    volatile LONG capture_fps_milli; // 0 = every new real present; 1..1000000 = 0.001..1000 FPS
    volatile LONG fg_multiplier;     // 0 = off, otherwise 2..6 (request only)
    volatile LONG revision;
};

inline LONG atomicRead(volatile LONG* value) {
    return InterlockedCompareExchange(value, 0, 0);
}

} // namespace gpu_frame_bridge
