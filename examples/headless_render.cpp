// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// headless_render — render a Shadertoy shader to a PPM via an offscreen EGL
// pbuffer.  Needs no compositor, so it is handy for CI snapshots and for
// eyeballing a shader without a window.  Force the software rasterizer with
// LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe to render where there is no
// GPU (and so a runaway shader can't trip a GPU watchdog).
//
//   headless_render <shader.json> <out.ppm> [media_dir] [W H frames time]

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <shadertoy/gl_renderer.hpp>
#include <shadertoy/inputs.hpp>
#include <shadertoy/loader.hpp>
#include <shadertoy/program.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr,
                 "usage: %s <shader.json> <out.ppm> [media_dir] [W H frames time]\n",
                 argv[0]);
    return 2;
  }
  const char* json = argv[1];
  const char* out = argv[2];
  const std::string media = argc > 3 ? argv[3] : "";
  const int W = argc > 5 ? std::atoi(argv[4]) : 800;
  const int H = argc > 5 ? std::atoi(argv[5]) : 450;
  const int frames = argc > 6 ? std::atoi(argv[6]) : 8;   // a few, so feedback settles
  const float t = argc > 7 ? static_cast<float>(std::atof(argv[7])) : 6.0f;

  EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
    std::fprintf(stderr, "headless_render: eglInitialize failed\n");
    return 1;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                             EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                             EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig cfg;
  EGLint n = 0;
  if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n == 0) {
    std::fprintf(stderr, "headless_render: no EGL config\n");
    return 1;
  }
  const EGLint pb_attr[] = {EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE};
  EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
  const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT ||
      !eglMakeCurrent(dpy, surf, surf, ctx)) {
    std::fprintf(stderr, "headless_render: pbuffer/context setup failed\n");
    return 1;
  }
  std::fprintf(stderr, "GL: %s | %s\n", glGetString(GL_VERSION),
               glGetString(GL_RENDERER));

  shadertoy::ShaderProgram prog;
  std::string err;
  if (!shadertoy::LoadShadertoyJsonFile(json, prog, &err)) {
    std::fprintf(stderr, "headless_render: load failed: %s\n", err.c_str());
    return 1;
  }
  shadertoy::GlRenderer renderer;
  if (!media.empty())
    renderer.SetMediaDir(media);
  if (!renderer.SetProgram(prog)) {
    std::fprintf(stderr, "headless_render: SetProgram failed\n");
    return 1;
  }

  shadertoy::ShaderInputs in;
  in.res_x = static_cast<float>(W);
  in.res_y = static_cast<float>(H);
  in.res_z = 1.0f;
  for (int f = 0; f < frames; ++f) {
    in.frame = f;
    in.time = t * static_cast<float>(f + 1) / static_cast<float>(frames);
    in.time_delta = t / static_cast<float>(frames);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, W, H);
    renderer.Render(in);
  }

  std::vector<unsigned char> px(static_cast<size_t>(W) * H * 4);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px.data());

  FILE* fp = std::fopen(out, "wb");
  if (fp == nullptr) {
    std::fprintf(stderr, "headless_render: cannot open %s\n", out);
    return 1;
  }
  std::fprintf(fp, "P6\n%d %d\n255\n", W, H);
  for (int y = H - 1; y >= 0; --y)  // flip: GL origin is bottom-left
    for (int x = 0; x < W; ++x) {
      const unsigned char* p = &px[(static_cast<size_t>(y) * W + x) * 4];
      std::fputc(p[0], fp);
      std::fputc(p[1], fp);
      std::fputc(p[2], fp);
    }
  std::fclose(fp);
  std::fprintf(stderr, "headless_render: wrote %s (%dx%d)\n", out, W, H);
  return 0;
}
