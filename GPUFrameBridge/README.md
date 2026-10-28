# GPU Frame Bridge — experimental Vulkan layer

This branch is an **intermediate diagnostic build**, not a finished frame-generation product.
It currently intercepts `vkQueuePresentKHR` through the official Vulkan loader layer
mechanism and selects which *real* presents would be sampled. It does not yet copy
or share a GPU texture, run NvOFA, invoke Streamline DLSS-G, or display generated frames.

## Controller

The Win32 `VkFrameBridgeController.exe` has an editable **Capture FPS** field.
It accepts decimal rates from `0` (every real present) through `1000`, for example
`60`, `80`, `112.5`, and `120`. Values higher than the game's actual base FPS
do not create new source frames. The Off/2x/3x/4x/5x/6x selector is recorded as
an **FG request only**. Nvidia optical flow 4x4 FAST at 10% is also a planned
fixed configuration, not yet active.

Start the controller, enter a rate, and press **Apply settings**. The Vulkan layer
polls a small shared-memory settings block on its real Present events and adjusts
its source-frame selection immediately. Close the controller to release the
control window; the layer may retain the last setting for its own process lifetime.

Logs:

- `%LOCALAPPDATA%\\VkFrameBridgeController.log` — GUI and settings.
- `%LOCALAPPDATA%\\VkFrameBridge.log` — Vulkan layer events and sampling counters.

## Build

The Windows x64 GitHub Actions workflow builds the layer DLL, the controller EXE,
and the portable scheduler test. The Vulkan-Headers package is fetched from
Khronos during CI. The initial Actions run failed **before the Windows runner
started**, so these Windows binaries have not yet been compiler-verified.

## Compatibility and risks

The layer sees only apps that use the Vulkan loader and explicitly enable the
layer. It cannot directly intercept Direct3D 11 games such as League of Legends.
In games with anti-cheat, including CS2, an external layer or injected component
can violate trusted-mode rules. Do not enable it in protected multiplayer sessions.
