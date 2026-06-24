// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// inputs.hpp — Shadertoy uniform state and GLSL source wrapping.
//
// The portable heart of shadertoy-cxx: no dependency on Wayland, DRM, EGL, or
// Vulkan.  It defines the per-frame Shadertoy uniform values (ShaderInputs),
// the matching Vulkan push-constant layout (PushConstants), and helpers that
// turn a bare Shadertoy "Image" shader — a function
// `void mainImage(out vec4, in vec2)` — into a complete fragment shader for
// either OpenGL ES 3.0 or Vulkan/SPIR-V (#version 450).
//
// Channel inputs (iChannel0..3) are declared so that shaders referencing them
// compile, but sample a 1x1 opaque-black texture — multi-pass / texture /
// audio Shadertoy inputs are out of scope.

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace shadertoy {

// ── ShaderInputs ──────────────────────────────────────────────────────────────
// Mirror of the Shadertoy uniform set, filled once per frame by the host and
// consumed by whichever renderer (GL or Vulkan) is active.
struct ShaderInputs {
  float res_x = 0.0f;       // iResolution.x  (viewport width, pixels)
  float res_y = 0.0f;       // iResolution.y  (viewport height, pixels)
  float res_z = 1.0f;       // iResolution.z  (pixel aspect ratio, usually 1)
  float time = 0.0f;        // iTime          (seconds since start)
  float time_delta = 0.0f;  // iTimeDelta     (seconds since previous frame)
  float frame_rate = 60.0f; // iFrameRate     (frames per second)
  float sample_rate = 44100.0f;  // iSampleRate (audio sample rate, Hz)
  int32_t frame = 0;        // iFrame         (frame counter)

  // iMouse: xy = current pixel coords while a button is held; zw = pixel coords
  // of the last button-down (z/w sign encodes the click state, per Shadertoy).
  float mouse_x = 0.0f, mouse_y = 0.0f, mouse_z = 0.0f, mouse_w = 0.0f;

  // iDate: (year, month [0-11], day, seconds-since-midnight).
  float date_y = 0.0f, date_m = 0.0f, date_d = 0.0f, date_s = 0.0f;
};

// ── Vulkan push-constant block ────────────────────────────────────────────────
// Packed to match the `layout(push_constant)` block emitted by the Vulkan
// preamble.  96 bytes — within the 128-byte guaranteed minimum push-constant
// size.  std430-compatible (all vec4/ivec4, 16-aligned).
struct PushConstants {
  float iResolution[4];  // xyz used, w padding
  float iMouse[4];
  float iDate[4];
  float iTimePack[4];    // x=iTime y=iTimeDelta z=iFrameRate w=iSampleRate
  int32_t iFramePack[4]; // x=iFrame, rest padding
};

[[nodiscard]] inline PushConstants ToPushConstants(
    const ShaderInputs& in) noexcept {
  PushConstants pc{};
  pc.iResolution[0] = in.res_x;
  pc.iResolution[1] = in.res_y;
  pc.iResolution[2] = in.res_z;
  pc.iResolution[3] = 0.0f;
  pc.iMouse[0] = in.mouse_x;
  pc.iMouse[1] = in.mouse_y;
  pc.iMouse[2] = in.mouse_z;
  pc.iMouse[3] = in.mouse_w;
  pc.iDate[0] = in.date_y;
  pc.iDate[1] = in.date_m;
  pc.iDate[2] = in.date_d;
  pc.iDate[3] = in.date_s;
  pc.iTimePack[0] = in.time;
  pc.iTimePack[1] = in.time_delta;
  pc.iTimePack[2] = in.frame_rate;
  pc.iTimePack[3] = in.sample_rate;
  pc.iFramePack[0] = in.frame;
  pc.iFramePack[1] = 0;
  pc.iFramePack[2] = 0;
  pc.iFramePack[3] = 0;
  return pc;
}

// ── Source assembly ───────────────────────────────────────────────────────────

/// Sampler dimensionality for a channel, so the wrapper declares iChannelN with
/// the GLSL sampler type the pass actually samples (a cubemap shader does
/// `texture(iChannelN, vec3)`, which will not compile against a sampler2D).
enum class SamplerDim { k2D, kCube, k3D };

/// A self-contained default Shadertoy Image shader (uses only iTime and
/// iResolution), so the library has something to render with no arguments.
[[nodiscard]] const char* DefaultImageShader() noexcept;

/// Wrap a Shadertoy pass (Common tab + pass code) into a complete GLSL ES 3.0
/// fragment shader, declaring iChannel0..3 with the given sampler types.
[[nodiscard]] std::string WrapGles(const std::string& common,
                                   const std::string& code,
                                   const std::array<SamplerDim, 4>& channels);

/// Convenience: wrap a pass whose channels are all 2D (or unsampled).
[[nodiscard]] inline std::string WrapGles(const std::string& common,
                                          const std::string& code) {
  return WrapGles(common, code,
                  {SamplerDim::k2D, SamplerDim::k2D, SamplerDim::k2D,
                   SamplerDim::k2D});
}

/// Single-pass convenience: wrap bare Image code with no Common tab.
[[nodiscard]] inline std::string WrapGles(const std::string& image_shader) {
  return WrapGles(std::string(), image_shader);
}

/// Wrap a Shadertoy pass (Common tab + pass code) into a complete Vulkan GLSL
/// fragment shader (ready to feed to glslang / shaderc for SPIR-V).
[[nodiscard]] std::string WrapVulkan(const std::string& common,
                                     const std::string& code,
                                     const std::array<SamplerDim, 4>& channels);

/// Convenience: wrap a pass whose channels are all 2D (or unsampled).
[[nodiscard]] inline std::string WrapVulkan(const std::string& common,
                                            const std::string& code) {
  return WrapVulkan(common, code,
                    {SamplerDim::k2D, SamplerDim::k2D, SamplerDim::k2D,
                     SamplerDim::k2D});
}

/// Single-pass convenience: wrap bare Image code with no Common tab.
[[nodiscard]] inline std::string WrapVulkan(const std::string& image_shader) {
  return WrapVulkan(std::string(), image_shader);
}

/// Read a Shadertoy Image shader from @p path.  Returns the file contents, or
/// an empty string on failure (caller should fall back to DefaultImageShader).
[[nodiscard]] std::string LoadShaderFile(const std::string& path);

}  // namespace shadertoy
