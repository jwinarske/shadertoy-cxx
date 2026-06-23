// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors

#include "shadertoy/program.hpp"

#include <utility>

namespace shadertoy {

ShaderProgram MakeSinglePass(std::string image_code, std::string name) {
  ShaderProgram prog;
  prog.name = std::move(name);
  prog.image.code = std::move(image_code);
  return prog;
}

}  // namespace shadertoy
