// SPDX-License-Identifier: GPL-2.0-or-later
// Opt-in NGX laboratory. Replays a frozen crop of a completed real GPU capture
// with known subpixel sampling and motion. No game or input automation.
#define NOMINMAX
#include <windows.h>
#include "gpu/dlss_bridge/spartanport_dlss_bridge.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
constexpr uint32_t W=768,H=512;
void Check(VkResult r) { if(r!=VK_SUCCESS) throw std::runtime_error("Vulkan "+std::to_string(r)); }
void Require(bool ok) { if(!ok) throw std::runtime_error("NGX/fixture failure"); }
void Log(int,const char* s) { std::puts(s); }
float Float(uint16_t h) {
    const float s=h&0x8000?-1.f:1.f; const auto e=(h>>10)&31,m=h&1023;
    return s*(e?std::ldexp(1.f+m/1024.f,int(e)-15):std::ldexp(float(m),-24));
}
uint16_t Half(float f) {
    const uint32_t b=std::bit_cast<uint32_t>(f),sign=(b>>16)&0x8000;
    const int e=int((b>>23)&255)-127+15; const auto m=b&0x7fffff;
    if(e<=0) return uint16_t(sign|(e < -10 ? 0 : ((m|0x800000)+(1u<<(13-e)))>>(14-e)));
    return uint16_t(sign|(e>=31?0x7c00:((uint32_t(e)<<10)+(m+0x1000)/0x2000)));
}
struct Context {
    const SpartanDlssApi* api{};
    VkInstance instance{}; VkPhysicalDevice physical{}; VkDevice device{}; VkQueue queue{};
    VkCommandPool pool{}; VkCommandBuffer cmd{}; VkPhysicalDeviceMemoryProperties memory{};
    uint32_t Memory(uint32_t bits,VkMemoryPropertyFlags flags) {
        for(uint32_t n=0;n<memory.memoryTypeCount;++n)
            if((bits&(1u<<n))&&(memory.memoryTypes[n].propertyFlags&flags)==flags) return n;
        throw std::runtime_error("memory type");
    }
    void Initialize(const std::filesystem::path& dll,const std::filesystem::path& output) {
        auto module=LoadLibraryW((dll/L"spartanport_dlss.dll").c_str()); Require(module);
        auto get=reinterpret_cast<SpartanDlssGetApiFn>(GetProcAddress(module,"SpartanDlssGetApi")); Require(get);
        api=get(); Require(api&&api->abi==SPARTAN_DLSS_BRIDGE_ABI);
        std::filesystem::create_directory(output/"ngx");
        Require(api->Configure(dll.c_str(),(output/"ngx").c_str(),Log));
        uint32_t n{}; const VkExtensionProperties* ext{};
        Require(api->InstanceExtensions(&n,&ext)); std::vector<const char*> names;
        for(uint32_t i=0;i<n;++i) names.push_back(ext[i].extensionName);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_3;
        app.pApplicationName="Frozen real GoW3 NGX replay";
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ci.pApplicationInfo=&app;
        ci.enabledExtensionCount=uint32_t(names.size());ci.ppEnabledExtensionNames=names.data();
        Check(vkCreateInstance(&ci,nullptr,&instance));
        Check(vkEnumeratePhysicalDevices(instance,&n,nullptr));std::vector<VkPhysicalDevice> devices(n);
        Check(vkEnumeratePhysicalDevices(instance,&n,devices.data()));
        for(auto d:devices) {VkPhysicalDeviceProperties p{};vkGetPhysicalDeviceProperties(d,&p);if(p.vendorID==0x10de) physical=d;}
        Require(physical);vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);std::vector<VkQueueFamilyProperties> queues(n);
        vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,queues.data());uint32_t family=0;
        while(family<n&&(queues[family].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))!=(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)) ++family;
        Require(family<n); names.clear(); Require(api->DeviceExtensions(instance,physical,&n,&ext));
        for(uint32_t i=0;i<n;++i) names.push_back(ext[i].extensionName);
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f.pNext=&f12;
        vkGetPhysicalDeviceFeatures2(physical,&f);float priority=1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
        VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};dc.queueCreateInfoCount=1;dc.pQueueCreateInfos=&qi;
        dc.pNext=&f;dc.enabledExtensionCount=uint32_t(names.size());dc.ppEnabledExtensionNames=names.data();
        Check(vkCreateDevice(physical,&dc,nullptr,&device));vkGetDeviceQueue(device,family,0,&queue);
        Require(api->Initialize(instance,physical,device,vkGetInstanceProcAddr,vkGetDeviceProcAddr));
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        Check(vkCreateCommandPool(device,&pc,nullptr,&pool));
        VkCommandBufferAllocateInfo ac{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ac.commandPool=pool;ac.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ac.commandBufferCount=1;
        Check(vkAllocateCommandBuffers(device,&ac,&cmd));
    }
    void Begin() {Check(vkResetCommandBuffer(cmd,0));VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(vkBeginCommandBuffer(cmd,&b));}
    void End() {Check(vkEndCommandBuffer(cmd));VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};s.commandBufferCount=1;s.pCommandBuffers=&cmd;Check(vkQueueSubmit(queue,1,&s,VK_NULL_HANDLE));Check(vkQueueWaitIdle(queue));}
    void Barrier() {VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;b.dstAccessMask=b.srcAccessMask;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,nullptr,0,nullptr);}
    ~Context() {if(device){vkDeviceWaitIdle(device);api->Shutdown();vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);}if(instance)vkDestroyInstance(instance,nullptr);}
};
struct Image {
    Context& c;VkImage image{};VkImageView view{};VkDeviceMemory memory{};VkFormat format;
    Image(Context& context,VkFormat f):c(context),format(f) {
        VkImageCreateInfo i{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};i.imageType=VK_IMAGE_TYPE_2D;i.format=f;i.extent={W,H,1};i.mipLevels=i.arrayLayers=1;i.samples=VK_SAMPLE_COUNT_1_BIT;
        i.tiling=VK_IMAGE_TILING_OPTIMAL;i.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateImage(c.device,&i,nullptr,&image));VkMemoryRequirements mr{};vkGetImageMemoryRequirements(c.device,image,&mr);
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=mr.size;a.memoryTypeIndex=c.Memory(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        Check(vkAllocateMemory(c.device,&a,nullptr,&memory));Check(vkBindImageMemory(c.device,image,memory,0));
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=f;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        Check(vkCreateImageView(c.device,&v,nullptr,&view));
    }
    SpartanDlssImage Resource() {return {image,view,{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1},format,W,H};}
    void Initial() {VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=image;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(c.cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);}
    ~Image(){vkDestroyImageView(c.device,view,nullptr);vkDestroyImage(c.device,image,nullptr);vkFreeMemory(c.device,memory,nullptr);}
};
struct Buffer {
    Context& c;VkBuffer buffer{};VkDeviceMemory memory{};void* data{};
    Buffer(Context& context):c(context) {
        VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};b.size=uint64_t(W)*H*32;b.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateBuffer(c.device,&b,nullptr,&buffer));VkMemoryRequirements mr{};vkGetBufferMemoryRequirements(c.device,buffer,&mr);
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};a.allocationSize=mr.size;a.memoryTypeIndex=c.Memory(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkAllocateMemory(c.device,&a,nullptr,&memory));Check(vkBindBufferMemory(c.device,buffer,memory,0));Check(vkMapMemory(c.device,memory,0,VK_WHOLE_SIZE,0,&data));
    }
    void Copy(Image& i,uint64_t offset,bool read=false) {VkBufferImageCopy r{};r.bufferOffset=offset;r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};r.imageExtent={W,H,1};
        if(read) vkCmdCopyImageToBuffer(c.cmd,i.image,VK_IMAGE_LAYOUT_GENERAL,buffer,1,&r);
        else vkCmdCopyBufferToImage(c.cmd,buffer,i.image,VK_IMAGE_LAYOUT_GENERAL,1,&r);}
    ~Buffer(){vkUnmapMemory(c.device,memory);vkDestroyBuffer(c.device,buffer,nullptr);vkFreeMemory(c.device,memory,nullptr);}
};
float Halton(unsigned n,unsigned b){float f=1,v=0;while(n){f/=b;v+=f*(n%b);n/=b;}return v;}
}
int main(int argc,char** argv) try {
    if(argc<4||argc>5) throw std::runtime_error("usage: gow3-dlaa-replay DLL_DIRECTORY CAPTURE_DIRECTORY NEW_OUTPUT_DIRECTORY [analytic]");
    const bool analytic=argc==5;
    if(analytic) Require(std::strcmp(argv[4],"analytic")==0);
    const auto dll=std::filesystem::absolute(argv[1]),source=std::filesystem::absolute(argv[2]),output=std::filesystem::absolute(argv[3]);
    Require(std::filesystem::create_directory(output));
    // A fixed crop from the real 1440p input. Interpolation is explicitly part
    // of the replay and shared by all candidates; it is not a fresh game frame.
    std::vector<uint16_t> raw(2560*1440*4);
    Require(std::filesystem::file_size(source/"input.rgba16f")==raw.size()*2);
    std::ifstream file(source/"input.rgba16f",std::ios::binary);file.read(reinterpret_cast<char*>(raw.data()),raw.size()*2);Require(bool(file));
    Require(std::all_of(raw.begin(),raw.end(),[](uint16_t h){return (h&0x7c00)!=0x7c00;}));
    const auto sample=[&](float x,float y,unsigned k){
        if(analytic) {const float p=k==0?x*.14f:k==1?y*.14f:(x*.8f+y*.6f)*.24f;return .25f+.18f*std::sin(p*6.2831853f);}
        x=std::clamp(x+220.f,0.f,2558.f);y=std::clamp(y+260.f,0.f,1438.f);auto ix=unsigned(x),iy=unsigned(y);float fx=x-ix,fy=y-iy;
        auto p=[&](unsigned dx,unsigned dy){return Float(raw[((iy+dy)*2560+ix+dx)*4+k]);};
        return (p(0,0)*(1-fx)+p(1,0)*fx)*(1-fy)+(p(0,1)*(1-fx)+p(1,1)*fx)*fy;};
    Context c;c.Initialize(dll,output);Image color(c,VK_FORMAT_R16G16B16A16_SFLOAT),depth(c,VK_FORMAT_R32_SFLOAT),motion(c,VK_FORMAT_R16G16_SFLOAT),filtered(c,VK_FORMAT_R16G16B16A16_SFLOAT);Buffer buffer(c);
    c.Begin();for(auto* i:{&color,&depth,&motion,&filtered})i->Initial();c.End();
    const uint64_t pixels=W*H,depth_offset=pixels*8,motion_offset=pixels*12,read_offset=pixels*16;
    std::ofstream stats(output/"results.csv");stats<<"name,preset,sign,sign_y,reset_each,translation,mae,rmse,edge_energy\n";
    struct Mode{const char* name;unsigned preset;float sign;bool reset;float move;float ysign=99;};
    const Mode modes[]{ {"negative",0,-1,false,0},{"positive",0,1,false,0},{"zero",0,0,false,0},
        {"reset",0,-1,true,0},{"K",11,-1,false,0},{"L",12,-1,false,0},
        {"moving",0,-1,false,.75f}, {"moving-positive",0,1,false,.75f},
        {"xy",0,1,false,0,-1}, {"yx",0,-1,false,0,1} };
    for(const auto mode:modes){
        double abs=0,square=0,energy=0;unsigned count=0;
        c.Begin();SpartanDlssFeature feature{W,H,W,H,0,0,mode.preset,1};Require(c.api->CreateFeature(c.cmd,&feature));c.End();
        for(unsigned frame=0;frame<48;++frame){
            const float jx=Halton(frame%8+1,2)-.5f,jy=Halton(frame%8+1,3)-.5f;
            auto* rgba=static_cast<uint16_t*>(buffer.data);auto* depths=reinterpret_cast<float*>(rgba+depth_offset/2);auto* mv=rgba+motion_offset/2;
            for(unsigned y=0;y<H;++y)for(unsigned x=0;x<W;++x){const auto n=y*W+x;
                for(unsigned k=0;k<3;++k)rgba[n*4+k]=Half(sample(x-jx-frame*mode.move,y-jy,k));rgba[n*4+3]=Half(1);
                depths[n]=.5f;mv[n*2]=Half(-mode.move);mv[n*2+1]=0;}
            c.Begin();c.Barrier();buffer.Copy(color,0);buffer.Copy(depth,depth_offset);buffer.Copy(motion,motion_offset);c.Barrier();
            SpartanDlssEvaluate e{color.Resource(),depth.Resource(),motion.Resource(),filtered.Resource(),mode.sign*jx,(mode.ysign==99?mode.sign:mode.ysign)*jy,int(!frame||mode.reset),1000.f/60,0};
            Require(c.api->Evaluate(c.cmd,&e));c.Barrier();buffer.Copy(filtered,read_offset,true);
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(c.cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);c.End();
            auto* result=reinterpret_cast<uint16_t*>(static_cast<char*>(buffer.data)+read_offset);
            if(frame>=40) {std::ofstream phase(output/(std::string(mode.name)+"."+std::to_string(frame)+".rgba16f"),std::ios::binary);phase.write(reinterpret_cast<char*>(result),pixels*8);}
            if(frame>=40) for(unsigned y=16;y<H-16;++y)for(unsigned x=64;x<W-16;++x)for(unsigned k=0;k<3;++k){
            float v=Float(result[(y*W+x)*4+k]),target=sample(x-frame*mode.move,y,k),d=v-target;Require(std::isfinite(v));
            abs+=std::abs(d);square+=d*d;energy+=std::abs(v-Float(result[(y*W+x-1)*4+k]));++count;}
        }
        auto* result=reinterpret_cast<uint16_t*>(static_cast<char*>(buffer.data)+read_offset);
        stats<<mode.name<<','<<mode.preset<<','<<mode.sign<<','<<(mode.ysign==99?mode.sign:mode.ysign)<<','<<mode.reset<<','<<mode.move<<','<<abs/count<<','<<std::sqrt(square/count)<<','<<energy/count<<'\n';stats.flush();
        std::ofstream image(output/(std::string(mode.name)+".rgba16f"),std::ios::binary);image.write(reinterpret_cast<char*>(result),pixels*8);
        std::printf("%s mae=%.7f rmse=%.7f edge=%.7f\n",mode.name,abs/count,std::sqrt(square/count),energy/count);
        c.api->ReleaseFeature();
    }
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}
