// Experimental passive collection, shared by the opt-in bridge and CPU tests.
#pragma once
#include "health_clock_trial.h"
#include <bcrypt.h>
#include <array>
#include <bit>
#include <cstdio>
#include <optional>
#include <string_view>
#include <type_traits>

namespace HealthCollection {
using namespace HealthTrial;
struct Id { std::array<unsigned char,16> bytes{}; };
using Entropy=bool(*)(Id&) noexcept;
inline bool RandomId(Id& id) noexcept {
    if(BCryptGenRandom(nullptr,id.bytes.data(),16,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0) return false;
    id.bytes[6]=(id.bytes[6]&15)|64; id.bytes[8]=(id.bytes[8]&63)|128;
    return true;
}
inline std::array<char,33> Hex(const Id& id) noexcept {
    constexpr char alphabet[]="0123456789abcdef";
    std::array<char,33> out{};
    for(size_t i=0;i<16;++i) {out[i*2]=alphabet[id.bytes[i]>>4]; out[i*2+1]=alphabet[id.bytes[i]&15];}
    return out;
}
struct Slot {
    Mailbox mailbox;
    Id collection;
    std::atomic<bool> published{},collecting{};
};

// Fixed process-lifetime storage. No destructor, free, reset or reuse API.
// Initialization belongs to the single host controller BEFORE guest entry.
class ProcessStore {
public:
    static constexpr uint32_t Capacity=4;
    static ProcessStore& Get() noexcept { static constinit ProcessStore value; return value; }
    ProcessStore(const ProcessStore&)=delete;
    ProcessStore(ProcessStore&&)=delete;
    bool Initialize(Entropy entropy=RandomId) noexcept {
        unsigned expected=0;
        if(!state.compare_exchange_strong(expected,1)) return expected==2;
        FILETIME creation{},exit{},kernel{},user{};
        if(!entropy(process) || !GetProcessTimes(GetCurrentProcess(),&creation,&exit,&kernel,&user)) {
            state.store(3,std::memory_order_release); return false;
        }
        pid=GetCurrentProcessId(); birth=(uint64_t(creation.dwHighDateTime)<<32)|creation.dwLowDateTime;
        state.store(2,std::memory_order_release); return true;
    }
    bool Ready() const noexcept {return state.load(std::memory_order_acquire)==2;}
    Slot* Claim(const Id& id,uint64_t lower) noexcept {
        if(!Ready() || !lower) return nullptr;
        auto index=claimed.load(std::memory_order_relaxed);
        if(id.bytes==process.bytes) return nullptr;
        for(uint32_t i=0;i<index && i<Capacity;++i)
            if(id.bytes==slots[i].collection.bytes) return nullptr;
        if(index==Capacity || !claimed.compare_exchange_strong(index,index+1)) return nullptr;
        Slot& slot=slots[index];
        slot.collection=id; slot.mailbox.session=index+1; slot.mailbox.generation=1; slot.mailbox.lower=lower;
        return &slot; // not visible to producers until the caller publishes it
    }
    uint32_t Claimed() const noexcept {return claimed.load();}
    const Id& ProcessId() const noexcept {return process;}
    uint32_t Pid() const noexcept {return pid;}
    uint64_t Birth() const noexcept {return birth;}
private:
    constexpr ProcessStore()=default;
    std::atomic<unsigned> state{};
    std::atomic<uint32_t> claimed{};
    Id process;
    uint32_t pid{};
    uint64_t birth{};
    std::array<Slot,Capacity> slots{};
};
static_assert(std::is_trivially_destructible_v<ProcessStore>);

using WriteFn=BOOL(WINAPI*)(HANDLE,LPCVOID,DWORD,LPDWORD,LPOVERLAPPED);
class Journal {
public:
    static constexpr uint64_t Maximum=16384,FooterReserve=512;
    explicit Journal(WriteFn write_=WriteFile) noexcept :write(write_) {}
    Journal(const Journal&)=delete;
    Journal(Journal&&)=delete;
    ~Journal(){CloseIncomplete();}
    bool Open(const wchar_t* path,std::string_view header,uint64_t limit=Maximum) noexcept {
        if(attempted || limit>Maximum || limit<FooterReserve || header.size()>limit-FooterReserve) return false;
        attempted=true; cap=limit;
        file=CreateFileW(path,GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) {error=GetLastError(); return false;}
        if(GetFileType(file)!=FILE_TYPE_DISK) {error=ERROR_NOT_SUPPORTED; CloseIncomplete(); return false;}
        if(!Write(header)) {CloseIncomplete(); return false;}
        return true;
    }
    bool Append(std::string_view line) noexcept {
        ++seen;
        if(file==INVALID_HANDLE_VALUE || failed || line.size()>cap-FooterReserve-bytes) {++dropped; return false;}
        if(!Write(line)) {++dropped; return false;}
        ++retained; return true;
    }
    void Finish(uint64_t producer_attempts,uint64_t producer_lost,bool clock_fault,bool loss_latched) noexcept {
        if(file==INVALID_HANDLE_VALUE) return;
        char footer[FooterReserve];
        const int n=std::snprintf(footer,sizeof(footer),
            "{\"type\":\"stop\",\"records_seen\":%llu,\"records_written\":%llu,\"records_dropped\":%llu,"
            "\"producer_attempts_snapshot\":%llu,\"producer_lost_snapshot\":%llu,"
            "\"producer_totals_terminal\":false,\"clock_fault\":%s,\"loss_latched\":%s}\n",
            ull(seen),ull(retained),ull(dropped),ull(producer_attempts),ull(producer_lost),
            clock_fault?"true":"false",loss_latched?"true":"false");
        if(!failed && n>0 && size_t(n)<sizeof(footer) && uint64_t(n)<=cap-bytes)
            complete=Write({footer,size_t(n)});
        if(!FlushFileBuffers(file)) {failed=true; complete=false; error=GetLastError();}
        CloseIncomplete();
    }
    void CloseIncomplete() noexcept {
        if(file!=INVALID_HANDLE_VALUE) {CloseHandle(file); file=INVALID_HANDLE_VALUE;}
    }
    uint64_t Bytes() const noexcept {return bytes;}
    uint64_t Dropped() const noexcept {return dropped;}
    uint64_t Seen() const noexcept {return seen;}
    bool Complete() const noexcept {return complete && !failed;}
    bool Failed() const noexcept {return failed || error;}
    DWORD Error() const noexcept {return error;}
private:
    static unsigned long long ull(uint64_t n) noexcept {return static_cast<unsigned long long>(n);}
    bool Write(std::string_view value) noexcept {
        DWORD written{};
        const BOOL ok=write(file,value.data(),static_cast<DWORD>(value.size()),&written,nullptr);
        bytes+=written;
        if(!ok || written!=value.size()) {failed=true; error=ok ? ERROR_WRITE_FAULT : GetLastError(); return false;}
        return true;
    }
    const WriteFn write;
    HANDLE file=INVALID_HANDLE_VALUE;
    bool attempted{},failed{},complete{};
    DWORD error{};
    uint64_t cap{},bytes{},seen{},retained{},dropped{};
};

template<class Source> class Collector {
public:
    explicit Collector(Source& source_,WriteFn write=WriteFile) noexcept :source(source_),journal(write) {}
    Collector(const Collector&)=delete;
    Collector(Collector&&)=delete;
    ~Collector(){Stop();}
    bool Start(const wchar_t* path,bool enabled=false,uint64_t limit=Journal::Maximum,Entropy entropy=RandomId) noexcept {
        if(attempted) return false;
        attempted=true;
        if(!enabled || !ProcessStore::Get().Ready()) return false;
        clock.emplace(source,true);
        const auto now=clock->Sample();
        if(!now.valid) return false;
        Id collection{};
        if(!entropy(collection)) return false;
        auto& store=ProcessStore::Get();
        slot=store.Claim(collection,now.ticks);
        if(!slot) return false;
        const auto process_hex=Hex(store.ProcessId()),collection_hex=Hex(collection);
        char header[512];
        const int n=std::snprintf(header,sizeof(header),
            "{\"type\":\"header\",\"schema\":\"health-passive-trial-v1\",\"process_id\":\"%s\","
            "\"collection_id\":\"%s\",\"pid\":%lu,\"process_birth\":%llu,\"session\":%llu,"
            "\"qpc_frequency\":%llu,\"health_is_informational\":true,\"proves_alive\":false}\n",
            process_hex.data(),collection_hex.data(),static_cast<unsigned long>(store.Pid()),ull(store.Birth()),
            ull(slot->mailbox.session),ull(clock->Frequency()));
        if(n<=0 || size_t(n)>=sizeof(header) || !journal.Open(path,{header,size_t(n)},limit)) return false;
        slot->collecting.store(true,std::memory_order_relaxed);
        slot->published.store(true,std::memory_order_release); active=true; return true;
    }
    Mailbox* EntryMailbox() const noexcept {
        return active && slot->published.load(std::memory_order_acquire) ? &slot->mailbox : nullptr;
    }
    bool Poll() noexcept {
        if(!active) return false;
        auto observation=PollTimed(slot->mailbox,*clock);
        loss_latched=loss_latched || observation.lost!=0;
        if(observation.state==PollState::Busy) {++busy_polls; return false;}
        char line[512]; int n{};
        if(observation.state==PollState::Packet) {
            const auto& packet=observation.sample.packet;
            n=std::snprintf(line,sizeof(line),
                "{\"type\":\"sample\",\"session\":%llu,\"generation\":%llu,\"publication\":%llu,"
                "\"lower\":%llu,\"upper\":%llu,\"time_valid\":%s,\"generation_matches\":%s,\"player_marker\":%llu,"
                "\"health_bits\":\"%08x\",\"maximum_bits\":\"%08x\",\"producer_lost_snapshot\":%llu,"
                "\"loss_latched\":%s}\n",ull(packet.session),ull(packet.generation),ull(packet.publication),
                ull(packet.lower),ull(observation.sample.upper),observation.time_valid?"true":"false",
                packet.generation==generation ? "true":"false",ull(packet.player),std::bit_cast<uint32_t>(packet.health),
                std::bit_cast<uint32_t>(packet.maximum),ull(observation.lost),loss_latched?"true":"false");
        } else if(clock->Faulted() && !fault_recorded) {
            n=std::snprintf(line,sizeof(line),"{\"type\":\"clock_invalid\",\"session\":%llu}\n",ull(slot->mailbox.session));
            fault_recorded=true;
        }
        if(n>0) {
            if(size_t(n)>=sizeof(line) || !journal.Append({line,size_t(n)})) loss_latched=true;
        }
        return observation.state==PollState::Packet;
    }
    bool AdvanceGeneration(uint64_t next) noexcept {
        if(!active || !Generation(slot->mailbox,next)) return false;
        generation=next;
        char line[96]; const int n=std::snprintf(line,sizeof(line),
            "{\"type\":\"diagnostic_generation\",\"value\":%llu,\"proves_respawn\":false}\n",ull(next));
        if(n<=0 || size_t(n)>=sizeof(line) || !journal.Append({line,size_t(n)})) loss_latched=true;
        return true;
    }
    void Stop() noexcept {
        if(!active) return;
        active=false; slot->collecting.store(false,std::memory_order_release);
        const auto lost=slot->mailbox.lost.load(); loss_latched=loss_latched || lost!=0;
        journal.Finish(slot->mailbox.attempts.load(),lost,clock->Faulted(),loss_latched);
        // No reset, free, unpatch, join, guest call or finality claim.
    }
    bool LossLatched() const noexcept {return loss_latched;}
    bool ClockFaulted() const noexcept {return clock && clock->Faulted();}
    // Consumer-only notices; reasons are fixed by the bridge, never guest text.
    void Unavailable(unsigned reason) noexcept {
        if(!active) return;
        char line[96];
        const int n=std::snprintf(line,sizeof(line),
            "{\"type\":\"bridge_unavailable\",\"reason_mask\":%u}\n",reason);
        if(n<=0 || size_t(n)>=sizeof(line) || !journal.Append({line,size_t(n)})) loss_latched=true;
    }
    const Journal& Log() const noexcept {return journal;}
private:
    static unsigned long long ull(uint64_t n) noexcept {return static_cast<unsigned long long>(n);}
    Source& source;
    Journal journal;
    std::optional<CheckedClock<Source>> clock;
    Slot* slot{};
    bool attempted{},active{},loss_latched{},fault_recorded{};
    uint64_t generation=1,busy_polls{};
};
} // namespace HealthCollection
