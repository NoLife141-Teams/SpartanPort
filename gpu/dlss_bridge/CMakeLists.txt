# SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), spartanport contributors
# SPDX-License-Identifier: MIT
#
# spartanport_dlss.dll links the NVIDIA DLSS SDK (MSVC only) and is built separately from the port:
#   cmake -S gpu/dlss_bridge -B out/dlss-bridge -G "Visual Studio 17 2022" -A x64 \
#         -DDLSS_SDK_ROOT=<github.com/NVIDIA/DLSS checkout> -DVULKAN_INCLUDE=<Vulkan-Headers include>
#   cmake --build out/dlss-bridge --config Release

cmake_minimum_required(VERSION 3.20)
project(spartanport_dlss LANGUAGES CXX)

set(DLSS_SDK_ROOT "" CACHE PATH "NVIDIA DLSS SDK root (contains include/ and lib/)")
set(VULKAN_INCLUDE "" CACHE PATH "Vulkan-Headers include directory")

if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The DLSS bridge is Windows x64 only")
endif()
if (NOT EXISTS "${DLSS_SDK_ROOT}/include/nvsdk_ngx_vk.h")
    message(FATAL_ERROR "Set DLSS_SDK_ROOT to the NVIDIA DLSS SDK (github.com/NVIDIA/DLSS)")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# nvsdk_ngx_s.lib is the static-CRT (/MT) variant: no Visual C++ runtime install needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

add_library(spartanport_dlss SHARED spartanport_dlss_bridge.cpp spartanport_dlss_bridge.h)
target_include_directories(spartanport_dlss PRIVATE "${DLSS_SDK_ROOT}/include" "${VULKAN_INCLUDE}")
target_link_libraries(spartanport_dlss PRIVATE "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
target_compile_definitions(spartanport_dlss PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

# Independent laboratory module. The public SDK currently lacks NR; keep a
# loadable diagnostic in that case, rather than misidentifying SR as DLSS 5.
option(SPARTAN_ENABLE_DLSSNR "Build the experimental NR adapter with an authorized beta SDK" OFF)
set(DLSS_NR_SDK_ROOT "${DLSS_SDK_ROOT}" CACHE PATH "NR headers package (e.g. RTX Remix ngx_sdk_dlnr); base SDK supplies static NGX library")
if(SPARTAN_ENABLE_DLSSNR AND
   NOT EXISTS "${DLSS_NR_SDK_ROOT}/include/nvsdk_ngx_helpers_dlssnr_vk.h")
    message(FATAL_ERROR "Set DLSS_NR_SDK_ROOT to the NVIDIA NR headers package containing include/nvsdk_ngx_helpers_dlssnr_vk.h; the public SR SDK alone is insufficient")
endif()
add_library(spartanport_dlssnr SHARED spartanport_dlssnr_bridge.cpp spartanport_dlssnr_bridge.h)
target_include_directories(spartanport_dlssnr PRIVATE "${DLSS_SDK_ROOT}/include" "${VULKAN_INCLUDE}")
if(SPARTAN_ENABLE_DLSSNR)
    target_include_directories(spartanport_dlssnr BEFORE PRIVATE "${DLSS_NR_SDK_ROOT}/include")
endif()
target_compile_definitions(spartanport_dlssnr PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
    SPARTAN_ENABLE_DLSSNR=$<BOOL:${SPARTAN_ENABLE_DLSSNR}>)
# Also used by the explicit discovery-only experiment with the public SDK.
target_link_libraries(spartanport_dlssnr PRIVATE "${DLSS_SDK_ROOT}/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib")
