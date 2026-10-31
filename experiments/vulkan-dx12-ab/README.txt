ALIEN / STREAMLINE VULKAN vs DIRECTX 12 A/B TEST
================================================
This ZIP contains the official NVIDIA Streamline sample application with
DLSS Frame Generation support, built from NVIDIA-RTX/Streamline_Sample and
Streamline SDK 2.14.16. It is a renderer/FG A/B experiment, not a replacement
for the existing click-through, no-center-hole, NvFBC/NvOFA Alien overlay.

Requirements: Windows x64, RTX 5060 Ti, up-to-date NVIDIA Game Ready driver.
No Vulkan SDK or compiler is needed to RUN the packaged app.

HOW TO RUN
1. Extract the ZIP completely; do not execute from inside the ZIP.
2. Double-click RUN_DX12.cmd. Select the same scene, resolution, DLSS-G
   multiplier, Reflex option and VSync setting for both trials.
3. Close the app completely.
4. Double-click RUN_VULKAN.cmd and repeat the same settings.
5. Do several runs each way, alternating the order (DX12/Vulkan/DX12/Vulkan).
6. Look at actual displayed-frame times and present mode with PresentMon.
   To compare input latency, use a high-speed camera or NVIDIA LDAT if
   available. FPS alone is not proof of lower click-to-photon latency.

IMPORTANT
- This app renders its own sample scene. It DOES NOT capture CS2/LoL.
- It is NOT the click-through external capture overlay.
- This test can isolate renderer + Streamline DLSS-G API differences, but
  cannot establish the latency difference of the complete Alien overlay
  (NvFBC -> CUDA -> external NvOFA -> DLSS-G -> click-through display).
- NVIDIA Streamline Vulkan FG is supported but may use interop optical flow
  by default. Native VK optical flow needs appropriate Vulkan setup.
- A Vulkan port of the full Alien overlay remains a separate engineering task.
- Generated frames count as displayed frames, not new game simulation/input
  samples. Keep the actual native/base FPS and frame time in your comparison.
- Do not enable third-party Vulkan layers or injection in protected online games.

PROVENANCE
https://github.com/NVIDIA-RTX/Streamline_Sample
https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.14.16
https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md
https://github.com/lumizabergja/AlienShootingGame
