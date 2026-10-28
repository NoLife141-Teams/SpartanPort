// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic capture-state/serialization tests. No Vulkan or game data.
#include <cassert>
#include <bit>
#include <fstream>
#include <iterator>
#include "gpu/shadps4/video_core/renderer_vulkan/gow3_pair_capture.h"
using namespace Vulkan::Gow3;
using namespace Vulkan::Gow3::Pair;
namespace fs=std::filesystem;
static void Touch(const fs::path& p) { std::ofstream f(p); assert(bool(f)); }
static std::string Read(const fs::path& p) {
    std::ifstream f(p); assert(bool(f)); return {std::istreambuf_iterator<char>(f),{}};
}
static void Ready(Capture& c) {
    auto* d=c.Pending(); assert(d); d->viewport_count=1;
    d->viewports[0].bits={0,0,std::bit_cast<uint32_t>(1920.f),std::bit_cast<uint32_t>(-1080.f),0,std::bit_cast<uint32_t>(1.f)};
    d->viewports[0].extent={1920,1080}; c.Recorded();
}
int main(int argc,char** argv) {
    assert(argc==2); const auto root=fs::absolute(argv[1]);
    assert(!fs::exists(root)); fs::create_directories(root);
    assert(!Requested(nullptr,"1") && !Requested("1",nullptr) && !Requested("1","true") && !Requested("01","1"));
    assert(Requested("1","1"));
    assert(!ShaderRequested("1","1",nullptr) && !ShaderRequested("1","1","true"));
    assert(!ShaderRequested("0","1","1") && !ShaderRequested("1","0","1"));
    assert(ShaderRequested("1","1","1"));
    const auto trigger=root/"trigger";
    {
        Capture disabled(false,root/"disabled.jsonl",trigger);
        disabled.Begin(1,2); disabled.Boundary({1,55,0,false},0,Status::Candidate);
        assert(disabled.GetStats().state==State::Disabled && !fs::exists(root/"disabled.jsonl"));
    }
    { Capture invalid(true,"relative",trigger); assert(invalid.GetStats().state==State::Failed); }
    { Capture invalid(true,trigger,trigger); assert(invalid.GetStats().state==State::Failed); }
    { Capture invalid(true,root/"big",trigger,Capture::Capacity+1); assert(invalid.GetStats().state==State::Failed); }
    const Attachment first{.uid=1,.alias_generation=7,.image=11,.view=12,.address=55,.cache_samples=2,.host_samples=2,.width=1920,.height=1080};
    Attachment replacement=first; replacement.uid=2; replacement.image=21; replacement.alias_generation=8;
    std::array<uint32_t,36> words{}; words[0]=0x80000000; words[1]=0x7fc12345; words[35]=0x12345678;
    {
        Capture c(true,root/"pair.jsonl",trigger);
        c.Begin(10,20); assert(!c.Pending()); c.Boundary({1,55,0,false},0,Status::Missing);
        assert(!fs::exists(root/"pair.jsonl")); Touch(trigger);
        c.Boundary({},1,Status::Missing); assert(c.GetStats().state==State::Armed);
        c.Boundary({2,55,0,false},1,Status::Warming); assert(c.GetStats().state==State::Active);
        c.Begin(DepthShader,0); Ready(c);
        assert(!c.Shader()); // separate shader detail opt-in is off by default
        c.Snapshot(words.data(),144,SnapshotReason::Copied,first); words[35]=0;
        c.Pending()->depth=first; c.Pending()->color_count=1; c.Pending()->colors[0]=replacement;
        c.Begin(DepthShader,0); Ready(c);
        c.Snapshot(nullptr,144,SnapshotReason::AllocationFailed,replacement);
        c.Pending()->depth=replacement;
        c.Boundary({100,55,0,false},2,Status::Invalid);
        c.Begin(DepthShader,0); Ready(c);
        c.Snapshot(nullptr,143,SnapshotReason::NoBinding,first); assert(!c.Pending()->camera_observed);
        c.Snapshot(nullptr,144,SnapshotReason::AllocationFailed,first);
        c.Snapshot(words.data(),144,SnapshotReason::Copied,first);
        assert(!c.Pending()->snapshot_present && c.Pending()->snapshot_conflict);
        assert(c.Pending()->snapshot_observations==2 && c.Pending()->snapshot_missing==1);
        c.Begin(123,456); // abandoned bind attempt: no viewport/recorded flag
        c.Boundary({3,55,0,true},3,Status::Invalid); // EOP identity can be numerically earlier
        const auto stats=c.GetStats(); assert(stats.state==State::Done && stats.retained==4 && stats.closed_intervals==2);
        c.Begin(999,999); c.Boundary({4,55,0,false},4,Status::Candidate);
        assert(c.GetStats().retained==4 && c.GetStats().closed_intervals==2);
    }
    const auto pair=Read(root/"pair.jsonl");
    assert(pair.find("\"80000000\",\"7fc12345\"")!=std::string::npos);
    assert(pair.find("\"12345678\"")!=std::string::npos); // exact copied bits, not later source
    assert(pair.find("\"pre_ui_color_proven\":false")!=std::string::npos);
    assert(pair.find("\"pixels_captured\":false")!=std::string::npos);
    assert(pair.find("ring-space-unavailable")!=std::string::npos);
    {
        Capture collision(true,root/"pair.jsonl",trigger); collision.Boundary({1,1,0,false},0,Status::Missing);
        assert(collision.GetStats().state==State::Failed && Read(root/"pair.jsonl")==pair);
    }
    {
        Capture c(true,root/"saturated.jsonl",trigger,1);
        c.Boundary({1,1,0,false},0,Status::Missing);
        c.Begin(1,2); Ready(c); c.Begin(2,3); Ready(c);
        c.Boundary({2,1,0,false},1,Status::Candidate);
        c.Begin(3,4); Ready(c); c.Boundary({3,2,1,false},2,Status::Candidate);
        assert(c.GetStats().retained==1 && c.GetStats().dropped==2);
    }
    assert(Read(root/"saturated.jsonl").find("\"complete_instrumented_draw_window\":0")!=std::string::npos);
    {
        Capture c(true,root/"partial.jsonl",trigger); c.Boundary({1,1,0,false},0,Status::Missing);
        c.Begin(1,2); Ready(c); // process shutdown before any complete interval
    }
    assert(Read(root/"partial.jsonl").find("\"closed_intervals\":0")!=std::string::npos);
    {
        Capture c(true,root/"overflow.jsonl",trigger); c.Boundary({1,1,0,false},0,Status::Missing);
        c.Begin(1,2); Ready(c); c.Pending()->viewport_overflow=true;
        c.Boundary({2,1,0,false},1,Status::Candidate); c.Boundary({3,1,0,false},2,Status::Missing);
    }
    assert(Read(root/"overflow.jsonl").find("\"complete_instrumented_draw_window\":0")!=std::string::npos);
    {
        Capture c(true,root/"duplicate.jsonl",trigger); c.Boundary({1,1,0,false},0,Status::Missing);
        c.Begin(1,2); Ready(c); c.Boundary({1,1,0,false},0,Status::Missing);
        assert(c.GetStats().state==State::Failed);
    }
    fs::create_directory(root/"directory-trigger");
    { Capture c(true,root/"directory.jsonl",root/"directory-trigger"); c.Boundary({1,1,0,false},0,Status::Missing); assert(c.GetStats().state==State::Armed); }
    assert(!fs::exists(root/"directory.jsonl"));
    {
        Capture c(true,root/"shader.jsonl",trigger,Capture::Capacity,true);
        c.Begin(DepthShader,20); assert(!c.Shader()); // armed interval is excluded
        c.Boundary({1,1,0,false},0,Status::Missing);
        c.Begin(DepthShader,20); Ready(c);
        auto* s=c.Shader(); assert(s); s->vs_key=0x101; s->ps_key=0x201;
        s->dynamic_seen=true; s->native_clip_control=true; s->depth_clamp_key=true;
        s->bias_enabled=true; s->bias_bits={0x80000000,0x3f800000,0x3d800000};
        s->bounds_enabled=true; s->bounds_bits={0x3e800000,0x3f400000};
        ShaderBinding b{1,7,3,0,99,512,144,true}; s->Binding(b); b.range=0;
        c.Snapshot(nullptr,144,SnapshotReason::AllocationFailed,first);
        c.Begin(999,20); Ready(c); assert(!c.Shader()); // exact shader guard
        c.Boundary({2,2,1,true},1,Status::Invalid);
        c.Begin(DepthShader,21); Ready(c); s=c.Shader(); assert(s);
        assert(!s->vs_key && !s->bias_enabled && !s->binding_count && !s->dynamic_seen);
        s->vs_key=0x102; s->ps_key=0x202; s->dynamic_seen=true;
        s->Binding({1,8,4,0,100,1024,144,false});
        c.Boundary({3,3,2,false},2,Status::Candidate);
        assert(c.GetStats().state==State::Done);
    }
    const auto shader=Read(root/"shader.jsonl");
    assert(shader.find("gow3-pair-metadata-v2")!=std::string::npos);
    assert(shader.find("\"vs_permutation_key\":\"101\",\"ps_permutation_key\":\"201\"")!=std::string::npos);
    assert(shader.find("\"vs_permutation_key\":\"102\",\"ps_permutation_key\":\"202\"")!=std::string::npos);
    assert(shader.find("\"range\":144,\"ring_copy\":1")!=std::string::npos);
    assert(shader.find("\"bias_bits\":[\"80000000\",\"3f800000\",\"3d800000\"]")!=std::string::npos);
    assert(shader.find("\"bias_enabled\":0,\"bias_bits\":[\"0\",\"0\",\"0\"]")!=std::string::npos);
    assert(shader.find("ring-space-unavailable")!=std::string::npos); // details do not heal missing snapshots
    assert(shader.find("\"fragment_depth_export\":\"inspect_exact_ps_spirv\"")!=std::string::npos);
    assert(shader.find("\"incomplete_shader_draws\":0")!=std::string::npos);
    {
        Capture c(true,root/"shader-incomplete.jsonl",trigger,Capture::Capacity,true);
        c.Boundary({1,1,0,false},0,Status::Missing);
        c.Begin(DepthShader,20); Ready(c); // missing bindings/dynamic/key stay unknown
        c.Begin(DepthShader,20); Ready(c); auto* s=c.Shader();
        s->vs_key=1; s->dynamic_seen=true;
        for(unsigned i=0;i<33;++i) s->Binding({i,i,i,0,9,0,144,true});
        assert(s->binding_count==32 && s->bindings_dropped==1);
        c.Boundary({2,1,0,false},1,Status::Candidate); c.Boundary({3,1,0,false},2,Status::Missing);
    }
    const auto incomplete=Read(root/"shader-incomplete.jsonl");
    assert(incomplete.find("\"bindings_dropped\":1")!=std::string::npos);
    assert(incomplete.find("\"incomplete_shader_draws\":2")!=std::string::npos);
    assert(incomplete.find("\"complete_instrumented_draw_window\":0")!=std::string::npos);
    std::puts("GoW3 pair metadata: PASS (strict opt-in, deferred ordered pair, exact bits, copied IDs, missing/conflicting snapshots, aborted draws, reused addresses, EOP order, capacity, partial/duplicate boundaries, no overwrite; CPU only)");
    std::puts("Target shader metadata: PASS (separate opt-in, exact shader, selected keys, binding copies, per-draw state reset, bias/bounds bits, unknown PS export, missing snapshots preserved, bounded overflow/incomplete evidence)");
}
