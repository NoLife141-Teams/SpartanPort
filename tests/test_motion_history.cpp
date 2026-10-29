// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone CPU regression checks: no game, Vulkan device or window required.
#include <array>
#include <cassert>
#include <cstdio>
#include "gpu/shadps4/video_core/renderer_vulkan/motion_history.h"

using namespace Vulkan::Motion;

int main() {
    const std::array<uint16_t, 5> indices{1000, 1007, 1002, 1007, UINT16_MAX};
    const auto range = IndexedRange(std::span(indices), -900, true);
    assert(range.first == 100 && range.count == 8);
    assert(IndexedRange(std::span(indices), -1001, true).count == 0);
    assert(IndexedRange(std::span(indices), 0, false).count == 64536);
    const std::array<uint32_t, 3> large{100000, 100003, UINT32_MAX};
    assert(IndexedRange(std::span(large), 7, true).count == 4);
    assert(IndexedRange(std::span(large), INT32_MAX, true).count == 0);
    const std::array<uint16_t, 1> restart{UINT16_MAX};
    assert(IndexedRange(std::span(restart), 0, true).count == 0);
    assert(IndexedRange(std::span<const uint16_t>{}, 0, false).count == 0);

    Draw draw{.shader = 1, .geometry = 2, .indices = 3, .topology = 4,
              .index_count = 5, .instances = 2, .first_instance = 100,
              .vertices = range};
    History history(32);
    history.NextFrame();
    const auto first = history.Prepare(draw);
    const auto duplicate = history.Prepare(draw);
    assert(first.store && !first.load && first.first_vertex == 100);
    assert(first.first_instance == 100 && first.instances == 2);
    assert(duplicate.store == first.store + 16); // shared geometry, different draw
    assert(history.Used() == 32); // firstInstance must NOT multiply the allocation
    assert(!history.Prepare(draw).store && history.stats.exhausted == 1);
    history.NextFrame();
    const auto next = history.Prepare(draw);
    assert(next.load == first.store && next.store != first.store);
    assert(history.Prepare(draw).load == duplicate.store);
    history.NextFrame(); // a missing frame invalidates the match
    history.NextFrame();
    assert(!history.Prepare(draw).load);

    // A changed index buffer may reference formerly unused holes inside the same min/max.
    // Do not accept that history merely because its addresses/counts stayed the same.
    history.NextFrame();
    auto changed = draw;
    changed.topology++;
    assert(!history.Prepare(changed).load);
    changed = draw;
    changed.first_instance++;
    assert(!history.Prepare(changed).load);
    history.NextFrame();
    changed = draw;
    changed.vertices.first++;
    assert(!history.Prepare(changed).load);

    History overflow(32);
    draw.vertices.first = 0;
    draw.vertices.count = INT32_MAX;
    draw.instances = UINT32_MAX;
    draw.first_instance = 0;
    assert(!overflow.Prepare(draw).store);
    assert(overflow.stats.exhausted == 1);
    draw.first_instance = 10;
    assert(!overflow.Prepare(draw).store && overflow.stats.invalid == 1);
    draw.first_instance = 0;
    draw.vertices.first = INT32_MAX;
    draw.vertices.count = 2;
    assert(!overflow.Prepare(draw).store && overflow.stats.invalid == 2);

    // Small skeletons: history only while the bone palette changes (weapon spinning).
    History gated(1024);
    const History::GateKey weapon{.shader = 7, .index_count = 3, .instances = 1};
    const auto frame = [&](uint64_t palette) {
        gated.NextFrame();
        return gated.Moving(weapon, palette);
    };
    assert(!gated.Moving(weapon, 1) && gated.stats.still == 1); // first sight
    assert(!frame(1));                    // same bones: camera motion
    assert(frame(2) && frame(3));         // spinning
    assert(!frame(3));                    // stopped
    gated.NextFrame();
    gated.NextFrame();
    assert(!gated.Moving(weapon, 4));     // after a skipped frame: no comparison
    assert(!gated.Moving(weapon, 5));     // second occurrence of the same draw
    assert(ClassifyBuffer(864) == BufferRole::Other && ClassifyBuffer(416) == BufferRole::Other);
    assert(ClassifyBuffer(96) == BufferRole::SmallSkeleton &&
           ClassifyBuffer(384) == BufferRole::SmallSkeleton);
    assert(ClassifyBuffer(656) == BufferRole::Skeleton && ClassifyBuffer(64) == BufferRole::Other);

    // Index ranges: reused between full scans, rescanned every Revalidate frames, trimmed.
    IndexRangeCache ranges;
    uint32_t scans = 0;
    const IndexRangeCache::Key mesh{.address = 0x1000, .count = 3, .index_size = 2};
    const auto scan = [&] {
        ++scans;
        return IndexRangeCache::Result{{4, 3}, 99};
    };
    assert(ranges.Get(mesh, 0, scan).topology == 99 && scans == 1);
    assert(ranges.Get(mesh, IndexRangeCache::Revalidate - 1, scan).range == (VertexRange{4, 3}));
    assert(scans == 1);
    ranges.Get(mesh, IndexRangeCache::Revalidate, scan);
    assert(scans == 2);
    ranges.Trim(IndexRangeCache::Revalidate + IndexRangeCache::Unused + 1);
    assert(ranges.Size() == 0);

    assert(JitterPhases(1920, 1920) == 8);
    assert(JitterPhases(1280, 1920) == 18);
    assert(JitterPhases(960, 1920) == 32);
    assert(JitterPhases(640, 1920) == 72);
    assert(JitterPhases(0, 1920) == 8);
    // Diagnostic mode never pairs duplicate draws by occurrence order. A
    // duplicate invalidates the next frame's key even if the next frame is unique.
    History strict(1024,true);
    draw={.shader=1,.geometry=2,.indices=3,.topology=4,.index_count=3,.instances=1,.vertices={0,3}};
    assert(strict.Prepare(draw).store);
    assert(!strict.Prepare(draw).store && strict.stats.ambiguous==1);
    strict.NextFrame();
    assert(!strict.Prepare(draw).load && strict.stats.ambiguous==2);
    strict.NextFrame();
    assert(!strict.Prepare(draw).load); // rejected previous key did not seed history
    strict.NextFrame();
    assert(strict.Prepare(draw).load);
    // Shared meshes keep their affine instance even if draws are reordered and
    // Gaia moves. Near ties, teleports, missing frames and repeated poses reject.
    AffineInstances tracker;
    AffineInstances::Pose a{1,0,0,0,1,0,0,0,1,10,20,30},b=a;
    b[9]=20;
    const auto ia=tracker.Resolve(draw,a),ib=tracker.Resolve(draw,b);
    assert(ia.id && ib.id && ia.id!=ib.id && !ia.matched && !ib.matched);
    tracker.NextFrame();a[9]+=.1f;b[9]+=.1f;
    assert(tracker.Resolve(draw,b).id==ib.id);
    const auto moved=tracker.Resolve(draw,a);
    assert(moved.id==ia.id && moved.matched && moved.repeated_geometry);
    tracker.NextFrame();
    auto repeated_pose=tracker.Resolve(draw,a);
    assert(repeated_pose.id==ia.id && tracker.Resolve(draw,a).id==ia.id);
    tracker.NextFrame();assert(!tracker.Resolve(draw,a).matched);
    tracker.NextFrame();tracker.NextFrame();assert(!tracker.Resolve(draw,a).matched);
    tracker.NextFrame();a[9]+=10;assert(!tracker.Resolve(draw,a).matched);
    auto bad=a;bad[0]=std::numeric_limits<float>::quiet_NaN();
    assert(!tracker.Resolve(draw,bad).id);
    AffineInstances ties;
    a={1,0,0,0,1,0,0,0,1,0,0,0};b=a;b[9]=.2f;
    ties.Resolve(draw,a);ties.Resolve(draw,b);ties.NextFrame();a[9]=.1f;
    assert(!ties.Resolve(draw,a).matched);
    // Instance identity participates in History keys; no cross-instance reads.
    History instance_history(1024,true);draw.instance_identity=ia.id;
    auto sa=instance_history.Prepare(draw);draw.instance_identity=ib.id;
    auto sb=instance_history.Prepare(draw);instance_history.NextFrame();
    assert(instance_history.Prepare(draw).load==sb.store);
    draw.instance_identity=ia.id;assert(instance_history.Prepare(draw).load==sa.store);
    std::puts("Motion history: PASS (ranges, matching, capacity, gating, strict duplicate rejection, index cache, jitter)");
}
