// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "gow3_camera_inputs.h"

namespace Vulkan::Gow3 {
// Explicit framebuffer/sample contract. Raw GoW3 VP matrices must not be fed
// to the older ZeroToW/full-viewport reference without converting their depth.
enum class ClipDepth { ZeroToW, MinusWToW };
struct ReprojectionViewport {
    double x{},y{},width{},height{},min_depth{},max_depth{1};
    ClipDepth clip=ClipDepth::MinusWToW;
    bool Valid() const {
        return std::isfinite(x)&&std::isfinite(y)&&std::isfinite(width)&&std::isfinite(height)&&
            std::isfinite(min_depth)&&std::isfinite(max_depth)&&width>0&&height!=0&&min_depth!=max_depth;
    }
    bool Contains(double sx,double sy) const {
        return sx>=x&&sx<x+width&&sy>=std::min(y,y+height)&&sy<std::max(y,y+height);
    }
};
inline constexpr Matrix IdentityMatrix{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
struct ReprojectedSample { double x{},y{},depth{}; };

// x/y are framebuffer sample locations, NOT integer pixel indices. The caller
// supplies measured MSAA positions, or explicitly labels a center approximation.
// world_current_to_previous is identity only for geometry fixed in world space.
inline bool ReprojectSample(const Matrix& inverse_current,const Matrix& previous,
    const ReprojectionViewport& current_view,const ReprojectionViewport& previous_view,
    double x,double y,double depth,ReprojectedSample& out,
    const Matrix& world_current_to_previous=IdentityMatrix,double clear_depth=1) {
    out={};
    if(!current_view.Valid()||!previous_view.Valid()||!std::isfinite(x)||!std::isfinite(y)||
       !std::isfinite(depth)||depth==clear_depth||!current_view.Contains(x,y)) return false;
    double z=(depth-current_view.min_depth)/(current_view.max_depth-current_view.min_depth);
    if(z<0||z>1) return false;
    if(current_view.clip==ClipDepth::MinusWToW) z=2*z-1;
    const auto world=Transform(inverse_current,{2*(x-current_view.x)/current_view.width-1,
        2*(y-current_view.y)/current_view.height-1,z,1});
    const auto clip=Transform(previous,Transform(world_current_to_previous,world));
    for(auto v:clip) if(!std::isfinite(v)) return false;
    if(clip[3]<=1e-8) return false;
    const double nx=clip[0]/clip[3],ny=clip[1]/clip[3],nz=clip[2]/clip[3];
    if(nx< -1||nx>1||ny< -1||ny>1||nz>1||nz<(previous_view.clip==ClipDepth::MinusWToW?-1:0)) return false;
    const double normalized_depth=previous_view.clip==ClipDepth::MinusWToW?(nz+1)/2:nz;
    out={previous_view.x+(nx+1)*previous_view.width/2,
         previous_view.y+(ny+1)*previous_view.height/2,
         previous_view.min_depth+normalized_depth*(previous_view.max_depth-previous_view.min_depth)};
    return previous_view.Contains(out.x,out.y);
}
} // namespace Vulkan::Gow3
