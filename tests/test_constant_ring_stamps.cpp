// SPDX-License-Identifier: GPL-2.0-or-later
// CPU-only publication tests: no GPU, game data, or guest memory reads.
#include <cassert>
#include <cstdio>
#include <utility>
#include <vector>
#include <deque>
#include "gpu/shadps4/video_core/renderer_vulkan/constant_ring_stamps.h"

using Stamp = std::pair<uint64_t, uint64_t>;

int main() {
    Vulkan::ConstantRingStamps state;
    std::vector<Stamp> published;
    const auto publish = [&](uint64_t end, uint64_t tick) {
        published.emplace_back(end, tick);
        return true;
    };
    state.Stamp(0, 1, publish);
    assert(published.empty());
    state.Stamp(1024, 1, publish);
    state.Stamp(2048, 1, publish);
    assert(published.empty()); // current submission must not be retired
    state.Stamp(4096, 2, publish);
    assert(published.size() == 1 && published.back() == Stamp(2048, 1));

    // Regression: a full ring cannot allocate, so end stops advancing. The
    // previous tick must still become visible to the GPU-completion consumer.
    state.Stamp(4096, 3, publish);
    assert(published.size() == 2 && published.back() == Stamp(4096, 2));
    state.Stamp(4096, 3, publish);
    state.Stamp(4096, 4, publish);
    assert(published.size() == 2); // no duplicate publication

    Vulkan::ConstantRingStamps full;
    constexpr uint64_t capacity = 32ull << 20;
    full.Stamp(capacity, 10, publish);
    const auto before = published.size();
    full.Stamp(capacity, 11, publish);
    assert(published.size() == before + 1 && published.back() == Stamp(capacity, 10));

    // A full publication queue must retry even without a new allocation.
    Vulkan::ConstantRingStamps retry;
    bool accept = false;
    const auto sometimes = [&](uint64_t end, uint64_t tick) {
        if (!accept) return false;
        return publish(end, tick);
    };
    retry.Stamp(100, 20, sometimes);
    retry.Stamp(100, 21, sometimes);
    const auto blocked = published.size();
    accept = true;
    retry.Stamp(100, 21, sometimes);
    assert(published.size() == blocked + 1 && published.back() == Stamp(100, 20));

    // Coalescing while the queue is full retains the later (safer) GPU tick.
    Vulkan::ConstantRingStamps coalesced;
    accept = false;
    coalesced.Stamp(100, 30, sometimes);
    coalesced.Stamp(200, 31, sometimes);
    accept = true;
    coalesced.Stamp(200, 32, sometimes);
    assert(published.back() == Stamp(200, 31));
    // Deterministic stress model: the GPU completes later ticks, publication
    // queues can fill, and allocation can stop. Published ranges must never
    // retire before their recorded GPU tick. This is not a Vulkan timing test.
    Vulkan::ConstantRingStamps stressed;
    std::deque<Stamp> queue;
    std::vector<uint64_t> ends_by_tick(1,0);
    uint64_t tick=1,end=0,retired=0,last_published=0;
    uint32_t random=0x12345678;
    const auto publish_bounded=[&](uint64_t range_end, uint64_t range_tick) {
        if (queue.size()==4) return false;
        assert(range_tick<ends_by_tick.size());
        assert(range_end<=ends_by_tick[range_tick]);
        assert(range_end>last_published);
        last_published=range_end;
        queue.emplace_back(range_end,range_tick);
        return true;
    };
    const auto retire_completed=[&](uint64_t completed) {
        while (!queue.empty() && queue.front().second<=completed) {
            assert(queue.front().first>=retired);
            retired=queue.front().first;
            assert(retired<=ends_by_tick[completed]);
            queue.pop_front();
        }
    };
    for (unsigned i=0;i<200000;++i) {
        random=random*1664525u+1013904223u;
        if ((random&7u)==0) ++tick;
        ends_by_tick.resize(size_t(tick+1),end);
        if ((random&0x30u)!=0) end+=64+((random>>8)&1023u);
        ends_by_tick[tick]=end;
        stressed.Stamp(end,tick,publish_bounded);
        if ((random&0x300u)==0 && tick>=5) retire_completed(tick-5);
    }
    // Flush the final stalled cursor once the queue and GPU have caught up.
    ++tick; ends_by_tick.resize(size_t(tick+1),end);
    retire_completed(tick);
    stressed.Stamp(end,tick,publish_bounded);
    retire_completed(tick);
    assert(retired==end && queue.empty());
    std::puts("Constant ring stamps: PASS (stalled cursor, coalescing, full queue retry, 200000 delayed-GPU synthetic steps)");
}
