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
