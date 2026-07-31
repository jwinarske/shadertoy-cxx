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

glesv2_dep   = dependency('glesv2', required: get_option('gl'))
vulkan_dep   = dependency('vulkan', required: get_option('vulkan'))
pipewire_dep = dependency('libpipewire-0.3', required: get_option('pipewire'))
alsa_dep     = dependency('alsa', required: get_option('alsa'))

have_gl       = glesv2_dep.found()
have_vulkan   = vulkan_dep.found()
have_pipewire = pipewire_dep.found()
have_alsa     = alsa_dep.found()

# ── Generated config header (include/shadertoy/config.hpp) ────────────────────

ver = meson.project_version().split('.')
conf = configuration_data()
conf.set('SHADERTOY_VERSION_MAJOR',  ver[0])
conf.set('SHADERTOY_VERSION_MINOR',  ver[1])
conf.set('SHADERTOY_VERSION_PATCH',  ver[2])
conf.set('SHADERTOY_VERSION_STRING', meson.project_version())
conf.set('SHADERTOY_HAVE_GL',       have_gl       ? '1' : '0')
conf.set('SHADERTOY_HAVE_VULKAN',   have_vulkan   ? '1' : '0')
conf.set('SHADERTOY_HAVE_PIPEWIRE', have_pipewire ? '1' : '0')
conf.set('SHADERTOY_HAVE_ALSA',     have_alsa     ? '1' : '0')
conf.set('SHADERTOY_HAVE_JSON',     '1')   # loader uses the vendored nlohmann/json
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
           'src/playlist.cpp', 'src/audio.cpp']
deps    = []
pc_requires = []

if have_gl
    sources += ['src/gl_renderer.cpp']
    deps    += glesv2_dep
    pc_requires += 'glesv2'
endif

# Linking glslang moves "is there a GLSL compiler?" from the target at runtime
# to the build. Optional on purpose: the subprocess path stays compiled in every
# configuration, because a deployment may want a distro-managed compiler it can
# update without rebuilding, or run on an image that ships the tool but not its
# development files. Only meaningful on a Vulkan build -- the GL renderer hands
# GLSL to the driver and needs no SPIR-V.
cxx          = meson.get_compiler('cpp')
have_glslang = false
glslang_dep  = dependency('', required: false)
if have_vulkan and not get_option('glslang').disabled()
    # No glslang.pc is shipped by most distributions, so go through its CMake
    # package config; both components are needed (the resource limits live in a
    # separate target).
    glslang_dep = dependency(
        'glslang',
        method:   'cmake',
        modules:  ['glslang::glslang', 'glslang::glslang-default-resource-limits'],
        required: get_option('glslang'),
    )
    # glslang's SPIR-V writer calls into SPIRV-Tools (disassembly and the
    # optimizer), and its CMake package does not carry those through this
    # lookup, so the link fails on spvContextCreate and friends unless they are
    # named here. They are part of the requirement, not an extra: glslang
    # without them does not link.
    spirv_tools_deps = []
    spirv_tools_ok   = glslang_dep.found()
    foreach lib : ['SPIRV-Tools-opt', 'SPIRV-Tools']
        found = cxx.find_library(lib, required: false)
        if found.found()
            spirv_tools_deps += found
        else
            spirv_tools_ok = false
        endif
    endforeach
    if glslang_dep.found() and not spirv_tools_ok
        msg = 'glslang was found but SPIRV-Tools was not; glslang cannot link without it'
        if get_option('glslang').enabled()
            error(msg + '. Install the SPIRV-Tools development package, or pass -Dglslang=disabled.')
        endif
        message(msg + ' -- falling back to the glslangValidator/glslc subprocess path')
    endif
    have_glslang = glslang_dep.found() and spirv_tools_ok
    if have_glslang
        glslang_dep = [glslang_dep] + spirv_tools_deps
    endif
endif

# stb_image is the single translation unit instantiating the library, and both
# renderers decode texture channels through it -- gated on either back-end
# rather than on GL, and listed once so it is never compiled twice.
if have_gl or have_vulkan
    sources += ['src/stb_image_impl.cpp']
endif

if have_vulkan
    sources += ['src/spirv_compile.cpp', 'src/vk_renderer.cpp',
                'src/vk_offscreen_renderer.cpp']
    deps    += vulkan_dep
    pc_requires += 'vulkan'
    if have_glslang
        sources += ['src/spirv_compile_glslang.cpp']
        deps    += glslang_dep
    endif
endif

pc_requires_private = []
if have_pipewire
    # PipeWire capture back-end; audio.cpp (core) provides the DSP + selector.
    # Private: it is an implementation detail, absent from the public headers.
    sources += ['src/audio_pipewire.cpp']
    deps    += pipewire_dep
    pc_requires_private += 'libpipewire-0.3'
endif

if have_alsa
    # ALSA capture back-end (works headless without a PipeWire daemon).
    sources += ['src/audio_alsa.cpp']
    deps    += alsa_dep
    pc_requires_private += 'alsa'
endif

# ── Shared library ────────────────────────────────────────────────────────────

shadertoy_lib = library(
    'shadertoy',
    sources,
    # Private, not in config.hpp: this only picks which translation unit defines
    # the compiler-backend stubs. Consumers neither link glslang nor need to
    # agree on it.
    cpp_args:                ['-DSHADERTOY_HAVE_GLSLANG_LIB=' + (have_glslang ? '1' : '0')],
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
    'include/shadertoy/audio.hpp',
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
        'include/shadertoy/vk_offscreen_renderer.hpp',
        subdir: 'shadertoy',
    )
endif
if have_pipewire
    install_headers('include/shadertoy/audio_pipewire.hpp', subdir: 'shadertoy')
endif
# config.hpp is installed by include/shadertoy/meson.build.

# ── pkg-config for downstream consumers ───────────────────────────────────────

pkg = import('pkgconfig')
pkg.generate(
    shadertoy_lib,
    name:        'shadertoy-cxx',
    description: 'Platform-agnostic Shadertoy renderer (OpenGL ES 3 + Vulkan)',
    subdirs:          [],
    requires:         pc_requires,
    requires_private: pc_requires_private,
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
    'PipeWire mic capture': have_pipewire,
    'ALSA mic capture':     have_alsa,
}, section: 'shadertoy-cxx back-ends')

# ── Examples (optional, off by default) ───────────────────────────────────────

if get_option('examples')
    subdir('examples')
endif
