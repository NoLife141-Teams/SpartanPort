// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <memory>
#include "video_core/renderer_vulkan/vk_common.h"
namespace Vulkan {
class Instance;
class Scheduler;
struct Frame;
// Explicit SDR presentation experiment: includes the game's HUD, uses reset on
// every frame, and does not claim temporal/depth integration or production use.
class NrPreview {
public:
    explicit NrPreview(const Instance& instance);
    ~NrPreview();
    void Apply(Scheduler& scheduler, vk::CommandBuffer command, const Frame& source,
               vk::Image destination, vk::Extent2D extent);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
