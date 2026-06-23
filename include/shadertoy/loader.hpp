// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// loader.hpp — load a Shadertoy shader from its API/export JSON.
//
// Accepts the JSON returned by the Shadertoy API (`{"Shader": {...}}`) or a
// bare shader object (`{"renderpass": [...], "info": {...}}`), and produces a
// ShaderProgram: the Common tab, Buffer A..D passes, the Image pass, and each
// pass's channel bindings (buffers resolved by output id; textures keep their
// `src` path for the host to resolve; audio/keyboard noted as stubs).
//
// Only compiled when the JSON back-end is enabled (SHADERTOY_HAVE_JSON).

#pragma once

#include <string>

#include "shadertoy/program.hpp"

namespace shadertoy {

/// Parse Shadertoy export JSON text into @p out.  Returns false on error and
/// fills @p error (if non-null) with a human-readable message.
[[nodiscard]] bool LoadShadertoyJson(const std::string& json_text,
                                     ShaderProgram& out,
                                     std::string* error = nullptr);

/// Read and parse a Shadertoy export JSON file.  Returns false on I/O or parse
/// error.
[[nodiscard]] bool LoadShadertoyJsonFile(const std::string& path,
                                         ShaderProgram& out,
                                         std::string* error = nullptr);

}  // namespace shadertoy
