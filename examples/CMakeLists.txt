# SPDX-License-Identifier: MIT
add_executable(shadertoy_probe probe.cpp)
target_link_libraries(shadertoy_probe PRIVATE shadertoy-cxx::shadertoy)
