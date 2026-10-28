# Preset C external frame generation — Numeric Capture FPS

This is based on the user's previously tested **Preset C / NvFBC Low-Latency v3**
source, with the same D3D12 presenter and NVIDIA Streamline DLSS-G integration.

## Controls

- **Game** — pick the target window, then click Refresh if necessary.
- **FG** — Off, 2x, 3x, 4x, 5x, or 6x. Availability depends on GPU, driver, and
  Streamline's reported supported multiplier.
- **Capture FPS** — a plain editable numeric field: e.g. `60`, `75`, `80`,
  `112.5`, `120`. Use `0` for uncapped capture. Accepted range: 0–1000.
  The rate is a **maximum** number of distinct source captures per second,
  not an artificial source-FPS generator. If the game renders fewer unique
  frames, this field cannot create more.
- **Center bypass (px)** — leaves a native square at screen center to avoid
  frame-generated crosshair flicker; `0` disables it.
- **NvOFA** — fixed **4x4 / FAST / 10% source size**. The optical flow path
  falls back to zero motion if its hardware initialization fails; check logs.

## Operation

1. Extract the entire ZIP, keeping all DLLs beside `FrameGeneration.exe`.
2. Run `FrameGeneration.exe` on Windows with an NVIDIA GPU and compatible
   driver. Do not launch from inside the ZIP.
3. Start the game in borderless/windowed mode, choose the game window in the
   dropdown and refresh as required.
4. Enter `75` (or any desired source capture rate), select FG multiplier,
   then click Start. Use Stop before changing settings.
5. Use Ctrl+Alt+F12 if the output must be hidden immediately.

## Diagnostics

The application records Streamline/NvFBC/NVOFA and frame-generation status
under the user profile. Examine `FrameGeneration*.log`,
`DLSS-Presenter*.log`, `sl*.log`, and `PresetC*.log` where present.

The Preset C override is requested through the NVIDIA driver profile;
the NVIDIA driver ultimately controls the exact model used. The runtime
reports supported FG multipliers and presentation statistics.

## Architecture, compatibility, limits

- **Capture:** the proven NvFBC GPU path; this is not the pure Vulkan-layer
  experiment.
- **Presentation:** Direct3D 12 flip-model output; hardware NVIDIA DLSS-G
  driven through Streamline.
- **Synthetic inputs:** this external tool supplies flat depth / synthetic
  motion where engine depth and motion buffers are not exposed. Visual
  artifacts are possible and **input latency is not guaranteed to beat native**.
- This release is compiled in GitHub Actions but can only be validated
  end-to-end on an NVIDIA RTX gaming PC.
- Game anti-cheat and capture restrictions remain in effect; avoid modifying
  protected game processes.

NVIDIA SDK components are distributed under their own licenses in
the included `licenses` folder.
