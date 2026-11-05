DLSS-G Windows runtime probe (build scaffold)
================================================
This build is a diagnostic probe, NOT the requested live overlay application.
It exercises the NVIDIA NGX DLSS Frame Generation feature through genuine
NVIDIA SDK entry points. It does not select a game PID or capture a live game.
It has no click-through presenter, and it does not include motion modes.

Run from a Windows terminal:
  dlssg-worker.exe --probe
The response is a JSON line with available and multi_frame_count_max.

This binary is compiled from the MIT-licensed HECer/DLSSG-Stream-Worker
repository at build time and includes an official NVIDIA DLL obtained from
the NVIDIA DLSS SDK. Please consult the respective upstream licenses.

It is intentionally named Probe so users do not mistake it for a working
external frame generation app.
