NvFast Confidence — fresh experimental Windows x64 app

1. Extract the complete folder. Keep all runtime DLLs next to the EXE.
2. Run NvFastConfidence.exe. Select your game window and FG multiplier.
3. Press Start and focus the game. Use borderless/windowed mode and SDR.
4. Ctrl+Alt+F8 stops; Stop also works after switching back to this app.

The only motion method is NvOFA 4x4 FAST. Output-cost confidence is always ON.
Motion analysis uses the full captured resolution. No NVENC, shader-estimated,
zero-motion or global-motion choices. FG Off is available for comparison.
Confidence selects consistent vectors; it does not scale valid motion down.

Capture: private legacy NvFBC CUDA PID interface -> shared D3D12 GPU buffer.
No image readback or encode/decode. One source frame in flight; capture starts
only after presentation pacing and previous resource use have completed.
Current-to-previous NvOFA motion, 8-bit confidence, Reflex On+Boost, GPU fences,
click-through output, pause/hide on focus loss, history reset on resume.
The UI separately reports capture submissions and DLSS-reported presents.
These are not measurements of unique source frames, scanout or input latency.

Requirements: compatible NVIDIA driver with legacy NvFBC CUDA PID capture,
RTX GPU with DLSS-G support, Windows 10/11, HAGS enabled for DLSS-G.
Unsupported capture or confidence stops with an error; there is no fallback.
A requested FG multiplier must be supported by the installed GPU/runtime.
If source/window resolution changes, Stop and Start to recreate resources.

LIMITS: external pixels provide no true game depth, engine motion vectors or
separate HUD. Flat depth and optical flow are approximations. HUD/camera cuts,
occlusion and high multipliers can produce artifacts. This is a new experimental
implementation, not the prior ZIP. NVIDIA GPU execution and latency require
hardware testing. A successful Windows build/WARP shader test does not prove
NvFBC or DLSS-G compatibility. No promise of lower-than-native latency.
The application requests DLSS-G through Streamline; it does not claim a verified
Preset C override. NVIDIA App/driver overrides may affect the runtime model.

last-error.txt contains the last failure. No per-frame disk logging.
Build provenance, source and checks are in BUILD-VALIDATION.txt and source/.
