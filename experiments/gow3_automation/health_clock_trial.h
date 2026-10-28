// Experimental host-consumer clock adapter, shared by the opt-in bridge and CPU tests.
#pragma once
#include "health_inline_trial.h"
#include <windows.h>
#include <utility>

namespace HealthTrial {
struct WindowsQpc {
    bool Frequency(int64_t& out) noexcept {
        LARGE_INTEGER value{};
        if(!QueryPerformanceFrequency(&value)) return false;
        out=value.QuadPart; return true;
    }
    bool Read(int64_t& out) noexcept {
        LARGE_INTEGER value{};
        if(!QueryPerformanceCounter(&value)) return false;
        out=value.QuadPart; return true;
    }
};
struct ClockValue { bool valid{}; uint64_t ticks{}; };
// One consumer owns this adapter. Raw QPC ticks avoid rounding and overflow in
// conversion; the immutable frequency accompanies the session metadata.
template<class Source> class CheckedClock {
public:
    explicit CheckedClock(Source& source_,bool enabled_=false) noexcept
        :source(source_),enabled(enabled_) {
        static_assert(noexcept(source.Frequency(std::declval<int64_t&>())));
        static_assert(noexcept(source.Read(std::declval<int64_t&>())));
        int64_t value{};
        if(enabled) {
            if(!source.Frequency(value) || value<=0) fault=true;
            else frequency=static_cast<uint64_t>(value);
        }
    }
    CheckedClock(const CheckedClock&)=delete;
    CheckedClock(CheckedClock&&)=delete;
    ClockValue Sample() noexcept {
        if(!enabled || fault) return {};
        int64_t value{};
        if(!source.Read(value) || value<=0 || static_cast<uint64_t>(value)<last) {
            fault=true; return {};
        }
        last=static_cast<uint64_t>(value);
        return {true,last};
    }
    bool Enabled() const noexcept { return enabled; }
    bool Faulted() const noexcept { return fault; }
    uint64_t Frequency() const noexcept { return frequency; }
private:
    Source& source;
    const bool enabled;
    bool fault{};
    uint64_t frequency{},last{};
};
enum class PollState { Disabled, Busy, Empty, Packet };
struct TimedRead {
    PollState state=PollState::Disabled;
    Read sample{};
    bool time_valid{};
    uint64_t attempts{},lost{}; // live samples, not terminal totals
};

template<class Clock> TimedRead PollTimed(Mailbox& box,Clock& clock) noexcept {
    static_assert(noexcept(clock.Sample()));
    TimedRead out;
    if(!clock.Enabled()) return out;
    uint32_t expected=0;
    if(!box.gate.compare_exchange_strong(expected,1,std::memory_order_acquire)) {
        out.state=PollState::Busy;
    } else {
        const bool present=box.ready!=0;
        if(present) out.sample.packet=box.packet;
        const auto now=clock.Sample(); // only on host; AFTER acquiring the gate
        if(present) {
            out.state=PollState::Packet;
            out.sample.upper=now.ticks;
            out.time_valid=now.valid && out.sample.packet.lower>0 &&
                           out.sample.packet.lower<=now.ticks;
            box.ready=0;
        } else out.state=PollState::Empty;
        // A failed clock invalidates subsequent lower bounds, never synthesizes time.
        box.lower=now.valid ? now.ticks : 0;
        box.gate.store(0,std::memory_order_release);
    }
    out.attempts=box.attempts.load(std::memory_order_relaxed);
    out.lost=box.lost.load(std::memory_order_relaxed);
    return out;
}
enum class BoundedAge { Unknown, FreshInterval, Stale };
// This classifies one recorded observation, never the current game state.
// A future observer must also latch any losses detected after this snapshot.
inline BoundedAge Age(const TimedRead& record,ClockValue now,uint64_t ttl,
                      uint64_t session,uint64_t generation,bool collecting) noexcept {
    const auto& sample=record.sample;
    if(!collecting || !now.valid || record.state!=PollState::Packet || !record.time_valid || record.lost ||
       !ttl || !session || !generation || sample.packet.session!=session ||
       sample.packet.generation!=generation || !sample.packet.lower ||
       sample.packet.lower>sample.upper || sample.upper>now.ticks) return BoundedAge::Unknown;
    if(now.ticks-sample.packet.lower<=ttl) return BoundedAge::FreshInterval;
    if(now.ticks-sample.upper>ttl) return BoundedAge::Stale;
    return BoundedAge::Unknown;
}
} // namespace HealthTrial
