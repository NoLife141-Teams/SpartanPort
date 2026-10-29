// SPDX-License-Identifier: GPL-2.0-or-later
// Offline NR laboratory. Consumes an actual post-tonemap display readback;
// never starts the game, changes its settings, or touches a save.
#define NOMINMAX
#include <windows.h>
#include "gpu/dlss_bridge/spartanport_dlssnr_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void Check(VkResult result) {
    Require(result == VK_SUCCESS, "Vulkan failure " + std::to_string(result));
}
void Log(int, const char* message) { std::fprintf(stderr, "NR: %s\n", message); }
std::string Json(const std::string& input) {
    std::string result = "\"";
    for (unsigned char c : input) {
        if (c == '\\' || c == '"') result += '\\';
        if (c < 32) {
            char escaped[7];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", unsigned(c));
            result += escaped;
        } else result += char(c);
    }
    return result + '"';
}

struct Context {
    HMODULE module{};
    const SpartanDlssNrApi* api{};
    VkInstance instance{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    VkDevice device{};
    VkQueue queue{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    void Nr(int32_t result) { Require(result != 0, api->Problem()); }
    std::vector<const char*> Names(uint32_t count, const VkExtensionProperties* extensions) {
        std::vector<const char*> names;
        for (uint32_t n = 0; n < count; ++n) names.push_back(extensions[n].extensionName);
        return names;
    }
    void Load(const std::filesystem::path& directory) {
        module = LoadLibraryExW((directory / L"spartanport_dlssnr.dll").c_str(), nullptr,
                               LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        Require(module, "Missing laboratory module spartanport_dlssnr.dll");
        auto get = reinterpret_cast<SpartanDlssNrGetApiFn>(GetProcAddress(module, "SpartanDlssNrGetApi"));
        api = get ? get() : nullptr;
        Require(api && api->abi == SPARTAN_DLSSNR_BRIDGE_ABI, "NR bridge ABI mismatch");
    }
    void Inventory(bool with_nr_extensions) {
        uint32_t count{};
        const VkExtensionProperties* required{};
        if (with_nr_extensions) Nr(api->InstanceExtensions(&count, &required));
        const auto names = Names(count, required);
        VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        application.apiVersion = VK_API_VERSION_1_3;
        application.pApplicationName = "SpartanPort offline DLSS-NR laboratory";
        VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        info.pApplicationInfo = &application;
        info.enabledExtensionCount = uint32_t(names.size());
        info.ppEnabledExtensionNames = names.data();
        Check(vkCreateInstance(&info, nullptr, &instance));
        Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties p{};
            vkGetPhysicalDeviceProperties(candidate, &p);
            if (p.vendorID == 0x10de) { physical = candidate; properties = p; break; }
        }
        Require(physical, "No NVIDIA Vulkan GPU found");
    }
    void Initialize() {
        uint32_t count{};
        const VkExtensionProperties* required{};
        Nr(api->DeviceExtensions(instance, physical, &count, &required));
        auto names = Names(count, required);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        uint32_t family{};
        constexpr auto flags = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_GRAPHICS_BIT;
        while (family < count && (families[family].queueFlags & flags) != flags) ++family;
        Require(family < count, "No graphics/compute queue");
        float priority = 1;
        VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        features.pNext = &features12;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.pNext = &features;
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue_info;
        info.enabledExtensionCount = uint32_t(names.size());
        info.ppEnabledExtensionNames = names.data();
        Check(vkCreateDevice(physical, &info, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        Nr(api->Initialize(instance, physical, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr));
        VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool_info.queueFamilyIndex = family;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        Check(vkCreateCommandPool(device, &pool_info, nullptr, &pool));
        VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocate.commandPool = pool;
        allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocate.commandBufferCount = 1;
        Check(vkAllocateCommandBuffers(device, &allocate, &command));
    }
    uint32_t Memory(uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties memory{};
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        for (uint32_t n = 0; n < memory.memoryTypeCount; ++n)
            if ((bits & (1u << n)) && (memory.memoryTypes[n].propertyFlags & flags) == flags) return n;
        throw std::runtime_error("No compatible Vulkan memory type");
    }
    void Begin() {
        Check(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        Check(vkBeginCommandBuffer(command, &begin));
    }
    void End() {
        Check(vkEndCommandBuffer(command));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        Check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        Check(vkQueueWaitIdle(queue));
    }
    ~Context() {
        if (device) vkDeviceWaitIdle(device);
        if (api) api->Shutdown();
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        if (module) FreeLibrary(module);
    }
};

// All these resources outlive evaluation and are destroyed after queue idle.
struct Fixture {
    Context& c;
    uint32_t width, height;
    VkImage images[2]{};
    VkImageView views[2]{};
    VkDeviceMemory image_memory[2]{};
    VkBuffer buffer{};
    VkDeviceMemory buffer_memory{};
    void* mapped{};
    uint64_t bytes;
    Fixture(Context& context, uint32_t w, uint32_t h) : c(context), width(w), height(h), bytes(uint64_t(w)*h*16) {}
    void Initialize() {
        for (unsigned n = 0; n < 2; ++n) {
            VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            info.imageType = VK_IMAGE_TYPE_2D;
            info.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            info.extent = {width, height, 1};
            info.mipLevels = info.arrayLayers = 1;
            info.samples = VK_SAMPLE_COUNT_1_BIT;
            info.tiling = VK_IMAGE_TILING_OPTIMAL;
            info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            Check(vkCreateImage(c.device, &info, nullptr, &images[n]));
            VkMemoryRequirements memory{};
            vkGetImageMemoryRequirements(c.device, images[n], &memory);
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = memory.size;
            allocate.memoryTypeIndex = c.Memory(memory.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            Check(vkAllocateMemory(c.device, &allocate, nullptr, &image_memory[n]));
            Check(vkBindImageMemory(c.device, images[n], image_memory[n], 0));
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = images[n];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = info.format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            Check(vkCreateImageView(c.device, &view, nullptr, &views[n]));
        }
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        Check(vkCreateBuffer(c.device, &info, nullptr, &buffer));
        VkMemoryRequirements memory{};
        vkGetBufferMemoryRequirements(c.device, buffer, &memory);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocate.allocationSize = memory.size;
        allocate.memoryTypeIndex = c.Memory(memory.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Check(vkAllocateMemory(c.device, &allocate, nullptr, &buffer_memory));
        Check(vkBindBufferMemory(c.device, buffer, buffer_memory, 0));
        Check(vkMapMemory(c.device, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    }
    SpartanDlssImage Resource(unsigned n) const {
        return {images[n], views[n], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                VK_FORMAT_R32G32B32A32_SFLOAT, width, height};
    }
    void ImageBarrier(unsigned n, bool initial = false) {
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout = initial ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = images[n];
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.srcAccessMask = initial ? 0 : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
        vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    VkBufferImageCopy Region() const {
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        return region;
    }
    ~Fixture() {
        if (c.device) vkDeviceWaitIdle(c.device);
        c.api->ReleaseFeature();
        if (mapped) vkUnmapMemory(c.device, buffer_memory);
        if (buffer) vkDestroyBuffer(c.device, buffer, nullptr);
        if (buffer_memory) vkFreeMemory(c.device, buffer_memory, nullptr);
        for (unsigned n = 0; n < 2; ++n) {
            if (views[n]) vkDestroyImageView(c.device, views[n], nullptr);
            if (images[n]) vkDestroyImage(c.device, images[n], nullptr);
            if (image_memory[n]) vkFreeMemory(c.device, image_memory[n], nullptr);
        }
    }
};
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 6) {
        std::fprintf(stderr, "usage: gow3-dlssnr-probe DLL_DIRECTORY NEW_OUTPUT_DIRECTORY [INPUT.bgra WIDTH HEIGHT]\n");
        return 1;
    }
    std::filesystem::path output;
    std::string gpu, status = "error", error;
    uint32_t driver{};
    bool compiled{}, runtime{}, created{}, evaluated{}, readback{}, owns_output{};
    bool discovered{};
    SpartanDlssNrDiscovery discovery{};
    int exit_code = 1;
    try {
        const auto directory = std::filesystem::absolute(argv[1]);
        output = std::filesystem::absolute(argv[2]);
        Require(std::filesystem::create_directory(output), "Output directory must be new");
        owns_output = true;
        Context c;
        c.Load(directory);
        compiled = c.api->sdk_compiled != 0;
        runtime = std::filesystem::is_regular_file(directory / "nvngx_dlssnr.dll");
        const bool dependencies = compiled && runtime;
        if (dependencies) c.Nr(c.api->Configure(directory.c_str(), output.c_str(), Log));
        c.Inventory(dependencies);
        gpu = c.properties.deviceName;
        driver = c.properties.driverVersion;
        if (runtime) {
            auto discover = reinterpret_cast<SpartanDlssNrDiscoverFn>(GetProcAddress(c.module, "SpartanDlssNrDiscover"));
            if (discover) {
                discover(c.instance, c.physical, directory.c_str(), output.c_str(), &discovery);
                discovered = true;
            }
        }
        if (!dependencies) {
            status = "blocked_dependencies";
            error = std::string(compiled ? "" : "NR beta SDK not compiled. ") +
                    (runtime ? "" : "nvngx_dlssnr.dll missing. ");
            exit_code = 20;
        } else {
            c.Initialize();
            status = "initialized_not_evaluated";
            exit_code = 0;
            if (argc == 6) {
                const auto w = std::stoul(argv[4]), h = std::stoul(argv[5]);
                Require(w && h && w <= 8192 && h <= 8192, "Invalid input extent");
                const uint64_t pixels = uint64_t(w)*h;
                Require(std::filesystem::file_size(argv[3]) == pixels*4, "Input must be raw BGRA8 at the supplied extent");
                std::vector<unsigned char> bgra(pixels*4);
                std::ifstream source(argv[3], std::ios::binary);
                source.read(reinterpret_cast<char*>(bgra.data()), std::streamsize(bgra.size()));
                Require(bool(source), "Failed to read input capture");
                Fixture fixture(c, uint32_t(w), uint32_t(h));
                fixture.Initialize();
                auto* rgba = static_cast<float*>(fixture.mapped);
                for (uint64_t n = 0; n < pixels; ++n) {
                    rgba[n*4] = bgra[n*4+2]/255.f;
                    rgba[n*4+1] = bgra[n*4+1]/255.f;
                    rgba[n*4+2] = bgra[n*4]/255.f;
                    rgba[n*4+3] = 1;
                }
                // Preserve display encoding: channel unpack only, no gamma,
                // tone mapping, resizing or sharpening of this real capture.
                c.Begin();
                fixture.ImageBarrier(0, true);
                fixture.ImageBarrier(1, true);
                auto region = fixture.Region();
                vkCmdCopyBufferToImage(c.command, fixture.buffer, fixture.images[0],
                    VK_IMAGE_LAYOUT_GENERAL, 1, &region);
                fixture.ImageBarrier(0);
                c.Nr(c.api->CreateFeature(c.command, uint32_t(w), uint32_t(h)));
                created = true;
                c.End();
                c.Begin();
                SpartanDlssNrFrame frame{};
                frame.color = fixture.Resource(0);
                frame.output = fixture.Resource(1);
                frame.display_encoded = 1;
                frame.reset = 1;
                // No invented motion/depth for a frozen display capture.
                frame.settings = {1, 1, 0, 0, 0, 0, 1};
                c.Nr(c.api->Evaluate(c.command, &frame));
                evaluated = true;
                fixture.ImageBarrier(1);
                vkCmdCopyImageToBuffer(c.command, fixture.images[1], VK_IMAGE_LAYOUT_GENERAL,
                    fixture.buffer, 1, &region);
                VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
                vkCmdPipelineBarrier(c.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                    0, 1, &host, 0, nullptr, 0, nullptr);
                c.End();
                Require(std::all_of(rgba, rgba+pixels*4, [](float x) { return std::isfinite(x); }),
                        "NR GPU readback contains non-finite values");
                std::ofstream result(output / "nr-output.rgba32f", std::ios::binary);
                result.write(reinterpret_cast<const char*>(rgba), std::streamsize(fixture.bytes));
                Require(bool(result), "Failed to write NR readback");
                readback = true;
                status = "evaluated_gpu_readback";
                // Feature must retire before its input/output images.
                c.api->ReleaseFeature();
            }
        }
    } catch (const std::exception& exception) {
        status = "error";
        error = exception.what();
        exit_code = 1;
    }
    const auto discovery_number = [&](uint32_t value) {
        return discovered ? std::to_string(value) : std::string("null");
    };
    const std::string report = "{\n  \"status\": " + Json(status) +
        ",\n  \"gpu\": " + Json(gpu) + ",\n  \"driver_raw\": " + std::to_string(driver) +
        ",\n  \"sdk_compiled\": " + (compiled ? "true" : "false") +
        ",\n  \"runtime_present\": " + (runtime ? "true" : "false") +
        ",\n  \"public_ngx_discovery_attempted\": " + (discovered ? "true" : "false") +
        ",\n  \"requirements_result\": " + discovery_number(discovery.requirements_result) +
        ",\n  \"feature_support\": " + discovery_number(discovery.support) +
        ",\n  \"instance_extensions_result\": " + discovery_number(discovery.instance_extensions_result) +
        ",\n  \"instance_extensions_count\": " + discovery_number(discovery.instance_extensions_count) +
        ",\n  \"device_extensions_result\": " + discovery_number(discovery.device_extensions_result) +
        ",\n  \"device_extensions_count\": " + discovery_number(discovery.device_extensions_count) +
        ",\n  \"feature_created\": " + (created ? "true" : "false") +
        ",\n  \"evaluate_recorded\": " + (evaluated ? "true" : "false") +
        ",\n  \"gpu_readback_completed\": " + (readback ? "true" : "false") +
        ",\n  \"problem\": " + Json(error) + "\n}\n";
    std::fputs(report.c_str(), stdout);
    // Never overwrite an existing report on an output-directory error.
    if (owns_output &&
        !std::filesystem::exists(output / "result.json")) {
        std::ofstream result(output / "result.json");
        result << report;
    }
    return exit_code;
}
