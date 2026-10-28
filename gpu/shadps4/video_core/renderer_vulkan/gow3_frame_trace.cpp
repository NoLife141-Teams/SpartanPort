// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_frame_trace.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>

namespace Vulkan::Gow3::Trace {
namespace {
const char* Name(Kind kind) {
    switch (kind) {
    case Kind::Display: return "display";
    case Kind::Interval: return "interval";
    case Kind::Queued: return "queued";
    case Kind::Dequeued: return "dequeued";
    case Kind::Present: return "present";
    case Kind::Unordered: return "unordered";
    case Kind::Start: return "start";
    }
    return "unknown";
}
const char* Name(Result result) {
    switch (result) {
    case Result::None: return "none";
    case Result::Incomplete: return "incomplete";
    case Result::NoDraws: return "no-draws";
    case Result::ShaderAbsent: return "shader-absent";
    case Result::Filtered: return "filtered";
    case Result::NoBinding: return "no-binding";
    case Result::SnapshotMissing: return "snapshot-missing";
    case Result::EmptyDisplayWithCamera: return "empty-display-with-camera";
    case Result::MultipleDisplayGroups: return "multiple-display-groups";
    case Result::Observed: return "observed";
    case Result::Presented: return "presented";
    case Result::Reused: return "reused";
    case Result::Minimized: return "minimized";
    case Result::AcquireFailed: return "acquire-failed";
    case Result::PresentFailed: return "present-failed";
    }
    return "unknown";
}
using U = unsigned long long;
std::filesystem::path EnvPath(const char* name) {
    const char* value=std::getenv(name);
    return value ? std::filesystem::path(value) : std::filesystem::path{};
}
// Runtime threads can outlive static destructors. Each completed flip flushes;
// leave this small process-lifetime sink to OS teardown instead of racing them.
Session& GetSink() {
    static Session* sink = new Session(
        IsEnabledValue(std::getenv("SPARTAN_GOW3_FRAME_TRACE")) && InputDiagnosticsEnabled(),
        EnvPath("SPARTAN_GOW3_FRAME_TRACE_FILE"), EnvPath("SPARTAN_GOW3_FRAME_TRACE_TRIGGER"));
    return *sink;
}
} // namespace

Journal::Journal(std::FILE* file_, uint64_t capacity_) : file(file_), capacity(capacity_) {
    if (!file) { ++errors; return; }
    Header();
    std::fputs("seq,steady_us,kind,result,flip,buffer,address,eop,interval,draw,draws,shader,eligible,snapshots,unsnapshotted,transitions,empty_transitions,camera_frame,color,depth,exports,bound,width,height,samples,missing,status,snapshot_reasons\n", file);
    if (std::ferror(file)) ++errors;
}
void Journal::Header() {
    if (!file) return;
    const long at = std::ftell(file);
    if (at < 0 || std::fseek(file, 0, SEEK_SET) != 0) { ++errors; return; }
    if (std::fprintf(file, "# gow3-frame-trace-v4 capacity=%020llu retained=%020llu dropped=%020llu io_errors=%020llu\n",
                     U(capacity), U(retained), U(dropped), U(errors)) < 0) ++errors;
    if (at && std::fseek(file, at, SEEK_SET) != 0) ++errors;
}
bool Journal::Write(const Event& e) {
    std::scoped_lock lock{mutex};
    ++sequence;
    if (!file || retained >= capacity || errors) { ++dropped; return false; }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto& c=e.counts;
    const auto& b=e.boundary;
    const auto reasons=FormatSnapshotCounts(b.snapshot_reasons);
    char line[1024];
    const int size = std::snprintf(line, sizeof(line),
        "%llu,%llu,%s,%s,%llu,%d,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u,%u,%s\n",
        U(sequence), U(us), Name(e.kind), Name(e.result), U(e.flip.id), e.flip.buffer,
        U(e.flip.address), unsigned(e.flip.eop), U(e.interval), U(e.draw), U(c.draws),
        U(c.shader), U(c.eligible), U(c.snapshots), U(c.unsnapshotted), U(c.transitions),
        U(c.empty_transitions), U(b.number), U(b.color), U(b.depth), b.exports, b.bound,
        b.width, b.height, b.samples, b.unsnapshotted, b.status, reasons.data());
    if (size < 0 || size >= int(sizeof(line)) ||
        std::fwrite(line, 1, size_t(size), file) != size_t(size)) {
        ++errors; ++dropped; return false;
    }
    ++retained;
    return true;
}
void Journal::Flush() {
    std::scoped_lock lock{mutex};
    Header();
    if (file && std::fflush(file) != 0) ++errors;
}
uint64_t Journal::Dropped() const { std::scoped_lock lock{mutex}; return dropped; }
uint64_t Journal::Retained() const { std::scoped_lock lock{mutex}; return retained; }
uint64_t Journal::Errors() const { std::scoped_lock lock{mutex}; return errors; }
bool Journal::Complete() const { std::scoped_lock lock{mutex}; return !errors && !dropped; }
Session::Session(bool enabled, std::filesystem::path destination_,
                 std::filesystem::path trigger_, uint64_t capacity_)
    : destination(std::move(destination_)), trigger(std::move(trigger_)), capacity(capacity_) {
    if (!enabled) return; // no filesystem operations when flags are off
    if (!destination.is_absolute() ||
        (!trigger.empty() && (!trigger.is_absolute() ||
                             trigger.lexically_normal()==destination.lexically_normal()))) {
        state=SessionState::Failed;
        std::puts("GoW3 frame trace: disabled (absolute, distinct output/trigger paths required)");
        return;
    }
    if (trigger.empty()) Open();
    else {
        state=SessionState::Armed;
        std::puts("GoW3 frame trace: armed; waiting for local trigger at an ordered flip");
    }
}
Session::~Session() {
    std::scoped_lock lock{mutex};
    if (journal) journal->Flush();
    journal.reset();
    if (file) std::fclose(file);
}
bool Session::Open() {
    // Exclusive creation is the final race-safe no-overwrite check. Do not
    // create directories, remove the trigger, or retry a failed opening.
    file=std::fopen(destination.string().c_str(), "wx");
    if (!file) {
        state=SessionState::Failed;
        std::puts("GoW3 frame trace: opening failed; existing output preserved, no retry");
        return false;
    }
    journal=std::make_unique<Journal>(file,capacity);
    state=SessionState::Active;
    std::puts("GoW3 frame trace: diagnostic journal open, bounded; camera decisions unchanged");
    return true;
}
bool Session::Enabled() const {
    std::scoped_lock lock{mutex};
    return state==SessionState::Armed || state==SessionState::Active;
}
bool Session::Write(const Event& event) {
    std::scoped_lock lock{mutex};
    return journal && journal->Write(event);
}
bool Session::OrderedInterval(Event event) {
    std::scoped_lock lock{mutex};
    // Only a real ordered accepted-flip interval may activate the window.
    if (event.kind!=Kind::Interval || !event.flip.id) return false;
    bool started=false;
    if (state==SessionState::Armed) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(trigger,error)) || error)
            return false;
        if (!Open()) return false;
        started=true;
        event.result=Result::Incomplete;
        journal->Write({.kind=Kind::Start,.result=Result::Incomplete,.flip=event.flip,
                        .interval=event.interval,.draw=event.draw,
                        .boundary={.number=event.boundary.number}});
    }
    if (journal) {
        // Start and activation interval stay adjacent even with concurrent
        // present/queue writers. Existing identity/counter values are retained.
        journal->Write(event);
        journal->Flush();
    }
    return started;
}
void Session::Flush() {
    std::scoped_lock lock{mutex};
    if (journal) journal->Flush();
}
SessionStats Session::Stats() const {
    std::scoped_lock lock{mutex};
    return {state, journal ? journal->Retained() : 0,
            journal ? journal->Dropped() : 0, journal ? journal->Errors() : 0};
}
bool Enabled() { return GetSink().Enabled(); }
void Write(const Event& event) { GetSink().Write(event); }
void WriteOrderedInterval(const Event& event) {
    if (GetSink().OrderedInterval(event)) {
        std::printf("GoW3 frame trace: started at ordered flip=%llu interval=%llu; first interval incomplete\n",
                    U(event.flip.id),U(event.interval));
    }
}
void Flush() { GetSink().Flush(); }

} // namespace Vulkan::Gow3::Trace
