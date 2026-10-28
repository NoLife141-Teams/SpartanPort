// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <optional>
#include "gow3_camera_inputs.h"
#include "gow3_snapshot_diagnostics.h"

namespace Vulkan::Gow3 {

// Accepted VideoOut request identity, independent of tracing and presentation.
// No guest pointers. IDs identify requests, not their cross-thread arrival order.
struct Flip {
    uint64_t id = 0, address = 0;
    int32_t buffer = -1;
    bool eop = false;
};
class FlipSequence {
public:
    Flip Accepted(uint64_t address, int32_t buffer, bool eop) {
        return {next.fetch_add(1, std::memory_order_relaxed), address, buffer, eop};
    }
private:
    std::atomic<uint64_t> next{1};
};

// Existing VideoOut queue rule, including its blank-frame exception.
inline bool CanAcceptFlip(int32_t buffer, uint64_t pending) {
    return buffer == -1 || pending <= 16;
}

enum class FlipRoute { OrderedTask, Consume, Unordered };
inline FlipRoute RouteFlip(bool stage_a, bool stage_b) {
    // EOP: synchronous SignalFlip callback on the draw consumer. Non-EOP:
    // SendCommand on stage A. Serial stage A must still use RunInOrder, whose
    // fallback drains pending draws before running the task directly.
    return stage_b ? FlipRoute::Consume : stage_a ? FlipRoute::OrderedTask :
                                                    FlipRoute::Unordered;
}

struct CameraFrame {
    Flip flip{};
    uint64_t number = 0;
    uint32_t samples = 0, unsnapshotted = 0, width = 0, height = 0;
    Status prior = Status::Missing, status = Status::Missing;
    SnapshotCounts snapshot_reasons{};
};

// Owned only by the ordered draw consumer (or drained serial stage A).
// Display transitions and presentations never complete camera history.
class CameraFrames {
public:
    void Observe(uint64_t shader, uint32_t slot, const void* bytes, size_t size,
                 uint64_t depth_target, uint32_t width, uint32_t height,
                 SnapshotReason reason = SnapshotReason::None) {
        const auto before = inputs.samples;
        inputs.Observe(shader, slot, bytes, size, depth_target, width, height);
        if (inputs.samples != before) {
            if (!bytes) ++unsnapshotted;
            ++snapshot_reasons[size_t(ResolveSnapshotReason(bytes!=nullptr,true,reason))];
        }
    }
    std::optional<CameraFrame> Display(uint32_t exports, uint32_t bound, bool display_target) {
        if (!display.Observe(exports, bound, display_target)) return std::nullopt;
        CameraFrame event{.number=completed, .samples=inputs.samples-display_samples,
                          .unsnapshotted=unsnapshotted-display_missing,
                          .status=inputs.status};
        display_samples=inputs.samples;
        display_missing=unsnapshotted;
        return event; // diagnostic delta only; no input reset
    }
    std::optional<CameraFrame> Finish(Flip flip) {
        if (!flip.id) return std::nullopt; // no accepted request / diagnostics disabled
        CameraFrame frame{.flip=flip, .number=completed++, .samples=inputs.samples,
                          .unsnapshotted=unsnapshotted, .width=inputs.width,
                          .height=inputs.height, .prior=inputs.status,
                          .status=inputs.FinishFrame(), .snapshot_reasons=snapshot_reasons};
        unsnapshotted=display_samples=display_missing=0;
        snapshot_reasons={};
        return frame;
    }
    const CameraInputs& Inputs() const { return inputs; }
    uint64_t Completed() const { return completed; }
private:
    CameraInputs inputs;
    DisplayPassTracker display;
    uint64_t completed = 0;
    uint32_t unsnapshotted = 0, display_samples = 0, display_missing = 0;
    SnapshotCounts snapshot_reasons{};
};
} // namespace Vulkan::Gow3
