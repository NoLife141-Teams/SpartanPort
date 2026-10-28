// Offline prototype only. Not linked to the runtime or a generated hook.
#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <type_traits>

namespace Gow3Passive {
enum class Kind : uint32_t {
    Unknown, DialogOpened, DialogChosen, DialogClosed, InputBlocked,
    Focus, QteHeartbeat, Health, LoadBoundary, RespawnBoundary
};
// Values copied by a producer. No pointers, strings, guest reads or ownership.
// An epoch is an explicit diagnostic generation, NOT proof of a game scene.
struct Event {
    uint64_t session{}, generation{}, producer{}, source_sequence{}, at_ms{};
    uint64_t actor_token{}, attempt{}, publication{};
    Kind kind=Kind::Unknown;
    uint32_t mode{}, result{}, flags{};
    float health{}, maximum{};
    bool coherent_origin{}; // adapter contract, not inferred from a raw memcpy
};
static_assert(std::is_trivially_copyable_v<Event>);

struct Batch {
    static constexpr uint32_t Capacity=64;
    std::array<Event,Capacity> events{};
    uint32_t count{};
    uint64_t attempts{}, lost{};
    bool busy{};
};

// Bounded multi-producer inbox: no spinning, allocation, callbacks or I/O.
// Only a complete by-value Event is published. A busy/full queue drops it.
// The gate synchronizes the payload; a trailing counter alone would not.
class Inbox {
public:
    explicit Inbox(bool requested=false) : enabled(requested) {}
    bool Push(Event event) noexcept {
        if(!enabled) return false;
        event.attempt=attempts.fetch_add(1,std::memory_order_relaxed)+1;
        if(gate.test_and_set(std::memory_order_acquire)) {
            lost.fetch_add(1,std::memory_order_relaxed); return false;
        }
        bool retained=count<Batch::Capacity;
        if(retained) {
            event.publication=++publications;
            events[count++]=event;
        } else lost.fetch_add(1,std::memory_order_relaxed);
        gate.clear(std::memory_order_release);
        return retained;
    }
    Batch Drain() noexcept {
        Batch batch;
        if(!enabled) return batch;
        if(gate.test_and_set(std::memory_order_acquire)) batch.busy=true;
        else {
            batch.count=count;
            for(uint32_t i=0;i<count;++i) batch.events[i]=events[i];
            count=0;
            gate.clear(std::memory_order_release);
        }
        // Concurrent sampling, not a terminal completeness certificate.
        // The exact retained+lost accounting is checked after producers stop.
        batch.attempts=attempts.load(std::memory_order_relaxed);
        batch.lost=lost.load(std::memory_order_relaxed);
        return batch;
    }
private:
    const bool enabled;
    std::atomic_flag gate=ATOMIC_FLAG_INIT;
    std::atomic<uint64_t> attempts{},lost{};
    uint64_t publications{};
    std::array<Event,Batch::Capacity> events{};
    uint32_t count{};
};

// Testable retained prefix; future file serializer must additionally enforce
// byte limits and exclusive creation. No overwrite/wrap of retained events.
template<size_t Capacity=4096> class Prefix {
public:
    bool Append(const Event& event) noexcept {
        if(count==Capacity) { ++lost; return false; }
        events[count++]=event; return true;
    }
    std::array<Event,Capacity> events{};
    size_t count{};
    uint64_t lost{};
};

enum class HealthEvidence {
    Unknown, FreshPositiveSample, FreshZeroSample, Stale, Invalid, Lost
};
// Consumer-only interpretation. There is intentionally no Alive/Playing
// state and no API that authorizes or injects input.
class View {
public:
    explicit View(uint64_t session_) : session(session_) {}
    bool BeginGeneration(uint64_t generation_) {
        if(!generation_ || generation_<=generation) { unknown=true; return false; }
        generation=generation_; have_health=false; unknown=false;
        // Loss remains latched across loads/respawns in this same session.
        return true;
    }
    void Loss(uint64_t total) { if(total) loss=true; }
    bool Observe(const Event& event) {
        if(event.session!=session || !generation || event.generation!=generation) {
            ++rejected_generation; return false;
        }
        if(event.kind==Kind::Unknown) { unknown=true; return false; }
        if(event.kind==Kind::LoadBoundary || event.kind==Kind::RespawnBoundary) {
            have_health=false; unknown=true; return true;
        }
        if(event.kind!=Kind::Health) return true;
        if(!event.coherent_origin || !event.producer || !event.source_sequence) {
            unknown=true; have_health=false; return false;
        }
        if(have_health && (event.at_ms<health.at_ms ||
             (event.producer==health.producer && event.source_sequence<=health.source_sequence))) {
            unknown=true; have_health=false; return false;
        }
        health=event; have_health=true;
        return true;
    }
    HealthEvidence HealthAt(uint64_t now, uint64_t ttl) const {
        if(loss) return HealthEvidence::Lost;
        if(unknown || !have_health || !ttl || now<health.at_ms) return HealthEvidence::Unknown;
        if(now-health.at_ms>=ttl) return HealthEvidence::Stale;
        if(!std::isfinite(health.health) || !std::isfinite(health.maximum) ||
           health.maximum<=0 || health.maximum>=1e7f || health.health<0 ||
           health.health>health.maximum*1.001f) return HealthEvidence::Invalid;
        return health.health==0 ? HealthEvidence::FreshZeroSample : HealthEvidence::FreshPositiveSample;
    }
    uint64_t RejectedGeneration() const { return rejected_generation; }
private:
    const uint64_t session;
    uint64_t generation{},rejected_generation{};
    bool have_health{},unknown{},loss{};
    Event health{};
};
} // namespace Gow3Passive
