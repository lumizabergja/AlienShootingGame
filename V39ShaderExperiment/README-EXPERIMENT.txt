V39 GPU Shader NVOFA-proxy experiment (source-first)
=================================================

This is an experimental add-on intended to run next to the untouched original
NvFBC-DX12-V39-4x3-Fix.exe. It is NOT a source-level modification of V39.

What it tries to do:
  * Normal mode: forward every NVIDIA Optical Flow API call to the genuine
    system32 nvofapi64.dll. Preserve original V39 behavior.
  * Shader mode: intercept the DirectX 12 optical-flow Execute call, read
    the two images V39 already registered with NVIDIA, and dispatch a
    newly written 4x4 image-matching shader. It writes NVOFA-compatible
    vector and cost textures to the buffers V39 already owns.
  * V39 keeps its NvFBC PID CUDA capture, Streamline/DLSS-G presenter,
    native 4:3 dimensions and all unrelated options.

Controls:
  Run V39 - NvOFA.cmd         Native NvOFA 4x4 FAST, confidence cost ON
  Run V39 - GPU Shader.cmd   Experimental GPU shader flow, confidence cost ON

The original EXE window will still say NvOFA. The launcher selects the
underlying backend via V39_MOTION for this experimental proxy.

Limitations and known risks:
  * There is no NVIDIA/RTX hardware in the Windows CI runner, so shader
    execution, NVFBC availability, frame pacing and latency cannot be
    validated there. Even a successful DLL build is not proof it works.
  * NVIDIA's API might bypass app-local DLL loading, in which case this
    proxy will have no effect. A driver-specific NvOFA resource state or
    format may also reject the shader path.
  * The shader estimates motion from pixel differences, not actual engine
    motion/depth. Thin edges, transparencies and HUD can flicker or wobble.
  * The proxy uses the genuine NVIDIA driver to register and initialize
    optical-flow resources; shader mode bypasses its execute operation,
    not all driver initialization. It may require supported NvOFA hardware.
  * There are deliberately no WGC/DXGI or motion fallbacks.
  * Do not install or rename nvofapi64.dll in Windows system directories.

Safe removal: delete ONLY the app-local experimental nvofapi64.dll, the
block_motion.hlsl and the two launchers. All original V39 files are unchanged.

Build (Windows x64, VS 2022 + Windows 10/11 SDK + CMake):
  cmake -S . -B build -A x64
  cmake --build build --config Release

The new nvofapi64.dll and block_motion.hlsl must sit beside the V39 EXE.
This is a source-first prototype and must not be labeled a validated ready-
to-run FG build until tested on a real target system.
