// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// spirv_compile.cpp — runtime GLSL→SPIR-V via glslangValidator / glslc.
//
// Always compiled. When the build also links glslang (SHADERTOY_GLSLANG_LINK),
// CompileToSpirv prefers that and this path remains as the escape hatch
// SHADERTOY_GLSLANG selects: a deployment may want a distro-managed compiler it
// can update without rebuilding, and an image may ship the tool without its
// development files.

#include "shadertoy/spirv_compile.hpp"

#include "spirv_compile_backend.hpp"

extern "C" {
#include <sys/wait.h>
#include <unistd.h>
}

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace shadertoy {

#if !SHADERTOY_HAVE_GLSLANG_LIB
// No compiler linked into this build: the subprocess path is the only one.
// Defined here rather than behind a weak symbol so a build that wires up
// neither backend fails to link instead of silently returning nothing.
bool HaveLinkedSpirvCompiler() {
  return false;
}
std::vector<uint32_t> CompileToSpirvLinked(const std::string& /*glsl_source*/,
                                           ShaderStage /*stage*/) {
  return {};
}
#endif

namespace {

// Create a unique temp file from @p tmpl (an mkstemps template ending in the
// desired suffix) and write @p text into it.  mkstemps creates the file
// race-free; we then write through a stream to keep the I/O free of raw
// pointer arithmetic.  Returns the path, or "" on failure.
[[nodiscard]] std::string WriteTempFile(std::string tmpl,
                                        const std::string& text,
                                        int suffix_len) {
  const int fd = ::mkstemps(tmpl.data(), suffix_len);
  if (fd < 0)
    return {};
  ::close(fd);  // reopen via stream; the unique name is already reserved
  std::ofstream out(tmpl, std::ios::binary | std::ios::trunc);
  if (!out) {
    ::unlink(tmpl.c_str());
    return {};
  }
  out << text;
  out.close();
  if (!out) {
    ::unlink(tmpl.c_str());
    return {};
  }
  return tmpl;
}

// Run a NULL-terminated argv and return its exit status, or -1 on spawn
// failure.  stdout/stderr are inherited so compiler diagnostics reach the user.
[[nodiscard]] int RunCompiler(const std::vector<char*>& argv) {
  const pid_t pid = ::fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    ::execvp(argv.front(), argv.data());
    ::_exit(127);  // reached only if exec failed
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR)
      return -1;
  }
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return -1;
}

[[nodiscard]] std::vector<uint32_t> ReadSpirv(const std::string& path) {
  std::vector<uint32_t> words;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f)
    return words;
  std::fseek(f, 0, SEEK_END);
  const long size = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (size > 0 && (size % 4) == 0) {
    words.resize(static_cast<size_t>(size) / 4);
    if (std::fread(words.data(), 1, static_cast<size_t>(size), f) !=
        static_cast<size_t>(size)) {
      words.clear();
    }
  }
  std::fclose(f);
  return words;
}

}  // namespace

std::vector<uint32_t> CompileToSpirv(const std::string& glsl_source,
                                     ShaderStage stage) {
  // SHADERTOY_GLSLANG names a binary, so setting it is a deliberate request for
  // the subprocess path -- the escape hatch that lets a deployment swap the
  // compiler without rebuilding, and the reason the linked backend is an option
  // rather than a replacement.
  const char* override_bin = std::getenv("SHADERTOY_GLSLANG");
  if (HaveLinkedSpirvCompiler() &&
      (override_bin == nullptr || *override_bin == '\0')) {
    // Authoritative when present: a failure here is the shader's, and retrying
    // it through a subprocess would only report the same error twice.
    return CompileToSpirvLinked(glsl_source, stage);
  }

  const char* tmpdir_env = std::getenv("TMPDIR");
  const std::string tmpdir = (tmpdir_env && *tmpdir_env) ? tmpdir_env : "/tmp";

  // glslangValidator / glslc infer the shader stage from the file extension.
  const char* ext = stage == ShaderStage::kVertex ? "/shadertoy-XXXXXX.vert"
                                                  : "/shadertoy-XXXXXX.frag";
  const char* glslc_stage = stage == ShaderStage::kVertex
                                ? "-fshader-stage=vertex"
                                : "-fshader-stage=fragment";

  std::string in_path = WriteTempFile(tmpdir + ext, glsl_source, 5);
  if (in_path.empty()) {
    std::fprintf(stderr, "shadertoy: failed to create temp shader file\n");
    return {};
  }
  std::string out_path = WriteTempFile(tmpdir + "/shadertoy-XXXXXX.spv", "", 4);
  if (out_path.empty()) {
    ::unlink(in_path.c_str());
    return {};
  }

  std::vector<uint32_t> spirv;
  auto try_glslang = [&](const char* prog) -> bool {
    // glslangValidator -V <in> -o <out>
    std::string p = prog, v = "-V", o = "-o";
    std::vector<char*> argv = {p.data(), v.data(),        in_path.data(),
                               o.data(), out_path.data(), nullptr};
    if (RunCompiler(argv) != 0)
      return false;
    spirv = ReadSpirv(out_path);
    return !spirv.empty();
  };
  auto try_glslc = [&](const char* prog) -> bool {
    // glslc -fshader-stage=<stage> <in> -o <out>
    std::string p = prog, st = glslc_stage, o = "-o";
    std::vector<char*> argv = {p.data(), st.data(),       in_path.data(),
                               o.data(), out_path.data(), nullptr};
    if (RunCompiler(argv) != 0)
      return false;
    spirv = ReadSpirv(out_path);
    return !spirv.empty();
  };

  bool ok = false;
  if (override_bin && *override_bin) {
    ok = std::strstr(override_bin, "glslc") ? try_glslc(override_bin)
                                            : try_glslang(override_bin);
  } else {
    ok = try_glslang("glslangValidator") || try_glslc("glslc");
  }

  ::unlink(in_path.c_str());
  ::unlink(out_path.c_str());

  if (!ok) {
    std::fprintf(stderr,
                 "shadertoy: SPIR-V compilation failed (need glslangValidator "
                 "or glslc on PATH, or set SHADERTOY_GLSLANG)\n");
    return {};
  }
  return spirv;
}

}  // namespace shadertoy
