// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic matrix/affine/palette contracts, not game shader code or assets.
#include <bit>
#include <cassert>
#include <cstdio>
#include <span>
#include "gow3_projection_fixture.h"
using namespace ProjectionFixture;
using Words=std::array<uint32_t,36>;
using Affine=std::array<float,16>;
struct Attributes { std::array<float,4> position,weights; std::array<uint32_t,4> indices; };
struct TransformSpec {
    double angle,sx,sy,sz,tx,ty,tz;
    Point Apply(Point p) const {
        return {std::cos(angle)*sx*p[0]-std::sin(angle)*sy*p[1]+tx,
                std::sin(angle)*sx*p[0]+std::cos(angle)*sy*p[1]+ty,sz*p[2]+tz};
    }
    Affine Values() const {
        return {float(std::cos(angle)*sx),float(std::sin(angle)*sx),0,0,
                float(-std::sin(angle)*sy),float(std::cos(angle)*sy),0,0,
                0,0,float(sz),0,float(tx),float(ty),float(tz),1};
    }
};
// A safe TEST-ONLY mathematical evaluator. It does not emulate eager GPU loads,
// robust-buffer-access behavior or vertex input format conversion.
bool Evaluate(const Words& words,const Attributes& a,std::span<const Affine> palette,
              std::array<float,4>& clip) {
    std::array<float,36> c{};
    for(size_t i=0;i<c.size();++i) c[i]=std::bit_cast<float>(words[i]);
    Affine t{};
    if(words[32]) {
        for(uint32_t index:a.indices) if(index>=palette.size()) return false;
        for(unsigned column=0;column<4;++column) for(unsigned row=0;row<3;++row) {
            const unsigned k=column*4+row;
            t[k]=a.weights[3]*palette[a.indices[3]][k]+(a.weights[2]*palette[a.indices[2]][k]+
                 (a.weights[0]*palette[a.indices[0]][k]+a.weights[1]*palette[a.indices[1]][k]));
        }
    } else for(unsigned i=0;i<16;++i) t[i]=c[16+i];
    std::array<float,3> world{};
    for(unsigned r=0;r<3;++r)
        world[r]=t[r]*a.position[0]+(t[4+r]*a.position[1]+(t[8+r]*a.position[2]+1.f*t[12+r]));
    for(unsigned r=0;r<4;++r)
        clip[r]=c[r]*world[0]+(c[4+r]*world[1]+(c[8+r]*world[2]+c[12+r]));
    return true;
}
int main() {
    const Camera camera{.eye={.1,-.15,.3},.yaw=.04,.ox=.06,.oy=-.025,.depth=Depth::MinusOneToOne};
    const Camera previous{.eye={-.2,.1,-.1},.yaw=-.03,.ox=-.02,.oy=.02,.depth=Depth::MinusOneToOne};
    const std::array<TransformSpec,4> specs{{{.12,1.1,.9,1.2,.2,-.1,1},
        {-.07,.8,1.05,1.1,-.2,.1,.5},{.03,1.2,.85,.95,.1,.2,1.5},{-.1,.95,1.15,1.05,-.1,-.15,.7}}};
    std::array<Affine,4> palette{};
    for(size_t i=0;i<4;++i) palette[i]=specs[i].Values();
    Words words{}; const auto vp=camera.VP();
    for(unsigned i=0;i<16;++i) { words[i]=std::bit_cast<uint32_t>(float(vp[i])); words[16+i]=std::bit_cast<uint32_t>(palette[0][i]); }
    size_t cases=0; double max_error=0;
    for(uint32_t flag:{0u,1u,0x80000000u}) for(double z:{2.,5.,12.,35.})
    for(double x:{-.5,0.,.5}) for(double y:{-.3,0.,.3}) for(bool normalized:{false,true}) {
        const Point p{x,y,z}; const std::array<float,4> weights=normalized?std::array<float,4>{.1f,.2f,.3f,.4f}:std::array<float,4>{.2f,.4f,.6f,.8f};
        const Attributes a{{float(x),float(y),float(z),17},weights,{2,0,3,1}};
        words[32]=flag;
        Point world=specs[0].Apply(p);
        if(flag) {
            world={};
            for(unsigned i=0;i<4;++i) { const auto v=specs[a.indices[i]].Apply(p);
                for(unsigned r=0;r<3;++r) world[r]+=double(a.weights[i])*v[r]; }
        }
        const auto projected=camera.Project(world);
        std::array<float,4> clip{}; assert(Evaluate(words,a,palette,clip));
        const double nx=clip[0]/double(clip[3]),ny=clip[1]/double(clip[3]),nz=clip[2]/double(clip[3]);
        const double error=std::max({std::abs(nx-(2*(projected[0]+.5)/960-1)),
                                    std::abs(ny-(1-2*(projected[1]+.5)/540)),std::abs(nz-projected[2])});
        max_error=std::max(max_error,error); assert(error<3e-7); ++cases;
        auto ignored=words; for(unsigned k:{19u,23u,27u,31u,33u,34u,35u}) ignored[k]=0x7fc12345;
        auto other=a; other.position[3]=-999; std::array<float,4> same{};
        assert(Evaluate(ignored,other,palette,same) && clip==same);
        if(flag) { for(unsigned i=16;i<32;++i) ignored[i]=0x7fc12345;
            assert(Evaluate(ignored,a,palette,same) && clip==same); }
    }
    // Near-plane fixture: exported MinusW clip Z is negative; no shader depth remap.
    const Camera near_camera{.near=1,.far=10,.depth=Depth::MinusOneToOne};
    const auto near_vp=near_camera.VP(); const TransformSpec identity{0,1,1,1,0,0,0};
    const auto ident=identity.Values();
    for(unsigned i=0;i<16;++i) { words[i]=std::bit_cast<uint32_t>(float(near_vp[i])); words[16+i]=std::bit_cast<uint32_t>(ident[i]); }
    words[32]=0; std::array<float,4> clip{};
    Attributes a{{.1f,.1f,1.5f,55},{0,0,0,1},{0,0,0,0}};
    assert(Evaluate(words,a,palette,clip)); assert(clip[2]<0 && clip[3]>0);
    const double q=clip[2]/double(clip[3]); assert(std::abs(q-(q+1)*.5)>.1);
    // Nonzero BIT PATTERN selects the palette even for float negative zero.
    words[32]=0x80000000; std::array<float,4> skinned{}; assert(Evaluate(words,a,palette,skinned)); assert(skinned!=clip);
    // The default-input permutation's index bit pattern is not a numeric float->int conversion.
    const uint32_t default_one=std::bit_cast<uint32_t>(1.f);
    assert(default_one==0x3f800000 && uint32_t(default_one*16u)==0xf8000000u);
    a.indices[3]=default_one; assert(!Evaluate(words,a,palette,skinned));
    // Object movement cannot be recovered by camera-only history.
    Matrix inverse{}; assert(Vulkan::Gow3::Inverse(ZeroOneDepth(camera.VP()),inverse));
    const Point before_world=specs[0].Apply({.2,.1,6}); Point now_world=before_world; now_world[0]+=.5;
    const auto now=camera.Project(now_world), before=previous.Project(before_world), static_before=previous.Project(now_world);
    std::array<double,2> camera_motion{};
    assert(Vulkan::Gow3::Reproject(inverse,ZeroOneDepth(previous.VP()),now[0],now[1],(now[2]+1)*.5,960,540,camera_motion));
    assert(Error(camera_motion,now,static_before)<1e-8 && Error(camera_motion,now,before)>1);
    std::printf("Vertex contract: %zu independent affine/palette/MinusW cases PASS, max NDC error %.9g; "
                "column layout, forced homogeneous W, ignored tail, raw flag, unnormalized weights, "
                "invalid default indices and object-motion counterexamples PASS (synthetic CPU only)\n",cases,max_error);
}
