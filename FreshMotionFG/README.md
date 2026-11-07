# FreshMotionFG — V39-style mailbox, no-NvOFA experiment

## Startup fix (2026-11-07)

**Present crash fix:** The follow-up diagnostic log proved CUDA capture,
shared-buffer interop, shader compilation, and the first D3D12 frame submission
all succeeded, then Windows raised access violation `0xC0000005` inside the
first `Present` call. The app had been using a DirectComposition swapchain,
which was not among the CreateSwapChain hooks registered by Streamline DLSS-G.
This revision switches to `CreateSwapChainForHwnd` with a normal flip-discard
swapchain, the presentation path documented by NVIDIA for Streamline. The
output is a full-screen opaque mirror window with click-through styles, not
a transparency-composited DirectComposition surface. Runtime success and
click-through behavior still require testing on the user's Windows machine.

The first user log showed CUDA PID capture, CUDA/D3D12 sharing, and Streamline
initialization working, followed by four invalid Streamline camera/common
constants warnings. This build now explicitly supplies zero jitter, zero
camera-pinhole offset, zero camera position and non-inverted depth. These were
missing before; the change is not hardware-runtime-verified.

A new `FreshMotionFG-Diagnostic.log` is written next to the EXE. If Start closes
or fails, attach this file plus the `sl(...)` and `FrameGeneration-Capture(...)`
logs. A fatal C++ exception is also displayed instead of silently changing
capture or motion mode. If a GPU driver crash prevents it, the last startup
checkpoint remains in the diagnostic log.

An **experimental** Windows 11 x64 application that captures a selected game's
PID through legacy Windows NvFBC CUDA, derives motion vectors entirely on the GPU,
provides a conservative flat depth surface, and invokes **real NVIDIA DLSS-G**
through NVIDIA's public Streamline SDK.

The core shader, Streamline pipeline, and presenter are newly written. With the
user's permission, its NvFBC/CUDA PID capture and latest-mailbox ownership code
were adapted from earlier source in the repository. This is **not** a patch to
the supplied V39 executable; its original source was not in the ZIP.

## What it is

- Capture: legacy **Windows NvFBC CUDA PID** with GPU-resident shared raw
  buffers. A dedicated D3D12 copy queue waits on the CUDA fence, copies the
  native-resolution BGRA frame into a three-slot D3D12 color mailbox, and
  signals a separate GPU-ready fence. There is no CPU image readback.
- Timing: an independent capture producer fills a latest-frame mailbox while
  the presenter paces GPU work; obsolete unconsumed frames are dropped. The
  presenter latches the newest available image late, then uses the previous
  displayed source for optical-flow estimation.
- Focus: Alt+Tab pauses capture publication, drains base rendering, temporarily
  disables DLSS-G, clears old motion history, and resumes with a fresh frame.
- D3D12 Agility SDK: 1.620.1 (SDK version 620), matching the supplied V39
  bundle; its signed D3D12Core.dll must live in D3D12/ next to the EXE.
- 4:3 game frames: preserve NvFBC-reported game dimensions and pitch, then
  stretch to the monitor output (for example 1280×960 → 1920×1080).
- Motion: bidirectional image-to-image 4x4 block matching in HLSL, forward/
  backward consistency rejection, confidence weighting, and edge-aware
  expansion to a dense motion-vector surface.
- Depth: a coherent flat R32_FLOAT plane (not invented or game-engine depth).
- Frame generation: Streamline DLSS-G, with multipliers Off/2x/3x/4x/5x/6x
  when permitted by the hardware/driver and supporting Reflex.
- Output: DX12 HWND flip-discard swapchain on a topmost, non-activating,
  click-through mirror window. It uses WDA_EXCLUDEFROMCAPTURE to request
  self-exclusion; failure is reported as an error.
- No NvOFA SDK, no mouse-motion prediction, no game process injection,
  no game memory access, and no bypass/fallback if an API fails.

## Important caveats

1. **This is not the original V39 binary or source.** Its build documentation
   describes a similar three-slot CUDA→D3D12 copy architecture, but the exact
   V39 scheduling, pre-recorded copy commands, NvOFA coupling and swapchain
   behavior cannot be copied byte-for-byte from a compiled executable. The
   capture module is an earlier legacy NvFBC CUDA PID implementation reused
   with permission. NVIDIA does **not** officially
   support Windows NvFBC on current drivers. It may fail to initialize and
   reports an error; there is **no DXGI/WGC fallback**.
2. No external display-capture API exposes game-authored motion vectors,
   depth, camera transforms, HUD-less scene color, or UI alpha. All flow here
   is measured/approximated from two captured images. Native Warzone quality
   is **not guaranteed**. Thin edges, translucent effects, newly revealed
   regions, moving players and UI may remain difficult.
3. NVIDIA's Streamline library and GPU driver choose the active FG model.
   The application does **not** claim that Preset C is active without driver
   verification. Configure a supported application in NVIDIA App if available.
4. The HWND flip-model presenter can still be composited by Windows and may drop
   or pace generated frames. A frame multiplier is not proof that every frame
   reached the monitor.
5. Windows CI can compile and package the app, but cannot exercise NvFBC/DLSS-G on
   an RTX GPU, measure click-to-photon latency, verify self-capture exclusion,
   or test a game's anti-cheat compatibility.
6. Use only with games that permit external capture/presenter overlays.
   Some protected games may block capture or overlays; the app does not
   attempt to work around those restrictions.

## Test instructions

1. Extract **all** files. Do not run the EXE inside the ZIP.
2. Update the NVIDIA driver and enable Windows Hardware-Accelerated GPU
   Scheduling, as required by DLSS-G.
3. Run the game on the NVIDIA-connected monitor and select its game PID in the UI.
4. Start FreshMotionFG.exe, choose the game window, choose 2x/3x/4x and
   begin with **25% GPU motion analysis**. Use Start.
5. Alt+Tab pauses and hides the presenter, resets frame history, then resumes
   capture/DLSS-G when the selected game regains focus. CUDA PID sessions that
   invalidate are recreated using the same interface. Resolution changes still
   require Stop and Start. **Real Alt+Tab recovery is unverified until tested.**
6. Compare against your original V39 separately. Test a moving camera, moving
   enemies, UI stability, true present rate, and click-to-photon latency.

The implementation intentionally fails with a visible diagnostic if GPU
NvFBC capture, CUDA–D3D12 shared interop, DLSS-G, or self-exclusion is unavailable.

## Building on Windows

Build with Visual Studio 2022 and the Windows 11 SDK. Clone NVIDIA's official
Streamline v2.14.16 repository, then run:

    cmake -S . -B build -A x64 -DSTREAMLINE_INCLUDE="PATH_TO_STREAMLINE/include"
    cmake --build build --config Release

Place the official signed Streamline x64 runtime DLLs from NVIDIA's SDK release
beside the executable, including nvngx_dlssg.dll, sl.interposer.dll,
sl.common.dll, sl.dlss_g.dll, sl.reflex.dll, and sl.pcl.dll.
Keep shaders/motion.hlsl next to the executable as motion.hlsl.
Place the Microsoft D3D12 Agility SDK 1.620.1 x64 D3D12Core.dll in D3D12/.

## Verification target

Compilation and successful app launch are separate goals. On a real RTX GPU,
verify slDLSSGGetState, captured unique frame timestamps, present counts,
actual generated-frame display rate, and high-speed-camera input latency.
Claims of faster or native-quality motion are not justified until measured.
