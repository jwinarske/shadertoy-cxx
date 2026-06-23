// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// playlist.hpp — an ordered set of shader programs to cycle through.
//
// Hosts build a Playlist from CLI arguments (shader files and/or directories)
// or fall back to the installed bundled set, then advance through it on a timer
// or user input, calling renderer.SetProgram(playlist.current()) on each change.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "shadertoy/program.hpp"

namespace shadertoy {

/// Compile-time install location of the bundled shader set
/// (${datadir}/shadertoy-cxx/shaders).  Overridable at runtime via the
/// SHADERTOY_SHADER_DIR environment variable (see DefaultShaderDir()).
[[nodiscard]] std::string DefaultShaderDir();

class Playlist {
 public:
  Playlist() = default;

  /// Append a program directly.
  void Add(ShaderProgram program);

  /// Load one shader file: ".json" is a full Shadertoy export (multi-pass);
  /// anything else is treated as bare Image GLSL (single pass).  Returns false
  /// on error, filling @p error if non-null.
  bool AddFile(const std::string& path, std::string* error = nullptr);

  /// Load every *.json / *.frag / *.glsl in @p dir (sorted by name).  Returns
  /// the number of programs added.
  size_t AddDirectory(const std::string& dir);

  [[nodiscard]] bool empty() const noexcept { return programs_.empty(); }
  [[nodiscard]] size_t size() const noexcept { return programs_.size(); }
  [[nodiscard]] size_t index() const noexcept { return index_; }

  [[nodiscard]] const ShaderProgram& current() const { return programs_[index_]; }

  /// Advance to the next / previous program (wraps).  No-op when empty.
  void next() noexcept;
  void prev() noexcept;
  void set_index(size_t i) noexcept;

 private:
  std::vector<ShaderProgram> programs_;
  size_t index_ = 0;
};

}  // namespace shadertoy
