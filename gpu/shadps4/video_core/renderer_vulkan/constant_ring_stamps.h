// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace Vulkan {

// Stage B's publication state, independent of Vulkan for CPU regression tests.
// publish(end, tick) returns false when the stamp queue is full. Unpublished
// ranges can be coalesced into a later tick, never retired before their GPU work.
class ConstantRingStamps {
public:
    template <typename Publish>
    void Stamp(uint64_t end, uint64_t tick, Publish&& publish) {
        // Even when allocation has stalled, a new submission tick must publish
        // the previous range. Otherwise a full ring can never reclaim it.
        if (tick != pending_tick && pending_end != last_published &&
            publish(pending_end, pending_tick)) {
            last_published = pending_end;
        }
        if (end != pending_end) {
            pending_end = end;
            pending_tick = tick;
        }
    }

private:
    uint64_t pending_end = 0, pending_tick = 0, last_published = 0;
};

} // namespace Vulkan
