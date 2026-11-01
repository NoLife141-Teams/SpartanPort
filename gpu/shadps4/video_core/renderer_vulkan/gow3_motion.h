// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdlib>
#include <cstring>
#include <atomic>
namespace Vulkan::Gow3 {
inline bool NeuralSceneRequested() {
    static const bool requested=[] {
        const auto one=[](const char* v){return v&&std::strcmp(v,"1")==0;};
        const char* upscale=std::getenv("SPARTAN_UPSCALER");
        return one(std::getenv("SPARTAN_GOW3_NR_SCENE")) && one(std::getenv("SPARTAN_GOW3_NR_PREVIEW")) &&
            one(std::getenv("SPARTAN_GOW3_INPUT_DIAGNOSTICS")) && upscale&&std::strcmp(upscale,"none")==0;
    }();
    return requested;
}
inline bool MotionRequested(const char* motion,const char* pixels,const char* inputs,const char* upscaler) {
    const auto one=[](const char* v){return v&&std::strcmp(v,"1")==0;};
    return one(motion)&&one(pixels)&&one(inputs)&&upscaler&&std::strcmp(upscaler,"none")==0;
}
inline bool ExplicitMotionDiagnostic() {
    static const bool enabled=MotionRequested(std::getenv("SPARTAN_GOW3_MOTION"),std::getenv("SPARTAN_GOW3_PIXELS"),
        std::getenv("SPARTAN_GOW3_INPUT_DIAGNOSTICS"),std::getenv("SPARTAN_UPSCALER"));
    return enabled;
}
inline bool MotionDiagnostic() { return ExplicitMotionDiagnostic() || NeuralSceneRequested(); }
inline std::atomic<bool>& NeuralMotionEnabled() { static std::atomic<bool> value{};return value; }
inline bool MotionWorkNeeded() {
    return !MotionDiagnostic() || ExplicitMotionDiagnostic() || NeuralMotionEnabled().load(std::memory_order_relaxed);
}
} // namespace Vulkan::Gow3
