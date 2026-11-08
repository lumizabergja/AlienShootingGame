cmake_minimum_required(VERSION 3.21)
project(FrameBridgeNative LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "FrameBridgeNative needs the Windows SDK and a Windows C++ compiler.")
endif()

add_executable(FrameBridgeNative WIN32 src/main.cpp)
target_compile_features(FrameBridgeNative PRIVATE cxx_std_17)
target_compile_definitions(FrameBridgeNative PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(FrameBridgeNative PRIVATE d3d11 dxgi d3dcompiler user32 gdi32 dwmapi)

# Optional real NVIDIA Streamline SDK integration diagnostics. This target
# calls NVIDIA's actual feature APIs; it does not use substitute algorithms.
option(FRAMEBRIDGE_BUILD_NVIDIA_PROBE "Build genuine NVIDIA Streamline capability probe" OFF)
set(FRAMEBRIDGE_SL_INCLUDE_DIR "" CACHE PATH "Directory containing official sl.h")
set(FRAMEBRIDGE_SL_INTERPOSER_LIB "" CACHE FILEPATH "Official sl.interposer.lib")
if(FRAMEBRIDGE_BUILD_NVIDIA_PROBE)
  if(NOT EXISTS "${FRAMEBRIDGE_SL_INCLUDE_DIR}/sl.h" OR
     NOT EXISTS "${FRAMEBRIDGE_SL_INTERPOSER_LIB}")
    message(FATAL_ERROR "The genuine NVIDIA Streamline SDK headers and import library are required.")
  endif()
  add_executable(FrameBridgeNvidiaProbe src/streamline_probe.cpp)
  target_compile_features(FrameBridgeNvidiaProbe PRIVATE cxx_std_17)
  target_compile_definitions(FrameBridgeNvidiaProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
  target_include_directories(FrameBridgeNvidiaProbe PRIVATE "${FRAMEBRIDGE_SL_INCLUDE_DIR}")
  target_link_libraries(FrameBridgeNvidiaProbe PRIVATE "${FRAMEBRIDGE_SL_INTERPOSER_LIB}" user32)
  install(TARGETS FrameBridgeNvidiaProbe RUNTIME DESTINATION .)
endif()

if(MSVC)
  target_compile_options(FrameBridgeNative PRIVATE /W4 /EHsc)
  target_link_options(FrameBridgeNative PRIVATE /SUBSYSTEM:WINDOWS)
  if(FRAMEBRIDGE_BUILD_NVIDIA_PROBE)
    target_compile_options(FrameBridgeNvidiaProbe PRIVATE /W4 /EHsc)
  endif()
endif()

install(TARGETS FrameBridgeNative RUNTIME DESTINATION .)
