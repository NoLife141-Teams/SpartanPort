// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Metadata only: an identity is NOT a content version or a lifetime lease.
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace Vulkan::Gow3::Provenance {
inline bool Requested(const char* value) { return value && std::strcmp(value,"1")==0; }
enum class Resolution : uint32_t { Unknown, NullView, NativeView, Unregistered, ScanLimit, CacheBusy };
enum class Path : uint32_t { Normal, Memo, Helper, Attachment };
struct Identity {
    uint64_t uid{}, registry_generation{}, alias_authority_generation{};
    uint64_t image{}, view{}, address{};
    uint32_t slot=~0u, backing_ordinal{}, view_ordinal{}, view_slot=~0u;
    uint32_t format{}, view_format{}, samples{}, aspect{}, mip{}, levels{}, layer{}, layers{};
    Resolution resolution=Resolution::Unknown;
    bool current_backing{};
};
// Backings and their view arrays are append-only within an Image's lifetime.
// Thus (nonzero image UID, 1-based backing ordinal, 1-based view ordinal) is
// an object-generation key even if SlotVector moves or reuses a slot/handle.
inline bool SameView(const Identity& a,const Identity& b) {
    return a.resolution==Resolution::NativeView && b.resolution==Resolution::NativeView &&
        a.uid && a.uid==b.uid && a.backing_ordinal && a.backing_ordinal==b.backing_ordinal &&
        a.view_ordinal && a.view_ordinal==b.view_ordinal;
}
struct Binding { Identity resource{}; Path path{}; bool observed{}; };
class Bindings {
public:
    static constexpr uint32_t Capacity=64;
    void Begin(uint64_t draw_,uint64_t interval_) {
        draw=draw_; interval=interval_; lost=0;
        // Owner does this BEFORE Fork. Helper is then the sole image writer;
        // the owner reads/resumes only AFTER Join. No mutable global token.
        if(draw) entries={};
    }
    bool Enabled() const { return draw!=0; }
    void Set(uint32_t index,Identity identity,Path path) {
        if(!Enabled()) return;
        if(index>=entries.size()) { ++lost; return; }
        entries[index]={identity,path,true};
    }
    Binding Get(uint64_t token,uint64_t frame,uint32_t index,uint64_t actual_view) const {
        if(!draw || token!=draw || frame!=interval || index>=entries.size()) return {};
        const auto& entry=entries[index];
        return entry.observed && entry.resource.view==actual_view ? entry : Binding{};
    }
    uint32_t Lost() const { return lost; }
private:
    std::array<Binding,Capacity> entries{};
    uint64_t draw{},interval{};
    uint32_t lost{};
};
enum class Reason : uint32_t {
    Registered, Unregistered, CpuOverlap, CpuPageUntrackHead, CpuPageUntrackTail,
    MaybeCpuDirty, GpuBaseInvalidation, BackingWriteApplied, AliasAuthorityPublished,
    AliasMember, Unmap, BackingWriteNoted
};
struct Event {
    uint64_t attempt{},uid{},related_uid{},registry_generation{},address{},size{};
    uint32_t slot=~0u;
    Reason reason{};
};
struct Batch {
    static constexpr uint32_t Capacity=64;
    std::array<Event,Capacity> events{};
    uint64_t attempts{},lost{};
    uint32_t count{};
    bool busy{};
};
// Owned by TextureCache, never references the file collector. Producers copy
// already-known IDs under the cache's existing synchronization. They never
// wait, allocate, read guest memory or call Vulkan. A consumer releases this
// gate before using any cache or collector API (no lock-order inversion).
class Inbox {
public:
    void Push(Event event) noexcept {
        event.attempt=attempts.fetch_add(1,std::memory_order_relaxed)+1;
        if(gate.test_and_set(std::memory_order_acquire)) {
            lost.fetch_add(1,std::memory_order_relaxed); return;
        }
        if(count<events.size()) events[count++]=event;
        else lost.fetch_add(1,std::memory_order_relaxed);
        gate.clear(std::memory_order_release);
    }
    Batch Drain() noexcept {
        Batch result;
        if(gate.test_and_set(std::memory_order_acquire)) result.busy=true;
        else {
            result.count=count;
            for(uint32_t i=0;i<count;++i) result.events[i]=events[i];
            count=0; gate.clear(std::memory_order_release);
        }
        result.attempts=attempts.load(std::memory_order_relaxed);
        result.lost=lost.load(std::memory_order_relaxed);
        return result;
    }
private:
    std::atomic_flag gate=ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> attempts{},lost{};
    std::array<Event,Batch::Capacity> events{};
    uint32_t count{};
};
static_assert(std::is_trivially_copyable_v<Identity> && std::is_trivially_copyable_v<Event>);
} // namespace Vulkan::Gow3::Provenance
