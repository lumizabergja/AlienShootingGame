# NvInterp - External PID GPU shader frame interpolation

Experimental Windows x64 presenter built independently of NVIDIA DLSS-G.
This project reuses only the known working NvFBC CUDA PID capture adapter from the prior V3; it does not use Streamline or any NvOFA pass.

## Run
1. Extract the ZIP to a folder.
2. Launch your game in borderless/windowed mode, and set its real frame-rate cap.
3. Launch NvInterp.exe, select the game window, choose 2x/3x/4x etc. and 4x4 motion resolution.
4. Press Start. The app brings the game to the foreground and presents to a topmost layered click-through window.
5. On your 240 Hz monitor, start with 60 real FPS -> 4x at 50% motion, then compare Native.
6. Read NvInterp-Diagnostic.log to see Input FPS, Submitted FPS, and submitted ratio.

## Architecture
NvFBC CUDA PID frame -> imported D3D12 buffer -> hierarchical 4x4 coarse/medium/fine motion estimation -> GPU dense motion field -> DX12 fullscreen interpolation shader -> paced Present() for real and synthetic frames. Shader extrapolates current pixels using previous-to-current movement estimates, avoiding a full source-frame lookahead delay.

## Limitations
- The extra frames are shader-warp extrapolations, NOT NVIDIA DLSS-G or Preset C.
- The submitted ratio reports Present calls, not physically displayed monitor frames. DWM/compositor may drop frames. Verify by visual test or high-speed camera.
- Fast movement, disocclusion, HUD elements and thin lines can have artifacts, particularly at 10% motion resolution.
- Nvidia's private NvFBC PID compatibility can stop working with a different driver.
- Windows GitHub CI can compile the EXE and HLSL, but cannot validate capture, click-through, or 240Hz timing on a real RTX GPU.
