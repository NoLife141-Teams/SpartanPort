// SPDX-License-Identifier: GPL-2.0-or-later
// Local test files in the caller's isolated test-data directory; no GPU/game.
#include <cassert>
#include <barrier>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/gow3_frame_trace.h"
using namespace Vulkan::Gow3;
using namespace Vulkan::Gow3::Trace;
namespace fs=std::filesystem;

static void Touch(const fs::path& file, const char* contents="trigger") {
    std::ofstream out(file); assert(out); out << contents; out.close(); assert(out);
}
static std::string Read(const fs::path& file) {
    std::ifstream in(file); assert(in);
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
static std::vector<std::vector<std::string>> Rows(const fs::path& file) {
    std::istringstream in(Read(file)); std::string line;
    std::vector<std::vector<std::string>> rows;
    while (std::getline(in,line)) {
        if (line.empty() || line[0]=='#' || line.starts_with("seq,")) continue;
        std::istringstream record(line); std::vector<std::string> fields;
        std::string field; while (std::getline(record,field,',')) fields.push_back(field);
        rows.push_back(fields);
    }
    return rows;
}
static Event Interval(uint64_t id=77, uint64_t interval=11) {
    return {.kind=Kind::Interval,.result=Result::Observed,
            .flip={id,0x2000,1,false},.interval=interval,.draw=1234,
            .boundary={.number=interval-1}};
}
int main(int argc, char** argv) {
    if (argc==2 && (std::strcmp(argv[1],"--runtime-before")==0 ||
                    std::strcmp(argv[1],"--runtime-after")==0)) {
        if (!Enabled()) return 10;
        const fs::path output=std::getenv("SPARTAN_GOW3_FRAME_TRACE_FILE");
        const fs::path trigger=std::getenv("SPARTAN_GOW3_FRAME_TRACE_TRIGGER");
        assert(!fs::exists(output));
        Write({.kind=Kind::Present,.flip={3,0x1000,0,false}});
        if (std::strcmp(argv[1],"--runtime-after")==0) {
            assert(!fs::exists(trigger));
            WriteOrderedInterval(Interval(499,36));
            assert(!fs::exists(output)); Touch(trigger);
        }
        WriteOrderedInterval(Interval(500,37)); Flush();
        const auto rows=Rows(output); assert(rows.size()==2);
        assert(rows[0][2]=="start" && rows[0][4]=="500" && rows[0][8]=="37");
        assert(rows[1][3]=="incomplete");
        return 0;
    }
    assert(!Enabled());
    const auto root=fs::absolute("trace-trigger-tests");
    assert(fs::create_directory(root)); // refuse to overwrite an earlier run
    const auto before=root/"before.trigger", before_out=root/"before.csv";
    Touch(before); // trigger present before process/session initialization
    {
        Session s(true,before_out,before);
        assert(s.Enabled() && s.Stats().state==SessionState::Armed);
        assert(!fs::exists(before_out));
        assert(!s.Write({.kind=Kind::Queued,.flip={7,0x1000,0,true}}));
        assert(!s.OrderedInterval({.kind=Kind::Queued,.flip={77,0x2000,1,false}}));
        assert(!s.OrderedInterval(Interval(0)));
        assert(!fs::exists(before_out)); // only an accepted ordered interval starts
        const auto event=Interval();
        assert(s.OrderedInterval(event));
        assert(event.result==Result::Observed); // caller/camera data not modified
        auto rows=Rows(before_out); assert(rows.size()==2);
        assert(rows[0][2]=="start" && rows[0][4]=="77" && rows[0][8]=="11");
        assert(rows[1][2]=="interval" && rows[1][3]=="incomplete");
        assert(rows[1][4]=="77" && rows[1][8]=="11" && rows[1][9]=="1234");
        Touch(before,"repeated");
        assert(!s.OrderedInterval(Interval(78,12)));
        rows=Rows(before_out); assert(rows.size()==3);
        assert(rows[2][4]=="78" && rows[2][3]=="observed");
        assert(Read(before)=="repeated"); // trigger is never deleted or altered
    }

    // Long startup consumes no journal capacity, but preserves all identities,
    // draw serials and camera history. Activation does not warm/reset camera.
    const auto after=root/"after.trigger", after_out=root/"after.csv";
    {
        Session s(true,after_out,after);
        Accumulator a; CameraFrames camera; FlipSequence ids;
        std::array<float,36> data{}; for(int i=0;i<4;++i) data[i*5]=data[16+i*5]=1;
        const auto finish=[&] {
            a.Draw(true,true); a.Snapshot(true);
            camera.Observe(DepthShader,1,data.data(),144,7,1920,1080);
            const auto flip=ids.Accepted(0x1000,0,false);
            const auto frame=camera.Finish(flip); assert(frame);
            return a.Finish(flip,{.number=frame->number,.samples=frame->samples,
                                 .status=uint32_t(frame->status)});
        };
        for(int i=0;i<64;++i) assert(!s.OrderedInterval(finish()));
        for(int i=0;i<20000;++i) assert(!s.Write({.kind=Kind::Present,.flip={1,0x1000,0,false}}));
        assert(!fs::exists(after_out) && !s.Stats().dropped && !s.Stats().retained);
        Touch(after);
        auto first=finish(); assert(first.flip.id==65 && first.interval==65 && first.draw==65);
        assert(first.boundary.status==uint32_t(Status::Candidate));
        assert(s.OrderedInterval(first));
        assert(camera.Completed()==65 && camera.Inputs().status==Status::Candidate);
        // Delayed/reused old presentations retain old identities. They cannot
        // be associated with a new interval just because capture began later.
        assert(s.Write({.kind=Kind::Present,.result=Result::Reused,.flip={1,0x1000,0,false}}));
        assert(!s.OrderedInterval(finish()));
        auto rows=Rows(after_out); assert(rows.size()==4);
        assert(rows[0][4]=="65" && rows[1][3]=="incomplete");
        assert(rows[2][2]=="present" && rows[2][4]=="1" && rows[2][3]=="reused");
        assert(rows[3][4]=="66" && rows[3][8]=="66" && rows[3][3]=="observed");
    }

    // Disabled means no activation or output even if a valid trigger exists.
    {
        Session s(false,root/"disabled.csv",before);
        assert(!s.Enabled() && !s.OrderedInterval(Interval()));
        assert(!s.Write(Interval()) && !fs::exists(root/"disabled.csv"));
    }
    // File collision between arming and activation: preserve it and fail once.
    {
        const auto output=root/"collision.csv";
        Session s(true,output,before); Touch(output,"sentinel");
        assert(!s.OrderedInterval(Interval()));
        assert(s.Stats().state==SessionState::Failed && !s.Enabled());
        assert(!s.OrderedInterval(Interval(78)) && !s.Write(Interval()));
        assert(Read(output)=="sentinel");
    }
    {
        Session relative(true,root/"relative.csv","relative.trigger");
        Session same(true,before,before);
        Session missing_parent(true,root/"absent"/"output.csv",before);
        assert(!relative.Enabled() && !same.Enabled());
        assert(!missing_parent.OrderedInterval(Interval()));
        assert(missing_parent.Stats().state==SessionState::Failed);
        assert(Read(before)=="repeated");
        const auto directory=root/"directory.trigger"; assert(fs::create_directory(directory));
        Session not_file(true,root/"directory.csv",directory);
        assert(!not_file.OrderedInterval(Interval()) && !fs::exists(root/"directory.csv"));
    }
    // Immediate mode remains available and does not require a trigger.
    {
        Session s(true,root/"immediate.csv");
        assert(s.Stats().state==SessionState::Active);
        assert(s.Write(Interval())); s.Flush();
        assert(Rows(root/"immediate.csv").size()==1);
    }
    // Concurrent writers race activation. Start + first interval must remain
    // the first two adjacent records; no writer can publish before the start.
    {
        const auto trigger=root/"race.trigger", output=root/"race.csv";
        Session s(true,output,trigger,1024);
        std::barrier ready(5); std::vector<std::thread> threads;
        for(int t=0;t<4;++t) threads.emplace_back([&,t] {
            for(int i=0;i<10;++i) assert(!s.Write({.kind=Kind::Queued,.flip={uint64_t(t+1),0,0,false}}));
            ready.arrive_and_wait(); ready.arrive_and_wait();
            for(int i=0;i<100;++i) {
                s.Write({.kind=Kind::Present,.result=Result::Presented,.flip={uint64_t(100+t),0,0,false}});
                if(i%10==0) s.Flush();
            }
        });
        ready.arrive_and_wait(); assert(!fs::exists(output)); Touch(trigger);
        ready.arrive_and_wait(); assert(s.OrderedInterval(Interval(90)));
        for(auto& t:threads) t.join(); s.Flush();
        const auto rows=Rows(output); assert(rows.size()>=2 && rows.size()<=402);
        assert(rows[0][2]=="start" && rows[1][2]=="interval" && rows[1][3]=="incomplete");
        assert(rows[0][4]=="90" && rows[1][4]=="90");
        assert(!s.Stats().errors && !s.Stats().dropped);
    }
    // After activation, every concurrent loss is counted exactly, including
    // the two activation records. Repeated triggers never reset the budget.
    {
        Session s(true,root/"bounded.csv",before,13);
        assert(s.OrderedInterval(Interval()));
        std::vector<std::thread> threads;
        for(int t=0;t<4;++t) threads.emplace_back([&] {
            for(int i=0;i<100;++i) s.Write({.kind=Kind::Present,.flip={12,0,0,false}});
        });
        for(auto& t:threads) t.join();
        assert(s.Stats().retained==13 && s.Stats().dropped==389 && !s.Stats().errors);
        Touch(before,"again"); assert(!s.OrderedInterval(Interval(78)));
        assert(s.Stats().retained==13 && s.Stats().dropped==390);
        assert(Rows(root/"bounded.csv").size()==13);
    }
    std::puts("GoW3 deferred trace: PASS (before/after-start trigger, ordered boundary, unchanged IDs/history, repeated/disabled/collision, delayed events, concurrent activation and bounded losses; CPU only)");
}
