// SPDX-License-Identifier: GPL-2.0-or-later
// Offline CPU bridge: no renderer/device/game linkage. Input is numeric camera
// matrices, viewports and framebuffer sample coordinates; output is numeric TSV.
#include "video_core/renderer_vulkan/gow3_reprojection.h"
#include <iomanip>
#include <iostream>
int main() {
    using namespace Vulkan::Gow3;
    ReprojectionViewport views[2]; Matrix matrices[2];
    for(auto& view:views) {
        int clip{};
        if(!(std::cin>>view.x>>view.y>>view.width>>view.height>>view.min_depth>>view.max_depth>>clip)||
           (clip!=0&&clip!=1)) return 2;
        view.clip=clip?ClipDepth::MinusWToW:ClipDepth::ZeroToW;
        if(!view.Valid()) return 2;
    }
    for(auto& matrix:matrices) for(auto& v:matrix) if(!(std::cin>>v)||!std::isfinite(v)) return 2;
    Matrix inverse{}; if(!Inverse(matrices[1],inverse)) return 2;
    std::cout<<std::setprecision(17);
    double x{},y{},depth{}; size_t count=0;
    while(std::cin>>x) {
        if(!(std::cin>>y>>depth)) return 2;
        if(++count>1'000'000) return 2;
        ReprojectedSample out{};
        const bool valid=ReprojectSample(inverse,matrices[0],views[1],views[0],x,y,depth,out);
        std::cout<<valid<<'\t'<<out.x<<'\t'<<out.y<<'\t'<<out.depth<<'\n';
    }
    return std::cin.eof()?0:2;
}
