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
#include <fcntl.h>
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
                                           ShaderStage /*stage*/,
                                           std::string* /*log*/) {
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
// failure.
//
// With @p capture_path set, the child's stdout and stderr are redirected there
// rather than inherited, so the caller can read the diagnostics back. A file
// rather than a pipe: the compiler can outproduce a pipe buffer on a shader
// with many errors, and a parent that waits before draining would deadlock.
[[nodiscard]] int RunCompiler(const std::vector<char*>& argv,
                              const char* capture_path = nullptr) {
  const pid_t pid = ::fork();
  if (pid < 0)
    return -1;
  if (pid == 0) {
    if (capture_path != nullptr) {
      // Append rather than truncate: CompileToSpirv may try glslangValidator
      // and then glslc, and a truncating second attempt erases the first's
      // diagnostics -- so a shader that a present compiler rejected reads as
      // "no compiler found" when the fallback is missing. The file is unique
      // per call, so there is nothing stale to append to.
      const int fd = ::open(capture_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
      if (fd >= 0) {
        ::dup2(fd, STDOUT_FILENO);
        ::dup2(fd, STDERR_FILENO);
        ::close(fd);
      }
    }
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

// Read a captured diagnostics file back, capped: a shader with hundreds of
// errors should not hand a host an unbounded string to render.
[[nodiscard]] std::string ReadCapture(const std::string& path) {
  constexpr std::streamsize kMaxLog = 64 * 1024;
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  std::string text((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  if (static_cast<std::streamsize>(text.size()) > kMaxLog) {
    text.resize(static_cast<size_t>(kMaxLog));
    text.append("\n... (truncated)");
  }
  return text;
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
                                     ShaderStage stage,
                                     std::string* log) {
  // SHADERTOY_GLSLANG names a binary, so setting it is a deliberate request for
  // the subprocess path -- the escape hatch that lets a deployment swap the
  // compiler without rebuilding, and the reason the linked backend is an option
  // rather than a replacement.
  const char* override_bin = std::getenv("SHADERTOY_GLSLANG");
  if (HaveLinkedSpirvCompiler() &&
      (override_bin == nullptr || *override_bin == '\0')) {
    // Authoritative when present: a failure here is the shader's, and retrying
    // it through a subprocess would only report the same error twice.
    return CompileToSpirvLinked(glsl_source, stage, log);
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

  // Only captured when the caller asked for it: redirecting otherwise would
  // take the compiler's output away from the terminal for no gain.
  const std::string capture =
      log != nullptr ? WriteTempFile(tmpdir + "/shadertoy-XXXXXX.log", "", 4)
                     : std::string();
  const char* capture_path = capture.empty() ? nullptr : capture.c_str();

  std::vector<uint32_t> spirv;
  auto try_glslang = [&](const char* prog) -> bool {
    // glslangValidator -V <in> -o <out>
    std::string p = prog, v = "-V", o = "-o";
    std::vector<char*> argv = {p.data(), v.data(),        in_path.data(),
                               o.data(), out_path.data(), nullptr};
    if (RunCompiler(argv, capture_path) != 0)
      return false;
    spirv = ReadSpirv(out_path);
    return !spirv.empty();
  };
  auto try_glslc = [&](const char* prog) -> bool {
    // glslc -fshader-stage=<stage> <in> -o <out>
    std::string p = prog, st = glslc_stage, o = "-o";
    std::vector<char*> argv = {p.data(), st.data(),       in_path.data(),
                               o.data(), out_path.data(), nullptr};
    if (RunCompiler(argv, capture_path) != 0)
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

  // Echo what was captured, so redirecting the child does not cost the
  // developer the output they would otherwise have seen inherited.
  //
  // Only fold it into the caller's log on failure: glslangValidator writes the
  // input filename to stdout on success, so appending unconditionally leaves a
  // non-empty log after a clean compile -- which a host reads as a failure
  // with a nonsense message.
  if (capture_path != nullptr) {
    std::string text = ReadCapture(capture);
    if (!text.empty()) {
      std::fputs(text.c_str(), stderr);
      if (log != nullptr && !ok) {
        log->append(text);
      }
    }
    ::unlink(capture.c_str());
  }
  ::unlink(in_path.c_str());
  ::unlink(out_path.c_str());

  if (!ok) {
    constexpr const char* kNoCompiler =
        "SPIR-V compilation failed (need glslangValidator or glslc on PATH, or "
        "set SHADERTOY_GLSLANG)";
    std::fprintf(stderr, "shadertoy: %s\n", kNoCompiler);
    if (log != nullptr && log->empty()) {
      // Nothing captured means no compiler ran at all, which is a different
      // problem from a shader the compiler rejected -- say which.
      log->assign(kNoCompiler);
    }
    return {};
  }
  return spirv;
}

}  // namespace shadertoy
