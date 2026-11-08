cmake_minimum_required(VERSION 3.21)
project(FrameBridgeNative LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "FrameBridgeNative needs the Windows SDK and a Windows C++ compiler.")
endif()

add_executable(FrameBridgeNative WIN32 src/main.cpp)
target_compile_features(FrameBridgeNative PRIVATE cxx_std_17)
target_compile_definitions(FrameBridgeNative PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(FrameBridgeNative PRIVATE d3d11 dxgi d3dcompiler user32 gdi32 dwmapi)

if(MSVC)
  target_compile_options(FrameBridgeNative PRIVATE /W4 /EHsc)
  target_link_options(FrameBridgeNative PRIVATE /SUBSYSTEM:WINDOWS)
endif()

install(TARGETS FrameBridgeNative RUNTIME DESTINATION .)
