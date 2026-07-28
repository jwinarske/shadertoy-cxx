# SPDX-License-Identifier: MIT
add_executable(shadertoy_probe probe.cpp)
target_link_libraries(shadertoy_probe PRIVATE shadertoy-cxx::shadertoy)

if(HAVE_VULKAN)
  add_executable(vk_offscreen_render vk_offscreen_render.cpp)
  target_link_libraries(vk_offscreen_render PRIVATE shadertoy-cxx::shadertoy
                        Vulkan::Vulkan)

  # Queries formats only — no library, no surface, no display. Links Vulkan
  # directly so it stays runnable on a target that has a driver but not this
  # library's other dependencies.
  add_executable(vk_format_probe vk_format_probe.cpp)
  target_link_libraries(vk_format_probe PRIVATE Vulkan::Vulkan)
endif()
