# Preset C v3 — Reliable Click-Through (No Native Center Hole)

This is the corrected portable Windows build based on the working v3 source.
The previous non-layered Independent Flip experiment did not reliably pass mouse
input through to the game, so reliable input takes priority here.

## Unchanged
- NvFBC PID capture and shared CUDA/D3D12 texture path.
- NvOFA 4x4 FAST, with 50%, 25%, or 10% resolution options.
- NVIDIA DLSS-G Preset C, FG Off–6x, telemetry, focus/Alt+Tab handling.
- DX12 flip-discard presenter, tearing support, low-latency queue.

## Corrected output
- Fully opaque WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE overlay.
- Native center-hole and SetWindowRgn removed.
- No WS_DISABLED. Click-through is intended to work across game processes.
- Window capture exclusion remains enabled to avoid NvFBC feedback.

Because a layered window is used to restore reliable click-through, this
variant is not expected to engage true Independent Flip consistently.
Use PresentMon to check the actual Windows present mode if desired.

## Run
1. Extract the entire ZIP (all DLLs must stay beside FrameGeneration.exe).
2. Run FrameGeneration.exe, choose the game, FG multiplier, and NvOFA resolution.
3. Click Start with your game in borderless/windowed mode.
4. Move the mouse, click, and confirm the game receives input.
5. Ctrl+Alt+F12 to stop. Keep your original v3 as a fallback.

Windows x64 build verification runs in GitHub Actions. Runtime click-through and
frame-generation behavior still require a test on your RTX 5060 Ti.
