# FreshMotionFG — independent no-NvOFA prototype

An **experimental** Windows 11 x64 application that captures a selected game's
PID through legacy Windows NvFBC CUDA, derives motion vectors entirely on the GPU,
provides a conservative flat depth surface, and invokes **real NVIDIA DLSS-G**
through NVIDIA's public Streamline SDK.

The core shader, Streamline pipeline, and presenter are newly written. With the
user's permission, its NvFBC/CUDA PID capture module was adapted from the earlier
NvFBC source in the repository. This is **not** a patch to V39 itself and it is
not verified to be byte-identical to V39's capture implementation.

## What it is

- Capture: legacy **Windows NvFBC CUDA PID** capture with GPU-resident
  D3D12 shared raw buffers and a CUDA→D3D12 shared fence. HLSL reads the
  raw BGRA buffer directly without a copy-to-texture stage.
- 4:3 game frames: preserve NvFBC-reported game dimensions and pitch, then
  stretch to the monitor output (for example 1280×960 → 1920×1080).
- Motion: bidirectional image-to-image 4x4 block matching in HLSL, forward/
  backward consistency rejection, confidence weighting, and edge-aware
  expansion to a dense motion-vector surface.
- Depth: a coherent flat R32_FLOAT plane (not invented or game-engine depth).
- Frame generation: Streamline DLSS-G, with multipliers Off/2x/3x/4x/5x/6x
  when permitted by the hardware/driver and supporting Reflex.
- Output: separate DX12 DirectComposition display on a topmost click-through
  window. It uses Windows' documented WDA_EXCLUDEFROMCAPTURE setting to request
  self-exclusion; failure is reported as an error.
- No NvOFA SDK, no mouse-motion prediction, no game process injection,
  no game memory access, and no bypass/fallback if an API fails.

## Important caveats

1. **This is not V39.** The capture implementation is an earlier legacy NvFBC
   CUDA PID module reused with permission. NVIDIA does **not** officially
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
4. The DirectComposition presenter can be composited by Windows and may drop
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
5. Alt+Tab hides the presenter while another process is focused. If NvFBC's
   PID session invalidates or the capture resolution changes, the app reports
   the error; press Stop and restart. Automatic recovery is not yet validated.
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

## Verification target

Compilation and successful app launch are separate goals. On a real RTX GPU,
verify slDLSSGGetState, captured unique frame timestamps, present counts,
actual generated-frame display rate, and high-speed-camera input latency.
Claims of faster or native-quality motion are not justified until measured.
