// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "vk_common.h"
#include "gow3_pair_capture.h"
#include "gow3_pixel_extract.h"
#include "motion_history.h"
#include <mutex>
namespace VideoCore { class TextureCache; }
namespace Vulkan {
class Instance; class Scheduler;
namespace Gow3::Pixels {
// Opt-in, process-lifetime sink: at most two allocations, retained until exit.
// This also keeps abandoned outputs alive if shutdown does not drain GPU work.
class Capture {
public:
    Capture(const Instance& instance,const char* directory);
    void Boundary(const CameraFrame& frame,const CameraInputs& camera,bool pair_active,Motion::History::Stats stats={});
    bool Wants(uint64_t interval) const { return first_interval && interval>=first_interval && interval<first_interval+2; }
    void Submit(uint64_t tick,vk::Result result);
    void Record(const Pair::Draw& draw,const Provenance::Identity& depth,
        vk::ImageLayout depth_layout,const Provenance::Identity& color,vk::ImageLayout color_layout,
        Scheduler& scheduler,VideoCore::TextureCache& cache,bool allowed,Source motion={});
    static bool Target(const Pair::Draw& draw) {
        return draw.vs==0x475a41468b1a1fbcull && draw.ps==0x475a4146e3a74097ull &&
            draw.vs_key==0xefd04d8023fec408ull && draw.ps_key==0xefd0530963dc1d55ull;
    }
private:
    struct Job {
        uint64_t interval{},draw{},tick{};
        Provenance::Identity depth{},color{};
        std::array<u32,8> sampled_components{};
        std::unique_ptr<Extractor::Output> output;
        CameraFrame frame{}; Matrix camera{};
        Motion::History::Stats motion_stats{};
        const char* error{};
        bool attempted{},submitted{},complete{},closed{},published{};
    };
    void Complete(Job& job);
    void Publish(Job& job);
    std::mutex mutex;
    std::filesystem::path directory;
    std::FILE* events{};
    std::unique_ptr<Extractor> extractor;
    std::array<Job,2> jobs{};
    uint64_t first_interval{};
};
} // namespace Gow3::Pixels
} // namespace Vulkan
