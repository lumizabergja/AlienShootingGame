cmake_minimum_required(VERSION 3.25)
project(ExternalDLSSG LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "This experimental application targets Windows 11 x64")
endif()
if(NOT MSVC)
  message(FATAL_ERROR "NVIDIA's NGX static loader requires the MSVC toolchain")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(DLSS_SDK_DIR "" CACHE PATH "NVIDIA/DLSS SDK checkout")
if(NOT EXISTS "${DLSS_SDK_DIR}/include/nvsdk_ngx_helpers_dlssg.h")
  message(FATAL_ERROR "Supply DLSS_SDK_DIR pointing to an official NVIDIA/DLSS checkout")
endif()

add_executable(ExternalDLSSG WIN32 src/main.cpp)
target_include_directories(ExternalDLSSG PRIVATE "${DLSS_SDK_DIR}/include")
target_compile_definitions(ExternalDLSSG PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(ExternalDLSSG PRIVATE /W4 /EHsc /utf-8)
target_link_libraries(ExternalDLSSG PRIVATE d3d11 d3d12 dxgi dxguid d3dcompiler user32 gdi32 dwmapi ole32
 "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
