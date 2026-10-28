#pragma once
#include "health_inline_trial.h"
#include <windows.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <thread>
#include <vector>

using namespace HealthTrial;
struct Region {
    void* data=VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    Region(){assert(data);}
    ~Region(){VirtualFree(data,0,MEM_RELEASE);}
    void seal(){DWORD old; assert(VirtualProtect(data,4096,PAGE_EXECUTE_READ,&old));
                assert(FlushInstructionCache(GetCurrentProcess(),data,4096));}
};
struct Result {
    uint64_t gpr[16]{};
    alignas(32) unsigned char simd[16][32]{};
    uint64_t rsp_before{},flags{},redzone[16]{};
    uint32_t mxcsr{},mxcsr_before{};
};
alignas(32) static unsigned char vector_seed[16][32];
static constexpr uint64_t Canary=0xcafecafe12345678ULL;
static constexpr uint64_t Seed(unsigned i){return 0x12340000u+i;}

// SysV entry accepts (actor, Result*), as the guest does. Only fake host actors
// are passed. Capture all GPRs, SIMD lanes, stack/red-zone and DF at resume.
class Harness : public Xbyak::CodeGenerator {
public:
    Harness(void* memory,uintptr_t target,bool avx): CodeGenerator(4096,memory) {
        using namespace Xbyak;
        const char* stage="setup";
        try {
        push(rbx); push(rbp); push(r12); push(r13); push(r14); push(r15);
        sub(rsp,512);
        mov(qword[rsp],rsi); mov(r13,rdi);
        mov(rax,rsi); mov(qword[rax+offsetof(Result,rsp_before)],rsp);
        stmxcsr(ptr[rax+offsetof(Result,mxcsr_before)]);
        mov(rax,reinterpret_cast<uintptr_t>(vector_seed));
        stage="SIMD seed";
        for(int i=0;i<16;++i) {
            if(avx) vmovdqu(Ymm(i),ptr[rax+i*32]); else movdqu(Xmm(i),ptr[rax+i*32]);
        }
        mov(rax,Canary);
        stage="GPR seed";
        for(int i=0;i<16;++i) mov(qword[rsp-16-i*8],rax);
        for(int i=0;i<16;++i) if(i!=4 && i!=13) mov(Reg64(i),Seed(i));
        cld();
        Label address;
        stage="indirect call";
        call(ptr[rip+address]); // near indirect call is inherently 64-bit in long mode
        stage="GPR capture";
        mov(qword[rsp+8],rax); mov(rax,qword[rsp]);
        for(int i=1;i<16;++i) mov(qword[rax+offsetof(Result,gpr)+i*8],Reg64(i));
        mov(rdi,qword[rsp+8]); mov(qword[rax],rdi);
        stage="SIMD capture";
        for(int i=0;i<16;++i) {
            if(avx) vmovdqu(ptr[rax+offsetof(Result,simd)+i*32],Ymm(i));
            else movdqu(ptr[rax+offsetof(Result,simd)+i*32],Xmm(i));
        }
        pushfq(); pop(rdi); mov(qword[rax+offsetof(Result,flags)],rdi);
        stage="MXCSR capture";
        stmxcsr(ptr[rax+offsetof(Result,mxcsr)]);
        stage="red zone capture";
        for(int i=0;i<16;++i) {
            mov(rdi,qword[rsp-16-i*8]); mov(qword[rax+offsetof(Result,redzone)+i*8],rdi);
        }
        if(avx) vzeroupper();
        add(rsp,512); pop(r15); pop(r14); pop(r13); pop(r12); pop(rbp); pop(rbx); ret();
        L(address); dq(target); ready();
        } catch(const Xbyak::Error& error) {
            std::fprintf(stderr,"Harness generation failed at %s: %s\n",stage,error.what());
            throw;
        }
    }
};

static void actor_data(unsigned char* actor,float health,bool player) {
    uint64_t vtable=0x1122334455667788ULL;
    std::memcpy(actor,&vtable,8);
    std::memcpy(actor+0x1cc,&health,4);
    const float maximum=health*2;
    std::memcpy(actor+0x1d0,&maximum,4);
    actor[0x298]=player ? 0 : 1;
}
static void check(const Result& r,const void* actor,bool avx) {
    for(unsigned i=0;i<16;++i) {
        const auto expected=i==0 ? 0x1122334455667788ULL :
            (i==7 || i==13) ? reinterpret_cast<uintptr_t>(actor) : i==4 ? r.rsp_before : Seed(i);
        assert(r.gpr[i]==expected);
    }
    for(int i=0;i<16;++i) assert(!std::memcmp(r.simd[i],vector_seed[i],avx ? 32 : 16));
    for(const auto value:r.redzone) assert(value==Canary);
    assert(!(r.flags&(1u<<10)) && r.mxcsr==r.mxcsr_before); // arithmetic flags intentionally not promised
}
using Invoke=void (__attribute__((sysv_abi)) *)(void*,Result*);
