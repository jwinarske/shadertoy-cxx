// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// shadertoy_probe — headless smoke test for the shadertoy-cxx library.
//
// Loads a Shadertoy Image shader (a file argument, or the built-in default),
// wraps it for both back-ends, and — when the Vulkan back-end is compiled in —
// compiles it to SPIR-V.  Needs no display, so it is handy for CI and for
// validating that an arbitrary shader will load before launching a window.
//
//   shadertoy_probe [shader.frag]

#include <shadertoy/config.hpp>
#include <shadertoy/inputs.hpp>

#if SHADERTOY_HAVE_VULKAN
#include <shadertoy/spirv_compile.hpp>
#endif

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
  std::string src =
      argc > 1 ? shadertoy::LoadShaderFile(argv[1]) : std::string();
  if (src.empty()) {
    if (argc > 1)
      std::fprintf(stderr, "probe: could not read %s, using default\n",
                   argv[1]);
    src = shadertoy::DefaultImageShader();
  }

  std::printf("shadertoy-cxx %s  (GL=%d, Vulkan=%d)\n", SHADERTOY_VERSION_STRING,
              SHADERTOY_HAVE_GL, SHADERTOY_HAVE_VULKAN);

  const std::string gles = shadertoy::WrapGles(src);
  std::printf("GLES ES 3.0 program: %zu bytes\n", gles.size());

#if SHADERTOY_HAVE_VULKAN
  const std::string vk = shadertoy::WrapVulkan(src);
  const auto spirv =
      shadertoy::CompileToSpirv(vk, shadertoy::ShaderStage::kFragment);
  if (spirv.empty()) {
    std::fprintf(stderr, "probe: SPIR-V compilation failed\n");
    return 1;
  }
  std::printf("SPIR-V fragment module: %zu words\n", spirv.size());
#endif

  std::printf("probe: OK\n");
  return 0;
}
