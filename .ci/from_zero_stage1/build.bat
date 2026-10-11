@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File tools\setup_streamline.ps1 || exit /b 1
cmake -S . -B build -A x64 -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded || exit /b 1
cmake --build build --config Release --parallel || exit /b 1
endlocal
