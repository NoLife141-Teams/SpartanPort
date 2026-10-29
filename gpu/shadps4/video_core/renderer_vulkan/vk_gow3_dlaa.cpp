// SPDX-License-Identifier: GPL-2.0-or-later
#include "vk_gow3_dlaa.h"
#include "gow3_motion.h"
#include "gow3_dlaa_control.h"
#include "gow3_nr_control.h"
#include "vk_instance.h"
#include "vk_scheduler.h"
#include "vk_dlss.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/gow3_dlaa_prepare_comp.h"
#include "video_core/host_shaders/gow3_dlaa_expand_comp.h"
#include "video_core/host_shaders/gow3_dlaa_prepare_single_comp.h"
#include "video_core/host_shaders/gow3_dlaa_expand_single_comp.h"
#include "video_core/amdgpu/spartanport_msaa.h"
#include "video_core/host_shaders/gow3_dlaa_sharpen_comp.h"
#include "video_core/host_shaders/gow3_dlaa_preview_comp.h"
#include "video_core/host_shaders/gow3_nr_prepare_comp.h"
#include "video_core/host_shaders/gow3_nr_prepare_msaa_comp.h"
#include "video_core/host_shaders/gow3_nr_encode_comp.h"
#include "video_core/host_shaders/gow3_nr_decode_comp.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <chrono>

namespace Vulkan {
namespace {
bool SupportedSize(u32 w,u32 h) { return (w==1920&&h==1080)||(w==2560&&h==1440)||(w==3840&&h==2160); }
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
    bool nr=Gow3::NeuralSceneRequested(),nr_created{};
    u32 nr_captures{};
    u64 nr_last_depth{};
    std::filesystem::path control;
    std::filesystem::path dump_directory;
    std::unique_ptr<VideoCore::Buffer> readback;
    bool dump_issued{};
    u64 compare_scene{};
    bool compare_issued{};
    u32 sharpness_percent{},sequence_count{};
    u64 sequence_start{};
    bool sequence_started{};
    std::chrono::steady_clock::time_point last_evaluation{};
    std::array<float,2> jitter{};
    struct Owned {
        VideoCore::UniqueImage image;
        vk::UniqueImageView view;
        vk::Format format{};
    };
    // Resolved color, nearest sample depth, motion in pixels, NGX output,
    // the matching-sample replacement consumed by the guest composition, the separate
    // sharpened presentation copy and a diagnostic-only low-strength snapshot.
    std::vector<Owned> images;
    vk::UniqueDescriptorSetLayout prepare_desc,expand_desc,sharpen_desc,preview_desc;
    vk::UniquePipelineLayout prepare_layout,expand_layout,sharpen_layout,preview_layout;
    vk::UniquePipeline prepare,expand,sharpen,preview;
    vk::UniqueDescriptorSetLayout nr_desc;
    vk::UniquePipelineLayout nr_layout;
    vk::UniquePipeline nr_encode,nr_decode;

    Impl(const Instance& i,Scheduler& s):instance(i),scheduler(s) {}
    void Phase() {
        if(nr) {jitter={};return;}
        const auto n=frame%8+1; jitter={Halton(n,2)-.5f,Halton(n,3)-.5f};
    }
    void Poll() {
        if(nr) {
            auto& ui=Gow3::NeuralPreviewControl();
            if(ui.toggles.exchange(0)&1) {
                active=!active;reset=true;
                ui.state=active?Gow3::NrState::Waiting:Gow3::NrState::Off;
                std::printf("GoW3 NR scene: requested %s scene=%llu\n",active?"ON":"OFF",(unsigned long long)frame);
            }
            return;
        }
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
        const auto amount=std::min(ui.sharpness_percent.load(),100u);
        if(amount!=sharpness_percent) {
            sharpness_percent=amount;
            std::printf("GoW3 DLAA: sharpness=%u%% scene=%llu history-retained\n",amount,(unsigned long long)frame);
        }
    }
    void Create() {
        const auto device=instance.GetDevice();
        constexpr std::array formats{vk::Format::eR16G16B16A16Sfloat,vk::Format::eR32Sfloat,
            vk::Format::eR16G16Sfloat,vk::Format::eR16G16B16A16Sfloat,vk::Format::eR16G16B16A16Sfloat,
            vk::Format::eR16G16B16A16Sfloat,vk::Format::eR16G16B16A16Sfloat,
            vk::Format::eR32G32B32A32Sfloat,vk::Format::eR32G32B32A32Sfloat,vk::Format::eR32G32B32A32Sfloat};
        images.resize(nr?10:7);
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
        prepare_layout=layout(3,nr?4:3,prepare_desc,sizeof(PrepareParams));
        expand_layout=layout(2,1,expand_desc,0);
        sharpen_layout=layout(0,2,sharpen_desc,sizeof(float));
        preview_layout=layout(0,4,preview_desc,0,true);
        const auto pipeline=[&](std::span<const u32> spirv,vk::PipelineLayout l) {
            auto module=Check(device.createShaderModuleUnique({.codeSize=spirv.size_bytes(),.pCode=spirv.data()}));
            return Check(device.createComputePipelineUnique({}, {.stage={.stage=vk::ShaderStageFlagBits::eCompute,
                .module=*module,.pName="main"},.layout=l}));
        };
        prepare=pipeline(nr?(AmdGpu::NoMsaa()?std::span<const u32>(GOW3_NR_PREPARE_COMP):std::span<const u32>(GOW3_NR_PREPARE_MSAA_COMP)):
            AmdGpu::NoMsaa()?std::span<const u32>(GOW3_DLAA_PREPARE_SINGLE_COMP):std::span<const u32>(GOW3_DLAA_PREPARE_COMP),*prepare_layout);
        expand=pipeline(AmdGpu::NoMsaa()?std::span<const u32>(GOW3_DLAA_EXPAND_SINGLE_COMP):std::span<const u32>(GOW3_DLAA_EXPAND_COMP),*expand_layout);
        sharpen=pipeline(GOW3_DLAA_SHARPEN_COMP,*sharpen_layout);
        preview=pipeline(GOW3_DLAA_PREVIEW_COMP,*preview_layout);
        if(nr) {
            nr_layout=layout(1,1,nr_desc,0);
            nr_encode=pipeline(GOW3_NR_ENCODE_COMP,*nr_layout);
            nr_decode=pipeline(GOW3_NR_DECODE_COMP,*nr_layout);
        }
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
            {.imageView=*images[encode?0:8].view,.imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=*images[encode?7:3].view,.imageLayout=vk::ImageLayout::eGeneral}};
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
        constexpr std::array<u32,5> ids{7,8,1,2,9},bpp{16,16,4,4,16};
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
    void Sharpen(vk::CommandBuffer cmd,float amount) {
        const vk::DescriptorImageInfo info[]{
            {.imageView=*images[3].view,.imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=*images[5].view,.imageLayout=vk::ImageLayout::eGeneral}};
        std::array<vk::WriteDescriptorSet,2> writes{};
        for(u32 n=0;n<2;++n) writes[n]={.dstBinding=n,.descriptorCount=1,
            .descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&info[n]};
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*sharpen);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*sharpen_layout,0,writes);
        cmd.pushConstants(*sharpen_layout,vk::ShaderStageFlagBits::eCompute,0,sizeof(amount),&amount);
        cmd.dispatch((width+7)/8,(height+7)/8,1);Barrier(cmd);
    }
    void Sequence(vk::CommandBuffer cmd) {
        if(dump_directory.empty()||evaluated<32||sequence_count==16) return;
        const auto directory=dump_directory/"sequence";
        if(!sequence_started) {
            if(!std::filesystem::is_regular_file(dump_directory/"sequence.trigger")) return;
            sequence_started=true;sequence_start=frame;
            std::error_code error;
            if(!std::filesystem::create_directory(directory,error)) {sequence_count=16;return;}
        }
        if(frame<sequence_start+sequence_count*8) return;
        Sharpen(cmd,.2f);
        const vk::ImageCopy region{.srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
            .dstSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.extent={width,height,1}};
        cmd.copyImage(vk::Image(images[5].image),vk::ImageLayout::eGeneral,
            vk::Image(images[6].image),vk::ImageLayout::eGeneral,region);Barrier(cmd);
        Sharpen(cmd,.4f);
        const u32 w=width/2,h=height/2;
        const u64 bytes=u64(w)*h*4;
        auto buffer=std::make_shared<VideoCore::Buffer>(instance,0,bytes*3,VideoCore::MemoryType::HostCached,"GoW3 sharpness sequence");
        const vk::DescriptorImageInfo info[]{
            {.imageView=*images[3].view,.imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=*images[6].view,.imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=*images[5].view,.imageLayout=vk::ImageLayout::eGeneral}};
        const vk::DescriptorBufferInfo output{buffer->Handle(),0,bytes*3};
        std::array<vk::WriteDescriptorSet,4> writes{};
        for(u32 n=0;n<3;++n) writes[n]={.dstBinding=n,.descriptorCount=1,
            .descriptorType=vk::DescriptorType::eStorageImage,.pImageInfo=&info[n]};
        writes[3]={.dstBinding=3,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&output};
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute,*preview);
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute,*preview_layout,0,writes);
        cmd.dispatch((w+7)/8,(h+7)/8,1);
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eComputeShader,
            .srcAccessMask=vk::AccessFlagBits2::eShaderWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,.dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host});
        const auto tick=scheduler.CurrentTick(),scene=frame;
        const auto index=sequence_count++;
        scheduler.DeferOperation([buffer,directory,bytes,w,h,tick,scene,index] {
            buffer->Invalidate(0,bytes*3);bool ok=true;
            for(u32 n=0;n<3;++n) {
                const auto stem=std::to_string(index)+"_"+std::to_string(n);
                auto* f=std::fopen((directory/(stem+".rgba8")).string().c_str(),"wbx");
                if(!f){ok=false;continue;}
                const bool written=std::fwrite(buffer->mapped_data.data()+n*bytes,1,bytes,f)==bytes;
                const bool closed=std::fclose(f)==0;ok &= written&&closed;
            }
            auto* f=std::fopen((directory/(std::to_string(index)+".json")).string().c_str(),"wbx");
            if(f) {std::fprintf(f,"{\"gpu_complete\":true,\"complete\":%s,\"tick\":%llu,\"scene\":%llu,\"width\":%u,\"height\":%u,\"sharpness\":[0,20,40]}\n",
                ok?"true":"false",(unsigned long long)tick,(unsigned long long)scene,w,h);std::fclose(f);}
            std::printf("GoW3 DLAA: sequence=%u scene=%llu gpu-complete=%u\n",index,(unsigned long long)scene,ok);
        });
        Barrier(cmd);
    }
    // Read back the same composition boundary in each isolated AA session. The
    // off paths copy their prepared input without replacing the guest binding.
    void Compare(vk::CommandBuffer cmd,bool filtered,const std::array<float,2>& phase) {
        if(!compare_scene||compare_issued||frame<compare_scene||dump_directory.empty()) return;
        compare_issued=true;
        Gow3::NativeDlaaControl().comparison_ready=true;
        const u64 bytes=u64(width)*height*8;
        auto buffer=std::make_shared<VideoCore::Buffer>(instance,0,bytes*2,VideoCore::MemoryType::HostCached,"GoW3 AA comparison");
        for(u32 n=0;n<2;++n) {
            const vk::BufferImageCopy region{.bufferOffset=n*bytes,
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={width,height,1}};
            cmd.copyImageToBuffer(vk::Image(images[n&&filtered?3:0].image),vk::ImageLayout::eGeneral,buffer->Handle(),region);
        }
        const vk::MemoryBarrier2 host{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&host});
        const auto tick=scheduler.CurrentTick(),scene=frame,count=evaluated+(filtered?1:0);
        scheduler.DeferOperation([buffer,directory=dump_directory,bytes,w=width,h=height,tick,scene,count,
            filtered,phase,vp=camera.current,samples=AmdGpu::NoMsaa()?1:2] {
            buffer->Invalidate(0,bytes*2);bool ok=true;
            for(u32 n=0;n<2;++n) {
                auto* f=std::fopen((directory/(n?"compare-output.rgba16f":"compare-input.rgba16f")).string().c_str(),"wbx");
                if(!f){ok=false;continue;}
                const bool written=std::fwrite(buffer->mapped_data.data()+n*bytes,1,bytes,f)==bytes;
                const bool closed=std::fclose(f)==0;ok &= written&&closed;
            }
            auto* f=std::fopen((directory/"compare.json").string().c_str(),"wbx");
            if(f) {
                std::fprintf(f,"{\"complete\":%s,\"gpu_complete\":true,\"scene\":%llu,\"tick\":%llu,\"evaluated\":%llu,\"width\":%u,\"height\":%u,\"samples\":%u,\"dlaa\":%s,\"sharpness\":0,\"jitter\":[%.9g,%.9g],\"camera_vp\":[",
                    ok?"true":"false",(unsigned long long)scene,(unsigned long long)tick,(unsigned long long)count,w,h,samples,filtered?"true":"false",filtered?phase[0]:0.f,filtered?phase[1]:0.f);
                for(u32 n=0;n<16;++n) std::fprintf(f,"%s%.17g",n?",":"",vp[n]);
                std::fputs("]}\n",f);std::fclose(f);
            }
            std::printf("GoW3 AA: GPU comparison complete=%u samples=%u dlaa=%u scene=%llu\n",ok,samples,filtered,(unsigned long long)scene);
        });
        Barrier(cmd);
    }
    void Dump(vk::CommandBuffer cmd) {
        if(dump_directory.empty() || dump_issued || evaluated<32 ||
           !std::filesystem::is_regular_file(dump_directory/"capture.trigger")) return;
        dump_issued=true;
        const u64 bytes=u64(width)*height*8;
        readback=std::make_unique<VideoCore::Buffer>(instance,0,bytes*4,VideoCore::MemoryType::HostCached,"GoW3 DLAA proof");
        for(u32 n=0;n<4;++n) {
            if(n>=2) Sharpen(cmd,n==2?.2f:.4f);
            const vk::BufferImageCopy region{.bufferOffset=n*bytes,
                .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},.imageExtent={width,height,1}};
            cmd.copyImageToBuffer(vk::Image(images[n>=2?5:n?3:0].image),vk::ImageLayout::eGeneral,readback->Handle(),region);
            Barrier(cmd);
        }
        const vk::MemoryBarrier2 b{.srcStageMask=vk::PipelineStageFlagBits2::eTransfer,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,.dstStageMask=vk::PipelineStageFlagBits2::eHost,
            .dstAccessMask=vk::AccessFlagBits2::eHostRead};
        cmd.pipelineBarrier2({.memoryBarrierCount=1,.pMemoryBarriers=&b});
        const auto tick=scheduler.CurrentTick(),scene=frame,count=evaluated+1;
        scheduler.DeferOperation([this,tick,scene,count,bytes,width=width,height=height] {
            readback->Invalidate(0,bytes*4);
            bool ok=true;
            const char* names[]{"input.rgba16f","output.rgba16f","low.rgba16f","medium.rgba16f"};
            for(u32 n=0;n<4;++n) {
                auto* f=std::fopen((dump_directory/names[n]).string().c_str(),"wbx");
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
    if(impl->nr) {
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
        impl->enabled=true;impl->active=false;impl->Phase();ui.guided=true;
        std::puts("GoW3 NR scene: verified pre-HUD scene, real depth/object+camera motion, unjittered; F7 toggles");
        return;
    }
    if(!Gow3::NativeDlaaRequested()) return;
    auto& ui=Gow3::NativeDlaaControl();ui.visible=true;
    impl->dlss=Dlss::Get();
    if(!impl->dlss || !impl->dlss->Available() || (!AmdGpu::NoMsaa()&&!i.GetPhysicalDevice().getFeatures().shaderStorageImageMultisample)) {
        ui.state=Gow3::DlaaState::Unavailable;
        std::puts("GoW3 DLAA: unavailable (NGX or multisample storage missing)"); return;
    }
    if(const char* value=std::getenv("SPARTAN_GOW3_DLAA_SWITCH")) impl->control=value;
    if(const char* value=std::getenv("SPARTAN_GOW3_AA_CAPTURE_SCENE")) impl->compare_scene=std::strtoull(value,nullptr,10);
    if(const char* value=std::getenv("SPARTAN_GOW3_DLAA_DUMP")) {
        const std::filesystem::path path(value);
        if(path.is_absolute()&&std::filesystem::is_directory(path)&&std::filesystem::is_empty(path)) impl->dump_directory=path;
    }
    impl->active=true;impl->enabled=true;ui.available=true;impl->Poll();impl->Phase();
    ui.state=impl->active?Gow3::DlaaState::Waiting:Gow3::DlaaState::Off;
    std::puts("GoW3 DLAA: native 1080p/1440p/4K composition hook ready, F10 toggles, 8 jitter phases");
}
Gow3Dlaa::~Gow3Dlaa() {
    if(impl->initialized) {
        impl->scheduler.Finish();
        if(impl->nr) {
            impl->dlss->ReleaseNrFeature();
            impl->dlss->Shutdown(); // before the owning Vulkan device is destroyed
        }
        else impl->dlss->ReleaseFeature();
    }
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
        if(p.active&&!p.failed) {
            if(p.nr) Gow3::NeuralPreviewControl().state=Gow3::NrState::Waiting;
            else Gow3::NativeDlaaControl().state=Gow3::DlaaState::Waiting;
        }}
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
    if(p.nr) p.Poll();
    const bool active=Active();
    ++p.frame;p.Phase();
    if(!p.nr) p.Poll();
    const bool comparison=p.compare_scene&&!p.compare_issued&&p.frame>=p.compare_scene;
    if((!active&&!comparison) || !depth || !color || !motion || depth_uid!=p.depth || !SupportedSize(w,h) ||
       (p.initialized && (w!=p.width || h!=p.height)) || (p.nr&&(w!=1920||h!=1080)) ||
       status!=Gow3::Status::Candidate) {
        if(p.nr) {
            Gow3::NeuralPreviewControl().state=p.failed?Gow3::NrState::Failed:
                p.active?Gow3::NrState::Waiting:Gow3::NrState::Off;
        } else Gow3::NativeDlaaControl().state=p.failed?Gow3::DlaaState::Failed:
                p.active?Gow3::DlaaState::Waiting:Gow3::DlaaState::Off;
        p.reset=true;++p.bypassed;
        if(p.bypassed%120==1) std::printf("GoW3 %s: bypass scene=%llu camera=%s sources=%u active=%u size=%ux%u\n",
            p.nr?"NR scene":"DLAA",(unsigned long long)p.frame,Gow3::Name(status),bool(depth&&color&&motion),active,w,h);
        return {};
    }
    bool first=!p.initialized;
    if(first) {p.width=w;p.height=h;p.Create();}
    // Synchronize the recorder before calling NGX; source cache lock is released.
    // NGX changes compute bindings, followed by explicit bindings for expansion.
    // Sampling an sRGB view decoded the input to linear RGB. NGX requires its
    // HDR processing flag for linear input, including an SDR display pipeline.
    if(active && (p.nr?!p.nr_created:!p.dlss->HasFeature({w,h,w,h,0,true}))) p.scheduler.WaitSubmitted();
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
    PrepareParams params;params.jitter=active?jitter:std::array<float,2>{};params.valid=1;
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
    if(p.nr) general.push_back({.imageView=*p.images[9].view,.imageLayout=vk::ImageLayout::eGeneral});
    cmd.pushConstants(*p.prepare_layout,vk::ShaderStageFlagBits::eCompute,0,sizeof(params),&params);
    dispatch(*p.prepare,*p.prepare_layout,general,3);
    Impl::Barrier(cmd);
    if(!active) { p.Compare(cmd,false,{});return {}; }
    if(p.nr) {
        if(p.nr_last_depth!=depth_uid) p.reset=true;
        p.NrConvert(cmd,true);
        bool ok=p.nr_created || p.dlss->CreateNrFeature(cmd,w,h);
        if(ok) p.nr_created=true;
        if(ok) ok=p.dlss->EvaluateNrScene(cmd,{p.Resource(7),p.Resource(8),p.Resource(1),p.Resource(2),p.reset});
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
    const Dlss::FeatureDesc desc{w,h,w,h,0,true};
    bool ok=p.dlss->HasFeature(desc)||p.dlss->CreateFeature(cmd,desc);
    const auto now=std::chrono::steady_clock::now();
    const float frame_ms=p.last_evaluation.time_since_epoch().count()?std::clamp(float(std::chrono::duration<double,std::milli>(now-p.last_evaluation).count()),1.f,250.f):1000.f/60.f;
    p.last_evaluation=now;
    if(ok) ok=p.dlss->Evaluate(cmd,{.color=p.Resource(0),.depth=p.Resource(1),.motion=p.Resource(2),.output=p.Resource(3),
        // This is the physical viewport translation, in top-left pixel space.
        // NGX needs that same signed translation (not the inverse sampling
        // location). A frozen real-input replay and analytic X/Y waves show
        // that negating it attenuates detail and leaves a subpixel phase error.
        .jitter_x=jitter[0],.jitter_y=jitter[1],.reset=p.reset,.frame_ms=frame_ms,.sharpness=0});
    if(!ok) { p.failed=true;p.reset=true;Gow3::NativeDlaaControl().state=Gow3::DlaaState::Failed;
        std::puts("GoW3 DLAA: NGX failed; native input retained");return {}; }
    Impl::Barrier(cmd);
    p.Compare(cmd,true,jitter);
    p.Dump(cmd);
    p.Sequence(cmd);
    Impl::Barrier(cmd);
    // Sharpen only the presentation copy. NGX history and game HUD are untouched.
    const bool sharpened=p.sharpness_percent!=0;
    if(sharpened) p.Sharpen(cmd,float(p.sharpness_percent)/100.f);
    const vk::DescriptorImageInfo expanded[]{
        {.imageView=*p.images[sharpened?5:3].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[0].view,.imageLayout=vk::ImageLayout::eGeneral},
        {.imageView=*p.images[4].view,.imageLayout=vk::ImageLayout::eGeneral}};
    dispatch(*p.expand,*p.expand_layout,expanded,2);
    Impl::Barrier(cmd);
    if(p.reset || p.evaluated%120==0) std::printf("GoW3 DLAA: evaluated=%llu scene=%llu quality=DLAA size=%ux%u reset=%u jitter=%.6f,%.6f\n",
        (unsigned long long)(p.evaluated+1),(unsigned long long)p.frame,w,h,p.reset,jitter[0],jitter[1]);
    auto& ui=Gow3::NativeDlaaControl();ui.width=w;ui.height=h;ui.state=Gow3::DlaaState::Active;
    ++p.evaluated;p.reset=false;
    return *p.images[4].view;
}
}
