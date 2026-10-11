cmake_minimum_required(VERSION 3.20)
project(ExternalDLSSGZero LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${cfg} CFGU)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFGU} ${CMAKE_BINARY_DIR}/bin)
endforeach()

if(MSVC)
  add_compile_options(/W4 /WX /permissive- /EHsc /Zc:__cplusplus)
endif()

set(STREAMLINE_FEATURE_DLSS_FG ON CACHE BOOL "" FORCE)
set(STREAMLINE_FEATURE_REFLEX ON CACHE BOOL "" FORCE)
set(STREAMLINE_TARGET_ARCH x64 CACHE STRING "" FORCE)
add_subdirectory(third_party/streamline)

add_executable(ExternalDLSSG WIN32 src/main.cpp)
target_link_libraries(ExternalDLSSG PRIVATE streamline user32 gdi32 shell32 ole32)
target_compile_definitions(ExternalDLSSG PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
add_dependencies(ExternalDLSSG CopyStreamlineDLLs)

# Critical: do NOT link dxgi.lib, d3d11.lib, or d3d12.lib directly. Streamline's
# interposer import library provides those exports for the primary render path.
