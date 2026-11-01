// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace SpartanPerfStats {
// Presentation-thread intervals, not GPU duration or physical display cadence.
// Fixed storage and no sampling/sorting when the optional graph is hidden.
struct FrameTimes {
    std::array<float,240> values{};
    size_t count{},next{};
    void Clear() {count=next=0;}
    void Push(float ms) {
        if(!std::isfinite(ms) || ms<=0 || ms>=1000) {Clear();return;}
        values[next]=ms;next=(next+1)%values.size();count=std::min(count+1,values.size());
    }
    struct Summary {float p99{},maximum{};};
    Summary Summarize() const {
        if(!count) return {};
        auto sorted=values;
        std::sort(sorted.begin(),sorted.begin()+count);
        const size_t rank=size_t(std::ceil(.99*double(count)))-1;
        return {sorted[rank],sorted[count-1]};
    }
};
}
