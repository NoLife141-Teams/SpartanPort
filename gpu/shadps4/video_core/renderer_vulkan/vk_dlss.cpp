// SPDX-FileCopyrightText: Copyright 2026 IFreemz, spartanport contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>

#include "video_core/renderer_vulkan/vk_dlss.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../../../dlss_bridge/spartanport_dlss_bridge.h"
#include "../../../dlss_bridge/spartanport_dlssnr_bridge.h"
#endif

namespace Vulkan {

int Dlss::QualityForScale(float scale) {
    // DLAA only for equal sizes: any real upscale needs at least the Quality mode.
    return scale >= 2.9f ? 4 : scale >= 1.95f ? 3 : scale >= 1.65f ? 2 : scale > 1.01f ? 1 : 0;
}

#ifdef _WIN32

namespace {
constexpr wchar_t BridgeName[] = L"spartanport_dlss.dll";
constexpr wchar_t NgxName[] = L"nvngx_dlss.dll";
constexpr wchar_t NrBridgeName[] = L"spartanport_dlssnr.dll";
constexpr wchar_t NrNgxName[] = L"nvngx_dlssnr.dll";
bool NrPreviewRequested() {
    const char* value = std::getenv("SPARTAN_GOW3_NR_PREVIEW");
    return value && std::string_view(value) == "1";
}

std::filesystem::path ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    path.resize(length);
    return std::filesystem::path{path}.parent_path();
}

void BridgeLog(int warning, const char* message) {
    std::printf("DLSS: %s%s\n", warning ? "warning: " : "", message);
}

bool CollectExtensions(const VkExtensionProperties* required, u32 count,
                       const std::vector<vk::ExtensionProperties>& available,
                       std::vector<std::string>& storage, std::vector<const char*>& enabled) {
    storage.clear();
    for (u32 i = 0; i < count; ++i) {
        const auto& request = required[i];
        const auto match = std::ranges::find_if(available, [&](const auto& extension) {
            return std::strcmp(extension.extensionName.data(), request.extensionName) == 0 &&
                   extension.specVersion >= request.specVersion;
        });
        if (match == available.end()) {
            std::printf("DLSS: missing Vulkan extension %s\n", request.extensionName);
            return false;
        }
        storage.emplace_back(request.extensionName);
    }
    for (const auto& name : storage) {
        if (std::ranges::none_of(enabled, [&](const char* current) { return name == current; })) {
            enabled.push_back(name.c_str());
        }
    }
    return true;
}
} // namespace

struct Dlss::Impl {
    HMODULE module{};
    const SpartanDlssApi* api{};
    const SpartanDlssNrApi* nr{};
    std::vector<std::string> instance_extensions, device_extensions;
    std::optional<FeatureDesc> feature;
    std::string problem;
    bool eligible{true}, instance_ready{}, device_ready{}, initialized{}, available{};

    void Disable(std::string_view reason) {
        eligible = available = false;
        problem = reason;
        std::printf("DLSS: unavailable (%.*s)\n", int(reason.size()), reason.data());
    }
};

Dlss* Dlss::Get() {
    static Dlss* const dlss = []() -> Dlss* {
        const char* setting = std::getenv("SPARTAN_DLSS");
        const bool nr = NrPreviewRequested();
        if (!nr && setting && setting[0] == '0') {
            return nullptr;
        }
        const auto directory = ExecutableDirectory();
        if (!std::filesystem::is_regular_file(directory / (nr ? NrBridgeName : BridgeName)) ||
            !std::filesystem::is_regular_file(directory / (nr ? NrNgxName : NgxName))) {
            return nullptr;
        }
        auto* created = new Dlss;
        // The process may end through quick_exit/_exit paths: release NGX first when it can.
        std::atexit([] {
            if (Dlss* live = Get()) {
                live->Shutdown();
            }
        });
        return created;
    }();
    return dlss;
}

Dlss::Dlss() : impl{std::make_unique<Impl>()} {
    const auto directory = ExecutableDirectory();
    const bool nr = NrPreviewRequested();
    impl->module = LoadLibraryW((directory / (nr ? NrBridgeName : BridgeName)).c_str());
    if (nr) {
        auto get = impl->module ? reinterpret_cast<SpartanDlssNrGetApiFn>(
            GetProcAddress(impl->module, "SpartanDlssNrGetApi")) : nullptr;
        impl->nr = get ? get() : nullptr;
        if (!impl->nr || impl->nr->abi != SPARTAN_DLSSNR_BRIDGE_ABI || !impl->nr->sdk_compiled) {
            impl->Disable("NR preview needs the NR SDK build of spartanport_dlssnr.dll");
            return;
        }
    }
    const auto get_api =
        impl->module ? reinterpret_cast<SpartanDlssGetApiFn>(GetProcAddress(impl->module, "SpartanDlssGetApi"))
                     : nullptr;
    impl->api = get_api ? get_api() : nullptr;
    if (!nr && (!impl->api || impl->api->abi != SPARTAN_DLSS_BRIDGE_ABI)) {
        impl->Disable("spartanport_dlss.dll is missing or from another version");
        return;
    }
    // NGX writes its logs and model updates here: beside the saves and shader caches.
    std::filesystem::path data = directory / "dlss";
    if (const char* user = std::getenv("SPARTAN_GPU_USER_DIR"); user && user[0]) {
        data = std::filesystem::path{user} / "dlss";
    }
    std::error_code error;
    std::filesystem::create_directories(data, error);
    const auto configure = nr ? impl->nr->Configure : impl->api->Configure;
    if (!configure(directory.c_str(), data.c_str(), BridgeLog)) {
        impl->Disable("bridge configuration failed");
    }
}

Dlss::~Dlss() {
    Shutdown();
}

void Dlss::AppendInstanceExtensions(std::vector<const char*>& enabled) {
    if (!impl->eligible) {
        return;
    }
    u32 count{};
    const VkExtensionProperties* required{};
    const auto query = impl->nr ? impl->nr->InstanceExtensions : impl->api->InstanceExtensions;
    if (!query(&count, &required)) {
        return impl->Disable("instance extension query failed");
    }
    const auto [result, extensions] = vk::enumerateInstanceExtensionProperties();
    if (result != vk::Result::eSuccess ||
        !CollectExtensions(required, count, extensions, impl->instance_extensions, enabled)) {
        return impl->Disable("required Vulkan instance extensions are missing");
    }
    impl->instance_ready = true;
}

void Dlss::AppendDeviceExtensions(vk::Instance instance, vk::PhysicalDevice physical,
                                  std::vector<const char*>& enabled) {
    if (!impl->eligible || !impl->instance_ready) {
        return;
    }
    const auto properties = physical.getProperties();
    if (properties.vendorID != 0x10de) {
        return impl->Disable("not an NVIDIA GPU");
    }
    u32 count{};
    const VkExtensionProperties* required{};
    const auto query = impl->nr ? impl->nr->DeviceExtensions : impl->api->DeviceExtensions;
    if (!query(instance, physical, &count, &required)) {
        return impl->Disable("this GPU or driver does not support DLSS (GeForce RTX needed)");
    }
    const auto [result, extensions] = physical.enumerateDeviceExtensionProperties();
    if (result != vk::Result::eSuccess ||
        !CollectExtensions(required, count, extensions, impl->device_extensions, enabled)) {
        return impl->Disable("required Vulkan device extensions are missing");
    }
    impl->device_ready = true;
}

void Dlss::Initialize(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device) {
    if (!impl->eligible || !impl->device_ready || impl->initialized) {
        return;
    }
    impl->initialized = true;
    const auto initialize = impl->nr ? impl->nr->Initialize : impl->api->Initialize;
    if (!initialize(instance, physical, device,
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                               VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr)) {
        return impl->Disable("NVIDIA NGX initialization failed (update the GPU driver)");
    }
    impl->available = true;
    std::printf("DLSS: %sready\n", impl->nr ? "NR preview (SR/DLAA excluded), " : "");
}

bool Dlss::Available() const {
    return impl->available && !impl->nr;
}

std::string Dlss::Problem() const {
    return impl->problem;
}

bool Dlss::HasFeature(const FeatureDesc& desc) const {
    return impl->feature && *impl->feature == desc;
}

bool Dlss::CreateFeature(vk::CommandBuffer command, const FeatureDesc& desc) {
    if (!Available()) {
        return false;
    }
    impl->feature.reset();
    const SpartanDlssFeature feature{desc.input_width,  desc.input_height, desc.output_width,
                                desc.output_height, desc.quality,      0,
                                0,                  desc.hdr ? 1 : 0};
    if (!impl->api->CreateFeature(command, &feature)) {
        return false;
    }
    impl->feature = desc;
    return true;
}

bool Dlss::Evaluate(vk::CommandBuffer command, const Frame& frame) {
    if (!Available() || !impl->feature) {
        return false;
    }
    const auto image = [](const Resource& r) {
        const vk::ImageSubresourceRange range{r.aspect, 0, 1, 0, 1};
        return SpartanDlssImage{r.image,
                           r.view,
                           static_cast<VkImageSubresourceRange>(range),
                           static_cast<VkFormat>(r.format),
                           r.width,
                           r.height};
    };
    const SpartanDlssEvaluate parameters{image(frame.color),  image(frame.depth), image(frame.motion),
                                    image(frame.output), frame.jitter_x,     frame.jitter_y,
                                    frame.reset ? 1 : 0, frame.frame_ms,     frame.sharpness};
    return impl->api->Evaluate(command, &parameters) != 0;
}

void Dlss::ReleaseFeature() {
    if (impl->nr) return;
    impl->feature.reset();
    if (impl->api) {
        impl->api->ReleaseFeature();
    }
}

void Dlss::Shutdown() {
    impl->available = false;
    impl->feature.reset();
    if ((impl->api || impl->nr) && impl->initialized) {
        if (impl->nr) impl->nr->Shutdown();
        else impl->api->Shutdown();
        impl->initialized = false;
    }
}

bool Dlss::NrAvailable() const { return impl->available && impl->nr; }
bool Dlss::CreateNrFeature(vk::CommandBuffer command, u32 width, u32 height) {
    return NrAvailable() && impl->nr->CreateFeature(command, width, height);
}
bool Dlss::EvaluateNrPreview(vk::CommandBuffer command, const Resource& color, const Resource& output) {
    if (!NrAvailable()) return false;
    const auto image = [](const Resource& r) {
        return SpartanDlssImage{r.image, r.view, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                                static_cast<VkFormat>(r.format), r.width, r.height};
    };
    SpartanDlssNrFrame frame{};
    frame.color = image(color);
    frame.output = image(output);
    frame.display_encoded = frame.reset = 1;
    frame.settings = {1, 1, 0, 0, 0, 0, 1};
    if (impl->nr->Evaluate(command, &frame)) return true;
    impl->problem = impl->nr->Problem();
    return false;
}
bool Dlss::EvaluateNrScene(vk::CommandBuffer command, const NrFrame& input) {
    if (!NrAvailable()) return false;
    const auto image = [](const Resource& r) {
        return SpartanDlssImage{r.image, r.view, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                                static_cast<VkFormat>(r.format), r.width, r.height};
    };
    SpartanDlssNrFrame frame{};
    frame.color = image(input.color);
    frame.output = image(input.output);
    frame.depth = image(input.depth);
    frame.motion = image(input.motion);
    frame.display_encoded = 1;
    frame.reset = input.reset;
    frame.depth_inverted = 0; // GoW3 hardware depth, near=0, far/clear=1
    frame.settings = {1, 1, 1, 0, 0, 0, 1};
    if (impl->nr->Evaluate(command, &frame)) return true;
    impl->problem = impl->nr->Problem();
    return false;
}
void Dlss::ReleaseNrFeature() {
    if (impl->nr && impl->initialized) impl->nr->ReleaseFeature();
}

#else

struct Dlss::Impl {};
Dlss::Dlss() = default;
Dlss::~Dlss() = default;
Dlss* Dlss::Get() {
    return nullptr;
}
void Dlss::AppendInstanceExtensions(std::vector<const char*>&) {}
void Dlss::AppendDeviceExtensions(vk::Instance, vk::PhysicalDevice, std::vector<const char*>&) {}
void Dlss::Initialize(vk::Instance, vk::PhysicalDevice, vk::Device) {}
void Dlss::Shutdown() {}
bool Dlss::Available() const {
    return false;
}
std::string Dlss::Problem() const {
    return {};
}
bool Dlss::HasFeature(const FeatureDesc&) const {
    return false;
}
bool Dlss::CreateFeature(vk::CommandBuffer, const FeatureDesc&) {
    return false;
}
bool Dlss::Evaluate(vk::CommandBuffer, const Frame&) {
    return false;
}
void Dlss::ReleaseFeature() {}
bool Dlss::NrAvailable() const { return false; }
bool Dlss::CreateNrFeature(vk::CommandBuffer, u32, u32) { return false; }
bool Dlss::EvaluateNrPreview(vk::CommandBuffer, const Resource&, const Resource&) { return false; }
bool Dlss::EvaluateNrScene(vk::CommandBuffer, const NrFrame&) { return false; }
void Dlss::ReleaseNrFeature() {}

#endif

} // namespace Vulkan
