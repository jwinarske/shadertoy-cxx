# shadertoy-cxx

A small, **platform-agnostic** C++17 library that renders
[Shadertoy](https://www.shadertoy.com) "Image" shaders. It carries no
windowing dependency of its own — the host project provides the surface
(Wayland, DRM/KMS, …) and a current context, and shadertoy-cxx does the rest.

It ships two interchangeable rendering back-ends, **gated independently** at
build time:

- **OpenGL ES 3.0** (`shadertoy::GlRenderer`) — for EGL hosts.
- **Vulkan** (`shadertoy::VkRenderer`) — renders into a swapchain; GLSL is
  compiled to SPIR-V at runtime so any shader loads without an offline step.

You can build with the GL back-end only, the Vulkan back-end only, both, or
neither (just the agnostic core).

## What it does

A Shadertoy Image shader is a single function:

```glsl
void mainImage(out vec4 fragColor, in vec2 fragCoord) { /* ... */ }
```

shadertoy-cxx wraps that snippet into a complete fragment shader for the chosen
back-end, declaring the standard uniforms (`iResolution`, `iTime`, `iTimeDelta`,
`iFrame`, `iMouse`, `iDate`, `iSampleRate`, `iFrameRate`, `iChannelResolution`)
and the `iChannel0..3` samplers. A full-screen triangle is drawn and the shader
does all the work.

### Multi-pass programs

Real Shadertoy shaders can have a **Common** tab, up to four **Buffer** passes
(A–D, each double-buffered so it can read its own and other buffers' previous
frame — feedback), and an **Image** pass, each binding up to four channels. The
`ShaderProgram` model (`program.hpp`) captures this, and the JSON loader
(`loader.hpp`) builds one straight from a Shadertoy API/export `.json`:

```cpp
shadertoy::ShaderProgram prog;
shadertoy::LoadShadertoyJsonFile("shader.json", prog);
glRenderer.SetProgram(prog);   // buffers + common + texture channels
```

Channel support: **buffer** and **texture** (image files, via stb_image) are
rendered; **keyboard** and **audio** channels are stubbed to a black/zero
texture (shaders compile and run, just without that input). Cubemap/volume/video
inputs are unsupported.

> The Vulkan back-end currently renders single-pass Image shaders; multi-pass on
> Vulkan is in progress. The OpenGL ES 3 back-end is fully multi-pass.

## Layout

```
include/shadertoy/
  inputs.hpp         ShaderInputs, PushConstants, source wrapping (no GPU dep)
  gl_renderer.hpp    GlRenderer            (needs GLES3 at compile time)
  spirv_compile.hpp  CompileToSpirv()      (Vulkan back-end)
  vk_renderer.hpp    VkRenderer            (needs vulkan)
  config.hpp         generated: SHADERTOY_HAVE_GL / SHADERTOY_HAVE_VULKAN
```

## Build & install

### Meson

```sh
meson setup build              # auto-detects glesv2 / vulkan
meson setup build -Dvulkan=disabled    # GL-only
meson setup build -Dgl=disabled        # Vulkan-only
meson install -C build
```

### CMake

```sh
cmake -S . -B build            # SHADERTOY_GL / SHADERTOY_VULKAN default AUTO
cmake -S . -B build -DSHADERTOY_VULKAN=OFF
cmake --build build
cmake --install build
```

Both installs provide headers, a versioned shared library, a `shadertoy-cxx.pc`
pkg-config file, and CMake package config (`shadertoy-cxx::shadertoy`).

## Using it

### Meson consumer

```meson
shadertoy_dep = dependency('shadertoy-cxx',
    fallback: ['shadertoy-cxx', 'shadertoy_dep'])
```

### CMake consumer

```cmake
find_package(shadertoy-cxx REQUIRED)
target_link_libraries(my_app PRIVATE shadertoy-cxx::shadertoy)
```

### Sketch (EGL host)

```cpp
#include <shadertoy/gl_renderer.hpp>

shadertoy::GlRenderer renderer;
renderer.Init(shadertoy::DefaultImageShader());   // or LoadShaderFile(path)

// per frame, with a current GL ES 3 context:
shadertoy::ShaderInputs in;
in.res_x = width; in.res_y = height; in.time = seconds;
renderer.Render(in);
// host swaps buffers
```

The Vulkan path is symmetric via `shadertoy::VkRenderer::Create()`, where the
host supplies the instance extensions and a `create_surface` callback for its
window system.

## Runtime requirement (Vulkan back-end)

SPIR-V is produced at runtime by invoking `glslangValidator` or `glslc` (Vulkan
SDK / mesa tools). Override the binary with `SHADERTOY_GLSLANG` if needed.

## License

MIT — see [LICENSE](LICENSE).
