// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// inputs.cpp — GLSL preambles, the default shader, and source-wrapping helpers.

#include "shadertoy/inputs.hpp"
#include <cstdlib>
#include <filesystem>

#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>

namespace shadertoy {

namespace {

// Default shader — an animated kaleidoscopic fractal using only iTime and
// iResolution, so it renders identically on the GL and Vulkan back-ends with
// no channel inputs.
constexpr const char* kDefaultImageShader = R"GLSL(
// Default shadertoy example — animated kaleidoscopic fractal.
// Replace by passing a shader file:  shadertoy_egl path/to/shader.frag
void mainImage(out vec4 fragColor, in vec2 fragCoord) {
    vec2 uv = (2.0 * fragCoord - iResolution.xy) / iResolution.y;
    float t = iTime * 0.4;
    vec3 col = vec3(0.0);
    float d = 0.0;
    for (int i = 0; i < 6; i++) {
        float fi = float(i);
        uv = abs(uv) / dot(uv, uv) - 0.9;
        d += length(uv);
        col += (0.5 + 0.5 * cos(vec3(0.0, 1.0, 2.0) + fi * 0.6 + t + d)) * 0.12;
    }
    float vignette =
        1.0 - 0.3 * length((fragCoord - 0.5 * iResolution.xy) / iResolution.y);
    fragColor = vec4(col * vignette, 1.0);
}
)GLSL";

// ── GLSL preambles
// ────────────────────────────────────────────────────────────

// OpenGL ES 3.0 (EGL back-end).  gl_FragCoord origin is bottom-left, matching
// Shadertoy, so fragCoord is passed straight through.
constexpr const char* kGlesHeader = R"GLSL(#version 300 es
precision highp float;
precision highp int;
precision highp sampler3D;
precision highp samplerCube;
#define HW_PERFORMANCE 1

out vec4 _shadertoy_fragColor;
uniform vec3  iResolution;
uniform float iTime;
uniform float iTimeDelta;
uniform float iFrameRate;
uniform int   iFrame;
uniform vec4  iMouse;
uniform vec4  iDate;
uniform float iSampleRate;
uniform vec3  iChannelResolution[4];
uniform float iChannelTime[4];
)GLSL";

constexpr const char* kGlesFooter = R"GLSL(
void main() {
    vec4 color = vec4(0.0, 0.0, 0.0, 1.0);
    mainImage(color, gl_FragCoord.xy);
    _shadertoy_fragColor = color;
}
)GLSL";

// Vulkan / SPIR-V (#version 450).  Uniforms arrive as a push-constant block;
// iChannel* are descriptor-bound samplers.  gl_FragCoord origin is top-left in
// Vulkan, so the Y axis is flipped to restore Shadertoy's bottom-left origin.
constexpr const char* kVulkanHeader = R"GLSL(#version 450
layout(location = 0) out vec4 _shadertoy_fragColor;
layout(push_constant) uniform _ShadertoyPC {
    vec4  _iResolution;
    vec4  _iMouse;
    vec4  _iDate;
    vec4  _iTimePack;   // x=iTime y=iTimeDelta z=iFrameRate w=iSampleRate
    ivec4 _iFramePack;  // x=iFrame
} _stpc;
#define iResolution (_stpc._iResolution.xyz)
#define iMouse      (_stpc._iMouse)
#define iDate       (_stpc._iDate)
#define iTime       (_stpc._iTimePack.x)
#define iTimeDelta  (_stpc._iTimePack.y)
#define iFrameRate  (_stpc._iTimePack.z)
#define iSampleRate (_stpc._iTimePack.w)
#define iFrame      (_stpc._iFramePack.x)
const vec3  iChannelResolution[4] =
    vec3[4](vec3(0.0), vec3(0.0), vec3(0.0), vec3(0.0));
const float iChannelTime[4] = float[4](0.0, 0.0, 0.0, 0.0);
)GLSL";

constexpr const char* kVulkanFooter = R"GLSL(
void main() {
    vec4 color = vec4(0.0, 0.0, 0.0, 1.0);
    vec2 fragCoord =
        vec2(gl_FragCoord.x, _stpc._iResolution.y - gl_FragCoord.y);
    mainImage(color, fragCoord);
    _shadertoy_fragColor = color;
}
)GLSL";

}  // namespace

const char* DefaultImageShader() noexcept {
  return kDefaultImageShader;
}

namespace {

// GLSL sampler type name for a channel's dimensionality.
const char* SamplerTypeName(SamplerDim d) {
  switch (d) {
    case SamplerDim::kCube:
      return "samplerCube";
    case SamplerDim::k3D:
      return "sampler3D";
    default:
      return "sampler2D";
  }
}

// GLSL ES reserves a set of words "for future use".  Shaders authored against
// desktop GL or WebGL2 sometimes use them as ordinary identifiers (e.g. a local
// named `sample` or `packed`), which the ES compiler rejects outright.  These
// words have no legal use in a GLSL ES shader body, so any occurrence is a
// stray identifier; rename every whole-identifier occurrence to a non-colliding
// form. Renaming *all* occurrences (declarations and uses alike) keeps the
// shader consistent and semantically unchanged.  Qualifiers like `in`/`out` are
// NOT listed — they have valid uses and cannot be blindly renamed.
std::string SanitizeReservedIdentifiers(const std::string& src) {
  static constexpr std::string_view kReserved[] = {
      "sample", "packed", "filter", "active", "partition", "superp"};
  const auto is_ident = [](unsigned char c) {
    return std::isalnum(c) != 0 || c == '_';
  };
  std::string out;
  out.reserve(src.size() + 32);
  std::size_t i = 0;
  while (i < src.size()) {
    const auto c = static_cast<unsigned char>(src[i]);
    // Scan a full identifier from its start so substrings (mySample, sample2D,
    // _sample) never match; non-identifier bytes are copied verbatim.
    if (std::isalpha(c) != 0 || c == '_') {
      std::size_t j = i + 1;
      while (j < src.size() && is_ident(static_cast<unsigned char>(src[j])))
        ++j;
      const std::string_view word(src.data() + i, j - i);
      out.append(word);
      for (const std::string_view reserved : kReserved) {
        if (word == reserved) {
          out.push_back('_');
          break;
        }
      }
      i = j;
    } else {
      out.push_back(src[i]);
      ++i;
    }
  }
  return out;
}

}  // namespace

std::string WrapGles(const std::string& common,
                     const std::string& code,
                     const std::array<SamplerDim, 4>& channels) {
  std::string samplers;
  for (int i = 0; i < 4; ++i) {
    samplers += "uniform ";
    samplers += SamplerTypeName(channels[static_cast<size_t>(i)]);
    samplers += " iChannel";
    samplers += static_cast<char>('0' + i);
    samplers += ";\n";
  }
  return std::string(kGlesHeader) + samplers +
         SanitizeReservedIdentifiers(common) + "\n" +
         SanitizeReservedIdentifiers(code) + kGlesFooter;
}

std::string WrapVulkan(const std::string& common,
                       const std::string& code,
                       const std::array<SamplerDim, 4>& channels) {
  std::string samplers;
  for (int i = 0; i < 4; ++i) {
    samplers += "layout(set = 0, binding = ";
    samplers += static_cast<char>('0' + i);
    samplers += ") uniform ";
    samplers += SamplerTypeName(channels[static_cast<size_t>(i)]);
    samplers += " iChannel";
    samplers += static_cast<char>('0' + i);
    samplers += ";\n";
  }
  return std::string(kVulkanHeader) + samplers +
         SanitizeReservedIdentifiers(common) + "\n" +
         SanitizeReservedIdentifiers(code) + kVulkanFooter;
}

std::string LoadShaderFile(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file)
    return {};
  std::ostringstream ss;
  ss << file.rdbuf();
  return ss.str();
}

std::string ResolveMediaPath(const std::string& src,
                             const std::string& media_dir) {
  std::string dir = media_dir;
  if (dir.empty()) {
    const char* env = std::getenv("SHADERTOY_MEDIA_DIR");
    if (env != nullptr)
      dir = env;
  }
  if (dir.empty())
    return src;
  const std::filesystem::path p(src);
  return (std::filesystem::path(dir) / p.filename()).string();
}

}  // namespace shadertoy
