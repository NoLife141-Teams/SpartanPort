// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace SpartanNr {
enum Profile { Subtle, Standard, Intense, Custom };
struct Appearance {
    int intensity=100, tone=100, structure=100, skin=0;
    int profile=Intense;
};
// One atomic word keeps a preset's four strengths coherent on the GPU thread.
constexpr uint32_t Pack(Appearance a) {
    return uint32_t(std::clamp(a.intensity,0,100)) |
        uint32_t(std::clamp(a.tone,0,100))<<7 |
        uint32_t(std::clamp(a.structure,0,100))<<14 |
        uint32_t(std::clamp(a.skin,0,100))<<21 |
        uint32_t(std::clamp(a.profile,0,3))<<28;
}
constexpr Appearance Unpack(uint32_t word) {
    return {int(word&127),int((word>>7)&127),int((word>>14)&127),
            int((word>>21)&127),int((word>>28)&3)};
}
constexpr Appearance Preset(int profile) {
    switch(profile) {
    case Subtle: return {40,40,25,0,Subtle};
    case Standard: return {75,75,50,0,Standard};
    default: return {100,100,100,0,Intense}; // Existing rendering stays the default.
    }
}
constexpr uint32_t Default=Pack(Preset(Intense));
constexpr uint32_t CustomDefault=Pack({100,100,100,0,Custom});
inline uint32_t Edit(uint32_t word,int channel,int value) {
    auto a=Unpack(word);
    switch(channel) {
    case 0:a.intensity=value;break;
    case 1:a.tone=value;break;
    case 2:a.structure=value;break;
    case 3:a.skin=value;break;
    default:return word;
    }
    a.profile=Custom;
    return Pack(a);
}
}
