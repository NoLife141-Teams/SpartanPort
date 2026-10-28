// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_pair_capture.h"
#include <cstring>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <utility>
namespace Vulkan::Gow3::Pair {
namespace {
int64_t BoundaryTime() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
void Hex(std::ostream& o,uint64_t v) { o << '"' << std::hex << v << std::dec << '"'; }
void Target(std::ostream& o,const Attachment& a) {
    o << "{\"uid\":"; Hex(o,a.uid); o << ",\"alias_generation\":"; Hex(o,a.alias_generation);
    o << ",\"host_image\":"; Hex(o,a.image); o << ",\"render_view\":"; Hex(o,a.view);
    o << ",\"guest_address_label\":"; Hex(o,a.address);
    o << ",\"format\":" << a.format << ",\"cache_samples\":" << a.cache_samples
      << ",\"host_samples\":" << a.host_samples << ",\"width\":" << a.width << ",\"height\":" << a.height
      << ",\"requested_mip\":" << a.mip << ",\"requested_layer\":" << a.layer << ",\"proxy\":" << a.proxy << '}';
}
void Token(std::ostream& o,Flip f) {
    o << "{\"id\":"; Hex(o,f.id); o << ",\"address\":"; Hex(o,f.address);
    o << ",\"buffer\":" << f.buffer << ",\"eop\":" << f.eop << '}';
}
void Resource(std::ostream& o,const Provenance::Identity& r) {
    // Fixed schema, decimal object labels; no pointers are retained/dereferenced.
    o << "{\"uid\":" << r.uid << ",\"slot\":" << r.slot
      << ",\"backing_ordinal\":" << r.backing_ordinal << ",\"view_ordinal\":" << r.view_ordinal
      << ",\"view_slot\":" << r.view_slot << ",\"image\":" << r.image << ",\"view\":" << r.view
      << ",\"registry_generation\":" << r.registry_generation
      << ",\"alias_authority_generation\":" << r.alias_authority_generation
      << ",\"address_label\":" << r.address << ",\"resolution\":" << uint32_t(r.resolution)
      << ",\"current_backing\":" << r.current_backing
      << ",\"format\":" << r.format << ",\"view_format\":" << r.view_format
      << ",\"samples\":" << r.samples << ",\"view_aspect_mask\":" << r.aspect
      << ",\"range\":[" << r.mip << ',' << r.levels << ',' << r.layer << ',' << r.layers << "]}";
}
}
bool Requested(const char* inputs,const char* capture) {
    return IsEnabledValue(inputs) && IsEnabledValue(capture);
}
bool ShaderRequested(const char* inputs,const char* capture,const char* shader) {
    return Requested(inputs,capture) && IsEnabledValue(shader);
}
bool GroupRequested(const char* inputs,const char* capture,const char* shader,const char* group) {
    return ShaderRequested(inputs,capture,shader) && IsEnabledValue(group);
}
Capture::Capture(bool enabled,std::filesystem::path output_,std::filesystem::path trigger_,uint64_t cap,bool details,
                 bool group,uint64_t byte_cap,bool provenance)
    : output(std::move(output_)),trigger(std::move(trigger_)),capacity(cap),shader_details(details),
      grouped(group),byte_capacity(byte_cap),provenance_details(enabled && provenance && group) {
    if(!enabled) return;
    if(!output.is_absolute() || !trigger.is_absolute() || output.lexically_normal()==trigger.lexically_normal() || cap>Capacity ||
       (group && !details) || byte_cap>ByteCapacity || byte_cap<FooterReserve*2) {
        state=State::Failed; return;
    }
    state=State::Armed;
}
Capture::~Capture() {
    if(state==State::Active) { FlushDraw(); End(false); }
    if(file) std::fclose(file);
}
std::filesystem::path Capture::OutputPath() const {
    return grouped ? std::filesystem::path(output.string()+"."+Labels[window]+".jsonl") : output;
}
std::filesystem::path Capture::TriggerPath() const {
    return grouped ? std::filesystem::path(trigger.string()+"."+Labels[window]) : trigger;
}
bool Capture::Write(const std::string& line,bool draw_line) {
    if(!file) return false;
    const auto limit=byte_capacity-(draw_line?FooterReserve:0);
    if(line.size()+1>limit || bytes>limit-line.size()-1) {
        if(draw_line) { ++byte_drops; byte_full=true; } else ++errors;
        return false;
    }
    bytes+=line.size()+1;
    if(std::fwrite(line.data(),1,line.size(),file)!=line.size() || std::fputc('\n',file)==EOF) {
        ++errors; return false;
    }
    return true;
}
void Capture::Begin(uint64_t vs,uint64_t ps) {
    if(!Enabled()) return;
    ++serial;
    if(state!=State::Active) return;
    FlushDraw(); draw={}; draw.serial=serial; draw.interval=interval; draw.vs=vs; draw.ps=ps; pending=true;
    draw.shader.enabled=shader_details && vs==DepthShader;
    draw.grouped_details=grouped;
    draw.provenance_details=provenance_details;
}
void Capture::ProvenanceEvents(const Provenance::Batch& batch,const char* point,uint64_t closing_flip) {
    if(!provenance_details) return;
    if(state==State::Armed) {
        // The ordered pre-boundary drain establishes this observation window's
        // baseline. Lifetime counters are never reset in the producer inbox.
        provenance_loss_baseline=provenance_lost=batch.lost;
        provenance_attempt_baseline=batch.attempts;
        provenance_busy_drains=batch.busy;
        provenance_last_attempt=batch.attempts; provenance_last_loss=batch.lost;
        provenance_counter_record_known=true; // persisted in the start record
        return;
    }
    if(state!=State::Active) return;
    provenance_lost=std::max(provenance_lost,batch.lost);
    provenance_busy_drains+=batch.busy;
    ++provenance_batches_seen; provenance_events_seen+=batch.count;
    const bool ordinary=point && (std::strcmp(point,"before_bindings")==0 || std::strcmp(point,"after_recording")==0);
    // Only elide EMPTY ordinary observations whose counters are already on
    // disk (or in the start baseline). Keep every event, busy drain, counter
    // change, unknown point and accepted-flip boundary. No draw is filtered.
    if(ordinary && !closing_flip && !batch.count && !batch.busy && provenance_counter_record_known &&
       batch.attempts==provenance_last_attempt && batch.lost==provenance_last_loss) {
        ++provenance_empty_elided; ++provenance_interval_empty; return;
    }
    std::ostringstream o;
    o << "{\"kind\":\"provenance_events\",\"observed_at\":\"" << point
      << "\",\"interval\":" << interval << ",\"draw_token\":" << (pending?draw.serial:0)
      << ",\"closing_flip\":" << closing_flip << ",\"attempts_total\":" << batch.attempts
      << ",\"lost_total\":" << batch.lost << ",\"drain_busy\":" << batch.busy
      << ",\"cpu_observation_only\":true,\"events\":[";
    for(uint32_t i=0;i<batch.count;++i) {
        if(i) o << ','; const auto& e=batch.events[i];
        o << "[" << e.attempt << ',' << e.uid << ',' << e.slot << ',' << e.related_uid
          << ',' << e.registry_generation << ',' << uint32_t(e.reason) << ',' << e.address << ',' << e.size << ']';
    }
    o << "]}";
    if(!Write(o.str(),true)) provenance_event_drops+=batch.count+1;
    else {
        ++provenance_batches_written; provenance_events_written+=batch.count;
        provenance_last_attempt=batch.attempts; provenance_last_loss=batch.lost;
        provenance_counter_record_known=true;
    }
}
void Capture::Snapshot(const void* bytes,size_t size,SnapshotReason why,Attachment source) {
    auto* d=Pending(); if(!d || size!=144) return;
    ++d->snapshot_observations; d->snapshot_missing += bytes==nullptr;
    ++d->snapshot_reasons[size_t(ResolveSnapshotReason(bytes!=nullptr,true,why))];
    std::array<uint32_t,36> words{};
    if(bytes) std::memcpy(words.data(),bytes,144);
    if(d->camera_observed) {
        d->snapshot_conflict |= d->snapshot_present!=bool(bytes) || (bytes && d->words!=words) ||
                               d->camera_depth.uid!=source.uid || d->camera_depth.image!=source.image;
        // Keep the first exact observation; never heal a missing copy.
        return;
    }
    d->camera_observed=true; d->snapshot_present=bytes!=nullptr;
    d->words=words; d->reason=why; d->camera_depth=source;
}
void Capture::FlushDraw() {
    if(!pending) return;
    pending=false;
    if(retained>=capacity || byte_full) { ++dropped; byte_drops+=byte_full; return; }
    const auto& d=draw;
    if(provenance_details) {
        bool incomplete=d.provenance_bindings_lost!=0;
        const auto known=[](const Provenance::Identity& r) {
            return r.resolution==Provenance::Resolution::NativeView || r.resolution==Provenance::Resolution::NullView;
        };
        for(uint32_t i=0;i<d.image_descriptor_count;++i)
            incomplete |= !d.image_descriptors[i].provenance.observed || !known(d.image_descriptors[i].provenance.resource);
        for(uint32_t i=0;i<std::min<uint32_t>(d.color_count,8);++i) incomplete |= !known(d.color_provenance[i]);
        if(d.depth_provenance.view) incomplete |= !known(d.depth_provenance); // includes stencil-only attachments
        provenance_incomplete_draws+=incomplete;
    }
    incomplete_metadata += d.viewport_overflow || d.proxy_path || d.image_descriptors_dropped ||
        (d.recorded && (!d.viewport_count || (grouped && !d.vs_key)));
    const auto& s=d.shader;
    incomplete_shader += s.enabled && d.recorded &&
        (!s.dynamic_seen || !s.camera_binding_seen || s.bindings_dropped || !s.vs_key ||
         (grouped && (!s.vertex_input_seen || s.vertex_dropped)));
    std::ostringstream o;
    o << "{\"kind\":\"draw\",\"interval\":" << d.interval << ",\"draw\":" << d.serial
      << ",\"recorded\":" << d.recorded << ",\"indirect\":" << d.indirect
      << ",\"extra_square_draw\":" << d.extra_square_draw << ",\"vs\":"; Hex(o,d.vs);
    o << ",\"ps\":"; Hex(o,d.ps);
    o << ",\"exports\":" << d.exports << ",\"bound\":" << d.bound
      << ",\"clip_space\":" << d.clip_space << ",\"clip_disabled\":" << d.clip_disabled
      << ",\"raster_sample_key\":" << d.raster_samples << ",\"guest_depth_samples\":" << d.guest_depth_samples
      << ",\"depth_clear_bits\":" << d.depth_clear_bits << ",\"depth_test\":" << d.depth_test
      << ",\"depth_write\":" << d.depth_write << ",\"depth_compare\":" << d.depth_compare
      << ",\"requested_depth_clear\":" << d.depth_clear << ",\"legacy_ui_hint\":" << d.legacy_ui_hint
      << ",\"proxy_path\":" << d.proxy_path << ",\"diagnostic_motion_attachment_excluded\":" << d.diagnostic_motion_attachment_excluded
      << ",\"motion_geometry_key\":" << d.motion_geometry_key << ",\"motion_object_buffer\":" << d.motion_object_buffer
      << ",\"motion_flags\":" << d.motion_flags << ",\"motion_index_snapshot\":" << d.motion_index_snapshot
      << ",\"render_extent\":[" << d.width << ',' << d.height << ']';
    const auto reasons=FormatSnapshotCounts(d.snapshot_reasons);
    o << ",\"camera_observed\":" << d.camera_observed << ",\"snapshot_present\":" << d.snapshot_present
      << ",\"snapshot_observations\":" << d.snapshot_observations << ",\"snapshot_missing\":" << d.snapshot_missing
      << ",\"snapshot_reasons\":\"" << reasons.data() << '"'
      << ",\"snapshot_conflict\":" << d.snapshot_conflict << ",\"snapshot_reason\":\"" << Name(d.reason) << "\",\"words_hex\":[";
    if(d.snapshot_present) for(size_t i=0;i<d.words.size();++i) { if(i) o << ','; Hex(o,d.words[i]); }
    o << "],\"camera_source_depth\":"; Target(o,d.camera_depth);
    o << ",\"render_depth\":"; Target(o,d.depth);
    o << ",\"render_colors\":[";
    for(size_t i=0;i<std::min<size_t>(d.color_count,d.colors.size());++i) { if(i) o << ','; Target(o,d.colors[i]); }
    o << "],\"viewport_overflow\":" << d.viewport_overflow << ",\"viewports\":[";
    for(size_t i=0;i<std::min<size_t>(d.viewport_count,d.viewports.size());++i) {
        if(i) o << ','; o << "{\"float_bits\":[";
        for(size_t k=0;k<6;++k) { if(k) o << ','; Hex(o,d.viewports[i].bits[k]); }
        o << "],\"scissor\":[" << d.viewports[i].offset[0] << ',' << d.viewports[i].offset[1]
          << ',' << d.viewports[i].extent[0] << ',' << d.viewports[i].extent[1] << "]}";
    }
    o << ']';
    if(grouped) {
        o << ",\"vs_permutation_key\":"; Hex(o,d.vs_key);
        o << ",\"ps_permutation_key\":"; Hex(o,d.ps_key);
        o << ",\"primitive_key\":" << d.primitive_key << ",\"logic_key\":" << d.logic_key << ",\"blend_control_key_bits\":[";
        for(size_t i=0;i<d.blend_key_bits.size();++i) { if(i) o << ','; Hex(o,d.blend_key_bits[i]); }
        o << "],\"color_write_masks\":[";
        for(size_t i=0;i<d.write_masks.size();++i) { if(i) o << ','; o << d.write_masks[i]; }
        o << "],\"blend_constant_bits\":[";
        for(size_t i=0;i<d.blend_constant_bits.size();++i) { if(i) o << ','; Hex(o,d.blend_constant_bits[i]); }
        o << ']';
        o << ",\"image_descriptors_dropped\":" << d.image_descriptors_dropped << ",\"image_descriptors\":[";
        for(size_t i=0;i<d.image_descriptor_count;++i) {
            if(i) o << ','; const auto& b=d.image_descriptors[i];
            o << "{\"binding\":" << b.binding << ",\"element\":" << b.element << ",\"type\":" << b.type
              << ",\"layout\":" << b.layout << ",\"view_label\":"; Hex(o,b.view);
            if(provenance_details) {
                o << ",\"provenance_observed\":" << b.provenance.observed
                  << ",\"path\":" << uint32_t(b.provenance.path) << ",\"resource\":";
                Resource(o,b.provenance.resource);
            }
            o << '}';
        }
        o << ']';
    }
    if(provenance_details) {
        o << ",\"provenance_bindings_lost\":" << d.provenance_bindings_lost << ",\"effective_color_provenance\":[";
        for(uint32_t i=0;i<std::min<uint32_t>(d.color_count,8);++i) {
            if(i) o << ','; Resource(o,d.color_provenance[i]);
        }
        o << "],\"effective_depth_provenance\":"; Resource(o,d.depth_provenance);
    }
    if(s.enabled) {
        o << ",\"shader_evidence\":{\"vs_permutation_key\":"; Hex(o,s.vs_key);
        o << ",\"ps_permutation_key\":"; Hex(o,s.ps_key);
        o << ",\"native_depth_clip_control\":" << s.native_clip_control
          << ",\"depth_clip_enable_supported\":" << s.depth_clip_feature
          << ",\"depth_range_unrestricted\":" << s.unrestricted_range
          << ",\"depth_clamp_key\":" << s.depth_clamp_key << ",\"depth_clip_key\":" << s.depth_clip_key
          << ",\"dynamic_state_recorded\":" << s.dynamic_seen << ",\"bias_enabled\":" << s.bias_enabled
          << ",\"bias_bits\":[";
        for(size_t i=0;i<s.bias_bits.size();++i) { if(i) o << ','; Hex(o,s.bias_bits[i]); }
        o << "],\"bounds_enabled\":" << s.bounds_enabled << ",\"bounds_bits\":[";
        for(size_t i=0;i<s.bounds_bits.size();++i) { if(i) o << ','; Hex(o,s.bounds_bits[i]); }
        o << "],\"bindings_dropped\":" << s.bindings_dropped << ",\"camera_binding_seen\":" << s.camera_binding_seen
          << ",\"bindings\":[";
        for(size_t i=0;i<s.binding_count;++i) {
            if(i) o << ','; const auto& b=s.bindings[i];
            o << "{\"resource_index\":" << b.resource_index << ",\"descriptor_binding\":" << b.descriptor_binding
              << ",\"offset_index\":" << b.offset_index << ",\"resource_kind\":" << b.resource_kind
              << ",\"host_buffer_label\":"; Hex(o,b.host_buffer);
            o << ",\"offset\":" << b.offset << ",\"range\":" << b.range << ",\"ring_copy\":" << b.ring_copy << '}';
        }
        // Info::stores is not persisted in cached Info; absence cannot prove no FragDepth.
        o << "],\"fragment_depth_export\":\"inspect_exact_ps_spirv\"";
        if(grouped) {
            o << ",\"vertex_input_seen\":" << s.vertex_input_seen << ",\"dynamic_vertex_input\":" << s.dynamic_vertex_input
              << ",\"vertex_dropped\":" << s.vertex_dropped << ",\"vertex_attributes\":[";
            for(size_t i=0;i<s.attribute_count;++i) {
                if(i) o << ','; const auto& a=s.attributes[i];
                o << "[" << a.location << ',' << a.binding << ',' << a.format << ',' << a.offset << ']';
            }
            o << "],\"vertex_streams\":[";
            for(size_t i=0;i<s.stream_count;++i) {
                if(i) o << ','; const auto& b=s.streams[i];
                o << "{\"binding\":" << b.binding << ",\"stride\":" << b.stride << ",\"rate\":" << b.rate
                  << ",\"divisor\":" << b.divisor << ",\"buffer_label\":"; Hex(o,b.buffer);
                o << ",\"offset\":" << b.offset << ",\"guest_declared_size\":" << b.size << '}';
            }
            o << ']';
        }
        o << '}';
    }
    o << '}'; if(Write(o.str(),true)) ++retained; else ++dropped;
}
void Capture::End(bool complete) {
    std::ostringstream o;
    o << "{\"kind\":\"end\",\"closed_intervals\":" << closed << ",\"retained_draws\":" << retained
      << ",\"dropped_draws\":" << dropped << ",\"io_errors_before_end\":" << errors
      << ",\"incomplete_metadata_draws\":" << incomplete_metadata
      << ",\"incomplete_shader_draws\":" << incomplete_shader
      << ",\"byte_budget\":" << byte_capacity << ",\"bytes_before_end\":" << bytes << ",\"byte_budget_drops\":" << byte_drops
      << ",\"complete_instrumented_draw_window\":" << (complete && !dropped && !errors && !incomplete_metadata && !incomplete_shader &&
          (!provenance_details || (!provenance_event_drops && provenance_lost==provenance_loss_baseline &&
                                  !provenance_busy_drains && !provenance_incomplete_draws)))
      << ",\"pixels_captured\":false,\"pre_ui_color_proven\":false,\"sample_locations_captured\":false";
    if(provenance_details) o << ",\"provenance_event_records_dropped\":" << provenance_event_drops
        << ",\"provenance_lost_total\":" << provenance_lost
        << ",\"provenance_loss_baseline\":" << provenance_loss_baseline
        << ",\"provenance_busy_drains\":" << provenance_busy_drains
        << ",\"provenance_incomplete_draws\":" << provenance_incomplete_draws
        << ",\"provenance_batches_seen\":" << provenance_batches_seen
        << ",\"provenance_batches_written\":" << provenance_batches_written
        << ",\"provenance_empty_batches_elided\":" << provenance_empty_elided
        << ",\"provenance_batches_dropped\":" << (provenance_batches_seen-provenance_batches_written-provenance_empty_elided)
        << ",\"provenance_events_seen\":" << provenance_events_seen
        << ",\"provenance_events_written\":" << provenance_events_written
        << ",\"provenance_events_dropped\":" << (provenance_events_seen-provenance_events_written)
        << ",\"content_versions_tracked\":false,\"resource_lifetime_retained\":false";
    o << '}';
    Write(o.str());
    if(file && std::fflush(file)!=0) ++errors;
    if(file) { if(std::fclose(file)!=0) ++errors; file=nullptr; }
    state=errors?State::Failed:State::Done;
    if(errors) std::printf("GoW3 pair metadata: write/flush/close error; file is not certified complete\n");
}
void Capture::Boundary(Flip flip,uint64_t camera_frame,Status camera_status) {
    if(!Enabled() || !flip.id) return;
    if(previous_flip==flip.id) { if(state==State::Active) { FlushDraw(); End(false); } state=State::Failed; return; }
    previous_flip=flip.id;
    if(state==State::Armed) {
        ++interval;
        std::error_code error;
        if(!std::filesystem::is_regular_file(std::filesystem::symlink_status(TriggerPath(),error)) || error) return;
        // The budget counts physical bytes. Text mode inserts an uncounted CR
        // for every LF on Windows. Keep exclusive creation, but write binary.
        file=std::fopen(OutputPath().string().c_str(),"wbx");
        if(!file) { state=State::Failed; std::printf("GoW3 pair metadata: output open failed; no overwrite or retry\n"); return; }
        state=State::Active;
        std::ostringstream o;
        o << "{\"kind\":\"start\",\"schema\":\"" << (provenance_details?"gow3-provenance-v2":grouped?"gow3-group-metadata-v1":shader_details?"gow3-pair-metadata-v2":"gow3-pair-metadata-v1")
          << "\",\"shader_details\":" << shader_details << ",\"capacity\":" << capacity
          << ",\"byte_budget\":" << byte_capacity << ",\"window_index\":" << window
          << ",\"requested_label\":\"" << (grouped?Labels[window]:"unlabeled") << "\",\"label_is_user_intent_not_measured_motion\":true"
          << ",\"first_interval\":" << interval << ",\"preceding_flip\":"; Token(o,flip);
        o << ",\"preceding_camera_frame\":" << camera_frame
          << ",\"cpu_boundary_unix_ns\":" << BoundaryTime()
          << ",\"preceding_interval_excluded\":true,\"scope\":\"ordered draw-recording path only; compute, transfers and filtered/HLE operations excluded\"";
        if(provenance_details) o << ",\"provenance_loss_baseline\":" << provenance_loss_baseline
            << ",\"provenance_attempt_baseline\":" << provenance_attempt_baseline
            << ",\"empty_unchanged_ordinary_observations_elided\":true"
            << ",\"provenance_scope\":\"CPU hook observations; concurrent events may drain later; no GPU/frame-causal ordering or exhaustive write coverage\"";
        o << '}';
        Write(o.str()); if(std::fflush(file)!=0) ++errors;
        std::printf("GoW3 pair metadata: armed window started after ordered flip=%llu; two intervals, no pixels\n",static_cast<unsigned long long>(flip.id));
        return;
    }
    FlushDraw();
    std::ostringstream o; o << "{\"kind\":\"interval\",\"interval\":" << interval << ",\"closing_flip\":"; Token(o,flip);
    o << ",\"camera_frame\":" << camera_frame << ",\"camera_status\":\"" << Name(camera_status)
      << "\",\"cpu_boundary_unix_ns\":" << BoundaryTime();
    if(provenance_details) o << ",\"provenance_empty_batches_elided\":" << provenance_interval_empty;
    o << '}'; provenance_interval_empty=0;
    Write(o.str()); ++interval; ++closed;
    if(closed==2) {
        End(true);
        if(grouped && state==State::Done && window+1<Labels.size()) {
            ++window; retained=dropped=errors=closed=incomplete_metadata=incomplete_shader=bytes=byte_drops=0;
            provenance_event_drops=provenance_incomplete_draws=0; // lifetime loss counter is NOT reset
            provenance_batches_seen=provenance_batches_written=provenance_events_seen=provenance_events_written=0;
            provenance_empty_elided=provenance_interval_empty=0; provenance_counter_record_known=false;
            byte_full=false;
            state=State::Armed; // serial/interval/flip identities deliberately never reset
        }
    }
    else if(std::fflush(file)!=0) ++errors;
}
} // namespace Vulkan::Gow3::Pair
