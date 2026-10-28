// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
namespace Vulkan::Gow3 {
enum class DlaaState : uint32_t { Off, Waiting, Active, Unavailable, Failed };
// Input, renderer and presenter communicate only through atomics. A key request
// does not claim NGX ran: only a successful evaluation publishes Active.
struct DlaaControl {
    std::atomic<bool> visible{},available{},requested{};
    std::atomic<uint32_t> pending_toggles{},width{},height{};
    std::atomic<DlaaState> state{DlaaState::Off};
    void Toggle() { if(available.load()) pending_toggles.fetch_add(1); }
};
inline DlaaControl& NativeDlaaControl() { static DlaaControl state; return state; }
}
