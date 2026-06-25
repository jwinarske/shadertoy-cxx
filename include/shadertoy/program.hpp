// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// program.hpp — multi-pass Shadertoy program model (platform-agnostic).
//
// A real Shadertoy shader can have a Common tab, up to four Buffer passes
// (A..D, each rendered to its own double-buffered render target so it can read
// its own and other buffers' previous-frame output), and an Image pass — every
// pass binding up to four input channels (another buffer, a texture, audio, or
// the keyboard).  This header captures that structure without any GPU
// dependency; the GL and Vulkan renderers consume it, and the Shadertoy JSON
// loader (loader.hpp) produces it.

#pragma once

#include <array>
#include <string>

namespace shadertoy {

// What an iChannel is bound to.
enum class ChannelKind {
  kNone,      // unbound — samples 1x1 black
  kBuffer,    // another render pass's output (Buffer A..D)
  kTexture,   // a 2D image file
  kKeyboard,  // 256x3 keyboard state texture (stubbed: all zero)
  kAudio,     // 512x2 audio FFT/waveform texture (stubbed: all zero)
  kCubemap,   // samplerCube; six faces from the media dir, else black
  kVolume,    // sampler3D;  procedural 32^3 noise (no public volume format)
};

enum class Filter { kNearest, kLinear, kMipmap };
enum class Wrap { kClamp, kRepeat };

// One iChannelN binding.
struct Channel {
  ChannelKind kind = ChannelKind::kNone;
  int buffer = -1;            // kBuffer: 0..3 (A..D)
  std::string texture_path;   // kTexture: image path (resolved by the host)
  Filter filter = Filter::kLinear;
  Wrap wrap = Wrap::kRepeat;
  bool vflip = true;          // Shadertoy textures default to vertically flipped
};

// A single render pass: bare Shadertoy code (defines mainImage) plus its four
// input channels.  The Common tab and the back-end preamble are prepended by
// the renderer, not stored here.
struct Pass {
  std::string code;
  std::array<Channel, 4> channels{};
};

// Buffer pass indices (Shadertoy "Buffer A".."Buffer D").
constexpr int kBufferA = 0;
constexpr int kBufferB = 1;
constexpr int kBufferC = 2;
constexpr int kBufferD = 3;
constexpr int kNumBuffers = 4;

// A complete multi-pass program.
struct ShaderProgram {
  std::string name;
  std::string common;                       // Common tab (prepended to all passes)
  std::array<Pass, kNumBuffers> buffers{};   // Buffer A..D
  std::array<bool, kNumBuffers> buffer_used{};  // which buffer slots are present
  Pass image;                                // the Image pass (always present)

  [[nodiscard]] bool uses_buffer(int i) const noexcept {
    return i >= 0 && i < kNumBuffers && buffer_used[static_cast<size_t>(i)];
  }
  [[nodiscard]] bool multipass() const noexcept {
    return buffer_used[0] || buffer_used[1] || buffer_used[2] || buffer_used[3];
  }
};

/// Build a single-pass program (just an Image pass) from bare Shadertoy code.
[[nodiscard]] ShaderProgram MakeSinglePass(std::string image_code,
                                           std::string name = "shader");

}  // namespace shadertoy
