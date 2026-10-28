#include "health_abi_fixture.h"

// Independent consumer policy for interval timestamps. The upper bound alone
// must never make an old pending packet look fresh.
enum class TimeQuality { Unknown, FreshBounded, Stale };
static TimeQuality quality(const Read& sample,uint64_t now,uint64_t ttl) {
    if(!sample.packet.lower || sample.packet.lower>sample.upper || sample.upper>now)
        return TimeQuality::Unknown;
    if(now-sample.packet.lower<=ttl) return TimeQuality::FreshBounded;
    if(now-sample.upper>ttl) return TimeQuality::Stale;
    return TimeQuality::Unknown;
}

int main(int argc,char** argv) {
    assert(argc==2);
    const bool avx=__builtin_cpu_supports("avx");
    for(int i=0;i<16;++i) for(int j=0;j<32;++j) vector_seed[i][j]=static_cast<unsigned char>(i*13+j);
    Region continuation; *static_cast<unsigned char*>(continuation.data)=0xc3; continuation.seal();
    const auto resume=reinterpret_cast<uintptr_t>(continuation.data);
    alignas(64) SpartanActorSlot legacy[2]{};
    Mailbox box; box.session=11; box.generation=1; box.lower=100;
    Region disabled,enabled;
    Stub off(disabled.data,4096,legacy,&box,resume,false),on(enabled.data,4096,legacy,&box,resume,true);
    unsigned char reference[256]; auto n=spartanport_actor_build_stub(reference,reinterpret_cast<uintptr_t>(legacy),resume);
    assert(off.getSize()==n && !std::memcmp(reference,disabled.data,n));
    disabled.seal(); enabled.seal();
    std::ofstream dump(std::string(argv[1])+"/enabled-stub.bin",std::ios::binary);
    assert(dump.write(static_cast<const char*>(enabled.data),on.getSize())); dump.close();
    Region disabled_harness,enabled_harness;
    Harness h0(disabled_harness.data,reinterpret_cast<uintptr_t>(disabled.data),avx);
    Harness h1(enabled_harness.data,reinterpret_cast<uintptr_t>(enabled.data),avx);
    disabled_harness.seal(); enabled_harness.seal();
    const auto invoke_off=reinterpret_cast<Invoke>(disabled_harness.data);
    const auto invoke=reinterpret_cast<Invoke>(enabled_harness.data);
    alignas(32) std::array<unsigned char,0x300> actor{};
    actor_data(actor.data(),20,true);
    const auto original_actor=actor;
    Result result;
    invoke_off(actor.data(),&result); check(result,actor.data(),avx);
    assert(!box.attempts.load() && !box.ready && actor==original_actor);
    puts("PASS disabled stub is byte-identical to existing generator; diagnostics untouched");
    invoke(actor.data(),&result); check(result,actor.data(),avx);
    assert(box.ready && box.attempts==1 && box.packet.health==20 && box.packet.maximum==40);
    assert(box.packet.player==1 && box.packet.lower==100 && box.packet.generation==1 && actor==original_actor);
    const auto first=box.packet;
    actor_data(actor.data(),30,false);
    invoke(actor.data(),&result); check(result,actor.data(),avx);
    assert(box.lost==1 && !std::memcmp(&box.packet,&first,sizeof(first)) && legacy[1].health==30);
    puts("PASS full mailbox drops diagnostics, preserves pending packet and legacy enemy recording");
    Read read{};
    assert(Generation(box,2));
    assert(!Generation(box,2) && !Generation(box,1) && box.generation==2);
    assert(Drain(box,read,[]{return 150;}));
    assert(read.packet.generation==1 && read.packet.lower==100 && read.upper==150);
    assert(quality(read,150,40)==TimeQuality::Unknown);
    assert(quality(read,155,100)==TimeQuality::FreshBounded);
    assert(quality(read,200,40)==TimeQuality::Stale);
    assert(quality(read,149,100)==TimeQuality::Unknown);
    Read invalid=read; invalid.packet.lower=151;
    assert(quality(invalid,200,100)==TimeQuality::Unknown);
    invalid.packet.lower=0;
    assert(quality(invalid,200,100)==TimeQuality::Unknown);
    puts("PASS interval time rejects future/invalid/uninitialized bounds; late drain cannot refresh old data");
    invoke(actor.data(),&result); check(result,actor.data(),avx);
    assert(box.packet.generation==2 && box.packet.lower==150 && box.packet.player==0);
    assert(Drain(box,read,[]{return 180;}));
    assert(read.packet.lower==150 && read.upper==180);
    puts("PASS pending records retain old generation/time; new record gets new diagnostic generation");
    box.gate.store(1);
    assert(!Generation(box,3) && box.generation==2);
    const auto before_lost=box.lost.load();
    actor_data(actor.data(),40,true);
    invoke(actor.data(),&result); check(result,actor.data(),avx);
    assert(box.lost==before_lost+1 && box.gate==1 && !box.ready && legacy[0].health==40);
    box.gate.store(0);
    puts("PASS busy path never waits, changes ownership or skips legacy health recording");
    // Two concurrent producers use different legacy arrays (the unchanged legacy
    // slots are NOT made concurrent-safe by this diagnostic experiment).
    alignas(64) SpartanActorSlot legacy2[2]{};
    Region enabled2,harness2;
    Stub on2(enabled2.data,4096,legacy2,&box,resume,true); enabled2.seal();
    Harness h2(harness2.data,reinterpret_cast<uintptr_t>(enabled2.data),avx); harness2.seal();
    auto invoke2=reinterpret_cast<Invoke>(harness2.data);
    const auto attempts_before=box.attempts.load(),loss_before=box.lost.load();
    std::atomic<bool> start=false; std::atomic<unsigned> done=0;
    constexpr unsigned Count=3000;
    auto producer=[&](Invoke call,unsigned id) {
        alignas(32) std::array<unsigned char,0x300> fake{};
        Result captured;
        while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for(unsigned i=1;i<=Count;++i) {
            actor_data(fake.data(),static_cast<float>(id*10000+i),id==1);
            call(fake.data(),&captured); check(captured,fake.data(),avx);
        }
        done.fetch_add(1,std::memory_order_release);
    };
    std::thread a(producer,invoke,1),b(producer,invoke2,2);
    start.store(true,std::memory_order_release);
    uint64_t kept=0,clock=180,last_publication=box.publications;
    auto consume=[&] {
        if(Drain(box,read,[&]{return ++clock;})) {
            assert(read.packet.maximum==read.packet.health*2 && read.packet.session==11 && read.packet.generation==2);
            assert(read.packet.player==(read.packet.health<20000 ? 1u : 0u));
            assert(read.packet.lower<=read.upper && read.packet.publication==++last_publication);
            ++kept;
        }
    };
    while(done.load(std::memory_order_acquire)!=2) {consume(); std::this_thread::yield();}
    a.join(); b.join(); consume();
    const auto lost=box.lost.load()-loss_before;
    assert(box.attempts.load()-attempts_before==2*Count && kept+lost==2*Count);
    std::printf("PASS concurrent inline producers: %u attempts, %llu kept, %llu lost, no mixed packet\n",2*Count,
                static_cast<unsigned long long>(kept),static_cast<unsigned long long>(lost));
    std::printf("PASS all paths: GPRs, %s, MXCSR, DF, stack and 128-byte SysV red zone preserved; stub=%zu bytes\n",
                avx ? "YMM0-15" : "XMM0-15",on.getSize());
    return 0;
}
