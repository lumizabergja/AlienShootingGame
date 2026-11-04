# NvShader — NvFBC CUDA / GPU Shader Motion / NVIDIA DLSS-G

Experimental Windows x64 presenter for NVIDIA RTX GPUs. This code begins a new DX12 presenter and GPU shader motion pipeline and reuses only the NvFBC CUDA capture adapter from the earlier verified V3 prototype.

## Running
1. Extract the complete ZIP (do not run the EXE from inside the archive).
2. Install a recent NVIDIA display driver, with NvFBC support already enabled on this machine.
3. In the NVIDIA App beta, add NvShader.exe as a program. Under Graphics > Driver Settings > DLSS Override - Model Presets, select Frame Generation **Preset C** and apply it. The app cannot guarantee or verify the active preset solely from a profile write.
4. Launch the game in borderless/windowed mode, launch NvShader.exe, select the actual game window.
5. Select Off or 2x–6x and Native/50%/25%/10% 4x4 shader motion, then Start. Alt-Tab should hide the presenter.
6. Stop before quitting.

## What it does
- NvFBC CUDA PID capture into D3D12-imported GPU buffers.
- GPU compute motion estimation using 4x4 block matching, no separate NvOFA API pass.
- Shader-generated dense motion-vector texture, conservative synthetic depth, D3D12 color decode.
- Streamline 2.14.16 DLSS-G, Reflex + PCL integration, 2-buffer flip-discard presenter.
- Click-through transparent topmost presenter; no injection into the game process.

## Important limits
- NVIDIA deprecated NvFBC on modern Windows; the inherited capture adapter must already work with your driver. Never assume it works on a new machine.
- NvFBC does not expose game-native depth, camera data, or a HUD-less frame. Native FG quality is **not guaranteed**. At 10% resolution, small moving detail is lost.
- GitHub-hosted Windows runners lack the RTX display hardware needed to verify actual DLSS-G activation, smoothness, mouse click-through, or frame pacing. A successful CI build proves compilation and packaging only.
- 4x4 motion estimation is the output grid at the chosen input scale. Frame-generation multiplication is subject to the hardware/runtime's reported maximum. Generated frames can be dropped by the Streamline pacer.
- NvFBC private PID mode only works when the capture adapter reports the selected game's PID; it will fail explicitly otherwise. No WGC/DXGI fallback is used.
