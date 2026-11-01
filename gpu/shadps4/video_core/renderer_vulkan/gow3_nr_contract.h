// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace Vulkan::Gow3 {
// Raw programs define the composition operation. Cache permutation ordinals
// describe insertion order, not shader semantics. The caller must additionally
// validate the two live sampled bindings, formats, samples and backing identity.
inline constexpr bool IsSceneComposition(uint64_t vertex, uint64_t fragment) {
    return vertex==0x475a41468b1a1fbcull && fragment==0x475a4146e3a74097ull;
}
} // namespace Vulkan::Gow3
