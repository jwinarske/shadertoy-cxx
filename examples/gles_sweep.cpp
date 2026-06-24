// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// gles_sweep — headless GL ES 3.0 compile sweep for Shadertoy shaders.
//
// Creates a surfaceless EGL context and compiles every pass of every shader
// (the WrapGles output, exactly as shadertoy_egl would) with the real driver.
// This is the ground-truth answer to "will it run on shadertoy_egl".
//
//   gles_sweep <dir-with-json>
//
// Prints one line per FAILING pass to stdout:
//   <path>\t<pass>\t<first-error-line>
// Summary to stderr.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <shadertoy/inputs.hpp>
#include <shadertoy/loader.hpp>
#include <shadertoy/program.hpp>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

EGLDisplay InitEgl(EGLContext& ctx) {
  auto get_platform_display =
      reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
          eglGetProcAddress("eglGetPlatformDisplayEXT"));
  EGLDisplay dpy = EGL_NO_DISPLAY;
  if (get_platform_display)
    dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY,
                               nullptr);
  if (dpy == EGL_NO_DISPLAY)
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
    std::fprintf(stderr, "gles_sweep: eglInitialize failed\n");
    return EGL_NO_DISPLAY;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
                             EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                             EGL_NONE};
  EGLConfig cfg;
  EGLint n = 0;
  if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &n) || n == 0) {
    std::fprintf(stderr, "gles_sweep: eglChooseConfig failed\n");
    return EGL_NO_DISPLAY;
  }
  const EGLint ctx_attr[] = {EGL_CONTEXT_MAJOR_VERSION, 3,
                             EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
  ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
  if (ctx == EGL_NO_CONTEXT) {
    std::fprintf(stderr, "gles_sweep: eglCreateContext failed\n");
    return EGL_NO_DISPLAY;
  }
  if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
    std::fprintf(stderr, "gles_sweep: eglMakeCurrent (surfaceless) failed\n");
    return EGL_NO_DISPLAY;
  }
  return dpy;
}

// Compile one fragment shader; return "" on success, else the first error line.
std::string CompileFrag(const std::string& src) {
  const GLuint sh = glCreateShader(GL_FRAGMENT_SHADER);
  const char* p = src.c_str();
  glShaderSource(sh, 1, &p, nullptr);
  glCompileShader(sh);
  GLint ok = GL_FALSE;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  std::string err;
  if (!ok) {
    GLint len = 0;
    glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
    std::string log(len > 0 ? static_cast<size_t>(len) : 256, '\0');
    glGetShaderInfoLog(sh, static_cast<GLsizei>(log.size()), nullptr,
                       log.data());
    // Prefer the first line containing "error"; fall back to first non-empty
    // (a leading "warning:" line never fails compilation, so skip past it).
    std::string first_nonempty;
    size_t a = 0;
    while (a < log.size()) {
      size_t b = log.find('\n', a);
      if (b == std::string::npos) b = log.size();
      std::string line = log.substr(a, b - a);
      while (!line.empty() && (line.back() == '\r' || line.back() == '\0'))
        line.pop_back();
      if (!line.empty()) {
        if (first_nonempty.empty()) first_nonempty = line;
        if (line.find("error") != std::string::npos) { err = line; break; }
      }
      a = b + 1;
    }
    if (err.empty()) err = first_nonempty;
    if (err.empty()) err = "compile failed (empty log)";
  }
  glDeleteShader(sh);
  return err;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <dir-with-json>\n", argv[0]);
    return 2;
  }
  EGLContext ctx = EGL_NO_CONTEXT;
  EGLDisplay dpy = InitEgl(ctx);
  if (dpy == EGL_NO_DISPLAY) return 1;
  std::fprintf(stderr, "GL_VERSION: %s | GLSL: %s\n",
               glGetString(GL_VERSION),
               glGetString(GL_SHADING_LANGUAGE_VERSION));

  const char* names[] = {"BufA", "BufB", "BufC", "BufD"};
  size_t total = 0, load_fail = 0, compile_fail = 0;
  std::vector<fs::path> files;
  for (const auto& e : fs::directory_iterator(argv[1]))
    if (e.path().extension() == ".json") files.push_back(e.path());

  for (const auto& f : files) {
    ++total;
    shadertoy::ShaderProgram prog;
    std::string lerr;
    if (!shadertoy::LoadShadertoyJsonFile(f.string(), prog, &lerr)) {
      ++load_fail;
      std::printf("%s\t(load)\t%s\n", f.c_str(), lerr.c_str());
      continue;
    }
    bool any_fail = false;
    auto do_pass = [&](const char* label, const shadertoy::Pass& pass) {
      const std::string gl = shadertoy::WrapGles(prog.common, pass.code);
      const std::string e = CompileFrag(gl);
      if (!e.empty()) {
        any_fail = true;
        std::printf("%s\t%s\t%s\n", f.c_str(), label, e.c_str());
      }
    };
    for (int b = 0; b < shadertoy::kNumBuffers; ++b)
      if (prog.uses_buffer(b))
        do_pass(names[b], prog.buffers[static_cast<size_t>(b)]);
    do_pass("Image", prog.image);
    if (any_fail) ++compile_fail;
  }

  std::fprintf(stderr,
               "gles_sweep: %zu shaders | load-fail %zu | compile-fail %zu | "
               "clean %zu\n",
               total, load_fail, compile_fail,
               total - load_fail - compile_fail);
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglTerminate(dpy);
  return 0;
}
