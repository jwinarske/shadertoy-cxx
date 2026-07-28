// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// vk_offscreen_render — render a Shadertoy shader to a PPM with
// VkOffscreenRenderer, on a device this program creates and owns.
//
// The Vulkan counterpart of headless_render, and the exercise for
// VkOffscreenRenderer's contract: everything Vulkan here belongs to the
// example, and the renderer only borrows it.  That is the same shape a host
// embedder is in, so if this works the embedder case works.
//
// Output is directly comparable with headless_render's, which is the point —
// the two back-ends rendering the same shader should agree.
//
//   vk_offscreen_render <shader.frag|shader.json> <out.ppm> [W H time]

#include <vulkan/vulkan.h>

#include <shadertoy/inputs.hpp>
#include <shadertoy/program.hpp>
#include <shadertoy/vk_offscreen_renderer.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Gpu {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice phys = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t queue_family = 0;
};

bool CreateGpu(Gpu& gpu) {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "vk_offscreen_render";
  app.apiVersion = VK_API_VERSION_1_1;

  VkInstanceCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  ici.pApplicationInfo = &app;
  if (vkCreateInstance(&ici, nullptr, &gpu.instance) != VK_SUCCESS) {
    std::fprintf(stderr, "vkCreateInstance failed\n");
    return false;
  }

  uint32_t count = 0;
  vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr);
  if (count == 0) {
    std::fprintf(stderr, "no Vulkan physical devices\n");
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(gpu.instance, &count, devices.data());

  // First device with a graphics queue. A discrete-vs-integrated preference
  // would be noise here — any of them renders this correctly.
  for (VkPhysicalDevice candidate : devices) {
    uint32_t families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
    std::vector<VkQueueFamilyProperties> props(families);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families,
                                             props.data());
    for (uint32_t i = 0; i < families; ++i) {
      if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
        gpu.phys = candidate;
        gpu.queue_family = i;
        break;
      }
    }
    if (gpu.phys != VK_NULL_HANDLE) {
      break;
    }
  }
  if (gpu.phys == VK_NULL_HANDLE) {
    std::fprintf(stderr, "no graphics queue family\n");
    return false;
  }

  const float priority = 1.0F;
  VkDeviceQueueCreateInfo dqci{};
  dqci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  dqci.queueFamilyIndex = gpu.queue_family;
  dqci.queueCount = 1;
  dqci.pQueuePriorities = &priority;

  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &dqci;
  if (vkCreateDevice(gpu.phys, &dci, nullptr, &gpu.device) != VK_SUCCESS) {
    std::fprintf(stderr, "vkCreateDevice failed\n");
    return false;
  }
  vkGetDeviceQueue(gpu.device, gpu.queue_family, 0, &gpu.queue);

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(gpu.phys, &props);
  std::fprintf(stderr, "device: %s\n", props.deviceName);
  return true;
}

bool FindMemoryType(VkPhysicalDevice phys,
                    uint32_t bits,
                    VkMemoryPropertyFlags want,
                    uint32_t* out) {
  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(phys, &mem);
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    if ((bits & (1U << i)) != 0 &&
        (mem.memoryTypes[i].propertyFlags & want) == want) {
      *out = i;
      return true;
    }
  }
  return false;
}

std::string ReadFile(const char* path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <shader.frag> <out.ppm> [W H time]\n",
                 argv[0]);
    return 2;
  }
  const char* shader_path = argv[1];
  const char* out_path = argv[2];
  const uint32_t width = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 320;
  const uint32_t height = argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 180;
  const float time = argc > 5 ? std::strtof(argv[5], nullptr) : 0.0F;

  const std::string source = ReadFile(shader_path);
  if (source.empty()) {
    std::fprintf(stderr, "could not read %s\n", shader_path);
    return 1;
  }

  Gpu gpu;
  if (!CreateGpu(gpu)) {
    return 1;
  }

  // B8G8R8A8_UNORM matches what a compositor most often hands out, so it is
  // the format worth exercising. The readback below unswizzles it.
  constexpr VkFormat kFormat = VK_FORMAT_B8G8R8A8_UNORM;

  shadertoy::VkExternalDevice external{};
  external.instance = gpu.instance;
  external.physical_device = gpu.phys;
  external.device = gpu.device;
  external.queue = gpu.queue;
  external.queue_family_index = gpu.queue_family;
  external.get_instance_proc_addr = vkGetInstanceProcAddr;

  shadertoy::VkOffscreenConfig config{};
  config.color_format = kFormat;
  config.final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

  auto renderer = shadertoy::VkOffscreenRenderer::Create(external, config);
  if (!renderer || !renderer->Init(source)) {
    return 1;
  }

  // ── The color target, owned here ────────────────────────────────────────
  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = kFormat;
  ici.extent = {width, height, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  if (vkCreateImage(gpu.device, &ici, nullptr, &image) != VK_SUCCESS) {
    std::fprintf(stderr, "vkCreateImage(target) failed\n");
    return 1;
  }
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(gpu.device, image, &req);
  uint32_t type_index = 0;
  if (!FindMemoryType(gpu.phys, req.memoryTypeBits,
                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type_index)) {
    std::fprintf(stderr, "no device-local memory type\n");
    return 1;
  }
  VkMemoryAllocateInfo mai{};
  mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type_index;
  VkDeviceMemory image_memory = VK_NULL_HANDLE;
  vkAllocateMemory(gpu.device, &mai, nullptr, &image_memory);
  vkBindImageMemory(gpu.device, image, image_memory, 0);

  VkImageViewCreateInfo ivci{};
  ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  ivci.image = image;
  ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  ivci.format = kFormat;
  ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  vkCreateImageView(gpu.device, &ivci, nullptr, &view);

  // ── Readback buffer ─────────────────────────────────────────────────────
  const VkDeviceSize buffer_size =
      static_cast<VkDeviceSize>(width) * height * 4;
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = buffer_size;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer = VK_NULL_HANDLE;
  vkCreateBuffer(gpu.device, &bci, nullptr, &buffer);
  VkMemoryRequirements breq{};
  vkGetBufferMemoryRequirements(gpu.device, buffer, &breq);
  if (!FindMemoryType(gpu.phys, breq.memoryTypeBits,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      &type_index)) {
    std::fprintf(stderr, "no host-visible memory type\n");
    return 1;
  }
  mai.allocationSize = breq.size;
  mai.memoryTypeIndex = type_index;
  VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
  vkAllocateMemory(gpu.device, &mai, nullptr, &buffer_memory);
  vkBindBufferMemory(gpu.device, buffer, buffer_memory, 0);

  // ── One command buffer: render, then copy out ───────────────────────────
  VkCommandPoolCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpci.queueFamilyIndex = gpu.queue_family;
  VkCommandPool pool = VK_NULL_HANDLE;
  vkCreateCommandPool(gpu.device, &cpci, nullptr, &pool);

  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(gpu.device, &cbai, &cmd);

  VkCommandBufferBeginInfo cbbi{};
  cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &cbbi);

  shadertoy::ShaderInputs inputs;
  inputs.res_x = static_cast<float>(width);
  inputs.res_y = static_cast<float>(height);
  inputs.time = time;
  inputs.frame = 0;

  shadertoy::VkOffscreenTarget target{};
  target.image = image;
  target.view = view;
  target.width = width;
  target.height = height;

  // This is the whole integration: the renderer records into a command buffer
  // it did not create, and the copy below rides in the same submit.
  if (!renderer->RecordRender(cmd, inputs, target)) {
    return 1;
  }

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {width, height, 1};
  // The render pass already left the image in TRANSFER_SRC_OPTIMAL and its
  // external subpass dependency ordered the transition against TRANSFER, so no
  // barrier is needed here — which is the contract being tested.
  vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         buffer, 1, &region);
  vkEndCommandBuffer(cmd);

  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  if (vkQueueSubmit(gpu.queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
    std::fprintf(stderr, "vkQueueSubmit failed\n");
    return 1;
  }
  vkQueueWaitIdle(gpu.queue);

  // ── Write the PPM ───────────────────────────────────────────────────────
  void* mapped = nullptr;
  vkMapMemory(gpu.device, buffer_memory, 0, buffer_size, 0, &mapped);
  const auto* pixels = static_cast<const uint8_t*>(mapped);

  std::ofstream out(out_path, std::ios::binary);
  if (!out) {
    std::fprintf(stderr, "could not write %s\n", out_path);
    return 1;
  }
  out << "P6\n" << width << " " << height << "\n255\n";
  for (uint32_t i = 0; i < width * height; ++i) {
    // BGRA in memory; PPM wants RGB.
    const uint8_t* p = pixels + (static_cast<size_t>(i) * 4);
    const char rgb[3] = {static_cast<char>(p[2]), static_cast<char>(p[1]),
                         static_cast<char>(p[0])};
    out.write(rgb, 3);
  }
  out.close();
  vkUnmapMemory(gpu.device, buffer_memory);

  std::fprintf(stderr, "wrote %s (%ux%u)\n", out_path, width, height);

  // Renderer first: it holds framebuffers built from `view`.
  renderer.reset();
  vkDestroyCommandPool(gpu.device, pool, nullptr);
  vkDestroyBuffer(gpu.device, buffer, nullptr);
  vkFreeMemory(gpu.device, buffer_memory, nullptr);
  vkDestroyImageView(gpu.device, view, nullptr);
  vkDestroyImage(gpu.device, image, nullptr);
  vkFreeMemory(gpu.device, image_memory, nullptr);
  vkDestroyDevice(gpu.device, nullptr);
  vkDestroyInstance(gpu.instance, nullptr);
  return 0;
}
