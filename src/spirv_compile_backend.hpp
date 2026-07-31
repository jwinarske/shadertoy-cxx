// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// Internal seam between CompileToSpirv and the compiler backends.
//
// Two exist: a linked glslang (spirv_compile_glslang.cpp, compiled only when
// the build found glslang) and the subprocess driver in spirv_compile.cpp,
// which is always compiled. Both are kept because they answer different needs
// -- linking removes a runtime dependency from the target image, while shelling
// out keeps the compiler swappable without rebuilding this library, and works
// on an image that ships the tool but not its development files.

#ifndef SHADERTOY_SPIRV_COMPILE_BACKEND_HPP
#define SHADERTOY_SPIRV_COMPILE_BACKEND_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "shadertoy/spirv_compile.hpp"

namespace shadertoy {

/// True when this build links a compiler. Exactly one translation unit defines
/// this pair: spirv_compile_glslang.cpp when glslang was found, and
/// spirv_compile.cpp's stubs otherwise, so the call site needs no preprocessor
/// branch and the linker catches a build that wires up neither.
[[nodiscard]] bool HaveLinkedSpirvCompiler();

/// Compile through the linked compiler. Only called when
/// HaveLinkedSpirvCompiler() is true; returns an empty vector on failure,
/// having reported the log to stderr.
[[nodiscard]] std::vector<uint32_t> CompileToSpirvLinked(
    const std::string& glsl_source,
    ShaderStage stage,
    std::string* log);

}  // namespace shadertoy

#endif  // SHADERTOY_SPIRV_COMPILE_BACKEND_HPP
