# SPDX-License-Identifier: MIT
add_executable(shadertoy_probe probe.cpp)
target_link_libraries(shadertoy_probe PRIVATE shadertoy-cxx::shadertoy)

if(HAVE_VULKAN)
  add_executable(vk_offscreen_render vk_offscreen_render.cpp)
  target_link_libraries(vk_offscreen_render PRIVATE shadertoy-cxx::shadertoy
                        Vulkan::Vulkan)
endif()
