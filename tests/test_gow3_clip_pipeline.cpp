// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic contracts, not a decoding of the GoW3 vertex program.
#include <cassert>
#include <cstdio>
#include "gow3_projection_fixture.h"
using namespace ProjectionFixture;
using Vulkan::Gow3::Inverse;
using Vulkan::Gow3::Transform;
namespace {
using Clip = std::array<double,4>;
struct RasterViewport { double x,y,width,height,low,high; };
bool Inside(Clip p,bool minus) {
    return p[3]>0 && std::abs(p[0])<=p[3] && std::abs(p[1])<=p[3] &&
           p[2]>=(minus?-p[3]:0) && p[2]<=p[3];
}
Clip Convert(Clip p) { p[2]=(p[2]+p[3])*.5; return p; }
Point Raster(Clip p,RasterViewport v,bool minus) {
    const double z=p[2]/p[3], unit=minus?(z+1)*.5:z;
    return {v.x+(p[0]/p[3]+1)*v.width*.5,
            v.y+(p[1]/p[3]+1)*v.height*.5,v.low+unit*(v.high-v.low)};
}
Point Unproject(Point p,const Matrix& inv,RasterViewport v,bool minus) {
    assert(v.width!=0 && v.height!=0 && v.high!=v.low);
    const double unit=(p[2]-v.low)/(v.high-v.low);
    const auto h=Transform(inv,{2*(p[0]-v.x)/v.width-1,
        2*(p[1]-v.y)/v.height-1,minus?2*unit-1:unit,1});
    assert(std::abs(h[3])>1e-8);
    return {h[0]/h[3],h[1]/h[3],h[2]/h[3]};
}
double Distance(Point a,Point b) {
    return std::hypot(a[0]-b[0],a[1]-b[1],a[2]-b[2]);
}
}
int main() {
    size_t clips=0,points=0; double worst=0;
    for(double w:{.5,1.,4.}) for(double z:{-1.25,-1.,-.5,0.,.5,1.,1.25}) {
        const Clip p{.25*w,-.25*w,z*w,w};
        assert(Inside(p,true)==Inside(Convert(p),false));
        if(z<0 && z>=-1) assert(Inside(p,true) && !Inside(p,false));
        ++clips;
    }
    const Camera camera{.eye={.3,-.2,.4},.yaw=.03,.ox=.08,.oy=-.03,
                        .depth=Depth::MinusOneToOne};
    Matrix inv{},converted_inv{};
    assert(Inverse(camera.VP(),inv)); assert(Inverse(ZeroOneDepth(camera.VP()),converted_inv));
    for(const auto range: {std::array{0.,1.},std::array{.2,.8},std::array{.8,.2}})
    for(bool negative:{false,true}) {
        const RasterViewport v{32,negative?474.:24.,800,negative?-450.:450.,range[0],range[1]};
        for(double z:{2.,9.,23.,70.}) for(double x:{-.4,.1,.7}) for(double y:{-.2,.1,.4}) {
            const Point world{x,y,z}; const auto oracle=camera.Project(world,{32,24,800,450});
            const auto clip=Transform(camera.VP(),{x,y,z,1});
            assert(Inside(clip,true));
            const auto native=Raster(clip,v,true),fallback=Raster(Convert(clip),v,false);
            assert(Distance(native,fallback)<1e-10);
            // Independent world -> camera oracle, without matrix multiplication.
            const double expected_y=negative?oracle[1]+.5:24+450-(oracle[1]+.5-24);
            const Point expected{oracle[0]+.5,expected_y,
                range[0]+(oracle[2]+1)*.5*(range[1]-range[0])};
            assert(Distance(native,expected)<1e-9);
            const double error=Distance(Unproject(native,inv,v,true),world);
            worst=std::max(worst,error); assert(error<1e-9);
            assert(Distance(Unproject(native,converted_inv,v,false),world)<1e-9);
            ++points;
        }
    }
    const Clip p{.2,.6,-.5,1};
    const RasterViewport negative{0,1080,1920,-1080,0,1},positive{0,0,1920,1080,0,1};
    const auto n=Raster(p,negative,true),q=Raster(p,positive,true);
    assert(Near(n[0],q[0]) && Near(n[1]+q[1],1080) && Near(n[2],q[2]));
    assert(Near(n[2],.25)); // not z/w=-.5; height sign does not alter depth
    assert(Near(Raster(Convert(p),negative,true)[2],.625)); // double conversion is wrong
    const Point world{.7,.2,7}; const auto clip=Transform(camera.VP(),{world[0],world[1],world[2],1});
    const auto pixel=Raster(clip,negative,true);
    assert(Distance(Unproject(pixel,inv,negative,false),world)>1);
    assert(Distance(Unproject(pixel,inv,positive,true),world)>.1);
    // Guest range low=offset-scale/high=offset+scale; clamping endpoints changes
    // the map. Final viewport values do not uniquely identify guest registers.
    for(double z:{-.75,0.,.75}) {
        const Clip c{0,0,z,1};
        assert(Near(Raster(c,{0,0,1,1,-1,1},true)[2],z));
        assert(Near(Raster(c,{0,0,1,1,0,1},true)[2],(z+1)*.5));
    }
    // Constant depth range is not invertible.
    assert(Near(Raster({0,0,-.5,1},{0,0,1,1,.4,.4},true)[2],.4));
    assert(Near(Raster({0,0,.8,1},{0,0,1,1,.4,.4},true)[2],.4));
    std::printf("Clip pipeline: %zu clipping cases, %zu independent world/viewport cases; "
                "max inverse error %.9g; direct/double depth and Y counterexamples PASS\n",
                clips,points,worst);
}
