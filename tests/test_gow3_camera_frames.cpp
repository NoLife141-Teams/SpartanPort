// SPDX-License-Identifier: GPL-2.0-or-later
// CPU-only ordering model using the production camera/route/queue-rule helpers.
// It does not simulate GPU execution or prove native callback scheduling.
#include <array>
#include <cassert>
#include <cstdio>
#include <deque>
#include <functional>
#include <vector>
#include "gpu/shadps4/video_core/renderer_vulkan/gow3_frame_trace.h"

using namespace Vulkan::Gow3;
static std::array<float,36> Constants(float x=0) {
    std::array<float,36> data{};
    for (int i=0;i<4;++i) data[i*5]=data[16+i*5]=1;
    data[12]=x;
    return data;
}

struct Model {
    CameraFrames camera;
    FlipSequence ids;
    Trace::Accumulator trace;
    Trace::Presentation presentation;
    Trace::Journal* journal = nullptr;
    std::deque<std::function<void()>> tasks;
    std::vector<CameraFrame> completed;
    bool pipe = true;

    void Drain() {
        while (!tasks.empty()) {
            auto task=std::move(tasks.front()); tasks.pop_front(); task();
        }
    }
    void Draw(float x=0, bool snapshot=true, uint64_t shader=DepthShader, uint64_t depth=7) {
        tasks.push_back([=,this] {
            const auto bytes=Constants(x);
            camera.Observe(shader,1,snapshot ? bytes.data() : nullptr,144,depth,1920,1080);
            if (journal) {
                trace.Draw(shader==DepthShader,true);
                if (shader==DepthShader) trace.Snapshot(snapshot);
            }
        });
    }
    void DisplayGroups(int groups) {
        tasks.push_back([=,this] {
            for (int i=0;i<groups;++i) {
                assert(!camera.Display(1,0,true));
                const auto group=camera.Display(1,1,true); assert(group);
                assert(!camera.Display(1,1,true));
                if (journal) journal->Write(trace.Display({.samples=group->samples,
                    .unsnapshotted=group->unsnapshotted, .status=uint32_t(group->status)}));
            }
        });
    }
    void Consume(Flip flip) {
        const auto frame=camera.Finish(flip); assert(frame);
        completed.push_back(*frame);
        if (journal) journal->Write(trace.Finish(flip,{.samples=frame->samples,
            .unsnapshotted=frame->unsnapshotted,.status=uint32_t(frame->status)}));
    }
    void Accepted(bool stage_b, bool eop, int buffer, uint64_t address, uint64_t pending) {
        if (!CanAcceptFlip(buffer,pending)) return;
        const auto flip=ids.Accepted(address,buffer,eop);
        switch (RouteFlip(!stage_b,stage_b)) {
        case FlipRoute::OrderedTask:
            // Model RunInOrder: copy token into the FIFO, or drain then execute.
            if (pipe) tasks.push_back([=,this] { Consume(flip); });
            else { Drain(); Consume(flip); }
            break;
        case FlipRoute::Consume: Consume(flip); break;
        case FlipRoute::Unordered: assert(false); break;
        }
    }
    void Submit(bool eop, int buffer=0, uint64_t pending=0) {
        const uint64_t address=buffer<0 ? 0 : 0x1000u+uint64_t(buffer)*0x1000;
        if (eop && pipe) {
            // SignalFlip invokes SubmitFlip synchronously on ordered stage B.
            tasks.push_back([=,this] { Accepted(true,eop,buffer,address,pending); });
        } else {
            // Non-EOP arrives on stage A via SendCommand; serial EOP already
            // follows preceding draws. Both use RunInOrder on stage A.
            if (eop) Drain();
            Accepted(false,eop,buffer,address,pending);
        }
    }
};

int main() {
    assert(RouteFlip(true,false)==FlipRoute::OrderedTask);
    assert(RouteFlip(false,true)==FlipRoute::Consume);
    assert(RouteFlip(false,false)==FlipRoute::Unordered);
    assert(CanAcceptFlip(0,16) && !CanAcceptFlip(0,17));
    assert(CanAcceptFlip(-1,17));
    assert(!Trace::Enabled()); // runtime tracing disabled for these tests

    // Old display-driven closure demonstrably destroys the valid interval.
    for (int groups=2;groups<=5;++groups) {
        CameraInputs old;
        const auto data=Constants(); old.Observe(DepthShader,1,data.data(),144,7,1920,1080);
        assert(old.FinishFrame()==Status::Warming);
        for (int i=1;i<groups;++i) assert(old.FinishFrame()==Status::Missing);
    }

    // Cross product: EOP/non-EOP, pipelined/serial, trace off/on/saturated,
    // 2..5 display transitions within one accepted request. One closure only.
    for (bool pipe : {false,true}) for (bool eop : {false,true})
    for (int logging=0;logging<3;++logging) for (int groups=2;groups<=5;++groups) {
        std::FILE* file=std::tmpfile(); assert(file);
        {
            Trace::Journal journal(file,logging==2 ? 1 : 128);
            Model m; m.pipe=pipe; if (logging) m.journal=&journal;
            m.Draw(); m.DisplayGroups(groups); m.Submit(eop); m.Drain();
            assert(m.completed.size()==1 && m.camera.Completed()==1);
            assert(m.completed[0].status==Status::Warming && m.completed[0].samples==1);
            m.Draw(.125f); m.DisplayGroups(groups); m.Submit(eop,1); m.Drain();
            assert(m.completed.size()==2 && m.camera.Completed()==2);
            assert(m.completed[1].status==Status::Candidate && m.completed[1].samples==1);
            assert(m.completed[1].flip.buffer==1 && m.completed[1].flip.address==0x2000);
            assert(m.completed[1].flip.eop==eop);
            assert(m.camera.Inputs().previous[12]==0 && m.camera.Inputs().current[12]==.125);
            if (logging==2) assert(journal.Dropped()>0);
        }
        std::fclose(file);
    }

    for (bool eop : {false,true}) {
        Model m;
        m.Draw(); m.Submit(eop); m.Drain();
        // Rejection cannot reset pending samples/history or consume an ID.
        m.Draw(); m.Submit(eop,0,17); m.Drain();
        assert(m.camera.Completed()==1 && m.camera.Inputs().samples==1);
        assert(!m.camera.Finish({}));
        m.Submit(eop,0,16); m.Drain();
        assert(m.completed.back().flip.id==2 && m.completed.back().status==Status::Candidate);
        // True interval without the expected shader must remain missing.
        m.Draw(0,true,DepthShader+1); m.DisplayGroups(3); m.Submit(eop); m.Drain();
        assert(m.completed.back().status==Status::Missing && m.completed.back().samples==0);
        m.Draw(); m.Submit(eop); m.Drain();
        assert(m.completed.back().status==Status::Warming);
        // A later good snapshot does not heal a missing one in this interval.
        m.Draw(0,false); m.DisplayGroups(5); m.Draw(); m.Submit(eop); m.Drain();
        assert(m.completed.back().status==Status::Invalid);
        assert(m.completed.back().samples==2 && m.completed.back().unsnapshotted==1);
        m.Draw(); m.Submit(eop); m.Drain();
        assert(m.completed.back().status==Status::Warming);
        // Display groups must not hide conflicting matrices or depth targets.
        for (bool depth_change : {false,true}) {
            m.Draw(); m.DisplayGroups(2);
            m.Draw(depth_change ? 0 : .25f,true,DepthShader,depth_change ? 8 : 7);
            m.Draw(); m.Submit(eop); m.Drain();
            assert(m.completed.back().status==Status::Conflict);
            assert(m.completed.back().samples==3 && !m.completed.back().unsnapshotted);
            m.Draw(); m.Submit(eop); m.Drain();
            assert(m.completed.back().status==Status::Warming);
        }
        // Delayed/reused presentation never closes inputs or consumes pending draws.
        const auto old=m.completed.front().flip;
        m.Draw(); m.Drain();
        const auto count=m.camera.Completed();
        m.presentation.Complete(old,false,Trace::Result::Presented);
        for (int i=0;i<5;++i) {
            const auto reused=m.presentation.Complete(m.presentation.ReuseSource(),true,
                                                       Trace::Result::Presented);
            assert(reused.flip.id==old.id && reused.result==Trace::Result::Reused);
            assert(m.camera.Completed()==count && m.camera.Inputs().samples==1);
        }
        m.Submit(eop,1); m.Drain();
        assert(m.completed.back().status==Status::Candidate);
        // A NEW accepted flip of the same buffer does close an empty interval.
        m.Submit(eop,1); m.Drain();
        assert(m.camera.Completed()==count+2 && m.completed.back().status==Status::Missing);
        m.Submit(eop,-1,17); m.Drain(); // accepted blank request
        assert(m.completed.back().flip.buffer==-1 && m.completed.back().status==Status::Missing);
    }
    // Token allocation order may differ from consumer order across A/B. Never
    // drop a valid request by comparing IDs to a global high-water mark.
    CameraFrames frames;
    FlipSequence ids;
    const auto a=ids.Accepted(0x1000,0,false), b=ids.Accepted(0x2000,1,true);
    assert(frames.Finish(b)->flip.id==2);
    assert(frames.Finish(a)->flip.id==1 && frames.Completed()==2);
    std::puts("GoW3 camera frames: PASS (2..5 display groups, EOP/non-EOP ordered/serial, trace off/full, rejection, absence, snapshot failure, conflicts, delayed/reused presentation; CPU only)");
}
