// SPDX-License-Identifier: GPL-2.0-or-later
// Standalone synthetic offscreen Vulkan test: no project runtime, game or NGX.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "gow3_projection_fixture.h"
using namespace ProjectionFixture;
namespace {
constexpr uint32_t W=64,H=48;
constexpr VkFormat DepthFormat=VK_FORMAT_D32_SFLOAT_S8_UINT;
constexpr VkFormat ColorFormat=VK_FORMAT_R32G32B32A32_SFLOAT;
enum class DepthPath { ZeroToW, MinusFallback, MinusNative };
struct Mode {
    DepthPath path=DepthPath::ZeroToW;
    bool negative=true;
    float low=0,high=1;
    bool Minus() const { return path!=DepthPath::ZeroToW; }
    const char* Name() const { return path==DepthPath::ZeroToW?"zero-to-w":
        path==DepthPath::MinusFallback?"minus-w-shader-fallback":"minus-w-native-ext"; }
};
std::atomic<unsigned> validation_errors{},validation_warnings{};
void Check(VkResult result,const char* action) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(action)+" VkResult="+std::to_string(result));
}
void Require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
VKAPI_ATTR VkBool32 VKAPI_CALL Debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void*) {
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++validation_errors;
    if(severity&VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ++validation_warnings;
    std::fprintf(stderr,"Vulkan validation: %s\n",data->pMessage); return VK_FALSE;
}
struct Context {
    VkInstance instance{}; VkDebugUtilsMessengerEXT debug{};
    VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    uint32_t family{}; VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceFeatures features{}; VkPhysicalDeviceMemoryProperties memory{};
    VkCommandPool pool{}; bool depth_clip_control{};
    ~Context() {
        if(device) { if(pool) vkDestroyCommandPool(device,pool,nullptr); vkDestroyDevice(device,nullptr); }
        if(debug) { const auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT")); if(destroy) destroy(instance,debug,nullptr); }
        if(instance) vkDestroyInstance(instance,nullptr);
    }
    uint32_t Memory(uint32_t bits,VkMemoryPropertyFlags flags) const {
        for(uint32_t i=0;i<memory.memoryTypeCount;++i)
            if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) return i;
        throw std::runtime_error("required memory type unavailable");
    }
    void Initialize() {
        uint32_t count=0; Check(vkEnumerateInstanceLayerProperties(&count,nullptr),"layers");
        std::vector<VkLayerProperties> layers(count); Check(vkEnumerateInstanceLayerProperties(&count,layers.data()),"layers");
        const bool validation=std::any_of(layers.begin(),layers.end(),[](const auto& l){return std::strcmp(l.layerName,"VK_LAYER_KHRONOS_validation")==0;});
        Check(vkEnumerateInstanceExtensionProperties(nullptr,&count,nullptr),"extensions");
        std::vector<VkExtensionProperties> extensions(count); Check(vkEnumerateInstanceExtensionProperties(nullptr,&count,extensions.data()),"extensions");
        const bool messages=std::any_of(extensions.begin(),extensions.end(),[](const auto& e){return std::strcmp(e.extensionName,VK_EXT_DEBUG_UTILS_EXTENSION_NAME)==0;});
        const char* layer="VK_LAYER_KHRONOS_validation"; const char* extension=VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName="GoW3 synthetic depth contract"; app.apiVersion=VK_API_VERSION_1_1;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&app;
        ci.enabledLayerCount=validation?1:0; ci.ppEnabledLayerNames=&layer;
        ci.enabledExtensionCount=messages?1:0; ci.ppEnabledExtensionNames=&extension;
        Check(vkCreateInstance(&ci,nullptr,&instance),"create instance");
        if(messages) {
            VkDebugUtilsMessengerCreateInfoEXT di{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            di.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            di.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            di.pfnUserCallback=Debug;
            const auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
            Require(create!=nullptr,"debug utils entry point missing"); Check(create(instance,&di,nullptr,&debug),"debug messenger");
        }
        std::printf("validation_layer=%s debug_utils=%s (no layer installation)\n",validation?"enabled":"unavailable",messages?"enabled":"unavailable");
        Check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"physical devices");
        Require(count>0,"no Vulkan physical device"); std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"physical devices");
        int best=-1;
        for(auto p:devices) {
            VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(p,&props);
            std::printf("available_device=%s vendor=0x%x type=%u\n",props.deviceName,props.vendorID,unsigned(props.deviceType));
            if(props.deviceType==VK_PHYSICAL_DEVICE_TYPE_CPU || props.apiVersion<VK_API_VERSION_1_1) continue;
            uint32_t qcount=0; vkGetPhysicalDeviceQueueFamilyProperties(p,&qcount,nullptr);
            std::vector<VkQueueFamilyProperties> queues(qcount); vkGetPhysicalDeviceQueueFamilyProperties(p,&qcount,queues.data());
            for(uint32_t i=0;i<qcount;++i) if((queues[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) {
                const int score=(props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU?10:0)+(props.vendorID==0x10de?1:0);
                if(score>best) { best=score; physical=p; family=i; properties=props; } break;
            }
        }
        Require(physical!=VK_NULL_HANDLE,"no hardware Vulkan 1.1 graphics+compute device (no software fallback)");
        vkGetPhysicalDeviceFeatures(physical,&features); vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        std::printf("selected=%s api=%u.%u.%u driver_raw=%u sampleRateShading=%u standardSampleLocations=%u push_limit=%u\n",properties.deviceName,VK_VERSION_MAJOR(properties.apiVersion),VK_VERSION_MINOR(properties.apiVersion),VK_VERSION_PATCH(properties.apiVersion),properties.driverVersion,features.sampleRateShading,properties.limits.standardSampleLocations,properties.limits.maxPushConstantsSize);
        Require(properties.limits.maxPushConstantsSize>=128,"128-byte push constants unavailable");
        const float priority=1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        VkPhysicalDeviceFeatures enabled{}; enabled.sampleRateShading=features.sampleRateShading;
        Check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,nullptr),"device extensions");
        std::vector<VkExtensionProperties> device_extensions(count);
        Check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,device_extensions.data()),"device extensions");
        const bool has_clip=std::any_of(device_extensions.begin(),device_extensions.end(),[](const auto& e){
            return std::strcmp(e.extensionName,VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME)==0;});
        VkPhysicalDeviceDepthClipControlFeaturesEXT clip_features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
        if(has_clip) {
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; query.pNext=&clip_features;
            vkGetPhysicalDeviceFeatures2(physical,&query);
        }
        depth_clip_control=has_clip && clip_features.depthClipControl;
        std::printf("depth_clip_control_extension=%d feature=%u native_path=%s\n",has_clip,clip_features.depthClipControl,depth_clip_control?"available":"unsupported");
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&qi; dc.pEnabledFeatures=&enabled;
        const char* clip_extension=VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME;
        if(depth_clip_control) { dc.pNext=&clip_features; dc.enabledExtensionCount=1; dc.ppEnabledExtensionNames=&clip_extension; }
        Check(vkCreateDevice(physical,&dc,nullptr,&device),"device"); vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.queueFamilyIndex=family;
        Check(vkCreateCommandPool(device,&pi,nullptr,&pool),"command pool");
    }
    bool Supports(VkFormat format,VkImageUsageFlags usage,VkSampleCountFlagBits samples) const {
        VkImageFormatProperties info{};
        const auto result=vkGetPhysicalDeviceImageFormatProperties(physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,usage,0,&info);
        std::printf("format=%d usage=0x%x samples=%u query=%d supported_sample_mask=0x%x\n",int(format),usage,unsigned(samples),int(result),info.sampleCounts);
        return result==VK_SUCCESS && (info.sampleCounts&samples) && info.maxExtent.width>=W && info.maxExtent.height>=H;
    }
};
struct Attachment {
    Context& c; VkImage image{}; VkDeviceMemory memory{}; VkImageView attachment{},sampled{};
    explicit Attachment(Context& context):c(context) {}
    ~Attachment() { if(sampled) vkDestroyImageView(c.device,sampled,nullptr); if(attachment) vkDestroyImageView(c.device,attachment,nullptr); if(image) vkDestroyImage(c.device,image,nullptr); if(memory) vkFreeMemory(c.device,memory,nullptr); }
    void Create(VkFormat format,VkImageUsageFlags usage,VkSampleCountFlagBits samples,bool depth) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D; ci.format=format;
        ci.extent={W,H,1}; ci.mipLevels=1; ci.arrayLayers=1; ci.samples=samples; ci.tiling=VK_IMAGE_TILING_OPTIMAL; ci.usage=usage;
        Check(vkCreateImage(c.device,&ci,nullptr,&image),"image");
        VkMemoryRequirements mr{}; vkGetImageMemoryRequirements(c.device,image,&mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=mr.size; ai.memoryTypeIndex=c.Memory(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(c.device,&ai,nullptr,&memory),"image memory"); Check(vkBindImageMemory(c.device,image,memory,0),"bind image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=format;
        vi.subresourceRange={depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),0,1,0,1};
        Check(vkCreateImageView(c.device,&vi,nullptr,&attachment),"attachment view");
        if(depth) vi.subresourceRange.aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        Check(vkCreateImageView(c.device,&vi,nullptr,&sampled),"sampled view");
    }
};
struct Sample { std::array<float,4> depth_position_id,motion_valid_fragment_depth; };
static_assert(sizeof(Sample)==32);
struct Case {
    Context& c; Attachment depth,color;
    VkRenderPass pass{}; VkFramebuffer framebuffer{};
    VkDescriptorSetLayout descriptors{}; VkDescriptorPool descriptor_pool{};
    VkPipelineLayout graphics_layout{},compute_layout{};
    VkPipeline background{},geometry{},compute{};
    VkBuffer buffer{}; VkDeviceMemory buffer_memory{};
    VkFence fence{}; VkCommandBuffer commands{};
    std::vector<VkShaderModule> modules;
    VkSampleCountFlagBits samples; unsigned count; Mode mode;
    Case(Context& context,unsigned n,Mode m={}):c(context),depth(c),color(c),samples(n==2?VK_SAMPLE_COUNT_2_BIT:VK_SAMPLE_COUNT_1_BIT),count(n),mode(m) {}
    ~Case() {
        if(commands) vkFreeCommandBuffers(c.device,c.pool,1,&commands);
        if(fence) vkDestroyFence(c.device,fence,nullptr);
        if(compute) vkDestroyPipeline(c.device,compute,nullptr); if(geometry) vkDestroyPipeline(c.device,geometry,nullptr); if(background) vkDestroyPipeline(c.device,background,nullptr);
        if(compute_layout) vkDestroyPipelineLayout(c.device,compute_layout,nullptr); if(graphics_layout) vkDestroyPipelineLayout(c.device,graphics_layout,nullptr);
        if(descriptor_pool) vkDestroyDescriptorPool(c.device,descriptor_pool,nullptr); if(descriptors) vkDestroyDescriptorSetLayout(c.device,descriptors,nullptr);
        if(framebuffer) vkDestroyFramebuffer(c.device,framebuffer,nullptr); if(pass) vkDestroyRenderPass(c.device,pass,nullptr);
        for(auto module:modules) vkDestroyShaderModule(c.device,module,nullptr);
        if(buffer) vkDestroyBuffer(c.device,buffer,nullptr); if(buffer_memory) vkFreeMemory(c.device,buffer_memory,nullptr);
    }
    VkShaderModule Shader(const std::filesystem::path& path) {
        std::ifstream input(path,std::ios::binary|std::ios::ate); Require(bool(input),"shader binary unavailable");
        const auto bytes=input.tellg(); Require(bytes>0 && bytes%4==0,"invalid shader size");
        std::vector<uint32_t> code(static_cast<size_t>(bytes)/4); input.seekg(0); input.read(reinterpret_cast<char*>(code.data()),bytes); Require(bool(input),"shader read failed");
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize=static_cast<size_t>(bytes); ci.pCode=code.data();
        VkShaderModule module{}; Check(vkCreateShaderModule(c.device,&ci,nullptr,&module),"shader module"); modules.push_back(module); return module;
    }
    void Create(const std::filesystem::path& shaders) {
        depth.Create(DepthFormat,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,samples,true);
        color.Create(ColorFormat,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,samples,false);
        std::array<VkAttachmentDescription,2> attachments{};
        attachments[0]={0,ColorFormat,samples,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_DONT_CARE,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        attachments[1]={0,DepthFormat,samples,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_STORE,VK_ATTACHMENT_LOAD_OP_CLEAR,VK_ATTACHMENT_STORE_OP_DONT_CARE,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        const VkAttachmentReference cr{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},dr{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount=1; sub.pColorAttachments=&cr; sub.pDepthStencilAttachment=&dr;
        VkSubpassDependency dep{}; dep.srcSubpass=0; dep.dstSubpass=VK_SUBPASS_EXTERNAL;
        dep.srcStageMask=VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT; dep.dstStageMask=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dep.srcAccessMask=VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; dep.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; rp.attachmentCount=2; rp.pAttachments=attachments.data(); rp.subpassCount=1; rp.pSubpasses=&sub; rp.dependencyCount=1; rp.pDependencies=&dep;
        Check(vkCreateRenderPass(c.device,&rp,nullptr,&pass),"render pass");
        const std::array views{color.attachment,depth.attachment};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass=pass; fi.attachmentCount=2; fi.pAttachments=views.data(); fi.width=W; fi.height=H; fi.layers=1;
        Check(vkCreateFramebuffer(c.device,&fi,nullptr,&framebuffer),"framebuffer");
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT,0,128};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; li.pushConstantRangeCount=1; li.pPushConstantRanges=&push;
        Check(vkCreatePipelineLayout(c.device,&li,nullptr,&graphics_layout),"graphics layout");
        const auto vert=Shader(shaders/"fixture.vert.spv"),frag=Shader(shaders/("fixture"+std::to_string(count)+".frag.spv"));
        std::array<VkPipelineShaderStageCreateInfo,2> stages{};
        stages[0]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vert,"main",nullptr};
        stages[1]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,frag,"main",nullptr};
        const VkBool32 convert=mode.path==DepthPath::MinusFallback;
        const VkSpecializationMapEntry vertex_entry{0,0,sizeof(convert)};
        const VkSpecializationInfo vertex_spec{1,&vertex_entry,sizeof(convert),&convert};
        stages[0].pSpecializationInfo=&vertex_spec;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        const VkViewport viewport{0,mode.negative?float(H):0,float(W),mode.negative?-float(H):float(H),mode.low,mode.high}; const VkRect2D scissor{{0,0},{W,H}};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount=1; vp.pViewports=&viewport; vp.scissorCount=1; vp.pScissors=&scissor;
        VkPipelineViewportDepthClipControlCreateInfoEXT clip_control{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
        if(mode.path==DepthPath::MinusNative) {
            Require(c.depth_clip_control,"native MinusW requested without supported feature");
            clip_control.negativeOneToOne=VK_TRUE; vp.pNext=&clip_control;
        }
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_NONE; rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=samples; ms.sampleShadingEnable=count==2; ms.minSampleShading=1;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthCompareOp=VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState ca{}; ca.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; cb.attachmentCount=1; cb.pAttachments=&ca;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pi.stageCount=2; pi.pStages=stages.data(); pi.pVertexInputState=&vi; pi.pInputAssemblyState=&ia; pi.pViewportState=&vp; pi.pRasterizationState=&rs; pi.pMultisampleState=&ms; pi.pDepthStencilState=&ds; pi.pColorBlendState=&cb; pi.layout=graphics_layout; pi.renderPass=pass;
        Check(vkCreateGraphicsPipelines(c.device,VK_NULL_HANDLE,1,&pi,nullptr,&background),"background pipeline");
        ds.depthTestEnable=VK_TRUE; ds.depthWriteEnable=VK_TRUE;
        Check(vkCreateGraphicsPipelines(c.device,VK_NULL_HANDLE,1,&pi,nullptr,&geometry),"geometry pipeline");
        std::array<VkDescriptorSetLayoutBinding,3> bindings{{{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}}};
        VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; dl.bindingCount=3; dl.pBindings=bindings.data();
        Check(vkCreateDescriptorSetLayout(c.device,&dl,nullptr,&descriptors),"descriptor layout");
        li.setLayoutCount=1; li.pSetLayouts=&descriptors; push.stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
        Check(vkCreatePipelineLayout(c.device,&li,nullptr,&compute_layout),"compute layout");
        VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; cp.layout=compute_layout;
        cp.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,Shader(shaders/("fixture"+std::to_string(count)+".comp.spv")),"main",nullptr};
        struct ComputeOptions { VkBool32 minus,negative; float low,high; };
        const ComputeOptions options{VkBool32(mode.Minus()),VkBool32(mode.negative),mode.low,mode.high};
        static_assert(sizeof(ComputeOptions)==16);
        const std::array<VkSpecializationMapEntry,4> entries{{{0,0,4},{1,4,4},{2,8,4},{3,12,4}}};
        const VkSpecializationInfo compute_spec{4,entries.data(),sizeof(options),&options};
        cp.stage.pSpecializationInfo=&compute_spec;
        Check(vkCreateComputePipelines(c.device,VK_NULL_HANDLE,1,&cp,nullptr,&compute),"compute pipeline");
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=W*H*count*sizeof(Sample); bi.usage=VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        Check(vkCreateBuffer(c.device,&bi,nullptr,&buffer),"readback buffer"); VkMemoryRequirements mr{}; vkGetBufferMemoryRequirements(c.device,buffer,&mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=mr.size; ai.memoryTypeIndex=c.Memory(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkAllocateMemory(c.device,&ai,nullptr,&buffer_memory),"readback memory"); Check(vkBindBufferMemory(c.device,buffer,buffer_memory,0),"bind readback");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; alloc.commandPool=c.pool; alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; alloc.commandBufferCount=1;
        Check(vkAllocateCommandBuffers(c.device,&alloc,&commands),"commands"); VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; Check(vkCreateFence(c.device,&fc,nullptr,&fence),"fence");
    }
    void Draw(const Camera& camera,double left,double right,double z,bool slope,bool screen=false) {
        std::array<float,32> push{};
        const Matrix matrix=screen?Matrix{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}:camera.VP();
        std::transform(matrix.begin(),matrix.end(),push.begin(),[](double d){return float(d);});
        for(unsigned i=0;i<4;++i) {
            const double x=(i&1)?right:left,y=(i&2)?H:0;
            const double nx=2*x/W-1,ny=mode.negative?1-2*y/H:2*y/H-1;
            const double rx=(nx-camera.ox)/camera.sx,ry=(ny-camera.oy)/camera.sy;
            const double distance=slope?8/(1-.3*rx-.2*ry):z;
            push[16+i*4]=float(screen?nx:rx*distance); push[17+i*4]=float(screen?ny:ry*distance);
            push[18+i*4]=float(screen?.5:distance); push[19+i*4]=1;
        }
        vkCmdPushConstants(commands,graphics_layout,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof(push),push.data()); vkCmdDraw(commands,4,1,0,0);
    }
    std::vector<Sample> Run(int scene,const Camera& current,const Camera& previous) {
        const std::array<VkDescriptorPoolSize,2> sizes{{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1}}};
        VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dp.maxSets=1; dp.poolSizeCount=2; dp.pPoolSizes=sizes.data();
        Check(vkCreateDescriptorPool(c.device,&dp,nullptr,&descriptor_pool),"descriptor pool");
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; da.descriptorPool=descriptor_pool; da.descriptorSetCount=1; da.pSetLayouts=&descriptors;
        VkDescriptorSet set{}; Check(vkAllocateDescriptorSets(c.device,&da,&set),"descriptor set");
        VkDescriptorImageInfo di{VK_NULL_HANDLE,depth.sampled,VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL},ci{VK_NULL_HANDLE,color.sampled,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo bi{buffer,0,VK_WHOLE_SIZE}; std::array<VkWriteDescriptorSet,3> writes{};
        for(unsigned i=0;i<3;++i) { writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=set; writes[i].dstBinding=i; writes[i].descriptorCount=1; writes[i].descriptorType=i==2?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; }
        writes[0].pImageInfo=&di; writes[1].pImageInfo=&ci; writes[2].pBufferInfo=&bi; vkUpdateDescriptorSets(c.device,3,writes.data(),0,nullptr);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; Check(vkBeginCommandBuffer(commands,&begin),"begin");
        std::array<VkClearValue,2> clear{}; clear[0].color={{-1,-1,-1,-1}}; clear[1].depthStencil={1,0};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; rp.renderPass=pass; rp.framebuffer=framebuffer; rp.renderArea={{0,0},{W,H}}; rp.clearValueCount=2; rp.pClearValues=clear.data();
        vkCmdBeginRenderPass(commands,&rp,VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,background); Draw(current,0,W,0,false,true);
        vkCmdBindPipeline(commands,VK_PIPELINE_BIND_POINT_GRAPHICS,geometry);
        if(scene==0) Draw(current,0,W,8,false);
        if(scene==1) { Draw(current,0,48.5,20,false); Draw(current,0,32.5,2,false); }
        if(scene==2) Draw(current,0,W,8,true);
        if(scene==3) Draw(current,0,W,.625,false); // negative clip z, inside [-w,w]
        if(scene==4) Draw(current,0,W,.25,false); // wholly before the near plane
        vkCmdEndRenderPass(commands);
        Matrix inverse{}; Require(Vulkan::Gow3::Inverse(current.VP(),inverse),"synthetic camera inverse");
        std::array<float,32> matrices{}; const auto prev=previous.VP();
        std::transform(inverse.begin(),inverse.end(),matrices.begin(),[](double x){return float(x);}); std::transform(prev.begin(),prev.end(),matrices.begin()+16,[](double x){return float(x);});
        vkCmdBindPipeline(commands,VK_PIPELINE_BIND_POINT_COMPUTE,compute); vkCmdBindDescriptorSets(commands,VK_PIPELINE_BIND_POINT_COMPUTE,compute_layout,0,1,&set,0,nullptr);
        vkCmdPushConstants(commands,compute_layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(matrices),matrices.data()); vkCmdDispatch(commands,W/8,H/8,1);
        VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commands,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        Check(vkEndCommandBuffer(commands),"end"); VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&commands;
        Check(vkQueueSubmit(c.queue,1,&submit,fence),"submit"); const auto waited=vkWaitForFences(c.device,1,&fence,VK_TRUE,30'000'000'000ull);
        // Avoid destroying resources still in flight after a timed-out submission.
        if(waited==VK_TIMEOUT) { std::fprintf(stderr,"FAIL owned GPU submission exceeded 30s; no retry\n"); std::fflush(nullptr); std::_Exit(3); }
        Check(waited,"wait fence");
        void* data{}; Check(vkMapMemory(c.device,buffer_memory,0,VK_WHOLE_SIZE,0,&data),"map readback");
        std::vector<Sample> result(W*H*count); std::memcpy(result.data(),data,result.size()*sizeof(Sample)); vkUnmapMemory(c.device,buffer_memory); return result;
    }
};
void Analyze(const std::vector<Sample>& values,unsigned count,int scene,const Camera& current,const Camera& previous,const char* camera_mode,Mode mode={}) {
    double max_depth=0,max_motion=0,max_cpu=0,max_fragment=0; size_t geometry=0,background=0,valid=0,outside=0,mixed=0;
    std::array<std::array<float,2>,2> locations{};
    Matrix inverse{}; Require(Vulkan::Gow3::Inverse(current.VP(),inverse),"CPU inverse");
    for(unsigned y=0;y<H;++y) for(unsigned x=0;x<W;++x) {
        std::array<double,2> pixel_depth{};
        for(unsigned s=0;s<count;++s) {
            const auto& value=values[(y*W+x)*count+s]; const auto& d=value.depth_position_id; const auto& m=value.motion_valid_fragment_depth;
            for(float a:d) Require(std::isfinite(a),"nonfinite readback metadata"); for(float a:m) Require(std::isfinite(a),"nonfinite motion readback");
            Require(d[1]>0 && d[1]<1 && d[2]>0 && d[2]<1 && d[3]==float(s),"sample position/identity invalid");
            if(x==0 && y==0) locations[s]={d[1],d[2]};
            Require(d[1]==locations[s][0] && d[2]==locations[s][1],"sample locations vary within synthetic attachment; extend oracle explicitly");
            const double px=x+d[1],py=y+d[2];
            const bool clear=scene==4 || (scene==1 && px>=48.5);
            if(clear) { ++background; Require(d[0]==1 && m[2]==0 && m[0]==0 && m[1]==0,"clear must be preserved and motion rejected"); pixel_depth[s]=1; continue; }
            ++geometry;
            const double ndc_y=mode.negative?1-2*py/H:2*py/H-1;
            const double rx=(2*px/W-1-current.ox)/current.sx,ry=(ndc_y-current.oy)/current.sy;
            const double z=scene==0?8:scene==1?(px<32.5?2:20):scene==3?.625:8/(1-.3*rx-.2*ry);
            const Point world{rx*z,ry*z,z}; auto now=current.Project(world,{0,0,W,H}),before=previous.Project(world,{0,0,W,H});
            if(!mode.negative) { now[1]=H-1-now[1]; before[1]=H-1-before[1]; }
            const double unit_depth=mode.Minus()?(now[2]+1)*.5:now[2];
            const double expected_depth=mode.low+(mode.high-mode.low)*unit_depth;
            const double depth_error=std::abs(d[0]-expected_depth); max_depth=std::max(max_depth,depth_error); pixel_depth[s]=d[0];
            Require(depth_error<2e-6,"depth sample differs from independent ray/plane oracle");
            const double fragment_error=std::abs(d[0]-m[3]); max_fragment=std::max(max_fragment,fragment_error);
            Require(fragment_error<2e-6,"stored depth differs from sample-shaded fragment depth");
            const bool expected=before[0]>=-.5 && before[0]<=W-.5 && before[1]>=-.5 && before[1]<=H-.5 && before[2]>=(mode.Minus()?-1:0) && before[2]<=1;
            Require((m[2]==1)==expected,"GPU visibility differs from independent world projection");
            if(!expected) { ++outside; Require(m[0]==0 && m[1]==0,"invalid motion not zero"); continue; }
            ++valid; const double error=std::hypot(m[0]-(before[0]-now[0]),m[1]-(before[1]-now[1])); max_motion=std::max(max_motion,error);
            Require(error<1e-3,"GPU motion differs from independent forward oracle by >= .001 px");
            // Production CPU reference accepts pixel-center coordinates >= 0;
            // compare only its supported interior domain. GPU fixture includes
            // actual subpixel positions at image borders as well.
            if(!mode.Minus() && mode.negative && mode.low==0 && mode.high==1 && now[0]>=0 && now[1]>=0) {
                std::array<double,2> cpu{};
                Require(Vulkan::Gow3::Reproject(inverse,previous.VP(),now[0],now[1],d[0],W,H,cpu),"CPU reference rejected interior sample");
                max_cpu=std::max(max_cpu,std::hypot(m[0]-cpu[0],m[1]-cpu[1]));
                Require(std::hypot(m[0]-cpu[0],m[1]-cpu[1])<1e-3,"GPU differs from CPU reference");
            }
        }
        if(count==2 && scene==1 && std::abs(pixel_depth[0]-pixel_depth[1])>1e-5) ++mixed;
    }
    if(count==2) Require(locations[0]!=locations[1],"two samples unexpectedly share a location");
    if(scene==1 && count==2) Require(mixed>=H,"fixture failed to cover a two-surface/clear edge");
    const char* scene_name=scene==0?"uniform":scene==1?"two-surfaces-clear":scene==2?"sloped":scene==3?"negative-clip-z":"near-clipped";
    std::printf("PASS samples=%u scene=%s camera=%s path=%s viewport_height=%d depth_range=(%.3g,%.3g) total=%zu geometry=%zu clear=%zu valid=%zu offscreen=%zu mixed_pixels=%zu max_depth_error=%.9g max_fragment_depth_error=%.9g max_motion_error_px=%.9g max_gpu_cpu_error_px=%.9g\n",count,scene_name,camera_mode,mode.Name(),mode.negative?-int(H):int(H),mode.low,mode.high,values.size(),geometry,background,valid,outside,mixed,max_depth,max_fragment,max_motion,max_cpu);
    for(unsigned s=0;s<count;++s) std::printf("sample[%u]=(%.6f,%.6f) %s\n",s,locations[s][0],locations[s][1],count==2?"measured from gl_SamplePosition":"nominal single-sample pixel center");
}
void ComparePaths(const std::vector<Sample>& a,const std::vector<Sample>& b) {
    Require(a.size()==b.size(),"path readback size mismatch");
    double depth_error=0,motion_error=0;
    for(size_t i=0;i<a.size();++i) {
        depth_error=std::max(depth_error,double(std::abs(a[i].depth_position_id[0]-b[i].depth_position_id[0])));
        for(unsigned c=1;c<4;++c) Require(a[i].depth_position_id[c]==b[i].depth_position_id[c],"path sample identity/position mismatch");
        const auto& x=a[i].motion_valid_fragment_depth; const auto& y=b[i].motion_valid_fragment_depth;
        Require(x[2]==y[2],"path visibility mismatch");
        motion_error=std::max(motion_error,std::hypot(double(x[0]-y[0]),double(x[1]-y[1])));
    }
    Require(depth_error<2e-6 && motion_error<1e-3,"native versus fallback exceeds tolerance");
    std::printf("PASS native_vs_fallback samples=%zu max_depth_error=%.9g max_motion_error_px=%.9g\n",a.size(),depth_error,motion_error);
}
} // namespace
int main(int argc,char** argv) {
    try {
        Require(argc==2,"usage: gow3-vulkan-depth-test <generated shader directory>"); Context context; context.Initialize();
        const Camera current{.ox=.04,.oy=-.025},previous{.eye={-.2,.1,-.3},.yaw=.07,.ox=-.02,.oy=.03};
        bool unsupported=false; unsigned completed=0;
        for(unsigned n:{1u,2u}) {
            const auto samples=n==2?VK_SAMPLE_COUNT_2_BIT:VK_SAMPLE_COUNT_1_BIT;
            const bool depth=context.Supports(DepthFormat,VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,samples);
            const bool color=context.Supports(ColorFormat,VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,samples);
            if(!depth || !color || (n==2 && !context.features.sampleRateShading)) {
                std::printf("UNSUPPORTED samples=%u depth=%d color=%d sampleRateShading=%u; no resolve or count substitution\n",n,depth,color,context.features.sampleRateShading); unsupported=true; continue;
            }
            { Case test(context,n); test.Create(argv[1]); const auto result=test.Run(0,current,current); Analyze(result,n,0,current,current,"stationary"); ++completed; }
            for(int scene=0;scene<3;++scene) { Case test(context,n); test.Create(argv[1]); const auto result=test.Run(scene,current,previous); Analyze(result,n,scene,current,previous,"moving"); ++completed; }
            auto minus_current=current,minus_previous=previous;
            minus_current.depth=minus_previous.depth=Depth::MinusOneToOne;
            for(bool negative:{false,true}) for(bool restricted:{false,true})
            for(int scene:{1,2,3,4}) {
                Mode mode{DepthPath::MinusFallback,negative,restricted?.2f:0,restricted?.8f:1};
                std::vector<Sample> fallback;
                { Case test(context,n,mode); test.Create(argv[1]); fallback=test.Run(scene,minus_current,minus_previous);
                  Analyze(fallback,n,scene,minus_current,minus_previous,"moving",mode); ++completed; }
                if(context.depth_clip_control) {
                    mode.path=DepthPath::MinusNative;
                    Case test(context,n,mode); test.Create(argv[1]); const auto native=test.Run(scene,minus_current,minus_previous);
                    Analyze(native,n,scene,minus_current,minus_previous,"moving",mode); ComparePaths(native,fallback); ++completed;
                } else { std::printf("UNSUPPORTED native MinusW path; fallback results do not validate it\n"); unsupported=true; }
            }
        }
        std::printf("completed_cases=%u validation_errors=%u validation_warnings=%u synthetic_only=true game_launched=false\n",completed,validation_errors.load(),validation_warnings.load());
        if(validation_errors) return 1; return unsupported?77:0;
    } catch(const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
