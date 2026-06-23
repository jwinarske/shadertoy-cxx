// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors

#include "shadertoy/playlist.hpp"

#include "shadertoy/config.hpp"
#include "shadertoy/inputs.hpp"
#include "shadertoy/loader.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <utility>

namespace shadertoy {

namespace fs = std::filesystem;

std::string DefaultShaderDir() {
  const char* env = std::getenv("SHADERTOY_SHADER_DIR");
  if (env != nullptr && *env != '\0')
    return env;
  return SHADERTOY_SHADER_DIR;
}

void Playlist::Add(ShaderProgram program) {
  programs_.push_back(std::move(program));
}

bool Playlist::AddFile(const std::string& path, std::string* error) {
  const std::string ext = fs::path(path).extension().string();
  if (ext == ".json") {
    ShaderProgram prog;
    if (!LoadShadertoyJsonFile(path, prog, error))
      return false;
    if (prog.name.empty())
      prog.name = fs::path(path).stem().string();
    programs_.push_back(std::move(prog));
    return true;
  }
  // Bare Image GLSL.
  const std::string src = LoadShaderFile(path);
  if (src.empty()) {
    if (error)
      *error = "cannot read " + path;
    return false;
  }
  programs_.push_back(MakeSinglePass(src, fs::path(path).stem().string()));
  return true;
}

size_t Playlist::AddDirectory(const std::string& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec))
    return 0;
  std::vector<std::string> files;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file())
      continue;
    const std::string ext = entry.path().extension().string();
    if (ext == ".json" || ext == ".frag" || ext == ".glsl")
      files.push_back(entry.path().string());
  }
  std::sort(files.begin(), files.end());
  size_t added = 0;
  for (const std::string& f : files) {
    if (AddFile(f, nullptr))
      ++added;
  }
  return added;
}

void Playlist::next() noexcept {
  if (!programs_.empty())
    index_ = (index_ + 1) % programs_.size();
}

void Playlist::prev() noexcept {
  if (!programs_.empty())
    index_ = (index_ + programs_.size() - 1) % programs_.size();
}

void Playlist::set_index(size_t i) noexcept {
  if (!programs_.empty())
    index_ = i % programs_.size();
}

}  // namespace shadertoy
