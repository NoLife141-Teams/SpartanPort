// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_projection_fixture.h"
#include "gpu/shadps4/video_core/renderer_vulkan/gow3_reprojection.h"
#include <cassert>
#include <cstdio>
using namespace Vulkan::Gow3;
using namespace ProjectionFixture;
int main() {
    size_t cases=0;
    for(auto mode:{Depth::Normal,Depth::MinusOneToOne}) for(double height:{540.,-540.})
    for(double minimum:{0.,.2}) for(double z:{2.,5.,12.,35.}) {
        Camera now{.eye={.1,-.15,.3},.yaw=.04,.ox=.06,.oy=-.025,.depth=mode};
        Camera before{.eye={-.2,.1,-.1},.yaw=-.03,.ox=-.02,.oy=.02,.depth=mode};
        const ReprojectionViewport view{11,height<0?553.:13.,960,height,minimum,.9,
            mode==Depth::MinusOneToOne?ClipDepth::MinusWToW:ClipDepth::ZeroToW};
        const Point point{.2,.1,z};
        // Analytic forward camera oracle supplies NDC independently of the inverse helper.
        const auto forward=[&](const Camera& camera,Point p) {
            const auto v=camera.Project(p);
            const double nx=2*(v[0]+.5)/960-1,ny=1-2*(v[1]+.5)/540;
            return ReprojectedSample{view.x+(nx+1)*view.width/2,view.y+(ny+1)*view.height/2,
                minimum+(.9-minimum)*(mode==Depth::MinusOneToOne?(v[2]+1)/2:v[2])};
        };
        const auto current=forward(now,point),expected=forward(before,point);
        Matrix inverse{}; assert(Inverse(now.VP(),inverse)); ReprojectedSample out;
        assert(ReprojectSample(inverse,before.VP(),view,view,current.x,current.y,current.depth,out));
        assert(std::hypot(out.x-expected.x,out.y-expected.y)<1e-8 && std::abs(out.depth-expected.depth)<1e-10);
        // Independently moving mesh: camera-only is wrong; known object history repairs it.
        Point moved=point; moved[0]+=.4; const auto moving=forward(now,moved);
        assert(ReprojectSample(inverse,before.VP(),view,view,moving.x,moving.y,moving.depth,out));
        assert(std::hypot(out.x-expected.x,out.y-expected.y)>1);
        auto object=IdentityMatrix; object[12]=-.4;
        assert(ReprojectSample(inverse,before.VP(),view,view,moving.x,moving.y,moving.depth,out,object));
        assert(std::hypot(out.x-expected.x,out.y-expected.y)<1e-8);
        assert(!ReprojectSample(inverse,before.VP(),view,view,current.x,current.y,1,out));
        auto invalid=view; invalid.height=0;
        assert(!ReprojectSample(inverse,before.VP(),invalid,view,current.x,current.y,current.depth,out));
        ++cases;
    }
    std::printf("PASS calibrated sample reprojection: %zu analytic camera/viewport/depth cases; moving-object controls; explicit sample coordinates\n",cases);
}
