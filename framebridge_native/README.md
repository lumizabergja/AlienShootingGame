# FrameBridge Native — fresh external capture experiment

**Current milestone: Windows GPU capture/presentation probe, not a completed DLSS-G or Reflex 2 implementation.**

The project contains entirely new code. It does not inject into or read another game's process memory. It uses Windows DXGI Desktop Duplication, copies the captured desktop image to a D3D11 GPU texture, and presents it through a flip-model swap chain. A limited *flat 2D translation experiment* can be toggled while the presenter runs. This is **not** NVIDIA Frame Warp and is off by default.

The project currently does **not** include NVIDIA DLSS Frame Generation; it is also **not** using NVIDIA's Reflex 2 Frame Warp. An app log explicitly reports these facts. Do not use this milestone to claim generated frames are visible, or to compare "native DLSS-G" latency.

## Why this milestone exists

Before integrating NVIDIA DLSS-G, we need a stable external source whose GPU ownership, frame age, copy cost, presentation cadence and self-capture behavior are measurable. Windows screen capture does not supply the original engine depth, dense object motion vectors, HUD-free scene or actual camera pose. DLSS-G can only receive reconstructed substitutes unless a game exports the true inputs.

The design goal for later milestones is **DX12 + Streamline + Reflex Low Latency**, with a separate experimental warp stage. This initial capture probe uses D3D11 because DXGI Desktop Duplication produces D3D11 textures, and it lets us validate the external acquisition path first. An actual DX12 backend and NVIDIA integration remain work to be done; neither is silently substituted with a mock.

## Build on Windows 11

Requirements:

- Windows 10 2004+ or Windows 11, desktop session with DWM composition.
- Visual Studio 2022 with **Desktop development with C++** and Windows SDK 10/11.
- CMake 3.21+ (included in recent Visual Studio installations).
- A GPU supporting D3D11 and DXGI Desktop Duplication.

From PowerShell:

```powershell
./scripts/build-windows.ps1
```

This creates `build/win64/Release/FrameBridgeNative.exe` and a ZIP in `dist/` if compilation succeeds. Alternatively:

```powershell
cmake -S . -B build/win64 -G "Visual Studio 17 2022" -A x64
cmake --build build/win64 --config Release
```

**No Windows executable has been compiled or tested in the author's Linux development environment.** A GitHub Actions Windows build definition is included to provide a reproducible CI compile check when pushed to a repository.

## Using the capture probe

- Run `FrameBridgeNative.exe --windowed` first to test capture in a regular window.
- Without `--windowed`, the app uses a borderless topmost, click-through window over the first attached desktop output.
- The app requests Windows' `WDA_EXCLUDEFROMCAPTURE` policy for its own window. If unavailable, full-screen mode **exits** rather than proceed with known self-capture risk. Actual exclusion behavior should still be verified visually on the target Windows build.
- `Alt+F8`: turn the **experimental 2D translation** on/off. It is only an offset of the captured image, **not** depth-aware reprojection, Reflex 2, or an accurate camera model.
- `Alt+F9`: pause/resume the presenter.
- `Alt+F10`: exit.
- A console prints captured FPS, presented FPS and time since the latest acquired desktop image. Frame age is a QPC-derived diagnostic, **not** end-to-end input latency.

The app does not fall back to another capture API if duplication fails. On exclusive-fullscreen capture restrictions, monitor changes and unsupported systems it logs the failure. Display changes and loss of duplication trigger reacquisition. It does not currently crop or identify individual windows/PIDs and can include unrelated desktop content.

## Required data contract for native-quality NVIDIA FG and Frame Warp

| Input | Capture probe | Native-quality producer |
|---|---|---|
| Final color | Yes, GPU-resident | Renderer/capture |
| Depth | No | Game engine or separate depth estimation |
| Dense camera and object motion vectors | No | Game engine or optical flow estimate |
| Camera matrices and FOV | No | Game engine or video pose estimation |
| HUD-less color and UI alpha | No | Game engine or approximate segmentation |
| Frame IDs and exact in-engine render timestamps | No | Game engine instrumentation |
| Latest actual camera state | No | Game-specific camera/input integration |
| Real DLSS-G | No | NVIDIA Streamline SDK, correct inputs and presentation integration |
| Real Reflex 2 Frame Warp | No | NVIDIA Frame Warp SDK and game camera information |

The public NVIDIA Streamline sample currently treats `STREAMLINE_FEATURE_LATEWARP` and `STREAMLINE_FEATURE_DLSS_FG` as mutually exclusive. The NVIDIA developer page asks developers to register for general Frame Warp integration availability. Consequently we cannot truthfully label a custom 2D GPU warp as NVIDIA Reflex 2.

## Next implementation milestones

1. Verify DXGI capture/self-exclusion on the actual target hardware and collect logs.
2. Implement a DX12 presenter and GPU interop/synchronization for D3D11 capture textures.
3. Add optional NvOFA optical flow and explicitly labeled AI-derived depth and pose modules.
4. Implement NVIDIA Streamline DLSS-G with strict capability/input checks, verified generated-frame counts and real Reflex Low Latency markers.
5. Add a proper depth-aware custom reprojection stage, and separately integrate actual NVIDIA Frame Warp when its SDK is available.

Sources: [NVIDIA Streamline DLSS-G guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md), [NVIDIA Reflex SDK](https://developer.nvidia.com/performance-rendering-tools/reflex), [Microsoft DXGI Desktop Duplication](https://learn.microsoft.com/windows/win32/direct3ddxgi/desktop-dup-api), [Windows window capture exclusion](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity).

