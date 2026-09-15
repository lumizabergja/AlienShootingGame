@echo off
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
 echo Open the x64 Native Tools Command Prompt for Visual Studio, then run build.bat.
 pause
 exit /b 1
)
if not defined VULKAN_SDK (
 echo Install the LunarG Vulkan SDK for Windows first, then reopen the command prompt.
 pause
 exit /b 1
)
cl /nologo /std:c++17 /EHsc /O2 /W4 main.cpp /I"%VULKAN_SDK%\Include" /Fe:VulkanCapturePrototype.exe /link /LIBPATH:"%VULKAN_SDK%\Lib" vulkan-1.lib d3d11.lib dxgi.lib user32.lib
if errorlevel 1 (
 echo Build failed. Copy the errors when asking for help.
 pause
 exit /b 1
)
echo Built VulkanCapturePrototype.exe
pause
