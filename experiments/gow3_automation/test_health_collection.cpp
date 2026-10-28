#include "health_collection_trial.h"
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <semaphore>
#include <string>
#include <thread>
#include <vector>
using namespace HealthCollection;
using namespace std::chrono_literals;
namespace fs=std::filesystem;
static bool NoEntropy(Id&) noexcept {return false;}
static bool DuplicateId(Id& id) noexcept {id=ProcessStore::Get().ProcessId(); return true;}
static std::string ReadFileBytes(const fs::path& file) {
    std::ifstream in(file,std::ios::binary); assert(in);
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
struct FakeClock {
    int64_t frequency=1000;
    std::vector<int64_t> values;
    size_t reads{},frequency_reads{};
    bool Frequency(int64_t& out) noexcept {++frequency_reads; out=frequency; return frequency>0;}
    bool Read(int64_t& out) noexcept {assert(reads<values.size()); out=values[reads++]; return out>=0;}
};
struct Region {
    void* data=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    Region(){assert(data);}
    ~Region(){assert(VirtualFree(data,0,MEM_RELEASE));}
    Region(const Region&)=delete;
    void Seal(){DWORD old{}; assert(VirtualProtect(data,4096,PAGE_EXECUTE_READ,&old));
        assert(FlushInstructionCache(GetCurrentProcess(),data,4096));}
};
struct Producer {
    alignas(64) SpartanActorSlot legacy[2]{};
    Region continuation,code,entry;
    using Function=void(__attribute__((sysv_abi)) *)(void*);
    Function call{};
    explicit Producer(Mailbox* mailbox) {
        *static_cast<unsigned char*>(continuation.data)=0xc3; continuation.Seal();
        Stub stub(code.data,4096,legacy,mailbox,reinterpret_cast<uintptr_t>(continuation.data),true);
        code.Seal();
        Xbyak::CodeGenerator wrapper(4096,entry.data); Xbyak::Label target;
        using namespace Xbyak::util;
        wrapper.push(r13); wrapper.mov(r13,rdi); wrapper.call(ptr[rip+target]);
        wrapper.pop(r13); wrapper.ret(); wrapper.L(target); wrapper.dq(reinterpret_cast<uintptr_t>(code.data));
        wrapper.ready(); entry.Seal(); call=reinterpret_cast<Function>(entry.data);
    }
    void Hit(float health,bool player=true) {
        std::array<unsigned char,0x300> actor{};
        const uint64_t vtable=0x1122334455667788ULL; const float maximum=health*2;
        std::memcpy(actor.data(),&vtable,8); std::memcpy(actor.data()+0x1cc,&health,4);
        std::memcpy(actor.data()+0x1d0,&maximum,4); actor[0x298]=player ? 0 : 1; call(actor.data());
    }
};
static unsigned partial_calls{};
static BOOL WINAPI PartialWrite(HANDLE file,LPCVOID buffer,DWORD size,LPDWORD written,LPOVERLAPPED overlap) {
    if(++partial_calls==2) {
        const BOOL result=WriteFile(file,buffer,(std::min)(DWORD(7),size),written,overlap);
        assert(result); SetLastError(ERROR_WRITE_FAULT); return FALSE;
    }
    return WriteFile(file,buffer,size,written,overlap);
}
static void JournalTests(const fs::path& directory) {
    const auto file=directory/"exact-limit.jsonl";
    const std::string header="{\"type\":\"header\"}\n";
    constexpr uint64_t Cap=2048;
    Journal log; assert(log.Open(file.c_str(),header,Cap));
    // Fill the data budget exactly, leaving the separate footer reservation.
    const size_t room=Cap-Journal::FooterReserve-header.size();
    const std::string line="{\"pad\":\""+std::string(room-11,'x')+"\"}\n";
    assert(line.size()==room && log.Append(line));
    assert(!log.Append("{}\n") && log.Dropped()==1 && log.Seen()==2);
    log.Finish(10,2,false,true);
    assert(log.Complete() && log.Bytes()<=Cap && fs::file_size(file)==log.Bytes());
    const auto original=ReadFileBytes(file);
    assert(original.find('\r')==std::string::npos && original.find("\"records_dropped\":1")!=std::string::npos);
    Journal collision; assert(!collision.Open(file.c_str(),header,Cap) && collision.Error()==ERROR_FILE_EXISTS);
    assert(ReadFileBytes(file)==original);
    const auto partial=directory/"partial-write.jsonl";
    Journal fault(PartialWrite); assert(fault.Open(partial.c_str(),header,Cap));
    assert(!fault.Append("{\"type\":\"sample\"}\n") && fault.Failed() && fault.Dropped()==1);
    fault.Finish(1,0,false,true);
    assert(!fault.Complete() && fs::file_size(partial)==header.size()+7);
    assert(ReadFileBytes(partial).find("\"type\":\"stop\"")==std::string::npos);
    Journal too_small;
    const auto missing=directory/"too-small.jsonl";
    assert(!too_small.Open(missing.c_str(),header,Journal::FooterReserve) && !fs::exists(missing));
    puts("PASS journal: exact Windows byte limit, reserved footer, saturation counted, CREATE_NEW preserves existing file, partial I/O remains incomplete");
}
struct DetachedSync {
    std::counting_semaphore<2> ready{0},resume{0},done{0};
};
static void Normal(const fs::path& directory) {
    auto& store=ProcessStore::Get(); assert(store.Initialize());
    assert(sizeof(ProcessStore)<2048 && store.Claimed()==0);
    WindowsQpc source;
    const auto no_id=directory/"no-id.jsonl";
    Collector missing_id(source);
    assert(!missing_id.Start(no_id.c_str(),true,4096,NoEntropy));
    assert(!missing_id.EntryMailbox() && !fs::exists(no_id) && store.Claimed()==0);
    Collector duplicate(source);
    assert(!duplicate.Start(no_id.c_str(),true,4096,DuplicateId) && store.Claimed()==0);
    FakeClock invalid; invalid.frequency=0;
    Collector invalid_start(invalid);
    assert(!invalid_start.Start(no_id.c_str(),true) && !fs::exists(no_id) && store.Claimed()==0);

    // This synchronization object intentionally lives until process teardown,
    // as do mailboxes. Detached threads do not depend on a controller's stack.
    auto* sync=new DetachedSync;
    Mailbox* old{};
    uint64_t first_session{},journal_bytes{};
    constexpr unsigned LateCalls=1000;
    const auto first_file=directory/"first.jsonl";
    {
        Collector first(source); assert(first.Start(first_file.c_str(),true,4096));
        old=first.EntryMailbox(); assert(old && old->lower && old->session);
        first_session=old->session;
        auto p=std::make_shared<Producer>(old),q=std::make_shared<Producer>(old);
        p->Hit(20); assert(first.AdvanceGeneration(2)); assert(first.Poll());
        assert(!first.AdvanceGeneration(2));
        p->Hit(21); q->Hit(22,false); assert(first.Poll() && first.LossLatched());
        q->Hit(23,false); assert(first.Poll());
        auto worker=[sync](std::shared_ptr<Producer> producer,unsigned id) {
            producer->Hit(static_cast<float>(id)); sync->ready.release();
            assert(sync->resume.try_acquire_for(3s));
            for(unsigned i=0;i<LateCalls;++i) producer->Hit(static_cast<float>(id*10000+i));
            producer.reset(); // code lifetime ends after the last generated call
            sync->done.release();
        };
        std::thread(worker,p,1).detach(); std::thread(worker,q,2).detach();
        assert(sync->ready.try_acquire_for(3s) && sync->ready.try_acquire_for(3s));
        assert(first.Poll());
        first.Stop(); assert(!first.EntryMailbox() && !first.Poll() && !first.AdvanceGeneration(3));
        assert(first.Log().Complete()); journal_bytes=first.Log().Bytes();
    } // adapter, journal and controller gone; mailbox is still process-owned
    const auto original_first=ReadFileBytes(first_file);
    assert(original_first.find("\"generation_matches\":false")!=std::string::npos);
    assert(original_first.find("\"loss_latched\":true")!=std::string::npos);
    assert(original_first.find("\"producer_totals_terminal\":false")!=std::string::npos);
    assert(original_first.find("\"proves_alive\":false")!=std::string::npos);
    assert(original_first.find("\"player_marker\":0")!=std::string::npos &&
           original_first.find("\"player_marker\":1")!=std::string::npos);
    assert(original_first.find("actor_value")==std::string::npos);
    const auto second_file=directory/"second.jsonl";
    Collector second(source); assert(second.Start(second_file.c_str(),true,4096));
    auto* fresh=second.EntryMailbox(); assert(fresh!=old && fresh->session!=first_session && fresh->generation==1);
    const auto before=old->attempts.load();
    sync->resume.release(2);
    assert(sync->done.try_acquire_for(3s) && sync->done.try_acquire_for(3s));
    assert(old->attempts==before+2*LateCalls && fresh->attempts==0);
    assert(old->session==first_session && old->generation==2);
    assert(ReadFileBytes(first_file)==original_first && fs::file_size(first_file)==journal_bytes);
    second.Stop();
    const auto second_text=ReadFileBytes(second_file);
    const auto id_begin=original_first.find("\"collection_id\":\"")+17;
    const auto second_id=second_text.find("\"collection_id\":\"")+17;
    assert(original_first.substr(id_begin,32)!=second_text.substr(second_id,32));
    const auto retained=old->publications; // safe: both workers finished all accesses
    assert(retained+old->lost.load()==old->attempts.load());
    std::printf("PASS detached producers: controller destroyed before %u late calls; %llu attempts=%llu publications+%llu losses; stable mailbox and unchanged closed journal\n",
        2*LateCalls,static_cast<unsigned long long>(old->attempts.load()),
        static_cast<unsigned long long>(retained),static_cast<unsigned long long>(old->lost.load()));

    FakeClock regressing; regressing.values={100,99,200};
    Collector bad_time(regressing);
    const auto bad_file=directory/"clock-invalid.jsonl";
    assert(bad_time.Start(bad_file.c_str(),true,4096));
    auto bad_producer=std::make_unique<Producer>(bad_time.EntryMailbox());
    bad_producer->Hit(10); assert(bad_time.Poll());
    bad_producer->Hit(11); assert(bad_time.Poll());
    bad_time.Stop();
    assert(regressing.reads==2);
    const auto bad_text=ReadFileBytes(bad_file);
    assert(bad_text.find("\"time_valid\":true")==std::string::npos && bad_text.find("\"clock_fault\":true")!=std::string::npos);
    Collector existing(source);
    assert(!existing.Start(first_file.c_str(),true,4096) && !existing.EntryMailbox());
    assert(ReadFileBytes(first_file)==original_first && store.Claimed()==ProcessStore::Capacity);
    Collector exhausted(source);
    const auto exhausted_file=directory/"exhausted.jsonl";
    assert(!exhausted.Start(exhausted_file.c_str(),true,4096) && !fs::exists(exhausted_file));
    JournalTests(directory);
    std::printf("PASS process namespace=%s, pid=%lu, fixed_store=%zu bytes, slots=%u (never reused)\n",
        Hex(store.ProcessId()).data(),static_cast<unsigned long>(store.Pid()),sizeof(ProcessStore),store.Claimed());
    puts("PASS full passive lifecycle: generation mismatch preserved, losses latched, invalid clock stays invalid, capacity exhaustion fails closed; no Alive state or game input");
}
int main(int argc,char** argv) {
    assert(argc==3);
    const std::string mode=argv[1]; const fs::path directory=fs::absolute(argv[2]);
    assert(fs::is_directory(directory));
    const auto file=directory/"must-not-exist.jsonl";
    if(mode=="disabled") {
        FakeClock source;
        Collector c(source);
        assert(!c.Start(file.c_str()) && !c.EntryMailbox() && !c.Poll()); c.Stop();
        assert(!ProcessStore::Get().Ready() && !ProcessStore::Get().Claimed());
        assert(!source.reads && !source.frequency_reads && !fs::exists(file));
        puts("PASS disabled: no identity initialization, clock calls, producer entry, journal or storage claim");
    } else if(mode=="identity-failure") {
        auto& store=ProcessStore::Get(); assert(!store.Initialize(NoEntropy) && !store.Initialize());
        FakeClock source; Collector c(source);
        assert(!c.Start(file.c_str(),true) && !source.frequency_reads && !fs::exists(file));
        puts("PASS process identity failure: sticky failure, no fallback identity or exposed producer");
    } else {assert(mode=="normal"); Normal(directory);}
}
