// SPDX-License-Identifier: MIT
// Copyright (c) 2026 shadertoy-cxx contributors
//
// vk_format_probe — which formats can hold a Shadertoy Buffer A..D on this GPU?
//
// A multi-pass program stores each buffer pass in an offscreen image that the
// next pass samples, so the format has to be renderable *and* samplable, with
// linear filtering for the ordinary bilinear fetch. That set is not the same on
// every driver, and picking a format by reputation rather than by asking gets
// two failures that look like bugs elsewhere:
//
//   - a format that is samplable but not renderable fails at image creation,
//     far from the line that chose it;
//   - a format that is renderable but not linearly filterable works, and
//     silently samples wrong.
//
// Both are real on shipping hardware. On a Raspberry Pi 5 (V3D 7.1.7.0, V3DV
// Mesa) R16G16B16A16_UNORM is not a color attachment at all, and
// R32G32B32A32_SFLOAT cannot be linearly filtered — while R16G16B16A16_SFLOAT,
// the format a multi-pass renderer actually wants, is fully supported.
//
// Run it on a target before assuming a buffer format there:
//
//   vk_format_probe
//
// Needs no display, no surface, and no window system — it creates an instance,
// queries formats, and exits, so it is usable over ssh on a headless board.

#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include <vulkan/vulkan.h>

namespace {

struct Candidate {
  VkFormat format;
  std::string_view name;
  std::string_view note;
};

// Ordered best-first for a Shadertoy buffer. Half-float leads because buffers
// are as often accumulators and state as they are color, carrying values well
// outside [0,1]; a UNORM target clamps and bands exactly the feedback effects
// that need the range. The UNORM entries are the fallbacks worth taking when
// nothing better is renderable, and are called out because taking one silently
// changes what a shader computes.
constexpr std::array<Candidate, 7> kCandidates{{
    {VK_FORMAT_R16G16B16A16_SFLOAT, "R16G16B16A16_SFLOAT",
     "preferred: half float, alpha, full range"},
    {VK_FORMAT_R32G32B32A32_SFLOAT, "R32G32B32A32_SFLOAT",
     "more range than needed; often not linearly filterable"},
    {VK_FORMAT_B10G11R11_UFLOAT_PACK32, "B10G11R11_UFLOAT_PACK32",
     "float range at 32bpp but NO ALPHA -- unusable if a shader writes .a"},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, "A2B10G10R10_UNORM_PACK32",
     "fallback: more precision than 8-bit, still clamped to [0,1]"},
    {VK_FORMAT_R16G16B16A16_UNORM, "R16G16B16A16_UNORM",
     "16-bit but frequently not a color attachment"},
    {VK_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM",
     "universal floor: clamps and bands accumulators"},
    {VK_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM",
     "universal floor: clamps and bands accumulators"},
}};

// What a buffer attachment must support. Blend is deliberately absent: the
// full-screen pass overwrites its target, so COLOR_ATTACHMENT_BLEND_BIT is
// reported for information but not required.
constexpr VkFormatFeatureFlags kRequired =
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;

constexpr const char* YesNo(const bool v) {
  return v ? "yes" : "no";
}

void ReportDevice(VkPhysicalDevice device) {
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(device, &props);
  std::printf("\ndevice: %s (Vulkan %u.%u.%u)\n", props.deviceName,
              VK_VERSION_MAJOR(props.apiVersion),
              VK_VERSION_MINOR(props.apiVersion),
              VK_VERSION_PATCH(props.apiVersion));
  std::printf("  %-26s %-9s %-8s %-7s %-6s %s\n", "format", "color_att",
              "sampled", "linear", "blend", "usable");

  std::string_view recommended;
  std::string_view recommended_note;
  for (const Candidate& c : kCandidates) {
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(device, c.format, &fp);
    const VkFormatFeatureFlags f = fp.optimalTilingFeatures;
    const bool color = (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0;
    const bool sampled = (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    const bool linear =
        (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    const bool blend = (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT) != 0;
    const bool usable = (f & kRequired) == kRequired;
    std::printf("  %-26s %-9s %-8s %-7s %-6s %s\n", c.name.data(), YesNo(color),
                YesNo(sampled), YesNo(linear), YesNo(blend),
                usable ? "YES" : "-");
    // First usable entry wins: the table is ordered best-first. B10G11R11 is
    // skipped even when usable, because it has no alpha channel to write.
    if (usable && recommended.empty() &&
        c.format != VK_FORMAT_B10G11R11_UFLOAT_PACK32) {
      recommended = c.name;
      recommended_note = c.note;
    }
  }

  if (recommended.empty()) {
    std::printf(
        "  => no candidate is renderable + samplable + linearly filterable;\n"
        "     multi-pass buffers cannot be stored on this device as listed\n");
    return;
  }
  std::printf("  => use %s (%s)\n", recommended.data(),
              recommended_note.data());
}

}  // namespace

int main() {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "vk_format_probe";
  // 1.0 keeps this runnable on the widest range of drivers; nothing here needs
  // a later core version, and the query itself is 1.0.
  app.apiVersion = VK_API_VERSION_1_0;

  VkInstanceCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  info.pApplicationInfo = &app;

  VkInstance instance = VK_NULL_HANDLE;
  if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) {
    std::fprintf(stderr, "vk_format_probe: vkCreateInstance failed\n");
    return 1;
  }

  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (count == 0) {
    std::fprintf(stderr, "vk_format_probe: no Vulkan physical devices\n");
    vkDestroyInstance(instance, nullptr);
    return 1;
  }
  std::array<VkPhysicalDevice, 8> devices{};
  if (count > devices.size()) {
    count = static_cast<uint32_t>(devices.size());
  }
  vkEnumeratePhysicalDevices(instance, &count, devices.data());

  std::printf(
      "Shadertoy buffer storage formats (optimal tiling).\n"
      "Required: COLOR_ATTACHMENT + SAMPLED_IMAGE + SAMPLED_IMAGE_FILTER_"
      "LINEAR.\n"
      "Blend is reported but not required: the pass overwrites its target.\n");
  for (uint32_t i = 0; i < count; ++i) {
    ReportDevice(devices[i]);
  }

  vkDestroyInstance(instance, nullptr);
  return 0;
}
