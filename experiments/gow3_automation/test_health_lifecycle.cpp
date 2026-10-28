#include "health_clock_trial.h"
#include <array>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <memory>
#include <semaphore>
#include <thread>
#include <type_traits>
#include <vector>
using namespace HealthTrial;
using namespace std::chrono_literals;

struct FakeSource {
    struct Value { bool ok; int64_t tick; };
    bool frequency_ok=true;
    int64_t frequency=1000;
    std::vector<Value> values;
    size_t reads{},frequency_reads{};
    bool Frequency(int64_t& out) noexcept { ++frequency_reads; out=frequency; return frequency_ok; }
    bool Read(int64_t& out) noexcept {
        assert(reads<values.size()); const auto value=values[reads++];
        out=value.tick; return value.ok;
    }
};
struct Region {
    inline static unsigned allocations{};
    void* address=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    Region(){assert(address); ++allocations;}
    ~Region(){assert(VirtualFree(address,0,MEM_RELEASE));}
    Region(const Region&)=delete;
    Region& operator=(const Region&)=delete;
    void Seal(){DWORD old{}; assert(VirtualProtect(address,4096,PAGE_EXECUTE_READ,&old));
        assert(FlushInstructionCache(GetCurrentProcess(),address,4096));}
};
// Storage may not move. Its identity is immutable and never recycled within
// this synthetic process. No assertion about game scene/respawn is attached.
struct Storage {
    Mailbox mailbox;
    const uint64_t identity;
    std::atomic<bool> collecting=true;
    std::atomic<unsigned>& destructions;
    Storage(uint64_t id,uint64_t lower,std::atomic<unsigned>& count)
        :identity(id),destructions(count) {
        mailbox.session=id; mailbox.generation=1; mailbox.lower=lower;
    }
    ~Storage(){destructions.fetch_add(1);}
    Storage(const Storage&)=delete;
    Storage(Storage&&)=delete;
};
static_assert(!std::is_move_constructible_v<Storage> && !std::is_copy_constructible_v<Storage>);

using Invoke=void (__attribute__((sysv_abi)) *)(void*);
struct Binding {
    // Destroy code first, storage last. Every worker retains its entire binding
    // until it can no longer call the generated entry point.
    const std::shared_ptr<Storage> storage;
    alignas(64) SpartanActorSlot legacy[2]{};
    Region continuation,code,entry;
    Invoke invoke{};
    explicit Binding(std::shared_ptr<Storage> state):storage(std::move(state)) {
        assert(storage && storage->collecting);
        *static_cast<unsigned char*>(continuation.address)=0xc3; continuation.Seal();
        Stub stub(code.address,4096,legacy,&storage->mailbox,
                  reinterpret_cast<uintptr_t>(continuation.address),true);
        assert(stub.getSize()<4096); code.Seal();
        Xbyak::CodeGenerator wrapper(4096,entry.address);
        using namespace Xbyak::util;
        Xbyak::Label target;
        wrapper.push(r13); wrapper.mov(r13,rdi); wrapper.call(ptr[rip+target]);
        wrapper.pop(r13); wrapper.ret(); wrapper.L(target);
        wrapper.dq(reinterpret_cast<uintptr_t>(code.address)); wrapper.ready(); entry.Seal();
        invoke=reinterpret_cast<Invoke>(entry.address);
    }
};
// Only this test controller creates bindings. Stop closes creation and drops
// its reference; it DOES NOT unpatch code, zero a mailbox or reclaim workers'
// bindings. Real guest threads do not possess these C++ leases: integration
// must retain storage for process lifetime or establish equivalent quiescence.
class Owner {
public:
    Owner()=default;
    Owner(const Owner&)=delete;
    Owner(Owner&&)=delete;
    template<class Clock> bool Start(Clock& clock,std::atomic<unsigned>& destructions) {
        if(attempted) return false;
        attempted=true;
        const auto now=clock.Sample();
        if(!now.valid) return false;
        const auto id=NextId(ids);
        if(!id) return false;
        storage=std::make_shared<Storage>(id,now.ticks,destructions);
        return true;
    }
    std::shared_ptr<Binding> Bind() {
        if(!storage || !storage->collecting) return {};
        return std::make_shared<Binding>(storage);
    }
    void Stop() noexcept {
        if(storage) {storage->collecting.store(false,std::memory_order_release); storage.reset();}
    }
    std::weak_ptr<Storage> Observe() const {return storage;}
    ~Owner(){Stop();}
    static uint64_t NextId(std::atomic<uint64_t>& counter) noexcept {
        auto previous=counter.load(std::memory_order_relaxed);
        if(previous==UINT64_MAX) return 0;
        // Fail closed on competing issuance rather than retry or wrap.
        if(!counter.compare_exchange_strong(previous,previous+1,std::memory_order_relaxed)) return 0;
        return previous+1;
    }
private:
    inline static std::atomic<uint64_t> ids{100}; // one source across clock adapter types
    bool attempted{};
    std::shared_ptr<Storage> storage;
};
static_assert(!std::is_copy_constructible_v<Owner> && !std::is_move_constructible_v<Owner>);
static_assert(!std::is_copy_constructible_v<CheckedClock<WindowsQpc>>);
static_assert(!std::is_move_constructible_v<CheckedClock<WindowsQpc>>);
static std::array<unsigned char,0x300> Actor(float health) {
    std::array<unsigned char,0x300> actor{};
    const uint64_t vtable=0x1234567812345678ULL;
    const float maximum=health*2;
    std::memcpy(actor.data(),&vtable,8);
    std::memcpy(actor.data()+0x1cc,&health,4);
    std::memcpy(actor.data()+0x1d0,&maximum,4);
    return actor;
}

static void ClockTests() {
    FakeSource disabled;
    CheckedClock off(disabled);
    Mailbox untouched;
    assert(PollTimed(untouched,off).state==PollState::Disabled && !off.Sample().valid);
    assert(!disabled.reads && !disabled.frequency_reads && !untouched.gate && !untouched.attempts);
    for(int mode=0;mode<3;++mode) {
        FakeSource source; source.frequency_ok=mode!=0; source.frequency=mode==1 ? 0 : -1;
        CheckedClock clock(source,true);
        assert(clock.Faulted() && !clock.Sample().valid && !source.reads);
    }
    FakeSource ordered; ordered.values={{true,100},{true,100},{true,99},{true,150}};
    CheckedClock clock(ordered,true);
    assert(clock.Sample().ticks==100 && clock.Sample().ticks==100);
    assert(!clock.Sample().valid && clock.Faulted() && !clock.Sample().valid && ordered.reads==3);
    for(const FakeSource::Value bad: {FakeSource::Value{false,120},{true,0},{true,-1}}) {
        FakeSource source; source.values={{true,100},bad,{true,200}};
        CheckedClock checked(source,true);
        Mailbox box; box.session=1; box.generation=1;
        assert(PollTimed(box,checked).state==PollState::Empty && box.lower==100);
        box.packet.session=1; box.packet.generation=1; box.packet.lower=100; box.ready=1;
        const auto value=PollTimed(box,checked);
        assert(value.state==PollState::Packet && !value.time_valid && checked.Faulted());
        assert(!box.gate && !box.ready && !box.lower && value.sample.packet.lower==100);
        assert(Age(value,checked.Sample(),100,1,1,true)==BoundedAge::Unknown);
        assert(PollTimed(box,checked).state==PollState::Empty && source.reads==2);
    }
    FakeSource busy; CheckedClock busy_clock(busy,true);
    Mailbox locked; locked.gate=1;
    assert(PollTimed(locked,busy_clock).state==PollState::Busy && !busy.reads && locked.gate==1);
    locked.gate=0;
    TimedRead r; r.state=PollState::Packet; r.time_valid=true;
    r.sample.packet={}; r.sample.packet.lower=100; r.sample.upper=150;
    r.sample.packet.session=7; r.sample.packet.generation=2;
    assert(Age(r,{true,150},40,7,2,true)==BoundedAge::Unknown);
    assert(Age(r,{true,150},50,7,2,true)==BoundedAge::FreshInterval);
    assert(Age(r,{true,201},50,7,2,true)==BoundedAge::Stale);
    assert(Age(r,{true,149},50,7,2,true)==BoundedAge::Unknown);
    assert(Age(r,{true,150},50,8,2,true)==BoundedAge::Unknown);
    assert(Age(r,{true,150},50,7,3,true)==BoundedAge::Unknown);
    assert(Age(r,{true,150},50,7,2,false)==BoundedAge::Unknown);
    assert(Age(r,clock.Sample(),50,7,2,true)==BoundedAge::Unknown); // old packet after clock fault
    r.lost=1; assert(Age(r,{true,150},50,7,2,true)==BoundedAge::Unknown);
    puts("PASS clock: disabled, bad frequency, failure, zero/negative/regressing time, sticky fault, busy gate, conservative age/session/generation/loss");
}

static void StartupAndBoundTests() {
    std::atomic<unsigned> destructions{};
    std::atomic<uint64_t> limit{UINT64_MAX-1};
    assert(Owner::NextId(limit)==UINT64_MAX && !Owner::NextId(limit) && limit==UINT64_MAX);
    FakeSource bad; bad.frequency_ok=false;
    CheckedClock bad_clock(bad,true);
    Owner failed; const auto allocations=Region::allocations;
    assert(!failed.Start(bad_clock,destructions) && !failed.Bind());
    assert(Region::allocations==allocations && failed.Observe().expired());
    WindowsQpc raw; CheckedClock clock(raw,true);
    Owner owner;
    assert(!owner.Bind() && owner.Start(clock,destructions) && !owner.Start(clock,destructions));
    auto binding=owner.Bind(); const auto original=&binding->storage->mailbox;
    auto actor=Actor(10);
    int64_t before{},after{};
    assert(raw.Read(before)); binding->invoke(actor.data()); assert(raw.Read(after));
    auto sample=PollTimed(*original,clock);
    assert(sample.state==PollState::Packet && sample.time_valid);
    assert(sample.sample.packet.lower<=static_cast<uint64_t>(before));
    assert(before<=after && static_cast<uint64_t>(after)<=sample.sample.upper);
    assert(sample.sample.packet.health==10 && sample.sample.packet.maximum==20);
    binding->invoke(actor.data());
    assert(Generation(*original,2));
    sample=PollTimed(*original,clock);
    assert(sample.sample.packet.generation==1 && sample.time_valid);
    assert(Age(sample,clock.Sample(),clock.Frequency(),original->session,2,true)==BoundedAge::Unknown);
    binding->invoke(actor.data()); sample=PollTimed(*original,clock);
    assert(sample.sample.packet.generation==2 && &binding->storage->mailbox==original);
    const auto weak=owner.Observe(); owner.Stop();
    assert(!owner.Bind() && !owner.Start(clock,destructions) && !weak.expired() && !destructions);
    // A raw entry can still run after collection closes: retain its storage/code.
    binding->invoke(actor.data()); sample=PollTimed(*original,clock);
    assert(sample.state==PollState::Packet);
    assert(Age(sample,clock.Sample(),clock.Frequency(),original->session,2,
               binding->storage->collecting.load())==BoundedAge::Unknown);
    binding.reset(); assert(weak.expired() && destructions==1);
    auto last=clock.Sample();
    for(int i=0;i<1000;++i) {auto next=clock.Sample(); assert(next.valid && next.ticks>=last.ticks); last=next;}
    std::printf("PASS actual Windows QPC: frequency=%llu ticks/s; event bracketed by host bounds; 1000 monotone reads\n",
                static_cast<unsigned long long>(clock.Frequency()));
    puts("PASS startup requires clock before code exposure; no restart/relabel; pending old generation retained; stop cannot authorize samples");
}

static void ActiveShutdownTests() {
    std::atomic<unsigned> destructions{};
    WindowsQpc source; CheckedClock clock(source,true);
    Owner old; assert(old.Start(clock,destructions));
    const auto old_weak=old.Observe();
    auto first=old.Bind(),second=old.Bind();
    auto* const old_address=&first->storage->mailbox;
    const auto old_id=first->storage->identity;
    std::counting_semaphore<2> at_boundary(0),after_stop(0),release_workers(0);
    std::atomic<unsigned> done{};
    constexpr unsigned LateCalls=2000;
    auto worker=[&](std::shared_ptr<Binding> binding,unsigned id) {
        auto actor=Actor(static_cast<float>(id));
        binding->invoke(actor.data());
        at_boundary.release(); assert(after_stop.try_acquire_for(2s));
        for(unsigned i=0;i<LateCalls;++i) {
            actor=Actor(static_cast<float>(id*10000+i)); binding->invoke(actor.data());
        }
        done.fetch_add(1,std::memory_order_release);
        assert(release_workers.try_acquire_for(2s));
    };
    std::thread a(worker,first,1),b(worker,second,2);
    assert(at_boundary.try_acquire_for(2s) && at_boundary.try_acquire_for(2s));
    uint64_t kept=0;
    if(PollTimed(*old_address,clock).state==PollState::Packet) ++kept;
    assert(old_address->gate==0); // nevertheless both workers have future calls!
    old.Stop(); first.reset(); second.reset();
    assert(!old.Bind() && !old_weak.expired() && !destructions);
    Owner next; assert(next.Start(clock,destructions));
    auto fresh=next.Bind();
    assert(&fresh->storage->mailbox!=old_address && fresh->storage->identity!=old_id);
    assert(fresh->storage->mailbox.generation==1 && !fresh->storage->mailbox.attempts);
    after_stop.release(2);
    const auto deadline=std::chrono::steady_clock::now()+2s;
    auto consume=[&] {
        const auto retained=old_weak.lock(); assert(retained && &retained->mailbox==old_address);
        const auto value=PollTimed(retained->mailbox,clock);
        if(value.state==PollState::Packet) {
            ++kept;
            assert(value.sample.packet.session==old_id && value.sample.packet.generation==1);
            assert(value.sample.packet.maximum==2*value.sample.packet.health);
            assert(Age(value,clock.Sample(),clock.Frequency(),old_id,1,
                       retained->collecting.load())==BoundedAge::Unknown);
            assert(Age(value,clock.Sample(),clock.Frequency(),fresh->storage->identity,1,true)==BoundedAge::Unknown);
        }
    };
    while(done.load(std::memory_order_acquire)!=2) {
        assert(std::chrono::steady_clock::now()<deadline); consume(); std::this_thread::yield();
    }
    consume();
    const auto attempts=old_address->attempts.load(),lost=old_address->lost.load();
    assert(attempts==2*(LateCalls+1) && kept+lost==attempts && !destructions);
    assert(!fresh->storage->mailbox.attempts); // no address/session aliasing
    release_workers.release(2); a.join(); b.join();
    assert(old_weak.expired() && destructions==1);
    next.Stop(); assert(destructions==1); fresh.reset(); assert(destructions==2);
    std::printf("PASS active shutdown: %llu attempts (%u after stop), %llu retained, %llu lost; old storage kept until workers release/join; new session isolated\n",
        static_cast<unsigned long long>(attempts),2*LateCalls,
        static_cast<unsigned long long>(kept),static_cast<unsigned long long>(lost));
}
static void AlreadyRunningWorkerStartup() {
    std::atomic<unsigned> destructions{};
    FakeSource source; source.values={{true,100},{true,120}};
    CheckedClock clock(source,true);
    Owner owner;
    std::binary_semaphore waiting(0),published(0);
    std::shared_ptr<Binding> binding;
    std::thread worker([&] {
        waiting.release();
        assert(published.try_acquire_for(2s));
        // Acquire/release handoff: initialized storage and executable pages
        // precede the first entry, even though this worker already existed.
        const auto retained=binding;
        assert(retained && retained->storage->mailbox.session &&
               retained->storage->mailbox.generation==1 && retained->storage->mailbox.lower==100);
        auto actor=Actor(33); retained->invoke(actor.data());
    });
    assert(waiting.try_acquire_for(2s) && !owner.Bind());
    assert(owner.Start(clock,destructions)); binding=owner.Bind();
    published.release(); worker.join();
    const auto captured=PollTimed(binding->storage->mailbox,clock);
    assert(captured.time_valid && captured.sample.packet.health==33);
    assert(captured.sample.packet.lower==100 && captured.sample.upper==120);
    WindowsQpc raw; CheckedClock other_clock(raw,true);
    Owner other; assert(other.Start(other_clock,destructions));
    auto other_binding=other.Bind();
    assert(other_binding->storage->identity!=binding->storage->identity);
    owner.Stop(); binding.reset(); other.Stop(); other_binding.reset();
    assert(destructions==2);
    puts("PASS worker exists before startup: release/acquire publication only after initialization; IDs unique across clock adapter types");
}
int main() {
    ClockTests(); StartupAndBoundTests(); AlreadyRunningWorkerStartup(); ActiveShutdownTests();
    puts("PASS no runtime integration, game execution, input, save access or guest pointers outside fake actors");
}
