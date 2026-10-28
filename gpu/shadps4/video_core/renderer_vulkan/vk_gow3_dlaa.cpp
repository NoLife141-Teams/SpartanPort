// SPDX-License-Identifier: GPL-2.0-or-later
#include "vk_gow3_dlaa.h"
#include "gow3_motion.h"
#include "gow3_dlaa_control.h"
#include "vk_instance.h"
#include "vk_scheduler.h"
#include "vk_dlss.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/gow3_dlaa_prepare_comp.h"
#include "video_core/host_shaders/gow3_dlaa_expand_comp.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <chrono>

namespace Vulkan {
namespace {
bool SupportedSize(u32 w,u32 h) { return (w==1920&&h==1080)||(w==2560&&h==1440); }
float Halton(u64 n,u32 base) {
    float f=1,value=0;
    while(n) { f/=base; value+=f*float(n%base); n/=base; }
    return value;
}
struct PrepareParams {
    std::array<float,16> reprojection{};
    std::array<float,2> jitter{};
    u32 valid{};
};
static_assert(sizeof(PrepareParams)==76);
}
struct Gow3Dlaa::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    Dlss* dlss{};
    Gow3::CameraInputs camera;
    u64 depth{},frame{},evaluated{},bypassed{},last_flip_scene{};
    u32 width{},height{};
    bool enabled{},active{},reset=true,initialized{},failed{};
    std::filesystem::path control;
    std::filesystem::path dump_directory;
    std::unique_ptr<VideoCore::Buffer> readback;
    bool dump_issued{};
    std::chrono::steady_clock::time_point last_evaluation{};
    std::array<float,2> jitter{};
    struct Owned {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::Format format{};
    };
    // Resolved color, nearest sample depth, motion in pixels, NGX output,
    // and the two-sample replacement consumed by the guest composition.
    std::array<Owned,5> images;
    vk::UniqueDescriptorSetLayout prepare_desc,expand_desc;
    vk::UniquePipelineLayout prepare_layout,expand_layout;
    vk::UniquePipeline prepare,expand;

    Impl(const Instance& i,Scheduler& s):instance(i),scheduler(s) {}
    void Phase() { const auto n=frame%8+1; jitter={Halton(n,2)-.5f,Halton(n,3)-.5f}; }
    void Poll() {
        bool next=active;
        if(!control.empty()) {
            std::ifstream in(control); std::string value; in>>value;
            if(value=="on"||value=="off") next=value=="on";
        }
        auto& ui=Gow3::NativeDlaaControl();
        if(ui.pending_toggles.exchange(0)%2) {
            next=!next;
            if(!control.empty()) { std::ofstream out(control);out<<(next?"on\n":"off\n"); }
        }
        if(active!=next) { active=next; reset=true; std::printf("GoW3 DLAA: switch=%s scene=%llu\n",active?"on":"off",(unsigned long long)frame); }
        ui.requested=active;
        if(!active) ui.state=Gow3::DlaaState::Off;
    }
    void Create() {
        const auto device=instance.GetDevice();
        constexpr std::array formats{vk::Format::eR16G16B16A16Sfloat,vk::Format::eR32Sfloat,
            vk::Format::eR16G16Sfloat,vk::Format::eR16G16B16A16Sfloat,vk::Format::eR16G16B16A16Sfloat};
        for(u32 n=0;n<images.size();++n) {
            auto& item=images[n]; item.format=formats[n];
            item.image=VideoCore::UniqueImage(device,instance.GetAllocator());
            item.image.Create({.imageType=vk::ImageType::e2D,.format=item.format,.extent={width,height,1},
                .mipLevels=1,.arrayLayers=1,.samples=n==4?vk::SampleCountFlagBits::e2:vk::SampleCountFlagBits::e1,
                .tiling=vk::ImageTiling::eOptimal,
                .usage=vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferSrc,
                .initialLayout=vk::ImageLayout::eUndefined});
            item.view=Check(device.createImageViewUnique({.image=vk::Image(item.image),.viewType=vk::ImageViewType::e2D,
                .format=item.format,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}));
        }
        const auto layout=[&](u32 sampled,u32 stored,vk::UniqueDescriptorSetLayout& desc,u32 push) {
            std::vector<vk::DescriptorSetLayoutBinding> bindings;
            for(u32 n=0;n<sampled+stored;++n) bindings.push_back({.binding=n,
                .descriptorType=n<sampled?vk::DescriptorType::eSampledImage:vk::DescriptorType::eStorageImage,
                .descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute});
            desc=Check(device.createDescriptorSetLayoutUnique({.flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                .bindingCount=u32(bindings.size()),.pBindings=bindings.data()}));
            const vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute,0,push};
            return Check(device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*desc,
                .pushConstantRangeCount=push?1u:0u,.pPushConstantRanges=push?&range:nullptr}));
        };
        prepare_layout=layout(3,3,prepare_desc,sizeof(PrepareParams));
        expand_layout=layout(2,1,expand_desc,0);
        const auto pipeline=[&](std::span<const u32> spirv,vk::PipelineLayout l) {
            auto module=Check(device.createShaderModuleUnique({.codeSize=spirv.size_bytes(),.pCode=spirv.data()}));
            return Check(device.createComputePipelineUnique({}, {.stage={.stage=vk::ShaderStageFlagBits::eCompute,
                .module=*module,.pName="main"},.layout=l}));
        };
        prepare=pipeline(GOW3_DLAA_PREPARE_COMP,*prepare_layout);
        expand=pipeline(GOW3_DLAA_EXPAND_COMP,*expand_layout);
        initialized=true;
    }
    static void Barrier(vk::CommandBuffer cmd) {
        const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b});
    }
    Dlss::Resource Resource(u32 index) {
        auto& i=images[index];return {vk::Image(i.image),*i.view,i.format,vk::ImageAspectFlagBits::eColor,width,height};
    }
    void Dump(vk::CommandBuffer cmd) {
        if(dump_directory.empty() || dump_issued || evaluated<32 ||
           !std::filesystem::is_regular_file(dump_directory/"capture.trigger")) return;
        dump_issued=true;
        const u64 bytes=u64(width)*height*8;
        readback=std::make_unique<VideoCore::Buffer>(instance,0,bytes*2,VideoCore::MemoryType::HostCached,"GoW3 DLAA proof");
        for(u32 n=0;n<2;++n) {
            const vk::BufferImageCopy region{.bufferOffset=n*bytes,
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={width,height,1}};
            cmd.copyImageToBuffer(vk::Image(images[n?3:0].image),vk::ImageLayout::eGeneral,readback->Handle(),region);
        }
        const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b});
        const auto tick=scheduler.CurrentTick(),scene=frame,count=evaluated+1;
        scheduler.DeferOperation([this,tick,scene,count,bytes,width=width,height=height] {
            readback->Invalidate(0,bytes*2);
            bool ok=true;
            for(u32 n=0;n<2;++n) {
                auto* f=std::fopen((dump_directory/(n?"output.rgba16f":"input.rgba16f")).string().c_str(),"wbx");
                if(!f) {ok=false;continue;}
                const bool written=std::fwrite(readback->mapped_data.data()+n*bytes,1,bytes,f)==bytes;
                const bool closed=std::fclose(f)==0;ok &= written&&closed;
            }
            auto* f=std::fopen((dump_directory/"capture.json").string().c_str(),"wbx");
            if(f) {
                std::fprintf(f,"{\"complete\":%s,\"tick\":%llu,\"scene\":%llu,\"evaluated\":%llu,\"width\":%u,\"height\":%u,\"format\":\"RGBA float16 little endian linear\",\"bytes_per_image\":%llu,\"gpu_complete\":true}\n",
                    ok?"true":"false",(unsigned long long)tick,(unsigned long long)scene,(unsigned long long)count,width,height,(unsigned long long)bytes);
                std::fclose(f);
            } else ok=false;
            std::printf("GoW3 DLAA: GPU input/output capture complete=%u scene=%llu tick=%llu\n",ok,(unsigned long long)scene,(unsigned long long)tick);
        });
    }
};
Gow3Dlaa::Gow3Dlaa(const Instance& i,Scheduler& s):impl(std::make_unique<Impl>(i,s)) {
    if(!Gow3::NativeDlaaRequested()) return;
    auto& ui=Gow3::NativeDlaaControl();ui.visible=true;
    impl->dlss=Dlss::Get();
    if(!impl->dlss || !impl->dlss->Available() || !i.GetPhysicalDevice().getFeatures().shaderStorageImageMultisample) {
        ui.state=Gow3::DlaaState::Unavailable;
        std::puts("GoW3 DLAA: unavailable (NGX or multisample storage missing)"); return;
    }
    if(const char* value=std::getenv("SPARTAN_GOW3_DLAA_SWITCH")) impl->control=value;
    if(const char* value=std::getenv("SPARTAN_GOW3_DLAA_DUMP")) {
        const std::filesystem::path path(value);
        if(path.is_absolute()&&std::filesystem::is_directory(path)&&std::filesystem::is_empty(path)) impl->dump_directory=path;
    }
    impl->active=true;impl->enabled=true;ui.available=true;impl->Poll();impl->Phase();
    ui.state=impl->active?Gow3::DlaaState::Waiting:Gow3::DlaaState::Off;
    std::puts("GoW3 DLAA: native 1080p/1440p composition hook ready, F10 toggles, 8 jitter phases");
}
Gow3Dlaa::~Gow3Dlaa() {
    if(impl->initialized) { impl->scheduler.Finish();impl->dlss->ReleaseFeature(); }
}
bool Gow3Dlaa::Enabled() const { return impl->enabled; }
bool Gow3Dlaa::Active() { return impl->enabled&&impl->active&&!impl->failed; }
u64 Gow3Dlaa::Depth() const { return impl->depth; }
bool Gow3Dlaa::OnFlip() {
    auto& p=*impl;
    if(!Enabled()) return false;
    const bool missing=p.frame==p.last_flip_scene;
    p.last_flip_scene=p.frame;
    if(missing) {p.reset=true;p.camera=Gow3::CameraInputs{};
        if(p.active&&!p.failed) Gow3::NativeDlaaControl().state=Gow3::DlaaState::Waiting;}
    return missing;
}
std::array<float,2> Gow3Dlaa::Jitter() const { return impl->jitter; }
void Gow3Dlaa::Observe(u64 shader,u32 slot,const void* bytes,size_t size,u64 depth,u32 w,u32 h) {
    if(!Enabled()) return;
    impl->depth=depth;
    impl->camera.Observe(shader,slot,bytes,size,depth,w,h);
}
vk::ImageView Gow3Dlaa::Process(vk::ImageView depth,vk::ImageLayout depth_layout,
    vk::ImageView color,vk::ImageLayout color_layout,vk::ImageView motion,u64 depth_uid,u32 w,u32 h) {
    auto& p=*impl;
    if(!Enabled()) return {};
    const auto jitter=p.jitter;
    const auto status=p.camera.FinishFrame();
    const bool active=Active();
    ++p.frame;p.Phase();p.Poll();
    if(!active || !depth || !color || !motion || depth_uid!=p.depth || !SupportedSize(w,h) ||
       (p.initialized && (w!=p.width || h!=p.height)) ||
       status!=Gow3::Status::Candidate) {
        Gow3::NativeDlaaControl().state=p.failed?Gow3::DlaaState::Failed:
            p.active?Gow3::DlaaState::Waiting:Gow3::DlaaState::Off;
        p.reset=true;++p.bypassed;
        if(p.bypassed%120==1) std::printf("GoW3 DLAA: bypass scene=%llu camera=%s sources=%u active=%u size=%ux%u\n",
            (unsigned long long)p.frame,Gow3::Name(status),bool(depth&&color&&motion),active,w,h);
        return {};
    }
    bool first=!p.initialized;
    if(first) {p.width=w;p.height=h;p.Create();}
    // Synchronize the recorder before calling NGX; source cache lock is released.
    // NGX changes compute bindings, followed by explicit bindings for expansion.
    // Sampling an sRGB view decoded the input to linear RGB. NGX requires its
    // HDR processing flag for linear input, including an SDR display pipeline.
    if(!p.dlss->HasFeature({w,h,w,h,0,true})) p.scheduler.WaitSubmitted();
    auto cmd=p.scheduler.CommandBuffer();
    if(first) for(auto& item:p.images) {
        const vk::ImageMemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,
            .image=vk::Image(item.image),.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b});
    }
    Impl::Barrier(cmd);
    PrepareParams params;params.jitter=jitter;params.valid=1;
    for(u32 c=0;c<4;++c) for(u32 r=0;r<4;++r) {
        double v=0;for(u32 k=0;k<4;++k) v+=p.camera.previous[k*4+r]*p.camera.inverse[c*4+k];
        params.reprojection[c*4+r]=float(v);
    }
    const auto dispatch=[&](vk::Pipeline pipe,vk::PipelineLayout layout,std::span<const vk::DescriptorImageInfo> infos,u32 sampled) {
        std::vector<vk::WriteDescriptorSet> writes;
        for(u32 n=0;n<infos.size();++n) writes.push_back({.dstBinding=n,.descriptorCount=1,
            .descriptorType=n<sampled?vk::DescriptorType::eSampledImage:vk::DescriptorType::eStorageImage,.pImageInfo=&infos[n]});
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,pipe);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,layout,0,writes);
        cmd.dispatch((w+7)/8,(h+7)/8,1);
    };
    const vk::DescriptorImageInfo general[]{
        {.imageView=depth,.imageLayout=depth_layout},{.imageView=color,.imageLayout=color_layout},
        {.imageView=motion,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[0].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[1].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[2].view,.imageLayout=vk::ImageLayout::eGeneral}};
    cmd.pushConstants(*p.prepare_layout,vk::ShaderStageFlagBits::eCompute,0,sizeof(params),&params);
    dispatch(*p.prepare,*p.prepare_layout,general,3);
    Impl::Barrier(cmd);
    const Dlss::FeatureDesc desc{w,h,w,h,0,true};
    bool ok=p.dlss->HasFeature(desc)||p.dlss->CreateFeature(cmd,desc);
    const auto now=std::chrono::steady_clock::now();
    const float frame_ms=p.last_evaluation.time_since_epoch().count()?std::clamp(float(std::chrono::duration<double,std::milli>(now-p.last_evaluation).count()),1.f,250.f):1000.f/60.f;
    p.last_evaluation=now;
    if(ok) ok=p.dlss->Evaluate(cmd,{.color=p.Resource(0),.depth=p.Resource(1),.motion=p.Resource(2),.output=p.Resource(3),
        .jitter_x=-jitter[0],.jitter_y=-jitter[1],.reset=p.reset,.frame_ms=frame_ms,.sharpness=0});
    if(!ok) { p.failed=true;p.reset=true;Gow3::NativeDlaaControl().state=Gow3::DlaaState::Failed;
        std::puts("GoW3 DLAA: NGX failed; native input retained");return {}; }
    Impl::Barrier(cmd);
    p.Dump(cmd);
    Impl::Barrier(cmd);
    const vk::DescriptorImageInfo expanded[]{
        {.imageView=*p.images[3].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[0].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[4].view,.imageLayout=vk::ImageLayout::eGeneral}};
    dispatch(*p.expand,*p.expand_layout,expanded,2);
    Impl::Barrier(cmd);
    if(p.reset || p.evaluated%120==0) std::printf("GoW3 DLAA: evaluated=%llu scene=%llu quality=DLAA size=%ux%u reset=%u jitter=%.6f,%.6f\n",
        (unsigned long long)(p.evaluated+1),(unsigned long long)p.frame,w,h,p.reset,-jitter[0],-jitter[1]);
    auto& ui=Gow3::NativeDlaaControl();ui.width=w;ui.height=h;ui.state=Gow3::DlaaState::Active;
    ++p.evaluated;p.reset=false;
    return *p.images[4].view;
}
}
