// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic CPU events only; no Vulkan, guest memory, game files or effects.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/gow3_frame_trace.h"

using namespace Vulkan::Gow3::Trace;
static void Prime(Accumulator& a) {
    assert(a.Finish({1,0x1000,0,true}).result == Result::Incomplete);
}
static std::string Read(std::FILE* file) {
    std::rewind(file);
    std::string text;
    char line[2048];
    while (std::fgets(line,sizeof(line),file)) text+=line;
    return text;
}
int main(int argc, char** argv) {
    if (argc==2 && std::strcmp(argv[1],"--sink-smoke")==0) {
        if (!Enabled()) return 10;
        Vulkan::Gow3::FlipSequence ids;
        const auto token=ids.Accepted(0x1000,0,true);
        assert(token.id);
        Write({.kind=Kind::Queued,.flip=token});
        Flush();
        return 0;
    }
    // Tracing stays off by default; accepted request identities are independent.
    assert(!Enabled());
    Vulkan::Gow3::FlipSequence ids;
    assert(ids.Accepted(0x1000,0,true).id==1);
    Accumulator a;
    Prime(a);
    a.Draw(true,true); a.Snapshot(true);
    const auto first=a.Display({.number=10,.color=0x1000,.samples=1});
    a.Draw(false,false);
    const auto second=a.Display({.number=11,.color=0x1000,.samples=0});
    const auto extra=a.Finish({2,0x1000,0,true});
    assert(first.interval==second.interval && second.interval==extra.interval);
    assert(extra.result==Result::EmptyDisplayWithCamera);
    assert(extra.counts.transitions==2 && extra.counts.empty_transitions==1);
    assert(extra.counts.snapshots==1 && extra.counts.unsnapshotted==0);

    a.Draw(false,false); a.Display({.samples=0});
    const auto absent=a.Finish({3,0x2000,1,true});
    assert(absent.result==Result::ShaderAbsent && absent.counts.shader==0);
    a.Draw(true,false); a.Display({.samples=0});
    assert(a.Finish({4,0x3000,2,true}).result==Result::Filtered);
    a.Draw(true,true);
    assert(a.Finish({5,0x1000,0,true}).result==Result::NoBinding);
    a.Draw(true,true); a.Snapshot(false);
    assert(a.Finish({6,0x1000,0,true}).result==Result::SnapshotMissing);
    assert(a.Finish({7,0x1000,0,true}).result==Result::NoDraws);

    // Buffer changes carry explicit IDs, including a non-EOP request. A later
    // presentation of old A must not be relabelled B or its recording interval.
    a.Draw(true,true); a.Snapshot(true); a.Display({.samples=1});
    const auto frame_a=a.Finish({8,0x1000,0,true});
    a.Draw(true,true); a.Snapshot(true); a.Display({.samples=1});
    const auto frame_b=a.Finish({9,0x2000,1,false},
                               {.number=8,.samples=1,.status=uint32_t(Vulkan::Gow3::Status::Candidate)});
    assert(frame_a.result==Result::Observed && frame_b.result==Result::Observed);
    assert(frame_b.boundary.number==8 && frame_b.boundary.samples==1 && frame_b.boundary.status==4);
    assert(frame_a.interval+1==frame_b.interval);
    Presentation presentation;
    assert(presentation.ReuseSource().id==0);
    const auto delayed=presentation.Complete(frame_a.flip,false,Result::Presented);
    const auto reused=presentation.Complete(presentation.ReuseSource(),true,Result::Presented);
    assert(delayed.flip.id==8 && delayed.flip.address==0x1000);
    assert(reused.flip.id==8 && reused.flip.address!=frame_b.flip.address);
    assert(reused.result==Result::Reused);
    const auto skipped=presentation.Complete(frame_b.flip,false,Result::Minimized);
    assert(skipped.flip.id==9 && skipped.result==Result::Minimized);
    assert(presentation.ReuseSource().id==9);
    a.Draw(true,true); a.Snapshot(true); a.Display({.samples=1}); a.Display({.samples=1});
    assert(a.Finish({10,0x2000,1,true}).result==Result::MultipleDisplayGroups);

    std::FILE* file=std::tmpfile(); assert(file);
    {
        Journal journal(file,4);
        assert(journal.Write(frame_a));
        assert(journal.Write(frame_b));
        assert(journal.Write(delayed));
        assert(journal.Write(reused));
        assert(journal.Complete());
        assert(!journal.Write(extra));
        journal.Flush();
        assert(journal.Retained()==4 && journal.Dropped()==1 && journal.Errors()==0);
        assert(!journal.Complete());
        auto text=Read(file);
        assert(text.find("gow3-frame-trace-v4")!=std::string::npos);
        assert(text.find("dropped=00000000000000000001")!=std::string::npos);
        assert(text.find(",present,presented,8,0,4096,1,")!=std::string::npos);
        assert(text.find(",present,reused,8,0,4096,1,")!=std::string::npos);
        assert(text.find(",interval,observed,9,1,8192,0,")!=std::string::npos);
        assert(text.size()<1024*4+1024);
    }
    std::fclose(file);
    // Concurrent producers retain complete records and count every loss.
    file=std::tmpfile(); assert(file);
    {
        Journal journal(file,13);
        std::vector<std::thread> threads;
        for (int i=0;i<4;++i) threads.emplace_back([&] {
            for (int j=0;j<20;++j) journal.Write(delayed);
        });
        for (auto& t:threads) t.join();
        journal.Flush();
        assert(journal.Retained()==13 && journal.Dropped()==67 && journal.Errors()==0);
        assert(Read(file).find("dropped=00000000000000000067")!=std::string::npos);
    }
    std::fclose(file);
    Journal unavailable(nullptr,4);
    assert(!unavailable.Write(extra) && unavailable.Errors()==1 && unavailable.Dropped()==1);
    assert(!unavailable.Complete());
    std::puts("GoW3 frame trace: PASS (display groups, absence/filtering, explicit flip IDs, delayed/reused presentation, bounded concurrent loss accounting; CPU only)");
}
