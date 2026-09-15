# Vulkan Capture Prototype — Windows

Status: experimental; not runtime-tested on an NVIDIA Windows PC. A successful GitHub Actions build provides a Windows x64 EXE. If you downloaded the Actions artifact, extract it and run VulkanCapturePrototype.exe; no local compiler or Vulkan SDK is needed. Source downloads require the build steps below.

## What it does

Lists visible windows; captures the desktop monitor containing the selected window using DXGI Desktop Duplication; crops its client area; scales the pixels to 960×540; presents them in a separate, native NVIDIA Vulkan Win32 swapchain. No game DLL replacement, game memory access, process injection, driver installation, or anti-cheat bypass is implemented.

This captures visible desktop pixels, not an unobscured/private game surface. Any window covering the source appears in the capture.

## Build

1. Install Visual Studio 2022 Build Tools with **Desktop development with C++** and a Windows SDK.
2. Install the Windows x64 Vulkan SDK from https://vulkan.lunarg.com/sdk/home .
3. Open **x64 Native Tools Command Prompt for VS 2022**.
4. Change directory to this extracted folder and run `build.bat`.
5. A successful build produces `VulkanCapturePrototype.exe` beside the source.

The EXE needs the Vulkan runtime from your NVIDIA graphics driver. It does not require shader compilation.

## First test

1. Use a moving, ordinary window first, before testing any game.
2. Run the EXE and select that window's number from the console list.
3. Enter output desktop coordinates separated by a space. For a second monitor located to the right of a 1920×1080 main monitor, try `1920 100`. Adjust to your actual monitor layout.
4. The output is a fixed-size 960×540 borderless, topmost, click-through window. Source input continues to go to the original window.
5. Exit with **Ctrl+Alt+Q**, or close the console.

Use a real second monitor for testing with a full-size source. A non-overlapping side-by-side test on one monitor is possible if the source is small enough. Output must be visible on the desktop and must never cover the source. The application stops if they overlap to prevent recursive capture.

For League of Legends, the in-match source is normally `League of Legends`, not the Riot launcher. Keep it visible in windowed/borderless mode, entirely on the monitor where it was selected. Do not minimize it. Keep the original game focused.

## Smooth Motion experiment

If NVIDIA App lets you select this EXE and exposes Smooth Motion for it, enable it for `VulkanCapturePrototype.exe`, then restart the prototype. Do not assume it activates merely because Vulkan initialization succeeds. This package implements no frame generation itself and has no detector for driver-generated frames. Its console count is the application's captured/presented rate, not the final driver-generated display rate.

Driver recognition of a transfer-only presentation application is unverified. Vulkan presentation does not guarantee Smooth Motion support, better latency, or compatibility with any particular game/anti-cheat version. This is not a Riot-approved application, and it cannot promise zero ban risk.

## Deliberate prototype limits

- CPU readback, CPU nearest-neighbor scaling, and re-upload; these add work and latency. It is not zero-copy or optimized for competitive play.
- One frame at a time, with `vkQueueWaitIdle` for upload-buffer reuse.
- NVIDIA Vulkan device required; 8-bit UNORM swapchain required.
- Immediate presentation if supported, otherwise FIFO. Tearing is possible with immediate mode.
- SDR landscape displays only; HDR and rotated displays are outside this prototype's scope.
- Desktop pointer is not separately composited; it may be absent from the output.
- Display changes, capture access loss, or swapchain changes require restarting.
- No full-screen overlay over the original source on one monitor: DXGI desktop capture would see the overlay instead of the covered source. This package does not solve that problem.
- No input forwarding or duplicate game rendering. Watch the separate output while keeping the original source focused.

If a test fails, send the console error or compiler output. Do not place any of these files in the LoL installation directory.

API references:
https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api
https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdCopyBufferToImage.html
