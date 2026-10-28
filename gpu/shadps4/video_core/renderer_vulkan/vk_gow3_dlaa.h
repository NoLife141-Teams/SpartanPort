// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gow3_camera_inputs.h"
#include "vk_common.h"
#include "video_core/texture_cache/image.h"
#include <memory>
namespace Vulkan {
class Instance; class Scheduler;
// Native GoW3 composition hook. Owns all NGX inputs/output, never rewrites guest
// images. The game's following composition and UI passes consume a replacement
// single- or two-sample view with the original sampled color/alpha semantics.
class Gow3Dlaa {
public:
    Gow3Dlaa(const Instance&,Scheduler&);
    ~Gow3Dlaa();
    bool Enabled() const;
    bool Active();
    void Observe(u64 shader,u32 slot,const void* bytes,size_t size,u64 depth,u32 w,u32 h);
    std::array<float,2> Jitter() const;
    u64 Depth() const;
    bool OnFlip();
    // Called exactly at the known composition; closes the scene-camera epoch,
    // independently of accepted-flip scheduling and diagnostic journal budgets.
    vk::ImageView Process(vk::ImageView depth,vk::ImageLayout depth_layout,
        vk::ImageView color,vk::ImageLayout color_layout,vk::ImageView motion,u64 depth_uid,u32 w,u32 h);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
