// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// gl_renderer.cpp — multi-pass OpenGL ES 3.0 Shadertoy renderer implementation.

#include "shadertoy/gl_renderer.hpp"

#include "stb_image.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <random>
#include <string>
#include <vector>

namespace shadertoy {

namespace {

// Full-screen triangle generated from gl_VertexID — no vertex buffer needed.
constexpr const char* kVertexShader = R"GLSL(#version 300 es
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

GLint MinFilter(Filter f) {
  switch (f) {
    case Filter::kNearest:
      return GL_NEAREST;
    case Filter::kMipmap:
      return GL_LINEAR_MIPMAP_LINEAR;
    case Filter::kLinear:
    default:
      return GL_LINEAR;
  }
}
GLint MagFilter(Filter f) {
  return f == Filter::kNearest ? GL_NEAREST : GL_LINEAR;
}
GLint WrapMode(Wrap w) {
  return w == Wrap::kClamp ? GL_CLAMP_TO_EDGE : GL_REPEAT;
}

}  // namespace

GlRenderer::~GlRenderer() {
  Destroy();
}

GLuint GlRenderer::Compile(GLenum type, const char* src) {
  const GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &src, nullptr);
  glCompileShader(shader);
  GLint ok = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok != GL_TRUE) {
    GLint len = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<size_t>(len > 0 ? len : 1), '\0');
    glGetShaderInfoLog(shader, len, nullptr, log.data());
    std::fprintf(stderr, "shadertoy: GLSL %s compile failed:\n%s\n",
                 type == GL_VERTEX_SHADER ? "vertex" : "fragment", log.c_str());
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

bool GlRenderer::BuildPass(const std::string& common,
                           const Pass& pass,
                           int target_buffer,
                           PassGL& out) {
  std::array<SamplerDim, 4> dims = {SamplerDim::k2D, SamplerDim::k2D,
                                    SamplerDim::k2D, SamplerDim::k2D};
  for (int i = 0; i < 4; ++i) {
    switch (pass.channels[static_cast<size_t>(i)].kind) {
      case ChannelKind::kCubemap:
        dims[static_cast<size_t>(i)] = SamplerDim::kCube;
        break;
      case ChannelKind::kVolume:
        dims[static_cast<size_t>(i)] = SamplerDim::k3D;
        break;
      default:
        break;
    }
  }
  const std::string fs_src = WrapGles(common, pass.code, dims);
  const GLuint vs = Compile(GL_VERTEX_SHADER, kVertexShader);
  const GLuint fs = Compile(GL_FRAGMENT_SHADER, fs_src.c_str());
  if (vs == 0 || fs == 0) {
    if (vs)
      glDeleteShader(vs);
    if (fs)
      glDeleteShader(fs);
    return false;
  }
  const GLuint program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glDeleteShader(vs);
  glDeleteShader(fs);
  GLint linked = GL_FALSE;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    GLint len = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
    std::string log(static_cast<size_t>(len > 0 ? len : 1), '\0');
    glGetProgramInfoLog(program, len, nullptr, log.data());
    std::fprintf(stderr, "shadertoy: program link failed:\n%s\n", log.c_str());
    glDeleteProgram(program);
    return false;
  }

  out.program = program;
  out.target_buffer = target_buffer;
  out.channels = pass.channels;
  out.loc_resolution = glGetUniformLocation(program, "iResolution");
  out.loc_time = glGetUniformLocation(program, "iTime");
  out.loc_time_delta = glGetUniformLocation(program, "iTimeDelta");
  out.loc_frame_rate = glGetUniformLocation(program, "iFrameRate");
  out.loc_frame = glGetUniformLocation(program, "iFrame");
  out.loc_mouse = glGetUniformLocation(program, "iMouse");
  out.loc_date = glGetUniformLocation(program, "iDate");
  out.loc_sample_rate = glGetUniformLocation(program, "iSampleRate");
  out.loc_channel_res = glGetUniformLocation(program, "iChannelResolution[0]");
  out.loc_channel[0] = glGetUniformLocation(program, "iChannel0");
  out.loc_channel[1] = glGetUniformLocation(program, "iChannel1");
  out.loc_channel[2] = glGetUniformLocation(program, "iChannel2");
  out.loc_channel[3] = glGetUniformLocation(program, "iChannel3");

  // One sampler object per channel, configured from the channel's settings.
  glGenSamplers(4, out.samplers.data());
  for (int i = 0; i < 4; ++i) {
    const Channel& ch = pass.channels[static_cast<size_t>(i)];
    const GLuint s = out.samplers[static_cast<size_t>(i)];
    glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, MinFilter(ch.filter));
    glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, MagFilter(ch.filter));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_S, WrapMode(ch.wrap));
    glSamplerParameteri(s, GL_TEXTURE_WRAP_T, WrapMode(ch.wrap));
  }
  return true;
}

GLuint GlRenderer::StubTexture() {
  if (dummy_tex_ != 0)
    return dummy_tex_;
  glGenTextures(1, &dummy_tex_);
  glBindTexture(GL_TEXTURE_2D, dummy_tex_);
  const std::array<GLubyte, 4> black = {0, 0, 0, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               black.data());
  glBindTexture(GL_TEXTURE_2D, 0);
  tex_dims_[dummy_tex_] = {1.0f, 1.0f, 1.0f};
  return dummy_tex_;
}

// A complete 1x1x6 black cubemap, bound when a channel's faces can't be loaded
// so samplerCube shaders still have something valid to sample.
GLuint GlRenderer::StubTextureCube() {
  if (dummy_cube_ != 0)
    return dummy_cube_;
  glGenTextures(1, &dummy_cube_);
  glBindTexture(GL_TEXTURE_CUBE_MAP, dummy_cube_);
  const std::array<GLubyte, 4> black = {0, 0, 0, 255};
  for (int face = 0; face < 6; ++face)
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(face), 0,
                 GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, black.data());
  glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
  tex_dims_[dummy_cube_] = {1.0f, 1.0f, 1.0f};
  return dummy_cube_;
}

void GlRenderer::SetAudioSource(std::shared_ptr<AudioSource> src) noexcept {
  // Only stop the source we own (the lazily created default), never an injected
  // one the caller (and possibly other renderers) still owns.
  if (audio_ && audio_started_ && !audio_custom_set_)
    audio_->Stop();
  audio_ = std::move(src);
  audio_custom_set_ = true;
  audio_started_ = false;
  audio_failed_ = false;
  audio_frame_ = -1;
}

void GlRenderer::UpdateAudio(const ShaderInputs& in) noexcept {
  if (!uses_audio_ || audio_failed_)
    return;  // audio_failed_ latches a failed open so we retry at most once
  // Lazily create the default microphone source the first time audio is needed.
  if (audio_ == nullptr) {
    if (audio_custom_set_ || !audio_enabled_)
      return;  // explicitly disabled, or a null custom source was set
    audio_ = MakeMicSource();
    if (audio_ == nullptr) {
      audio_failed_ = true;
      return;  // no capture back-end compiled in (kAudio stays a black stub)
    }
  }
  // The renderer manages only the lazily created default source; an injected
  // (possibly shared) source is started and stopped by the caller.
  if (!audio_custom_set_ && !audio_started_) {
    if (!audio_->Start()) {
      audio_.reset();
      audio_failed_ = true;  // no device/permission: give up, do not retry
      return;
    }
    audio_started_ = true;
  }
  if (in.frame == audio_frame_)
    return;  // already uploaded this frame (multiple passes may sample audio)
  audio_frame_ = in.frame;

  unsigned char px[kAudioTexBytes];
  if (!audio_->Fill(px))
    return;
  if (audio_tex_ == 0) {
    glGenTextures(1, &audio_tex_);
    glBindTexture(GL_TEXTURE_2D, audio_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAudioTexWidth, 2, 0, GL_RED,
                 GL_UNSIGNED_BYTE, nullptr);
    tex_dims_[audio_tex_] = {static_cast<float>(kAudioTexWidth), 2.0f, 1.0f};
  } else {
    glBindTexture(GL_TEXTURE_2D, audio_tex_);
  }
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kAudioTexWidth, 2, GL_RED,
                  GL_UNSIGNED_BYTE, px);
  glBindTexture(GL_TEXTURE_2D, 0);
}

std::string GlRenderer::ResolveMediaPath(const std::string& src) const {
  // Map a Shadertoy media src ("/media/a/<hash>.png") to a local file by
  // joining its basename with the media dir.  Without a media dir, return the
  // src verbatim (caller falls back to a stub if it does not exist).
  std::string dir = media_dir_;
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

GLuint GlRenderer::LoadCubemap(const Channel& ch) {
  if (ch.texture_path.empty())
    return StubTextureCube();
  const std::string key = "cube:" + ch.texture_path;
  auto it = textures_.find(key);
  if (it != textures_.end())
    return it->second;

  // Face 0 is the src; faces 1..5 are "<stem>_<i><ext>" beside it.
  const std::filesystem::path base(ResolveMediaPath(ch.texture_path));
  std::array<std::filesystem::path, 6> faces;
  faces[0] = base;
  for (int i = 1; i < 6; ++i)
    faces[static_cast<size_t>(i)] =
        base.parent_path() / (base.stem().string() + "_" + std::to_string(i) +
                              base.extension().string());

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_CUBE_MAP, tex);
  stbi_set_flip_vertically_on_load(0);  // cubemaps are not flipped
  bool ok = true;
  int face_w = 1, face_h = 1;
  for (int i = 0; i < 6 && ok; ++i) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load(faces[static_cast<size_t>(i)].string().c_str(), &w,
                            &h, &comp, 4);
    if (px == nullptr) {
      ok = false;
      break;
    }
    face_w = w;
    face_h = h;
    glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(i), 0,
                 GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    stbi_image_free(px);
  }
  if (!ok) {
    glDeleteTextures(1, &tex);
    glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
    std::fprintf(stderr, "shadertoy: cubemap %s incomplete (using black)\n",
                 ch.texture_path.c_str());
    const GLuint stub = StubTextureCube();
    textures_[key] = stub;  // cache the fallback so we do not retry every frame
    return stub;
  }
  glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
  glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
  tex_dims_[tex] = {static_cast<float>(face_w), static_cast<float>(face_h),
                    1.0f};
  textures_[key] = tex;
  return tex;
}

GLuint GlRenderer::LoadVolume(const Channel& ch) {
  // Shadertoy ships no public volume (.bin) format, so synthesise a
  // deterministic 32^3 RGBA noise volume.  Keyed on the channel src so the same
  // volume is stable across frames and runs.
  const std::string key =
      "vol:" +
      (ch.texture_path.empty() ? std::string("noise") : ch.texture_path);
  auto it = textures_.find(key);
  if (it != textures_.end())
    return it->second;

  constexpr int kN = 32;
  std::vector<std::uint8_t> voxels(static_cast<size_t>(kN) * kN * kN * 4);
  std::mt19937_64 rng(std::hash<std::string>{}(key));
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto& v : voxels)
    v = static_cast<std::uint8_t>(dist(rng));

  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_3D, tex);
  glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA, kN, kN, kN, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, voxels.data());
  glGenerateMipmap(GL_TEXTURE_3D);
  glBindTexture(GL_TEXTURE_3D, 0);
  tex_dims_[tex] = {static_cast<float>(kN), static_cast<float>(kN),
                    static_cast<float>(kN)};
  textures_[key] = tex;
  return tex;
}

GLuint GlRenderer::TextureFor(const Channel& ch) {
  if (ch.kind != ChannelKind::kTexture || ch.texture_path.empty())
    return StubTexture();
  auto it = textures_.find(ch.texture_path);
  if (it != textures_.end())
    return it->second;

  const std::string path = ResolveMediaPath(ch.texture_path);
  stbi_set_flip_vertically_on_load(ch.vflip ? 1 : 0);
  int w = 0, h = 0, comp = 0;
  stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
  if (pixels == nullptr) {
    std::fprintf(stderr, "shadertoy: cannot load texture %s (using black)\n",
                 ch.texture_path.c_str());
    textures_[ch.texture_path] = StubTexture();
    return dummy_tex_;
  }
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               pixels);
  glGenerateMipmap(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
  stbi_image_free(pixels);
  tex_dims_[tex] = {static_cast<float>(w), static_cast<float>(h), 1.0f};
  textures_[ch.texture_path] = tex;
  return tex;
}

void GlRenderer::EnsureBuffers(int w, int h) {
  if (w == fb_w_ && h == fb_h_)
    return;
  fb_w_ = w;
  fb_h_ = h;
  for (int b = 0; b < kNumBuffers; ++b) {
    if (!buffer_used_[static_cast<size_t>(b)])
      continue;
    BufferGL& buf = buffers_[static_cast<size_t>(b)];
    for (int i = 0; i < 2; ++i) {
      if (buf.tex[i] == 0)
        glGenTextures(1, &buf.tex[i]);
      glBindTexture(GL_TEXTURE_2D, buf.tex[i]);
      // RGBA16F: filterable in core ES3, renderable with
      // EXT_color_buffer_float.
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA,
                   GL_HALF_FLOAT, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      if (buf.fbo[i] == 0)
        glGenFramebuffers(1, &buf.fbo[i]);
      glBindFramebuffer(GL_FRAMEBUFFER, buf.fbo[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                             GL_TEXTURE_2D, buf.tex[i], 0);
      glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
    }
    buf.front = 0;
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GlRenderer::RenderPass(const PassGL& p, const ShaderInputs& in) noexcept {
  if (p.target_buffer < 0) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // Shadertoy ignores the Image pass's alpha and always presents an opaque
    // frame.  Clear the destination alpha to 1 and mask alpha writes so the
    // compositor never blends the window with whatever is behind it.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
  } else {
    const BufferGL& buf = buffers_[static_cast<size_t>(p.target_buffer)];
    glBindFramebuffer(GL_FRAMEBUFFER, buf.fbo[1 - buf.front]);  // write to back
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE,
                GL_TRUE);  // buffers keep their alpha
  }
  glViewport(0, 0, fb_w_, fb_h_);
  glUseProgram(p.program);

  if (p.loc_resolution >= 0)
    glUniform3f(p.loc_resolution, static_cast<float>(fb_w_),
                static_cast<float>(fb_h_), 1.0f);
  if (p.loc_time >= 0)
    glUniform1f(p.loc_time, in.time);
  if (p.loc_time_delta >= 0)
    glUniform1f(p.loc_time_delta, in.time_delta);
  if (p.loc_frame_rate >= 0)
    glUniform1f(p.loc_frame_rate, in.frame_rate);
  if (p.loc_frame >= 0)
    glUniform1i(p.loc_frame, in.frame);
  if (p.loc_mouse >= 0)
    glUniform4f(p.loc_mouse, in.mouse_x, in.mouse_y, in.mouse_z, in.mouse_w);
  if (p.loc_date >= 0)
    glUniform4f(p.loc_date, in.date_y, in.date_m, in.date_d, in.date_s);
  if (p.loc_sample_rate >= 0)
    glUniform1f(p.loc_sample_rate, in.sample_rate);

  std::array<float, 12> chan_res{};
  for (int i = 0; i < 4; ++i) {
    const Channel& ch = p.channels[static_cast<size_t>(i)];
    GLuint tex = 0;
    GLenum target = GL_TEXTURE_2D;
    float rw = 1.0f, rh = 1.0f, rz = 1.0f;  // iChannelResolution (avoid /0)
    auto* self = const_cast<GlRenderer*>(this);
    if (ch.kind == ChannelKind::kBuffer && uses_buffer_internal(ch.buffer)) {
      const BufferGL& src = buffers_[static_cast<size_t>(ch.buffer)];
      tex = src.tex[src.front];  // read previous frame's output
      rw = static_cast<float>(fb_w_);
      rh = static_cast<float>(fb_h_);
    } else {
      if (ch.kind == ChannelKind::kTexture) {
        tex = self->TextureFor(ch);
      } else if (ch.kind == ChannelKind::kCubemap) {
        target = GL_TEXTURE_CUBE_MAP;
        tex = self->LoadCubemap(ch);  // real faces if media present, else black
      } else if (ch.kind == ChannelKind::kVolume) {
        target = GL_TEXTURE_3D;
        tex = self->LoadVolume(ch);  // 32^3 procedural noise
      } else if (ch.kind == ChannelKind::kAudio && audio_tex_ != 0) {
        tex = audio_tex_;  // live FFT/waveform (UpdateAudio ran this frame)
      } else {
        tex = self->StubTexture();  // keyboard / silent audio / unbound
      }
      const auto it = tex_dims_.find(tex);
      if (it != tex_dims_.end()) {
        rw = it->second[0];
        rh = it->second[1];
        rz = it->second[2];
      }
    }
    chan_res[static_cast<size_t>(i) * 3 + 0] = rw;
    chan_res[static_cast<size_t>(i) * 3 + 1] = rh;
    chan_res[static_cast<size_t>(i) * 3 + 2] = rz;
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
    glBindTexture(target, tex);
    glBindSampler(static_cast<GLuint>(i), p.samplers[static_cast<size_t>(i)]);
    if (p.loc_channel[static_cast<size_t>(i)] >= 0)
      glUniform1i(p.loc_channel[static_cast<size_t>(i)], i);
  }
  if (p.loc_channel_res >= 0)
    glUniform3fv(p.loc_channel_res, 4, chan_res.data());

  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
}

bool GlRenderer::uses_buffer_internal(int b) const noexcept {
  return b >= 0 && b < kNumBuffers && buffer_used_[static_cast<size_t>(b)];
}

bool GlRenderer::SetProgram(const ShaderProgram& program) {
  if (vao_ == 0)
    glGenVertexArrays(1, &vao_);

  // Build into temporaries first so a failure leaves the current program live.
  std::vector<PassGL> new_buffers;
  PassGL new_image;
  std::array<bool, kNumBuffers> used = program.buffer_used;

  for (int b = 0; b < kNumBuffers; ++b) {
    if (!program.uses_buffer(b))
      continue;
    PassGL pg;
    if (!BuildPass(program.common, program.buffers[static_cast<size_t>(b)], b,
                   pg)) {
      for (PassGL& done : new_buffers)
        DestroyPass(done);
      return false;
    }
    new_buffers.push_back(std::move(pg));
  }
  if (!BuildPass(program.common, program.image, -1, new_image)) {
    for (PassGL& done : new_buffers)
      DestroyPass(done);
    return false;
  }

  // Swap in the new program; tear down the old one.
  for (PassGL& p : buffer_passes_)
    DestroyPass(p);
  DestroyPass(image_pass_);
  buffer_passes_ = std::move(new_buffers);
  image_pass_ = std::move(new_image);
  buffer_used_ = used;

  // Does any pass sample an audio channel?  Drives lazy mic capture in Render.
  uses_audio_ = false;
  auto pass_uses_audio = [](const Pass& p) {
    for (const Channel& c : p.channels)
      if (c.kind == ChannelKind::kAudio)
        return true;
    return false;
  };
  uses_audio_ = pass_uses_audio(program.image);
  for (int b = 0; b < kNumBuffers && !uses_audio_; ++b)
    if (program.uses_buffer(b))
      uses_audio_ = pass_uses_audio(program.buffers[static_cast<size_t>(b)]);
  // Release the microphone when switching to a shader that does not use it
  // (only the source we own; an injected one is the caller's to manage).
  if (!uses_audio_ && audio_ && audio_started_ && !audio_custom_set_) {
    audio_->Stop();
    audio_started_ = false;
  }
  audio_failed_ = false;  // re-attempt the device for the new program
  audio_frame_ = -1;

  // Force buffer (re)allocation on the next Render.
  fb_w_ = 0;
  fb_h_ = 0;
  ready_ = true;
  return true;
}

bool GlRenderer::Init(const std::string& image_shader) {
  return SetProgram(MakeSinglePass(image_shader));
}

void GlRenderer::Render(const ShaderInputs& inputs) noexcept {
  if (!ready_)
    return;
  const int w = static_cast<int>(inputs.res_x);
  const int h = static_cast<int>(inputs.res_y);
  if (w <= 0 || h <= 0)
    return;
  EnsureBuffers(w, h);
  UpdateAudio(
      inputs);  // capture → audio_tex_ (once per frame; no-op if unused)

  // All passes read buffers' front (previous frame) and write to backs; swap
  // once at the end (Shadertoy double-buffer semantics).
  for (const PassGL& bp : buffer_passes_)
    RenderPass(bp, inputs);
  RenderPass(image_pass_, inputs);

  for (int b = 0; b < kNumBuffers; ++b) {
    if (buffer_used_[static_cast<size_t>(b)])
      buffers_[static_cast<size_t>(b)].front ^= 1;
  }
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);  // image pass masked alpha
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GlRenderer::DestroyPass(PassGL& p) noexcept {
  if (p.program) {
    glDeleteProgram(p.program);
    p.program = 0;
  }
  if (p.samplers[0] != 0) {
    glDeleteSamplers(4, p.samplers.data());
    p.samplers = {0, 0, 0, 0};
  }
}

void GlRenderer::Destroy() noexcept {
  for (PassGL& p : buffer_passes_)
    DestroyPass(p);
  buffer_passes_.clear();
  DestroyPass(image_pass_);
  for (BufferGL& buf : buffers_) {
    for (int i = 0; i < 2; ++i) {
      if (buf.tex[i])
        glDeleteTextures(1, &buf.tex[i]);
      if (buf.fbo[i])
        glDeleteFramebuffers(1, &buf.fbo[i]);
    }
    buf = BufferGL{};
  }
  for (auto& kv : textures_) {
    // Skip the shared stubs; they are deleted once, below.
    if (kv.second != 0 && kv.second != dummy_tex_ && kv.second != dummy_cube_)
      glDeleteTextures(1, &kv.second);
  }
  textures_.clear();
  tex_dims_.clear();
  if (dummy_tex_) {
    glDeleteTextures(1, &dummy_tex_);
    dummy_tex_ = 0;
  }
  if (dummy_cube_) {
    glDeleteTextures(1, &dummy_cube_);
    dummy_cube_ = 0;
  }
  if (audio_tex_) {
    glDeleteTextures(1, &audio_tex_);
    audio_tex_ = 0;
  }
  if (audio_ && audio_started_ && !audio_custom_set_)
    audio_->Stop();  // an injected source is the caller's to stop
  audio_.reset();
  audio_started_ = false;
  audio_failed_ = false;
  audio_custom_set_ = false;
  uses_audio_ = false;
  audio_frame_ = -1;
  if (vao_) {
    glDeleteVertexArrays(1, &vao_);
    vao_ = 0;
  }
  buffer_used_ = {};
  fb_w_ = fb_h_ = 0;
  ready_ = false;
}

}  // namespace shadertoy
