#pragma once

// The DLSS-G Streamline options do not select a model preset. NVIDIA exposes
// Preset C through its driver settings (DRS) on compatible drivers instead.
struct PresetCResult {
    bool configured{};
    int error{};
    const wchar_t *stage{L"NVAPI unavailable"};
};

PresetCResult applyPresetCProfile();
