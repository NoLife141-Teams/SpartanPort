// SPDX-License-Identifier: GPL-2.0-or-later
// CPU characterization of the actual container/recording primitives.
// No Vulkan instance, scheduler execution, game data or GPU-lifetime simulation.
#include <cassert>
#include <cstdio>
#include <memory>
#include <vector>
#include "common/slot_vector.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

extern "C" { std::uint64_t runtime_disabled_optimizations=0; }
struct Resource {
    uint64_t uid;
    std::unique_ptr<int> owned;
    explicit Resource(uint64_t value):uid(value),owned(std::make_unique<int>(int(value))) {}
    Resource(Resource&&)=default;
};
int main() {
    {
        Common::SlotVector<Resource> slots;
        const auto a=slots.insert(100); const auto index=a.index;
        slots.erase(a); const auto b=slots.insert(200);
        assert(b.index==index && slots[b].uid==200 && slots[b].uid!=100);
        std::puts("PASS SlotVector reuses index: nonzero UID is required");
        const auto old_address=reinterpret_cast<uintptr_t>(&slots[b]);
        const int* allocation=slots[b].owned.get();
        for(unsigned i=0;i<2048;++i) (void)slots.insert(300+i);
        assert(reinterpret_cast<uintptr_t>(&slots[b])!=old_address);
        assert(slots[b].uid==200 && slots[b].owned.get()==allocation);
        // Never dereference the old address: Reserve moved/destroyed the object.
        std::puts("PASS SlotVector growth moves objects, owned payload remains valid");
    }
    {
        Vulkan::RecordChunk chunk; uint64_t mutable_frame=1;
        std::vector<uint64_t> observed;
        for(unsigned i=0;i<3;++i) {
            mutable_frame=10+i;
            assert(chunk.Push([token=mutable_frame,&observed](vk::CommandBuffer){observed.push_back(token);}));
        }
        mutable_frame=999; chunk.Execute(vk::CommandBuffer{});
        assert((observed==std::vector<uint64_t>{10,11,12}) && chunk.Empty());
        std::puts("PASS RecordChunk FIFO transports values, independent of later mutable frame");
    }
    {
        Vulkan::RecordChunk chunk; auto owner=std::make_shared<int>(42); std::weak_ptr<int> weak=owner;
        assert(chunk.Push([keep=owner](vk::CommandBuffer){assert(*keep==42);}));
        owner.reset(); assert(!weak.expired()); chunk.Execute(vk::CommandBuffer{});
        assert(weak.expired());
        std::puts("PASS RecordChunk destroys closure at CPU recording completion, not GPU completion");
    }
    {
        Vulkan::RecordChunk chunk; auto pending=std::make_shared<int>(7); std::weak_ptr<int> weak=pending;
        // Capture only a label. The independent pending owner must outlive recording.
        assert(chunk.Push([label=*pending](vk::CommandBuffer){assert(label==7);}));
        chunk.Execute(vk::CommandBuffer{}); assert(!weak.expired());
        pending.reset(); assert(weak.expired());
        std::puts("PASS independent owner outlives RecordChunk (no claim about a GPU fence)");
    }
    {
        Vulkan::RecordChunk chunk; unsigned calls=0;
        std::array<std::byte,32768> payload{};
        auto command=[payload,&calls](vk::CommandBuffer){assert(payload[0]==std::byte{}); ++calls;};
        unsigned accepted=0;
        while(chunk.Push(command)) {++accepted; assert(accepted<8);}
        assert(accepted>0 && chunk.Size()<=Vulkan::RecordChunk::Capacity);
        auto pointer=std::make_unique<int>(81); int* raw=pointer.get();
        auto rejected=[owned=std::move(pointer),payload,&calls,raw](vk::CommandBuffer){assert(owned.get()==raw && *owned==81 && payload[0]==std::byte{}); ++calls;};
        assert(!chunk.Push(std::move(rejected))); // refusal leaves ownership untouched
        rejected(vk::CommandBuffer{}); chunk.Execute(vk::CommandBuffer{});
        assert(calls==accepted+1 && chunk.Empty());
        std::puts("PASS RecordChunk capacity refusal preserves move-only command ownership");
    }
    std::puts("PASS 6 CPU characterization checks; runtime integration and GPU synchronization untested here");
}
