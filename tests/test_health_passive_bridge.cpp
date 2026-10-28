// Uses the actual integrated bridge, but no SDL, GPU, guest image or game.
#include "../experiments/gow3_automation/health_abi_fixture.h"
#include "../src/health_passive_bridge.h"
#include <chrono>
#include <filesystem>
#include <memory>
#include <semaphore>
#include <string>
using namespace std::chrono_literals;
namespace fs=std::filesystem;
static std::string Bytes(const fs::path& p) {
    std::ifstream f(p,std::ios::binary); assert(f);
    return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
struct WindowOwner {
    std::binary_semaphore ready{0},request{0},reply{0};
    std::atomic<unsigned> command{};
    std::atomic<int> started{};
    std::thread thread;
    WindowOwner():thread([this] {
        started.store(spartanport_health_passive_start()); ready.release();
        for(;;) {
            assert(request.try_acquire_for(5s));
            const unsigned c=command.load();
            if(c==1) spartanport_health_passive_tick();
            else spartanport_health_passive_stop();
            reply.release(); if(c==2) return;
        }
    }) {assert(ready.try_acquire_for(5s));}
    void Do(unsigned c) {
        command.store(c); request.release(); assert(reply.try_acquire_for(5s));
        if(c==2) thread.join();
    }
    ~WindowOwner(){if(thread.joinable()) Do(2);}
};
struct Fixture {
    Region continuation,code,harness;
    alignas(64) SpartanActorSlot legacy[2]{};
    Mailbox* mailbox{};
    Invoke invoke{};
    const bool avx=__builtin_cpu_supports("avx");
    explicit Fixture(bool diagnostic) {
        *static_cast<unsigned char*>(continuation.data)=0xc3; continuation.seal();
        const auto slots=reinterpret_cast<uintptr_t>(legacy),resume=reinterpret_cast<uintptr_t>(continuation.data);
        const unsigned n=spartanport_health_passive_build(static_cast<unsigned char*>(code.data),4096,slots,resume);
        assert(n);
        if(diagnostic) {
            assert(n==392);
            const auto* bytes=static_cast<const unsigned char*>(code.data);
            assert(bytes[0]==0x48 && bytes[1]==0xbf);
            // This is the host mailbox immediate in OUR generated test code.
            // No guest memory or recorded actor pointer is dereferenced here.
            std::memcpy(&mailbox,bytes+2,sizeof(mailbox));
            Region reference;
            Stub expected(reference.data,4096,legacy,mailbox,resume,true);
            assert(expected.getSize()==n && !std::memcmp(reference.data,code.data,n));
        } else {
            unsigned char expected[256];
            const auto size=spartanport_actor_build_stub(expected,slots,resume);
            assert(size==n && !std::memcmp(expected,code.data,n));
        }
        code.seal();
        Harness wrapper(harness.data,reinterpret_cast<uintptr_t>(code.data),avx); harness.seal();
        invoke=reinterpret_cast<Invoke>(harness.data);
    }
    void Hit(float health,bool player) {
        alignas(32) std::array<unsigned char,0x300> actor{};
        actor_data(actor.data(),health,player); const auto original=actor;
        Result result; invoke(actor.data(),&result); check(result,actor.data(),avx);
        assert(actor==original);
    }
};
int main(int argc,char** argv) {
    assert(argc==3);
    for(int i=0;i<16;++i) for(int j=0;j<32;++j) vector_seed[i][j]=static_cast<unsigned char>(i*13+j);
    const std::string mode=argv[1];
    const fs::path output=fs::absolute(argv[2]);
    const auto log=output/L"health.jsonl";
    assert(fs::is_directory(output));
    assert(!_wputenv_s(L"SPARTAN_HEALTH_PASSIVE",mode=="off" ? L"" : mode=="zero" ? L"0" : mode=="truthy" ? L"true" : L"1"));
    assert(!_wputenv_s(L"SPARTAN_HEALTH_PASSIVE_LOG",mode=="relative" ? L"relative-health.jsonl" :
        mode=="unc" ? L"\\\\unresolved.invalid\\share\\health.jsonl" : log.c_str()));
    if(mode=="existing") {std::ofstream existing(log,std::ios::binary); existing<<"preserved-marker\n";}
    // Before owner initialization, even an opt-in environment exposes no new producer.
    {Fixture before(false); before.Hit(15,true);}
    WindowOwner owner;
    const bool enabled=mode=="on" || mode=="wrong-owner" || mode=="unavailable" || mode=="small-buffer";
    assert(bool(owner.started.load())==enabled);
    if(!enabled) {
        Fixture f(false); f.Hit(20,true); f.Hit(21,false); owner.Do(2);
        if(mode=="existing") assert(Bytes(log)=="preserved-marker\n");
        else assert(!fs::exists(log));
        puts("PASS inactive bridge: exact legacy bytes and ABI; no journal created/overwritten"); return 0;
    }
    auto p=std::make_shared<Fixture>(true),q=std::make_shared<Fixture>(true);
    assert(p->mailbox==q->mailbox);
    if(mode=="small-buffer") {
        std::array<unsigned char,256> out{}; out.fill(0xa5);
        unsigned char legacy[256];
        const auto n=spartanport_actor_build_stub(legacy,123,456);
        assert(spartanport_health_passive_build(out.data(),n,123,456)==n);
        assert(!std::memcmp(out.data(),legacy,n));
        for(size_t i=n;i<out.size();++i) assert(out[i]==0xa5);
        Fixture pending_abort(false); owner.Do(1); owner.Do(2);
        assert(Bytes(log).find("\"reason_mask\":4")!=std::string::npos);
        puts("PASS insufficient stub buffer: legacy fallback, untouched canary, owner records failure"); return 0;
    }
    if(mode=="wrong-owner" || mode=="unavailable") {
        if(mode=="wrong-owner") spartanport_health_passive_tick();
        else spartanport_health_passive_unavailable(1);
        Fixture pending_abort(false); owner.Do(1); owner.Do(2);
        const auto text=Bytes(log);
        assert(text.find(mode=="wrong-owner" ? "\"reason_mask\":8" : "\"reason_mask\":1")!=std::string::npos);
        Fixture stopped(false); p->Hit(20,true); // old code/storage still safe after close
        assert(Bytes(log)==text);
        puts("PASS uncertain source/foreign owner: no file access on foreign thread; owner closes; old producer remains memory-safe"); return 0;
    }
    p->Hit(20,true); q->Hit(21,false); // full mailbox
    assert(p->mailbox->lost==1);
    p->mailbox->gate=1;
    q->Hit(22,false); assert(p->mailbox->gate==1 && p->mailbox->lost==2);
    p->mailbox->gate=0; owner.Do(1);
    q->Hit(23,false); owner.Do(1);
    std::atomic<unsigned> done{};
    auto producer=[&](std::shared_ptr<Fixture> f,unsigned id) {
        for(unsigned i=0;i<1000;++i) f->Hit(static_cast<float>(id*10000+i),id==1);
        done.fetch_add(1,std::memory_order_release);
    };
    std::thread a(producer,p,1),b(producer,q,2);
    const auto deadline=std::chrono::steady_clock::now()+5s;
    while(done.load(std::memory_order_acquire)!=2) {
        assert(std::chrono::steady_clock::now()<deadline); owner.Do(1);
    }
    a.join(); b.join(); owner.Do(1); owner.Do(2);
    const auto text=Bytes(log);
    assert(text.find("\"type\":\"stop\"")!=std::string::npos && text.find("\"loss_latched\":true")!=std::string::npos);
    assert(text.find("\"player_marker\":0")!=std::string::npos && text.find("\"player_marker\":1")!=std::string::npos);
    assert(fs::file_size(log)<=16384);
    Fixture after(false); const auto previous=p->mailbox->attempts.load();
    p->Hit(99,true); assert(p->mailbox->attempts==previous+1 && Bytes(log)==text);
    assert(p->mailbox->publications+p->mailbox->lost.load()==p->mailbox->attempts.load());
    std::printf("PASS integrated bridge: byte-equal generator, GPR/YMM/MXCSR/DF/stack ABI, busy/full paths, %llu attempts, %llu losses; owner-only close and post-stop storage\n",
        static_cast<unsigned long long>(p->mailbox->attempts.load()),static_cast<unsigned long long>(p->mailbox->lost.load()));
}
