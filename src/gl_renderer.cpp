// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// gl_renderer.cpp — OpenGL ES 3.0 Shadertoy renderer implementation.

#include "shadertoy/gl_renderer.hpp"

#include <array>
#include <cstdio>
#include <string>

namespace shadertoy {

namespace {

// Full-screen triangle generated from gl_VertexID — no vertex buffer needed.
constexpr const char* kVertexShader = R"GLSL(#version 300 es
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

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
    std::fprintf(stderr, "shadertoy: GLSL %s shader compile failed:\n%s\n",
                 type == GL_VERTEX_SHADER ? "vertex" : "fragment", log.c_str());
    glDeleteShader(shader);
    return 0;
  }
  return shader;
}

void GlRenderer::PrintProgramLog(GLuint program, const char* stage) {
  GLint len = 0;
  glGetProgramiv(program, GL_INFO_LOG_LENGTH, &len);
  std::string log(static_cast<size_t>(len > 0 ? len : 1), '\0');
  glGetProgramInfoLog(program, len, nullptr, log.data());
  std::fprintf(stderr, "shadertoy: program %s failed:\n%s\n", stage,
               log.c_str());
}

bool GlRenderer::Init(const std::string& image_shader) {
  const std::string fs_src = WrapGles(image_shader);

  const GLuint vs = Compile(GL_VERTEX_SHADER, kVertexShader);
  const GLuint fs = Compile(GL_FRAGMENT_SHADER, fs_src.c_str());
  if (vs == 0 || fs == 0) {
    if (vs)
      glDeleteShader(vs);
    if (fs)
      glDeleteShader(fs);
    return false;
  }

  program_ = glCreateProgram();
  glAttachShader(program_, vs);
  glAttachShader(program_, fs);
  glLinkProgram(program_);
  glDeleteShader(vs);
  glDeleteShader(fs);

  GLint linked = GL_FALSE;
  glGetProgramiv(program_, GL_LINK_STATUS, &linked);
  if (linked != GL_TRUE) {
    PrintProgramLog(program_, "link");
    glDeleteProgram(program_);
    program_ = 0;
    return false;
  }

  CacheUniformLocations();
  SetupGeometry();
  SetupDummyChannels();
  return true;
}

void GlRenderer::CacheUniformLocations() noexcept {
  loc_resolution_ = glGetUniformLocation(program_, "iResolution");
  loc_time_ = glGetUniformLocation(program_, "iTime");
  loc_time_delta_ = glGetUniformLocation(program_, "iTimeDelta");
  loc_frame_rate_ = glGetUniformLocation(program_, "iFrameRate");
  loc_frame_ = glGetUniformLocation(program_, "iFrame");
  loc_mouse_ = glGetUniformLocation(program_, "iMouse");
  loc_date_ = glGetUniformLocation(program_, "iDate");
  loc_sample_rate_ = glGetUniformLocation(program_, "iSampleRate");
  loc_channel_[0] = glGetUniformLocation(program_, "iChannel0");
  loc_channel_[1] = glGetUniformLocation(program_, "iChannel1");
  loc_channel_[2] = glGetUniformLocation(program_, "iChannel2");
  loc_channel_[3] = glGetUniformLocation(program_, "iChannel3");
}

void GlRenderer::SetupGeometry() noexcept {
  // GLES 3 core profile requires a bound VAO even for attribute-less draws.
  glGenVertexArrays(1, &vao_);
}

void GlRenderer::SetupDummyChannels() noexcept {
  glGenTextures(1, &dummy_tex_);
  glBindTexture(GL_TEXTURE_2D, dummy_tex_);
  const std::array<GLubyte, 4> black = {0, 0, 0, 255};
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE,
               black.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);
}

void GlRenderer::Render(const ShaderInputs& inputs) noexcept {
  glViewport(0, 0, static_cast<GLsizei>(inputs.res_x),
             static_cast<GLsizei>(inputs.res_y));
  glUseProgram(program_);

  if (loc_resolution_ >= 0)
    glUniform3f(loc_resolution_, inputs.res_x, inputs.res_y, inputs.res_z);
  if (loc_time_ >= 0)
    glUniform1f(loc_time_, inputs.time);
  if (loc_time_delta_ >= 0)
    glUniform1f(loc_time_delta_, inputs.time_delta);
  if (loc_frame_rate_ >= 0)
    glUniform1f(loc_frame_rate_, inputs.frame_rate);
  if (loc_frame_ >= 0)
    glUniform1i(loc_frame_, inputs.frame);
  if (loc_mouse_ >= 0)
    glUniform4f(loc_mouse_, inputs.mouse_x, inputs.mouse_y, inputs.mouse_z,
                inputs.mouse_w);
  if (loc_date_ >= 0)
    glUniform4f(loc_date_, inputs.date_y, inputs.date_m, inputs.date_d,
                inputs.date_s);
  if (loc_sample_rate_ >= 0)
    glUniform1f(loc_sample_rate_, inputs.sample_rate);

  // Bind the dummy black texture to all four channel samplers so shaders
  // referencing iChannelN sample (0,0,0,1) rather than failing.
  for (int i = 0; i < 4; ++i) {
    const GLint loc = loc_channel_.at(static_cast<size_t>(i));
    if (loc >= 0) {
      glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
      glBindTexture(GL_TEXTURE_2D, dummy_tex_);
      glUniform1i(loc, i);
    }
  }

  glBindVertexArray(vao_);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
}

void GlRenderer::Destroy() noexcept {
  if (dummy_tex_) {
    glDeleteTextures(1, &dummy_tex_);
    dummy_tex_ = 0;
  }
  if (vao_) {
    glDeleteVertexArrays(1, &vao_);
    vao_ = 0;
  }
  if (program_) {
    glDeleteProgram(program_);
    program_ = 0;
  }
}

}  // namespace shadertoy
