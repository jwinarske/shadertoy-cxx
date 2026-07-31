// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// spirv_compile_glslang.cpp — GLSL→SPIR-V through linked glslang.
//
// Compiled only when the build found glslang and linking was not disabled (see
// SHADERTOY_GLSLANG_LINK). The subprocess path in spirv_compile.cpp stays
// available either way: a deployment may prefer a distro-managed compiler it
// can update without rebuilding this library, and some images ship the binary
// but not the development files.
//
// The value of linking is that it turns a runtime discovery into a build-time
// one. Shelling out means the compiler's absence surfaces on the target, at the
// first shader, as a black view — a build that links has already answered the
// question.

#include "spirv_compile_backend.hpp"

#include <cstdio>
#include <mutex>

#include <glslang/Include/glslang_c_interface.h>
#include <glslang/Public/resource_limits_c.h>

namespace shadertoy {
namespace {

// glslang_initialize_process() is per-process and must precede any shader call.
// Guarded rather than ref-counted: finalize is not worth racing on teardown,
// and the process is exiting anyway.
void EnsureGlslangInitialized() {
  static std::once_flag once;
  std::call_once(once, [] { glslang_initialize_process(); });
}

}  // namespace

bool HaveLinkedSpirvCompiler() {
  return true;
}

std::vector<uint32_t> CompileToSpirvLinked(const std::string& glsl_source,
                                           const ShaderStage stage,
                                           std::string* log) {
  // Report to stderr and to the caller's buffer both: stderr is where a
  // developer looks, the buffer is what a host can put in front of a user.
  const auto record = [log](const char* what, const char* info,
                            const char* debug) {
    std::fprintf(stderr, "shadertoy: %s:\n%s%s\n", what,
                 info != nullptr ? info : "", debug != nullptr ? debug : "");
    if (log != nullptr) {
      log->append(what);
      log->append(":\n");
      if (info != nullptr) {
        log->append(info);
      }
      if (debug != nullptr) {
        log->append(debug);
      }
    }
  };
  EnsureGlslangInitialized();

  glslang_input_t input{};
  input.language = GLSLANG_SOURCE_GLSL;
  input.stage = stage == ShaderStage::kVertex ? GLSLANG_STAGE_VERTEX
                                              : GLSLANG_STAGE_FRAGMENT;
  input.client = GLSLANG_CLIENT_VULKAN;
  input.client_version = GLSLANG_TARGET_VULKAN_1_1;
  input.target_language = GLSLANG_TARGET_SPV;
  input.target_language_version = GLSLANG_TARGET_SPV_1_3;
  input.code = glsl_source.c_str();
  input.default_version = 450;
  input.default_profile = GLSLANG_NO_PROFILE;
  input.force_default_version_and_profile = 0;
  input.forward_compatible = 0;
  input.messages = GLSLANG_MSG_DEFAULT_BIT;
  input.resource = glslang_default_resource();

  glslang_shader_t* shader = glslang_shader_create(&input);
  if (shader == nullptr) {
    record("glslang_shader_create failed", nullptr, nullptr);
    return {};
  }

  if (glslang_shader_preprocess(shader, &input) == 0) {
    record("GLSL preprocess failed", glslang_shader_get_info_log(shader),
           glslang_shader_get_info_debug_log(shader));
    glslang_shader_delete(shader);
    return {};
  }
  if (glslang_shader_parse(shader, &input) == 0) {
    record("GLSL parse failed", glslang_shader_get_info_log(shader),
           glslang_shader_get_info_debug_log(shader));
    glslang_shader_delete(shader);
    return {};
  }

  glslang_program_t* program = glslang_program_create();
  if (program == nullptr) {
    record("glslang_program_create failed", nullptr, nullptr);
    glslang_shader_delete(shader);
    return {};
  }
  glslang_program_add_shader(program, shader);

  if (glslang_program_link(program, GLSLANG_MSG_SPV_RULES_BIT |
                                        GLSLANG_MSG_VULKAN_RULES_BIT) == 0) {
    record("GLSL link failed", glslang_program_get_info_log(program),
           glslang_program_get_info_debug_log(program));
    glslang_program_delete(program);
    glslang_shader_delete(shader);
    return {};
  }

  glslang_program_SPIRV_generate(program, input.stage);

  std::vector<uint32_t> spirv(glslang_program_SPIRV_get_size(program));
  if (!spirv.empty()) {
    glslang_program_SPIRV_get(program, spirv.data());
  }
  // Non-fatal: the SPIR-V generator emits warnings here even on success.
  if (const char* msg = glslang_program_SPIRV_get_messages(program);
      msg != nullptr && *msg != '\0') {
    std::fprintf(stderr, "shadertoy: SPIR-V generator: %s\n", msg);
  }

  glslang_program_delete(program);
  glslang_shader_delete(shader);

  if (spirv.empty()) {
    record("glslang produced no SPIR-V", nullptr, nullptr);
  }
  return spirv;
}

}  // namespace shadertoy
