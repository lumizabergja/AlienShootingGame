# FrameBridge Native — fresh external capture experiment

**Current milestone: Windows GPU capture/presentation plus a genuine NVIDIA Streamline SDK capability probe. Not yet a full frame generator or Reflex 2.**

The project contains entirely new code. It does not inject into or read another game's process memory. It uses Windows DXGI Desktop Duplication, copies the captured desktop image to a D3D11 GPU texture, and presents it through a flip-model swap chain. The previous experimental 2D warp has been **removed** because the requested feature is genuine NVIDIA Reflex 2 Frame Warp.

The optional `FrameBridgeNvidiaProbe.exe` uses **real NVIDIA Streamline SDK calls** to check DLSS-G support, initialize a D3D12 device, and query NVIDIA's own runtime MFG limits. This is not a mock SDK. It does **not** present generated frames. The capture presenter also does **not** include NVIDIA DLSS Frame Generation, and neither executable includes Reflex 2 Frame Warp.

## Why this milestone exists

Before integrating NVIDIA DLSS-G, we need a stable external source whose GPU ownership, frame age, copy cost, presentation cadence and self-capture behavior are measurable. Windows screen capture does not supply the original engine depth, dense object motion vectors, HUD-free scene or actual camera pose. DLSS-G can only receive reconstructed substitutes unless a game exports the true inputs.

The design goal for later milestones is **DX12 + Streamline DLSS-G + real Reflex Low Latency**, with genuine Frame Warp only once its SDK and camera inputs are available. This initial capture probe uses D3D11 because DXGI Desktop Duplication produces D3D11 textures. The NVIDIA capability probe uses the real D3D12 API; a combined D3D12 capture/presenter with actual generated frames remains to be built.

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

**Windows CI compilation is available through the accompanying GitHub Actions workflow; runtime success on the target GPU still must be tested.**

### Genuine NVIDIA SDK probe

The SDK probe requires the official NVIDIA **Streamline SDK 2.14.16**, which NVIDIA distributes on its GitHub releases page. Build with:

```powershell
cmake -S . -B build/win64 -G "Visual Studio 17 2022" -A x64 `
  -DFRAMEBRIDGE_BUILD_NVIDIA_PROBE=ON `
  -DFRAMEBRIDGE_SL_INCLUDE_DIR="C:/streamline/include" `
  -DFRAMEBRIDGE_SL_INTERPOSER_LIB="C:/streamline/lib/x64/sl.interposer.lib"
cmake --build build/win64 --config Release
```

Use the paths from your extracted SDK package. Official production `sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, and associated dependencies must be in the same directory as the probe executable. The GitHub workflow is responsible for downloading the official release and packaging these dependencies when possible.

Run `FrameBridgeNvidiaProbe.exe`, then send back `FrameBridgeNvidiaProbe.log`. It records actual Streamline initialization results, adapter support, SDK requirements, and the maximum MFG multiplier reported by NVIDIA. A successful probe still means **zero generated frames**; there is no host swap chain or frame tagging in this diagnostic executable.

## Using the capture probe

- Run `FrameBridgeNative.exe --windowed` first to test capture in a regular window.
- Without `--windowed`, the app uses a borderless topmost, click-through window over the first attached desktop output.
- The app requests Windows' `WDA_EXCLUDEFROMCAPTURE` policy for its own window. If unavailable, full-screen mode **exits** rather than proceed with known self-capture risk. Actual exclusion behavior should still be verified visually on the target Windows build.
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

The public NVIDIA Streamline sample currently treats `STREAMLINE_FEATURE_LATEWARP` and `STREAMLINE_FEATURE_DLSS_FG` as mutually exclusive, although NVIDIA has demonstrated both technologies together in native game integrations. The NVIDIA developer page asks developers to register for general Frame Warp integration availability. Consequently this program does not offer a substitute for the actual NVIDIA technology.

## Next implementation milestones

1. Verify NVIDIA SDK support and DXGI capture/self-exclusion on the target hardware; collect both logs.
2. Implement a DX12 presenter and GPU interop/synchronization for D3D11 capture textures.
3. Add optional NvOFA optical flow and explicitly labeled AI-derived depth and pose modules.
4. Implement NVIDIA Streamline DLSS-G with correct resource tagging, exact frame IDs, real Reflex Low Latency markers, and verified generated-frame counts.
5. Integrate **genuine NVIDIA Reflex 2 Frame Warp**, not custom image translation, when SDK access and accurate camera input are available.

Sources: [NVIDIA Streamline DLSS-G guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md), [NVIDIA Reflex SDK](https://developer.nvidia.com/performance-rendering-tools/reflex), [Microsoft DXGI Desktop Duplication](https://learn.microsoft.com/windows/win32/direct3ddxgi/desktop-dup-api), [Windows window capture exclusion](https://learn.microsoft.com/windows/win32/api/winuser/nf-winuser-setwindowdisplayaffinity).

