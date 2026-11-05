# External NVIDIA DLSS Frame Generation experiment

This is a **new**, standalone Windows 11 x64 experiment that invokes
**NVIDIA NGX's real Frame Generation feature** from its own DX12 process.
It is not a replacement for in-game Streamline integration and may not
work with all NVIDIA drivers, games, or NVIDIA driver-selected presets.
It has not been validated on RTX hardware in CI (GitHub runners have no RTX GPU).

## What is implemented

- Native Windows game-window selection, including displayed PID
- DirectX Desktop Duplication (GPU texture) on an NVIDIA-connected monitor
- D3D11 shared NT handle, shared GPU fence, and D3D12 texture path
- Direct NGX DLSS-G evaluation using captured color, synthetic flat depth,
  approximate motion vectors, and identity camera transforms
- A DX12 click-through, non-activating topmost presentation window
- Automatic hiding when the game is not focused
- x2, x3, or x4 output selection **subject to the driver's reported MFG maximum**
- Three motion experiments: Zero, Global GPU estimate, Local GPU shader estimate

## Important limitations

1. **NVIDIA Optical Flow hardware input is not integrated.** The three available
   motion options are zero or shader-derived vectors. An NvOFA fourth option
   would require a separate SDK-backed implementation.
2. **Preset C is not directly selectable through the public NGX API used here.**
   This app requests genuine DLSS-G; the particular model/preset is chosen by
   the installed runtime and driver. Do not assume Preset C without verification.
3. **Depth and game motion vectors are not available externally.** Flat depth
   and estimated motion can produce ghosting, wobble, and flicker. Pixel-perfect
   native-quality frame generation cannot be promised.
4. **PID selects and tracks the game window**, but Desktop Duplication itself
   captures a GPU-connected display. This is not PID-scoped API capture.
5. Exclusive fullscreen games can cover any external HWND. Use borderless.
6. The first test version uses synchronous GPU fences for correctness, so
   latency is not yet optimized; NVIDIA Reflex is not integrated.
7. Frame counts and actual display present rate are not guaranteed to match
   the nominal x2/x3/x4 settings, especially with DWM or focus restrictions.
8. Anti-cheat compatibility is not guaranteed. Nothing here injects into games;
   test only in modes where the game's rules permit external overlay software.

## Build and run

Use Visual Studio 2022 on Windows 11 with the current Windows SDK:

```powershell
git clone https://github.com/NVIDIA/DLSS.git DLSS
cmake -S . -B build -A x64 -G "Visual Studio 17 2022" -D DLSS_SDK_DIR="$pwd/DLSS"
cmake --build build --config Release
```

Copy NVIDIA's official production `nvngx_dlssg.dll` next to
`ExternalDLSSG.exe`. Launch it, select a running game, choose a motion mode
and multiplier, then click Start. The selected game must be foreground.
Use Stop before changing modes.

## Development note

The separate `dlssg-worker.exe --probe` diagnostic confirms NGX availability
on a real RTX GPU and can be built with the same NVIDIA SDK. A successful CI
build does *not* establish that the live overlay is functional on your GPU.
