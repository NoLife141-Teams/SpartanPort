// SPDX-License-Identifier: GPL-2.0-or-later
// Deterministic CPU replay of synthetic draws, accepted flips and depth samples.
// No window, process control, game file, save, GPU or NGX is used.
#include <cassert>
#include <cstdio>
#include "gow3_projection_fixture.h"
#include "gpu/shadps4/video_core/renderer_vulkan/gow3_camera_frames.h"

using namespace ProjectionFixture;
using namespace Vulkan::Gow3;
namespace {
Camera At(unsigned frame, bool minus) {
    const double t = frame < 20 ? 0 : double(frame - 20);
    Camera c{.eye={.4*std::sin(t*.025), .15*std::sin(t*.02), .2*std::cos(t*.015)},
             .yaw=.08*std::sin(t*.03), .ox=.04, .oy=-.02};
    c.depth = minus ? Depth::MinusOneToOne : Depth::Normal;
    // An explicit synthetic scene/camera discontinuity. Candidate is not a cut detector.
    if (frame >= 220) { c.eye[0] += .6; c.yaw += .12; }
    return c;
}
Matrix Adapted(const Camera& c) {
    // Test-only, explicitly chosen contract; never interpret unknown game depth this way.
    return c.depth == Depth::MinusOneToOne ? ZeroOneDepth(c.VP()) : c.VP();
}
std::array<float,36> Snapshot(const Camera& c) {
    std::array<float,36> bytes{};
    const auto m=Adapted(c);
    for (size_t i=0;i<16;++i) bytes[i]=float(m[i]);
    return bytes;
}
Viewport Extent(unsigned f) { return f>=180 && f<200 ? Viewport{0,0,1280,720} : Viewport{}; }
Point WorldAtSample(const Camera& c, Viewport v, double x, double y, double z) {
    // Independent analytic inverse camera construction, no production Inverse/Transform.
    const double vx=(2*(x+.5)/v.width-1-c.ox)*z/c.sx;
    const double vy=(1-2*(y+.5)/v.height-c.oy)*z/c.sy;
    return {c.eye[0]+std::cos(c.yaw)*vx+std::sin(c.yaw)*z,
            c.eye[1]+vy, c.eye[2]-std::sin(c.yaw)*vx+std::cos(c.yaw)*z};
}
double StoredDepth(const Camera& c, double z) {
    return c.depth==Depth::MinusOneToOne ? (z+1)*.5 : z;
}
Status Expected(unsigned f) {
    if (f==60) return Status::Missing;
    if (f==100) return Status::Invalid;
    if (f==140) return Status::Conflict;
    if (f==0 || f==61 || f==101 || f==141 || f==180 || f==200) return Status::Warming;
    return Status::Candidate;
}
}

int main() {
    size_t intervals=0, candidates=0, rejected=0, warming=0, samples=0;
    size_t stale_pair_detected=0, cuts_not_classified=0;
    double worst=0, max_stale_error=0;
    for (bool minus : {false,true}) {
        CameraFrames frames;
        FlipSequence flips;
        for (unsigned f=0;f<240;++f) {
            const auto camera=At(f,minus);
            const auto view=Extent(f);
            const auto bytes=Snapshot(camera);
            const uint64_t depth=0x1000 + (f%2)*0x1000;
            // Three observations separated by display transitions remain one camera interval.
            for (unsigned group=0;group<5;++group) {
                if (group<3) frames.Observe(f==60 ? DepthShader+1 : DepthShader,1,
                    bytes.data(),sizeof(bytes),depth,view.width,view.height);
                frames.Display(0,0,false);
                frames.Display(1,1,true);
            }
            if (f==100) frames.Observe(DepthShader,1,nullptr,144,depth,view.width,view.height,
                                       SnapshotReason::AllocationFailed);
            if (f==140) {
                auto different=bytes; different[12]+=.1f;
                frames.Observe(DepthShader,1,different.data(),144,depth,view.width,view.height);
            }
            if (f%17==0) assert(!frames.Finish({})); // rejected/no accepted request is inert
            const auto result=frames.Finish(flips.Accepted(depth,int32_t(f%2),bool(f%2)));
            assert(result && result->status==Expected(f));
            assert(result->flip.id==f+1 && frames.Completed()==f+1);
            assert(result->samples==(f==60 ? 0u : (f==100 || f==140) ? 4u : 3u));
            assert(result->unsnapshotted==(f==100 ? 1u : 0u));
            assert(result->snapshot_reasons[size_t(SnapshotReason::AllocationFailed)]==(f==100 ? 1u : 0u));
            ++intervals;
            if (result->status==Status::Warming) { ++warming; continue; }
            if (result->status!=Status::Candidate) { ++rejected; continue; }
            ++candidates;
            const auto& pair=frames.Inputs();
            const auto prior=At(f-1,minus);
            const auto prior_bytes=Snapshot(prior);
            for (size_t i=0;i<16;++i) {
                assert(pair.current[i]==double(bytes[i]));
                assert(pair.previous[i]==double(prior_bytes[i]));
            }
            for (double column : {.35,.50,.65}) for (unsigned sample=0;sample<2;++sample) {
                // Two separate surfaces and chosen sample positions: no depth resolve/average.
                const double x=std::floor(column*view.width)+(sample ? .75 : .25)-.5;
                const double y=std::floor(.45*view.height)+(sample ? .25 : .75)-.5;
                const auto world=WorldAtSample(camera,view,x,y,sample ? 25 : 5);
                const auto now=camera.Project(world,view), before=prior.Project(world,view);
                std::array<double,2> motion{};
                assert(Reproject(pair.inverse,pair.previous,x,y,StoredDepth(camera,now[2]),
                                 view.width,view.height,motion));
                const double error=Error(motion,now,before);
                worst=std::max(worst,error);
                assert(error<.002); // float snapshot quantization vs independent double oracle
                if (f<20) assert(std::hypot(motion[0],motion[1])<.002);
                ++samples;
                if (f>21 && f!=61 && f!=101 && f!=141 && f!=180 && f!=200) {
                    // Negative control: an apparently valid matrix from the wrong interval.
                    const bool ok=Reproject(pair.inverse,Adapted(At(f-2,minus)),x,y,
                        StoredDepth(camera,now[2]),view.width,view.height,motion);
                    const double stale_error=ok ? Error(motion,now,before) : 1e6;
                    max_stale_error=std::max(max_stale_error,stale_error);
                    stale_pair_detected+=stale_error>.005;
                }
            }
            if (f==220) {
                assert(result->status==Status::Candidate);
                ++cuts_not_classified; // documents a limitation; does not bless temporal blending
            }
        }
    }
    assert(intervals==480 && candidates==462 && warming==12 && rejected==6);
    assert(samples==2772 && stale_pair_detected>1000 && cuts_not_classified==2);
    std::printf("PASS synthetic offline sequence: intervals=%zu candidate=%zu warming=%zu rejected=%zu "
                "per_sample_checks=%zu max_error_pixels=%.9g stale_pair_counterexamples=%zu "
                "max_stale_error_pixels=%.9g unclassified_synthetic_cuts=%zu\n",
                intervals,candidates,warming,rejected,samples,worst,stale_pair_detected,
                max_stale_error,cuts_not_classified);
    std::puts("CPU only: explicit conventions, static world geometry, no GoW3 calibration or DLAA validation.");
}
