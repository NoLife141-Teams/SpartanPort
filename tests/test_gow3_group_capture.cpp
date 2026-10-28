// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic grouped metadata protocol. No GPU, guest data or game access.
#include <cassert>
#include <fstream>
#include <iterator>
#include "gpu/shadps4/video_core/renderer_vulkan/gow3_pair_capture.h"
using namespace Vulkan::Gow3;
using namespace Vulkan::Gow3::Pair;
namespace fs=std::filesystem;
static void Touch(const fs::path& p) { std::ofstream f(p); assert(bool(f)); }
static std::string Read(const fs::path& p) { std::ifstream f(p); assert(f); return {std::istreambuf_iterator<char>(f),{}}; }
static void DrawOne(Capture& c,uint64_t key=11) {
    c.Begin(DepthShader,44); auto* d=c.Pending(); assert(d && d->grouped_details);
    d->viewport_count=1; d->vs_key=key; d->ps_key=45;
    d->blend_key_bits[0]=0x12345678; d->write_masks[0]=7; d->blend_constant_bits[3]=0x80000000;
    auto* s=c.Shader(); assert(s); s->vs_key=key; s->ps_key=45;
    s->dynamic_seen=true; s->vertex_input_seen=true; s->dynamic_vertex_input=true;
    s->Binding({1,1,1,0,7,1024,144,true});
    s->attribute_count=1; s->attributes[0]={2,0,109,12};
    s->stream_count=1; s->streams[0]={0,16,0,1,77,4096,800};
    d->Image({7,0,2,5,99}); c.Recorded();
}
int main(int argc,char** argv) {
    assert(argc==2); const auto root=fs::absolute(argv[1]); assert(!fs::exists(root)); fs::create_directories(root);
    assert(GroupRequested("1","1","1","1"));
    for(const char* v:{"0","true","01",""}) {
        assert(!GroupRequested(v,"1","1","1") && !GroupRequested("1",v,"1","1"));
        assert(!GroupRequested("1","1",v,"1") && !GroupRequested("1","1","1",v));
    }
    assert(!GroupRequested("1","1","1",nullptr));
    { Capture c(true,root/"invalid",root/"trigger",1,false,true); assert(c.GetStats().state==State::Failed); }
    { Capture c(true,root/"invalid",root/"trigger",1,true,true,8191); assert(c.GetStats().state==State::Failed); }
    { Capture c(true,root/"invalid",root/"trigger",1,true,true,Capture::ByteCapacity+1); assert(c.GetStats().state==State::Failed); }
    {
        Capture c(false,root/"disabled",root/"trigger",Capture::Capacity,true,true);
        c.Begin(1,2); c.Boundary({1,1,0,false},1,Status::Missing); assert(c.GetStats().state==State::Disabled);
    }
    {
        Capture c(true,root/"group",root/"start",Capture::Capacity,true,true);
        c.Begin(1,2); c.Boundary({90,1,0,false},1,Status::Missing); assert(!fs::exists(c.OutputPath()));
        uint64_t next=100;
        for(unsigned w=0;w<4;++w) {
            const auto path=c.OutputPath();
            assert(path==fs::path((root/"group").string()+"."+Capture::Labels[w]+".jsonl"));
            Touch(c.TriggerPath()); c.Begin(1,2); assert(!c.Pending());
            c.Boundary({next++,1,0,false},w*3,Status::Missing); assert(c.GetStats().state==State::Active);
            DrawOne(c,11+w); std::array<uint32_t,36> words{}; words[32]=w;
            c.Snapshot(w==1?nullptr:words.data(),144,w==1?SnapshotReason::AllocationFailed:SnapshotReason::Copied,{});
            c.Boundary({next++,1,0,true},w*3+1,Status::Candidate);
            DrawOne(c,11+w); c.Boundary({next++,2,1,false},w*3+2,Status::Missing);
            assert(c.GetStats().state==(w==3?State::Done:State::Armed));
            const auto text=Read(path);
            assert(text.find("\"closed_intervals\":2")!=std::string::npos);
            assert(text.find("\"complete_instrumented_draw_window\":1")!=std::string::npos);
            assert(text.find("\"vertex_attributes\":[[2,0,109,12]]")!=std::string::npos);
            assert(text.find("\"view_label\":\"63\"")!=std::string::npos);
            assert(text.find("\"blend_control_key_bits\":[\"12345678\"")!=std::string::npos);
            assert(text.find("\"color_write_masks\":[7,0")!=std::string::npos);
            assert(text.find("\"blend_constant_bits\":[\"0\",\"0\",\"0\",\"80000000\"]")!=std::string::npos);
            if(w==1) assert(text.find("ring-space-unavailable")!=std::string::npos);
        }
        c.Begin(1,2); c.Boundary({next,2,1,false},99,Status::Candidate); assert(c.GetStats().state==State::Done);
    }
    // A subsequent output collision is terminal, including pre-existing trigger.
    {
        Capture c(true,root/"collision",root/"collision-trigger",Capture::Capacity,true,true);
        Touch(c.TriggerPath()); c.Boundary({1,1,0,false},1,Status::Missing);
        c.Boundary({2,1,0,true},2,Status::Missing); c.Boundary({3,2,1,false},3,Status::Missing);
        Touch(c.TriggerPath()); Touch(c.OutputPath()); const auto path=c.OutputPath();
        c.Boundary({4,2,1,false},4,Status::Missing); assert(c.GetStats().state==State::Failed && fs::file_size(path)==0);
    }
    // Exact byte bound with footer retained; finite pre-existing triggers cannot loop.
    {
        Capture c(true,root/"budget",root/"budget-trigger",Capture::Capacity,true,true,8192);
        for(const char* label:Capture::Labels) Touch(fs::path((root/"budget-trigger").string()+"."+label));
        uint64_t id=1;
        for(unsigned w=0;w<4;++w) {
            const auto path=c.OutputPath(); c.Boundary({id++,1,0,false},1,Status::Missing);
            for(unsigned n=0;n<30;++n) DrawOne(c);
            c.Boundary({id++,1,0,false},2,Status::Candidate); c.Boundary({id++,1,0,false},3,Status::Candidate);
            const auto text=Read(path); assert(fs::file_size(path)<=8192);
            assert(text.find("\"complete_instrumented_draw_window\":0")!=std::string::npos);
            assert(text.find("\"byte_budget_drops\":0")==std::string::npos);
        }
        assert(c.GetStats().state==State::Done);
    }
    {
        Capture c(true,root/"incomplete",root/"incomplete-trigger",Capture::Capacity,true,true);
        Touch(c.TriggerPath()); const auto path=c.OutputPath(); c.Boundary({1,1,0,false},1,Status::Missing);
        DrawOne(c); c.Shader()->vertex_input_seen=false;
        for(unsigned n=0;n<65;++n) c.Pending()->Image({n,0,2,5,n});
        assert(c.Pending()->image_descriptors_dropped==2);
        c.Begin(123,456); assert(!c.Shader() && c.Pending()->image_descriptor_count==0);
        c.Boundary({2,1,0,false},2,Status::Candidate); c.Boundary({3,1,0,false},3,Status::Candidate);
        const auto text=Read(path); assert(text.find("\"incomplete_shader_draws\":1")!=std::string::npos);
        assert(text.find("\"complete_instrumented_draw_window\":0")!=std::string::npos);
    }
    std::puts("Grouped capture: PASS (strict opt-in, four independent deferred windows, stable IDs, early/late triggers, repeat exhaustion, collision refusal, exact byte/draw bounds, copied vertex/descriptor metadata, missing snapshots and incomplete evidence preserved; CPU only)");
}
