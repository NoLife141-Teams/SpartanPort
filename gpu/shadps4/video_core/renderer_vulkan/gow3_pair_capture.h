// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Bounded metadata only. No guest reads, GPU objects retained or pixel readback.
// Every method is owned by the ordered draw consumer; never call from presenter
// or binding workers. Draw IDs are local to this capture, not global trace IDs.
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include "gow3_camera_frames.h"
#include "gow3_resource_provenance.h"
namespace Vulkan::Gow3::Pair {
struct Attachment {
    uint64_t uid{}, alias_generation{}, image{}, view{}, address{};
    uint32_t format{}, cache_samples{}, host_samples{}, width{}, height{}, mip{}, layer{};
    bool proxy{};
};
struct Viewport {
    std::array<uint32_t,6> bits{}; // exact float x,y,width,height,minDepth,maxDepth
    std::array<int32_t,2> offset{};
    std::array<uint32_t,2> extent{};
};
struct ShaderBinding {
    uint32_t resource_index{},descriptor_binding{},offset_index{},resource_kind{};
    uint64_t host_buffer{},offset{},range{};
    bool ring_copy{};
};
struct VertexAttribute { uint32_t location{},binding{},format{},offset{}; };
struct VertexStream {
    uint32_t binding{},stride{},rate{},divisor{};
    uint64_t buffer{},offset{},size{};
};
struct ImageDescriptor {
    uint32_t binding{},element{},type{},layout{};
    uint64_t view{}; // observed label only, not resource ownership or pixel evidence
    Provenance::Binding provenance{};
};
struct ShaderEvidence {
    bool enabled{},dynamic_seen{},native_clip_control{},depth_clip_feature{},unrestricted_range{};
    bool depth_clamp_key{},depth_clip_key{},bias_enabled{},bounds_enabled{};
    uint64_t vs_key{},ps_key{}; // actual selected pipeline stage keys, not a global last counter
    std::array<uint32_t,3> bias_bits{}; // actual constant, clamp, slope arguments
    std::array<uint32_t,2> bounds_bits{};
    std::array<ShaderBinding,32> bindings{};
    uint32_t binding_count{},bindings_dropped{};
    bool camera_binding_seen{};
    bool vertex_input_seen{},dynamic_vertex_input{};
    std::array<VertexAttribute,32> attributes{};
    std::array<VertexStream,32> streams{};
    uint32_t attribute_count{},stream_count{},vertex_dropped{};
    void Binding(ShaderBinding b) {
        camera_binding_seen |= b.resource_index==1;
        if(binding_count<bindings.size()) bindings[binding_count++]=b;
        else ++bindings_dropped;
    }
};
struct Draw {
    uint64_t serial{}, interval{}, vs{}, ps{};
    uint32_t exports{}, bound{}, clip_space{}, raster_samples{}, depth_compare{};
    uint32_t guest_depth_samples{}, depth_clear_bits{};
    bool clip_disabled{}, depth_test{}, depth_write{}, depth_clear{}, legacy_ui_hint{};
    bool recorded{}, indirect{}, extra_square_draw{}, viewport_overflow{}, proxy_path{};
    uint32_t width{},height{},viewport_count{},color_count{};
    Attachment camera_depth{}, depth{};
    std::array<Attachment,8> colors{};
    std::array<Viewport,16> viewports{};
    bool camera_observed{}, snapshot_present{}, snapshot_conflict{};
    uint32_t snapshot_observations{},snapshot_missing{};
    SnapshotCounts snapshot_reasons{};
    SnapshotReason reason=SnapshotReason::None;
    std::array<uint32_t,36> words{}; // exact existing 144-byte snapshot, not a float conversion
    ShaderEvidence shader{};
    uint64_t vs_key{},ps_key{}; // selected keys also for non-camera graphics draws
    std::array<ImageDescriptor,64> image_descriptors{};
    uint32_t image_descriptor_count{},image_descriptors_dropped{};
    bool grouped_details{};
    bool diagnostic_motion_attachment_excluded{};
    uint64_t motion_geometry_key{},motion_object_buffer{};
    uint32_t motion_flags{};
    bool motion_index_snapshot{};
    bool provenance_details{};
    uint32_t provenance_bindings_lost{};
    std::array<Provenance::Identity,8> color_provenance{};
    Provenance::Identity depth_provenance{};
    std::array<uint32_t,8> blend_key_bits{},write_masks{};
    std::array<uint32_t,4> blend_constant_bits{};
    uint32_t primitive_key{},logic_key{};
    void Image(ImageDescriptor d) {
        if(image_descriptor_count<image_descriptors.size()) image_descriptors[image_descriptor_count++]=d;
        else ++image_descriptors_dropped;
    }
};
enum class State { Disabled, Armed, Active, Done, Failed };
struct Stats {
    State state;
    uint64_t retained{},dropped{},errors{},closed_intervals{};
};
class Capture {
public:
    static constexpr uint64_t Capacity=8192;
    static constexpr uint64_t ByteCapacity=16*1024*1024;
    static constexpr uint64_t FooterReserve=4096;
    static constexpr std::array<const char*,4> Labels={"stationary","motion","pause","resume"};
    Capture(bool enabled,std::filesystem::path output,std::filesystem::path trigger,
            uint64_t capacity=Capacity,bool shader_details=false,bool grouped=false,
            uint64_t byte_capacity=ByteCapacity,bool provenance=false);
    ~Capture();
    Capture(const Capture&)=delete;
    Capture& operator=(const Capture&)=delete;
    bool Enabled() const { return state==State::Armed || state==State::Active; }
    Draw* Pending() { return state==State::Active && pending ? &draw : nullptr; }
    ShaderEvidence* Shader() { auto* d=Pending(); return d && d->shader.enabled ? &d->shader : nullptr; }
    void Begin(uint64_t vs,uint64_t ps);
    void Snapshot(const void* bytes,size_t size,SnapshotReason reason,Attachment source);
    void Recorded() { if(auto* d=Pending()) d->recorded=true; }
    // Accepted, ordered boundaries only. No presentation/redraw entry point.
    void Boundary(Flip flip,uint64_t camera_frame,Status camera_status);
    bool ProvenanceEnabled() const { return provenance_details; }
    // Observed at a CPU drain point, never attributed as the cause of a draw.
    void ProvenanceEvents(const Provenance::Batch& batch,const char* point,uint64_t closing_flip=0);
    Stats GetStats() const { return {state,retained,dropped,errors,closed}; }
    std::filesystem::path OutputPath() const;
    std::filesystem::path TriggerPath() const;
private:
    void FlushDraw();
    bool Write(const std::string& line,bool draw_line=false);
    void End(bool complete);
    std::filesystem::path output,trigger;
    uint64_t capacity,serial{},interval=1,retained{},dropped{},errors{},closed{},incomplete_metadata{},incomplete_shader{};
    uint64_t previous_flip{};
    State state=State::Disabled;
    std::FILE* file{};
    Draw draw{};
    bool pending{};
    bool shader_details{};
    bool grouped{};
    uint32_t window{};
    uint64_t byte_capacity{},bytes{},byte_drops{};
    bool byte_full{};
    bool provenance_details{};
    uint64_t provenance_event_drops{},provenance_lost{},provenance_incomplete_draws{};
    uint64_t provenance_loss_baseline{},provenance_attempt_baseline{},provenance_busy_drains{};
    uint64_t provenance_batches_seen{},provenance_batches_written{},provenance_events_seen{},provenance_events_written{};
    uint64_t provenance_empty_elided{},provenance_interval_empty{},provenance_last_attempt{},provenance_last_loss{};
    bool provenance_counter_record_known{};
};
bool Requested(const char* inputs,const char* capture);
bool ShaderRequested(const char* inputs,const char* capture,const char* shader);
bool GroupRequested(const char* inputs,const char* capture,const char* shader,const char* group);
} // namespace Vulkan::Gow3::Pair
