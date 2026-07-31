// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// spirv_compile.hpp — runtime GLSL→SPIR-V compilation.
//
// To support *any* Shadertoy shader at runtime the Vulkan back-end must turn
// GLSL into SPIR-V on the fly.  Rather than take a hard link-time dependency on
// libshaderc/glslang (often unpackaged), the implementation drives the standard
// command-line compilers — `glslangValidator` or `glslc`, shipped with the
// Vulkan SDK / mesa tools — via fork+exec.  Override the binary with the
// SHADERTOY_GLSLANG environment variable.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace shadertoy {

enum class ShaderStage { kVertex, kFragment };

/// Compile a complete Vulkan GLSL shader (@p glsl_source) of the given
/// @p stage to SPIR-V.  Returns the SPIR-V words, or an empty vector on
/// failure.
///
/// When @p log is non-null it receives the compiler's diagnostics — the reason
/// a shader was rejected, with line numbers — so a host can show them rather
/// than only reporting that something failed. Diagnostics still go to stderr
/// either way, since that is where a developer looks first. The linked and
/// subprocess back-ends both fill it; the subprocess one captures the child's
/// stderr to do so.
[[nodiscard]] std::vector<uint32_t> CompileToSpirv(
    const std::string& glsl_source,
    ShaderStage stage,
    std::string* log = nullptr);

}  // namespace shadertoy
