# Lossless Scaling → Vulkan WSI Presenter

Experimental Windows presenter that captures the active Lossless Scaling output without modifying or injecting into League or Lossless Scaling.

The path is:

**League → Lossless Scaling/LSFG → DXGI Desktop Duplication → Vulkan Win32 surface/swapchain → vkQueuePresentKHR → display**

This is real Vulkan WSI output. Lossless Scaling itself remains Direct3D; the separate final presenter is Vulkan.

## Run

1. Extract both EXE files into the same folder.
2. Put League in **Borderless** mode on the **primary monitor**.
3. Open Lossless Scaling and activate scaling/LSFG.
4. Run `WSICapturePresenter.exe`.
5. Press **F8** to close the presenter.
6. In NVIDIA App, enable Smooth Motion for `VulkanPresenterCore.exe`, not League or Lossless Scaling.

The launcher chooses the largest visible window owned by `LosslessScaling.exe`. The Vulkan window is click-through and requests Windows capture exclusion to prevent a feedback/mirror loop.

## Current prototype limits

- Captures DXGI output 0 (the primary display).
- CPU-copies captured BGRA frames into the Vulkan uploader, so it adds latency.
- Desktop Duplication captures the final desktop region; other overlays above Lossless Scaling may appear.
- Windows or the driver may refuse capture exclusion. If you see an infinite mirror, press F8.
- Smooth Motion detection is driver-dependent. A real Vulkan swapchain is created, but this cannot guarantee NVIDIA enables the feature.
- This does not hook or inject into League, Vanguard, or Lossless Scaling.

The Vulkan presentation core is derived from Neural Lens at pinned commit
`17aa7492c95e7b54222cde36f8c803811d361ec8` and is distributed under GPL-3.0. See `UPSTREAM-LICENSE`.
