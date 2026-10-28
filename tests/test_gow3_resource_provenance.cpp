// SPDX-License-Identifier: GPL-2.0-or-later
// Production provenance structures + real SlotVector, BindHelper and RecordChunk.
// CPU only: no Image constructor, Vulkan instance, game or guest memory.
#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <thread>
#include <vector>
#include "common/slot_vector.h"
#include "video_core/renderer_vulkan/gow3_pair_capture.h"
#include "video_core/renderer_vulkan/vk_bind_helper.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
extern "C" { std::uint64_t runtime_disabled_optimizations=0; }
// BindHelper's only unrelated external dependency; deliberately no OS rename.
namespace Common { void SetCurrentThreadName(const char*) {} }
using namespace Vulkan::Gow3;
namespace P=Vulkan::Gow3::Provenance;
namespace fs=std::filesystem;
static P::Identity Native(uint64_t uid) {
    P::Identity r; r.uid=uid; r.slot=7; r.backing_ordinal=2; r.view_ordinal=3;
    r.view_slot=8; r.image=100; r.view=101; r.address=0x4000;
    r.resolution=P::Resolution::NativeView; return r;
}
static std::string Read(const fs::path& path) {
    std::ifstream f(path,std::ios::binary); assert(f); return {std::istreambuf_iterator<char>(f),{}};
}
static void Touch(const fs::path& path) { std::ofstream f(path); assert(f); }
int main(int argc,char** argv) {
    assert(argc==2); const auto root=fs::absolute(argv[1]); assert(!fs::exists(root)); fs::create_directories(root);
    assert(P::Requested("1")); for(const char* s:std::array<const char*,5>{nullptr,"","0","true","01"}) assert(!P::Requested(s));
    const auto original=Native(41);
    {
        Common::SlotVector<P::Identity> slots;
        auto id=slots.insert(original); auto token=slots[id]; slots.erase(id);
        auto replacement=Native(42); const auto reused=slots.insert(replacement);
        assert(reused.index==id.index && !P::SameView(token,slots[reused]));
        auto* old_address=&slots[reused];
        for(unsigned i=0;i<2048;++i) (void)slots.insert(Native(100+i));
        assert(old_address!=&slots[reused] && P::SameView(replacement,slots[reused]));
        auto moved=std::move(slots[reused]); assert(P::SameView(replacement,moved));
        std::puts("PASS real SlotVector reuse/growth/move: value identities survive; reused handles do not alias UIDs");
    }
    {
        auto b=original; b.backing_ordinal=1; assert(!P::SameView(original,b));
        b.backing_ordinal=2; assert(P::SameView(original,b));
        b.view_ordinal=4; assert(!P::SameView(original,b));
        b=original; b.resolution=P::Resolution::Unregistered; assert(!P::SameView(original,b));
        b=original; b.uid=0; assert(!P::SameView(original,b));
        b=Native(99); assert(b.address==original.address && !P::SameView(original,b));
        b=original; b.alias_authority_generation=999; b.registry_generation=77;
        assert(P::SameView(original,b)); // object identity only, NOT equivalent content
        std::puts("PASS backing/view generations, unregistered/zero UID rejection, alias identities and authority-versus-content distinction");
    }
    {
        P::Bindings b; b.Begin(71,9);
        struct Task { P::Bindings* bindings; P::Identity identity; } task{&b,original};
        Vulkan::BindHelper helper(true);
        helper.Fork([](void* p) {
            auto& t=*static_cast<Task*>(p); assert(Vulkan::BindHelper::OnHelper());
            t.bindings->Set(2,t.identity,P::Path::Helper);
        },&task);
        helper.Join(); b.Set(3,original,P::Path::Normal); b.Set(4,original,P::Path::Memo);
        assert(b.Get(71,9,2,101).path==P::Path::Helper && b.Get(71,9,3,101).observed && b.Get(71,9,4,101).observed);
        assert(!b.Get(72,9,2,101).observed && !b.Get(71,10,2,101).observed && !b.Get(71,9,2,999).observed);
        b.Set(P::Bindings::Capacity,original,P::Path::Normal); assert(b.Lost()==1);
        b.Begin(72,9); assert(!b.Get(72,9,2,101).observed);
        b.Begin(0,0); b.Set(0,original,P::Path::Normal); assert(!b.Enabled() && !b.Lost());
        std::puts("PASS actual BindHelper fork/join, partial fallback, memo path, explicit draw/interval/view matching, bounds and disabled binding path");
    }
    {
        P::Inbox inbox;
        inbox.Push({.uid=41,.related_uid=42,.address=0x4000,.slot=7,.reason=P::Reason::AliasMember});
        inbox.Push({.uid=41,.address=0x4000,.slot=7,.reason=P::Reason::Unregistered});
        inbox.Push({.uid=42,.address=0x4000,.slot=7,.reason=P::Reason::Registered});
        auto batch=inbox.Drain(); assert(batch.count==3 && batch.events[1].uid==41 && batch.events[2].uid==42);
        Vulkan::RecordChunk chunk; uint64_t later_uid=999; P::Batch consumed;
        assert(chunk.Push([copy=batch,&consumed](vk::CommandBuffer){consumed=copy;}));
        later_uid=123; chunk.Execute(vk::CommandBuffer{});
        assert(consumed.events[0].related_uid==42 && consumed.events[1].uid!=later_uid);
        for(unsigned i=0;i<100;++i) inbox.Push({.uid=1000+i,.reason=P::Reason::CpuOverlap});
        batch=inbox.Drain(); assert(batch.count==64 && batch.lost==36 && batch.attempts==103);
        assert(inbox.Drain().count==0);
        std::puts("PASS invalidation keeps original UID across reuse; actual RecordChunk copies tokens; exact queue saturation/loss accounting");
    }
    {
        P::Inbox inbox; std::atomic<unsigned> done{}; std::vector<std::jthread> producers;
        constexpr unsigned per=4000,threads=4; uint64_t observed=0; std::set<uint64_t> ids;
        for(unsigned t=0;t<threads;++t) producers.emplace_back([&,t]{
            for(unsigned i=0;i<per;++i) inbox.Push({.uid=t*per+i+1,.reason=P::Reason::CpuOverlap});
            done.fetch_add(1,std::memory_order_release);
        });
        auto consume=[&](const P::Batch& batch) {
            for(unsigned i=0;i<batch.count;++i) {
                const auto& e=batch.events[i]; assert(e.uid && e.uid<=per*threads && ids.insert(e.attempt).second); ++observed;
            }
        };
        while(done.load(std::memory_order_acquire)!=threads) { consume(inbox.Drain()); std::this_thread::yield(); }
        producers.clear(); const auto last=inbox.Drain(); consume(last);
        assert(last.attempts==per*threads && observed+last.lost==last.attempts);
        std::printf("PASS concurrent queue: attempts=%llu observed=%llu lost=%llu, every event retained or counted lost\n",
            (unsigned long long)last.attempts,(unsigned long long)observed,(unsigned long long)last.lost);
    }
    {
        Pair::Capture disabled(false,root/"disabled",root/"disabled-trigger",8192,true,true,Pair::Capture::ByteCapacity,true);
        disabled.Begin(1,2); disabled.ProvenanceEvents({},"disabled"); disabled.Boundary({1,1,0,false},1,Status::Missing);
        assert(!disabled.ProvenanceEnabled() && !fs::exists(disabled.OutputPath()));
        Pair::Capture c(true,root/"data",root/"trigger",8192,true,true,Pair::Capture::ByteCapacity,true);
        Touch(c.TriggerPath()); c.Boundary({1,1,0,false},1,Status::Missing); auto path=c.OutputPath();
        c.Begin(123,456); auto* d=c.Pending(); assert(d && d->provenance_details);
        d->viewport_count=1; d->vs_key=123; d->color_count=1; d->color_provenance[0]=original;
        d->Image({2,0,2,5,101,{original,P::Path::Memo,true}}); c.Recorded();
        P::Batch batch; batch.events[0]={.attempt=9,.uid=41,.related_uid=42,.slot=7,.reason=P::Reason::AliasMember};
        batch.count=1; batch.attempts=10; batch.lost=2;
        c.ProvenanceEvents(batch,"after_recording"); c.Boundary({2,1,0,true},2,Status::Candidate);
        c.Boundary({3,1,0,false},3,Status::Missing);
        const auto text=Read(path);
        assert(text.find("gow3-provenance-v2")!=std::string::npos && text.find("\"backing_ordinal\":2")!=std::string::npos);
        assert(text.find("\"path\":1")!=std::string::npos && text.find("\"provenance_lost_total\":2")!=std::string::npos);
        assert(text.find("\"content_versions_tracked\":false")!=std::string::npos);
        assert(text.find("\"complete_instrumented_draw_window\":0")!=std::string::npos);
        Pair::Capture collision(true,root/"data",root/"trigger",8192,true,true,Pair::Capture::ByteCapacity,true);
        collision.Boundary({1,1,0,false},1,Status::Missing);
        assert(collision.GetStats().state==Pair::State::Failed && Read(path)==text);
        std::puts("PASS actual collector: disabled creates no output; joined identities/invalidations serialize; exclusive output preserves old file");
    }
    {
        Pair::Capture c(true,root/"bounded",root/"bounded-trigger",8192,true,true,8192,true);
        Touch(c.TriggerPath()); c.Boundary({1,1,0,false},1,Status::Missing); const auto path=c.OutputPath();
        P::Batch b; b.count=64; for(auto& e:b.events) e={.uid=999999,.reason=P::Reason::CpuOverlap};
        for(unsigned i=0;i<100;++i) c.ProvenanceEvents(b,"after_recording");
        c.Boundary({2,1,0,true},2,Status::Missing); c.Boundary({3,1,0,false},3,Status::Missing);
        assert(fs::file_size(path)<=8192 && Read(path).find("\"provenance_event_records_dropped\":0")==std::string::npos);
        std::puts("PASS exact bounded physical bytes and retained loss footer under provenance-event saturation");
    }
    for(bool busy:{false,true}) {
        const auto name=busy?"busy":"baseline";
        Pair::Capture c(true,root/name,root/(std::string(name)+"-trigger"),8192,true,true,Pair::Capture::ByteCapacity,true);
        P::Batch old; old.attempts=100; old.lost=36;
        c.ProvenanceEvents(old,"before_accepted_flip",1);
        Touch(c.TriggerPath()); c.Boundary({1,1,0,false},1,Status::Missing); const auto path=c.OutputPath();
        old.busy=busy; c.ProvenanceEvents(old,"before_accepted_flip",2);
        c.Boundary({2,1,0,false},2,Status::Missing); c.Boundary({3,1,0,false},3,Status::Missing);
        const auto text=Read(path);
        assert(text.find("\"provenance_loss_baseline\":36")!=std::string::npos);
        assert(text.find(busy?"\"complete_instrumented_draw_window\":0":"\"complete_instrumented_draw_window\":1")!=std::string::npos);
    }
    std::puts("PASS deferred loss baseline preserves lifetime counters; busy drain prevents complete-window claim");
    {
        Pair::Capture c(true,root/"stencil",root/"stencil-trigger",8192,true,true,Pair::Capture::ByteCapacity,true);
        Touch(c.TriggerPath()); c.Boundary({1,1,0,false},1,Status::Missing); const auto path=c.OutputPath();
        c.Begin(123,0); auto* d=c.Pending(); d->vs_key=123; d->viewport_count=1;
        // No depth observation, but an effective stencil-only attachment exists.
        d->depth_provenance.view=999; d->depth_provenance.resolution=P::Resolution::CacheBusy;
        c.Recorded(); c.Boundary({2,1,0,false},2,Status::Missing); c.Boundary({3,1,0,false},3,Status::Missing);
        const auto text=Read(path);
        assert(text.find("\"provenance_incomplete_draws\":1")!=std::string::npos &&
               text.find("\"complete_instrumented_draw_window\":0")!=std::string::npos);
        std::puts("PASS unresolved effective stencil-only attachment prevents complete-window claim");
    }
    std::puts("PASS provenance CPU suite; no runtime Image construction or GPU execution, no content/lifetime claim");
}
