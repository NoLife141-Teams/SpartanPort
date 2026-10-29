// SPDX-License-Identifier: GPL-2.0-or-later
#include "vk_gow3_nr.h"
#include "gow3_motion.h"
#include "gow3_nr_control.h"
#include "vk_instance.h"
#include "vk_scheduler.h"
#include "vk_dlss.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/amdgpu/spartanport_msaa.h"
#include "video_core/host_shaders/gow3_nr_prepare_comp.h"
#include "video_core/host_shaders/gow3_nr_prepare_msaa_comp.h"
#include "video_core/host_shaders/gow3_nr_expand_comp.h"
#include "video_core/host_shaders/gow3_nr_expand_single_comp.h"
#include "video_core/host_shaders/gow3_nr_encode_comp.h"
#include "video_core/host_shaders/gow3_nr_decode_comp.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
namespace Vulkan {
namespace {
struct PrepareParams {
    std::array<float,16> reprojection{};
    std::array<float,2> jitter{};
    u32 valid{};
};
static_assert(sizeof(PrepareParams)==76);
}
struct Gow3Nr::Impl {
    const Instance& instance;
    Scheduler& scheduler;
    Dlss* dlss{};
    Gow3::CameraInputs camera;
    u64 depth{},frame{},evaluated{},bypassed{},last_flip_scene{},nr_last_depth{};
    u32 width{},height{},nr_captures{};
    bool enabled{},active{},reset=true,initialized{},failed{},nr_created{};
    std::filesystem::path dump_directory;
    struct Owned { VideoCore::UniqueImage image; vk::UniqueImageView view; vk::Format format{}; };
    // Linear scene, depth, motion, linear NR result, replacement, encoded input,
    // encoded NR result and diagnostic guide provenance.
    std::vector<Owned> images;
    vk::UniqueDescriptorSetLayout prepare_desc,expand_desc,nr_desc;
    vk::UniquePipelineLayout prepare_layout,expand_layout,nr_layout;
    vk::UniquePipeline prepare,expand,nr_encode,nr_decode;
    Impl(const Instance& i,Scheduler& s):instance(i),scheduler(s) {}
    void Poll() {
        auto& ui=Gow3::NeuralPreviewControl();
        if(ui.toggles.exchange(0)&1) {
            active=!active;reset=true;
            ui.state=active?Gow3::NrState::Waiting:Gow3::NrState::Off;
            std::printf("GoW3 NR scene: requested %s scene=%llu\n",active?"ON":"OFF",(unsigned long long)frame);
        }
    }
    void Create() {
        const auto device=instance.GetDevice();
        constexpr std::array formats{vk::Format::eR16G16B16A16Sfloat,vk::Format::eR32Sfloat,
            vk::Format::eR16G16Sfloat,vk::Format::eR16G16B16A16Sfloat,vk::Format::eR16G16B16A16Sfloat,
            vk::Format::eR32G32B32A32Sfloat,vk::Format::eR32G32B32A32Sfloat,vk::Format::eR32G32B32A32Sfloat};
        images.resize(8);
        for(u32 n=0;n<images.size();++n) {
            auto& item=images[n]; item.format=formats[n];
            item.image=VideoCore::UniqueImage(device,instance.GetAllocator());
            item.image.Create({.imageType=vk::ImageType::e2D,.format=item.format,.extent={width,height,1},
                .mipLevels=1,.arrayLayers=1,.samples=n==4&&!AmdGpu::NoMsaa()?vk::SampleCountFlagBits::e2:vk::SampleCountFlagBits::e1,
                .tiling=vk::ImageTiling::eOptimal,
                .usage=vk::ImageUsageFlagBits::eStorage|vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferSrc|vk::ImageUsageFlagBits::eTransferDst,
                .initialLayout=vk::ImageLayout::eUndefined});
            item.view=Check(device.createImageViewUnique({.image=vk::Image(item.image),.viewType=vk::ImageViewType::e2D,
                .format=item.format,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}}));
        }
        const auto layout=[&](u32 sampled,u32 stored,vk::UniqueDescriptorSetLayout& desc,u32 push,bool buffer=false) {
            std::vector<vk::DescriptorSetLayoutBinding> bindings;
            for(u32 n=0;n<sampled+stored;++n) bindings.push_back({.binding=n,
                .descriptorType=buffer&&n==sampled+stored-1?vk::DescriptorType::eStorageBuffer:
                    n<sampled?vk::DescriptorType::eSampledImage:vk::DescriptorType::eStorageImage,
                .descriptorCount=1,.stageFlags=vk::ShaderStageFlagBits::eCompute});
            desc=Check(device.createDescriptorSetLayoutUnique({.flags=vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
                .bindingCount=u32(bindings.size()),.pBindings=bindings.data()}));
            const vk::PushConstantRange range{vk::ShaderStageFlagBits::eCompute,0,push};
            return Check(device.createPipelineLayoutUnique({.setLayoutCount=1,.pSetLayouts=&*desc,
                .pushConstantRangeCount=push?1u:0u,.pPushConstantRanges=push?&range:nullptr}));
        };
        prepare_layout=layout(3,4,prepare_desc,sizeof(PrepareParams));
        expand_layout=layout(2,1,expand_desc,0);
        const auto pipeline=[&](std::span<const u32> spirv,vk::PipelineLayout l) {
            auto module=Check(device.createShaderModuleUnique({.codeSize=spirv.size_bytes(),.pCode=spirv.data()}));
            return Check(device.createComputePipelineUnique({}, {.stage={.stage=vk::ShaderStageFlagBits::eCompute,
                .module=*module,.pName="main"},.layout=l}));
        };
        prepare=pipeline(AmdGpu::NoMsaa()?std::span<const u32>(GOW3_NR_PREPARE_COMP):std::span<const u32>(GOW3_NR_PREPARE_MSAA_COMP),*prepare_layout);
        expand=pipeline(AmdGpu::NoMsaa()?std::span<const u32>(GOW3_NR_EXPAND_SINGLE_COMP):std::span<const u32>(GOW3_NR_EXPAND_COMP),*expand_layout);
        nr_layout=layout(1,1,nr_desc,0);
        nr_encode=pipeline(GOW3_NR_ENCODE_COMP,*nr_layout);
        nr_decode=pipeline(GOW3_NR_DECODE_COMP,*nr_layout);
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
    void NrConvert(vk::CommandBuffer cmd,bool encode) {
        const vk::DescriptorImageInfo infos[]{
            {.imageView=*images[encode?0:6].view,.imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=*images[encode?5:3].view,.imageLayout=vk::ImageLayout::eGeneral}};
        const vk::WriteDescriptorSet writes[]{
            {.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&infos[0]},
            {.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&infos[1]}};
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,encode?*nr_encode:*nr_decode);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*nr_layout,0,writes);
        cmd.dispatch((width+7)/8,(height+7)/8,1);Barrier(cmd);
    }
    void NrDump(vk::CommandBuffer cmd) {
        if(dump_directory.empty() || evaluated<24 || nr_captures==8 || (nr_captures>=4&&evaluated<240)) return;
        const auto index=nr_captures++;
        const u64 pixels=u64(width)*height;
        constexpr std::array<u32,5> ids{5,6,1,2,7},bpp{16,16,4,4,16};
        std::array<u64,5> offsets{};u64 bytes=0;
        for(u32 n=0;n<ids.size();++n) {offsets[n]=bytes;bytes+=pixels*bpp[n];}
        auto buffer=std::make_shared<VideoCore::Buffer>(instance,0,bytes,VideoCore::MemoryType::HostCached,"GoW3 NR real guides");
        Barrier(cmd);
        for(u32 n=0;n<ids.size();++n) cmd.copyImageToBuffer(vk::Image(images[ids[n]].image),vk::ImageLayout::eGeneral,
            buffer->Handle(),vk::BufferImageCopy{.bufferOffset=offsets[n],
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={width,height,1}});
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host});
        const auto directory=dump_directory;
        const auto tick=scheduler.CurrentTick(),scene=frame,depth_uid=depth,count=evaluated+1;
        const auto samples=AmdGpu::NoMsaa()?1u:2u;
        const auto vp=camera.current,previous=camera.previous;
        scheduler.DeferOperation([buffer,bytes,offsets,pixels,index,directory,tick,scene,depth_uid,count,vp,previous,samples,w=width,h=height,was_reset=reset] {
            buffer->Invalidate(0,bytes);bool ok=true;
            constexpr const char* names[]{"input.rgba32f","output.rgba32f","depth.r32f","motion.rg16f","guides.rgba32f"};
            constexpr u32 bpp[]{16,16,4,4,16};
            for(u32 n=0;n<5;++n) {
                const auto path=directory/(std::to_string(index)+"."+names[n]);
                auto* file=std::fopen(path.string().c_str(),"wbx");
                if(!file) {ok=false;continue;}
                const bool written=std::fwrite(buffer->mapped_data.data()+offsets[n],1,pixels*bpp[n],file)==pixels*bpp[n];
                const bool closed=std::fclose(file)==0;ok &= written&&closed;
            }
            auto* meta=std::fopen((directory/(std::to_string(index)+".json")).string().c_str(),"wbx");
            if(meta) {
                std::fprintf(meta,"{\"complete\":%s,\"gpu_complete\":true,\"index\":%u,\"tick\":%llu,\"scene\":%llu,\"evaluated\":%llu,\"depth_uid\":%llu,\"width\":%u,\"height\":%u,\"reset\":%s,\"jitter\":[0,0],\"motion_depth\":true,\"hud_included\":false,\"stage\":\"verified scene source before guest composition and HUD\",\"camera_vp\":[",
                    ok?"true":"false",index,(unsigned long long)tick,(unsigned long long)scene,(unsigned long long)count,(unsigned long long)depth_uid,w,h,was_reset?"true":"false");
                for(u32 n=0;n<16;++n) std::fprintf(meta,"%s%.17g",n?",":"",vp[n]);
                std::fputs("],\"previous_vp\":[",meta);
                for(u32 n=0;n<16;++n) std::fprintf(meta,"%s%.17g",n?",":"",previous[n]);
                std::fprintf(meta,"],\"scene_samples\":%u}\n",samples);std::fclose(meta);
            } else ok=false;
            std::printf("GoW3 NR scene: real-guide capture index=%u complete=%u scene=%llu\n",index,ok,(unsigned long long)scene);
        });
        Barrier(cmd);
    }
};
Gow3Nr::Gow3Nr(const Instance& i,Scheduler& s):impl(std::make_unique<Impl>(i,s)) {
    if(!Gow3::NeuralSceneRequested()) return;
    auto& ui=Gow3::NeuralPreviewControl();ui.visible=true;
    impl->dlss=Dlss::Get();
    ui.available=impl->dlss && impl->dlss->NrAvailable() &&
        (AmdGpu::NoMsaa()||i.GetPhysicalDevice().getFeatures().shaderStorageImageMultisample);
    ui.state=ui.available?Gow3::NrState::Off:Gow3::NrState::Unavailable;
    if(!ui.available) return;
    if(const char* value=std::getenv("SPARTAN_GOW3_NR_DUMP")) {
        const std::filesystem::path path(value);
        if(path.is_absolute()&&std::filesystem::is_directory(path)&&std::filesystem::is_empty(path)) impl->dump_directory=path;
    }
    const char* start=std::getenv("SPARTAN_DLSS5_NR");
    impl->enabled=true;impl->active=start&&std::strcmp(start,"1")==0;ui.guided=true;
    ui.state=impl->active?Gow3::NrState::Waiting:Gow3::NrState::Off;
    std::printf("GoW3 NR scene: startup %s\n",impl->active?"ON":"OFF");
    std::puts("GoW3 NR scene: verified pre-HUD scene, real depth/object+camera motion, unjittered; F7 toggles");
}
Gow3Nr::~Gow3Nr() {
    if(impl->initialized) {
        impl->scheduler.Finish();
        impl->dlss->ReleaseNrFeature();
        impl->dlss->Shutdown();
    }
}
bool Gow3Nr::Enabled() const { return impl->enabled; }
bool Gow3Nr::Active() { return impl->enabled&&impl->active&&!impl->failed; }
u64 Gow3Nr::Depth() const { return impl->depth; }
bool Gow3Nr::OnFlip() {
    auto& p=*impl;
    if(!Enabled()) return false;
    const bool missing=p.frame==p.last_flip_scene;
    p.last_flip_scene=p.frame;
    if(missing) {p.reset=true;p.camera=Gow3::CameraInputs{};
        if(p.active&&!p.failed) Gow3::NeuralPreviewControl().state=Gow3::NrState::Waiting;
    }
    return missing;
}
void Gow3Nr::Observe(u64 shader,u32 slot,const void* bytes,size_t size,u64 depth,u32 w,u32 h) {
    if(!Enabled()) return;
    impl->depth=depth;
    impl->camera.Observe(shader,slot,bytes,size,depth,w,h);
}
vk::ImageView Gow3Nr::Process(vk::ImageView depth,vk::ImageLayout depth_layout,
    vk::ImageView color,vk::ImageLayout color_layout,vk::ImageView motion,u64 depth_uid,u32 w,u32 h) {
    auto& p=*impl;
    if(!Enabled()) return {};
    const auto status=p.camera.FinishFrame();
    p.Poll();
    const bool active=Active();
    ++p.frame;
    if(!active || !depth || !color || !motion || depth_uid!=p.depth || (w!=1920||h!=1080) ||
       (p.initialized && (w!=p.width || h!=p.height)) ||
       status!=Gow3::Status::Candidate) {
        Gow3::NeuralPreviewControl().state=p.failed?Gow3::NrState::Failed:
                p.active?Gow3::NrState::Waiting:Gow3::NrState::Off;
        p.reset=true;++p.bypassed;
        if(p.bypassed%120==1) std::printf("GoW3 NR scene: bypass scene=%llu camera=%s sources=%u active=%u size=%ux%u\n",
            (unsigned long long)p.frame,Gow3::Name(status),bool(depth&&color&&motion),active,w,h);
        return {};
    }
    bool first=!p.initialized;
    if(first) {p.width=w;p.height=h;p.Create();}
    // Synchronize the recorder before calling NGX; source cache lock is released.
    // NGX changes compute bindings, followed by explicit bindings for expansion.
    // NR receives restored display encoding; the guest composition still reads linear RGB.
    if(!p.nr_created) p.scheduler.WaitSubmitted();
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
    PrepareParams params;params.jitter={};params.valid=1;
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
    std::vector<vk::DescriptorImageInfo> general{
        {.imageView=depth,.imageLayout=depth_layout},{.imageView=color,.imageLayout=color_layout},
        {.imageView=motion,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[0].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[1].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[2].view,.imageLayout=vk::ImageLayout::eGeneral}};
    general.push_back({.imageView=*p.images[7].view,.imageLayout=vk::ImageLayout::eGeneral});
    cmd.pushConstants(*p.prepare_layout,vk::ShaderStageFlagBits::eCompute,0,sizeof(params),&params);
    dispatch(*p.prepare,*p.prepare_layout,general,3);
    Impl::Barrier(cmd);
    if(p.nr_last_depth!=depth_uid) p.reset=true;
    p.NrConvert(cmd,true);
    bool ok=p.nr_created || p.dlss->CreateNrFeature(cmd,w,h);
    if(ok) p.nr_created=true;
    if(ok) ok=p.dlss->EvaluateNrScene(cmd,{p.Resource(5),p.Resource(6),p.Resource(1),p.Resource(2),p.reset});
    if(!ok) {
        p.failed=true;p.reset=true;auto& ui=Gow3::NeuralPreviewControl();ui.state=Gow3::NrState::Failed;ui.available=false;
        std::printf("GoW3 NR scene: evaluation failed; original scene retained (%s)\n",p.dlss->Problem().c_str());
        return {};
    }
    Impl::Barrier(cmd);
    p.NrDump(cmd);
    p.NrConvert(cmd,false);
    const vk::DescriptorImageInfo expanded[]{
        {.imageView=*p.images[3].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[0].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[4].view,.imageLayout=vk::ImageLayout::eGeneral}};
    dispatch(*p.expand,*p.expand_layout,expanded,2);Impl::Barrier(cmd);
    auto& ui=Gow3::NeuralPreviewControl();ui.width=w;ui.height=h;
    const bool was_reset=p.reset;
    const auto count=++p.evaluated;
    const auto scene=p.frame;
    p.scheduler.DeferOperation([count,scene,was_reset] {
        auto& live=Gow3::NeuralPreviewControl();live.completed=count;live.hud_protected=true;
        auto expected=Gow3::NrState::Waiting;
        live.state.compare_exchange_strong(expected,Gow3::NrState::Active);
        if(was_reset||count%120==0) std::printf("GoW3 NR scene: GPU-complete=%llu scene=%llu reset=%u depth+motion=1 pre-HUD=1\n",
            (unsigned long long)count,(unsigned long long)scene,was_reset);
    });
    p.reset=false;p.nr_last_depth=depth_uid;
    return *p.images[4].view;
}
}
