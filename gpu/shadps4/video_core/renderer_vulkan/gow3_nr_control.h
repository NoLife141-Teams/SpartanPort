// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
namespace Vulkan::Gow3 {
enum class NrState : uint32_t { Off, Waiting, Active, Unavailable, Failed };
struct NrControl {
    std::atomic<bool> visible{}, available{};
    std::atomic<bool> guided{}, hud_protected{};
    std::atomic<uint32_t> toggles{}, width{}, height{};
    std::atomic<uint64_t> completed{};
    std::atomic<NrState> state{NrState::Off};
};
inline NrControl& NeuralPreviewControl() { static NrControl control; return control; }
}
