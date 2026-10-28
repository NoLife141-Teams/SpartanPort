// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Experimental GoW3 input validation, independent of Vulkan and NGX. This is NOT
// a DLAA readiness gate: jitter, object motion and pre-HUD color remain unvalidated.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace Vulkan::Gow3 {
// CPU model assumption, NOT calibrated from the game's matrices/viewport.
using Matrix = std::array<double, 16>; // column-major, [0,1] clip depth
inline constexpr uint64_t DepthShader = 0xaed40011eb5259f3ull;

// Shader color exports can remain declared while their attachments are disabled.
// Only an exported slot with a bound attachment makes this a color draw. Cached
// bindings outside the current shader's exports do not participate in the draw.
inline bool IsDepthOnlyDraw(uint32_t color_exports, uint32_t bound_colors,
                            bool depth_attachment, bool depth_test, bool depth_write) {
    return (color_exports & bound_colors)==0 && depth_attachment && depth_test && depth_write;
}

// A stale display address in the registers does not mean a draw writes to it.
// Track contiguous groups of actual single-target display draws, not depth
// passes that retain the same export mask and old color-buffer registers.
// Diagnostic groups only: multiple groups can occur within one accepted flip.
class DisplayPassTracker {
public:
    bool Observe(uint32_t color_exports, uint32_t bound_colors, bool color0_is_display) {
        const bool display = color_exports==1 && bound_colors==1 && color0_is_display;
        const bool boundary = display && !in_display;
        in_display = display;
        return boundary;
    }
private:
    bool in_display = false;
};

inline bool Inverse(const Matrix& m, Matrix& out) {
    double a[4][8]{};
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) {
        if (!std::isfinite(m[c*4+r])) return false;
        a[r][c]=m[c*4+r]; a[r][c+4]=(r==c);
    }
    for (int c=0;c<4;++c) {
        int p=c;
        for (int r=c+1;r<4;++r) if (std::abs(a[r][c])>std::abs(a[p][c])) p=r;
        if (std::abs(a[p][c])<1e-10) return false;
        for (int k=0;k<8;++k) std::swap(a[c][k],a[p][k]);
        const double d=a[c][c];
        for (double& v:a[c]) v/=d;
        for (int r=0;r<4;++r) if (r!=c) {
            const double f=a[r][c];
            for (int k=0;k<8;++k) a[r][k]-=f*a[c][k];
        }
    }
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) {
        out[c*4+r]=a[r][c+4];
        if (!std::isfinite(out[c*4+r])) return false;
    }
    return true;
}

inline std::array<double,4> Transform(const Matrix& m, const std::array<double,4>& v) {
    std::array<double,4> out{};
    for (int r=0;r<4;++r) for (int c=0;c<4;++c) out[r]+=m[c*4+r]*v[c];
    return out;
}

// CPU reference, unjittered inputs, top-left pixel coordinates, previous-current
// in render pixels. Assumes a full viewport, unchanged extent, [0,1] stored
// depth equal to clip z/w, and normal-Z clear=1. It consumes one depth value;
// it does not resolve MSAA or associate that value with a color surface.
// Reject background, behind-camera and off-screen projections.
inline bool Reproject(const Matrix& inverse_current, const Matrix& previous,
                      double x, double y, double depth, uint32_t width, uint32_t height,
                      std::array<double,2>& motion) {
    motion={};
    if (!width || !height || !std::isfinite(depth) || depth<0 || depth>=1 ||
        !std::isfinite(x) || !std::isfinite(y) || x<0 || y<0 || x>=width || y>=height) return false;
    const auto world=Transform(inverse_current,{2*(x+.5)/width-1,1-2*(y+.5)/height,depth,1});
    const auto clip=Transform(previous,world);
    if (!std::isfinite(clip[3]) || clip[3]<=1e-8) return false;
    const double nx=clip[0]/clip[3], ny=clip[1]/clip[3], nz=clip[2]/clip[3];
    if (!std::isfinite(nx)||!std::isfinite(ny)||!std::isfinite(nz)||
        std::abs(nx)>1 || std::abs(ny)>1 || nz<0 || nz>1) return false;
    motion={(nx+1)*.5*width-.5-x,(1-ny)*.5*height-.5-y};
    return true;
}

enum class Status { Missing, Invalid, Conflict, Warming, Candidate };
inline const char* Name(Status s) {
    switch(s) {
    case Status::Missing:return "missing"; case Status::Invalid:return "invalid";
    case Status::Conflict:return "conflict"; case Status::Warming:return "warming";
    case Status::Candidate:return "camera-candidate";
    }
    return "invalid";
}

class CameraInputs {
public:
    // Caller proves vertex-stage, depth-only draw, writing D32S8, and slot 1.
    // Bytes must be a stable host snapshot, NEVER a guest address used as a label.
    void Observe(uint64_t shader, uint32_t slot, const void* bytes, size_t size,
                 uint64_t depth_target, uint32_t w, uint32_t h) {
        if (shader!=DepthShader || slot!=1 || size!=144) return;
        ++samples;
        if (!bytes || !depth_target || !w || !h) { invalid=true; return; }
        std::array<float,36> f{};
        std::memcpy(f.data(),bytes,144);
        for (float v:f) if (!std::isfinite(v)) { invalid=true; return; }
        Matrix vp{}, inv{};
        for (size_t i=0;i<16;++i) vp[i]=f[i];
        if (!Inverse(vp,inv)) { invalid=true; return; }
        if (seen && (vp!=current || target!=depth_target || width!=w || height!=h)) {
            conflict=true; return;
        }
        current=vp; inverse=inv; target=depth_target; width=w; height=h; seen=true;
    }
    Status FinishFrame() {
        status=invalid ? Status::Invalid : conflict ? Status::Conflict : !seen ? Status::Missing :
            previous_valid && width==previous_width && height==previous_height ?
                Status::Candidate : Status::Warming;
        previous_valid=seen && !invalid && !conflict;
        previous=last; last=current; previous_width=width; previous_height=height;
        seen=invalid=conflict=false; samples=0;
        return status;
    }
    // Completed pair is available after FinishFrame only when status is Candidate.
    // Exposed for diagnostics / CPU reference tests, not native dispatch.
    Matrix current{}, inverse{}, previous{};
    uint32_t width{},height{},samples{};
    Status status=Status::Missing;
private:
    Matrix last{};
    uint64_t target{};
    uint32_t previous_width{},previous_height{};
    bool seen{},invalid{},conflict{},previous_valid{};
};
} // namespace Vulkan::Gow3
