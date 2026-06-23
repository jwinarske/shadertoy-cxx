// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// vk_renderer.hpp — Vulkan Shadertoy renderer.
//
// Renders a Shadertoy Image shader (compiled to SPIR-V at runtime) into a
// swapchain.  The only platform-specific dependency is creating the
// VkSurfaceKHR: the host supplies the required instance extensions and a
// callback that turns the renderer's VkInstance into a surface for its window
// system (VK_KHR_wayland_surface, VK_KHR_display, …).  Everything else — device
// selection, swapchain, pipeline, per-frame draw and resize — is self-contained
// and identical across host projects.

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "shadertoy/inputs.hpp"

namespace shadertoy {

struct VkRendererConfig {
  // Instance extensions the platform needs for its surface (e.g.
  // VK_KHR_SURFACE_EXTENSION_NAME + VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME).
  std::vector<const char*> instance_extensions;

  // Creates a VkSurfaceKHR for the host's window from the renderer's instance.
  // Return VK_NULL_HANDLE on failure.
  std::function<VkSurfaceKHR(VkInstance)> create_surface;

  std::string image_shader;  // bare Shadertoy Image shader source
  uint32_t width = 800;
  uint32_t height = 600;
  bool vsync = true;          // FIFO present mode when true, MAILBOX otherwise
  bool enable_validation = false;
};

class VkRenderer {
 public:
  // Builds the full Vulkan pipeline.  Returns nullptr on any failure (causes
  // are logged to stderr).
  [[nodiscard]] static std::unique_ptr<VkRenderer> Create(
      const VkRendererConfig& config);

  ~VkRenderer();

  VkRenderer(const VkRenderer&) = delete;
  VkRenderer& operator=(const VkRenderer&) = delete;

  // Draw and present one frame.  Returns false only on an unrecoverable device
  // error; a swapchain that is merely out-of-date is rebuilt transparently.
  [[nodiscard]] bool Render(const ShaderInputs& inputs);

  // Record a new target size; the swapchain is rebuilt on the next frame.
  void Resize(uint32_t width, uint32_t height) noexcept;

 private:
  VkRenderer() = default;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace shadertoy
