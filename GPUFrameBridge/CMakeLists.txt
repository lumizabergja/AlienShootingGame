cmake_minimum_required(VERSION 3.20)
project(VkFrameBridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(present_sampler_test tests/present_sampler_test.cpp)
target_include_directories(present_sampler_test PRIVATE include)

enable_testing()
add_test(NAME present_sampler_test COMMAND present_sampler_test)

if(WIN32)
    add_executable(VkFrameBridgeController WIN32 src/bridge_controller.cpp)
    target_include_directories(VkFrameBridgeController PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(VkFrameBridgeController PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(VkFrameBridgeController PRIVATE user32 gdi32)
    if(MINGW)
        target_link_options(VkFrameBridgeController PRIVATE -municode)
    endif()

    find_path(VULKAN_HEADERS_INCLUDE_DIR vulkan/vulkan.h
        HINTS "${VULKAN_HEADERS_DIR}" "$ENV{VULKAN_SDK}/Include" "$ENV{VULKAN_SDK}/include"
        REQUIRED)
    add_library(VkFrameBridgeLayer SHARED src/vk_frame_layer.cpp)
    target_include_directories(VkFrameBridgeLayer PRIVATE
        "${VULKAN_HEADERS_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(VkFrameBridgeLayer PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    set_target_properties(VkFrameBridgeLayer PROPERTIES PREFIX "")
    add_custom_command(TARGET VkFrameBridgeLayer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/VK_LAYER_FRAMEBRIDGE_sampler.json"
            "$<TARGET_FILE_DIR:VkFrameBridgeLayer>/VK_LAYER_FRAMEBRIDGE_sampler.json")
endif()
