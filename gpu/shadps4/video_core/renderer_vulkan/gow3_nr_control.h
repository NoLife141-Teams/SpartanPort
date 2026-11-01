// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
namespace Vulkan::Gow3 {
enum class NrState : uint32_t { Off, Waiting, Active, Unavailable, Failed, Neutral };
struct NrControl {
    std::atomic<bool> visible{}, available{};
    std::atomic<bool> guided{}, hud_protected{};
    std::atomic<uint32_t> toggles{}, width{}, height{};
    std::atomic<uint64_t> completed{};
    std::atomic<uint64_t> generation{};
    std::atomic<NrState> state{NrState::Off};
    void Complete(uint64_t epoch,uint64_t count) {
        if(generation.load()!=epoch) return;
        completed=count;hud_protected=true;
        auto expected=NrState::Waiting;
        state.compare_exchange_strong(expected,NrState::Active);
    }
};
inline NrControl& NeuralPreviewControl() { static NrControl control; return control; }
}
