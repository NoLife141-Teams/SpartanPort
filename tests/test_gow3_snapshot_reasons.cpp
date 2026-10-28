// SPDX-License-Identifier: GPL-2.0-or-later
// CPU-only diagnostic provenance/guard tests. No guest memory or GPU access.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numeric>
#include <string>
#include "gpu/shadps4/video_core/renderer_vulkan/gow3_frame_trace.h"
using namespace Vulkan::Gow3;

int main() {
    for (const char* value : std::array<const char*,9>{nullptr,"","0","01","true","yes","1 "," 1","-1"})
        assert(!IsEnabledValue(value));
    assert(IsEnabledValue("1"));
    assert(!InputDiagnosticsEnabled() && !Trace::Enabled());

    std::array<float,36> bytes{};
    for (int i=0;i<4;++i) bytes[i*5]=bytes[16+i*5]=1;
    const auto sum=[](const SnapshotCounts& c) { return std::accumulate(c.begin(),c.end(),0u); };
    FlipSequence ids;
    CameraFrames camera;
    // Every missing-snapshot reason remains invalid after subsequent good data;
    // the producer's reason follows this observation, not a latest global value.
    for (uint32_t i=uint32_t(SnapshotReason::NoPacket);i<uint32_t(SnapshotReason::Count);++i) {
        const auto reason=SnapshotReason(i);
        camera.Observe(DepthShader,1,nullptr,144,7,1920,1080,reason);
        camera.Display(1,0,true); camera.Display(1,1,true);
        camera.Observe(DepthShader,1,bytes.data(),144,7,1920,1080,SnapshotReason::Copied);
        const auto frame=camera.Finish(ids.Accepted(0x1000,0,false));
        assert(frame && frame->status==Status::Invalid);
        assert(frame->samples==2 && frame->unsnapshotted==1);
        assert(frame->snapshot_reasons[i]==1 && sum(frame->snapshot_reasons)==2);
        assert(frame->snapshot_reasons[size_t(SnapshotReason::Copied)]==1);
        camera.Observe(DepthShader,1,bytes.data(),144,7,1920,1080,reason);
        const auto next=camera.Finish(ids.Accepted(0x2000,1,true));
        assert(next->status==Status::Warming && next->unsnapshotted==0);
        assert(sum(next->snapshot_reasons)==1);
        assert(next->snapshot_reasons[size_t(SnapshotReason::Copied)]==1);
    }
    assert(ResolveSnapshotReason(false,false,SnapshotReason::AllocationFailed)==SnapshotReason::NoPacket);
    assert(ResolveSnapshotReason(false,true,SnapshotReason::Copied)==SnapshotReason::CopyMissing);
    assert(ResolveSnapshotReason(false,true,SnapshotReason::None)==SnapshotReason::Unknown);
    assert(ResolveSnapshotReason(false,true,SnapshotReason(999))==SnapshotReason::Unknown);
    // A reported successful producer copy cannot silently become a good input
    // if its binding is absent on the consumer.
    camera.Observe(DepthShader,1,nullptr,144,7,1920,1080,SnapshotReason::Copied);
    camera.Observe(DepthShader,1,nullptr,144,7,1920,1080,SnapshotReason::PendingWrite);
    camera.Observe(DepthShader,1,nullptr,144,7,1920,1080,SnapshotReason::AllocationFailed);
    camera.Observe(DepthShader+1,1,nullptr,144,7,1920,1080,SnapshotReason::AllocationFailed);
    camera.Observe(DepthShader,0,nullptr,144,7,1920,1080,SnapshotReason::AllocationFailed);
    camera.Observe(DepthShader,1,nullptr,143,7,1920,1080,SnapshotReason::AllocationFailed);
    const auto frame=camera.Finish(ids.Accepted(0x1000,0,false));
    assert(frame->status==Status::Invalid && frame->samples==3 && frame->unsnapshotted==3);
    assert(sum(frame->snapshot_reasons)==3);
    assert(frame->snapshot_reasons[size_t(SnapshotReason::CopyMissing)]==1);
    assert(frame->snapshot_reasons[size_t(SnapshotReason::PendingWrite)]==1);
    assert(frame->snapshot_reasons[size_t(SnapshotReason::AllocationFailed)]==1);
    assert(sum(camera.Finish(ids.Accepted(0x1000,0,false))->snapshot_reasons)==0);

    SnapshotCounts maximum; maximum.fill(std::numeric_limits<uint32_t>::max());
    const auto formatted=FormatSnapshotCounts(maximum);
    assert(std::strlen(formatted.data())<formatted.size()-1);
    assert(std::strstr(formatted.data(),"unknown:4294967295"));
    assert(!std::strchr(formatted.data(),',') && !std::strchr(formatted.data(),'\n'));
    assert(FormatSnapshotCounts({})[0]=='\0');
    auto* file=std::tmpfile(); assert(file);
    {
        Trace::Journal journal(file,1);
        assert(journal.Write({.kind=Trace::Kind::Interval,.flip=frame->flip,
            .boundary={.samples=frame->samples,.unsnapshotted=frame->unsnapshotted,
                       .status=uint32_t(frame->status),.snapshot_reasons=frame->snapshot_reasons}}));
        assert(!journal.Write({})); journal.Flush();
        assert(journal.Dropped()==1 && !journal.Errors());
        std::rewind(file); std::string text; char line[1024];
        while (std::fgets(line,sizeof(line),file)) text+=line;
        assert(text.find("gow3-frame-trace-v4")!=std::string::npos);
        assert(text.find("snapshot_reasons")!=std::string::npos);
        assert(text.find("queued-write-overlap:1;ring-space-unavailable:1;copied-binding-missing:1")!=std::string::npos);
        assert(text.size()<2048);
    }
    std::fclose(file);
    std::puts("GoW3 snapshot reasons: PASS (strict opt-in, transported reasons, no healing, exact-input guards, per-interval reset, bounded CSV; CPU only)");
}
