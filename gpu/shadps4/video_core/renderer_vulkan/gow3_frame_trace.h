// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <filesystem>
#include <memory>
#include "gow3_camera_frames.h"

namespace Vulkan::Gow3::Trace {

// Values only: never retain guest pointers or refer to a latest global flip.
using Flip = Gow3::Flip;
enum class Kind { Display, Interval, Queued, Dequeued, Present, Unordered, Start };
enum class Result {
    None, Incomplete, NoDraws, ShaderAbsent, Filtered, NoBinding, SnapshotMissing,
    EmptyDisplayWithCamera, MultipleDisplayGroups, Observed,
    Presented, Reused, Minimized, AcquireFailed, PresentFailed
};
struct Counts {
    uint64_t draws = 0, shader = 0, eligible = 0, snapshots = 0, unsnapshotted = 0;
    uint64_t transitions = 0, empty_transitions = 0;
};
struct Boundary {
    uint64_t number = 0, color = 0, depth = 0;
    uint32_t exports = 0, bound = 0, width = 0, height = 0;
    uint32_t samples = 0, unsnapshotted = 0, status = 0;
    SnapshotCounts snapshot_reasons{};
};
struct Event {
    Kind kind = Kind::Display;
    Result result = Result::None;
    Flip flip{};
    uint64_t interval = 0, draw = 0;
    Counts counts{};
    Boundary boundary{};
};

// Owned by the presentation thread, alongside last_submit_frame. Recording a
// newer request cannot change the source of a delayed present or a redraw.
class Presentation {
public:
    Event Complete(Flip flip, bool reused, Result result) {
        if (!reused) last = flip;
        return {.kind=Kind::Present,
                .result=result == Result::Presented && reused ? Result::Reused : result,
                .flip=flip};
    }
    Flip ReuseSource() const { return last; }
private:
    Flip last{};
};

// Owned by the same ordered draw consumer as the camera diagnostic. This
// independent accounting never calls CameraInputs::FinishFrame or changes it.
class Accumulator {
public:
    void Draw(bool expected_shader, bool eligible) {
        ++draw_serial;
        ++counts.draws;
        counts.shader += expected_shader;
        counts.eligible += expected_shader && eligible;
    }
    void Snapshot(bool present) {
        if (present) ++counts.snapshots;
        else ++counts.unsnapshotted;
    }
    Event Display(Boundary boundary) {
        ++counts.transitions;
        counts.empty_transitions += boundary.samples == 0;
        return {.kind=Kind::Display, .interval=interval, .draw=draw_serial,
                .counts=counts, .boundary=boundary};
    }
    Event Finish(Flip flip, Boundary completed_frame = {}) {
        const Result result = !complete ? Result::Incomplete :
            !counts.draws ? Result::NoDraws : !counts.shader ? Result::ShaderAbsent :
            !counts.eligible ? Result::Filtered :
            counts.unsnapshotted ? Result::SnapshotMissing :
            !counts.snapshots ? Result::NoBinding :
            counts.transitions > 1 && counts.empty_transitions ? Result::EmptyDisplayWithCamera :
            counts.transitions > 1 ? Result::MultipleDisplayGroups : Result::Observed;
        Event event{.kind=Kind::Interval, .result=result, .flip=flip,
                    .interval=interval++, .draw=draw_serial, .counts=counts,
                    .boundary=completed_frame};
        counts = {};
        complete = true;
        return event;
    }
private:
    uint64_t interval = 1, draw_serial = 0;
    bool complete = false; // first interval has no observed preceding flip
    Counts counts{};
};

// Caller owns the FILE. Fixed event budget, no wrap/overwrite of retained
// events. Dropped events and I/O errors invalidate a completeness conclusion.
class Journal {
public:
    static constexpr uint64_t Capacity = 16384;
    explicit Journal(std::FILE* file, uint64_t capacity = Capacity);
    bool Write(const Event& event);
    void Flush();
    uint64_t Dropped() const;
    uint64_t Retained() const;
    uint64_t Errors() const;
    bool Complete() const;
private:
    void Header();
    std::FILE* file;
    const uint64_t capacity;
    uint64_t retained = 0, dropped = 0, errors = 0, sequence = 0;
    mutable std::mutex mutex;
};

enum class SessionState { Disabled, Armed, Active, Failed };
struct SessionStats {
    SessionState state;
    uint64_t retained = 0, dropped = 0, errors = 0;
};

// The one-shot window is opened only by OrderedInterval on the draw consumer.
// Other threads may Write/Flush, but cannot activate or reset it. Suppression
// before the start marker is outside the window, not journal loss. Late events
// retain their original IDs and require a matching interval to be usable.
class Session {
public:
    Session(bool enabled, std::filesystem::path destination,
            std::filesystem::path trigger = {}, uint64_t capacity = Journal::Capacity);
    ~Session();
    bool Enabled() const;
    bool Write(const Event& event);
    // Returns true only for the first deferred activation. Never resets IDs or
    // the accumulator; the activation interval is explicitly incomplete.
    bool OrderedInterval(Event event);
    void Flush();
    SessionStats Stats() const;
private:
    bool Open(); // caller owns mutex (or constructor has not published this)
    std::filesystem::path destination, trigger;
    uint64_t capacity;
    SessionState state = SessionState::Disabled;
    std::FILE* file = nullptr;
    std::unique_ptr<Journal> journal;
    mutable std::mutex mutex;
};

// Opt-in runtime sink. Requires diagnostics=1, trace=1 and a new absolute file.
bool Enabled();
void Write(const Event& event);
void WriteOrderedInterval(const Event& event);
void Flush();

} // namespace Vulkan::Gow3::Trace
