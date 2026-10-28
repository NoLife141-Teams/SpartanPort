// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace Vulkan::Gow3 {
inline bool IsEnabledValue(const char* value) {
    return value && std::strcmp(value, "1") == 0;
}
inline bool InputDiagnosticsEnabled() {
    return IsEnabledValue(std::getenv("SPARTAN_GOW3_INPUT_DIAGNOSTICS"));
}

// Stage A records the first existing rejection it encounters, in the existing
// packet's reserved word. Stage B consumes that packet's value, never a global
// latest reason. No extra guest reads, cache queries or synchronization.
enum class SnapshotReason : uint32_t {
    None, Copied, NoPacket, RingUnavailable, RingDisabled, NoBinding,
    EmptySource, Writable, EmptyRange, TooLarge, GpuModified, PendingWrite,
    AllocationFailed, CopyMissing, Unknown, Count
};
using SnapshotCounts = std::array<uint32_t, size_t(SnapshotReason::Count)>;
inline const char* Name(SnapshotReason reason) {
    switch (reason) {
    case SnapshotReason::None: return "none";
    case SnapshotReason::Copied: return "copied";
    case SnapshotReason::NoPacket: return "no-ring-context";
    case SnapshotReason::RingUnavailable: return "ring-absent";
    case SnapshotReason::RingDisabled: return "ring-disabled";
    case SnapshotReason::NoBinding: return "no-matching-binding";
    case SnapshotReason::EmptySource: return "zero-address-or-size";
    case SnapshotReason::Writable: return "declared-writable";
    case SnapshotReason::EmptyRange: return "empty-range";
    case SnapshotReason::TooLarge: return "too-large";
    // These are conservative producer observations, not claims that the exact
    // camera bytes were changed, or that the GPU/VRAM allocation failed.
    case SnapshotReason::GpuModified: return "gpu-dirty-range";
    case SnapshotReason::PendingWrite: return "queued-write-overlap";
    case SnapshotReason::AllocationFailed: return "ring-space-unavailable";
    case SnapshotReason::CopyMissing: return "copied-binding-missing";
    default: return "unknown";
    }
}
inline SnapshotReason ResolveSnapshotReason(bool snapshot, bool packet,
                                           SnapshotReason producer) {
    if (snapshot) return SnapshotReason::Copied;
    if (!packet) return SnapshotReason::NoPacket;
    if (producer == SnapshotReason::Copied) return SnapshotReason::CopyMissing;
    if (producer == SnapshotReason::None || producer >= SnapshotReason::Count)
        return SnapshotReason::Unknown;
    return producer;
}
// Fixed storage, CSV-safe (no commas/newlines). Large counts cannot overflow it.
inline std::array<char,512> FormatSnapshotCounts(const SnapshotCounts& counts) {
    std::array<char,512> text{};
    size_t at=0;
    for (size_t i=0;i<counts.size();++i) {
        if (!counts[i]) continue;
        const int n=std::snprintf(text.data()+at,text.size()-at,"%s%s:%u",
                                  at ? ";" : "",Name(SnapshotReason(i)),counts[i]);
        if (n<0 || size_t(n)>=text.size()-at) break;
        at+=size_t(n);
    }
    return text;
}
} // namespace Vulkan::Gow3
