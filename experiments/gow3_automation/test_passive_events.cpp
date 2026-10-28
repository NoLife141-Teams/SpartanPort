#include "passive_events.h"
#include <cassert>
#include <cstdio>
#include <thread>
#include <limits>

using namespace Gow3Passive;
static Event Health(uint64_t sequence=1) {
    return {.session=1,.generation=1,.producer=1,.source_sequence=sequence,.at_ms=100,
            .actor_token=9,.kind=Kind::Health,.health=20,.maximum=100,.coherent_origin=true};
}

int main() {
    Inbox disabled;
    assert(!disabled.Push(Health()));
    auto batch=disabled.Drain();
    assert(!batch.count && !batch.attempts && !batch.lost && !batch.busy);
    puts("PASS disabled: no publication or counters");

    Inbox bounded(true);
    for(uint64_t i=1;i<=70;++i) bounded.Push(Health(i));
    batch=bounded.Drain();
    assert(batch.count==64 && batch.attempts==70 && batch.lost==6);
    for(uint32_t i=0;i<batch.count;++i) {
        assert(batch.events[i].source_sequence==i+1 && batch.events[i].publication==i+1);
    }
    Prefix<2> prefix;
    assert(prefix.Append(batch.events[0]) && prefix.Append(batch.events[1]));
    assert(!prefix.Append(batch.events[2]) && prefix.count==2 && prefix.lost==1);
    assert(prefix.events[0].source_sequence==1 && prefix.events[1].source_sequence==2);
    puts("PASS queue/prefix saturation: exact losses, retained prefix unchanged");

    Inbox concurrent(true);
    std::atomic<bool> start=false;
    std::atomic<unsigned> done=0;
    constexpr uint64_t count=4000;
    auto produce=[&](uint64_t id) {
        while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for(uint64_t i=1;i<=count;++i) {
            Event e=Health(i); e.producer=id; e.actor_token=id*100000+i;
            e.at_ms=e.actor_token*3;
            e.health=static_cast<float>(i); e.maximum=e.health*2;
            concurrent.Push(e);
        }
        done.fetch_add(1,std::memory_order_release);
    };
    std::thread a(produce,1),b(produce,2);
    start.store(true,std::memory_order_release);
    uint64_t retained=0,last_publication=0;
    auto inspect=[&](const Batch& current) {
        for(uint32_t i=0;i<current.count;++i) {
            const auto& e=current.events[i];
            assert(e.actor_token==e.producer*100000+e.source_sequence);
            assert(e.at_ms==e.actor_token*3);
            assert(e.health==static_cast<float>(e.source_sequence) && e.maximum==e.health*2);
            assert(e.publication==last_publication+1);
            last_publication=e.publication; ++retained;
        }
    };
    while(done.load(std::memory_order_acquire)!=2) {
        inspect(concurrent.Drain()); std::this_thread::yield();
    }
    a.join(); b.join();
    batch=concurrent.Drain(); inspect(batch);
    assert(!batch.busy && batch.attempts==2*count && retained+batch.lost==2*count);
    std::printf("PASS concurrent producers: attempts=%llu retained=%llu lost=%llu, no torn payload\n",
                static_cast<unsigned long long>(batch.attempts),static_cast<unsigned long long>(retained),
                static_cast<unsigned long long>(batch.lost));

    View view(1);
    assert(view.BeginGeneration(1));
    auto e=Health();
    assert(view.Observe(e) && view.HealthAt(100,100)==HealthEvidence::FreshPositiveSample);
    assert(view.HealthAt(200,100)==HealthEvidence::Stale);
    assert(view.HealthAt(99,100)==HealthEvidence::Unknown);
    e.source_sequence=2; e.health=0; e.at_ms=110;
    assert(view.Observe(e) && view.HealthAt(110,100)==HealthEvidence::FreshZeroSample);
    // A zero sample is not an assertion about the game's death/menu state.
    e.source_sequence=3; e.health=std::numeric_limits<float>::quiet_NaN();
    assert(view.Observe(e) && view.HealthAt(110,100)==HealthEvidence::Invalid);
    puts("PASS health freshness, future timestamps, zero samples and invalid numbers");

    assert(view.BeginGeneration(2));
    e=Health(4);
    assert(!view.Observe(e) && view.RejectedGeneration()==1);
    assert(view.HealthAt(120,100)==HealthEvidence::Unknown);
    e.generation=2; e.kind=Kind::DialogClosed;
    assert(view.Observe(e) && view.HealthAt(120,100)==HealthEvidence::Unknown);
    e.kind=Kind::Health; e.at_ms=120;
    assert(view.Observe(e) && view.HealthAt(120,100)==HealthEvidence::FreshPositiveSample);
    e.kind=Kind::RespawnBoundary;
    assert(view.Observe(e) && view.HealthAt(120,100)==HealthEvidence::Unknown);
    assert(view.BeginGeneration(3));
    e=Health(5); e.generation=3; e.at_ms=130; // same opaque token after a synthetic respawn
    assert(view.Observe(e) && view.HealthAt(130,100)==HealthEvidence::FreshPositiveSample);
    e.session=2;
    assert(!view.Observe(e));
    puts("PASS load/respawn invalidation, same token reuse, late generation and foreign session");

    e=Health(6); e.generation=3; e.at_ms=140; e.coherent_origin=false;
    assert(!view.Observe(e) && view.HealthAt(140,100)==HealthEvidence::Unknown);
    View loss(1); loss.BeginGeneration(1); loss.Observe(Health()); loss.Loss(1);
    assert(loss.HealthAt(100,100)==HealthEvidence::Lost);
    loss.BeginGeneration(2); e=Health(); e.generation=2; loss.Observe(e);
    assert(loss.HealthAt(100,100)==HealthEvidence::Lost);
    View unknown(1); unknown.BeginGeneration(1); unknown.Observe(Health());
    e=Health(2); e.kind=Kind::Unknown;
    assert(!unknown.Observe(e) && unknown.HealthAt(100,100)==HealthEvidence::Unknown);
    puts("PASS incoherent sources, losses and unknown states remain unusable");

    // Controlled counterexample for the existing trailing-sequence pattern.
    // All fields are atomics here, avoiding a C++ data race in the test itself.
    std::atomic<unsigned> seq=7,health=20,maximum=100;
    const auto before=seq.load();
    health.store(80); // writer has not yet stored maximum or incremented seq
    const auto h=health.load(),m=maximum.load(),after=seq.load();
    assert(before==after && h==80 && m==100);
    maximum.store(400); seq.fetch_add(1);
    puts("PASS counterexample: equal trailing seq reads can accept mixed old/new fields");
    return 0;
}
