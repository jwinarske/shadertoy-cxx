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
/// @p stage to SPIR-V.  Returns the SPIR-V words, or an empty vector on failure
/// (diagnostics are printed to stderr by the underlying compiler).
[[nodiscard]] std::vector<uint32_t> CompileToSpirv(const std::string& glsl_source,
                                                   ShaderStage stage);

}  // namespace shadertoy
