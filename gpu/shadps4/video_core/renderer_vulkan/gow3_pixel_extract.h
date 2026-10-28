// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Shared by the runtime and the independent Vulkan fixture. No cache, scheduler,
// file or guest-memory access. The caller owns submission/completion and sources.
#include <vulkan/vulkan_core.h>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>

namespace Vulkan::Gow3::Pixels {
inline constexpr uint64_t MemoryBudget=256ull*1024*1024;
inline constexpr uint64_t FileBudget=192ull*1024*1024;
inline constexpr uint32_t MaxWidth=1920,MaxHeight=1080;
inline constexpr uint64_t ByteSize(uint32_t width,uint32_t height) {
    return width && height && width<=MaxWidth && height<=MaxHeight ? uint64_t(width)*height*2*5*4 : 0;
}
static_assert(2*ByteSize(MaxWidth,MaxHeight)<=FileBudget);
struct Source {
    VkImageView view{};
    VkImageLayout layout{};
};
class Extractor {
public:
    struct Output;
    Extractor(VkDevice device,PFN_vkGetDeviceProcAddr get,
              const VkPhysicalDeviceMemoryProperties& memory,
              uint64_t max_storage_range,std::span<const uint32_t> spirv);
    ~Extractor();
    Extractor(const Extractor&)=delete;
    std::unique_ptr<Output> Allocate(uint32_t width,uint32_t height);
    // Must be outside rendering. Full read-before/read-after dependencies leave
    // source layouts and the runtime image tracker unchanged. No resolve/copy.
    void Record(VkCommandBuffer command,Output& out,Source depth,Source color);
    std::span<const std::byte> ReadCompleted(Output& out);
    void MarkCompleted(Output& out);
    uint64_t Allocated() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
struct Extractor::Output {
    ~Output();
    Output(const Output&)=delete;
    uint32_t width{},height{};
    uint64_t bytes{},allocation_bytes{};
    bool recorded{},completed{};
private:
    friend class Extractor;
    explicit Output(Extractor::Impl* owner):owner(owner) {}
    Extractor::Impl* owner;
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkDescriptorSet descriptor{};
    void* mapped{};
};
} // namespace Vulkan::Gow3::Pixels
