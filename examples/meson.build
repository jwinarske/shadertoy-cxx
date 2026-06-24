# SPDX-License-Identifier: MIT
executable(
    'shadertoy_probe',
    'probe.cpp',
    dependencies: shadertoy_dep,
    install:      false,
)

# gles_sweep — headless GL ES compile sweep (ground-truth shader validation).
# Needs a surfaceless EGL context, so it is gated on egl + glesv2 being present.
egl_dep = dependency('egl', required: false)
if egl_dep.found() and glesv2_dep.found()
    executable(
        'gles_sweep',
        'gles_sweep.cpp',
        dependencies: [shadertoy_dep, egl_dep, glesv2_dep],
        install:      false,
    )
endif
