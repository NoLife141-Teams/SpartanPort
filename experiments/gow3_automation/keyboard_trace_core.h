#pragma once
#include "../../src/keyboard_trace_bridge.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace KeyTrace {
enum Reason : unsigned { None, Closed, Expired, Saturated, ClockFault, IoFault, OwnerFault };
enum Kind : unsigned { Context=1, S=2, Read=3 };
struct Record {
    uint64_t serial{}, first{}, last{}, begin{}, end{}, context0{}, context1{}, pump{}, count{1}, max_gap{};
    uint32_t kind{}, flags{};
    uint8_t pad_y{}, host_y{}, final_y{};
};
struct Counters { uint64_t reads{}, offers{}, queued{}, coalesced{}, busy{}, full{}, closed{}, expired{}, redacted{}; };
inline bool Permitted(uint64_t context) { return (context & 7)==KT_FOCUS; }
// Fixed process-lifetime queue. Producers never wait, allocate, perform I/O or touch SDL.
class Core {
public:
    static constexpr size_t Capacity=128;
    bool Start(uint64_t now,uint64_t frequency,uint32_t window,uint32_t flags) {
        if(started || !now || !frequency || !window || frequency>(UINT64_MAX-now)/10) return false;
        started=true; start=now; deadline=now+10*frequency; target=window;
        context.store((uint64_t(1)<<3)|(flags&7),std::memory_order_release);
        active.store(true,std::memory_order_release);
        Observe(now,flags,0,true); return true;
    }
    bool Active() const {return active.load(std::memory_order_acquire);}
    uint64_t Deadline() const {return deadline;}
    uint32_t Window() const {return target;}
    unsigned StopReason() const {return reason.load();}
    void RequestStop(Reason why) {unsigned expected=None; reason.compare_exchange_strong(expected,why); active.store(false,std::memory_order_release);}
    bool Alive(uint64_t now) {
        if(!Active()) return false;
        if(!now || now<start) {RequestStop(ClockFault); return false;}
        if(now>=deadline) {++expired; RequestStop(Expired); return false;}
        return true;
    }
    void Observe(uint64_t now,uint32_t flags,uint64_t pump,bool force=false) {
        if(!Alive(now)) return;
        const auto old=context.load(std::memory_order_acquire);
        if(!force && (old&7)==(flags&7)) return;
        const auto next=((old>>3)+1)*8+(flags&7);
        context.store(next,std::memory_order_release);
        Record r; r.kind=Context; r.begin=r.end=now; r.context0=old; r.context1=next; r.pump=pump;
        Offer(r);
    }
    void Key(uint64_t now,uint32_t window,uint32_t flags,uint64_t pump,bool down,bool repeat) {
        if(!Alive(now) || window!=target || (flags&7)!=KT_FOCUS) return;
        Observe(now,flags,pump);
        Record r; r.kind=S; r.begin=r.end=now; r.context0=r.context1=context.load(); r.pump=pump;
        r.flags=(down?1u:0u)|(repeat?2u:0u); Offer(r);
    }
    SpartanKeyTraceRead Begin(uint64_t now) {
        if(!Alive(now)) return {};
        return {++reads,now,context.load(std::memory_order_acquire),0,0,0,0};
    }
    void End(SpartanKeyTraceRead t,uint64_t now) {
        if(!t.id) return;
        if(now<t.begin) {RequestStop(ClockFault); return;}
        if(!Alive(now)) {++closed; return;}
        Record r; r.kind=Read; r.first=r.last=t.id; r.begin=t.begin; r.end=now;
        r.context0=t.context; r.context1=context.load(std::memory_order_acquire); r.flags=t.flags&2047;
        r.pad_y=(r.flags&KT_PAD_Y_VALID)?t.pad_y:0;
        r.host_y=(r.flags&KT_HOST_Y_VALID)?t.host_y:0;
        r.final_y=(r.flags&KT_FINAL_Y_VALID)?t.final_y:0;
        // No S or axis values across focus/capture/text changes or hidden input.
        if(r.context0!=r.context1 || !Permitted(r.context0) || !Permitted(r.context1) ||
           (r.flags&(KT_HOST_BLOCK|KT_GATE_BLOCK))) {
            r.flags&=KT_HOST_BLOCK|KT_GATE_BLOCK|KT_HOLD_BEFORE|KT_HOLD_AFTER; ++redacted;
            r.pad_y=r.host_y=r.final_y=0;
        }
        Offer(r);
    }
    size_t Drain(std::span<Record> out,bool& contended) {
        contended=gate.test_and_set(std::memory_order_acquire);
        if(contended) return 0;
        const auto n=count<out.size()?count:out.size();
        for(size_t i=0;i<n;++i) out[i]=queue[(head+i)%Capacity];
        head=(head+n)%Capacity; count-=n; gate.clear(std::memory_order_release); return n;
    }
    Counters Counts() const {return {reads.load(),offers.load(),queued.load(),coalesced.load(),busy.load(),full.load(),closed.load(),expired.load(),redacted.load()};}
private:
    void Offer(Record r) {
        ++offers;
        if(gate.test_and_set(std::memory_order_acquire)) {++busy; return;}
        if(!Active()) {++closed; gate.clear(std::memory_order_release); return;}
        if(count && r.kind==Read) {
            auto& tail=queue[(head+count-1)%Capacity];
            if(tail.kind==Read && tail.flags==r.flags && tail.context0==r.context0 && tail.context1==r.context1 &&
               tail.pad_y==r.pad_y && tail.host_y==r.host_y && tail.final_y==r.final_y &&
               r.first==tail.last+1 && r.begin>=tail.end) {
                const auto gap=r.begin-tail.end; if(gap>tail.max_gap) tail.max_gap=gap;
                tail.last=r.last; tail.end=r.end; ++tail.count; ++coalesced;
                gate.clear(std::memory_order_release); return;
            }
        }
        if(count==Capacity) {++full; RequestStop(Saturated); gate.clear(std::memory_order_release); return;}
        r.serial=++queued; queue[(head+count)%Capacity]=r; ++count;
        gate.clear(std::memory_order_release);
    }
    std::atomic<bool> active{};
    std::atomic<unsigned> reason{};
    std::atomic<uint64_t> context{},reads{},offers{},queued{},coalesced{},busy{},full{},closed{},expired{},redacted{};
    std::atomic_flag gate=ATOMIC_FLAG_INIT;
    std::array<Record,Capacity> queue{};
    size_t head{},count{};
    bool started{};
    uint64_t start{},deadline{};
    uint32_t target{};
};
static_assert(std::is_trivially_destructible_v<Core>);
static_assert(std::atomic<uint64_t>::is_always_lock_free);
}
