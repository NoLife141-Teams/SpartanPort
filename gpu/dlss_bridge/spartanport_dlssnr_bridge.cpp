// SPDX-License-Identifier: MIT
// Experimental NGX NR adapter. API names follow NVIDIA's vk_gltf_renderer
// src/dlss.cpp (USE_DLSSNR), adapted to the RTX Remix ngx_sdk_dlnr v2 headers.
// NR code is explicitly opt-in; the established SR/DLAA bridge is unchanged.
#include "spartanport_dlssnr_bridge.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>

#include <nvsdk_ngx_vk.h>
#if SPARTAN_ENABLE_DLSSNR
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_helpers_vk.h>
#include <nvsdk_ngx_helpers_dlssnr_vk.h>
#endif

namespace {
constexpr char ProjectId[] = "42359704-c9f3-4806-9fb5-d469000fdb8a";
constexpr char EngineVersion[] = "spartanport-windows-nr-laboratory";
SpartanDlssLogFn logger{};
std::string problem;
std::wstring dll_directory, data_directory;

int32_t Fail(const char* message) {
    problem = message;
    if (logger) logger(1, problem.c_str());
    return 0;
}

bool Unit(float value) { return std::isfinite(value) && value >= 0 && value <= 1; }
bool Image(const SpartanDlssImage& image) {
    return image.image && image.view && image.width && image.height &&
           image.range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT &&
           image.range.levelCount == 1 && image.range.layerCount == 1;
}

#if SPARTAN_ENABLE_DLSSNR
const wchar_t* dll_path{};
NVSDK_NGX_FeatureCommonInfo common{};
NVSDK_NGX_FeatureDiscoveryInfo discovery{};
NVSDK_NGX_Parameter* parameters{};
NVSDK_NGX_Handle* feature{};
VkDevice device{};
uint32_t width{}, height{};
bool initialized{}, configured{};

bool Check(const char* operation, NVSDK_NGX_Result result) {
    if (NVSDK_NGX_SUCCEED(result)) return true;
    std::array<char, 256> message{};
    std::snprintf(message.data(), message.size(), "%s failed: 0x%08x", operation,
                  static_cast<unsigned>(result));
    Fail(message.data());
    return false;
}
#endif

int32_t Configure(const wchar_t* dll, const wchar_t* data, SpartanDlssLogFn log) {
    logger = log;
#if SPARTAN_ENABLE_DLSSNR
    if (configured) return Fail("NR already configured; shut down before reconfiguration");
    if (!dll || !data) return Fail("NR needs runtime and writable data directories");
    dll_directory = dll;
    data_directory = data;
    dll_path = dll_directory.c_str();
    common.PathListInfo = {&dll_path, 1};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.FeatureID = NVSDK_NGX_Feature_DLSSNR;
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc = {ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, EngineVersion};
    discovery.ApplicationDataPath = data_directory.c_str();
    discovery.FeatureInfo = &common;
    configured = true;
    problem.clear();
    return 1;
#else
    return Fail("DLSS-NR beta SDK missing at build time (public DLSS SR SDK is insufficient)");
#endif
}

int32_t InstanceExtensions(uint32_t* count, const VkExtensionProperties** extensions) {
    if (!count || !extensions) return Fail("Invalid NR instance extension arguments");
    *count = 0;
    *extensions = nullptr;
#if SPARTAN_ENABLE_DLSSNR
    if (!configured) return Fail("NR not configured");
    VkExtensionProperties* required{};
    if (!Check("NR instance extensions", NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
                   &discovery, count, &required))) return 0;
    *extensions = required;
    return 1;
#else
    return Fail("NR SDK not compiled");
#endif
}

int32_t DeviceExtensions(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                         const VkExtensionProperties** extensions) {
    if (!count || !extensions) return Fail("Invalid NR device extension arguments");
    *count = 0;
    *extensions = nullptr;
#if SPARTAN_ENABLE_DLSSNR
    if (!configured || !instance || !physical) return Fail("NR needs a configured Vulkan GPU");
    NVSDK_NGX_FeatureRequirement support{};
    if (!Check("NR feature requirements", NVSDK_NGX_VULKAN_GetFeatureRequirements(
                   instance, physical, &discovery, &support))) return 0;
    if (support.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported)
        return Fail("NR unsupported by this GPU, driver or application registration");
    VkExtensionProperties* required{};
    if (!Check("NR device extensions", NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                   instance, physical, &discovery, count, &required))) return 0;
    *extensions = required;
    return 1;
#else
    return Fail("NR SDK not compiled");
#endif
}

int32_t Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice vk_device,
                   PFN_vkGetInstanceProcAddr get_instance, PFN_vkGetDeviceProcAddr get_device) {
#if SPARTAN_ENABLE_DLSSNR
    if (!configured || initialized || !instance || !physical || !vk_device)
        return Fail("Invalid NR initialization state");
    if (!Check("NR NGX initialization", NVSDK_NGX_VULKAN_Init_with_ProjectID(
                   ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, EngineVersion, data_directory.c_str(),
                   instance, physical, vk_device, get_instance, get_device, &common))) return 0;
    initialized = true;
    device = vk_device;
    return Check("NR capability parameters", NVSDK_NGX_VULKAN_GetCapabilityParameters(&parameters));
#else
    return Fail("NR SDK not compiled");
#endif
}

void ReleaseFeature() {
#if SPARTAN_ENABLE_DLSSNR
    if (feature) NVSDK_NGX_VULKAN_ReleaseFeature(feature);
    feature = nullptr;
    width = height = 0;
#endif
}

int32_t CreateFeature(VkCommandBuffer command, uint32_t w, uint32_t h) {
#if SPARTAN_ENABLE_DLSSNR
    if (!initialized || !parameters || !command || !w || !h)
        return Fail("Invalid NR feature creation state/extent");
    ReleaseFeature();
    NVSDK_NGX_DLSSNR_Create_Params settings{w, h};
    const auto result = NGX_VULKAN_CREATE_DLSSNR_EXT1(
        device, command, 1, 1, &feature, parameters, &settings);
    if (!Check("NR feature creation", result) || !feature) {
        ReleaseFeature();
        return 0;
    }
    width = w;
    height = h;
    return 1;
#else
    return Fail("NR SDK not compiled");
#endif
}

int32_t Evaluate(VkCommandBuffer command, const SpartanDlssNrFrame* frame) {
    if (!command || !frame || !Image(frame->color) || !Image(frame->output))
        return Fail("NR requires valid, single-sample color and output images");
    const auto& f = *frame;
    if (f.display_encoded != 1) return Fail("NR requires an explicit post-tonemap LDR input contract");
    if (f.color.image == f.output.image || f.color.width != f.output.width ||
        f.color.height != f.output.height || f.output.format != VK_FORMAT_R32G32B32A32_SFLOAT)
        return Fail("NR requires a separate RGBA32F output at the display extent");
    if (f.color.format != VK_FORMAT_R8G8B8A8_UNORM &&
        f.color.format != VK_FORMAT_B8G8R8A8_UNORM &&
        f.color.format != VK_FORMAT_R32G32B32A32_SFLOAT)
        return Fail("NR input must contain display-encoded LDR; linear HDR/sRGB views are rejected");
    const auto& s = f.settings;
    if (!Unit(s.intensity) || !Unit(s.local_tone) || !Unit(s.local_structure) ||
        !Unit(s.global_tone) || !Unit(s.skin_structure)) return Fail("Invalid NR strengths");
    if (s.global_tone != 0) return Fail("Global tone is not exposed by the RTX Remix NR v2 contract");
    const bool auxiliary = f.depth.image || f.motion.image;
    if (auxiliary && (!Image(f.depth) || !Image(f.motion) ||
                      f.depth.width != f.motion.width || f.depth.height != f.motion.height))
        return Fail("NR depth/motion must form a matching pair");
    if (!auxiliary && s.local_structure > 0)
        return Fail("NR local structure requires aligned depth and motion in this adapter");
#if SPARTAN_ENABLE_DLSSNR
    if (!feature || !parameters || width != f.color.width || height != f.color.height)
        return Fail("NR feature absent or wrong extent");
    const auto wrap = [](const SpartanDlssImage& image, bool writable) {
        return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, image.range,
            image.format, image.width, image.height, writable);
    };
    auto color = wrap(f.color, false), output = wrap(f.output, true);
    NVSDK_NGX_Resource_VK depth{}, motion{};
    NVSDK_NGX_VK_DLSSNR_Eval_Params p{};
    p.pInColor = &color;
    p.pInOutput = &output;
    p.InColorSubrectSize = p.InOutputSubrectSize = {width, height};
    if (auxiliary) {
        depth = wrap(f.depth, false);
        motion = wrap(f.motion, false);
        p.pInDepth = &depth;
        p.pInMVec = &motion;
        p.InDepthSubrectSize = p.InMVecSubrectSize = {f.depth.width, f.depth.height};
        p.InMVecScaleX = float(width) / float(f.motion.width);
        p.InMVecScaleY = float(height) / float(f.motion.height);
    }
    p.InEnabled = 1;
    p.InReset = f.reset;
    p.InDepthInverted = f.depth_inverted;
    p.InIntensity = s.intensity;
    p.InLocalToneStrength = s.local_tone;
    p.InLocalStructureStrength = s.local_structure;
    p.InSkinStructureStrength = s.skin_structure;
    p.InStyle = s.style;
    p.InUseAutoMask = s.auto_mask;
    return Check("NR evaluation", NGX_VULKAN_EVALUATE_DLSSNR_EXT(command, feature, parameters, &p));
#else
    return Fail("NR SDK not compiled");
#endif
}

void Shutdown() {
    ReleaseFeature();
#if SPARTAN_ENABLE_DLSSNR
    if (parameters) NVSDK_NGX_VULKAN_DestroyParameters(parameters);
    parameters = nullptr;
    if (initialized) NVSDK_NGX_VULKAN_Shutdown1(device);
    initialized = configured = false;
    device = VK_NULL_HANDLE;
#endif
}

const char* Problem() { return problem.c_str(); }
const SpartanDlssNrApi api{SPARTAN_DLSSNR_BRIDGE_ABI, SPARTAN_ENABLE_DLSSNR, Configure,
    InstanceExtensions, DeviceExtensions, Initialize, CreateFeature, Evaluate, ReleaseFeature,
    Shutdown, Problem};
}

extern "C" __declspec(dllexport) const SpartanDlssNrApi* SpartanDlssNrGetApi() { return &api; }

extern "C" __declspec(dllexport) void SpartanDlssNrDiscover(
    VkInstance instance, VkPhysicalDevice physical, const wchar_t* dll_directory,
    const wchar_t* data_directory, SpartanDlssNrDiscovery* result) {
    if (!result || !instance || !physical || !dll_directory || !data_directory) return;
    *result = {};
    result->feature_id = 18;
    result->support = ~uint32_t(0); // not queried successfully, not "supported"
    NVSDK_NGX_FeatureCommonInfo info{};
    info.PathListInfo = {&dll_directory, 1};
    NVSDK_NGX_FeatureDiscoveryInfo request{};
    request.SDKVersion = NVSDK_NGX_Version_API;
    request.FeatureID = static_cast<NVSDK_NGX_Feature>(result->feature_id);
    request.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    request.Identifier.v.ProjectDesc = {ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, EngineVersion};
    request.ApplicationDataPath = data_directory;
    request.FeatureInfo = &info;
    NVSDK_NGX_FeatureRequirement support{};
    result->requirements_result = NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &request, &support);
    if (NVSDK_NGX_SUCCEED(static_cast<NVSDK_NGX_Result>(result->requirements_result)))
        result->support = static_cast<uint32_t>(support.FeatureSupported);
    VkExtensionProperties* extensions{};
    result->instance_extensions_result = NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(
        &request, &result->instance_extensions_count, &extensions);
    result->device_extensions_result = NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
        instance, physical, &request, &result->device_extensions_count, &extensions);
}
