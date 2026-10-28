// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Synthetic geometry only. These conventions and sample positions are NOT
// measurements of GoW3. The forward oracle never calls Inverse or Transform.
#include <array>
#include <cmath>
#include "gpu/shadps4/video_core/renderer_vulkan/gow3_camera_inputs.h"

namespace ProjectionFixture {
using Vulkan::Gow3::Matrix;
using Point = std::array<double, 3>;
enum class Depth { Normal, Reversed, MinusOneToOne };
struct Viewport {
    double left = 0, top = 0;
    uint32_t width = 960, height = 540;
};
struct Camera {
    Point eye{};
    double yaw = 0, sx = 1.4, sy = 2.1, ox = 0, oy = 0;
    double near = .5, far = 100;
    Depth depth = Depth::Normal;
    std::array<double, 2> DepthCoefficients() const {
        if (depth == Depth::Reversed) return {-near / (far-near), far*near / (far-near)};
        if (depth == Depth::MinusOneToOne)
            return {(far+near) / (far-near), -2*far*near / (far-near)};
        return {far / (far-near), -far*near / (far-near)};
    }
    Matrix VP() const {
        const double c = std::cos(yaw), s = std::sin(yaw);
        const auto [a, b] = DepthCoefficients();
        const std::array<std::array<double, 4>, 4> rows{{
            {sx*c+ox*s, 0, -sx*s+ox*c, 0},
            {oy*s, sy, oy*c, 0}, {a*s, 0, a*c, b}, {s, 0, c, 0}}};
        Matrix m{};
        for (size_t r=0; r<4; ++r) {
            for (size_t col=0; col<4; ++col) m[4*col+r] = rows[r][col];
            for (size_t col=0; col<3; ++col) m[12+r] -= rows[r][col]*eye[col];
        }
        return m;
    }
    // Explicit world -> camera -> perspective -> viewport calculation.
    // Returns absolute pixel coordinates (integer means center), and clip z/w.
    Point Project(Point world, Viewport v = {}) const {
        const double dx=world[0]-eye[0], dy=world[1]-eye[1], dz=world[2]-eye[2];
        const double vx=std::cos(yaw)*dx-std::sin(yaw)*dz;
        const double vz=std::sin(yaw)*dx+std::cos(yaw)*dz;
        const auto [a,b] = DepthCoefficients();
        return {v.left+(1+sx*vx/vz+ox)*v.width/2-.5,
                v.top+(1-sy*dy/vz-oy)*v.height/2-.5, a+b/vz};
    }
};
inline bool Near(double a, double b, double tolerance = 1e-8) {
    return std::abs(a-b) < tolerance;
}
inline double Error(std::array<double,2> motion, Point now, Point before) {
    return std::hypot(motion[0]-(before[0]-now[0]), motion[1]-(before[1]-now[1]));
}
inline Matrix Transpose(const Matrix& m) {
    Matrix t{};
    for (size_t r=0;r<4;++r) for (size_t c=0;c<4;++c) t[r*4+c]=m[c*4+r];
    return t;
}
inline Matrix ZeroOneDepth(Matrix m) {
    for (size_t c=0;c<4;++c) m[4*c+2] = (m[4*c+2]+m[4*c+3])/2;
    return m;
}
} // namespace ProjectionFixture
