// SPDX-License-Identifier: GPL-2.0-or-later
#include "gow3_pixel_extract.h"
#include <array>
#include <stdexcept>
#include <string>
#include <algorithm>

namespace Vulkan::Gow3::Pixels {
namespace {
void Check(VkResult result,const char* operation) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": VkResult="+std::to_string(result));
}
}
struct Extractor::Impl {
    VkDevice device;
    VkPhysicalDeviceMemoryProperties memory;
    uint64_t range{},allocated{};
    uint32_t words=5;
    uint64_t budget=MemoryBudget;
    uint32_t slots{},inflight{};
#define PIXEL_FUNCTIONS(X) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) \
    X(BindBufferMemory) X(MapMemory) X(UnmapMemory) X(InvalidateMappedMemoryRanges) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) \
    X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateComputePipelines) X(DestroyPipeline) \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(UpdateDescriptorSets) \
    X(CmdPipelineBarrier) X(CmdBindPipeline) X(CmdBindDescriptorSets) X(CmdPushConstants) X(CmdDispatch)
#define DECLARE(name) PFN_vk##name name{};
    PIXEL_FUNCTIONS(DECLARE)
#undef DECLARE
    VkDescriptorSetLayout descriptors{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    ~Impl() {
        // Abandoned submitted work must never be freed early. Runtime instances
        // intentionally live until process exit; this is also a final RAII guard.
        if(inflight) return;
        if(pool) DestroyDescriptorPool(device,pool,nullptr);
        if(pipeline) DestroyPipeline(device,pipeline,nullptr);
        if(layout) DestroyPipelineLayout(device,layout,nullptr);
        if(descriptors) DestroyDescriptorSetLayout(device,descriptors,nullptr);
    }
};
Extractor::Extractor(VkDevice device,PFN_vkGetDeviceProcAddr get,
                     const VkPhysicalDeviceMemoryProperties& memory,
                     uint64_t max_storage_range,std::span<const uint32_t> spirv,uint32_t words)
    :impl(std::make_unique<Impl>()) {
    auto& p=*impl;p.device=device;p.memory=memory;p.range=max_storage_range;
    if(words!=5 && words!=8) throw std::runtime_error("Unsupported pixel record stride");
    p.words=words; p.budget=MemoryBudget;
#define LOAD(name) p.name=reinterpret_cast<PFN_vk##name>(get(device,"vk" #name)); if(!p.name) throw std::runtime_error("Missing vk" #name);
    PIXEL_FUNCTIONS(LOAD)
#undef LOAD
    if(spirv.empty() || spirv.size_bytes()>1024*1024) throw std::runtime_error("Invalid pixel extraction shader");
    std::array<VkDescriptorSetLayoutBinding,4> bindings{};
    for(uint32_t i=0;i<4;++i) bindings[i]={i,i==2?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=4;dl.pBindings=bindings.data();
    Check(p.CreateDescriptorSetLayout(device,&dl,nullptr,&p.descriptors),"pixel descriptors");
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,8};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&p.descriptors;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&push;
    Check(p.CreatePipelineLayout(device,&pl,nullptr,&p.layout),"pixel pipeline layout");
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=spirv.size_bytes();sm.pCode=spirv.data();VkShaderModule module{};
    Check(p.CreateShaderModule(device,&sm,nullptr,&module),"pixel shader");
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.layout=p.layout;cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
    const auto result=p.CreateComputePipelines(device,VK_NULL_HANDLE,1,&cp,nullptr,&p.pipeline);
    p.DestroyShaderModule(device,module,nullptr);Check(result,"pixel pipeline");
    const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,6},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,2}}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=2;pool.poolSizeCount=2;pool.pPoolSizes=sizes.data();
    Check(p.CreateDescriptorPool(device,&pool,nullptr,&p.pool),"pixel descriptor pool");
}
Extractor::~Extractor()=default;
std::unique_ptr<Extractor::Output> Extractor::Allocate(uint32_t width,uint32_t height) {
    auto& p=*impl;const auto bytes=ByteSize(width,height)/5*p.words;
    if(!bytes || bytes>p.range || p.slots==2) throw std::runtime_error("Pixel dimensions/range/two-frame limit");
    auto out=std::unique_ptr<Output>(new Output(&p));out->width=width;out->height=height;out->bytes=bytes;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=bytes;bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;bi.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    Check(p.CreateBuffer(p.device,&bi,nullptr,&out->buffer),"pixel output buffer");
    VkMemoryRequirements requirement{};p.GetBufferMemoryRequirements(p.device,out->buffer,&requirement);
    if(requirement.size>p.budget || p.allocated>p.budget-requirement.size) throw std::runtime_error("Pixel allocation budget");
    uint32_t selected=~0u;int score=-1;
    for(uint32_t i=0;i<p.memory.memoryTypeCount;++i) {
        const auto flags=p.memory.memoryTypes[i].propertyFlags;
        if(!(requirement.memoryTypeBits&(1u<<i)) || !(flags&VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        const int candidate=((flags&VK_MEMORY_PROPERTY_HOST_CACHED_BIT)?2:0)+((flags&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)?1:0);
        if(candidate>score){score=candidate;selected=i;}
    }
    if(selected==~0u) throw std::runtime_error("No host-visible pixel output memory");
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=requirement.size;allocation.memoryTypeIndex=selected;
    Check(p.AllocateMemory(p.device,&allocation,nullptr,&out->memory),"pixel output memory");
    out->allocation_bytes=requirement.size;p.allocated+=requirement.size;
    Check(p.BindBufferMemory(p.device,out->buffer,out->memory,0),"bind pixel output");
    Check(p.MapMemory(p.device,out->memory,0,VK_WHOLE_SIZE,0,&out->mapped),"map pixel output");
    VkDescriptorSetAllocateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};set.descriptorPool=p.pool;set.descriptorSetCount=1;set.pSetLayouts=&p.descriptors;
    Check(p.AllocateDescriptorSets(p.device,&set,&out->descriptor),"pixel descriptor set");++p.slots;
    return out;
}
Extractor::Output::~Output() {
    if(recorded && !completed) return; // Intentional bounded abandonment, not an unsafe early free.
    if(mapped) owner->UnmapMemory(owner->device,memory);
    if(buffer) owner->DestroyBuffer(owner->device,buffer,nullptr);
    if(memory) {owner->FreeMemory(owner->device,memory,nullptr);owner->allocated-=allocation_bytes;}
}
void Extractor::Record(VkCommandBuffer command,Output& out,Source depth,Source color,Source motion) {
    auto& p=*impl;
    if(out.recorded || !depth.view || !color.view) throw std::runtime_error("Pixel output reused or null source");
    const std::array<VkDescriptorImageInfo,2> images{{{VK_NULL_HANDLE,depth.view,depth.layout},{VK_NULL_HANDLE,color.view,color.layout}}};
    const VkDescriptorBufferInfo buffer{out.buffer,0,out.bytes};std::array<VkWriteDescriptorSet,3> writes{};
    for(uint32_t i=0;i<3;++i){auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=out.descriptor;w.dstBinding=i;w.descriptorCount=1;w.descriptorType=i==2?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;if(i==2)w.pBufferInfo=&buffer;else w.pImageInfo=&images[i];}
    p.UpdateDescriptorSets(p.device,3,writes.data(),0,nullptr);
    if(p.words==8) {
        if(!motion.view) throw std::runtime_error("Missing diagnostic motion view");
        const VkDescriptorImageInfo info{VK_NULL_HANDLE,motion.view,motion.layout};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};w.dstSet=out.descriptor;w.dstBinding=3;
        w.descriptorCount=1;w.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;w.pImageInfo=&info;
        p.UpdateDescriptorSets(p.device,1,&w,0,nullptr);
    }
    const VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_MEMORY_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT};
    p.CmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&before,0,nullptr,0,nullptr);
    p.CmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,p.pipeline);
    p.CmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,p.layout,0,1,&out.descriptor,0,nullptr);
    const std::array<uint32_t,2> size{out.width,out.height};p.CmdPushConstants(command,p.layout,VK_SHADER_STAGE_COMPUTE_BIT,0,8,size.data());
    p.CmdDispatch(command,(out.width+7)/8,(out.height+7)/8,1);
    // Sources are read-only. Order every later read/write after extraction, so
    // following cache transitions need no synthetic tracker layout restoration.
    const VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER,nullptr,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT};
    p.CmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&after,0,nullptr,0,nullptr);
    VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;host.srcQueueFamilyIndex=host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.buffer=out.buffer;host.size=VK_WHOLE_SIZE;
    p.CmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,0,nullptr,1,&host,0,nullptr);
    out.recorded=true;++p.inflight;
}
void Extractor::MarkCompleted(Output& out) {
    if(!out.recorded || out.completed) throw std::runtime_error("Invalid pixel completion");
    out.completed=true;--impl->inflight;
}
std::span<const std::byte> Extractor::ReadCompleted(Output& out) {
    if(!out.completed) throw std::runtime_error("Pixel read before GPU completion");
    VkMappedMemoryRange memory{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};memory.memory=out.memory;memory.size=VK_WHOLE_SIZE;
    Check(impl->InvalidateMappedMemoryRanges(impl->device,1,&memory),"invalidate pixel output");
    return {static_cast<const std::byte*>(out.mapped),size_t(out.bytes)};
}
uint64_t Extractor::Allocated() const {return impl->allocated;}
#undef PIXEL_FUNCTIONS
} // namespace Vulkan::Gow3::Pixels
