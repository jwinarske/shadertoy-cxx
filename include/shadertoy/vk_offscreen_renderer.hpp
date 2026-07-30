// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// vk_offscreen_renderer.hpp — Vulkan Shadertoy renderer targeting a
// caller-owned image, on a caller-owned device.
//
// The swapchain renderer (vk_renderer.hpp) owns its VkInstance and VkDevice and
// presents to a surface, which suits a standalone application.  A host that
// already has a device and wants the frame *as an image* — a compositor
// plugin exporting a dma-buf, an offscreen recorder, a texture source for a
// larger scene — cannot use it: there is no second device to spare and no
// surface to present to.
//
// This renderer inverts both of those.  It ADOPTS the host's Vulkan objects
// (never creating or destroying them), renders into an image the host supplies,
// and RECORDS into a command buffer the host supplies rather than submitting
// anything itself.  The host keeps control of queue submission, which is what
// lets it batch the render with its own work — a copy into an exportable
// image, a queue-family ownership transfer — in one submit, and apply whatever
// synchronization its compositor requires.
//
// Every Vulkan entry point is resolved through the caller's
// vkGetInstanceProcAddr.  That matters when the host interposes the loader: an
// embedder that serializes queue access by wrapping vkQueueSubmit only stays
// correct if everything sharing the queue resolves through the same loader.

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <string>

#include "shadertoy/inputs.hpp"
#include "shadertoy/program.hpp"

namespace shadertoy {

/// Vulkan objects owned by the host and borrowed by the renderer.
///
/// None of these are destroyed by the renderer, and all must outlive it. The
/// device must have been created with a graphics-capable queue family.
struct VkExternalDevice {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;

  /// A graphics queue on @device. Used only during Create (a one-time upload of
  /// the stub channel texture) — never at frame time, where the caller submits.
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queue_family_index = 0;

  /// Every entry point is resolved through this. Pass the host's loader, not
  /// the one you would get by linking libvulkan: a host that interposes
  /// vkQueueSubmit to serialize a shared queue needs this renderer's setup
  /// submit to go through the same interposition.
  PFN_vkGetInstanceProcAddr get_instance_proc_addr = nullptr;
};

/// What the renderer builds its pipeline against. Fixed for the renderer's
/// lifetime, because the render pass is built from it.
struct VkOffscreenConfig {
  /// Format of the color target. Must match every image later passed to
  /// RecordRender.
  VkFormat color_format = VK_FORMAT_B8G8R8A8_UNORM;

  /// Layout the color target is left in once rendering completes. A host that
  /// copies out of the image wants TRANSFER_SRC_OPTIMAL; one that samples it
  /// wants SHADER_READ_ONLY_OPTIMAL; one that exports it directly to a
  /// compositor may want PRESENT_SRC or GENERAL.
  VkImageLayout final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

  /// Layout the color target is in when RecordRender is called. UNDEFINED is
  /// correct for a fresh image, or for any image whose previous contents are
  /// being discarded — which is the usual case for a ring slot.
  VkImageLayout initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

/// One frame's destination. The image and view are the caller's; the renderer
/// holds no reference to them beyond the recorded command buffer.
struct VkOffscreenTarget {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;
};

/// Renders a Shadertoy program into a caller-owned image.
///
/// Not thread-safe: one renderer belongs to one thread. Several renderers may
/// share a device, which is the point — a host with N views builds N of these.
class VkOffscreenRenderer {
 public:
  /// Adopt @device and build everything that does not depend on the shader.
  /// Returns nullptr on failure, with the reason on stderr.
  [[nodiscard]] static std::unique_ptr<VkOffscreenRenderer> Create(
      const VkExternalDevice& device,
      const VkOffscreenConfig& config);

  ~VkOffscreenRenderer();

  VkOffscreenRenderer(const VkOffscreenRenderer&) = delete;
  VkOffscreenRenderer& operator=(const VkOffscreenRenderer&) = delete;

  /// Build (or replace) the active program. Compiles GLSL to SPIR-V and builds
  /// the pipeline. Returns false on compile or link failure, leaving the
  /// previous program intact — matching GlRenderer::SetProgram, so a host can
  /// hot-swap shaders without risking a blank view.
  ///
  /// Multi-pass programs (Buffer A..D) are supported: each buffer pass renders
  /// into its own ping-ponged offscreen pair, and a kBuffer channel samples the
  /// previous frame's half, as Shadertoy does. Buffer storage is chosen by
  /// asking the device (see examples/vk_format_probe); half-float is preferred
  /// and a fallback to a clamped format is reported, because it changes what an
  /// accumulating shader computes.
  ///
  /// Channels this renderer cannot supply -- textures, cubemaps, audio,
  /// keyboard -- still sample the 1x1 black stub, so every iChannel is declared
  /// sampler2D and a shader sampling a cubemap channel will not compile.
  [[nodiscard]] bool SetProgram(const ShaderProgram& program);

  /// Single-pass convenience, mirroring GlRenderer::Init.
  [[nodiscard]] bool Init(const std::string& image_shader);

  /// Record the draw into @cmd. The caller has already begun @cmd and submits
  /// it; nothing here touches a queue.
  ///
  /// On return the recorded commands will leave @target.image in the config's
  /// final_layout. inputs.res_x/res_y should match the target size — they are
  /// what the shader sees as iResolution, and are not derived from it.
  ///
  /// Returns false if no program is set or the target is malformed.
  [[nodiscard]] bool RecordRender(VkCommandBuffer cmd,
                                  const ShaderInputs& inputs,
                                  const VkOffscreenTarget& target);

  /// Drop any framebuffer cached for @view. Call when retiring an image, so the
  /// renderer does not outlive the view it was built from.
  void ForgetTarget(VkImageView view);

  [[nodiscard]] bool has_program() const noexcept;

 private:
  VkOffscreenRenderer();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace shadertoy
