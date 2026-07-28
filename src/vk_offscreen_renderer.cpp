// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// vk_offscreen_renderer.cpp — see vk_offscreen_renderer.hpp.

#include "shadertoy/vk_offscreen_renderer.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "shadertoy/spirv_compile.hpp"

namespace shadertoy {
namespace {

// Log a Vulkan failure and bail out of the calling bool-returning function.
#define ST_VKO_CHECK(expr, msg)                                    \
  do {                                                             \
    const VkResult _r = (expr);                                    \
    if (_r != VK_SUCCESS) {                                        \
      std::fprintf(stderr, "shadertoy: %s (VkResult %d)\n", (msg), \
                   static_cast<int>(_r));                          \
      return false;                                                \
    }                                                              \
  } while (0)

// A full-screen triangle generated from gl_VertexIndex — no vertex buffer.
constexpr const char* kVertexShader = R"GLSL(#version 450
void main() {
    vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

constexpr uint32_t kChannelCount = 4;

}  // namespace

// ══════════════════════════════════════════════════════════════════════════════
// Dispatch table
// ══════════════════════════════════════════════════════════════════════════════
//
// Every entry point the renderer uses, resolved through the host's loader. Not
// a convenience: a host that interposes vkQueueSubmit to serialize a shared
// queue is only correct if everyone submitting on that queue went through the
// same loader, so linking libvulkan and calling directly would reintroduce the
// race the host is trying to prevent.
struct VkOffscreenApi {
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
  PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties =
      nullptr;

  PFN_vkCreateShaderModule CreateShaderModule = nullptr;
  PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
  PFN_vkCreateRenderPass CreateRenderPass = nullptr;
  PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
  PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
  PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
  PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
  PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
  PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
  PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
  PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
  PFN_vkDestroyPipeline DestroyPipeline = nullptr;
  PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
  PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
  PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
  PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
  PFN_vkCreateSampler CreateSampler = nullptr;
  PFN_vkDestroySampler DestroySampler = nullptr;
  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkDestroyImage DestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
  PFN_vkBindImageMemory BindImageMemory = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkFreeMemory FreeMemory = nullptr;
  PFN_vkCreateImageView CreateImageView = nullptr;
  PFN_vkDestroyImageView DestroyImageView = nullptr;
  PFN_vkCreateCommandPool CreateCommandPool = nullptr;
  PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
  PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
  PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
  PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
  PFN_vkQueueSubmit QueueSubmit = nullptr;
  PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
  PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
  PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
  PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
  PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
  PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
  PFN_vkCmdPushConstants CmdPushConstants = nullptr;
  PFN_vkCmdDraw CmdDraw = nullptr;
  PFN_vkCmdSetViewport CmdSetViewport = nullptr;
  PFN_vkCmdSetScissor CmdSetScissor = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCmdClearColorImage CmdClearColorImage = nullptr;

  [[nodiscard]] bool Load(PFN_vkGetInstanceProcAddr gipa,
                          VkInstance instance,
                          VkDevice device);
};

namespace {

// Resolve one entry point, recording the first failure so Load can name it.
template <typename Fn>
bool Resolve(Fn& slot,
             PFN_vkVoidFunction raw,
             const char* name,
             const char** missing) {
  slot = reinterpret_cast<Fn>(raw);
  if (slot == nullptr && *missing == nullptr) {
    *missing = name;
    return false;
  }
  return slot != nullptr;
}

}  // namespace

bool VkOffscreenApi::Load(PFN_vkGetInstanceProcAddr gipa,
                          VkInstance instance,
                          VkDevice device) {
  if (gipa == nullptr) {
    std::fprintf(stderr,
                 "shadertoy: VkExternalDevice.get_instance_proc_addr is null; "
                 "the renderer cannot resolve entry points\n");
    return false;
  }
  const char* missing = nullptr;

#define ST_VKO_INSTANCE_FN(field, name) \
  Resolve(field, gipa(instance, name), name, &missing)

  ST_VKO_INSTANCE_FN(GetDeviceProcAddr, "vkGetDeviceProcAddr");
  ST_VKO_INSTANCE_FN(GetPhysicalDeviceMemoryProperties,
                     "vkGetPhysicalDeviceMemoryProperties");
#undef ST_VKO_INSTANCE_FN

  if (GetDeviceProcAddr == nullptr) {
    std::fprintf(stderr, "shadertoy: could not resolve vkGetDeviceProcAddr\n");
    return false;
  }

  // Device-level entry points go through vkGetDeviceProcAddr, which skips the
  // loader's dispatch trampoline. The interposed vkQueueSubmit is still
  // honored: an interposing loader returns its own wrapper from
  // vkGetDeviceProcAddr too.
#define ST_VKO_DEVICE_FN(field, name) \
  Resolve(field, GetDeviceProcAddr(device, name), name, &missing)

  ST_VKO_DEVICE_FN(CreateShaderModule, "vkCreateShaderModule");
  ST_VKO_DEVICE_FN(DestroyShaderModule, "vkDestroyShaderModule");
  ST_VKO_DEVICE_FN(CreateRenderPass, "vkCreateRenderPass");
  ST_VKO_DEVICE_FN(DestroyRenderPass, "vkDestroyRenderPass");
  ST_VKO_DEVICE_FN(CreateFramebuffer, "vkCreateFramebuffer");
  ST_VKO_DEVICE_FN(DestroyFramebuffer, "vkDestroyFramebuffer");
  ST_VKO_DEVICE_FN(CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout");
  ST_VKO_DEVICE_FN(DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout");
  ST_VKO_DEVICE_FN(CreatePipelineLayout, "vkCreatePipelineLayout");
  ST_VKO_DEVICE_FN(DestroyPipelineLayout, "vkDestroyPipelineLayout");
  ST_VKO_DEVICE_FN(CreateGraphicsPipelines, "vkCreateGraphicsPipelines");
  ST_VKO_DEVICE_FN(DestroyPipeline, "vkDestroyPipeline");
  ST_VKO_DEVICE_FN(CreateDescriptorPool, "vkCreateDescriptorPool");
  ST_VKO_DEVICE_FN(DestroyDescriptorPool, "vkDestroyDescriptorPool");
  ST_VKO_DEVICE_FN(AllocateDescriptorSets, "vkAllocateDescriptorSets");
  ST_VKO_DEVICE_FN(UpdateDescriptorSets, "vkUpdateDescriptorSets");
  ST_VKO_DEVICE_FN(CreateSampler, "vkCreateSampler");
  ST_VKO_DEVICE_FN(DestroySampler, "vkDestroySampler");
  ST_VKO_DEVICE_FN(CreateImage, "vkCreateImage");
  ST_VKO_DEVICE_FN(DestroyImage, "vkDestroyImage");
  ST_VKO_DEVICE_FN(GetImageMemoryRequirements, "vkGetImageMemoryRequirements");
  ST_VKO_DEVICE_FN(BindImageMemory, "vkBindImageMemory");
  ST_VKO_DEVICE_FN(AllocateMemory, "vkAllocateMemory");
  ST_VKO_DEVICE_FN(FreeMemory, "vkFreeMemory");
  ST_VKO_DEVICE_FN(CreateImageView, "vkCreateImageView");
  ST_VKO_DEVICE_FN(DestroyImageView, "vkDestroyImageView");
  ST_VKO_DEVICE_FN(CreateCommandPool, "vkCreateCommandPool");
  ST_VKO_DEVICE_FN(DestroyCommandPool, "vkDestroyCommandPool");
  ST_VKO_DEVICE_FN(AllocateCommandBuffers, "vkAllocateCommandBuffers");
  ST_VKO_DEVICE_FN(FreeCommandBuffers, "vkFreeCommandBuffers");
  ST_VKO_DEVICE_FN(BeginCommandBuffer, "vkBeginCommandBuffer");
  ST_VKO_DEVICE_FN(EndCommandBuffer, "vkEndCommandBuffer");
  ST_VKO_DEVICE_FN(QueueSubmit, "vkQueueSubmit");
  ST_VKO_DEVICE_FN(QueueWaitIdle, "vkQueueWaitIdle");
  ST_VKO_DEVICE_FN(DeviceWaitIdle, "vkDeviceWaitIdle");
  ST_VKO_DEVICE_FN(CmdBeginRenderPass, "vkCmdBeginRenderPass");
  ST_VKO_DEVICE_FN(CmdEndRenderPass, "vkCmdEndRenderPass");
  ST_VKO_DEVICE_FN(CmdBindPipeline, "vkCmdBindPipeline");
  ST_VKO_DEVICE_FN(CmdBindDescriptorSets, "vkCmdBindDescriptorSets");
  ST_VKO_DEVICE_FN(CmdPushConstants, "vkCmdPushConstants");
  ST_VKO_DEVICE_FN(CmdDraw, "vkCmdDraw");
  ST_VKO_DEVICE_FN(CmdSetViewport, "vkCmdSetViewport");
  ST_VKO_DEVICE_FN(CmdSetScissor, "vkCmdSetScissor");
  ST_VKO_DEVICE_FN(CmdPipelineBarrier, "vkCmdPipelineBarrier");
  ST_VKO_DEVICE_FN(CmdClearColorImage, "vkCmdClearColorImage");
#undef ST_VKO_DEVICE_FN

  if (missing != nullptr) {
    std::fprintf(stderr, "shadertoy: could not resolve %s\n", missing);
    return false;
  }
  return true;
}

// ══════════════════════════════════════════════════════════════════════════════
// VkOffscreenRenderer::Impl
// ══════════════════════════════════════════════════════════════════════════════

struct VkOffscreenRenderer::Impl {
  VkExternalDevice dev{};
  VkOffscreenConfig cfg{};
  VkOffscreenApi api{};

  VkRenderPass render_pass = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkShaderModule vert_module = VK_NULL_HANDLE;
  VkShaderModule frag_module = VK_NULL_HANDLE;

  // 1x1 black stub bound to every iChannel, matching the swapchain renderer.
  VkImage stub_image = VK_NULL_HANDLE;
  VkDeviceMemory stub_memory = VK_NULL_HANDLE;
  VkImageView stub_view = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

  VkCommandPool setup_pool = VK_NULL_HANDLE;

  // Framebuffers are keyed by image view: a host cycling a ring of N images
  // presents the same handful of views over and over, so building one per
  // frame would be pure waste.
  std::unordered_map<VkImageView, VkFramebuffer> framebuffers;

  ~Impl() { Cleanup(); }

  [[nodiscard]] bool CreateRenderPass();
  [[nodiscard]] bool CreateStubTexture();
  [[nodiscard]] bool CreateDescriptors();
  [[nodiscard]] bool BuildPipeline(const std::string& frag_glsl);
  [[nodiscard]] VkFramebuffer FramebufferFor(const VkOffscreenTarget& target);
  void Cleanup();

  [[nodiscard]] bool FindMemoryType(uint32_t type_bits,
                                    VkMemoryPropertyFlags props,
                                    uint32_t* out_index) const;
  [[nodiscard]] VkShaderModule MakeModule(const std::vector<uint32_t>& spirv);
};

// ── Render pass
// ───────────────────────────────────────────────────────────────

bool VkOffscreenRenderer::Impl::CreateRenderPass() {
  VkAttachmentDescription color{};
  color.format = cfg.color_format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  // The Image pass covers every pixel, so the previous contents are never read.
  // CLEAR rather than DONT_CARE all the same: a shader that writes alpha < 1,
  // or a driver that would otherwise hand back uninitialized memory, should not
  // leak whatever the ring slot held two frames ago.
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = cfg.initial_layout;
  color.finalLayout = cfg.final_layout;

  VkAttachmentReference color_ref{};
  color_ref.attachment = 0;
  color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &color_ref;

  // Two dependencies rather than one. The first orders the attachment write
  // after whatever the host did to the image before (nothing, for an UNDEFINED
  // ring slot). The second is what makes the result safe for the host to
  // consume — without it the transition to final_layout is unordered against
  // the host's copy or sample, which is exactly the hand-off this renderer
  // exists to support.
  std::array<VkSubpassDependency, 2> deps{};
  deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  deps[0].dstSubpass = 0;
  deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].srcAccessMask = 0;
  deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  deps[1].srcSubpass = 0;
  deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstAccessMask =
      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT;

  VkRenderPassCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = 1;
  info.pAttachments = &color;
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  info.dependencyCount = static_cast<uint32_t>(deps.size());
  info.pDependencies = deps.data();

  ST_VKO_CHECK(api.CreateRenderPass(dev.device, &info, nullptr, &render_pass),
               "vkCreateRenderPass failed");
  return true;
}

// ── Stub channel texture
// ──────────────────────────────────────────────────────

bool VkOffscreenRenderer::Impl::FindMemoryType(uint32_t type_bits,
                                               VkMemoryPropertyFlags props,
                                               uint32_t* out_index) const {
  VkPhysicalDeviceMemoryProperties mem{};
  api.GetPhysicalDeviceMemoryProperties(dev.physical_device, &mem);
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    const bool usable = (type_bits & (1U << i)) != 0;
    const bool suitable = (mem.memoryTypes[i].propertyFlags & props) == props;
    if (usable && suitable) {
      *out_index = i;
      return true;
    }
  }
  std::fprintf(stderr, "shadertoy: no memory type with the required flags\n");
  return false;
}

bool VkOffscreenRenderer::Impl::CreateStubTexture() {
  VkImageCreateInfo image{};
  image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = VK_FORMAT_R8G8B8A8_UNORM;
  image.extent = {1, 1, 1};
  image.mipLevels = 1;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ST_VKO_CHECK(api.CreateImage(dev.device, &image, nullptr, &stub_image),
               "vkCreateImage(stub) failed");

  VkMemoryRequirements req{};
  api.GetImageMemoryRequirements(dev.device, stub_image, &req);
  uint32_t type_index = 0;
  if (!FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      &type_index)) {
    return false;
  }
  VkMemoryAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  alloc.allocationSize = req.size;
  alloc.memoryTypeIndex = type_index;
  ST_VKO_CHECK(api.AllocateMemory(dev.device, &alloc, nullptr, &stub_memory),
               "vkAllocateMemory(stub) failed");
  ST_VKO_CHECK(api.BindImageMemory(dev.device, stub_image, stub_memory, 0),
               "vkBindImageMemory(stub) failed");

  // Clear to opaque black on the device rather than staging a 4-byte upload —
  // one command buffer, no host-visible buffer, same result.
  VkCommandPoolCreateInfo pool{};
  pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
  pool.queueFamilyIndex = dev.queue_family_index;
  ST_VKO_CHECK(api.CreateCommandPool(dev.device, &pool, nullptr, &setup_pool),
               "vkCreateCommandPool(setup) failed");

  VkCommandBufferAllocateInfo cba{};
  cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cba.commandPool = setup_pool;
  cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cba.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  ST_VKO_CHECK(api.AllocateCommandBuffers(dev.device, &cba, &cmd),
               "vkAllocateCommandBuffers(setup) failed");

  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ST_VKO_CHECK(api.BeginCommandBuffer(cmd, &begin),
               "vkBeginCommandBuffer(setup) failed");

  VkImageMemoryBarrier to_dst{};
  to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.image = stub_image;
  to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  to_dst.srcAccessMask = 0;
  to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_dst);

  const VkClearColorValue black{{0.0F, 0.0F, 0.0F, 1.0F}};
  const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  api.CmdClearColorImage(cmd, stub_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &black, 1, &range);

  VkImageMemoryBarrier to_read = to_dst;
  to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &to_read);

  ST_VKO_CHECK(api.EndCommandBuffer(cmd), "vkEndCommandBuffer(setup) failed");

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  // The only queue submission this renderer ever makes. Frame work is recorded
  // into the caller's command buffer and submitted by the caller.
  ST_VKO_CHECK(api.QueueSubmit(dev.queue, 1, &submit, VK_NULL_HANDLE),
               "vkQueueSubmit(setup) failed");
  ST_VKO_CHECK(api.QueueWaitIdle(dev.queue), "vkQueueWaitIdle(setup) failed");
  api.FreeCommandBuffers(dev.device, setup_pool, 1, &cmd);

  VkImageViewCreateInfo view{};
  view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view.image = stub_image;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = VK_FORMAT_R8G8B8A8_UNORM;
  view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  ST_VKO_CHECK(api.CreateImageView(dev.device, &view, nullptr, &stub_view),
               "vkCreateImageView(stub) failed");
  return true;
}

// ── Descriptors
// ───────────────────────────────────────────────────────────────

bool VkOffscreenRenderer::Impl::CreateDescriptors() {
  VkSamplerCreateInfo sampler_info{};
  sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampler_info.magFilter = VK_FILTER_LINEAR;
  sampler_info.minFilter = VK_FILTER_LINEAR;
  sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  sampler_info.maxLod = VK_LOD_CLAMP_NONE;
  ST_VKO_CHECK(api.CreateSampler(dev.device, &sampler_info, nullptr, &sampler),
               "vkCreateSampler failed");

  std::array<VkDescriptorSetLayoutBinding, kChannelCount> bindings{};
  for (uint32_t i = 0; i < kChannelCount; ++i) {
    bindings[i].binding = i;
    bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    bindings[i].descriptorCount = 1;
    bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  }
  VkDescriptorSetLayoutCreateInfo layout{};
  layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layout.bindingCount = static_cast<uint32_t>(bindings.size());
  layout.pBindings = bindings.data();
  ST_VKO_CHECK(
      api.CreateDescriptorSetLayout(dev.device, &layout, nullptr, &set_layout),
      "vkCreateDescriptorSetLayout failed");

  VkDescriptorPoolSize size{};
  size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  size.descriptorCount = kChannelCount;
  VkDescriptorPoolCreateInfo pool{};
  pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pool.maxSets = 1;
  pool.poolSizeCount = 1;
  pool.pPoolSizes = &size;
  ST_VKO_CHECK(
      api.CreateDescriptorPool(dev.device, &pool, nullptr, &descriptor_pool),
      "vkCreateDescriptorPool failed");

  VkDescriptorSetAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  alloc.descriptorPool = descriptor_pool;
  alloc.descriptorSetCount = 1;
  alloc.pSetLayouts = &set_layout;
  ST_VKO_CHECK(api.AllocateDescriptorSets(dev.device, &alloc, &descriptor_set),
               "vkAllocateDescriptorSets failed");

  VkDescriptorImageInfo image_info{};
  image_info.sampler = sampler;
  image_info.imageView = stub_view;
  image_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  std::array<VkWriteDescriptorSet, kChannelCount> writes{};
  for (uint32_t i = 0; i < kChannelCount; ++i) {
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = descriptor_set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &image_info;
  }
  api.UpdateDescriptorSets(dev.device, static_cast<uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);
  return true;
}

// ── Pipeline
// ──────────────────────────────────────────────────────────────────

VkShaderModule VkOffscreenRenderer::Impl::MakeModule(
    const std::vector<uint32_t>& spirv) {
  if (spirv.empty()) {
    return VK_NULL_HANDLE;
  }
  VkShaderModuleCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = spirv.size() * sizeof(uint32_t);
  info.pCode = spirv.data();
  VkShaderModule module = VK_NULL_HANDLE;
  if (api.CreateShaderModule(dev.device, &info, nullptr, &module) !=
      VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return module;
}

bool VkOffscreenRenderer::Impl::BuildPipeline(const std::string& frag_glsl) {
  const std::vector<uint32_t> vert_spirv =
      CompileToSpirv(kVertexShader, ShaderStage::kVertex);
  const std::vector<uint32_t> frag_spirv =
      CompileToSpirv(frag_glsl, ShaderStage::kFragment);
  if (vert_spirv.empty() || frag_spirv.empty()) {
    std::fprintf(stderr, "shadertoy: GLSL to SPIR-V compilation failed\n");
    return false;
  }

  // Build into locals and only publish on success, so a failed SetProgram
  // leaves the previous program running.
  VkShaderModule new_vert = MakeModule(vert_spirv);
  VkShaderModule new_frag = MakeModule(frag_spirv);
  if (new_vert == VK_NULL_HANDLE || new_frag == VK_NULL_HANDLE) {
    if (new_vert != VK_NULL_HANDLE) {
      api.DestroyShaderModule(dev.device, new_vert, nullptr);
    }
    if (new_frag != VK_NULL_HANDLE) {
      api.DestroyShaderModule(dev.device, new_frag, nullptr);
    }
    std::fprintf(stderr, "shadertoy: vkCreateShaderModule failed\n");
    return false;
  }

  if (pipeline_layout == VK_NULL_HANDLE) {
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.offset = 0;
    push.size = sizeof(PushConstants);
    VkPipelineLayoutCreateInfo layout{};
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (api.CreatePipelineLayout(dev.device, &layout, nullptr,
                                 &pipeline_layout) != VK_SUCCESS) {
      api.DestroyShaderModule(dev.device, new_vert, nullptr);
      api.DestroyShaderModule(dev.device, new_frag, nullptr);
      std::fprintf(stderr, "shadertoy: vkCreatePipelineLayout failed\n");
      return false;
    }
  }

  std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = new_vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = new_frag;
  stages[1].pName = "main";

  VkPipelineVertexInputStateCreateInfo vertex_input{};
  vertex_input.sType =
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

  VkPipelineInputAssemblyStateCreateInfo assembly{};
  assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  // Viewport and scissor are dynamic: the target size is a per-frame property
  // here, so baking it into the pipeline would mean rebuilding on every resize.
  VkPipelineViewportStateCreateInfo viewport{};
  viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0F;

  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState blend_attachment{};
  blend_attachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1;
  blend.pAttachments = &blend_attachment;

  const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                     VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic{};
  dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamic.dynamicStateCount = static_cast<uint32_t>(dynamic_states.size());
  dynamic.pDynamicStates = dynamic_states.data();

  VkGraphicsPipelineCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  info.stageCount = static_cast<uint32_t>(stages.size());
  info.pStages = stages.data();
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &raster;
  info.pMultisampleState = &multisample;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = pipeline_layout;
  info.renderPass = render_pass;
  info.subpass = 0;

  VkPipeline new_pipeline = VK_NULL_HANDLE;
  const VkResult result = api.CreateGraphicsPipelines(
      dev.device, VK_NULL_HANDLE, 1, &info, nullptr, &new_pipeline);
  if (result != VK_SUCCESS) {
    api.DestroyShaderModule(dev.device, new_vert, nullptr);
    api.DestroyShaderModule(dev.device, new_frag, nullptr);
    std::fprintf(stderr, "shadertoy: vkCreateGraphicsPipelines (VkResult %d)\n",
                 static_cast<int>(result));
    return false;
  }

  // Success — retire the old program. The caller must not have work in flight
  // against it; the host owns submission and so owns that guarantee.
  if (pipeline != VK_NULL_HANDLE) {
    api.DestroyPipeline(dev.device, pipeline, nullptr);
  }
  if (vert_module != VK_NULL_HANDLE) {
    api.DestroyShaderModule(dev.device, vert_module, nullptr);
  }
  if (frag_module != VK_NULL_HANDLE) {
    api.DestroyShaderModule(dev.device, frag_module, nullptr);
  }
  pipeline = new_pipeline;
  vert_module = new_vert;
  frag_module = new_frag;
  return true;
}

// ── Framebuffer cache
// ─────────────────────────────────────────────────────────

VkFramebuffer VkOffscreenRenderer::Impl::FramebufferFor(
    const VkOffscreenTarget& target) {
  if (const auto it = framebuffers.find(target.view);
      it != framebuffers.end()) {
    return it->second;
  }
  VkFramebufferCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  info.renderPass = render_pass;
  info.attachmentCount = 1;
  info.pAttachments = &target.view;
  info.width = target.width;
  info.height = target.height;
  info.layers = 1;
  VkFramebuffer fb = VK_NULL_HANDLE;
  if (api.CreateFramebuffer(dev.device, &info, nullptr, &fb) != VK_SUCCESS) {
    std::fprintf(stderr, "shadertoy: vkCreateFramebuffer failed\n");
    return VK_NULL_HANDLE;
  }
  framebuffers.emplace(target.view, fb);
  return fb;
}

// ── Teardown
// ──────────────────────────────────────────────────────────────────

void VkOffscreenRenderer::Impl::Cleanup() {
  if (dev.device == VK_NULL_HANDLE || api.DeviceWaitIdle == nullptr) {
    return;
  }
  // The host may still have work in flight referencing these objects. It owns
  // submission, so it should have idled already — but destroying a live
  // pipeline is unrecoverable, and one wait at teardown costs nothing.
  api.DeviceWaitIdle(dev.device);

  for (auto& [view, fb] : framebuffers) {
    api.DestroyFramebuffer(dev.device, fb, nullptr);
  }
  framebuffers.clear();

  if (pipeline != VK_NULL_HANDLE) {
    api.DestroyPipeline(dev.device, pipeline, nullptr);
    pipeline = VK_NULL_HANDLE;
  }
  if (pipeline_layout != VK_NULL_HANDLE) {
    api.DestroyPipelineLayout(dev.device, pipeline_layout, nullptr);
    pipeline_layout = VK_NULL_HANDLE;
  }
  if (vert_module != VK_NULL_HANDLE) {
    api.DestroyShaderModule(dev.device, vert_module, nullptr);
    vert_module = VK_NULL_HANDLE;
  }
  if (frag_module != VK_NULL_HANDLE) {
    api.DestroyShaderModule(dev.device, frag_module, nullptr);
    frag_module = VK_NULL_HANDLE;
  }
  if (descriptor_pool != VK_NULL_HANDLE) {
    api.DestroyDescriptorPool(dev.device, descriptor_pool, nullptr);
    descriptor_pool = VK_NULL_HANDLE;
  }
  if (set_layout != VK_NULL_HANDLE) {
    api.DestroyDescriptorSetLayout(dev.device, set_layout, nullptr);
    set_layout = VK_NULL_HANDLE;
  }
  if (sampler != VK_NULL_HANDLE) {
    api.DestroySampler(dev.device, sampler, nullptr);
    sampler = VK_NULL_HANDLE;
  }
  if (stub_view != VK_NULL_HANDLE) {
    api.DestroyImageView(dev.device, stub_view, nullptr);
    stub_view = VK_NULL_HANDLE;
  }
  if (stub_image != VK_NULL_HANDLE) {
    api.DestroyImage(dev.device, stub_image, nullptr);
    stub_image = VK_NULL_HANDLE;
  }
  if (stub_memory != VK_NULL_HANDLE) {
    api.FreeMemory(dev.device, stub_memory, nullptr);
    stub_memory = VK_NULL_HANDLE;
  }
  if (setup_pool != VK_NULL_HANDLE) {
    api.DestroyCommandPool(dev.device, setup_pool, nullptr);
    setup_pool = VK_NULL_HANDLE;
  }
  if (render_pass != VK_NULL_HANDLE) {
    api.DestroyRenderPass(dev.device, render_pass, nullptr);
    render_pass = VK_NULL_HANDLE;
  }
  // dev.instance / physical_device / device / queue are the host's: not ours
  // to destroy.
}

// ══════════════════════════════════════════════════════════════════════════════
// VkOffscreenRenderer
// ══════════════════════════════════════════════════════════════════════════════

VkOffscreenRenderer::VkOffscreenRenderer() : impl_(std::make_unique<Impl>()) {}
VkOffscreenRenderer::~VkOffscreenRenderer() = default;

std::unique_ptr<VkOffscreenRenderer> VkOffscreenRenderer::Create(
    const VkExternalDevice& device,
    const VkOffscreenConfig& config) {
  if (device.device == VK_NULL_HANDLE ||
      device.physical_device == VK_NULL_HANDLE ||
      device.queue == VK_NULL_HANDLE) {
    std::fprintf(stderr,
                 "shadertoy: VkExternalDevice is missing a device, physical "
                 "device, or queue\n");
    return nullptr;
  }

  std::unique_ptr<VkOffscreenRenderer> self(new VkOffscreenRenderer());
  Impl& impl = *self->impl_;
  impl.dev = device;
  impl.cfg = config;

  if (!impl.api.Load(device.get_instance_proc_addr, device.instance,
                     device.device)) {
    return nullptr;
  }
  if (!impl.CreateRenderPass() || !impl.CreateStubTexture() ||
      !impl.CreateDescriptors()) {
    return nullptr;
  }
  return self;
}

bool VkOffscreenRenderer::SetProgram(const ShaderProgram& program) {
  if (program.multipass()) {
    std::fprintf(stderr,
                 "shadertoy: VkOffscreenRenderer does not implement Buffer "
                 "A..D yet; refusing '%s' rather than rendering only its Image "
                 "pass\n",
                 program.name.c_str());
    return false;
  }
  // Channels are not bound yet either — every iChannel samples the 1x1 black
  // stub, so declare them all as sampler2D. Same behavior as the swapchain
  // renderer; a shader that samples a cubemap channel will not compile.
  const std::string frag = WrapVulkan(program.common, program.image.code);
  return impl_->BuildPipeline(frag);
}

bool VkOffscreenRenderer::Init(const std::string& image_shader) {
  return SetProgram(MakeSinglePass(image_shader));
}

bool VkOffscreenRenderer::has_program() const noexcept {
  return impl_->pipeline != VK_NULL_HANDLE;
}

bool VkOffscreenRenderer::RecordRender(VkCommandBuffer cmd,
                                       const ShaderInputs& inputs,
                                       const VkOffscreenTarget& target) {
  Impl& impl = *impl_;
  if (impl.pipeline == VK_NULL_HANDLE) {
    std::fprintf(stderr, "shadertoy: RecordRender with no program set\n");
    return false;
  }
  if (cmd == VK_NULL_HANDLE || target.image == VK_NULL_HANDLE ||
      target.view == VK_NULL_HANDLE || target.width == 0 ||
      target.height == 0) {
    std::fprintf(stderr,
                 "shadertoy: RecordRender given an incomplete target\n");
    return false;
  }
  const VkFramebuffer fb = impl.FramebufferFor(target);
  if (fb == VK_NULL_HANDLE) {
    return false;
  }

  VkClearValue clear{};
  clear.color = {{0.0F, 0.0F, 0.0F, 1.0F}};

  VkRenderPassBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin.renderPass = impl.render_pass;
  begin.framebuffer = fb;
  begin.renderArea.offset = {0, 0};
  begin.renderArea.extent = {target.width, target.height};
  begin.clearValueCount = 1;
  begin.pClearValues = &clear;
  impl.api.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport{};
  viewport.x = 0.0F;
  viewport.y = 0.0F;
  viewport.width = static_cast<float>(target.width);
  viewport.height = static_cast<float>(target.height);
  viewport.minDepth = 0.0F;
  viewport.maxDepth = 1.0F;
  impl.api.CmdSetViewport(cmd, 0, 1, &viewport);

  VkRect2D scissor{};
  scissor.offset = {0, 0};
  scissor.extent = {target.width, target.height};
  impl.api.CmdSetScissor(cmd, 0, 1, &scissor);

  impl.api.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, impl.pipeline);
  impl.api.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 impl.pipeline_layout, 0, 1,
                                 &impl.descriptor_set, 0, nullptr);

  const PushConstants push = ToPushConstants(inputs);
  impl.api.CmdPushConstants(cmd, impl.pipeline_layout,
                            VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                            &push);

  impl.api.CmdDraw(cmd, 3, 1, 0, 0);
  impl.api.CmdEndRenderPass(cmd);
  return true;
}

void VkOffscreenRenderer::ForgetTarget(VkImageView view) {
  Impl& impl = *impl_;
  const auto it = impl.framebuffers.find(view);
  if (it == impl.framebuffers.end()) {
    return;
  }
  impl.api.DestroyFramebuffer(impl.dev.device, it->second, nullptr);
  impl.framebuffers.erase(it);
}

}  // namespace shadertoy
