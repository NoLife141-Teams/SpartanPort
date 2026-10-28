// SPDX-License-Identifier: GPL-2.0-or-later
// ABI description only. Never loads the runtime, deserializes cache objects,
// registers a cached SRT walker, creates a Vulkan device, or opens user paths.
#include <cstdio>
#include <cstddef>
#include "shader_recompiler/specialization.h"
using namespace Shader;
int main() {
    RuntimeInfo runtime{}; InfoPersistent info{}; BufferResource buffer{}; VsAttribSpecialization attrib{};
    const auto offset=[](const auto& owner,const auto& field) {
        return reinterpret_cast<const char*>(&field)-reinterpret_cast<const char*>(&owner);
    };
    std::printf("{\"size_t\":%zu,\"bindings\":%zu,\"runtime\":%zu,\"info\":%zu,"
                "\"vs_attrib\":%zu,\"buffer_spec\":%zu,\"image_spec\":%zu,\"fmask_spec\":%zu,\"sampler_spec\":%zu,"
                "\"fetch_object\":%zu,\"fetch_attribute\":%zu,\"buffer_resource\":%zu,",
        sizeof(size_t),sizeof(Backend::Bindings),sizeof(RuntimeInfo),sizeof(InfoPersistent),
        sizeof(VsAttribSpecialization),sizeof(BufferSpecialization),sizeof(ImageSpecialization),
        sizeof(FMaskSpecialization),sizeof(SamplerSpecialization),sizeof(Gcn::FetchShaderData),
        sizeof(Gcn::VertexAttribute),sizeof(BufferResource));
    std::printf("\"runtime_sw\":%td,\"runtime_hw\":%td,\"emulate_depth\":%td,\"clip_disabled\":%td,\"motion\":%td,",
        offset(runtime,runtime.sw_stage),offset(runtime,runtime.hw_stage),
        offset(runtime,runtime.hw.vs.emulate_depth_negative_one_to_one),offset(runtime,runtime.hw.vs.clip_disable),
        offset(runtime,runtime.hw.vs.motion_vectors));
    std::printf("\"pgm_hash\":%td,\"info_hw\":%td,\"info_sw\":%td,\"buffers_data\":%td,",
        offset(info,info.pgm_hash),offset(info,info.hw_stage),offset(info,info.sw_stage),
        reinterpret_cast<const char*>(info.buffers.data())-reinterpret_cast<const char*>(&info));
    std::printf("\"buffer_kind\":%td,\"buffer_written\":%td,\"sharp_offsets\":%td,\"sharp_mask\":%td,",
        offset(buffer,buffer.buffer_type),offset(buffer,buffer.is_written),
        offset(buffer,buffer.sharp_fetch.offsets),offset(buffer,buffer.sharp_fetch.load_mask));
    std::printf("\"attrib_divisor\":%td,\"attrib_class\":%td,\"attrib_select\":%td}\n",
        offset(attrib,attrib.divisor),offset(attrib,attrib.num_class),offset(attrib,attrib.dst_select));
}
