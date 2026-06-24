// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// loader.cpp — Shadertoy export JSON → ShaderProgram (via nlohmann/json).

#include "shadertoy/loader.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace shadertoy {

namespace {

using nlohmann::json;

// Normalise an "id" field (Shadertoy uses ints in old exports, strings in new
// ones) to a comparable string key.
std::string IdKey(const json& v) {
  if (v.is_string())
    return v.get<std::string>();
  if (v.is_number_integer())
    return std::to_string(v.get<long long>());
  if (v.is_number_unsigned())
    return std::to_string(v.get<unsigned long long>());
  if (v.is_number())
    return std::to_string(v.get<long long>());
  return {};
}

// "Buf A" / "Buffer C" / "Image" → buffer index 0..3, or -1.
int BufferIndexFromName(const std::string& name) {
  for (auto it = name.rbegin(); it != name.rend(); ++it) {
    const char c = *it;
    if (c == ' ' || c == '\t')
      continue;
    switch (c) {
      case 'A': case 'a': return kBufferA;
      case 'B': case 'b': return kBufferB;
      case 'C': case 'c': return kBufferC;
      case 'D': case 'd': return kBufferD;
      default: return -1;
    }
  }
  return -1;
}

Filter ParseFilter(const std::string& s) {
  if (s == "nearest")
    return Filter::kNearest;
  if (s == "mipmap")
    return Filter::kMipmap;
  return Filter::kLinear;
}

Wrap ParseWrap(const std::string& s) {
  return s == "clamp" ? Wrap::kClamp : Wrap::kRepeat;
}

bool AsBool(const json& v, bool dflt) {
  if (v.is_boolean())
    return v.get<bool>();
  if (v.is_string()) {
    const auto s = v.get<std::string>();
    return s == "true" || s == "1";
  }
  return dflt;
}

void ParseSampler(const json& input, Channel& ch) {
  if (!input.contains("sampler") || !input["sampler"].is_object())
    return;
  const json& s = input["sampler"];
  if (s.contains("filter") && s["filter"].is_string())
    ch.filter = ParseFilter(s["filter"].get<std::string>());
  if (s.contains("wrap") && s["wrap"].is_string())
    ch.wrap = ParseWrap(s["wrap"].get<std::string>());
  if (s.contains("vflip"))
    ch.vflip = AsBool(s["vflip"], true);
}

}  // namespace

bool LoadShadertoyJson(const std::string& json_text,
                       ShaderProgram& out,
                       std::string* error) {
  json root = json::parse(json_text, nullptr, /*allow_exceptions=*/false);
  if (root.is_discarded()) {
    if (error)
      *error = "invalid JSON";
    return false;
  }

  // Accept {"Shader": {...}} or a bare shader object.
  const json* shader = &root;
  if (root.contains("Shader"))
    shader = &root["Shader"];
  if (!shader->contains("renderpass") || !(*shader)["renderpass"].is_array()) {
    if (error)
      *error = "no renderpass array";
    return false;
  }

  out = ShaderProgram{};
  if (shader->contains("info") && (*shader)["info"].contains("name") &&
      (*shader)["info"]["name"].is_string())
    out.name = (*shader)["info"]["name"].get<std::string>();

  const json& passes = (*shader)["renderpass"];

  // Pass 1: map each buffer pass's output id → buffer index.
  std::map<std::string, int> outputs_to_buffer;
  for (const json& p : passes) {
    const std::string type = p.value("type", "");
    if (type != "buffer")
      continue;
    int idx = BufferIndexFromName(p.value("name", ""));
    if (idx < 0)
      continue;
    if (p.contains("outputs") && p["outputs"].is_array() &&
        !p["outputs"].empty() && p["outputs"][0].contains("id"))
      outputs_to_buffer[IdKey(p["outputs"][0]["id"])] = idx;
  }

  auto parse_inputs = [&](const json& p, Pass& dst) {
    if (!p.contains("inputs") || !p["inputs"].is_array())
      return;
    for (const json& in : p["inputs"]) {
      const int channel = in.value("channel", -1);
      if (channel < 0 || channel > 3)
        continue;
      Channel ch;
      const std::string ctype = in.value("ctype", "");
      if (ctype == "buffer") {
        ch.kind = ChannelKind::kBuffer;
        auto it = outputs_to_buffer.find(in.contains("id") ? IdKey(in["id"])
                                                           : std::string());
        ch.buffer = (it != outputs_to_buffer.end()) ? it->second : -1;
        if (ch.buffer < 0)
          continue;  // dangling buffer reference
      } else if (ctype == "texture") {
        ch.kind = ChannelKind::kTexture;
        ch.texture_path = in.value("src", "");
      } else if (ctype == "keyboard") {
        ch.kind = ChannelKind::kKeyboard;
      } else if (ctype == "musicstream" || ctype == "music" || ctype == "mic" ||
                 ctype == "soundcloud") {
        ch.kind = ChannelKind::kAudio;
      } else if (ctype == "cubemap") {
        ch.kind = ChannelKind::kCubemap;
        ch.texture_path = in.value("src", "");  // src kept to resolve media files
      } else if (ctype == "volume") {
        ch.kind = ChannelKind::kVolume;
        ch.texture_path = in.value("src", "");  // src kept to resolve media files
      } else {
        continue;  // video/webcam — unsupported, leave channel unbound
      }
      ParseSampler(in, ch);
      dst.channels[static_cast<size_t>(channel)] = std::move(ch);
    }
  };

  bool have_image = false;
  for (const json& p : passes) {
    const std::string type = p.value("type", "");
    const std::string code = p.value("code", "");
    if (type == "common") {
      out.common = code;
    } else if (type == "image") {
      out.image.code = code;
      parse_inputs(p, out.image);
      have_image = true;
    } else if (type == "buffer") {
      const int idx = BufferIndexFromName(p.value("name", ""));
      if (idx < 0)
        continue;
      out.buffers[static_cast<size_t>(idx)].code = code;
      parse_inputs(p, out.buffers[static_cast<size_t>(idx)]);
      out.buffer_used[static_cast<size_t>(idx)] = true;
    }
    // "sound"/"cubemap" passes are ignored.
  }

  if (!have_image) {
    if (error)
      *error = "no image pass";
    return false;
  }
  return true;
}

bool LoadShadertoyJsonFile(const std::string& path,
                           ShaderProgram& out,
                           std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error)
      *error = "cannot open " + path;
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return LoadShadertoyJson(ss.str(), out, error);
}

}  // namespace shadertoy
