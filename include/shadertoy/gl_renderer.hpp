// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// gl_renderer.hpp — OpenGL ES 3.0 Shadertoy renderer.
//
// Depends only on a GL ES 3 context being current; it knows nothing about
// Wayland, DRM, or how the EGL context was created.  The host creates the
// context, makes it current, calls Render() each frame, then performs its own
// buffer swap.  Rendering is a single full-screen triangle; the Shadertoy
// fragment shader does all the work.

#pragma once

#include <GLES3/gl3.h>

#include <array>
#include <string>

#include "shadertoy/inputs.hpp"

namespace shadertoy {

class GlRenderer {
 public:
  GlRenderer() = default;
  ~GlRenderer();

  GlRenderer(const GlRenderer&) = delete;
  GlRenderer& operator=(const GlRenderer&) = delete;

  /// Compile @p image_shader (a bare Shadertoy Image shader) and build the
  /// pipeline.  Requires a current GL context.  Returns false on compile or
  /// link failure (diagnostics are printed to stderr).
  [[nodiscard]] bool Init(const std::string& image_shader);

  /// Draw one frame into the current framebuffer using @p inputs.
  void Render(const ShaderInputs& inputs) noexcept;

  void Destroy() noexcept;

 private:
  static GLuint Compile(GLenum type, const char* src);
  static void PrintProgramLog(GLuint program, const char* stage);
  void CacheUniformLocations() noexcept;
  void SetupGeometry() noexcept;
  void SetupDummyChannels() noexcept;

  GLuint program_ = 0;
  GLuint vao_ = 0;
  GLuint dummy_tex_ = 0;

  GLint loc_resolution_ = -1;
  GLint loc_time_ = -1;
  GLint loc_time_delta_ = -1;
  GLint loc_frame_rate_ = -1;
  GLint loc_frame_ = -1;
  GLint loc_mouse_ = -1;
  GLint loc_date_ = -1;
  GLint loc_sample_rate_ = -1;
  std::array<GLint, 4> loc_channel_ = {-1, -1, -1, -1};
};

}  // namespace shadertoy
