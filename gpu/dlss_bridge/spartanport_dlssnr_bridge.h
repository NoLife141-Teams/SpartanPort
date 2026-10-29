// SPDX-License-Identifier: MIT
#pragma once

#include "spartanport_dlss_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPARTAN_DLSSNR_BRIDGE_ABI 1

// Laboratory-only API, deliberately separate from the established SR/DLAA ABI.
// Do not initialize this module alongside the SR bridge in the same process.
typedef struct SpartanDlssNrSettings {
    float intensity;
    float local_tone;
    float local_structure;
    float global_tone; // reserved, must be 0 with the RTX Remix NR v2 contract
    float skin_structure;
    uint32_t style;
    int32_t auto_mask;
} SpartanDlssNrSettings;

typedef struct SpartanDlssNrFrame {
    // Display-encoded LDR [0,1], AFTER tone mapping. No linear HDR or sRGB view
    // that would decode these values again. Input/output must not alias.
    SpartanDlssImage color;
    SpartanDlssImage output; // RGBA32F storage, same extent as color
    SpartanDlssImage depth;  // optional, paired with motion
    SpartanDlssImage motion; // optional, current -> previous render pixels
    int32_t display_encoded; // must be 1: caller attests post-tonemap LDR [0,1]
    int32_t reset;
    int32_t depth_inverted;
    SpartanDlssNrSettings settings;
} SpartanDlssNrFrame;

typedef struct SpartanDlssNrApi {
    uint32_t abi;
    int32_t sdk_compiled; // 0: no NR Create/Evaluate backend; discovery remains available
    int32_t (*Configure)(const wchar_t* dll_directory, const wchar_t* data_directory,
                         SpartanDlssLogFn log);
    int32_t (*InstanceExtensions)(uint32_t* count, const VkExtensionProperties** extensions);
    int32_t (*DeviceExtensions)(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                                const VkExtensionProperties** extensions);
    int32_t (*Initialize)(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                          PFN_vkGetInstanceProcAddr get_instance_proc,
                          PFN_vkGetDeviceProcAddr get_device_proc);
    int32_t (*CreateFeature)(VkCommandBuffer command, uint32_t width, uint32_t height);
    int32_t (*Evaluate)(VkCommandBuffer command, const SpartanDlssNrFrame* frame);
    // Caller waits for all submitted GPU work before these operations/recreation.
    void (*ReleaseFeature)(void);
    void (*Shutdown)(void);
    const char* (*Problem)(void); // valid until the next API call
} SpartanDlssNrApi;

typedef const SpartanDlssNrApi* (*SpartanDlssNrGetApiFn)(void);

// Explicit public-NGX discovery experiment. Feature 18 is reserved in the
// public SR header, named DLSSNR by the NVIDIA RTX Remix NR definitions. This does
// not supply the missing beta helpers or prove creation/evaluation support.
typedef struct SpartanDlssNrDiscovery {
    uint32_t feature_id;
    uint32_t requirements_result;
    uint32_t support;
    uint32_t instance_extensions_result;
    uint32_t instance_extensions_count;
    uint32_t device_extensions_result;
    uint32_t device_extensions_count;
} SpartanDlssNrDiscovery;
typedef void (*SpartanDlssNrDiscoverFn)(VkInstance instance, VkPhysicalDevice physical,
    const wchar_t* dll_directory, const wchar_t* data_directory, SpartanDlssNrDiscovery* result);

#ifdef __cplusplus
}
#endif
