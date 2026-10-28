// Experimental x86-64 producer generator, shared by the opt-in bridge and CPU tests.
#pragma once
#include "../../src/actor_hook.h"
#include "../../gpu/third_party/xbyak/xbyak/xbyak.h"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace HealthTrial {
struct Packet {
    uint64_t actor_value{}; // opaque copied value, never dereferenced by the reader
    float health{},maximum{};
    uint64_t player{},session{},generation{},lower{},publication{};
};
struct alignas(64) Mailbox {
    std::atomic<uint32_t> gate{};
    uint32_t ready{};
    std::atomic<uint64_t> attempts{},lost{};
    uint64_t publications{},session{},generation{},lower{};
    Packet packet{};
};
static_assert(std::atomic<uint32_t>::is_always_lock_free && sizeof(std::atomic<uint32_t>)==4);
static_assert(std::atomic<uint64_t>::is_always_lock_free && sizeof(std::atomic<uint64_t>)==8);
static_assert(offsetof(Mailbox,ready)==4 && offsetof(Mailbox,attempts)==8 && offsetof(Mailbox,packet)==56);
struct Read { Packet packet; uint64_t upper; };

// Host consumer only. Clock is invoked AFTER acquiring the gate and copying.
// Updating the lower bound here cannot rejuvenate the already copied packet.
template<class Clock> bool Drain(Mailbox& box,Read& out,Clock clock) {
    uint32_t expected=0;
    if(!box.gate.compare_exchange_strong(expected,1,std::memory_order_acquire)) return false;
    const bool present=box.ready!=0;
    if(present) out.packet=box.packet;
    const uint64_t now=clock();
    if(present) { out.upper=now; box.ready=0; }
    box.lower=now;
    box.gate.store(0,std::memory_order_release);
    return present;
}
inline bool Generation(Mailbox& box,uint64_t next) {
    uint32_t expected=0;
    if(!box.gate.compare_exchange_strong(expected,1,std::memory_order_acquire)) return false;
    const bool valid=next>box.generation;
    if(valid) box.generation=next; // pending packet retains its publication generation
    box.gate.store(0,std::memory_order_release);
    return valid;
}

// Uses only RAX/RDI and arithmetic flags, just as the current producer.
// No call, stack access, SIMD operation, timing instruction, wait or retry.
class Stub : public Xbyak::CodeGenerator {
    uintptr_t slots,box_address,resume;
    static constexpr int P=offsetof(Mailbox,packet);
    void box() { mov(rax,box_address); }
    void finish() {
        // Exactly the two overwritten instructions, then the original continuation.
        db(spartanport_actor_original,SPARTAN_ACTOR_HOOK_LENGTH);
        db(0xff); db(0x25); dd(0); dq(resume);
    }
    void actor(bool player) {
        const uintptr_t target=slots+(player ? 0 : sizeof(SpartanActorSlot));
        mov(rax,target);
        mov(qword[rax],r13);
        mov(edi,dword[r13+0x1cc]); // same guest read as existing hook, once
        mov(dword[rax+8],edi);
        box(); mov(dword[rax+P+offsetof(Packet,health)],edi);
        mov(rax,target);
        mov(edi,dword[r13+0x1d0]); // same guest read as existing hook, once
        mov(dword[rax+12],edi);
        lock(); inc(dword[rax+16]);
        box(); mov(dword[rax+P+offsetof(Packet,maximum)],edi);
        mov(qword[rax+P+offsetof(Packet,actor_value)],r13);
        mov(qword[rax+P+offsetof(Packet,player)],player ? 1 : 0);
    }
    void field(int from,int to) {
        mov(rdi,qword[rax+from]); mov(qword[rax+P+to],rdi);
    }
public:
    Stub(void* memory,size_t capacity,SpartanActorSlot* legacy,Mailbox* mailbox,uintptr_t continuation,bool enabled)
        : CodeGenerator(capacity,memory),slots(reinterpret_cast<uintptr_t>(legacy)),
          box_address(reinterpret_cast<uintptr_t>(mailbox)),resume(continuation) {
        if(!enabled) {
            unsigned char original[256];
            const auto size=spartanport_actor_build_stub(original,slots,resume);
            db(original,size); ready(); return;
        }
        Xbyak::Label drop,full,enemy,publish;
        mov(rdi,box_address);
        lock(); inc(qword[rdi+offsetof(Mailbox,attempts)]);
        mov(eax,1);
        xchg(dword[rdi+offsetof(Mailbox,gate)],eax); // implicit locked instruction
        test(eax,eax); jnz(drop,T_NEAR);
        cmp(dword[rdi+offsetof(Mailbox,ready)],0); jne(full,T_NEAR);
        cmp(byte[r13+0x298],0); // same classification read as existing hook, once
        jne(enemy,T_NEAR);
        actor(true); jmp(publish,T_NEAR);
        L(enemy); actor(false);
        L(publish);
        field(offsetof(Mailbox,session),offsetof(Packet,session));
        field(offsetof(Mailbox,generation),offsetof(Packet,generation));
        field(offsetof(Mailbox,lower),offsetof(Packet,lower));
        inc(qword[rax+offsetof(Mailbox,publications)]);
        field(offsetof(Mailbox,publications),offsetof(Packet,publication));
        mov(dword[rax+offsetof(Mailbox,ready)],1);
        mov(dword[rax+offsetof(Mailbox,gate)],0); // x86 store-release
        finish();
        L(full);
        mov(dword[rdi+offsetof(Mailbox,gate)],0);
        L(drop);
        // A refusal affects diagnostics only; run the entire legacy hook.
        mov(rdi,box_address);
        lock(); inc(qword[rdi+offsetof(Mailbox,lost)]);
        unsigned char original[256];
        const auto size=spartanport_actor_build_stub(original,slots,resume);
        db(original,size);
        ready();
    }
};
} // namespace HealthTrial
