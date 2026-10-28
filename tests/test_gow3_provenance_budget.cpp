// SPDX-License-Identifier: GPL-2.0-or-later
// CPU-only writer/filter regression and optional LOCAL metadata replay.
// Boost.JSON is already installed with the existing toolchain; no new dependency.
#include <boost/json/src.hpp>
#include <cassert>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "video_core/renderer_vulkan/gow3_pair_capture.h"
namespace J=boost::json; namespace fs=std::filesystem;
using namespace Vulkan::Gow3; using namespace Vulkan::Gow3::Pair;
static uint64_t N(const J::value& v) {
    if(v.is_string()) return std::stoull(std::string(v.as_string()),nullptr,16);
    if(v.is_bool()) return v.as_bool();
    return v.is_uint64()?v.as_uint64():uint64_t(v.as_int64());
}
static J::object ReadEnd(const fs::path& p) {
    std::ifstream f(p); assert(f); std::string line,last; while(std::getline(f,line)) last=line;
    return J::parse(last).as_object();
}
static void Touch(const fs::path& p) { std::ofstream f(p); assert(f); }
template<class T,size_t S> static void A(std::array<T,S>& out,const J::value& v) {
    const auto& a=v.as_array(); assert(a.size()<=S); for(size_t i=0;i<a.size();++i) out[i]=T(N(a[i]));
}
#define R(field,key) d.field=decltype(d.field)(N(o.at(key)))
#define F(field) R(field,#field)
static Provenance::Identity Identity(const J::object& o) {
    Provenance::Identity d; F(uid);F(slot);F(backing_ordinal);F(view_ordinal);F(view_slot);F(image);F(view);
    F(registry_generation);F(alias_authority_generation);R(address,"address_label");F(resolution);
    F(current_backing);F(format);F(view_format);F(samples);R(aspect,"view_aspect_mask");
    const auto& a=o.at("range").as_array(); assert(a.size()==4);
    d.mip=N(a[0]);d.levels=N(a[1]);d.layer=N(a[2]);d.layers=N(a[3]);return d;
}
static Attachment Target(const J::object& o) {
    Attachment d;F(uid);F(alias_generation);R(image,"host_image");R(view,"render_view");R(address,"guest_address_label");
    F(format);F(cache_samples);F(host_samples);F(width);F(height);R(mip,"requested_mip");R(layer,"requested_layer");F(proxy);return d;
}
static ShaderEvidence Shader(const J::object& o) {
    ShaderEvidence d;d.enabled=true;R(vs_key,"vs_permutation_key");R(ps_key,"ps_permutation_key");
    R(native_clip_control,"native_depth_clip_control");R(depth_clip_feature,"depth_clip_enable_supported");
    R(unrestricted_range,"depth_range_unrestricted");F(depth_clamp_key);F(depth_clip_key);R(dynamic_seen,"dynamic_state_recorded");
    F(bias_enabled);F(bounds_enabled);A(d.bias_bits,o.at("bias_bits"));A(d.bounds_bits,o.at("bounds_bits"));
    F(bindings_dropped);F(camera_binding_seen);F(vertex_input_seen);F(dynamic_vertex_input);F(vertex_dropped);
    for(const auto& v:o.at("bindings").as_array()) {
        const auto& b=v.as_object(); assert(d.binding_count<d.bindings.size());
        d.bindings[d.binding_count++]={uint32_t(N(b.at("resource_index"))),uint32_t(N(b.at("descriptor_binding"))),
            uint32_t(N(b.at("offset_index"))),uint32_t(N(b.at("resource_kind"))),N(b.at("host_buffer_label")),N(b.at("offset")),N(b.at("range")),bool(N(b.at("ring_copy")))};
    }
    for(const auto& v:o.at("vertex_attributes").as_array()) {
        const auto& a=v.as_array(); assert(d.attribute_count<d.attributes.size());
        d.attributes[d.attribute_count++]={uint32_t(N(a[0])),uint32_t(N(a[1])),uint32_t(N(a[2])),uint32_t(N(a[3]))};
    }
    for(const auto& v:o.at("vertex_streams").as_array()) {
        const auto& b=v.as_object(); assert(d.stream_count<d.streams.size());
        d.streams[d.stream_count++]={uint32_t(N(b.at("binding"))),uint32_t(N(b.at("stride"))),uint32_t(N(b.at("rate"))),
            uint32_t(N(b.at("divisor"))),N(b.at("buffer_label")),N(b.at("offset")),N(b.at("guest_declared_size"))};
    } return d;
}
static Draw ParseDraw(const J::object& o) {
    Draw d;R(serial,"draw");F(interval);F(vs);F(ps);F(recorded);F(indirect);F(extra_square_draw);F(exports);F(bound);
    F(clip_space);F(clip_disabled);R(raster_samples,"raster_sample_key");F(guest_depth_samples);F(depth_clear_bits);
    F(depth_test);F(depth_write);F(depth_compare);R(depth_clear,"requested_depth_clear");F(legacy_ui_hint);F(proxy_path);
    const auto& extent=o.at("render_extent").as_array();d.width=N(extent[0]);d.height=N(extent[1]);
    F(camera_observed);F(snapshot_present);F(snapshot_observations);F(snapshot_missing);F(snapshot_conflict);
    for(unsigned i=0;i<unsigned(SnapshotReason::Count);++i) if(o.at("snapshot_reason").as_string()==Name(SnapshotReason(i))) d.reason=SnapshotReason(i);
    const auto reason_text=std::string(o.at("snapshot_reasons").as_string());
    for(unsigned i=0;i<unsigned(SnapshotReason::Count);++i) {
        const auto key=std::string(Name(SnapshotReason(i)))+":"; const auto at=reason_text.find(key);
        if(at!=std::string::npos) d.snapshot_reasons[i]=std::stoul(reason_text.substr(at+key.size()));
    }
    A(d.words,o.at("words_hex"));d.camera_depth=Target(o.at("camera_source_depth").as_object());d.depth=Target(o.at("render_depth").as_object());
    for(const auto& v:o.at("render_colors").as_array()) {assert(d.color_count<8);d.colors[d.color_count++]=Target(v.as_object());}
    F(viewport_overflow);
    for(const auto& v:o.at("viewports").as_array()) {
        assert(d.viewport_count<16);auto& p=d.viewports[d.viewport_count++];const auto& vobj=v.as_object();
        A(p.bits,vobj.at("float_bits"));const auto& a=vobj.at("scissor").as_array();
        p.offset={int32_t(N(a[0])),int32_t(N(a[1]))};p.extent={uint32_t(N(a[2])),uint32_t(N(a[3]))};
    }
    d.grouped_details=d.provenance_details=true;R(vs_key,"vs_permutation_key");R(ps_key,"ps_permutation_key");F(primitive_key);F(logic_key);
    A(d.blend_key_bits,o.at("blend_control_key_bits"));A(d.write_masks,o.at("color_write_masks"));A(d.blend_constant_bits,o.at("blend_constant_bits"));
    F(image_descriptors_dropped);F(provenance_bindings_lost);
    for(const auto& v:o.at("image_descriptors").as_array()) {
        const auto& b=v.as_object();d.Image({uint32_t(N(b.at("binding"))),uint32_t(N(b.at("element"))),uint32_t(N(b.at("type"))),
            uint32_t(N(b.at("layout"))),N(b.at("view_label")),{Identity(b.at("resource").as_object()),Provenance::Path(N(b.at("path"))),bool(N(b.at("provenance_observed")))}});
    }
    unsigned i=0;for(const auto& v:o.at("effective_color_provenance").as_array()) {assert(i<8);d.color_provenance[i++]=Identity(v.as_object());}
    d.depth_provenance=Identity(o.at("effective_depth_provenance").as_object());
    if(const auto* v=o.if_contains("shader_evidence")) d.shader=Shader(v->as_object());return d;
}
#undef R
#undef F
static void FilterTests(const fs::path& root) {
    Capture c(true,root/"filter",root/"trigger",8192,true,true,Capture::ByteCapacity,true);
    Provenance::Batch b;b.attempts=10;b.lost=2;c.ProvenanceEvents(b,"before_accepted_flip",1);
    Touch(c.TriggerPath());c.Boundary({1,1,0,false},1,Status::Missing);const auto path=c.OutputPath();
    for(unsigned i=0;i<2000;++i) c.ProvenanceEvents(b,"after_recording");
    b.attempts++;c.ProvenanceEvents(b,"before_bindings"); // changed counter, even with zero events
    c.ProvenanceEvents(b,"before_accepted_flip",2); // never elide boundaries
    b.busy=true;c.ProvenanceEvents(b,"after_recording");b.busy=false;
    b.lost++;c.ProvenanceEvents(b,"before_bindings");
    b.count=1;b.events[0]={.attempt=11,.uid=7,.reason=Provenance::Reason::CpuOverlap};
    c.ProvenanceEvents(b,"after_recording");c.ProvenanceEvents(b,"after_recording"); // never deduplicate actual events
    c.Boundary({2,1,0,false},2,Status::Missing);c.Boundary({3,1,0,false},3,Status::Missing);
    const auto end=ReadEnd(path);assert(N(end.at("provenance_empty_batches_elided"))==2000);
    assert(N(end.at("provenance_batches_seen"))==2006 && N(end.at("provenance_batches_written"))==6);
    assert(N(end.at("provenance_events_seen"))==2 && N(end.at("provenance_events_written"))==2);
    assert(!N(end.at("provenance_batches_dropped")) && !N(end.at("provenance_events_dropped")));
    assert(!N(end.at("complete_instrumented_draw_window")));
    std::puts("PASS filter: only unchanged empty ordinary observations elided; counter changes, busy drains, boundaries and every event retained");
    {
        Capture failed(true,root/"failed-write",root/"failed-trigger",8192,true,true,8192,true);
        Provenance::Batch baseline;failed.ProvenanceEvents(baseline,"before_accepted_flip",1);
        Touch(failed.TriggerPath());failed.Boundary({1,1,0,false},1,Status::Missing);const auto failed_path=failed.OutputPath();
        Provenance::Batch large;large.attempts=1;large.count=64;
        for(auto& event:large.events) event={~0ull,~0ull,~0ull,~0ull,~0ull,~0ull,~0u,Provenance::Reason::CpuOverlap};
        failed.ProvenanceEvents(large,"after_recording"); // exceeds reserved payload budget
        large.count=0;failed.ProvenanceEvents(large,"after_recording"); // changed counter must still be written
        failed.ProvenanceEvents(large,"after_recording"); // now redundant
        failed.Boundary({2,1,0,false},2,Status::Missing);failed.Boundary({3,1,0,false},3,Status::Missing);
        const auto e=ReadEnd(failed_path);
        assert(N(e.at("provenance_batches_seen"))==3 && N(e.at("provenance_batches_written"))==1);
        assert(N(e.at("provenance_empty_batches_elided"))==1 && N(e.at("provenance_batches_dropped"))==1);
        assert(N(e.at("provenance_events_dropped"))==64 && N(e.at("provenance_event_records_dropped"))==65);
        assert(fs::file_size(failed_path)<=8192 && !N(e.at("complete_instrumented_draw_window")));
        std::puts("PASS rejected batch does not advance serialized-counter baseline; exact event/batch losses and bounded footer");
    }
}
struct Observation { Provenance::Batch batch;std::string point;uint64_t draw{},closing{}; };
static void Replay(const fs::path& root,const fs::path& source) {
    assert(fs::file_size(source)<=Capture::ByteCapacity);
    std::ifstream f(source);assert(f);std::string line;std::map<uint64_t,Draw> draws;std::vector<Observation> observations;
    Provenance::Batch baseline;uint64_t source_interval{};bool closed=false;
    while(std::getline(f,line)) {
        assert(line.size()<65536);const auto o=J::parse(line).as_object();const auto kind=o.at("kind").as_string();
        if(kind=="start") {source_interval=N(o.at("first_interval"));baseline.attempts=N(o.at("provenance_attempt_baseline"));baseline.lost=N(o.at("provenance_loss_baseline"));}
        else if(kind=="draw") {auto d=ParseDraw(o);assert(d.interval==source_interval && draws.size()<4096);draws.emplace(d.serial,d);}
        else if(kind=="provenance_events") {
            assert(N(o.at("interval"))==source_interval && observations.size()<8193);Observation e;
            e.point=std::string(o.at("observed_at").as_string());e.draw=N(o.at("draw_token"));e.closing=N(o.at("closing_flip"));
            e.batch.attempts=N(o.at("attempts_total"));e.batch.lost=N(o.at("lost_total"));e.batch.busy=N(o.at("drain_busy"));
            for(const auto& v:o.at("events").as_array()) {const auto& a=v.as_array();assert(e.batch.count<64);
                e.batch.events[e.batch.count++]={N(a[0]),N(a[1]),N(a[3]),N(a[4]),N(a[6]),N(a[7]),uint32_t(N(a[2])),Provenance::Reason(N(a[5]))};}
            observations.push_back(e);
        } else if(kind=="interval") {closed=true;break;}
    }
    assert(closed && !draws.empty() && !observations.empty() && observations.back().closing);
    const auto delta=observations.back().batch.attempts-baseline.attempts;
    const auto first=draws.begin()->first;assert(draws.rbegin()->first-first+1==draws.size());
    Capture c(true,root/"replay",root/"replay-trigger",8192,true,true,Capture::ByteCapacity,true);
    c.ProvenanceEvents(baseline,"before_accepted_flip",1);Touch(c.TriggerPath());c.Boundary({1,1,0,false},1,Status::Missing);
    const auto output=c.OutputPath();uint64_t events=0;
    for(unsigned repeat=0;repeat<2;++repeat) {
        uint64_t current=0;
        for(auto e:observations) {
            if(e.draw!=current) {
                assert(draws.contains(e.draw));auto d=draws.at(e.draw);c.Begin(d.vs,d.ps);auto* pending=c.Pending();assert(pending);
                d.interval=pending->interval;d.serial+=repeat*draws.size();*pending=d;current=e.draw;
            }
            e.batch.attempts+=repeat*delta;for(unsigned i=0;i<e.batch.count;++i)e.batch.events[i].attempt+=repeat*delta;
            c.ProvenanceEvents(e.batch,e.point.c_str(),e.closing?repeat+2:0);events+=e.batch.count;
        }
        c.Boundary({repeat+2,1,0,false},repeat+2,Status::Candidate);
    }
    const auto end=ReadEnd(output);
    assert(N(end.at("retained_draws"))==2*draws.size() && !N(end.at("dropped_draws")));
    assert(N(end.at("provenance_events_seen"))==events && N(end.at("provenance_events_written"))==events);
    assert(!N(end.at("provenance_batches_dropped")) && N(end.at("complete_instrumented_draw_window")));
    assert(fs::file_size(output)<=Capture::ByteCapacity);
    std::printf("PASS LOCAL SYNTHETIC replay of the FIRST complete interval TWICE: draws=%llu events=%llu bytes=%llu elided=%llu; not a recovered second game interval\n",
        (unsigned long long)(2*draws.size()),(unsigned long long)events,(unsigned long long)fs::file_size(output),(unsigned long long)N(end.at("provenance_empty_batches_elided")));
}
int main(int argc,char** argv) {
    assert(argc==2 || argc==3);const auto root=fs::absolute(argv[1]);assert(!fs::exists(root));fs::create_directories(root);
    FilterTests(root);if(argc==3) Replay(root,fs::absolute(argv[2]));
}
