// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_nr_preview.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_dlss.h"
#include "video_core/renderer_vulkan/gow3_nr_control.h"
#include "video_core/renderer_vulkan/gow3_motion.h"
#include "video_core/buffer_cache/buffer.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <vk_mem_alloc.h>

namespace Vulkan {
vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width, s32 swapchain_height);
struct NrPreview::Impl {
    const Instance& instance;
    Dlss* dlss{};
    std::array<vk::Image, 2> images{};
    std::array<vk::ImageView, 2> views{};
    std::array<VmaAllocation, 2> allocations{};
    uint32_t width{}, height{};
    uint64_t evaluated{};
    bool enabled{}, failed{}, created{}, captured{};
    std::filesystem::path dump;
    explicit Impl(const Instance& instance_) : instance(instance_) {}
    void Fail(const char* reason) {
        failed = true;
        auto& control = Gow3::NeuralPreviewControl();
        control.available = false;
        control.state = Gow3::NrState::Failed;
        std::printf("GoW3 NR preview: disabled (%s)\n", reason);
    }
    void Barrier(vk::CommandBuffer command, unsigned n, vk::ImageLayout from, vk::ImageLayout to) {
        command.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
            vk::PipelineStageFlagBits::eAllCommands, {}, {}, {}, vk::ImageMemoryBarrier{
                .srcAccessMask = from == vk::ImageLayout::eUndefined ? vk::AccessFlags{} :
                    vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eMemoryRead,
                .dstAccessMask = vk::AccessFlagBits::eMemoryWrite | vk::AccessFlagBits::eMemoryRead,
                .oldLayout = from, .newLayout = to,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = images[n],
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
    }
    bool Create(vk::CommandBuffer command, uint32_t w, uint32_t h) {
        width = w;
        height = h;
        const auto format = vk::Format::eR32G32B32A32Sfloat;
        const auto required = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst |
            vk::FormatFeatureFlagBits::eSampledImage | vk::FormatFeatureFlagBits::eStorageImage;
        if ((instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures & required) != required) {
            Fail("RGBA32F blit/storage unsupported");
            return false;
        }
        for (unsigned n = 0; n < 2; ++n) {
            const vk::ImageCreateInfo info{
                .imageType = vk::ImageType::e2D, .format = format, .extent = {w, h, 1},
                .mipLevels = 1, .arrayLayers = 1, .samples = vk::SampleCountFlagBits::e1,
                .tiling = vk::ImageTiling::eOptimal,
                .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage |
                         vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst};
            const VkImageCreateInfo raw_info = info;
            VmaAllocationCreateInfo allocation{};
            allocation.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
            VkImage image{};
            if (vmaCreateImage(instance.GetAllocator(), &raw_info, &allocation, &image,
                               &allocations[n], nullptr) != VK_SUCCESS) {
                Fail("NR image allocation failed");
                return false;
            }
            images[n] = image;
            const auto [result, view] = instance.GetDevice().createImageView(vk::ImageViewCreateInfo{
                .image = images[n], .viewType = vk::ImageViewType::e2D, .format = format,
                .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}});
            if (result != vk::Result::eSuccess) { Fail("NR image view failed"); return false; }
            views[n] = view;
            Barrier(command, n, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral);
        }
        if (!dlss->CreateNrFeature(command, w, h)) { Fail("NGX NR creation failed"); return false; }
        created = true;
        auto& control = Gow3::NeuralPreviewControl();
        control.width = w;
        control.height = h;
        return true;
    }
    Dlss::Resource Resource(unsigned n) const {
        return {images[n], views[n], vk::Format::eR32G32B32A32Sfloat,
                vk::ImageAspectFlagBits::eColor, width, height};
    }
    void Capture(Scheduler& scheduler, vk::CommandBuffer command) {
        if (captured || dump.empty() || evaluated != 60) return;
        captured = true;
        const uint64_t bytes = uint64_t(width)*height*16;
        auto buffer = std::make_shared<VideoCore::Buffer>(instance, 0, bytes*2,
            VideoCore::MemoryType::HostCached, "NR matched display input/output");
        for (unsigned n = 0; n < 2; ++n) {
            Barrier(command, n, vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal);
            command.copyImageToBuffer(images[n], vk::ImageLayout::eTransferSrcOptimal, buffer->Handle(),
                vk::BufferImageCopy{.bufferOffset = bytes*n,
                    .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                    .imageExtent = {width, height, 1}});
            Barrier(command, n, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral);
        }
        command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eHost,
            {}, vk::MemoryBarrier{.srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                                 .dstAccessMask = vk::AccessFlagBits::eHostRead}, {}, {});
        const auto directory = dump;
        const auto tick = scheduler.CurrentTick();
        scheduler.DeferOperation([buffer, bytes, directory, tick, w=width, h=height] {
            buffer->Invalidate(0, bytes*2);
            bool ok = true;
            for (unsigned n = 0; n < 2; ++n) {
                const auto path = directory / (n ? "output.rgba32f" : "input.rgba32f");
                FILE* file = std::fopen(path.string().c_str(), "wbx");
                if (!file) { ok = false; continue; }
                const bool written = std::fwrite(buffer->mapped_data.data()+bytes*n, 1, bytes, file) == bytes;
                const bool closed = std::fclose(file) == 0;
                ok &= written && closed;
            }
            FILE* meta = std::fopen((directory / "capture.json").string().c_str(), "wbx");
            if (meta) {
                std::fprintf(meta, "{\"complete\":%s,\"gpu_complete\":true,\"width\":%u,\"height\":%u,\"tick\":%llu,\"evaluation\":60,\"reset_each_frame\":true,\"motion_depth\":false,\"hud_included\":true,\"format\":\"RGBA32F display encoded LDR\"}\n",
                    ok ? "true" : "false", w, h, (unsigned long long)tick);
                std::fclose(meta);
            } else ok = false;
            std::printf("GoW3 NR preview: matched GPU capture complete=%u %ux%u\n", ok, w, h);
        });
    }
};

NrPreview::NrPreview(const Instance& instance) : impl(std::make_unique<Impl>(instance)) {
    if (Gow3::NeuralSceneRequested()) return; // draw scheduler owns the pre-HUD feature and F7
    const char* mode = std::getenv("SPARTAN_GOW3_NR_PREVIEW");
    if (!mode || std::string_view(mode) != "1") return;
    auto& control = Gow3::NeuralPreviewControl();
    control.visible = true;
    impl->dlss = Dlss::Get();
    control.available = impl->dlss && impl->dlss->NrAvailable();
    control.state = control.available ? Gow3::NrState::Off : Gow3::NrState::Unavailable;
    if (const char* directory = std::getenv("SPARTAN_GOW3_NR_DUMP")) {
        const std::filesystem::path path(directory);
        if (path.is_absolute() && std::filesystem::is_directory(path) && std::filesystem::is_empty(path)) impl->dump = path;
    }
    std::puts("GoW3 NR preview: F7 toggles LDR tone-only experiment; HUD included, no motion/depth, reset each frame");
}
NrPreview::~NrPreview() {
    if (!impl->dlss) return;
    const auto device = impl->instance.GetDevice();
    (void)device.waitIdle();
    impl->dlss->Shutdown();
    for (unsigned n = 0; n < 2; ++n) {
        if (impl->views[n]) device.destroyImageView(impl->views[n]);
        if (impl->images[n]) vmaDestroyImage(impl->instance.GetAllocator(), impl->images[n], impl->allocations[n]);
    }
}
void NrPreview::Apply(Scheduler& scheduler, vk::CommandBuffer command, const Frame& source,
                      vk::Image destination, vk::Extent2D extent) {
    auto& control = Gow3::NeuralPreviewControl();
    if (!impl->dlss || impl->failed || !control.available.load()) return;
    if (control.toggles.exchange(0) & 1) {
        impl->enabled = !impl->enabled;
        control.state = impl->enabled ? Gow3::NrState::Waiting : Gow3::NrState::Off;
        std::printf("GoW3 NR preview: requested %s\n", impl->enabled ? "ON" : "OFF");
    }
    if (!impl->enabled) return;
    // Restrict the experiment to an unambiguous SDR display-encoded source.
    if (source.is_hdr || (source.format != vk::Format::eB8G8R8A8Unorm &&
                          source.format != vk::Format::eR8G8B8A8Unorm)) {
        impl->Fail("NR preview requires an SDR UNORM display frame"); return;
    }
    if (!impl->created && !impl->Create(command, source.width, source.height)) return;
    if (source.width != impl->width || source.height != impl->height) {
        impl->Fail("NR preview extent changed; restart the experiment"); return;
    }
    const auto blit = [](uint32_t w, uint32_t h) {
        const vk::ImageSubresourceLayers layer{vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        const std::array offsets{vk::Offset3D{}, vk::Offset3D{int(w), int(h), 1}};
        return vk::ImageBlit{.srcSubresource=layer, .srcOffsets=offsets,
                            .dstSubresource=layer, .dstOffsets=offsets};
    };
    impl->Barrier(command, 0, vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferDstOptimal);
    command.blitImage(source.image, vk::ImageLayout::eTransferSrcOptimal, impl->images[0],
        vk::ImageLayout::eTransferDstOptimal, blit(source.width, source.height), vk::Filter::eNearest);
    impl->Barrier(command, 0, vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eGeneral);
    impl->Barrier(command, 1, vk::ImageLayout::eGeneral, vk::ImageLayout::eGeneral);
    if (!impl->dlss->EvaluateNrPreview(command, impl->Resource(0), impl->Resource(1))) {
        impl->Fail(impl->dlss->Problem().c_str()); return;
    }
    ++impl->evaluated;
    impl->Capture(scheduler, command);
    impl->Barrier(command, 1, vk::ImageLayout::eGeneral, vk::ImageLayout::eTransferSrcOptimal);
    // The ordinary presentation blit already wrote destination. Order the NR
    // overwrite after it; preserve that ordinary image on any earlier failure.
    command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer,
        {}, vk::MemoryBarrier{.srcAccessMask=vk::AccessFlagBits::eTransferWrite,
                             .dstAccessMask=vk::AccessFlagBits::eTransferWrite}, {}, {});
    command.blitImage(impl->images[1], vk::ImageLayout::eTransferSrcOptimal, destination,
        vk::ImageLayout::eTransferDstOptimal,
        MakeImageBlitFit(source.width, source.height, extent.width, extent.height), vk::Filter::eLinear);
    impl->Barrier(command, 1, vk::ImageLayout::eTransferSrcOptimal, vk::ImageLayout::eGeneral);
    scheduler.DeferOperation([] {
        auto& live = Gow3::NeuralPreviewControl();
        const auto count = live.completed.fetch_add(1)+1;
        auto expected = Gow3::NrState::Waiting;
        live.state.compare_exchange_strong(expected, Gow3::NrState::Active);
        if (count == 1 || count % 120 == 0) std::printf("GoW3 NR preview: GPU-complete evaluations=%llu\n", (unsigned long long)count);
    });
}
}
