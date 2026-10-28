// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU tests: no game files, GPU, NGX or Vulkan headers required.
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include "gpu/shadps4/video_core/renderer_vulkan/gow3_camera_inputs.h"

using namespace Vulkan::Gow3;

static Matrix Identity() {
    Matrix result{};
    for (int i=0;i<4;++i) result[i*5]=1;
    return result;
}
static bool Near(double a, double b) { return std::abs(a-b)<1e-9; }
static std::array<float,36> Constants(const Matrix& vp=Identity()) {
    std::array<float,36> result{};
    for (size_t i=0;i<16;++i) result[i]=static_cast<float>(vp[i]);
    for (int i=0;i<4;++i) result[16+i*5]=1;
    return result;
}
static void Observe(CameraInputs& inputs, const std::array<float,36>& bytes,
                    uint64_t target=1, uint32_t w=1920, uint32_t h=1080) {
    inputs.Observe(DepthShader,1,bytes.data(),sizeof(bytes),target,w,h);
}

int main() {
    // Regression: the GoW3 capture has a declared color export, but color0 is
    // unbound. It is eligible for depth diagnostics, unlike a bound color pass.
    assert(IsDepthOnlyDraw(1,0,true,true,true));
    assert(IsDepthOnlyDraw(0,0,true,true,true));
    for (uint32_t slot=0;slot<8;++slot) {
        const uint32_t color=1u<<slot;
        assert(!IsDepthOnlyDraw(color,color,true,true,true));
        assert(!IsDepthOnlyDraw(0xff,color,true,true,true));
    }
    // Cached bindings outside this shader's exports are not active attachments.
    assert(IsDepthOnlyDraw(1,0x80,true,true,true));
    // No depth target, disabled depth test, or disabled depth writes still reject.
    assert(!IsDepthOnlyDraw(1,0,false,true,true));
    assert(!IsDepthOnlyDraw(1,0,true,false,true));
    assert(!IsDepthOnlyDraw(1,0,true,true,false));

    // Diagnostic grouping: stale display registers do not start a display
    // group. Groups no longer close camera history; accepted flips do.
    DisplayPassTracker display;
    assert(!display.Observe(1,0,true));
    assert(!display.Observe(1,1,false));
    assert(display.Observe(1,1,true));
    assert(!display.Observe(1,1,true));
    assert(!display.Observe(1,0,true));
    assert(!display.Observe(3,3,true));
    assert(display.Observe(1,1,true));
    assert(!display.Observe(1,1,true));

    const Matrix identity=Identity();
    Matrix inverse{};
    assert(Inverse(identity,inverse) && inverse==identity);
    // Non-affine matrix, zero first pivot: exercises pivoting and all four rows.
    const Matrix general{0,2,1,0, 1,0,0,1, 0,1,3,1, 1,0,2,2};
    assert(Inverse(general,inverse));
    for (int c=0;c<4;++c) {
        std::array<double,4> basis{}; basis[c]=1;
        const auto recovered=Transform(inverse,Transform(general,basis));
        for (int r=0;r<4;++r) assert(Near(recovered[r],r==c ? 1 : 0));
    }
    Matrix bad{};
    assert(!Inverse(bad,inverse));
    bad=identity; bad[0]=std::numeric_limits<double>::quiet_NaN();
    assert(!Inverse(bad,inverse));
    bad=identity; bad[5]=std::numeric_limits<double>::infinity();
    assert(!Inverse(bad,inverse));

    std::array<double,2> motion{7,8};
    assert(Reproject(identity,identity,23,17,.5,100,80,motion));
    assert(Near(motion[0],0) && Near(motion[1],0));
    Matrix translated=identity;
    translated[12]=.2; translated[13]=.25;
    assert(Reproject(identity,translated,23,17,.5,100,80,motion));
    assert(Near(motion[0],10) && Near(motion[1],-10));
    assert(Reproject(identity,translated,46,34,.5,200,160,motion));
    assert(Near(motion[0],20) && Near(motion[1],-20));
    assert(Inverse(translated,inverse));
    assert(Reproject(inverse,identity,23,17,.5,100,80,motion));
    assert(Near(motion[0],-10) && Near(motion[1],10));

    // D3D left-handed perspective, near=.5, far=100. A view translation of
    // +.1 at z=2 moves 5 pixels horizontally at x projection scale 2.
    Matrix perspective{};
    perspective[0]=2; perspective[5]=3;
    perspective[10]=100/99.5; perspective[11]=1;
    perspective[14]=-50/99.5;
    assert(Inverse(perspective,inverse));
    const double depth=perspective[10]+perspective[14]/2;
    assert(Reproject(inverse,perspective,49.5,39.5,depth,100,80,motion));
    assert(Near(motion[0],0) && Near(motion[1],0));
    auto previous=perspective;
    previous[12]=.2; previous[13]=.3;
    assert(Reproject(inverse,previous,49.5,39.5,depth,100,80,motion));
    assert(Near(motion[0],5) && Near(motion[1],-6));

    const auto reject=[&](const Matrix& inv, const Matrix& prev, double x, double y,
                          double d, uint32_t w=100, uint32_t h=80) {
        motion={7,8};
        assert(!Reproject(inv,prev,x,y,d,w,h,motion));
        assert(motion[0]==0 && motion[1]==0);
    };
    const double nan=std::numeric_limits<double>::quiet_NaN();
    for (double d : {-1.,1.,2.,nan,std::numeric_limits<double>::infinity()})
        reject(identity,identity,20,20,d);
    reject(identity,identity,-1,20,.5); reject(identity,identity,100,20,.5);
    reject(identity,identity,20,-1,.5); reject(identity,identity,20,80,.5);
    reject(identity,identity,nan,20,.5); reject(identity,identity,20,nan,.5);
    reject(identity,identity,20,20,.5,0,80); reject(identity,identity,20,20,.5,100,0);
    bad=identity; bad[15]=-1; reject(identity,bad,20,20,.5);
    bad=identity; bad[15]=0; reject(identity,bad,20,20,.5);
    bad=identity; bad[12]=4; reject(identity,bad,20,20,.5);
    bad=identity; bad[13]=4; reject(identity,bad,20,20,.5);
    bad=identity; bad[14]=1; reject(identity,bad,20,20,.5);
    bad=identity; bad[14]=-1; reject(identity,bad,20,20,.5);
    bad=identity; bad[0]=nan; reject(identity,bad,20,20,.5);
    reject(bad,identity,20,20,.5);

    const auto bytes=Constants();
    CameraInputs inputs;
    assert(inputs.FinishFrame()==Status::Missing);
    inputs.Observe(DepthShader+1,1,bytes.data(),144,1,1920,1080);
    inputs.Observe(DepthShader,0,bytes.data(),144,1,1920,1080);
    inputs.Observe(DepthShader,1,bytes.data(),143,1,1920,1080);
    inputs.Observe(DepthShader,1,bytes.data(),864,1,1920,1080);
    assert(inputs.samples==0 && inputs.FinishFrame()==Status::Missing);
    Observe(inputs,bytes); Observe(inputs,bytes);
    assert(inputs.samples==2 && inputs.current==identity);
    assert(inputs.FinishFrame()==Status::Warming && inputs.samples==0);
    Observe(inputs,bytes);
    assert(inputs.FinishFrame()==Status::Candidate);
    assert(inputs.FinishFrame()==Status::Missing);
    Observe(inputs,bytes); assert(inputs.FinishFrame()==Status::Warming);
    Observe(inputs,bytes,1,1280,720); assert(inputs.FinishFrame()==Status::Warming);
    Observe(inputs,bytes,1,1280,720); assert(inputs.FinishFrame()==Status::Candidate);

    // Byte copies must permit unaligned snapshot storage.
    std::array<unsigned char,145> unaligned{};
    std::memcpy(unaligned.data()+1,bytes.data(),144);
    inputs.Observe(DepthShader,1,unaligned.data()+1,144,1,1280,720);
    assert(inputs.current==identity && inputs.FinishFrame()==Status::Candidate);

    auto changed=bytes; changed[12]=.01f;
    for (int kind=0;kind<4;++kind) {
        Observe(inputs,bytes);
        if (kind==0) Observe(inputs,changed);
        if (kind==1) Observe(inputs,bytes,2);
        if (kind==2) Observe(inputs,bytes,1,1280,1080);
        if (kind==3) Observe(inputs,bytes,1,1920,720);
        Observe(inputs,bytes); // A later good draw cannot heal the conflicted frame.
        assert(inputs.FinishFrame()==Status::Conflict);
        Observe(inputs,bytes); assert(inputs.FinishFrame()==Status::Warming);
    }
    for (int kind=0;kind<7;++kind) {
        Observe(inputs,bytes);
        auto invalid=bytes;
        if (kind==0) invalid[0]=std::numeric_limits<float>::quiet_NaN();
        if (kind==1) invalid[35]=std::numeric_limits<float>::infinity();
        if (kind==2) invalid.fill(0);
        if (kind<3) Observe(inputs,invalid);
        if (kind==3) Observe(inputs,bytes,0);
        if (kind==4) Observe(inputs,bytes,1,0,1080);
        if (kind==5) Observe(inputs,bytes,1,1920,0);
        if (kind==6) inputs.Observe(DepthShader,1,nullptr,144,1,1920,1080);
        Observe(inputs,bytes);
        assert(inputs.FinishFrame()==Status::Invalid);
        Observe(inputs,bytes); assert(inputs.FinishFrame()==Status::Warming);
    }
    // The completed pair must survive FinishFrame; otherwise a moving camera
    // would reproject against itself and diagnostics would falsely report zero.
    CameraInputs completed;
    Observe(completed,bytes);
    assert(completed.FinishFrame()==Status::Warming);
    Matrix moved=identity; moved[12]=.125; moved[13]=-.25;
    Observe(completed,Constants(moved));
    assert(completed.FinishFrame()==Status::Candidate);
    assert(completed.previous==identity && completed.current==moved);
    const std::array<double,4> point{.2,.3,.5,1};
    const auto roundtrip=Transform(completed.inverse,Transform(completed.current,point));
    for (size_t i=0;i<4;++i) assert(Near(roundtrip[i],point[i]));
    assert(Reproject(completed.inverse,completed.previous,960,540,.5,
                     completed.width,completed.height,motion));
    assert(Near(motion[0],-120) && Near(motion[1],-135));
    Observe(completed,bytes);
    assert(completed.FinishFrame()==Status::Candidate);
    assert(completed.previous==moved && completed.current==identity);
    assert(std::strcmp(Name(Status::Candidate),"camera-candidate")==0);
    std::puts("GoW3 camera input CPU tests passed (synthetic inputs; not native DLAA validation)");
}
