// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// vk_offscreen_renderer.cpp — see vk_offscreen_renderer.hpp.

#include "shadertoy/vk_offscreen_renderer.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "shadertoy/inputs.hpp"  // ResolveMediaPath
#include "shadertoy/spirv_compile.hpp"

#include "stb_image.h"

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

// Candidate storage formats for Buffer A..D, best first.
//
// Half-float leads because Shadertoy buffers are as often accumulators and
// state as they are color, carrying values well outside [0,1]; a UNORM target
// clamps and bands exactly the feedback effects that need the range. The UNORM
// entries are fallbacks, and taking one silently changes what a shader
// computes, so the selection says so out loud.
//
// Chosen by asking the driver rather than by reputation. On a Pi 5 (V3D, V3DV
// Mesa) R16G16B16A16_UNORM is not a color attachment at all and
// R32G32B32A32_SFLOAT cannot be linearly filtered -- one fails at image
// creation, the other silently samples wrong -- so neither is listed.
// B10G11R11 is excluded for having no alpha to write.
// examples/vk_format_probe prints the same table for any target.
constexpr std::array<VkFormat, 3> kBufferFormatCandidates{
    VK_FORMAT_R16G16B16A16_SFLOAT,
    VK_FORMAT_A2B10G10R10_UNORM_PACK32,
    VK_FORMAT_R8G8B8A8_UNORM,
};

// A buffer is rendered into and then sampled, with the bilinear fetch a
// Shadertoy shader expects. Blend is deliberately not required: the full-screen
// pass overwrites its target.
constexpr VkFormatFeatureFlags kBufferFormatFeatures =
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

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
  PFN_vkGetPhysicalDeviceFormatProperties GetPhysicalDeviceFormatProperties =
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
  PFN_vkCreateBuffer CreateBuffer = nullptr;
  PFN_vkDestroyBuffer DestroyBuffer = nullptr;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
  PFN_vkBindBufferMemory BindBufferMemory = nullptr;
  PFN_vkMapMemory MapMemory = nullptr;
  PFN_vkUnmapMemory UnmapMemory = nullptr;
  PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
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
  ST_VKO_INSTANCE_FN(GetPhysicalDeviceFormatProperties,
                     "vkGetPhysicalDeviceFormatProperties");
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
  ST_VKO_DEVICE_FN(CreateBuffer, "vkCreateBuffer");
  ST_VKO_DEVICE_FN(DestroyBuffer, "vkDestroyBuffer");
  ST_VKO_DEVICE_FN(GetBufferMemoryRequirements,
                   "vkGetBufferMemoryRequirements");
  ST_VKO_DEVICE_FN(BindBufferMemory, "vkBindBufferMemory");
  ST_VKO_DEVICE_FN(MapMemory, "vkMapMemory");
  ST_VKO_DEVICE_FN(UnmapMemory, "vkUnmapMemory");
  ST_VKO_DEVICE_FN(CmdCopyBufferToImage, "vkCmdCopyBufferToImage");
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

// A Buffer A..D render target: two images, ping-ponged.
//
// A Shadertoy buffer may sample its own previous frame, so a single image
// cannot serve as both this frame's attachment and this frame's input. Every
// pass reads the front (last frame's result) and writes the back; the pair
// swaps once at end of frame. This is the same arrangement GlRenderer uses.
struct BufferVk {
  std::array<VkImage, 2> image{};
  std::array<VkDeviceMemory, 2> memory{};
  std::array<VkImageView, 2> view{};
  std::array<VkFramebuffer, 2> fb{};
  uint32_t width = 0;
  uint32_t height = 0;
  bool used = false;
};

// One pass of the program: its own pipeline (each pass is a different shader)
// and one descriptor set per ping-pong parity.
//
// Two sets rather than one rewritten per frame: a set records into the
// caller's command buffer, so rewriting it would race a frame still in flight.
// Parity is global -- every buffer swaps together -- so two sets cover it, on
// the same "no more than one frame in flight against these buffers" assumption
// the ping-pong itself rests on.
struct PassVk {
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkShaderModule frag = VK_NULL_HANDLE;
  std::array<VkDescriptorSet, 2> set{};
  std::array<Channel, kChannelCount> channels{};
  int target_buffer = -1;  // -1: the Image pass, drawn into the caller's target
};

// A decoded texture channel, cached by resolved path so several passes (or
// several channels) sharing an image decode and upload it once.
struct TextureVk {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  uint32_t width = 0;
  uint32_t height = 0;
};

// Channel storage kinds, matching the sampler a pass declares. A descriptor's
// image view type has to agree with the shader's declaration, so a cubemap
// channel binds a cube view even when its faces failed to load -- hence one
// stub per kind rather than a single 2D one.
enum class ChannelDim { k2D, kCube, k3D };

// The six faces Shadertoy implies: face 0 is the src, faces 1..5 are
// "<stem>_<i><ext>" beside it. Same convention GlRenderer uses.
constexpr uint32_t kCubeFaces = 6;

// Shadertoy publishes no volume format, so a volume channel is a deterministic
// noise block. 32^3 matches GlRenderer, and seeding from the channel src keeps
// it stable across frames and runs.
constexpr uint32_t kVolumeSize = 32;

struct VkOffscreenRenderer::Impl {
  VkExternalDevice dev{};
  VkOffscreenConfig cfg{};
  VkOffscreenApi api{};

  VkRenderPass render_pass = VK_NULL_HANDLE;
  // Buffer passes end in SHADER_READ_ONLY_OPTIMAL for the next pass to sample,
  // where the target's pass ends in the caller's requested final_layout, so the
  // two cannot share a VkRenderPass.
  VkRenderPass buffer_render_pass = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule vert_module = VK_NULL_HANDLE;

  // Buffer passes in ascending order, Image pass last. Empty until SetProgram.
  std::vector<PassVk> passes;
  std::array<BufferVk, kNumBuffers> buffers{};
  // Which half of every ping-pong pair currently holds the previous frame.
  uint32_t front = 0;
  // Resolved once against the physical device; VK_FORMAT_UNDEFINED until then.
  VkFormat buffer_format = VK_FORMAT_UNDEFINED;
  // Descriptor sets are allocated by BuildProgram but can only be filled in
  // once the buffers they name exist, which needs a target size. Cleared
  // whenever the sets or the views they point at are replaced.
  bool sets_written = false;
  // Freshly created buffer images are UNDEFINED, but the first frame samples
  // the front half before any pass has written it. Recorded into the caller's
  // command buffer on the next frame, so no extra queue submission is needed.
  bool buffers_need_layout_init = false;
  // Kept so a target resize can rebuild the buffers and rewrite the sets
  // without the host having to call SetProgram again.
  ShaderProgram program{};

  // 1x1 black stubs, bound to any channel this renderer cannot supply. One per
  // sampler kind: a descriptor's view type must match the shader's
  // declaration, so a samplerCube channel cannot fall back to a 2D image.
  VkImage stub_image = VK_NULL_HANDLE;
  VkDeviceMemory stub_memory = VK_NULL_HANDLE;
  VkImageView stub_view = VK_NULL_HANDLE;
  TextureVk stub_cube{};
  TextureVk stub_volume{};

  // Audio channel: a 512x2 R8 image the capture source refills every frame.
  // Unlike every other channel this is not uploaded once -- so the copy is
  // recorded into the caller's command buffer rather than submitted here,
  // preserving "RecordRender submits nothing".
  TextureVk audio_image{};
  VkBuffer audio_staging = VK_NULL_HANDLE;
  VkDeviceMemory audio_staging_memory = VK_NULL_HANDLE;
  void* audio_mapped = nullptr;
  std::shared_ptr<AudioSource> audio;
  bool audio_enabled = true;
  bool audio_custom_set = false;  // a source came from SetAudioSource
  bool audio_started = false;
  bool audio_failed = false;  // latched: a failed open is not retried
  bool uses_audio = false;    // the current program binds a kAudio channel
  int64_t audio_frame = -1;   // guards one refill per frame
  // Disabling audio has to push one frame of silence: simply ceasing to refill
  // would leave the last capture frozen in the image, which reads as audio
  // that stopped responding rather than audio that is off.
  bool audio_silence_pending = false;
  VkSampler sampler = VK_NULL_HANDLE;
  // Samplers keyed by the Channel's filter/wrap, built on demand. Shadertoy
  // sets these per channel, and a nearest-filtered lookup table sampled
  // linearly is a visibly different shader.
  std::map<std::pair<int, int>, VkSampler> samplers;
  // Decoded texture channels, keyed by resolved path.
  std::map<std::string, TextureVk> textures;
  std::string media_dir;
  // Diagnostics from the most recent BuildProgram; empty on success.
  std::string compile_log;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;

  VkCommandPool setup_pool = VK_NULL_HANDLE;

  // Framebuffers are keyed by image view: a host cycling a ring of N images
  // presents the same handful of views over and over, so building one per
  // frame would be pure waste.
  std::unordered_map<VkImageView, VkFramebuffer> framebuffers;

  ~Impl() { Cleanup(); }

  /// Record and submit the staging copy for a texture channel, leaving the
  /// image in SHADER_READ_ONLY_OPTIMAL. Submits, like the stub texture does --
  /// at SetProgram time, never per frame.
  [[nodiscard]] bool UploadChannelImage(VkImage image,
                                        VkBuffer staging,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t depth,
                                        uint32_t layers);
  [[nodiscard]] VkSampler SamplerFor(const Channel& ch);
  /// Decode and upload @p ch's image, or return nullptr when it cannot be
  /// loaded (the caller then binds the stub, as before).
  [[nodiscard]] const TextureVk* TextureFor(const Channel& ch);
  /// Load a cubemap channel's six faces, or return nullptr (caller binds the
  /// cube stub).
  [[nodiscard]] const TextureVk* CubemapFor(const Channel& ch);
  /// Synthesise a volume channel's noise block; keyed on the channel src.
  [[nodiscard]] const TextureVk* VolumeFor(const Channel& ch);
  /// Create an image + view of @p dim and upload @p pixels (one contiguous
  /// block covering every layer or slice).
  [[nodiscard]] bool CreateChannelImage(ChannelDim dim,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t depth_or_layers,
                                        const void* pixels,
                                        TextureVk* out);
  /// The stub matching @p dim, built on first use.
  [[nodiscard]] VkImageView StubViewFor(ChannelDim dim);
  /// Create the audio image and its persistent mapped staging buffer. Called
  /// on first use, so a program without an audio channel allocates neither.
  [[nodiscard]] bool EnsureAudioImage();
  /// Pull a frame of capture into the staging buffer and record the copy into
  /// @p cmd. No-op when the program binds no audio channel, when capture is
  /// disabled, or when this frame already refilled it.
  void RecordAudioUpdate(VkCommandBuffer cmd, const ShaderInputs& inputs);
  /// Record the staging-buffer-to-image copy and its barriers into @p cmd.
  void RecordAudioCopy(VkCommandBuffer cmd);
  [[nodiscard]] bool SelectBufferFormat();
  [[nodiscard]] bool CreateRenderPass();
  [[nodiscard]] bool CreateStubTexture();
  [[nodiscard]] bool CreateDescriptors();
  [[nodiscard]] bool BuildProgram(const ShaderProgram& src);
  [[nodiscard]] bool EnsureBuffers(uint32_t width, uint32_t height);
  void WriteDescriptorSets();
  void DestroyBuffers();
  void DestroyPasses();
  [[nodiscard]] bool MakePipeline(const std::string& frag_glsl,
                                  VkRenderPass pass,
                                  VkPipeline* out_pipeline,
                                  VkShaderModule* out_frag,
                                  std::string* log = nullptr);
  [[nodiscard]] VkFramebuffer FramebufferFor(const VkOffscreenTarget& target);
  void Cleanup();

  [[nodiscard]] bool FindMemoryType(uint32_t type_bits,
                                    VkMemoryPropertyFlags props,
                                    uint32_t* out_index) const;
  [[nodiscard]] VkShaderModule MakeModule(const std::vector<uint32_t>& spirv);
};

// ── Render pass
// ───────────────────────────────────────────────────────────────

namespace {

// Build the one-subpass color render pass both the target and the buffer
// attachments use. They differ only in format and in the layouts they start and
// end in, so the dependency reasoning below is written once.
[[nodiscard]] bool MakeColorRenderPass(const VkOffscreenApi& api,
                                       VkDevice device,
                                       VkFormat format,
                                       VkImageLayout initial_layout,
                                       VkImageLayout final_layout,
                                       VkRenderPass* out) {
  VkAttachmentDescription color{};
  color.format = format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  // The Image pass covers every pixel, so the previous contents are never read.
  // CLEAR rather than DONT_CARE all the same: a shader that writes alpha < 1,
  // or a driver that would otherwise hand back uninitialized memory, should not
  // leak whatever the ring slot held two frames ago.
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = initial_layout;
  color.finalLayout = final_layout;

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

  ST_VKO_CHECK(api.CreateRenderPass(device, &info, nullptr, out),
               "vkCreateRenderPass failed");
  return true;
}

}  // namespace

bool VkOffscreenRenderer::Impl::SelectBufferFormat() {
  for (const VkFormat candidate : kBufferFormatCandidates) {
    VkFormatProperties props{};
    api.GetPhysicalDeviceFormatProperties(dev.physical_device, candidate,
                                          &props);
    if ((props.optimalTilingFeatures & kBufferFormatFeatures) !=
        kBufferFormatFeatures) {
      continue;
    }
    buffer_format = candidate;
    // Say so when the choice is not the half-float one. A UNORM buffer clamps
    // to [0,1], which does not fail -- it quietly changes what an accumulating
    // shader computes, and that reads as a bug in the shader.
    if (candidate != kBufferFormatCandidates.front()) {
      std::fprintf(stderr,
                   "shadertoy: multi-pass buffers fall back to a clamped "
                   "format (VkFormat %d); shaders that accumulate outside "
                   "[0,1] will differ from Shadertoy\n",
                   static_cast<int>(candidate));
    }
    return true;
  }
  std::fprintf(stderr,
               "shadertoy: no buffer format on this device is renderable, "
               "samplable and linearly filterable; multi-pass is unavailable "
               "(see examples/vk_format_probe)\n");
  return false;
}

bool VkOffscreenRenderer::Impl::CreateRenderPass() {
  if (!MakeColorRenderPass(api, dev.device, cfg.color_format,
                           cfg.initial_layout, cfg.final_layout,
                           &render_pass)) {
    return false;
  }
  // Buffer attachments are this renderer's own, so it dictates both ends:
  // UNDEFINED in (the previous contents are the frame before last, which the
  // pass overwrites) and SHADER_READ_ONLY out, ready for the next pass to
  // sample without a separate barrier.
  return MakeColorRenderPass(
      api, dev.device, buffer_format, VK_IMAGE_LAYOUT_UNDEFINED,
      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, &buffer_render_pass);
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

// ── Audio channel
// ────────────────────────────────────────────────────────────

bool VkOffscreenRenderer::Impl::EnsureAudioImage() {
  if (audio_image.view != VK_NULL_HANDLE) {
    return true;
  }
  constexpr uint32_t kW = static_cast<uint32_t>(kAudioTexWidth);
  constexpr uint32_t kH = 2;

  // R8: the capture fills one byte per texel -- row 0 the FFT, row 1 the
  // waveform -- and the shader reads .x, matching the GL renderer's GL_R8.
  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8_UNORM;
  ici.extent = {kW, kH, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  ST_VKO_CHECK(api.CreateImage(dev.device, &ici, nullptr, &audio_image.image),
               "vkCreateImage(audio) failed");

  VkMemoryRequirements req{};
  api.GetImageMemoryRequirements(dev.device, audio_image.image, &req);
  uint32_t type_index = 0;
  if (!FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      &type_index)) {
    return false;
  }
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type_index;
  ST_VKO_CHECK(
      api.AllocateMemory(dev.device, &mai, nullptr, &audio_image.memory),
      "vkAllocateMemory(audio) failed");
  ST_VKO_CHECK(
      api.BindImageMemory(dev.device, audio_image.image, audio_image.memory, 0),
      "vkBindImageMemory(audio) failed");

  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = audio_image.image;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = VK_FORMAT_R8_UNORM;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  ST_VKO_CHECK(
      api.CreateImageView(dev.device, &vci, nullptr, &audio_image.view),
      "vkCreateImageView(audio) failed");
  audio_image.width = kW;
  audio_image.height = kH;

  // The staging buffer is persistent and stays mapped: this is refilled every
  // frame, so mapping and unmapping per frame would be pure overhead.
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = static_cast<VkDeviceSize>(kAudioTexBytes);
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ST_VKO_CHECK(api.CreateBuffer(dev.device, &bci, nullptr, &audio_staging),
               "vkCreateBuffer(audio) failed");
  VkMemoryRequirements breq{};
  api.GetBufferMemoryRequirements(dev.device, audio_staging, &breq);
  if (!FindMemoryType(breq.memoryTypeBits,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &type_index)) {
    return false;
  }
  mai.allocationSize = breq.size;
  mai.memoryTypeIndex = type_index;
  ST_VKO_CHECK(
      api.AllocateMemory(dev.device, &mai, nullptr, &audio_staging_memory),
      "vkAllocateMemory(audio staging) failed");
  ST_VKO_CHECK(
      api.BindBufferMemory(dev.device, audio_staging, audio_staging_memory, 0),
      "vkBindBufferMemory(audio) failed");
  ST_VKO_CHECK(api.MapMemory(dev.device, audio_staging_memory, 0,
                             static_cast<VkDeviceSize>(kAudioTexBytes), 0,
                             &audio_mapped),
               "vkMapMemory(audio) failed");
  // Silent until the first capture lands, so a frame recorded before any audio
  // arrives samples zeros rather than uninitialized memory.
  std::memset(audio_mapped, 0, static_cast<size_t>(kAudioTexBytes));
  return true;
}

void VkOffscreenRenderer::Impl::RecordAudioUpdate(VkCommandBuffer cmd,
                                                  const ShaderInputs& inputs) {
  if (!uses_audio || audio_failed) {
    return;
  }
  if (audio_silence_pending) {
    if (EnsureAudioImage()) {
      std::memset(audio_mapped, 0, static_cast<size_t>(kAudioTexBytes));
      RecordAudioCopy(cmd);
    }
    audio_silence_pending = false;
    return;
  }
  // Lazily open the default microphone the first time audio is actually
  // needed, matching GlRenderer.
  if (audio == nullptr) {
    if (audio_custom_set || !audio_enabled) {
      return;  // explicitly disabled, or a null custom source was set
    }
    audio = MakeMicSource();
    if (audio == nullptr) {
      audio_failed = true;  // no capture back-end compiled in
      return;
    }
  }
  // An injected source is started and stopped by its owner; only the lazily
  // created default one is this renderer's to drive.
  if (!audio_custom_set && !audio_started) {
    if (!audio->Start()) {
      audio.reset();
      audio_failed = true;  // no device or permission: do not retry
      return;
    }
    audio_started = true;
  }
  if (inputs.frame == audio_frame) {
    return;  // already refilled this frame; several passes may sample audio
  }
  if (!EnsureAudioImage()) {
    audio_failed = true;
    return;
  }
  audio_frame = inputs.frame;
  if (!audio->Fill(static_cast<unsigned char*>(audio_mapped))) {
    return;  // no new capture this frame; the image keeps the last one
  }

  RecordAudioCopy(cmd);
}

void VkOffscreenRenderer::Impl::RecordAudioCopy(VkCommandBuffer cmd) {
  const auto barrier =
      [&](VkImageLayout old_layout, VkImageLayout new_layout,
          VkAccessFlags src_access, VkPipelineStageFlags src_stage,
          VkAccessFlags dst_access, VkPipelineStageFlags dst_stage) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = old_layout;
        b.newLayout = new_layout;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = audio_image.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = src_access;
        b.dstAccessMask = dst_access;
        api.CmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0,
                               nullptr, 1, &b);
      };

  // UNDEFINED as the source layout: the previous contents are last frame's
  // capture, which this overwrites completely, so there is nothing to preserve
  // and the driver is free to discard.
  barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {audio_image.width, audio_image.height, 1};
  api.CmdCopyBufferToImage(cmd, audio_staging, audio_image.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// ── Texture channels
// ──────────────────────────────────────────────────────────

bool VkOffscreenRenderer::Impl::UploadChannelImage(VkImage image,
                                                   VkBuffer staging,
                                                   const uint32_t width,
                                                   const uint32_t height,
                                                   const uint32_t depth,
                                                   const uint32_t layers) {
  VkCommandBufferAllocateInfo cba{};
  cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cba.commandPool = setup_pool;
  cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cba.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  ST_VKO_CHECK(api.AllocateCommandBuffers(dev.device, &cba, &cmd),
               "vkAllocateCommandBuffers(texture) failed");
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  ST_VKO_CHECK(api.BeginCommandBuffer(cmd, &begin),
               "vkBeginCommandBuffer(texture) failed");

  VkImageMemoryBarrier to_dst{};
  to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.image = image;
  to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
  to_dst.srcAccessMask = 0;
  to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &to_dst);

  // One region covers every layer or slice: the staging buffer holds them
  // contiguously, in the order Vulkan expects (+X,-X,+Y,-Y,+Z,-Z for a cube).
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
  region.imageExtent = {width, height, depth};
  api.CmdCopyBufferToImage(cmd, staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier to_read = to_dst;
  to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  api.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &to_read);

  ST_VKO_CHECK(api.EndCommandBuffer(cmd), "vkEndCommandBuffer(texture) failed");
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  ST_VKO_CHECK(api.QueueSubmit(dev.queue, 1, &submit, VK_NULL_HANDLE),
               "vkQueueSubmit(texture) failed");
  ST_VKO_CHECK(api.QueueWaitIdle(dev.queue), "vkQueueWaitIdle(texture) failed");
  api.FreeCommandBuffers(dev.device, setup_pool, 1, &cmd);
  return true;
}

VkSampler VkOffscreenRenderer::Impl::SamplerFor(const Channel& ch) {
  const auto key =
      std::make_pair(static_cast<int>(ch.filter), static_cast<int>(ch.wrap));
  if (const auto it = samplers.find(key); it != samplers.end()) {
    return it->second;
  }
  // kMipmap is treated as linear: these images are uploaded with a single
  // level, so asking for mipmapped minification would sample a level that does
  // not exist. Generating the chain is a later change, not a silent one.
  const VkFilter filter =
      ch.filter == Filter::kNearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
  const VkSamplerAddressMode mode = ch.wrap == Wrap::kClamp
                                        ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                                        : VK_SAMPLER_ADDRESS_MODE_REPEAT;
  VkSamplerCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  info.magFilter = filter;
  info.minFilter = filter;
  info.addressModeU = mode;
  info.addressModeV = mode;
  info.addressModeW = mode;
  info.maxLod = VK_LOD_CLAMP_NONE;
  VkSampler out = VK_NULL_HANDLE;
  if (api.CreateSampler(dev.device, &info, nullptr, &out) != VK_SUCCESS) {
    return sampler;  // fall back to the default rather than binding nothing
  }
  samplers.emplace(key, out);
  return out;
}

bool VkOffscreenRenderer::Impl::CreateChannelImage(
    const ChannelDim dim,
    const uint32_t width,
    const uint32_t height,
    const uint32_t depth_or_layers,
    const void* pixels,
    TextureVk* out) {
  const bool is_cube = dim == ChannelDim::kCube;
  const bool is_3d = dim == ChannelDim::k3D;
  const uint32_t layers = is_cube ? kCubeFaces : 1;
  const uint32_t depth = is_3d ? depth_or_layers : 1;
  const VkDeviceSize bytes =
      static_cast<VkDeviceSize>(width) * height * depth * layers * 4;

  out->width = width;
  out->height = height;

  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  bool ok = true;
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ok = api.CreateBuffer(dev.device, &bci, nullptr, &staging) == VK_SUCCESS;
  if (ok) {
    VkMemoryRequirements req{};
    api.GetBufferMemoryRequirements(dev.device, staging, &req);
    uint32_t type_index = 0;
    ok = FindMemoryType(req.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &type_index);
    if (ok) {
      VkMemoryAllocateInfo mai{};
      mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      mai.allocationSize = req.size;
      mai.memoryTypeIndex = type_index;
      ok = api.AllocateMemory(dev.device, &mai, nullptr, &staging_mem) ==
           VK_SUCCESS;
    }
    if (ok) {
      ok = api.BindBufferMemory(dev.device, staging, staging_mem, 0) ==
           VK_SUCCESS;
    }
    if (ok) {
      void* mapped = nullptr;
      ok = api.MapMemory(dev.device, staging_mem, 0, bytes, 0, &mapped) ==
           VK_SUCCESS;
      if (ok) {
        std::memcpy(mapped, pixels, static_cast<size_t>(bytes));
        api.UnmapMemory(dev.device, staging_mem);
      }
    }
  }

  if (ok) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    // A cube view needs its image created cube-compatible; the flag cannot be
    // added at view time.
    ici.flags = is_cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {width, height, depth};
    ici.mipLevels = 1;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ok = api.CreateImage(dev.device, &ici, nullptr, &out->image) == VK_SUCCESS;
  }
  if (ok) {
    VkMemoryRequirements req{};
    api.GetImageMemoryRequirements(dev.device, out->image, &req);
    uint32_t type_index = 0;
    ok = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        &type_index);
    if (ok) {
      VkMemoryAllocateInfo mai{};
      mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      mai.allocationSize = req.size;
      mai.memoryTypeIndex = type_index;
      ok = api.AllocateMemory(dev.device, &mai, nullptr, &out->memory) ==
           VK_SUCCESS;
    }
    if (ok) {
      ok = api.BindImageMemory(dev.device, out->image, out->memory, 0) ==
           VK_SUCCESS;
    }
  }
  if (ok) {
    ok = UploadChannelImage(out->image, staging, width, height, depth, layers);
  }
  if (ok) {
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = out->image;
    vci.viewType = is_cube ? VK_IMAGE_VIEW_TYPE_CUBE
                   : is_3d ? VK_IMAGE_VIEW_TYPE_3D
                           : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    ok = api.CreateImageView(dev.device, &vci, nullptr, &out->view) ==
         VK_SUCCESS;
  }

  if (staging != VK_NULL_HANDLE) {
    api.DestroyBuffer(dev.device, staging, nullptr);
  }
  if (staging_mem != VK_NULL_HANDLE) {
    api.FreeMemory(dev.device, staging_mem, nullptr);
  }
  if (!ok) {
    if (out->view != VK_NULL_HANDLE) {
      api.DestroyImageView(dev.device, out->view, nullptr);
    }
    if (out->image != VK_NULL_HANDLE) {
      api.DestroyImage(dev.device, out->image, nullptr);
    }
    if (out->memory != VK_NULL_HANDLE) {
      api.FreeMemory(dev.device, out->memory, nullptr);
    }
    *out = TextureVk{};
  }
  return ok;
}

VkImageView VkOffscreenRenderer::Impl::StubViewFor(const ChannelDim dim) {
  if (dim == ChannelDim::k2D) {
    return stub_view;
  }
  TextureVk& stub = dim == ChannelDim::kCube ? stub_cube : stub_volume;
  if (stub.view != VK_NULL_HANDLE) {
    return stub.view;
  }
  // 1x1 (or 1^3) opaque black, matching the 2D stub, so an unbound channel of
  // any kind reads the same as it always has.
  const std::array<uint8_t, 4 * kCubeFaces> black{};
  const uint32_t layers = dim == ChannelDim::kCube ? kCubeFaces : 1;
  if (!CreateChannelImage(dim, 1, 1, dim == ChannelDim::k3D ? 1 : layers,
                          black.data(), &stub)) {
    std::fprintf(stderr,
                 "shadertoy: could not create the %s channel stub; that "
                 "channel kind will not bind\n",
                 dim == ChannelDim::kCube ? "cubemap" : "volume");
    return VK_NULL_HANDLE;
  }
  return stub.view;
}

const TextureVk* VkOffscreenRenderer::Impl::CubemapFor(const Channel& ch) {
  if (ch.texture_path.empty()) {
    return nullptr;
  }
  const std::string key = "cube:" + ch.texture_path;
  if (const auto it = textures.find(key); it != textures.end()) {
    return &it->second;
  }

  const std::filesystem::path base(
      ResolveMediaPath(ch.texture_path, media_dir));
  std::array<std::filesystem::path, kCubeFaces> faces;
  faces[0] = base;
  for (uint32_t i = 1; i < kCubeFaces; ++i) {
    faces[i] =
        base.parent_path() / (base.stem().string() + "_" + std::to_string(i) +
                              base.extension().string());
  }

  // Cube faces are not flipped, unlike 2D textures: the cube sampling
  // convention already accounts for orientation, and flipping would mirror
  // every face.
  stbi_set_flip_vertically_on_load(0);
  std::vector<uint8_t> pixels;
  uint32_t face_w = 0;
  uint32_t face_h = 0;
  bool ok = true;
  for (uint32_t i = 0; i < kCubeFaces && ok; ++i) {
    int w = 0;
    int h = 0;
    int comp = 0;
    stbi_uc* px = stbi_load(faces[i].string().c_str(), &w, &h, &comp, 4);
    if (px == nullptr || w <= 0 || h <= 0) {
      if (px != nullptr) {
        stbi_image_free(px);
      }
      std::fprintf(stderr,
                   "shadertoy: cubemap channel '%s': face %u ('%s') could not "
                   "be loaded; sampling the black stub instead\n",
                   ch.texture_path.c_str(), i, faces[i].string().c_str());
      ok = false;
      break;
    }
    // Every face must match: a cube image is one allocation with six layers.
    if (i == 0) {
      face_w = static_cast<uint32_t>(w);
      face_h = static_cast<uint32_t>(h);
      pixels.resize(static_cast<size_t>(face_w) * face_h * 4 * kCubeFaces);
    } else if (static_cast<uint32_t>(w) != face_w ||
               static_cast<uint32_t>(h) != face_h) {
      std::fprintf(stderr,
                   "shadertoy: cubemap channel '%s': face %u is %dx%d but face "
                   "0 is %ux%u; sampling the black stub instead\n",
                   ch.texture_path.c_str(), i, w, h, face_w, face_h);
      stbi_image_free(px);
      ok = false;
      break;
    }
    std::memcpy(pixels.data() + static_cast<size_t>(i) * face_w * face_h * 4,
                px, static_cast<size_t>(face_w) * face_h * 4);
    stbi_image_free(px);
  }
  if (!ok) {
    return nullptr;
  }

  TextureVk tex{};
  if (!CreateChannelImage(ChannelDim::kCube, face_w, face_h, kCubeFaces,
                          pixels.data(), &tex)) {
    std::fprintf(stderr, "shadertoy: failed to upload cubemap channel '%s'\n",
                 ch.texture_path.c_str());
    return nullptr;
  }
  const auto [it, inserted] = textures.emplace(key, tex);
  return &it->second;
}

const TextureVk* VkOffscreenRenderer::Impl::VolumeFor(const Channel& ch) {
  const std::string key =
      "vol:" +
      (ch.texture_path.empty() ? std::string("noise") : ch.texture_path);
  if (const auto it = textures.find(key); it != textures.end()) {
    return &it->second;
  }
  // Deterministic, and seeded the same way GlRenderer seeds it, so a shader
  // looks the same on both back-ends rather than merely non-empty on each.
  std::vector<uint8_t> voxels(static_cast<size_t>(kVolumeSize) * kVolumeSize *
                              kVolumeSize * 4);
  std::mt19937_64 rng(std::hash<std::string>{}(key));
  std::uniform_int_distribution<int> dist(0, 255);
  for (auto& v : voxels) {
    v = static_cast<uint8_t>(dist(rng));
  }
  TextureVk tex{};
  if (!CreateChannelImage(ChannelDim::k3D, kVolumeSize, kVolumeSize,
                          kVolumeSize, voxels.data(), &tex)) {
    std::fprintf(stderr, "shadertoy: failed to create the volume channel\n");
    return nullptr;
  }
  const auto [it, inserted] = textures.emplace(key, tex);
  return &it->second;
}

const TextureVk* VkOffscreenRenderer::Impl::TextureFor(const Channel& ch) {
  if (ch.texture_path.empty()) {
    return nullptr;
  }
  const std::string path = ResolveMediaPath(ch.texture_path, media_dir);
  // Keyed by resolved path and vflip: the same file sampled both ways is two
  // different uploads, and Shadertoy defaults textures to flipped.
  const std::string key = path + (ch.vflip ? "#flip" : "");
  if (const auto it = textures.find(key); it != textures.end()) {
    return &it->second;
  }

  stbi_set_flip_vertically_on_load(ch.vflip ? 1 : 0);
  int w = 0;
  int h = 0;
  int comp = 0;
  stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
  if (pixels == nullptr || w <= 0 || h <= 0) {
    if (pixels != nullptr) {
      stbi_image_free(pixels);
    }
    std::fprintf(stderr,
                 "shadertoy: could not load texture channel '%s' (resolved to "
                 "'%s'); sampling the black stub instead\n",
                 ch.texture_path.c_str(), path.c_str());
    return nullptr;
  }
  const auto width = static_cast<uint32_t>(w);
  const auto height = static_cast<uint32_t>(h);
  const VkDeviceSize bytes = static_cast<VkDeviceSize>(width) * height * 4;

  TextureVk tex{};
  tex.width = width;
  tex.height = height;

  // Staging buffer -> device-local image. This submits, like the stub texture
  // does; it happens when a program is set, never per frame.
  VkBuffer staging = VK_NULL_HANDLE;
  VkDeviceMemory staging_mem = VK_NULL_HANDLE;
  bool ok = true;
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ok = api.CreateBuffer(dev.device, &bci, nullptr, &staging) == VK_SUCCESS;
  if (ok) {
    VkMemoryRequirements req{};
    api.GetBufferMemoryRequirements(dev.device, staging, &req);
    uint32_t type_index = 0;
    ok = FindMemoryType(req.memoryTypeBits,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        &type_index);
    if (ok) {
      VkMemoryAllocateInfo mai{};
      mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      mai.allocationSize = req.size;
      mai.memoryTypeIndex = type_index;
      ok = api.AllocateMemory(dev.device, &mai, nullptr, &staging_mem) ==
           VK_SUCCESS;
    }
    if (ok) {
      ok = api.BindBufferMemory(dev.device, staging, staging_mem, 0) ==
           VK_SUCCESS;
    }
    if (ok) {
      void* mapped = nullptr;
      ok = api.MapMemory(dev.device, staging_mem, 0, bytes, 0, &mapped) ==
           VK_SUCCESS;
      if (ok) {
        std::memcpy(mapped, pixels, static_cast<size_t>(bytes));
        api.UnmapMemory(dev.device, staging_mem);
      }
    }
  }
  stbi_image_free(pixels);

  if (ok) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ok = api.CreateImage(dev.device, &ici, nullptr, &tex.image) == VK_SUCCESS;
  }
  if (ok) {
    VkMemoryRequirements req{};
    api.GetImageMemoryRequirements(dev.device, tex.image, &req);
    uint32_t type_index = 0;
    ok = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                        &type_index);
    if (ok) {
      VkMemoryAllocateInfo mai{};
      mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      mai.allocationSize = req.size;
      mai.memoryTypeIndex = type_index;
      ok = api.AllocateMemory(dev.device, &mai, nullptr, &tex.memory) ==
           VK_SUCCESS;
    }
    if (ok) {
      ok = api.BindImageMemory(dev.device, tex.image, tex.memory, 0) ==
           VK_SUCCESS;
    }
  }
  if (ok) {
    ok = UploadChannelImage(tex.image, staging, width, height, 1, 1);
  }
  if (ok) {
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = tex.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ok =
        api.CreateImageView(dev.device, &vci, nullptr, &tex.view) == VK_SUCCESS;
  }

  if (staging != VK_NULL_HANDLE) {
    api.DestroyBuffer(dev.device, staging, nullptr);
  }
  if (staging_mem != VK_NULL_HANDLE) {
    api.FreeMemory(dev.device, staging_mem, nullptr);
  }
  if (!ok) {
    if (tex.view != VK_NULL_HANDLE) {
      api.DestroyImageView(dev.device, tex.view, nullptr);
    }
    if (tex.image != VK_NULL_HANDLE) {
      api.DestroyImage(dev.device, tex.image, nullptr);
    }
    if (tex.memory != VK_NULL_HANDLE) {
      api.FreeMemory(dev.device, tex.memory, nullptr);
    }
    std::fprintf(stderr,
                 "shadertoy: failed to upload texture channel '%s'; sampling "
                 "the black stub instead\n",
                 ch.texture_path.c_str());
    return nullptr;
  }
  const auto [it, inserted] = textures.emplace(key, tex);
  return &it->second;
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

  // The pool is sized to the program, so it is created in BuildProgram rather
  // than here: a single-pass program needs two sets, a five-pass one ten.
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

bool VkOffscreenRenderer::Impl::MakePipeline(const std::string& frag_glsl,
                                             VkRenderPass pass,
                                             VkPipeline* out_pipeline,
                                             VkShaderModule* out_frag,
                                             std::string* log) {
  // The vertex stage is the same generated full-screen triangle for every pass,
  // so it is compiled once and shared rather than per pass.
  if (vert_module == VK_NULL_HANDLE) {
    const std::vector<uint32_t> vert_spirv =
        CompileToSpirv(kVertexShader, ShaderStage::kVertex);
    vert_module = MakeModule(vert_spirv);
    if (vert_module == VK_NULL_HANDLE) {
      std::fprintf(stderr, "shadertoy: vertex shader compilation failed\n");
      return false;
    }
  }
  const std::vector<uint32_t> frag_spirv =
      CompileToSpirv(frag_glsl, ShaderStage::kFragment, log);
  if (frag_spirv.empty()) {
    std::fprintf(stderr, "shadertoy: GLSL to SPIR-V compilation failed\n");
    return false;
  }

  // Built into locals and handed back only on success, so a failed SetProgram
  // leaves the previous program running.
  VkShaderModule new_vert = vert_module;
  VkShaderModule new_frag = MakeModule(frag_spirv);
  if (new_frag == VK_NULL_HANDLE) {
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
  info.renderPass = pass;
  info.subpass = 0;

  VkPipeline new_pipeline = VK_NULL_HANDLE;
  const VkResult result = api.CreateGraphicsPipelines(
      dev.device, VK_NULL_HANDLE, 1, &info, nullptr, &new_pipeline);
  if (result != VK_SUCCESS) {
    api.DestroyShaderModule(dev.device, new_frag, nullptr);
    std::fprintf(stderr, "shadertoy: vkCreateGraphicsPipelines (VkResult %d)\n",
                 static_cast<int>(result));
    return false;
  }

  // Hand both back; the caller owns them and publishes only once every pass of
  // the program has been built.
  (void)new_vert;
  *out_pipeline = new_pipeline;
  *out_frag = new_frag;
  return true;
}

// ── Buffer passes (Buffer A..D)
// ────────────────────────────────────────────────

void VkOffscreenRenderer::Impl::DestroyBuffers() {
  for (BufferVk& b : buffers) {
    for (size_t i = 0; i < 2; ++i) {
      if (b.fb[i] != VK_NULL_HANDLE) {
        api.DestroyFramebuffer(dev.device, b.fb[i], nullptr);
      }
      if (b.view[i] != VK_NULL_HANDLE) {
        api.DestroyImageView(dev.device, b.view[i], nullptr);
      }
      if (b.image[i] != VK_NULL_HANDLE) {
        api.DestroyImage(dev.device, b.image[i], nullptr);
      }
      if (b.memory[i] != VK_NULL_HANDLE) {
        api.FreeMemory(dev.device, b.memory[i], nullptr);
      }
    }
    b = BufferVk{};
  }
}

bool VkOffscreenRenderer::Impl::EnsureBuffers(const uint32_t width,
                                              const uint32_t height) {
  // Buffers are the size of the target, so a resize rebuilds them. Nothing here
  // submits: the images are created UNDEFINED and the first render pass that
  // writes one performs the transition, which keeps this renderer's "only one
  // queue submission, ever" property intact.
  bool need_rebuild = false;
  for (int i = 0; i < kNumBuffers; ++i) {
    const BufferVk& b = buffers[static_cast<size_t>(i)];
    const bool wanted = program.uses_buffer(i);
    if (wanted != b.used ||
        (wanted && (b.width != width || b.height != height))) {
      need_rebuild = true;
      break;
    }
  }
  if (!need_rebuild) {
    // A new program reuses buffers of the same size and shape, so nothing is
    // rebuilt -- but its sets are freshly allocated and still empty. Binding
    // one unwritten is undefined, and it would go unnoticed until some shader
    // actually sampled a channel.
    if (!sets_written) {
      WriteDescriptorSets();
      sets_written = true;
    }
    return true;
  }
  // The host must not have frames in flight across a resize; it owns submission
  // and so owns that guarantee, exactly as it does for SetProgram.
  DestroyBuffers();

  for (int i = 0; i < kNumBuffers; ++i) {
    if (!program.uses_buffer(i)) {
      continue;
    }
    BufferVk& b = buffers[static_cast<size_t>(i)];
    b.used = true;
    b.width = width;
    b.height = height;
    for (size_t half = 0; half < 2; ++half) {
      VkImageCreateInfo image{};
      image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
      image.imageType = VK_IMAGE_TYPE_2D;
      image.format = buffer_format;
      image.extent = {width, height, 1};
      image.mipLevels = 1;
      image.arrayLayers = 1;
      image.samples = VK_SAMPLE_COUNT_1_BIT;
      image.tiling = VK_IMAGE_TILING_OPTIMAL;
      image.usage =
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
      image.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      ST_VKO_CHECK(api.CreateImage(dev.device, &image, nullptr, &b.image[half]),
                   "vkCreateImage(buffer) failed");

      VkMemoryRequirements req{};
      api.GetImageMemoryRequirements(dev.device, b.image[half], &req);
      uint32_t type_index = 0;
      if (!FindMemoryType(req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index)) {
        return false;
      }
      VkMemoryAllocateInfo alloc{};
      alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      alloc.allocationSize = req.size;
      alloc.memoryTypeIndex = type_index;
      ST_VKO_CHECK(
          api.AllocateMemory(dev.device, &alloc, nullptr, &b.memory[half]),
          "vkAllocateMemory(buffer) failed");
      ST_VKO_CHECK(
          api.BindImageMemory(dev.device, b.image[half], b.memory[half], 0),
          "vkBindImageMemory(buffer) failed");

      VkImageViewCreateInfo view{};
      view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      view.image = b.image[half];
      view.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view.format = buffer_format;
      view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      ST_VKO_CHECK(
          api.CreateImageView(dev.device, &view, nullptr, &b.view[half]),
          "vkCreateImageView(buffer) failed");

      VkFramebufferCreateInfo fb{};
      fb.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      fb.renderPass = buffer_render_pass;
      fb.attachmentCount = 1;
      fb.pAttachments = &b.view[half];
      fb.width = width;
      fb.height = height;
      fb.layers = 1;
      ST_VKO_CHECK(api.CreateFramebuffer(dev.device, &fb, nullptr, &b.fb[half]),
                   "vkCreateFramebuffer(buffer) failed");
    }
  }
  // The sets name buffer views, which have just been replaced.
  WriteDescriptorSets();
  sets_written = true;
  buffers_need_layout_init = true;
  return true;
}

void VkOffscreenRenderer::Impl::WriteDescriptorSets() {
  // One set per pass per ping-pong parity. Parity p means "front == p", so a
  // kBuffer channel in that set points at the half holding the previous frame.
  // Everything this renderer cannot supply -- textures, cubemaps, audio,
  // keyboard -- resolves to the 1x1 black stub, as it did before multi-pass.
  for (const PassVk& pass : passes) {
    for (uint32_t parity = 0; parity < 2; ++parity) {
      if (pass.set[parity] == VK_NULL_HANDLE) {
        continue;
      }
      std::array<VkDescriptorImageInfo, kChannelCount> infos{};
      std::array<VkWriteDescriptorSet, kChannelCount> writes{};
      for (uint32_t i = 0; i < kChannelCount; ++i) {
        const Channel& ch = pass.channels[i];
        VkImageView view = stub_view;
        VkSampler chan_sampler = sampler;
        if (ch.kind == ChannelKind::kBuffer && ch.buffer >= 0 &&
            ch.buffer < kNumBuffers) {
          const BufferVk& src = buffers[static_cast<size_t>(ch.buffer)];
          if (src.used && src.view[parity] != VK_NULL_HANDLE) {
            view = src.view[parity];
          }
        } else if (ch.kind == ChannelKind::kTexture) {
          // Falls through to the stub when the file cannot be found or
          // decoded, which TextureFor has already reported.
          if (const TextureVk* tex = TextureFor(ch); tex != nullptr) {
            view = tex->view;
            chan_sampler = SamplerFor(ch);
          }
        } else if (ch.kind == ChannelKind::kCubemap) {
          // The stub has to be a cube too: the pass declares samplerCube for
          // this binding whether or not the faces loaded.
          view = StubViewFor(ChannelDim::kCube);
          if (const TextureVk* tex = CubemapFor(ch); tex != nullptr) {
            view = tex->view;
            chan_sampler = SamplerFor(ch);
          }
        } else if (ch.kind == ChannelKind::kVolume) {
          view = StubViewFor(ChannelDim::k3D);
          if (const TextureVk* tex = VolumeFor(ch); tex != nullptr) {
            view = tex->view;
            chan_sampler = SamplerFor(ch);
          }
        } else if (ch.kind == ChannelKind::kAudio) {
          // Created here rather than at first capture: the descriptor has to
          // name a real view now, and a silent image is the correct reading
          // until a frame of audio lands.
          if (EnsureAudioImage()) {
            view = audio_image.view;
          }
        }
        infos[i].sampler = chan_sampler;
        infos[i].imageView = view;
        infos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = pass.set[parity];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
      }
      api.UpdateDescriptorSets(dev.device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
  }
}

void VkOffscreenRenderer::Impl::DestroyPasses() {
  for (PassVk& pass : passes) {
    if (pass.pipeline != VK_NULL_HANDLE) {
      api.DestroyPipeline(dev.device, pass.pipeline, nullptr);
    }
    if (pass.frag != VK_NULL_HANDLE) {
      api.DestroyShaderModule(dev.device, pass.frag, nullptr);
    }
  }
  passes.clear();
  // Frees every set allocated from it; the sets are not freed individually.
  if (descriptor_pool != VK_NULL_HANDLE) {
    api.DestroyDescriptorPool(dev.device, descriptor_pool, nullptr);
    descriptor_pool = VK_NULL_HANDLE;
  }
}

bool VkOffscreenRenderer::Impl::BuildProgram(const ShaderProgram& src) {
  // Buffer passes first, in Shadertoy order, then Image. A buffer reads the
  // previous frame of every buffer, so within a frame the order only decides
  // which passes see this frame's writes -- and none do, by construction.
  std::vector<PassVk> built;
  std::vector<const Pass*> sources;
  for (int i = 0; i < kNumBuffers; ++i) {
    if (src.uses_buffer(i)) {
      PassVk p{};
      p.target_buffer = i;
      p.channels = src.buffers[static_cast<size_t>(i)].channels;
      built.push_back(p);
      sources.push_back(&src.buffers[static_cast<size_t>(i)]);
    }
  }
  {
    PassVk p{};
    p.target_buffer = -1;
    p.channels = src.image.channels;
    built.push_back(p);
    sources.push_back(&src.image);
  }

  // Compile and build every pipeline before touching live state, so a program
  // that fails to compile leaves the previous one running.
  compile_log.clear();
  // The sampler a channel is declared with and the view type its descriptor
  // binds have to agree, so the declaration is derived from the same kinds the
  // descriptor writes below use.
  const auto dims_for = [](const Pass& pass) {
    std::array<SamplerDim, kChannelCount> dims{};
    for (uint32_t i = 0; i < kChannelCount; ++i) {
      switch (pass.channels[i].kind) {
        case ChannelKind::kCubemap:
          dims[i] = SamplerDim::kCube;
          break;
        case ChannelKind::kVolume:
          dims[i] = SamplerDim::k3D;
          break;
        default:
          dims[i] = SamplerDim::k2D;
          break;
      }
    }
    return dims;
  };

  bool ok = true;
  for (size_t i = 0; i < built.size() && ok; ++i) {
    const std::string frag =
        WrapVulkan(src.common, sources[i]->code, dims_for(*sources[i]));
    VkRenderPass pass =
        built[i].target_buffer < 0 ? render_pass : buffer_render_pass;
    std::string pass_log;
    ok =
        MakePipeline(frag, pass, &built[i].pipeline, &built[i].frag, &pass_log);
    if (!pass_log.empty()) {
      // Name the pass: "it failed" is much less useful than "Buffer B failed"
      // when a program has five of them, and the line numbers in the log are
      // relative to that pass's wrapped source.
      if (built[i].target_buffer < 0) {
        compile_log.append("Image pass:\n");
      } else {
        compile_log.append("Buffer ");
        compile_log.push_back(static_cast<char>('A' + built[i].target_buffer));
        compile_log.append(" pass:\n");
      }
      compile_log.append(pass_log);
    }
  }

  VkDescriptorPool new_pool = VK_NULL_HANDLE;
  if (ok) {
    const auto sets = static_cast<uint32_t>(built.size() * 2);
    VkDescriptorPoolSize size{};
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = kChannelCount * sets;
    VkDescriptorPoolCreateInfo pool{};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = sets;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = &size;
    if (api.CreateDescriptorPool(dev.device, &pool, nullptr, &new_pool) !=
        VK_SUCCESS) {
      std::fprintf(stderr, "shadertoy: vkCreateDescriptorPool failed\n");
      ok = false;
    }
  }
  if (ok) {
    for (PassVk& p : built) {
      const std::array<VkDescriptorSetLayout, 2> layouts{set_layout,
                                                         set_layout};
      VkDescriptorSetAllocateInfo alloc{};
      alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      alloc.descriptorPool = new_pool;
      alloc.descriptorSetCount = 2;
      alloc.pSetLayouts = layouts.data();
      if (api.AllocateDescriptorSets(dev.device, &alloc, p.set.data()) !=
          VK_SUCCESS) {
        std::fprintf(stderr, "shadertoy: vkAllocateDescriptorSets failed\n");
        ok = false;
        break;
      }
    }
  }

  if (!ok) {
    for (PassVk& p : built) {
      if (p.pipeline != VK_NULL_HANDLE) {
        api.DestroyPipeline(dev.device, p.pipeline, nullptr);
      }
      if (p.frag != VK_NULL_HANDLE) {
        api.DestroyShaderModule(dev.device, p.frag, nullptr);
      }
    }
    if (new_pool != VK_NULL_HANDLE) {
      api.DestroyDescriptorPool(dev.device, new_pool, nullptr);
    }
    return false;
  }

  // Publish. The caller must not have work in flight against the old program;
  // the host owns submission and so owns that guarantee.
  DestroyPasses();
  DestroyBuffers();  // sized to the old program's buffer set
  passes = std::move(built);
  descriptor_pool = new_pool;
  program = src;
  // Whether to open a capture device at all is a property of the program, so
  // it is answered here rather than probed per frame.
  uses_audio = false;
  for (const PassVk& p : passes) {
    for (const Channel& ch : p.channels) {
      if (ch.kind == ChannelKind::kAudio) {
        uses_audio = true;
      }
    }
  }
  front = 0;
  sets_written = false;
  // Sets are written once the buffers exist, which RecordRender arranges as
  // soon as it knows the target size. Until then they hold undefined contents
  // and are never bound, because no frame has been recorded.
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

  // Passes own the per-pass pipelines and fragment modules and free the
  // descriptor pool; buffers own their images, memory, views and framebuffers.
  DestroyPasses();
  DestroyBuffers();

  if (buffer_render_pass != VK_NULL_HANDLE) {
    api.DestroyRenderPass(dev.device, buffer_render_pass, nullptr);
    buffer_render_pass = VK_NULL_HANDLE;
  }
  if (pipeline_layout != VK_NULL_HANDLE) {
    api.DestroyPipelineLayout(dev.device, pipeline_layout, nullptr);
    pipeline_layout = VK_NULL_HANDLE;
  }
  if (vert_module != VK_NULL_HANDLE) {
    api.DestroyShaderModule(dev.device, vert_module, nullptr);
    vert_module = VK_NULL_HANDLE;
  }
  if (descriptor_pool != VK_NULL_HANDLE) {
    api.DestroyDescriptorPool(dev.device, descriptor_pool, nullptr);
    descriptor_pool = VK_NULL_HANDLE;
  }
  if (set_layout != VK_NULL_HANDLE) {
    api.DestroyDescriptorSetLayout(dev.device, set_layout, nullptr);
    set_layout = VK_NULL_HANDLE;
  }
  for (auto& [key, tex] : textures) {
    if (tex.view != VK_NULL_HANDLE) {
      api.DestroyImageView(dev.device, tex.view, nullptr);
    }
    if (tex.image != VK_NULL_HANDLE) {
      api.DestroyImage(dev.device, tex.image, nullptr);
    }
    if (tex.memory != VK_NULL_HANDLE) {
      api.FreeMemory(dev.device, tex.memory, nullptr);
    }
  }
  textures.clear();
  if (audio_started && audio != nullptr && !audio_custom_set) {
    audio->Stop();  // only the source this renderer opened is its to close
  }
  if (audio_mapped != nullptr) {
    api.UnmapMemory(dev.device, audio_staging_memory);
    audio_mapped = nullptr;
  }
  if (audio_staging != VK_NULL_HANDLE) {
    api.DestroyBuffer(dev.device, audio_staging, nullptr);
    audio_staging = VK_NULL_HANDLE;
  }
  if (audio_staging_memory != VK_NULL_HANDLE) {
    api.FreeMemory(dev.device, audio_staging_memory, nullptr);
    audio_staging_memory = VK_NULL_HANDLE;
  }
  if (audio_image.view != VK_NULL_HANDLE) {
    api.DestroyImageView(dev.device, audio_image.view, nullptr);
  }
  if (audio_image.image != VK_NULL_HANDLE) {
    api.DestroyImage(dev.device, audio_image.image, nullptr);
  }
  if (audio_image.memory != VK_NULL_HANDLE) {
    api.FreeMemory(dev.device, audio_image.memory, nullptr);
  }
  audio_image = TextureVk{};
  for (TextureVk* stub : {&stub_cube, &stub_volume}) {
    if (stub->view != VK_NULL_HANDLE) {
      api.DestroyImageView(dev.device, stub->view, nullptr);
    }
    if (stub->image != VK_NULL_HANDLE) {
      api.DestroyImage(dev.device, stub->image, nullptr);
    }
    if (stub->memory != VK_NULL_HANDLE) {
      api.FreeMemory(dev.device, stub->memory, nullptr);
    }
    *stub = TextureVk{};
  }
  for (auto& [key, s] : samplers) {
    api.DestroySampler(dev.device, s, nullptr);
  }
  samplers.clear();
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
  // SelectBufferFormat first: CreateRenderPass builds the buffer render pass
  // against the format it picks.
  if (!impl.SelectBufferFormat() || !impl.CreateRenderPass() ||
      !impl.CreateStubTexture() || !impl.CreateDescriptors()) {
    return nullptr;
  }
  return self;
}

bool VkOffscreenRenderer::SetProgram(const ShaderProgram& program) {
  // Channels this renderer cannot supply still resolve to the 1x1 black stub,
  // so every iChannel is declared sampler2D and a shader sampling a cubemap
  // channel will not compile -- unchanged from before multi-pass. What is new
  // is that a kBuffer channel now names a real image.
  return impl_->BuildProgram(program);
}

const std::string& VkOffscreenRenderer::last_compile_log() const {
  return impl_->compile_log;
}

void VkOffscreenRenderer::SetAudioSource(
    std::shared_ptr<AudioSource> src) noexcept {
  // Setting a source -- including a null one -- takes the default microphone
  // out of play, so a caller can disable audio outright by passing null.
  impl_->audio = std::move(src);
  impl_->audio_custom_set = true;
  impl_->audio_started = false;
  impl_->audio_failed = false;
  if (impl_->audio == nullptr) {
    impl_->audio_silence_pending = true;
  }
}

void VkOffscreenRenderer::SetAudioEnabled(const bool enabled) noexcept {
  impl_->audio_enabled = enabled;
  if (!enabled) {
    impl_->audio_silence_pending = true;
  }
}

void VkOffscreenRenderer::SetMediaDir(std::string dir) {
  impl_->media_dir = std::move(dir);
}

bool VkOffscreenRenderer::Init(const std::string& image_shader) {
  return SetProgram(MakeSinglePass(image_shader));
}

bool VkOffscreenRenderer::has_program() const noexcept {
  return !impl_->passes.empty();
}

bool VkOffscreenRenderer::RecordRender(VkCommandBuffer cmd,
                                       const ShaderInputs& inputs,
                                       const VkOffscreenTarget& target) {
  Impl& impl = *impl_;
  if (impl.passes.empty()) {
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
  // Buffers are target-sized; this rebuilds them on the first frame and after a
  // resize, and rewrites the descriptor sets that name their views.
  if (!impl.EnsureBuffers(target.width, target.height)) {
    return false;
  }
  const VkFramebuffer target_fb = impl.FramebufferFor(target);
  if (target_fb == VK_NULL_HANDLE) {
    return false;
  }

  // Move both halves of every buffer into the layout their descriptors claim.
  // Without this the first frame samples an image still in UNDEFINED, which is
  // undefined use even though it happens to read as black on the drivers here.
  if (impl.buffers_need_layout_init) {
    std::vector<VkImageMemoryBarrier> barriers;
    for (const BufferVk& b : impl.buffers) {
      if (!b.used) {
        continue;
      }
      for (size_t half = 0; half < 2; ++half) {
        VkImageMemoryBarrier bar{};
        bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = b.image[half];
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        bar.srcAccessMask = 0;
        bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barriers.push_back(bar);
      }
    }
    if (!barriers.empty()) {
      impl.api.CmdPipelineBarrier(
          cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
          static_cast<uint32_t>(barriers.size()), barriers.data());
    }
    impl.buffers_need_layout_init = false;
  }

  // Before any pass samples it, and in the caller's command buffer so this
  // still submits nothing.
  impl.RecordAudioUpdate(cmd, inputs);

  const PushConstants push = ToPushConstants(inputs);
  const uint32_t parity = impl.front;

  // Every pass reads the front half (last frame's output) and writes the back.
  // Nothing within this frame reads what this frame wrote, which is what makes
  // the pass order irrelevant to correctness and matches Shadertoy.
  for (const PassVk& pass : impl.passes) {
    VkFramebuffer fb = target_fb;
    uint32_t width = target.width;
    uint32_t height = target.height;
    if (pass.target_buffer >= 0) {
      const BufferVk& dst =
          impl.buffers[static_cast<size_t>(pass.target_buffer)];
      if (!dst.used) {
        continue;
      }
      fb = dst.fb[1U - parity];  // the back half
      width = dst.width;
      height = dst.height;
    }

    VkClearValue clear{};
    clear.color = {{0.0F, 0.0F, 0.0F, 1.0F}};
    VkRenderPassBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin.renderPass =
        pass.target_buffer < 0 ? impl.render_pass : impl.buffer_render_pass;
    begin.framebuffer = fb;
    begin.renderArea.offset = {0, 0};
    begin.renderArea.extent = {width, height};
    begin.clearValueCount = 1;
    begin.pClearValues = &clear;
    impl.api.CmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.x = 0.0F;
    viewport.y = 0.0F;
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.minDepth = 0.0F;
    viewport.maxDepth = 1.0F;
    impl.api.CmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {width, height};
    impl.api.CmdSetScissor(cmd, 0, 1, &scissor);

    impl.api.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                             pass.pipeline);
    impl.api.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   impl.pipeline_layout, 0, 1,
                                   &pass.set[parity], 0, nullptr);
    impl.api.CmdPushConstants(cmd, impl.pipeline_layout,
                              VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                              &push);
    impl.api.CmdDraw(cmd, 3, 1, 0, 0);
    impl.api.CmdEndRenderPass(cmd);
    // The buffer render pass ends in SHADER_READ_ONLY_OPTIMAL and its
    // subpass-to-external dependency covers the fragment read, so a later pass
    // sampling this buffer needs no barrier here. It will not sample this
    // frame's write in any case -- it reads the other half.
  }

  // One flip for all buffers: what this frame wrote becomes next frame's front.
  impl.front = 1U - impl.front;
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
