# SPDX-License-Identifier: MIT
executable(
    'shadertoy_probe',
    'probe.cpp',
    dependencies: shadertoy_dep,
    install:      false,
)

# Offscreen tools — need an EGL context, so gated on egl + glesv2 being present.
#   gles_sweep      — headless GL ES compile sweep (ground-truth validation).
#   headless_render — render a shader to a PPM (CI snapshots, windowless preview).
egl_dep = dependency('egl', required: false)
if egl_dep.found() and glesv2_dep.found()
    executable(
        'gles_sweep',
        'gles_sweep.cpp',
        dependencies: [shadertoy_dep, egl_dep, glesv2_dep],
        install:      false,
    )
    executable(
        'headless_render',
        'headless_render.cpp',
        dependencies: [shadertoy_dep, egl_dep, glesv2_dep],
        install:      false,
    )
endif

# Vulkan offscreen renderer — targets a caller-owned image on a caller-owned
# device, so it needs no surface and no compositor.
if vulkan_dep.found()
    executable(
        'vk_offscreen_render',
        'vk_offscreen_render.cpp',
        dependencies: [shadertoy_dep, vulkan_dep],
        install:      false,
    )

    # Queries formats only — no library, no surface, no display. Depends on
    # Vulkan alone so it stays runnable on a target that has a driver but not
    # this library's other dependencies.
    executable(
        'vk_format_probe',
        'vk_format_probe.cpp',
        dependencies: [vulkan_dep],
        install:      false,
    )
endif
