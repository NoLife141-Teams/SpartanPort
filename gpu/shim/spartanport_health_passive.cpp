// Experimental opt-in integration. The guest producer contains no host call.
#include "../../src/health_passive_bridge.h"
#include "../../src/actor_hook.h"
#ifdef _WIN32
#include "../../experiments/gow3_automation/health_collection_trial.h"
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <new>

namespace {
using Consumer=HealthCollection::Collector<HealthTrial::WindowsQpc>;
enum class State : unsigned { Cold, Disabled, Starting, Active, Stopping, Stopped, Failed };
// Reason bits: 1 signature, 2 relative range, 4 stub generation,
// 8 consumer ownership, 16 clock fault, 32 journal I/O failure.
std::atomic<State> state{State::Cold};
std::atomic<DWORD> owner{};
std::atomic<HealthTrial::Mailbox*> producer_box{};
std::atomic<unsigned> stop_reasons{};
HealthTrial::WindowsQpc clock_source;
alignas(Consumer) std::byte consumer_storage[sizeof(Consumer)];
Consumer* consumer; // only the owner thread ever dereferences this pointer

bool OnOwner() noexcept {
    if(owner.load(std::memory_order_acquire)==GetCurrentThreadId()) return true;
    stop_reasons.fetch_or(8,std::memory_order_release); return false;
}
void CloseOnOwner() noexcept {
    state.store(State::Stopping,std::memory_order_release);
    producer_box.store(nullptr,std::memory_order_release);
    consumer->Stop();
    state.store(State::Stopped,std::memory_order_release);
    // Do not destroy the consumer or reclaim any embedded/published storage.
}
}

extern "C" int spartanport_health_passive_start(void) {
    State expected=State::Cold;
    if(!state.compare_exchange_strong(expected,State::Starting)) return 0;
    const wchar_t* enabled=_wgetenv(L"SPARTAN_HEALTH_PASSIVE");
    if(!enabled || std::wcscmp(enabled,L"1")!=0) {
        state.store(State::Disabled,std::memory_order_release); return 0;
    }
    owner.store(GetCurrentThreadId(),std::memory_order_release);
    const wchar_t* path=_wgetenv(L"SPARTAN_HEALTH_PASSIVE_LOG");
    try {
        const bool local_drive=path && std::wcslen(path)>=3 &&
            ((path[0]>=L'A' && path[0]<=L'Z') || (path[0]>=L'a' && path[0]<=L'z')) &&
            path[1]==L':' && (path[2]==L'\\' || path[2]==L'/');
        if(!local_drive || !std::filesystem::path(path).is_absolute()) {
            std::fputs("Health passive: unavailable (an absolute local-drive SPARTAN_HEALTH_PASSIVE_LOG is required)\n",stderr);
            state.store(State::Failed,std::memory_order_release); return 0;
        }
        if(!HealthCollection::ProcessStore::Get().Initialize()) {
            std::fputs("Health passive: unavailable (process identity initialization failed)\n",stderr);
            state.store(State::Failed,std::memory_order_release); return 0;
        }
        consumer=::new(static_cast<void*>(consumer_storage)) Consumer(clock_source);
        if(!consumer->Start(path,true)) {
            std::fprintf(stderr,"Health passive: unavailable (clock, identity or exclusive journal initialization; Windows error %lu)\n",
                         static_cast<unsigned long>(consumer->Log().Error()));
            state.store(State::Failed,std::memory_order_release); return 0;
        }
        producer_box.store(consumer->EntryMailbox(),std::memory_order_release);
        state.store(State::Active,std::memory_order_release);
        std::puts("Health passive: opt-in journal ready on window thread (16 KiB maximum; informational health only)");
        return 1;
    } catch(...) {
        std::fputs("Health passive: unavailable (initialization exception)\n",stderr);
        if(consumer) consumer->Stop();
        state.store(State::Failed,std::memory_order_release); return 0;
    }
}
extern "C" void spartanport_health_passive_tick(void) {
    if(state.load(std::memory_order_acquire)!=State::Active || !OnOwner()) return;
    unsigned reasons=stop_reasons.exchange(0,std::memory_order_acq_rel);
    if(!reasons) {
        consumer->Poll();
        if(consumer->ClockFaulted()) reasons|=16;
        if(consumer->Log().Failed()) reasons|=32;
    }
    if(reasons) {
        state.store(State::Stopping,std::memory_order_release);
        consumer->Unavailable(reasons);
        std::fprintf(stderr,"Health passive: collection stopped (reason mask %u); no liveness inference\n",reasons);
        CloseOnOwner();
    }
}
extern "C" void spartanport_health_passive_stop(void) {
    if(state.load(std::memory_order_acquire)!=State::Active || !OnOwner()) return;
    state.store(State::Stopping,std::memory_order_release);
    const unsigned reasons=stop_reasons.exchange(0,std::memory_order_acq_rel);
    if(reasons) consumer->Unavailable(reasons);
    CloseOnOwner();
}
extern "C" void spartanport_health_passive_unavailable(unsigned reason) {
    if(state.load(std::memory_order_acquire)==State::Active)
        stop_reasons.fetch_or(reason==2 ? 2 : 1,std::memory_order_release);
}
extern "C" unsigned spartanport_health_passive_build(unsigned char* out,size_t capacity,
                                                     uintptr_t slots,uintptr_t resume) {
    // The off path calls the exact legacy generator, including its instruction bytes.
    unsigned char legacy[256];
    const unsigned legacy_size=spartanport_actor_build_stub(legacy,slots,resume);
    if(!out || capacity<legacy_size) return 0;
    auto* box=state.load(std::memory_order_acquire)==State::Active && !stop_reasons.load(std::memory_order_acquire) ?
        producer_box.load(std::memory_order_acquire) : nullptr;
    if(box) {
        try {
            HealthTrial::Stub code(out,capacity,reinterpret_cast<SpartanActorSlot*>(slots),box,resume,true);
            return static_cast<unsigned>(code.getSize());
        } catch(...) {
            stop_reasons.fetch_or(4,std::memory_order_release);
        }
    }
    std::memcpy(out,legacy,legacy_size); return legacy_size;
}
#else
extern "C" int spartanport_health_passive_start(void) {return 0;}
extern "C" void spartanport_health_passive_tick(void) {}
extern "C" void spartanport_health_passive_stop(void) {}
extern "C" void spartanport_health_passive_unavailable(unsigned) {}
extern "C" unsigned spartanport_health_passive_build(unsigned char* out,size_t capacity,
                                                     uintptr_t slots,uintptr_t resume) {
    unsigned char legacy[256];
    const unsigned n=spartanport_actor_build_stub(legacy,slots,resume);
    if(!out || capacity<n) return 0;
    memcpy(out,legacy,n); return n;
}
#endif
