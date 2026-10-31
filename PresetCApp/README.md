# Preset C v3 — Independent Flip Candidate (No Native Center Hole)

This is a separate experiment built from the supplied working v3 source.
The original v3 ZIP and other branches are unchanged.

## Preserved features
- NvFBC PID game capture and direct shared CUDA/D3D12 texture path.
- NvOFA 4x4 FAST and 50%, 25%, 10% resolution choices.
- DLSS-G Preset C, FG Off–6x, frame telemetry, focus/Alt+Tab.
- DX12 flip-discard swapchain, tearing support, max frame latency 1.
- Auto-rebuild on monitor/resolution changes.

## Independent Flip candidate changes
- Entire native center-hole/window-region bypass removed from the UI and output.
- No WS_EX_LAYERED or SetLayeredWindowAttributes on output.
- Full-monitor borderless output uses WS_DISABLED for cross-process mouse hit-test avoidance and WS_EX_NOACTIVATE.
- The v3 WDA_EXCLUDEFROMCAPTURE guard stays enabled to reduce NvFBC feedback risk.
- DXGI output hardware-composition capability is logged (capability is **not** proof of active Independent Flip).

## Run
1. Extract the whole ZIP into one folder; keep all DLLs beside FrameGeneration.exe.
2. Start FrameGeneration.exe. Select game, FG multiplier, and NvOFA options.
3. Start with a borderless game. The center hole is deliberately gone.
4. Ctrl+Alt+F12 stops the session if the output/input behaves unexpectedly.
5. Keep the original v3 as a fallback.

## Verify actual Windows present mode
Run Intel PresentMon and monitor FrameGeneration.exe while FG is active.
- Hardware: Independent Flip — direct scanout active.
- Hardware Composed: Independent Flip — hardware overlay path active.
- Composed: Flip — DXGI flip model, but not Independent Flip.

Windows/driver choose the mode dynamically. There is **no guaranteed Independent Flip** flag.
Windows capture exclusion (WDA_EXCLUDEFROMCAPTURE) relies on DWM and may prevent
or interfere with the transition to Independent Flip. If your NvFBC image
feeds back or click-through fails, stop with Ctrl+Alt+F12 and return to v3.

## Verification limits
Compiled on GitHub's Windows x64 runner. It has no RTX 5060 Ti or a
240 Hz game display, so only on-device testing can confirm input pass-through,
NvFBC capture exclusion, frame generation, and Independent Flip activation.
