# DXGI Capture → Vulkan WSI Presenter

Experimental Windows presenter for League of Legends. It leaves the game process and files untouched.

The path is:

**League (borderless) → DXGI Desktop Duplication → Vulkan Win32 surface/swapchain → vkQueuePresentKHR → display**

This is real Vulkan WSI output. It is not WGC, DXVK, Special K, RenderDoc, or injection.

## Run

1. Extract both EXE files into the same folder.
2. Put League in **Borderless** mode on the **primary monitor**.
3. Start a match, then run `WSICapturePresenter.exe`.
4. Press **F8** to close the presenter.
5. In NVIDIA App, enable Smooth Motion for `VulkanPresenterCore.exe`, not League.

The Vulkan window is click-through so input continues to reach League.

## Current prototype limits

- Captures DXGI output 0 (the primary display).
- CPU-copies captured BGRA frames into the Vulkan uploader, so it adds latency. A future version should use shared GPU resources.
- Desktop Duplication captures the desktop region occupied by League, so overlays or windows placed above it can be captured too.
- Smooth Motion detection is driver-dependent; a real Vulkan swapchain is created, but this cannot guarantee that NVIDIA enables the feature.
- Use at your own risk. This does not hook or inject into League/Vanguard.

The Vulkan presentation core is derived from Neural Lens at pinned commit
`17aa7492c95e7b54222cde36f8c803811d361ec8` and is distributed under GPL-3.0. See `UPSTREAM-LICENSE`.
