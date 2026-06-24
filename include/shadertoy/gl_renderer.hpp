// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// gl_renderer.hpp — multi-pass OpenGL ES 3.0 Shadertoy renderer.
//
// Renders a ShaderProgram: each Buffer pass (A..D) draws into its own
// double-buffered RGBA32F render target so it can read the previous frame's
// output of itself and other buffers (Shadertoy feedback semantics), then the
// Image pass draws to the default framebuffer.  Channels bind to buffers,
// image textures (via stb_image), or stub keyboard/audio textures.
//
// Depends only on a current GL ES 3 context; it knows nothing about Wayland,
// DRM, or how the EGL context was created.  The host makes the context current,
// calls Render() each frame, and performs its own buffer swap.

#pragma once

#include <GLES3/gl3.h>

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "shadertoy/inputs.hpp"
#include "shadertoy/program.hpp"

namespace shadertoy {

class GlRenderer {
 public:
  GlRenderer() = default;
  ~GlRenderer();

  GlRenderer(const GlRenderer&) = delete;
  GlRenderer& operator=(const GlRenderer&) = delete;

  /// Single-pass convenience: build from bare Shadertoy Image code.
  [[nodiscard]] bool Init(const std::string& image_shader);

  /// Build (or replace) the active multi-pass program.  Requires a current GL
  /// context.  Returns false on compile/link failure (diagnostics to stderr);
  /// on failure the previous program is left intact.
  [[nodiscard]] bool SetProgram(const ShaderProgram& program);

  /// Directory holding Shadertoy media (image/cubemap files).  A channel's
  /// Shadertoy src ("/media/a/<hash>.png") is resolved to <dir>/<hash>.png.
  /// Defaults to $SHADERTOY_MEDIA_DIR; if unset and no dir is given, the src is
  /// tried as a literal path.  Cube/texture channels with no resolvable file
  /// fall back to a black stub.
  void SetMediaDir(std::string dir) { media_dir_ = std::move(dir); }

  /// Draw one frame.  inputs.res_x/res_y must be the destination framebuffer
  /// size; the Image pass renders into the currently bound framebuffer (0).
  void Render(const ShaderInputs& inputs) noexcept;

  void Destroy() noexcept;

 private:
  struct PassGL {
    GLuint program = 0;
    GLint loc_resolution = -1, loc_time = -1, loc_time_delta = -1,
          loc_frame_rate = -1, loc_frame = -1, loc_mouse = -1, loc_date = -1,
          loc_sample_rate = -1, loc_channel_res = -1;
    std::array<GLint, 4> loc_channel = {-1, -1, -1, -1};
    std::array<Channel, 4> channels{};
    std::array<GLuint, 4> samplers = {0, 0, 0, 0};
    int target_buffer = -1;  // -1 = Image (default FBO); else buffer index
  };
  struct BufferGL {
    std::array<GLuint, 2> tex = {0, 0};
    std::array<GLuint, 2> fbo = {0, 0};
    int front = 0;
  };

  bool BuildPass(const std::string& common,
                 const Pass& pass,
                 int target_buffer,
                 PassGL& out);
  GLuint TextureFor(const Channel& ch);
  GLuint LoadCubemap(const Channel& ch);  // 6-face GL_TEXTURE_CUBE_MAP
  GLuint LoadVolume(const Channel& ch);   // 32^3 procedural-noise GL_TEXTURE_3D
  [[nodiscard]] std::string ResolveMediaPath(const std::string& src) const;
  GLuint StubTexture();      // 1x1 black GL_TEXTURE_2D
  GLuint StubTextureCube();  // 1x1x6 black GL_TEXTURE_CUBE_MAP (cubemap fallback)
  void EnsureBuffers(int w, int h);
  void RenderPass(const PassGL& p, const ShaderInputs& in) noexcept;
  void DestroyPass(PassGL& p) noexcept;
  [[nodiscard]] bool uses_buffer_internal(int b) const noexcept;
  static GLuint Compile(GLenum type, const char* src);

  std::vector<PassGL> buffer_passes_;  // only the used buffers, in A..D order
  PassGL image_pass_;
  std::array<BufferGL, kNumBuffers> buffers_{};
  std::array<bool, kNumBuffers> buffer_used_{};
  std::unordered_map<std::string, GLuint> textures_;  // path → GL texture
  std::unordered_map<GLuint, std::array<float, 3>>
      tex_dims_;  // GL texture id → iChannelResolution (w,h,depth)
  std::string media_dir_;  // base dir for resolving Shadertoy media src paths
  GLuint vao_ = 0;
  GLuint dummy_tex_ = 0;
  GLuint dummy_cube_ = 0;
  int fb_w_ = 0;
  int fb_h_ = 0;
  bool ready_ = false;
};

}  // namespace shadertoy
