# SPDX-License-Identifier: MIT
# Copyright (c) 2026 shadertoy-cxx contributors

project(
    'shadertoy-cxx',
    'cpp',
    version: '0.3.0',
    license: 'MIT',
    meson_version: '>= 1.1',
    default_options: [
        'cpp_std=c++17',
        'warning_level=2',
        'b_ndebug=if-release',
    ],
)

# ── Back-end dependencies (independently gated) ───────────────────────────────
# The 'gl' and 'vulkan' features are auto by default: each back-end is compiled
# when its library is present and silently dropped otherwise.  Set
# -Dgl=disabled / -Dvulkan=disabled to force a build that excludes one (or both)
# back-end — the library then ships only the platform-agnostic core.

glesv2_dep = dependency('glesv2', required: get_option('gl'))
vulkan_dep = dependency('vulkan', required: get_option('vulkan'))

have_gl     = glesv2_dep.found()
have_vulkan = vulkan_dep.found()

# ── Generated config header (include/shadertoy/config.hpp) ────────────────────

ver = meson.project_version().split('.')
conf = configuration_data()
conf.set('SHADERTOY_VERSION_MAJOR',  ver[0])
conf.set('SHADERTOY_VERSION_MINOR',  ver[1])
conf.set('SHADERTOY_VERSION_PATCH',  ver[2])
conf.set('SHADERTOY_VERSION_STRING', meson.project_version())
conf.set('SHADERTOY_HAVE_GL',     have_gl     ? '1' : '0')
conf.set('SHADERTOY_HAVE_VULKAN', have_vulkan ? '1' : '0')
conf.set('SHADERTOY_HAVE_JSON',   '1')   # loader uses the vendored nlohmann/json
shader_install_dir = get_option('prefix') / get_option('datadir') / 'shadertoy-cxx' / 'shaders'
conf.set('SHADERTOY_SHADER_DIR',  shader_install_dir)

subdir('include/shadertoy')   # generates config.hpp into the build include tree

# ── Public + private include directories ──────────────────────────────────────

public_inc  = include_directories('include')
# Vendored, header-only third-party deps (not exposed in public headers).
private_inc = include_directories('third_party/stb', 'third_party')

# ── Library sources — core is always built; back-ends gate in ─────────────────
# The Common-tab/multipass model, the Shadertoy JSON loader (nlohmann/json), and
# the source-wrapping helpers are always present.

sources = ['src/inputs.cpp', 'src/program.cpp', 'src/loader.cpp',
           'src/playlist.cpp']
deps    = []
pc_requires = []

if have_gl
    # gl_renderer pulls in stb_image (vendored) for texture channels.
    sources += ['src/gl_renderer.cpp', 'src/stb_image_impl.cpp']
    deps    += glesv2_dep
    pc_requires += 'glesv2'
endif

if have_vulkan
    sources += ['src/spirv_compile.cpp', 'src/vk_renderer.cpp']
    deps    += vulkan_dep
    pc_requires += 'vulkan'
endif

# ── Shared library ────────────────────────────────────────────────────────────

shadertoy_lib = library(
    'shadertoy',
    sources,
    version:                 meson.project_version(),
    soversion:               ver[0],
    include_directories:     [public_inc, private_inc],
    dependencies:            deps,
    gnu_symbol_visibility:   'default',
    install:                 true,
)

# ── Install public headers ────────────────────────────────────────────────────

install_headers(
    'include/shadertoy/inputs.hpp',
    'include/shadertoy/program.hpp',
    'include/shadertoy/loader.hpp',
    'include/shadertoy/playlist.hpp',
    subdir: 'shadertoy',
)

# ── Install the bundled shader set ────────────────────────────────────────────
# Only license-compatible shaders are bundled (see shaders/CREDITS.md):
# original MIT shaders plus X3yXRd (MIT, notice retained).
install_data(
    'shaders/default.frag',
    'shaders/trail.json',
    'shaders/X3yXRd.json',
    'shaders/CREDITS.md',
    install_dir: get_option('datadir') / 'shadertoy-cxx' / 'shaders',
)

if have_gl
    install_headers('include/shadertoy/gl_renderer.hpp', subdir: 'shadertoy')
endif
if have_vulkan
    install_headers(
        'include/shadertoy/spirv_compile.hpp',
        'include/shadertoy/vk_renderer.hpp',
        subdir: 'shadertoy',
    )
endif
# config.hpp is installed by include/shadertoy/meson.build.

# ── pkg-config for downstream consumers ───────────────────────────────────────

pkg = import('pkgconfig')
pkg.generate(
    shadertoy_lib,
    name:        'shadertoy-cxx',
    description: 'Platform-agnostic Shadertoy renderer (OpenGL ES 3 + Vulkan)',
    subdirs:     [],
    requires:    pc_requires,
)

# ── Dependency object for use as a Meson subproject ───────────────────────────

shadertoy_dep = declare_dependency(
    link_with:           shadertoy_lib,
    include_directories: public_inc,
    dependencies:        deps,
)
meson.override_dependency('shadertoy-cxx', shadertoy_dep)

summary({
    'OpenGL ES 3 back-end': have_gl,
    'Vulkan back-end':      have_vulkan,
}, section: 'shadertoy-cxx back-ends')

# ── Examples (optional, off by default) ───────────────────────────────────────

if get_option('examples')
    subdir('examples')
endif
