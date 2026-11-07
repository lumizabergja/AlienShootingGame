cmake_minimum_required(VERSION 3.21)
project(V39ShaderProxy LANGUAGES CXX)
if(NOT WIN32)
  message(FATAL_ERROR "V39 Shader Proxy targets Windows x64 with the Windows SDK")
endif()
set(CMAKE_CXX_STANDARD 20)
add_library(nvofapi64 SHARED src/nvof_proxy.cpp)
target_link_libraries(nvofapi64 PRIVATE d3d12 dxgi d3dcompiler)
target_compile_definitions(nvofapi64 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(nvofapi64 PROPERTIES OUTPUT_NAME "nvofapi64")
add_custom_command(TARGET nvofapi64 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
  "${CMAKE_CURRENT_SOURCE_DIR}/shaders/block_motion.hlsl"
  "$<TARGET_FILE_DIR:nvofapi64>/block_motion.hlsl")
