// SPDX-License-Identifier: GPL-2.0-or-later
// Offline contract tests, not a calibration of the captured game matrices.
#include <cassert>
#include <cstdio>
#include "gow3_projection_fixture.h"
using namespace Vulkan::Gow3;
using namespace ProjectionFixture;

int main() {
    const Camera current{.eye={.3,-.2,.4}, .yaw=.03, .ox=.08, .oy=-.03};
    const Camera previous{.eye={-.1,.1,-.2}, .yaw=-.04, .ox=-.02, .oy=.04};
    Matrix inv{};
    assert(Inverse(current.VP(), inv));
    size_t cases=0;
    double worst=0;
    // Independent forward projections of known static world points. Different
    // rotations/translations, off-center projection, depths and aspect ratios
    // prevent identity-only tests from hiding a convention error.
    for (Viewport v : {Viewport{}, Viewport{0,0,1920,1080}, Viewport{0,0,803,617}}) {
        for (double z : {4.,9.,23.,70.}) for (double x : {-1.,0.,1.})
        for (double y : {-.4,0.,.4}) {
            const Point point{x,y,z};
            const auto now=current.Project(point,v), before=previous.Project(point,v);
            std::array<double,2> motion{};
            assert(Reproject(inv,previous.VP(),now[0],now[1],now[2],v.width,v.height,motion));
            const double error=Error(motion,now,before);
            worst=std::max(worst,error); assert(error<1e-8); ++cases;
            assert(Reproject(inv,current.VP(),now[0],now[1],now[2],v.width,v.height,motion));
            assert(std::hypot(motion[0],motion[1])<1e-8);
        }
    }
    const Point point{.7,.2,7};
    const auto now=current.Project(point), before=previous.Project(point);
    std::array<double,2> motion{};
    // A self-pair cancels even a consistently transposed matrix: zero motion is
    // not evidence for storage convention. The moving pair below distinguishes it.
    Matrix transposed_inv{};
    assert(Inverse(Transpose(current.VP()),transposed_inv));
    assert(Reproject(transposed_inv,Transpose(current.VP()),now[0],now[1],now[2],960,540,motion));
    assert(std::hypot(motion[0],motion[1])<1e-8);
    const bool wrong_layout=Reproject(transposed_inv,Transpose(previous.VP()),
                                     now[0],now[1],now[2],960,540,motion);
    assert(!wrong_layout || Error(motion,now,before)>1);
    // Flipping the input Y but leaving matrices/output unchanged is detectable.
    const bool wrong_y=Reproject(inv,previous.VP(),now[0],539-now[1],now[2],960,540,motion);
    assert(!wrong_y || Error(motion,now,before)>1);
    assert(Reproject(inv,previous.VP(),now[0],now[1],now[2],960,540,motion));
    assert(Error({-motion[0],-motion[1]},now,before)>1); // wrong vector sign
    const auto correct=motion;
    assert(Reproject(inv,previous.VP(),now[0]+.5,now[1]+.5,now[2],960,540,motion));
    assert(std::hypot(motion[0]-correct[0],motion[1]-correct[1])>.001);

    // The API assumes a full viewport and equal current/previous dimensions.
    // This TEST-ONLY adapter normalizes each viewport independently; it is not
    // wired into rendering and is not permission to accept resolution changes.
    const Viewport cv{32,24,800,450}, pv{80,40,1000,600};
    const auto n=current.Project(point,cv), p=previous.Project(point,pv);
    assert(Reproject(inv,previous.VP(),n[0]-cv.left,n[1]-cv.top,n[2],cv.width,cv.height,motion));
    const std::array<double,2> adapted{
        pv.left+(n[0]-cv.left+motion[0]+.5)*pv.width/cv.width-.5-n[0],
        pv.top+(n[1]-cv.top+motion[1]+.5)*pv.height/cv.height-.5-n[1]};
    assert(Error(adapted,n,p)<1e-8);
    assert(Error(motion,n,p)>1);
    assert(Reproject(inv,previous.VP(),n[0],n[1],n[2],1024,768,motion));
    assert(Error(motion,n,p)>1); // allocation extent is not the active viewport

    // A [-1,1] clip projection may still be stored as [0,1] attachment depth.
    // Passing that value directly to the unconverted VP gives a wrong result.
    auto gl_current=current, gl_previous=previous;
    gl_current.depth=gl_previous.depth=Depth::MinusOneToOne;
    const auto gn=gl_current.Project(point), gp=gl_previous.Project(point);
    const double stored_depth=(gn[2]+1)/2;
    Matrix gl_inv{};
    assert(Inverse(gl_current.VP(),gl_inv));
    const bool wrong_depth=Reproject(gl_inv,gl_previous.VP(),gn[0],gn[1],stored_depth,960,540,motion);
    assert(!wrong_depth || Error(motion,gn,gp)>1);
    assert(Inverse(ZeroOneDepth(gl_current.VP()),gl_inv));
    assert(Reproject(gl_inv,ZeroOneDepth(gl_previous.VP()),gn[0],gn[1],stored_depth,960,540,motion));
    assert(Error(motion,gn,gp)<1e-8);
    // A non-default depth range likewise needs a verified inverse mapping.
    assert(Reproject(inv,previous.VP(),now[0],now[1],.2+.6*now[2],960,540,motion));
    assert(Error(motion,now,before)>1);
    assert(Reproject(inv,previous.VP(),now[0],now[1],((.2+.6*now[2])-.2)/.6,960,540,motion));
    assert(Error(motion,now,before)<1e-8);

    // Reversed-Z interior geometry works with its matching VP, but the current
    // hard-coded background rule is normal-Z: zero clear can be accepted, near=1
    // rejected. Document that limitation rather than enabling reversed-Z.
    auto rc=current, rp=previous;
    rc.depth=rp.depth=Depth::Reversed;
    Matrix ri{}; assert(Inverse(rc.VP(),ri));
    const auto rn=rc.Project(point), rprev=rp.Project(point);
    assert(Reproject(ri,rp.VP(),rn[0],rn[1],rn[2],960,540,motion));
    assert(Error(motion,rn,rprev)<1e-8);
    // Exact binary coefficients separate the clear policy from roundoff at a
    // clip boundary. An arbitrary rotated VP can put nz infinitesimally below
    // zero there and be rejected by the existing strict bounds instead.
    const Camera clear_case{.sx=2, .sy=2, .near=1, .far=2, .depth=Depth::Reversed};
    assert(Inverse(clear_case.VP(),ri));
    assert(Reproject(ri,clear_case.VP(),400,200,0,960,540,motion));
    assert(!Reproject(ri,clear_case.VP(),400,200,1,960,540,motion));
    std::printf("Reprojection contract: %zu forward-oracle cases, max error %.3g pixels; "
                "convention counterexamples passed (synthetic only)\n",cases,worst);
}
