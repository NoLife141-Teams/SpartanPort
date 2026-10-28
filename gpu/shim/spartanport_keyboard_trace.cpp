// Single-use, opt-in passive S / left-stick Y trace. No input injection or guest memory reads.
#include "../../src/keyboard_trace_bridge.h"
#ifdef _WIN32
#include "../../experiments/gow3_automation/keyboard_trace_core.h"
#include "../../experiments/gow3_automation/keyboard_trace_journal.h"
#include <bcrypt.h>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {
enum class State {Cold,Disabled,Armed,Active,Stopped};
std::atomic<State> state{State::Cold};
std::atomic<DWORD> owner{};
KeyTrace::Core core; // trivial, never destroyed/reset/reused while producers can exist
KeyTrace::Journal journal; // exclusively owner thread, no destruction callback
std::wstring output,trigger;
uint64_t frequency{},pump{},last_check{},last_drain{},owner_last{};
bool owner_clock_fault{};
unsigned long long U(uint64_t n) {return static_cast<unsigned long long>(n);}
bool Local(const wchar_t* p) {
    return p && std::wcslen(p)>=3 && ((p[0]>=L'A'&&p[0]<=L'Z')||(p[0]>=L'a'&&p[0]<=L'z')) &&
           p[1]==L':' && (p[2]==L'\\'||p[2]==L'/');
}
uint64_t Now() {
    LARGE_INTEGER n{};
    if(!QueryPerformanceCounter(&n) || n.QuadPart<=0) {core.RequestStop(KeyTrace::ClockFault);return 0;}
    return static_cast<uint64_t>(n.QuadPart);
}
bool OnOwner() {
    if(owner.load(std::memory_order_acquire)==GetCurrentThreadId()) return true;
    core.RequestStop(KeyTrace::OwnerFault);return false;
}
uint64_t OwnerNow() {
    const auto n=Now();
    if(!n || n<owner_last) {owner_clock_fault=true;core.RequestStop(KeyTrace::ClockFault);return 0;}
    owner_last=n;return n;
}
bool Drain() {
    std::array<KeyTrace::Record,KeyTrace::Core::Capacity> rows;
    bool busy=false;
    const auto n=core.Drain(rows,busy);
    for(size_t i=0;i<n;++i) {
        const auto& r=rows[i]; char line[640];
        const char* kind=r.kind==KeyTrace::Context?"context":r.kind==KeyTrace::S?"s":"read_range";
        const int size=std::snprintf(line,sizeof(line),
            "{\"type\":\"%s\",\"serial\":%llu,\"first\":%llu,\"last\":%llu,\"begin\":%llu,\"end\":%llu,"
            "\"context0\":%llu,\"context1\":%llu,\"pump\":%llu,\"count\":%llu,\"max_gap\":%llu,\"flags\":%u,"
            "\"pad_y\":%u,\"host_y\":%u,\"final_y\":%u}\n",
            kind,U(r.serial),U(r.first),U(r.last),U(r.begin),U(r.end),U(r.context0),U(r.context1),U(r.pump),U(r.count),U(r.max_gap),r.flags,
            unsigned(r.pad_y),unsigned(r.host_y),unsigned(r.final_y));
        if(size<=0 || size>=static_cast<int>(sizeof(line))) {core.RequestStop(KeyTrace::IoFault);continue;}
        if(!journal.Append({line,static_cast<size_t>(size)}))
            core.RequestStop(journal.Failed()?KeyTrace::IoFault:KeyTrace::Saturated);
    }
    return !busy;
}
void Finish(KeyTrace::Reason why) {
    core.RequestStop(why);
    const bool drained=Drain();
    const auto c=core.Counts(); char footer[KeyTrace::Journal::Reserve];
    const int n=std::snprintf(footer,sizeof(footer),
        "{\"type\":\"stop\",\"reason\":%u,\"reads_started_snapshot\":%llu,\"offers_snapshot\":%llu,"
        "\"queued_snapshot\":%llu,\"coalesced_snapshot\":%llu,\"busy_lost_snapshot\":%llu,\"full_lost_snapshot\":%llu,"
        "\"closed_or_expired_snapshot\":%llu,\"expiry_observed_snapshot\":%llu,\"redacted_reads_snapshot\":%llu,"
        "\"rows_seen\":%llu,\"rows_written\":%llu,\"rows_dropped\":%llu,\"queue_drained\":%s,\"producer_totals_terminal\":false}\n",
        core.StopReason(),U(c.reads),U(c.offers),U(c.queued),U(c.coalesced),U(c.busy),U(c.full),U(c.closed),U(c.expired),U(c.redacted),
        U(journal.Seen()),U(journal.Written()),U(journal.Dropped()),drained?"true":"false");
    const bool complete=n>0 && n<static_cast<int>(sizeof(footer)) && journal.Finish({footer,static_cast<size_t>(n)});
    if(!complete) journal.Close();
    state.store(State::Stopped,std::memory_order_release);
    std::printf("Keyboard trace: stopped reason=%u bytes=%llu footer=%s; no input changed\n",core.StopReason(),U(journal.Bytes()),complete?"complete":"incomplete");
}
bool Activate(uint64_t now,uint32_t window,uint32_t flags) {
    unsigned char id[16]; FILETIME born{},exit{},kernel{},user{};
    if(BCryptGenRandom(nullptr,id,sizeof(id),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0 ||
       !GetProcessTimes(GetCurrentProcess(),&born,&exit,&kernel,&user)) return false;
    char hex[33];for(unsigned i=0;i<16;++i)std::snprintf(hex+i*2,3,"%02x",id[i]);
    char header[768];
    const int n=std::snprintf(header,sizeof(header),
        "{\"type\":\"header\",\"schema\":\"keyboard-s-left-y-v1\",\"collection\":\"%s\",\"pid\":%lu,\"birth\":%llu,"
        "\"window\":%u,\"start\":%llu,\"frequency\":%llu,\"duration_seconds\":10,\"byte_limit\":65536,"
        "\"first_interval_complete\":false,\"keys\":[\"S\"],\"axes\":[\"left_y\"],\"text_recorded\":false,\"input_modified\":false}\n",
        hex,static_cast<unsigned long>(GetCurrentProcessId()),U((uint64_t(born.dwHighDateTime)<<32)|born.dwLowDateTime),window,U(now),U(frequency));
    if(n<=0 || n>=static_cast<int>(sizeof(header)) || !journal.Open(output.c_str(),{header,static_cast<size_t>(n)})) return false;
    if(!core.Start(now,frequency,window,flags)) {journal.Close();return false;}
    last_drain=now; state.store(State::Active,std::memory_order_release);
    std::puts("Keyboard trace: started, 10 seconds / 64 KiB maximum; S and left Y only, no input changed");return true;
}
}
extern "C" int spartanport_key_trace_start(void) {
    State expected=State::Cold;if(!state.compare_exchange_strong(expected,State::Disabled))return 0;
    const auto* flag=_wgetenv(L"SPARTAN_KEY_TRACE");
    if(!flag || std::wcscmp(flag,L"1"))return 0;
    owner.store(GetCurrentThreadId(),std::memory_order_release);
    const auto* path=_wgetenv(L"SPARTAN_KEY_TRACE_FILE");const auto* signal=_wgetenv(L"SPARTAN_KEY_TRACE_TRIGGER");
    LARGE_INTEGER hz{};
    if(!Local(path)||!Local(signal)||!_wcsicmp(path,signal)||!QueryPerformanceFrequency(&hz)||hz.QuadPart<=0 ||
       GetFileAttributesW(path)!=INVALID_FILE_ATTRIBUTES) {
        std::fputs("Keyboard trace: not armed (paths, existing output or clock)\n",stderr);return 0;
    }
    try {output=path;trigger=signal;}catch(...){return 0;}
    frequency=static_cast<uint64_t>(hz.QuadPart);
    state.store(State::Armed,std::memory_order_release);
    std::puts("Keyboard trace: armed, waiting for session trigger; no input changed");return 1;
}
extern "C" int spartanport_key_trace_enabled(void) {
    const auto s=state.load(std::memory_order_acquire);return s==State::Armed||s==State::Active;
}
extern "C" void spartanport_key_trace_window(uint32_t window,uint32_t flags,int new_pump) {
    if(!spartanport_key_trace_enabled()||!OnOwner())return;
    const auto now=OwnerNow();if(!now){if(state.load()==State::Active)Finish(KeyTrace::ClockFault);else state.store(State::Stopped);return;}
    if(new_pump)++pump;
    if(state.load()==State::Armed) {
        if(core.StopReason()!=KeyTrace::None){state.store(State::Stopped);return;}
        if(last_check && now-last_check<frequency/10)return;
        last_check=now;const auto attr=GetFileAttributesW(trigger.c_str());
        if(attr==INVALID_FILE_ATTRIBUTES || (attr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return;
        if(!Activate(now,window,flags)){state.store(State::Stopped);std::fputs("Keyboard trace: activation failed; output preserved\n",stderr);}
    }else if(window==core.Window()) core.Observe(now,flags,pump);
}
extern "C" void spartanport_key_trace_s(uint32_t window,uint32_t flags,int down,int repeat) {
    if(state.load()!=State::Active||!OnOwner())return;
    core.Key(OwnerNow(),window,flags,pump,down!=0,repeat!=0);
}
extern "C" void spartanport_key_trace_tick(void) {
    if(state.load()!=State::Active||!OnOwner())return;
    const auto now=OwnerNow();
    if(!now || owner_clock_fault)core.RequestStop(KeyTrace::ClockFault);else core.Alive(now);
    if(!core.Active()){Finish(static_cast<KeyTrace::Reason>(core.StopReason()));return;}
    if(now-last_drain>=frequency/10){last_drain=now;Drain();if(!core.Active())Finish(static_cast<KeyTrace::Reason>(core.StopReason()));}
}
extern "C" void spartanport_key_trace_stop(void) {
    if(!spartanport_key_trace_enabled()||!OnOwner())return;
    if(state.load()==State::Active)Finish(KeyTrace::Closed);else state.store(State::Stopped);
}
extern "C" SpartanKeyTraceRead spartanport_key_trace_begin(void) {
    if(state.load(std::memory_order_acquire)!=State::Active)return {};
    return core.Begin(Now());
}
extern "C" void spartanport_key_trace_end(SpartanKeyTraceRead r) {
    if(!r.id)return;core.End(r,Now());
}
#else
extern "C" int spartanport_key_trace_start(void){return 0;}
extern "C" int spartanport_key_trace_enabled(void){return 0;}
extern "C" void spartanport_key_trace_window(uint32_t,uint32_t,int){}
extern "C" void spartanport_key_trace_s(uint32_t,uint32_t,int,int){}
extern "C" void spartanport_key_trace_tick(void){}
extern "C" void spartanport_key_trace_stop(void){}
extern "C" SpartanKeyTraceRead spartanport_key_trace_begin(void){return {};}
extern "C" void spartanport_key_trace_end(SpartanKeyTraceRead){}
#endif
