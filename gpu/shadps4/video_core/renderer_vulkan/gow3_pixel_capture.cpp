// SPDX-License-Identifier: GPL-2.0-or-later
#include "vk_instance.h"
#include "vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/host_shaders/gow3_pixel_extract_comp.h"
#include "gow3_pixel_capture.h"
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace Vulkan::Gow3::Pixels {
Capture::Capture(const Instance& instance,const char* path):directory(path?path:"") {
    if(!directory.is_absolute() || !std::filesystem::is_directory(directory) ||
       !std::filesystem::is_empty(directory)) throw std::runtime_error("Pixel directory must be absolute and empty");
    events=std::fopen((directory/"pixels.jsonl").string().c_str(),"wbx");
    if(!events) throw std::runtime_error("Pixel journal exclusive creation failed");
    const auto physical=instance.GetPhysicalDevice();
    const VkPhysicalDeviceMemoryProperties memory=physical.getMemoryProperties();
    extractor=std::make_unique<Extractor>(VkDevice(instance.GetDevice()),
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,memory,
        physical.getProperties().limits.maxStorageBufferRange,GOW3_PIXEL_EXTRACT_COMP);
    std::fputs("{\"kind\":\"start\",\"schema\":\"gow3-real-pixels-v1\",\"max_frames\":2,\"memory_budget\":268435456,\"file_budget\":201326592,\"pre_hud_proven\":false,\"sample_positions_measured\":false,\"format\":\"little-endian float32 pixel-major sample-major RGBA-linear depth; 2 samples; 5 floats per sample\"}\n",events);
    std::fflush(events);
}
void Capture::Boundary(const CameraFrame& frame,const CameraInputs& camera,bool pair_active) {
    std::scoped_lock lock{mutex};
    for(auto& job:jobs) if(job.interval==frame.number+1) {
        job.closed=true; job.frame=frame; job.camera=camera.current;
        if(!job.attempted) job.error="no-target-draw";
        if(frame.status!=Status::Candidate || frame.unsnapshotted || !frame.samples)
            job.error="camera-interval-invalid";
        if(job.output && (frame.width!=job.output->width || frame.height!=job.output->height))
            job.error="camera-dimensions-mismatch";
        Publish(job);
    }
    if(!first_interval && pair_active) {
        first_interval=frame.number+2;
        for(size_t i=0;i<jobs.size();++i) jobs[i].interval=first_interval+i;
        std::printf("GoW3 pixels: armed two intervals starting %llu\n",(unsigned long long)first_interval);
    }
}
void Capture::Submit(uint64_t tick,vk::Result result) {
    std::scoped_lock lock{mutex};
    for(auto& job:jobs) if(job.output && job.tick==tick) {
        job.submitted=result==vk::Result::eSuccess;
        if(!job.submitted) { job.error="queue-submit-failed"; Publish(job); }
    }
}
void Capture::Record(const Pair::Draw& draw,const Provenance::Identity& depth,
    vk::ImageLayout depth_layout,const Provenance::Identity& color,vk::ImageLayout color_layout,
    Scheduler& scheduler,VideoCore::TextureCache& cache,bool allowed) {
    Job* selected=nullptr;
    {
        std::scoped_lock lock{mutex};
        for(auto& job:jobs) if(job.interval==draw.interval && !job.attempted) { selected=&job; break; }
        if(!selected) return;
        auto& job=*selected; job.attempted=true; job.draw=draw.serial; job.depth=depth; job.color=color;
        if(!allowed || scheduler.HasActiveRendering()) { job.error="active-rendering-or-proxy"; return; }
        job.tick=scheduler.CurrentTick();
        try {
            job.error=cache.WithPixelSources(depth,depth_layout,color,color_layout,job.sampled_components,
                [&](vk::ImageView d,vk::ImageView c,u32 w,u32 h) {
                    job.output=extractor->Allocate(w,h);
                    auto* output=job.output.get(); auto* extract=extractor.get();
                    scheduler.Record([extract,output,d,c,depth_layout,color_layout](vk::CommandBuffer cmd) {
                        extract->Record(VkCommandBuffer(cmd),*output,
                            {VkImageView(d),VkImageLayout(depth_layout)},{VkImageView(c),VkImageLayout(color_layout)});
                    });
                });
        } catch(const std::exception& error) {
            std::printf("GoW3 pixels: record failed: %s\n",error.what()); job.error="allocation-or-record-failed";
        }
    }
    // Never acquire pending_ops_mutex while holding the cache or collector lock.
    if(selected->output && !selected->error) scheduler.DeferOperation([this,selected] { Complete(*selected); });
}
void Capture::Complete(Job& job) {
    std::scoped_lock lock{mutex};
    try { extractor->MarkCompleted(*job.output); job.complete=true; }
    catch(const std::exception&) { job.error="invalid-gpu-completion"; }
    Publish(job);
}
void Capture::Publish(Job& job) {
    if(job.published || !job.closed || (job.output && !job.complete && !job.error)) return;
    const auto filename="interval-"+std::to_string(job.interval)+".rgba-depth.f32";
    if(!job.error && (!job.submitted || !job.complete)) job.error="submission-or-completion-missing";
    if(!job.error) {
        try {
            const auto data=extractor->ReadCompleted(*job.output);
            auto* file=std::fopen((directory/filename).string().c_str(),"wbx");
            if(!file) throw std::runtime_error("exclusive pixel open");
            const bool written=std::fwrite(data.data(),1,data.size(),file)==data.size();
            const bool closed=std::fclose(file)==0;
            if(!written || !closed) throw std::runtime_error("pixel file write");
        } catch(const std::exception& error) {
            std::printf("GoW3 pixels: export failed: %s\n",error.what()); job.error="pixel-export-failed";
        }
    }
    std::ostringstream o; o<<std::setprecision(17);
    o<<"{\"kind\":\"result\",\"complete\":"<<(!job.error?"true":"false")
     <<",\"error\":\""<<(job.error?job.error:"")<<"\",\"interval\":"<<job.interval<<",\"draw\":"<<job.draw
     <<",\"tick\":"<<job.tick<<",\"submitted\":"<<job.submitted<<",\"gpu_complete\":"<<job.complete
     <<",\"closing_flip\":"<<job.frame.flip.id<<",\"camera_frame\":"<<job.frame.number
     <<",\"camera_status\":\""<<Name(job.frame.status)<<"\",\"width\":"<<(job.output?job.output->width:0)
     <<",\"height\":"<<(job.output?job.output->height:0)<<",\"bytes\":"<<(job.output?job.output->bytes:0)
     <<",\"allocated_bytes\":"<<extractor->Allocated()<<",\"file\":\""<<filename<<"\"";
    const auto identity=[&](const char* name,const Provenance::Identity& id) {
        o<<",\""<<name<<"\":{\"uid\":"<<id.uid<<",\"backing\":"<<id.backing_ordinal
         <<",\"view_ordinal\":"<<id.view_ordinal<<",\"image\":"<<id.image<<",\"view\":"<<id.view
         <<",\"address\":"<<id.address<<",\"format\":"<<id.format<<",\"samples\":"<<id.samples<<'}';
    };
    identity("depth",job.depth); identity("color",job.color);
    o<<",\"sampled_components_vk_enum_depth_then_color\":[";
    for(size_t i=0;i<job.sampled_components.size();++i) o<<(i?",":"")<<job.sampled_components[i];
    o<<"],\"color_semantics\":\"actual sampled view, sRGB decoded, component mapping applied; not stored RGBA bytes\"";
    o<<",\"camera_vp\":["; for(size_t i=0;i<job.camera.size();++i) o<<(i?",":"")<<job.camera[i]; o<<"]}";
    const auto line=o.str()+"\n";
    if(std::fwrite(line.data(),1,line.size(),events)!=line.size() || std::fflush(events)!=0)
        std::printf("GoW3 pixels: journal write failed; no valid result may be claimed\n");
    job.published=true;
    std::printf("GoW3 pixels: interval=%llu tick=%llu complete=%u error=%s\n",
        (unsigned long long)job.interval,(unsigned long long)job.tick,!job.error,job.error?job.error:"none");
}
} // namespace Vulkan::Gow3::Pixels
