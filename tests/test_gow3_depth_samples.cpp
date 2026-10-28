// SPDX-License-Identifier: GPL-2.0-or-later
// Tests of candidate sampling contracts, NOT an MSAA resolve implementation.
#include <cassert>
#include <cstdio>
#include <limits>
#include "gow3_projection_fixture.h"
using namespace Vulkan::Gow3;
using namespace ProjectionFixture;

int main() {
    const Camera current{};
    const Camera previous{.eye={-.2,.1,-.3}, .yaw=.07};
    Matrix inv{}; assert(Inverse(current.VP(),inv));
    // A synthetic 2-sample pixel; positions are arbitrary, not read from GoW3.
    const std::array<std::array<double,2>,2> locations{{{.25,.75},{.75,.25}}};
    const auto point_at=[&](size_t sample,double z) -> Point {
        const auto s=locations[sample];
        return {((400+s[0])/960*2-1)*z/current.sx,
                (1-(240+s[1])/540*2)*z/current.sy,z};
    };
    std::array<double,2> depths{}, horizontal{};
    for (size_t i=0;i<2;++i) {
        const auto world=point_at(i,i==0 ? 2 : 20);
        const auto now=current.Project(world), before=previous.Project(world);
        depths[i]=now[2];
        std::array<double,2> motion{};
        assert(Reproject(inv,previous.VP(),now[0],now[1],now[2],960,540,motion));
        assert(Error(motion,now,before)<1e-8); horizontal[i]=motion[0];
        // Substituting the pixel center for a sample position changes motion
        // under rotation/forward translation. Retain the sample index/position.
        assert(Reproject(inv,previous.VP(),400,240,now[2],960,540,motion));
        assert(Error(motion,now,before)>.001);
    }
    assert(std::abs(horizontal[0]-horizontal[1])>1);
    assert(depths[0]<depths[1]); // nearest is min only under normal-Z assumptions
    auto reverse=current; reverse.depth=Depth::Reversed;
    const auto near_reverse=reverse.Project(point_at(0,2))[2];
    const auto far_reverse=reverse.Project(point_at(1,20))[2];
    assert(near_reverse>far_reverse); // reversed-Z nearest is max
    assert(Near(depths[0]+near_reverse,1) && Near(depths[1]+far_reverse,1));

    // Averaging nonlinear depth from two surfaces synthesizes a third surface.
    // No choice (sample 0/min/max/mean) identifies the resolved color's surface
    // without the matching coverage/color and occlusion information.
    const auto [a,b]=current.DepthCoefficients();
    const double average_depth=(depths[0]+depths[1])/2;
    const double average_z=b/(average_depth-a);
    assert(Near(average_z,40./11.));
    assert(!Near(average_z,2) && !Near(average_z,20) && !Near(average_z,11));
    const auto point_center=[&](double z) -> Point {
        return {((400.5)/960*2-1)*z/current.sx,(1-240.5/540*2)*z/current.sy,z};
    };
    std::array<double,2> mean_motion{};
    assert(Reproject(inv,previous.VP(),400,240,average_depth,960,540,mean_motion));
    for (double z : {2.,20.}) {
        const auto world=point_center(z);
        assert(Error(mean_motion,current.Project(world),previous.Project(world))>1);
    }
    const auto invented=point_center(average_z);
    assert(Error(mean_motion,current.Project(invented),previous.Project(invented))<1e-8);
    // A uniform plane is a control: identical sample depths have no resolve
    // ambiguity, although subpixel positions still differ.
    assert(Near(current.Project(point_at(0,8))[2],current.Project(point_at(1,8))[2]));

    // Sample 0 can be background while sample 1 is foreground; swapping labels
    // changes sample-zero selection but cannot change min/max. Clear handling
    // must be separate from a reduction and must be calibrated, not inferred.
    const std::array<double,2> edge{1,depths[0]}, swapped{depths[0],1};
    std::array<double,2> motion{};
    assert(!Reproject(inv,previous.VP(),400,240,edge[0],960,540,motion));
    assert(Reproject(inv,previous.VP(),400,240,swapped[0],960,540,motion));
    assert(std::min(edge[0],edge[1])==std::min(swapped[0],swapped[1]));
    assert(std::max(edge[0],edge[1])==std::max(swapped[0],swapped[1]));
    const double clear_average=(1+depths[0])/2;
    const double invented_z=b/(clear_average-a);
    assert(invented_z>2 && invented_z<current.far);
    assert(Reproject(inv,previous.VP(),400,240,clear_average,960,540,motion));
    assert(!Reproject(inv,previous.VP(),400,240,std::numeric_limits<double>::quiet_NaN(),960,540,motion));
    assert(!Reproject(inv,previous.VP(),400,240,std::numeric_limits<double>::infinity(),960,540,motion));
    std::printf("Depth sample contracts: two surfaces z=2/20 average to phantom z=%.9f; "
                "sample-position, clear and normal/reversed-Z counterexamples passed (CPU only)\n",average_z);
}
