// SPDX-License-Identifier: GPL-2.0-or-later
#include "vk_gow3_nr.h"
#include "gow3_motion.h"
#include "gow3_nr_control.h"
#include "spartanport_settings.h"
#include "vk_instance.h"
#include "vk_scheduler.h"
#include "vk_gpu_profiler.h"
#include "vk_dlss.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/amdgpu/spartanport_msaa.h"
#include "video_core/host_shaders/gow3_nr_prepare_comp.h"
#include "video_core/host_shaders/gow3_nr_prepare_msaa_comp.h"
#include "video_core/host_shaders/gow3_nr_prepare_guides_comp.h"
#include "video_core/host_shaders/gow3_nr_prepare_msaa_guides_comp.h"
#include "video_core/host_shaders/gow3_nr_expand_comp.h"
#include "video_core/host_shaders/gow3_nr_expand_single_comp.h"
#include "video_core/host_shaders/gow3_nr_encode_comp.h"
#include "video_core/host_shaders/gow3_nr_decode_comp.h"
#include "video_core/host_shaders/gow3_nr_prepare_fused_comp.h"
#include "video_core/host_shaders/gow3_nr_prepare_msaa_fused_comp.h"
#include "video_core/host_shaders/gow3_nr_prepare_guides_fused_comp.h"
#include "video_core/host_shaders/gow3_nr_prepare_msaa_guides_fused_comp.h"
#include "video_core/host_shaders/gow3_nr_expand_fused_comp.h"
#include "video_core/host_shaders/gow3_nr_expand_single_fused_comp.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <utility>
#include <chrono>
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
    u32 appearance=~0u;
    bool enabled{},active{},reset=true,initialized{},failed{},nr_created{};
    bool frame_evaluated{},history_reset{};
    // Diagnostic A/B switch; normal play uses the fused conversion path.
    bool fused=!(std::getenv("SPARTAN_GOW3_NR_FUSED") &&
                 std::strcmp(std::getenv("SPARTAN_GOW3_NR_FUSED"),"0")==0);
    std::filesystem::path dump_directory;
    struct Owned { VideoCore::UniqueImage image; vk::UniqueImageView view; vk::Format format{}; };
    // Linear scene, depth, motion, linear NR result, replacement, encoded input,
    // encoded NR result and diagnostic guide provenance.
    std::vector<Owned> images;
    vk::UniqueDescriptorSetLayout prepare_desc,expand_desc,nr_desc;
    vk::UniquePipelineLayout prepare_layout,expand_layout,nr_layout;
    vk::UniquePipeline prepare,expand,nr_encode,nr_decode,validate_prepare;
    Impl(const Instance& i,Scheduler& s):instance(i),scheduler(s) {}
    void NewEpoch() { reset=true;Gow3::NeuralPreviewControl().generation.fetch_add(1); }
    void UpdateMotionWork() {
        const bool work=enabled&&active&&!failed&&SpartanNr::Unpack(SpartanSettings::Get().nr_appearance.load()).intensity>0;
        if(Gow3::NeuralMotionEnabled().exchange(work)!=work) {NewEpoch();history_reset=true;}
    }
    void Poll() {
        auto& ui=Gow3::NeuralPreviewControl();
        if(ui.toggles.exchange(0)&1) {
            active=!active;NewEpoch();
            ui.state=active?Gow3::NrState::Waiting:Gow3::NrState::Off;
            std::printf("GoW3 NR scene: requested %s scene=%llu\n",active?"ON":"OFF",(unsigned long long)frame);
        }
    }
    void Create() {
        const auto device=instance.GetDevice();
        std::array formats{vk::Format::eR16G16B16A16Sfloat,vk::Format::eR32Sfloat,
            vk::Format::eR16G16Sfloat,vk::Format::eR16G16B16A16Sfloat,vk::Format::eR16G16B16A16Sfloat,
            vk::Format::eR32G32B32A32Sfloat,vk::Format::eR32G32B32A32Sfloat,vk::Format::eR32G32B32A32Sfloat};
        // Provenance is a diagnostic output, not an NGX input. Avoid allocating
        // and writing a full RGBA32F 4K image every frame during normal play.
        const bool guides=!dump_directory.empty();
        if(fused&&guides) formats[3]=vk::Format::eR32G32B32A32Sfloat;
        images.resize(guides?8:7);
        for(u32 n=0;n<images.size();++n) {
            if(fused && !guides && (n==0 || n==3)) continue;
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
        if(!prepare) CreatePipelines();
        std::printf("GoW3 NR scene: fused conversions=%u saved intermediate bytes=%llu\n",fused,
            fused&&!guides?(unsigned long long)width*height*16:0ull);
        initialized=true;
    }
    void CreatePipelines() {
        const auto device=instance.GetDevice();
        const bool guides=!dump_directory.empty();
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
        prepare_layout=layout(3,guides?4:3,prepare_desc,sizeof(PrepareParams));
        expand_layout=layout(2,1,expand_desc,0);
        const auto pipeline=[&](std::span<const u32> spirv,vk::PipelineLayout l) {
            auto module=Check(device.createShaderModuleUnique({.codeSize=spirv.size_bytes(),.pCode=spirv.data()}));
            return Check(device.createComputePipelineUnique({}, {.stage={.stage=vk::ShaderStageFlagBits::eCompute,
                .module=*module,.pName="main"},.layout=l}));
        };
        const auto prepare_code=fused?(guides?
            (AmdGpu::NoMsaa()?std::span<const u32>(GOW3_NR_PREPARE_GUIDES_FUSED_COMP):std::span<const u32>(GOW3_NR_PREPARE_MSAA_GUIDES_FUSED_COMP)):
            (AmdGpu::NoMsaa()?std::span<const u32>(GOW3_NR_PREPARE_FUSED_COMP):std::span<const u32>(GOW3_NR_PREPARE_MSAA_FUSED_COMP))):guides?
            (AmdGpu::NoMsaa()?std::span<const u32>(GOW3_NR_PREPARE_GUIDES_COMP):std::span<const u32>(GOW3_NR_PREPARE_MSAA_GUIDES_COMP)):
            (AmdGpu::NoMsaa()?std::span<const u32>(GOW3_NR_PREPARE_COMP):std::span<const u32>(GOW3_NR_PREPARE_MSAA_COMP));
        prepare=pipeline(prepare_code,*prepare_layout);
        expand=pipeline(fused?
            (AmdGpu::NoMsaa()?std::span<const u32>(GOW3_NR_EXPAND_SINGLE_FUSED_COMP):std::span<const u32>(GOW3_NR_EXPAND_FUSED_COMP)):
            (AmdGpu::NoMsaa()?std::span<const u32>(GOW3_NR_EXPAND_SINGLE_COMP):std::span<const u32>(GOW3_NR_EXPAND_COMP)),*expand_layout);
        if(fused&&guides)
            validate_prepare=pipeline(AmdGpu::NoMsaa()?std::span<const u32>(GOW3_NR_PREPARE_GUIDES_COMP):
                std::span<const u32>(GOW3_NR_PREPARE_MSAA_GUIDES_COMP),*prepare_layout);
        if(!fused || guides) {
            nr_layout=layout(1,1,nr_desc,0);
            nr_encode=pipeline(GOW3_NR_ENCODE_COMP,*nr_layout);
            nr_decode=pipeline(GOW3_NR_DECODE_COMP,*nr_layout);
        }
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
    void NrConvert(vk::CommandBuffer cmd,bool encode,u32 destination=~0u) {
        const vk::DescriptorImageInfo infos[]{
            {.imageView=*images[encode?0:6].view,.imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=*images[destination!=~0u?destination:encode?5:3].view,.imageLayout=vk::ImageLayout::eGeneral}};
        const vk::WriteDescriptorSet writes[]{
            {.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=&infos[0]},
            {.dstBinding=1,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&infos[1]}};
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,encode?*nr_encode:*nr_decode);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*nr_layout,0,writes);
        cmd.dispatch((width+7)/8,(height+7)/8,1);Barrier(cmd);
    }
    bool WantsCapture() const {
        // A four-frame 4K sequence already stores ~1.7 GiB of real guides.
        const u32 limit=width>=3840?4u:8u;
        return !dump_directory.empty() && evaluated>=24 && nr_captures<limit && (nr_captures<4||evaluated>=240);
    }
    void NrDump(vk::CommandBuffer cmd) {
        if(!WantsCapture()) return;
        const auto index=nr_captures++;
        const u64 pixels=u64(width)*height;
        std::vector<u32> ids{5,6,1,2,7},bpp{16,16,4,4,16};
        if(fused) {ids.push_back(3);bpp.push_back(16);}
        std::vector<u64> offsets(ids.size());u64 bytes=0;
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
        const auto look=SpartanNr::Unpack(appearance);
        scheduler.DeferOperation([buffer,bytes,offsets,pixels,index,directory,tick,scene,depth_uid,count,vp,previous,samples,look,w=width,h=height,was_reset=reset] {
            buffer->Invalidate(0,bytes);bool ok=true;
            constexpr const char* names[]{"input.rgba32f","output.rgba32f","depth.r32f","motion.rg16f","guides.rgba32f","reference-input.rgba32f"};
            constexpr u32 bpp[]{16,16,4,4,16,16};
            for(u32 n=0;n<offsets.size();++n) {
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
                std::fprintf(meta,"],\"scene_samples\":%u,\"nr_profile\":%d,\"nr_strengths_percent\":[%d,%d,%d,%d]}\n",
                    samples,look.profile,look.intensity,look.tone,look.structure,look.skin);std::fclose(meta);
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
    impl->UpdateMotionWork();
    ui.state=impl->active?Gow3::NrState::Waiting:Gow3::NrState::Off;
    std::printf("GoW3 NR scene: startup %s\n",impl->active?"ON":"OFF");
    std::puts("GoW3 NR scene: verified pre-HUD scene, real depth/object+camera motion, unjittered; F7 toggles");
    // Compile our format-specific host pipelines before gameplay, while startup
    // is still loading. Extent-dependent images and NGX stay lazy until proven.
    impl->CreatePipelines();
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
bool Gow3Nr::TakeEvaluatedFrame() { return std::exchange(impl->frame_evaluated,false); }
bool Gow3Nr::TakeHistoryReset() { return std::exchange(impl->history_reset,false); }
u64 Gow3Nr::Depth() const { return impl->depth; }
bool Gow3Nr::OnFlip() {
    auto& p=*impl;
    if(!Enabled()) return false;
    p.Poll();p.UpdateMotionWork();
    const bool missing=p.frame==p.last_flip_scene;
    p.last_flip_scene=p.frame;
    if(missing) {p.NewEpoch();p.camera=Gow3::CameraInputs{};
        if(p.active&&!p.failed) Gow3::NeuralPreviewControl().state=Gow3::NrState::Waiting;
    }
    const bool reset_history=TakeHistoryReset();
    return missing || reset_history;
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
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto status=p.camera.FinishFrame(u64(now));
    if(p.camera.discontinuity) {
        p.NewEpoch();p.history_reset=true;
        std::printf("GoW3 NR scene: camera history reset scene=%llu\n",(unsigned long long)p.frame);
    }
    p.Poll();
    const u32 appearance=SpartanSettings::Get().nr_appearance.load();
    if(appearance!=p.appearance) {
        p.appearance=appearance;p.NewEpoch();
        const auto a=SpartanNr::Unpack(appearance);
        std::printf("GoW3 NR appearance: profile=%d intensity=%d tone=%d structure=%d skin=%d\n",
            a.profile,a.intensity,a.tone,a.structure,a.skin);
    }
    const bool active=Active();
    p.UpdateMotionWork();
    ++p.frame;
    // Zero strength is visually neutral. Keep the guest MSAA samples untouched;
    // do not resolve, dispatch NGX or wait for an NR result that cannot contribute.
    if(active && SpartanNr::Unpack(appearance).intensity==0) {
        Gow3::NeuralPreviewControl().state=Gow3::NrState::Neutral;
        p.NewEpoch();
        return {};
    }
    const bool supported_extent=(w==1920&&h==1080)||(w==2560&&h==1440)||(w==3840&&h==2160);
    if(!active || !depth || !color || !motion || depth_uid!=p.depth || !supported_extent ||
       (p.initialized && (w!=p.width || h!=p.height)) ||
       status!=Gow3::Status::Candidate) {
        Gow3::NeuralPreviewControl().state=p.failed?Gow3::NrState::Failed:
                p.active?Gow3::NrState::Waiting:Gow3::NrState::Off;
        p.NewEpoch();++p.bypassed;
        if(p.bypassed%120==1) std::printf("GoW3 NR scene: bypass scene=%llu camera=%s sources=%u active=%u size=%ux%u\n",
            (unsigned long long)p.frame,Gow3::Name(status),bool(depth&&color&&motion),active,w,h);
        return {};
    }
    bool first=!p.initialized;
    if(first) {
        p.width=w;p.height=h;p.Create();
        std::printf("GoW3 NR scene: native resources %ux%u samples=%u (color/depth/motion/output)\n",w,h,AmdGpu::NoMsaa()?1:2);
    }
    // Synchronize the recorder before calling NGX; source cache lock is released.
    // NGX changes compute bindings, followed by explicit bindings for expansion.
    // NR receives restored display encoding; the guest composition still reads linear RGB.
    if(!p.nr_created) p.scheduler.WaitSubmitted();
    auto cmd=p.scheduler.CommandBuffer();
    if(first) for(auto& item:p.images) {
        if(!item.view) continue;
        const vk::ImageMemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eTopOfPipe,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .oldLayout=vk::ImageLayout::eUndefined,.newLayout=vk::ImageLayout::eGeneral,
            .image=vk::Image(item.image),.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,1}};
        cmd.pipelineBarrier2({.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&b});
    }
    Impl::Barrier(cmd);
    auto* profiler=GpuProfiler::Get();
    const u64 previous_profile=profiler?profiler->Current():0;
    if(profiler) profiler->Mark(0x4e520001,[]{return "NR prepare depth/color/motion";});
    if(p.nr_last_depth!=depth_uid) p.NewEpoch();
    PrepareParams params;params.jitter={};params.valid=p.reset?0u:1u;
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
        {.imageView=*p.images[p.fused?5:0].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[1].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[2].view,.imageLayout=vk::ImageLayout::eGeneral}};
    if(!p.dump_directory.empty())
        general.push_back({.imageView=*p.images[7].view,.imageLayout=vk::ImageLayout::eGeneral});
    cmd.pushConstants(*p.prepare_layout,vk::ShaderStageFlagBits::eCompute,0,sizeof(params),&params);
    if(p.fused && p.WantsCapture()) {
        // Diagnostic only: evaluate the independent, old prepare+encode shaders
        // on these exact scene pixels, then compare their raw input with fusion.
        auto reference=general;reference[3].imageView=*p.images[0].view;
        dispatch(*p.validate_prepare,*p.prepare_layout,reference,3);Impl::Barrier(cmd);
        p.NrConvert(cmd,true,3);
    }
    dispatch(*p.prepare,*p.prepare_layout,general,3);
    Impl::Barrier(cmd);
    if(!p.fused) {
        if(profiler) profiler->Mark(0x4e520002,[]{return "NR encode display RGB";});
        p.NrConvert(cmd,true);
    }
    if(profiler) profiler->Mark(0x4e520003,[]{return "NR NVIDIA evaluation";});
    bool ok=p.nr_created || p.dlss->CreateNrFeature(cmd,w,h);
    if(ok) p.nr_created=true;
    const auto a=SpartanNr::Unpack(p.appearance);
    if(ok) ok=p.dlss->EvaluateNrScene(cmd,{p.Resource(5),p.Resource(6),p.Resource(1),p.Resource(2),p.reset,
        {a.intensity/100.f,a.tone/100.f,a.structure/100.f,a.skin/100.f}});
    if(!ok) {
        if(profiler) profiler->Resume(previous_profile);
        p.failed=true;p.NewEpoch();p.UpdateMotionWork();auto& ui=Gow3::NeuralPreviewControl();ui.state=Gow3::NrState::Failed;ui.available=false;
        std::printf("GoW3 NR scene: evaluation failed; original scene retained (%s)\n",p.dlss->Problem().c_str());
        return {};
    }
    Impl::Barrier(cmd);
    p.NrDump(cmd);
    if(!p.fused) {
        if(profiler) profiler->Mark(0x4e520004,[]{return "NR decode linear RGB";});
        p.NrConvert(cmd,false);
    }
    if(profiler) profiler->Mark(0x4e520005,[]{return "NR expand MSAA output";});
    const vk::DescriptorImageInfo expanded[]{
        {.imageView=*p.images[p.fused?6:3].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=p.fused?color:*p.images[0].view,.imageLayout=p.fused?color_layout:vk::ImageLayout::eGeneral},
        {.imageView=*p.images[4].view,.imageLayout=vk::ImageLayout::eGeneral}};
    dispatch(*p.expand,*p.expand_layout,expanded,2);Impl::Barrier(cmd);
    if(profiler) profiler->Resume(previous_profile);
    auto& ui=Gow3::NeuralPreviewControl();ui.width=w;ui.height=h;
    const bool was_reset=p.reset;
    const auto count=++p.evaluated;
    const auto scene=p.frame;
    const auto generation=ui.generation.load();
    p.scheduler.DeferOperation([count,scene,was_reset,generation] {
        auto& live=Gow3::NeuralPreviewControl();live.Complete(generation,count);
        if(was_reset||count%120==0) std::printf("GoW3 NR scene: GPU-complete=%llu scene=%llu reset=%u depth+motion=1 pre-HUD=1\n",
            (unsigned long long)count,(unsigned long long)scene,was_reset);
    });
    p.reset=false;p.nr_last_depth=depth_uid;
    p.frame_evaluated=true;
    return *p.images[4].view;
}
}
