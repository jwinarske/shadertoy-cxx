// SPDX-License-Identifier: MIT
// Copyright (c) 2026 wayland-cxx-scanner contributors
//
// vk_renderer — Vulkan Shadertoy renderer implementation (platform-agnostic).
//
// Uses the Vulkan C API directly so the file carries no binding-version
// baggage and reads the same in any host project.  The only platform seam is
// VkRendererConfig::create_surface (and the instance extension list); the rest
// — physical-device selection, swapchain, render pass, pipeline, descriptor
// set, per-frame command recording, present and swapchain recreation — is
// fully self-contained.

#include "shadertoy/vk_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "shadertoy/spirv_compile.hpp"

namespace shadertoy {

namespace {

constexpr uint32_t kMaxFramesInFlight = 2;

// Log a Vulkan failure and bail out of the calling bool-returning function.
#define ST_VK_CHECK(expr, msg)                                       \
  do {                                                               \
    const VkResult _r = (expr);                                      \
    if (_r != VK_SUCCESS) {                                          \
      std::fprintf(stderr, "shadertoy: %s (VkResult %d)\n", (msg),   \
                   static_cast<int>(_r));                            \
      return false;                                                  \
    }                                                                \
  } while (0)

// Vertex shader: a single full-screen triangle generated from gl_VertexIndex.
constexpr const char* kVulkanVertexShader = R"GLSL(#version 450
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

}  // namespace

// ══════════════════════════════════════════════════════════════════════════════
// VkRenderer::Impl
// ══════════════════════════════════════════════════════════════════════════════

struct VkRenderer::Impl {
  // ── Configuration captured at creation ────────────────────────────────────
  bool vsync = true;
  uint32_t desired_width = 800;
  uint32_t desired_height = 600;

  // ── Core objects ──────────────────────────────────────────────────────────
  VkInstance instance = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;

  // ── Swapchain ─────────────────────────────────────────────────────────────
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat swap_format = VK_FORMAT_UNDEFINED;
  VkExtent2D extent = {0, 0};
  std::vector<VkImage> images;
  std::vector<VkImageView> views;
  std::vector<VkFramebuffer> framebuffers;

  // ── Pipeline ──────────────────────────────────────────────────────────────
  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkShaderModule vert_module = VK_NULL_HANDLE;
  VkShaderModule frag_module = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;

  // ── Dummy channel texture (1x1 black) bound to iChannel0..3 ───────────────
  VkImage dummy_image = VK_NULL_HANDLE;
  VkDeviceMemory dummy_memory = VK_NULL_HANDLE;
  VkImageView dummy_view = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

  // ── Commands & synchronization ────────────────────────────────────────────
  VkCommandPool command_pool = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kMaxFramesInFlight> command_buffers = {};
  std::array<VkSemaphore, kMaxFramesInFlight> image_available = {};
  std::array<VkSemaphore, kMaxFramesInFlight> render_finished = {};
  std::array<VkFence, kMaxFramesInFlight> in_flight = {};
  uint32_t current_frame = 0;
  bool needs_recreate = false;

  ~Impl() { Cleanup(); }

  // ── Setup steps ───────────────────────────────────────────────────────────
  bool CreateInstance(const VkRendererConfig& cfg);
  bool PickPhysicalDevice();
  bool CreateDevice();
  bool CreateRenderPass();
  bool CreateDescriptorResources();
  bool CreateShaderModules(const std::string& image_shader);
  bool CreatePipeline();
  bool CreateCommandResources();
  bool CreateSyncObjects();
  bool CreateSwapchain();

  // ── Frame & lifecycle ─────────────────────────────────────────────────────
  bool RecreateSwapchain();
  void CleanupSwapchain();
  void Cleanup();
  bool RecordCommandBuffer(VkCommandBuffer cmd,
                           uint32_t image_index,
                           const ShaderInputs& inputs);
  bool DrawFrame(const ShaderInputs& inputs);

  // ── Helpers ───────────────────────────────────────────────────────────────
  [[nodiscard]] bool FindMemoryType(uint32_t type_bits,
                                    VkMemoryPropertyFlags props,
                                    uint32_t* out_index) const;
  [[nodiscard]] VkShaderModule MakeModule(const std::vector<uint32_t>& spirv);
};

// ── Helpers ───────────────────────────────────────────────────────────────────

bool VkRenderer::Impl::FindMemoryType(uint32_t type_bits,
                                      VkMemoryPropertyFlags props,
                                      uint32_t* out_index) const {
  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mem);
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    const bool type_ok = (type_bits & (1u << i)) != 0u;
    const bool props_ok =
        (mem.memoryTypes[i].propertyFlags & props) == props;
    if (type_ok && props_ok) {
      *out_index = i;
      return true;
    }
  }
  return false;
}

VkShaderModule VkRenderer::Impl::MakeModule(const std::vector<uint32_t>& spirv) {
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = spirv.size() * sizeof(uint32_t);
  info.pCode = spirv.data();
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
    return VK_NULL_HANDLE;
  return module;
}

// ── Instance ──────────────────────────────────────────────────────────────────

bool VkRenderer::Impl::CreateInstance(const VkRendererConfig& cfg) {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "shadertoy";
  app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  app.pEngineName = "shadertoy";
  app.apiVersion = VK_API_VERSION_1_1;

  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;
  info.enabledExtensionCount =
      static_cast<uint32_t>(cfg.instance_extensions.size());
  info.ppEnabledExtensionNames = cfg.instance_extensions.data();

  const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";
  if (cfg.enable_validation) {
    info.enabledLayerCount = 1;
    info.ppEnabledLayerNames = &kValidationLayer;
  }

  ST_VK_CHECK(vkCreateInstance(&info, nullptr, &instance),
              "vkCreateInstance failed");
  return true;
}

// ── Physical device ───────────────────────────────────────────────────────────

bool VkRenderer::Impl::PickPhysicalDevice() {
  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (count == 0) {
    std::fprintf(stderr, "shadertoy: no Vulkan physical devices\n");
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());

  for (VkPhysicalDevice candidate : devices) {
    uint32_t qcount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qcount);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qcount, qprops.data());

    for (uint32_t i = 0; i < qcount; ++i) {
      const bool graphics =
          (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u;
      VkBool32 present = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present);
      if (graphics && present == VK_TRUE) {
        phys = candidate;
        queue_family = i;
        return true;
      }
    }
  }
  std::fprintf(stderr,
               "shadertoy: no graphics+present queue family found\n");
  return false;
}

// ── Logical device ────────────────────────────────────────────────────────────

bool VkRenderer::Impl::CreateDevice() {
  const float priority = 1.0f;
  VkDeviceQueueCreateInfo qinfo{};
  qinfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qinfo.queueFamilyIndex = queue_family;
  qinfo.queueCount = 1;
  qinfo.pQueuePriorities = &priority;

  const char* kSwapchainExt = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  VkDeviceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  info.queueCreateInfoCount = 1;
  info.pQueueCreateInfos = &qinfo;
  info.enabledExtensionCount = 1;
  info.ppEnabledExtensionNames = &kSwapchainExt;

  ST_VK_CHECK(vkCreateDevice(phys, &info, nullptr, &device),
              "vkCreateDevice failed");
  vkGetDeviceQueue(device, queue_family, 0, &queue);
  return true;
}

// ── Swapchain ─────────────────────────────────────────────────────────────────

bool VkRenderer::Impl::CreateSwapchain() {
  VkSurfaceCapabilitiesKHR caps{};
  ST_VK_CHECK(
      vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps),
      "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed");

  // Surface format: prefer 8-bit BGRA, else take whatever is first.
  uint32_t fmt_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmt_count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(fmt_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmt_count,
                                       formats.data());
  VkSurfaceFormatKHR chosen = formats.empty()
                                  ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                       VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                  : formats.front();
  for (const auto& f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      chosen = f;
      break;
    }
  }
  swap_format = chosen.format;

  // Present mode: FIFO (vsync, always supported) or MAILBOX/IMMEDIATE.
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
  if (!vsync) {
    uint32_t pm_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pm_count,
                                              nullptr);
    std::vector<VkPresentModeKHR> modes(pm_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &pm_count,
                                              modes.data());
    for (VkPresentModeKHR m : modes) {
      if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
        present_mode = m;
        break;
      }
      if (m == VK_PRESENT_MODE_IMMEDIATE_KHR)
        present_mode = m;
    }
  }

  // Extent: honour the surface's fixed size, or clamp our desired size.
  if (caps.currentExtent.width != 0xFFFFFFFFu) {
    extent = caps.currentExtent;
  } else {
    extent.width = desired_width;
    extent.height = desired_height;
    extent.width = std::max(caps.minImageExtent.width,
                            std::min(caps.maxImageExtent.width, extent.width));
    extent.height =
        std::max(caps.minImageExtent.height,
                 std::min(caps.maxImageExtent.height, extent.height));
  }
  if (extent.width == 0 || extent.height == 0)
    return false;  // window minimized — caller retries later

  uint32_t image_count = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
    image_count = caps.maxImageCount;

  VkSwapchainCreateInfoKHR info{};
  info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  info.surface = surface;
  info.minImageCount = image_count;
  info.imageFormat = chosen.format;
  info.imageColorSpace = chosen.colorSpace;
  info.imageExtent = extent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = present_mode;
  info.clipped = VK_TRUE;
  info.oldSwapchain = VK_NULL_HANDLE;

  ST_VK_CHECK(vkCreateSwapchainKHR(device, &info, nullptr, &swapchain),
              "vkCreateSwapchainKHR failed");

  vkGetSwapchainImagesKHR(device, swapchain, &image_count, nullptr);
  images.resize(image_count);
  vkGetSwapchainImagesKHR(device, swapchain, &image_count, images.data());

  views.resize(image_count);
  for (uint32_t i = 0; i < image_count; ++i) {
    VkImageViewCreateInfo vinfo{};
    vinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vinfo.image = images[i];
    vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vinfo.format = swap_format;
    vinfo.components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY,
                        VK_COMPONENT_SWIZZLE_IDENTITY};
    vinfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vinfo.subresourceRange.levelCount = 1;
    vinfo.subresourceRange.layerCount = 1;
    ST_VK_CHECK(vkCreateImageView(device, &vinfo, nullptr, &views[i]),
                "vkCreateImageView failed");
  }

  framebuffers.resize(image_count);
  for (uint32_t i = 0; i < image_count; ++i) {
    VkFramebufferCreateInfo finfo{};
    finfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    finfo.renderPass = render_pass;
    finfo.attachmentCount = 1;
    finfo.pAttachments = &views[i];
    finfo.width = extent.width;
    finfo.height = extent.height;
    finfo.layers = 1;
    ST_VK_CHECK(vkCreateFramebuffer(device, &finfo, nullptr, &framebuffers[i]),
                "vkCreateFramebuffer failed");
  }
  return true;
}

// ── Render pass ───────────────────────────────────────────────────────────────

bool VkRenderer::Impl::CreateRenderPass() {
  // swap_format must be known before the render pass is built; query it once
  // here using the same logic as CreateSwapchain's format selection.
  uint32_t fmt_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmt_count, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(fmt_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmt_count,
                                       formats.data());
  swap_format = formats.empty() ? VK_FORMAT_B8G8R8A8_UNORM
                                : formats.front().format;
  for (const auto& f : formats) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      swap_format = f.format;
      break;
    }
  }

  VkAttachmentDescription color{};
  color.format = swap_format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentReference color_ref{};
  color_ref.attachment = 0;
  color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;

  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.srcAccessMask = 0;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = 1;
  info.pAttachments = &color;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  info.dependencyCount = 1;
  info.pDependencies = &dep;

  ST_VK_CHECK(vkCreateRenderPass(device, &info, nullptr, &render_pass),
              "vkCreateRenderPass failed");
  return true;
}

// ── Descriptor resources (dummy black channel texture) ────────────────────────

bool VkRenderer::Impl::CreateDescriptorResources() {
  // 1x1 black image.
  VkImageCreateInfo iinfo{};
  iinfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  iinfo.imageType = VK_IMAGE_TYPE_2D;
  iinfo.format = VK_FORMAT_R8G8B8A8_UNORM;
  iinfo.extent = {1, 1, 1};
  iinfo.mipLevels = 1;
  iinfo.arrayLayers = 1;
  iinfo.samples = VK_SAMPLE_COUNT_1_BIT;
  iinfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  iinfo.usage =
      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  iinfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ST_VK_CHECK(vkCreateImage(device, &iinfo, nullptr, &dummy_image),
              "vkCreateImage(dummy) failed");

  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device, dummy_image, &req);
  uint32_t mem_type = 0;
  if (!FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      &mem_type)) {
    std::fprintf(stderr, "shadertoy: no device-local memory type\n");
    return false;
  }
  VkMemoryAllocateInfo ainfo{};
  ainfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ainfo.allocationSize = req.size;
  ainfo.memoryTypeIndex = mem_type;
  ST_VK_CHECK(vkAllocateMemory(device, &ainfo, nullptr, &dummy_memory),
              "vkAllocateMemory(dummy) failed");
  ST_VK_CHECK(vkBindImageMemory(device, dummy_image, dummy_memory, 0),
              "vkBindImageMemory(dummy) failed");

  // One-time command buffer: clear the image to black and move it to
  // SHADER_READ_ONLY_OPTIMAL.
  VkCommandPoolCreateInfo pinfo{};
  pinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pinfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pinfo.queueFamilyIndex = queue_family;
  VkCommandPool tmp_pool = VK_NULL_HANDLE;
  ST_VK_CHECK(vkCreateCommandPool(device, &pinfo, nullptr, &tmp_pool),
              "vkCreateCommandPool(tmp) failed");

  VkCommandBufferAllocateInfo cbinfo{};
  cbinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbinfo.commandPool = tmp_pool;
  cbinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbinfo.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(device, &cbinfo, &cmd);

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &begin);

  VkImageSubresourceRange range{};
  range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  range.levelCount = 1;
  range.layerCount = 1;

  VkImageMemoryBarrier to_dst{};
  to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.image = dummy_image;
  to_dst.subresourceRange = range;
  to_dst.srcAccessMask = 0;
  to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &to_dst);

  VkClearColorValue black{};
  black.float32[0] = 0.0f;
  black.float32[1] = 0.0f;
  black.float32[2] = 0.0f;
  black.float32[3] = 1.0f;
  vkCmdClearColorImage(cmd, dummy_image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

  VkImageMemoryBarrier to_read = to_dst;
  to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &to_read);

  vkEndCommandBuffer(cmd);
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
  vkQueueWaitIdle(queue);
  vkDestroyCommandPool(device, tmp_pool, nullptr);

  // Image view + sampler.
  VkImageViewCreateInfo vinfo{};
  vinfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vinfo.image = dummy_image;
  vinfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vinfo.format = VK_FORMAT_R8G8B8A8_UNORM;
  vinfo.subresourceRange = range;
  ST_VK_CHECK(vkCreateImageView(device, &vinfo, nullptr, &dummy_view),
              "vkCreateImageView(dummy) failed");

  VkSamplerCreateInfo sinfo{};
  sinfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sinfo.magFilter = VK_FILTER_LINEAR;
  sinfo.minFilter = VK_FILTER_LINEAR;
  sinfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sinfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sinfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sinfo.maxLod = VK_LOD_CLAMP_NONE;
  ST_VK_CHECK(vkCreateSampler(device, &sinfo, nullptr, &sampler),
              "vkCreateSampler failed");

  // Descriptor set layout: 4 combined image samplers (iChannel0..3).
  std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
  for (uint32_t i = 0; i < bindings.size(); ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo linfo{};
  linfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  linfo.bindingCount = static_cast<uint32_t>(bindings.size());
  linfo.pBindings = bindings.data();
  ST_VK_CHECK(
      vkCreateDescriptorSetLayout(device, &linfo, nullptr, &set_layout),
      "vkCreateDescriptorSetLayout failed");

  // Pool + set, all four bindings pointing at the dummy texture.
  VkDescriptorPoolSize pool_size{};
  pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  pool_size.descriptorCount = 4;
  VkDescriptorPoolCreateInfo dpinfo{};
  dpinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpinfo.maxSets = 1;
  dpinfo.poolSizeCount = 1;
  dpinfo.pPoolSizes = &pool_size;
  ST_VK_CHECK(
      vkCreateDescriptorPool(device, &dpinfo, nullptr, &descriptor_pool),
      "vkCreateDescriptorPool failed");

  VkDescriptorSetAllocateInfo dsinfo{};
  dsinfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsinfo.descriptorPool = descriptor_pool;
  dsinfo.descriptorSetCount = 1;
  dsinfo.pSetLayouts = &set_layout;
  ST_VK_CHECK(vkAllocateDescriptorSets(device, &dsinfo, &descriptor_set),
              "vkAllocateDescriptorSets failed");

  VkDescriptorImageInfo image_info{};
  image_info.sampler = sampler;
  image_info.imageView = dummy_view;
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  std::array<VkWriteDescriptorSet, 4> writes{};
  for (uint32_t i = 0; i < writes.size(); ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = descriptor_set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &image_info;
  }
  vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                         writes.data(), 0, nullptr);
  return true;
}

// ── Shader modules ────────────────────────────────────────────────────────────

bool VkRenderer::Impl::CreateShaderModules(const std::string& image_shader) {
  const std::vector<uint32_t> vert_spv =
      CompileToSpirv(kVulkanVertexShader, ShaderStage::kVertex);
  if (vert_spv.empty())
    return false;
  const std::vector<uint32_t> frag_spv =
      CompileToSpirv(WrapVulkan(image_shader), ShaderStage::kFragment);
  if (frag_spv.empty()) {
    std::fprintf(stderr, "shadertoy: fragment shader compilation failed\n");
    return false;
  }
  vert_module = MakeModule(vert_spv);
  frag_module = MakeModule(frag_spv);
  if (vert_module == VK_NULL_HANDLE || frag_module == VK_NULL_HANDLE) {
    std::fprintf(stderr, "shadertoy: vkCreateShaderModule failed\n");
    return false;
  }
  return true;
}

// ── Pipeline ──────────────────────────────────────────────────────────────────

bool VkRenderer::Impl::CreatePipeline() {
  VkPushConstantRange pc_range{};
  pc_range.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  pc_range.offset = 0;
  pc_range.size = sizeof(PushConstants);

  VkPipelineLayoutCreateInfo linfo{};
  linfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  linfo.setLayoutCount = 1;
  linfo.pSetLayouts = &set_layout;
  linfo.pushConstantRangeCount = 1;
  linfo.pPushConstantRanges = &pc_range;
  ST_VK_CHECK(
      vkCreatePipelineLayout(device, &linfo, nullptr, &pipeline_layout),
      "vkCreatePipelineLayout failed");

  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert_module;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag_module;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vinput{};
  vinput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo asm_state{};
  asm_state.sType =
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  asm_state.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo vp{};
  vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rast{};
  rast.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rast.polygonMode = VK_POLYGON_MODE_FILL;
  rast.cullMode = VK_CULL_MODE_NONE;
  rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rast.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState blend_att{};
  blend_att.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_att;

  const std::array<VkDynamicState, 2> dyn_states = {
      VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{};
  dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dyn.dynamicStateCount = static_cast<uint32_t>(dyn_states.size());
  dyn.pDynamicStates = dyn_states.data();

  VkGraphicsPipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.stageCount = static_cast<uint32_t>(stages.size());
  info.pStages = stages.data();
  info.pVertexInputState = &vinput;
  info.pInputAssemblyState = &asm_state;
  info.pViewportState = &vp;
  info.pRasterizationState = &rast;
  info.pMultisampleState = &ms;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dyn;
  info.layout = pipeline_layout;
  info.renderPass = render_pass;
  info.subpass = 0;

  ST_VK_CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info,
                                        nullptr, &pipeline),
              "vkCreateGraphicsPipelines failed");
  return true;
}

// ── Command + sync resources ──────────────────────────────────────────────────

bool VkRenderer::Impl::CreateCommandResources() {
  VkCommandPoolCreateInfo pinfo{};
  pinfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pinfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pinfo.queueFamilyIndex = queue_family;
  ST_VK_CHECK(vkCreateCommandPool(device, &pinfo, nullptr, &command_pool),
              "vkCreateCommandPool failed");

  VkCommandBufferAllocateInfo cbinfo{};
  cbinfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbinfo.commandPool = command_pool;
  cbinfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbinfo.commandBufferCount = kMaxFramesInFlight;
  ST_VK_CHECK(
      vkAllocateCommandBuffers(device, &cbinfo, command_buffers.data()),
      "vkAllocateCommandBuffers failed");
  return true;
}

bool VkRenderer::Impl::CreateSyncObjects() {
  VkSemaphoreCreateInfo sem{};
  sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkFenceCreateInfo fence{};
  fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    ST_VK_CHECK(
        vkCreateSemaphore(device, &sem, nullptr, &image_available[i]),
        "vkCreateSemaphore failed");
    ST_VK_CHECK(
        vkCreateSemaphore(device, &sem, nullptr, &render_finished[i]),
        "vkCreateSemaphore failed");
    ST_VK_CHECK(vkCreateFence(device, &fence, nullptr, &in_flight[i]),
                "vkCreateFence failed");
  }
  return true;
}

// ── Swapchain lifecycle ───────────────────────────────────────────────────────

void VkRenderer::Impl::CleanupSwapchain() {
  for (VkFramebuffer fb : framebuffers)
    if (fb != VK_NULL_HANDLE)
      vkDestroyFramebuffer(device, fb, nullptr);
  framebuffers.clear();
  for (VkImageView v : views)
    if (v != VK_NULL_HANDLE)
      vkDestroyImageView(device, v, nullptr);
  views.clear();
  if (swapchain != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device, swapchain, nullptr);
    swapchain = VK_NULL_HANDLE;
  }
}

bool VkRenderer::Impl::RecreateSwapchain() {
  vkDeviceWaitIdle(device);
  CleanupSwapchain();
  return CreateSwapchain();
}

// ── Per-frame command recording ───────────────────────────────────────────────

bool VkRenderer::Impl::RecordCommandBuffer(VkCommandBuffer cmd,
                                           uint32_t image_index,
                                           const ShaderInputs& inputs) {
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  ST_VK_CHECK(vkBeginCommandBuffer(cmd, &begin),
              "vkBeginCommandBuffer failed");

  VkClearValue clear{};
  clear.color = {{0.0f, 0.0f, 0.0f, 1.0f}};
  VkRenderPassBeginInfo rp{};
  rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rp.renderPass = render_pass;
  rp.framebuffer = framebuffers[image_index];
  rp.renderArea.extent = extent;
  rp.clearValueCount = 1;
  rp.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

  VkViewport viewport{};
  viewport.width = static_cast<float>(extent.width);
  viewport.height = static_cast<float>(extent.height);
  viewport.maxDepth = 1.0f;
  vkCmdSetViewport(cmd, 0, 1, &viewport);
  VkRect2D scissor{};
  scissor.extent = extent;
  vkCmdSetScissor(cmd, 0, 1, &scissor);

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline_layout, 0, 1, &descriptor_set, 0, nullptr);

  // The shader expects iResolution to match the framebuffer it renders into.
  ShaderInputs frame_inputs = inputs;
  frame_inputs.res_x = static_cast<float>(extent.width);
  frame_inputs.res_y = static_cast<float>(extent.height);
  const PushConstants pc = ToPushConstants(frame_inputs);
  vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(pc), &pc);

  vkCmdDraw(cmd, 3, 1, 0, 0);
  vkCmdEndRenderPass(cmd);
  ST_VK_CHECK(vkEndCommandBuffer(cmd), "vkEndCommandBuffer failed");
  return true;
}

// ── Draw one frame ────────────────────────────────────────────────────────────

bool VkRenderer::Impl::DrawFrame(const ShaderInputs& inputs) {
  if (needs_recreate) {
    needs_recreate = false;
    if (!RecreateSwapchain())
      return true;  // likely minimized; try again next frame
  }
  if (swapchain == VK_NULL_HANDLE)
    return true;

  vkWaitForFences(device, 1, &in_flight[current_frame], VK_TRUE, UINT64_MAX);

  uint32_t image_index = 0;
  VkResult acquire = vkAcquireNextImageKHR(
      device, swapchain, UINT64_MAX, image_available[current_frame],
      VK_NULL_HANDLE, &image_index);
  if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
    needs_recreate = true;
    return true;
  }
  if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
    std::fprintf(stderr, "shadertoy: vkAcquireNextImageKHR failed (%d)\n",
                 static_cast<int>(acquire));
    return false;
  }

  vkResetFences(device, 1, &in_flight[current_frame]);
  VkCommandBuffer cmd = command_buffers[current_frame];
  vkResetCommandBuffer(cmd, 0);
  if (!RecordCommandBuffer(cmd, image_index, inputs))
    return false;

  const VkPipelineStageFlags wait_stage =
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &image_available[current_frame];
  submit.pWaitDstStageMask = &wait_stage;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &render_finished[current_frame];
  ST_VK_CHECK(
      vkQueueSubmit(queue, 1, &submit, in_flight[current_frame]),
      "vkQueueSubmit failed");

  VkPresentInfoKHR present{};
  present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &render_finished[current_frame];
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain;
  present.pImageIndices = &image_index;
  VkResult pres = vkQueuePresentKHR(queue, &present);
  if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR)
    needs_recreate = true;
  else if (pres != VK_SUCCESS)
    return false;

  current_frame = (current_frame + 1) % kMaxFramesInFlight;
  return true;
}

// ── Teardown ──────────────────────────────────────────────────────────────────

void VkRenderer::Impl::Cleanup() {
  if (device != VK_NULL_HANDLE)
    vkDeviceWaitIdle(device);

  for (uint32_t i = 0; i < kMaxFramesInFlight; ++i) {
    if (image_available[i] != VK_NULL_HANDLE)
      vkDestroySemaphore(device, image_available[i], nullptr);
    if (render_finished[i] != VK_NULL_HANDLE)
      vkDestroySemaphore(device, render_finished[i], nullptr);
    if (in_flight[i] != VK_NULL_HANDLE)
      vkDestroyFence(device, in_flight[i], nullptr);
  }
  if (command_pool != VK_NULL_HANDLE)
    vkDestroyCommandPool(device, command_pool, nullptr);

  CleanupSwapchain();

  if (pipeline != VK_NULL_HANDLE)
    vkDestroyPipeline(device, pipeline, nullptr);
  if (pipeline_layout != VK_NULL_HANDLE)
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
  if (render_pass != VK_NULL_HANDLE)
    vkDestroyRenderPass(device, render_pass, nullptr);
  if (vert_module != VK_NULL_HANDLE)
    vkDestroyShaderModule(device, vert_module, nullptr);
  if (frag_module != VK_NULL_HANDLE)
    vkDestroyShaderModule(device, frag_module, nullptr);

  if (descriptor_pool != VK_NULL_HANDLE)
    vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
  if (set_layout != VK_NULL_HANDLE)
    vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
  if (sampler != VK_NULL_HANDLE)
    vkDestroySampler(device, sampler, nullptr);
  if (dummy_view != VK_NULL_HANDLE)
    vkDestroyImageView(device, dummy_view, nullptr);
  if (dummy_image != VK_NULL_HANDLE)
    vkDestroyImage(device, dummy_image, nullptr);
  if (dummy_memory != VK_NULL_HANDLE)
    vkFreeMemory(device, dummy_memory, nullptr);

  if (device != VK_NULL_HANDLE)
    vkDestroyDevice(device, nullptr);
  if (surface != VK_NULL_HANDLE)
    vkDestroySurfaceKHR(instance, surface, nullptr);
  if (instance != VK_NULL_HANDLE)
    vkDestroyInstance(instance, nullptr);
}

// ══════════════════════════════════════════════════════════════════════════════
// VkRenderer (public facade)
// ══════════════════════════════════════════════════════════════════════════════

std::unique_ptr<VkRenderer> VkRenderer::Create(const VkRendererConfig& config) {
  std::unique_ptr<VkRenderer> self(new VkRenderer());
  self->impl_ = std::make_unique<Impl>();
  Impl& d = *self->impl_;
  d.vsync = config.vsync;
  d.desired_width = config.width;
  d.desired_height = config.height;

  if (!d.CreateInstance(config))
    return nullptr;
  d.surface = config.create_surface ? config.create_surface(d.instance)
                                    : VK_NULL_HANDLE;
  if (d.surface == VK_NULL_HANDLE) {
    std::fprintf(stderr, "shadertoy: surface creation failed\n");
    return nullptr;
  }
  if (!d.PickPhysicalDevice() || !d.CreateDevice() || !d.CreateRenderPass() ||
      !d.CreateDescriptorResources() ||
      !d.CreateShaderModules(config.image_shader) || !d.CreatePipeline() ||
      !d.CreateCommandResources() || !d.CreateSyncObjects() ||
      !d.CreateSwapchain()) {
    return nullptr;
  }
  return self;
}

VkRenderer::~VkRenderer() = default;

bool VkRenderer::Render(const ShaderInputs& inputs) {
  return impl_->DrawFrame(inputs);
}

void VkRenderer::Resize(uint32_t width, uint32_t height) noexcept {
  impl_->desired_width = width;
  impl_->desired_height = height;
  impl_->needs_recreate = true;
}

}  // namespace shadertoy
