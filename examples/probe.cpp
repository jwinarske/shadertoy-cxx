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
#include <shadertoy/loader.hpp>
#include <shadertoy/program.hpp>

#if SHADERTOY_HAVE_VULKAN
#include <shadertoy/spirv_compile.hpp>
#endif

#include <cstdio>
#include <cstring>
#include <string>

namespace {
const char* kKindName(shadertoy::ChannelKind k) {
  switch (k) {
    case shadertoy::ChannelKind::kBuffer: return "buffer";
    case shadertoy::ChannelKind::kTexture: return "texture";
    case shadertoy::ChannelKind::kKeyboard: return "keyboard";
    case shadertoy::ChannelKind::kAudio: return "audio";
    default: return "-";
  }
}
void DumpPass(const char* label, const shadertoy::Pass& p) {
  std::printf("  %-8s code=%zuB  channels=[", label, p.code.size());
  for (int i = 0; i < 4; ++i) {
    const auto& c = p.channels[static_cast<size_t>(i)];
    std::printf("%s%s", i ? "," : "", kKindName(c.kind));
    if (c.kind == shadertoy::ChannelKind::kBuffer)
      std::printf("%d", c.buffer);
  }
  std::printf("]\n");
}

int ProbeJson(const char* path) {
  shadertoy::ShaderProgram prog;
  std::string err;
  if (!shadertoy::LoadShadertoyJsonFile(path, prog, &err)) {
    std::fprintf(stderr, "probe: load %s failed: %s\n", path, err.c_str());
    return 1;
  }
  std::printf("loaded \"%s\"  multipass=%d  common=%zuB\n", prog.name.c_str(),
              prog.multipass() ? 1 : 0, prog.common.size());
  const char* names[] = {"Buf A", "Buf B", "Buf C", "Buf D"};
  for (int b = 0; b < shadertoy::kNumBuffers; ++b)
    if (prog.uses_buffer(b))
      DumpPass(names[b], prog.buffers[static_cast<size_t>(b)]);
  DumpPass("Image", prog.image);
  std::printf("probe: OK\n");
  return 0;
}
}  // namespace

int main(int argc, char** argv) {
  // A .json argument is parsed as a full Shadertoy export (multi-pass).
  if (argc > 1) {
    const char* a = argv[1];
    const size_t n = std::strlen(a);
    if (n > 5 && std::strcmp(a + n - 5, ".json") == 0)
      return ProbeJson(a);
  }

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
