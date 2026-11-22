// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_external.h"
#include "video_core/renderer_gxm/gxm_rasterizer.h"

namespace GxmRenderer {

namespace {

const char* TopologyName(Pica::PipelineRegs::TriangleTopology topology) {
    switch (topology) {
    case Pica::PipelineRegs::TriangleTopology::List:
        return "list";
    case Pica::PipelineRegs::TriangleTopology::Strip:
        return "strip";
    case Pica::PipelineRegs::TriangleTopology::Fan:
        return "fan";
    case Pica::PipelineRegs::TriangleTopology::Shader:
        return "shader";
    }
    return "unknown";
}

} // Anonymous namespace

RasterizerGxm::RasterizerGxm(Pica::PicaCore& pica_) : pica{pica_} {}

RasterizerGxm::~RasterizerGxm() = default;

void RasterizerSgxd::AddTriangle(const Pica::OutputVertex&, const Pica::OutputVertex&,
                                const Pica::OutputVertex&) {
    // The CPU vertex path. The default attribute index is 0xF while immediate-mode vertices
    // are submitted.
    const auto& pipeline = pica.regs.internal.pipeline;
    UNREACHABLE_MSG("sgxd: AddTriangle, a draw on the CPU vertex path: geometry shader {}, "
                    "topology {}, {} vertices, immediate mode {}",
                    pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes ? "on" : "off",
                    TopologyName(pipeline.triangle_topology), pipeline.num_vertices,
                    pipeline.vs_default_attributes_setup.index == 0xF ? "yes" : "no");
}

void RasterizerGxm::DrawTriangles() {
    // Reached at the end of every CPU-path draw, even one with no triangle.
    const auto& pipeline = pica.regs.internal.pipeline;
    UNREACHABLE_MSG("sgxd: DrawTriangles, a draw on the CPU vertex path: geometry shader {}, "
                    "topology {}, {} vertices",
                    pipeline.use_gs == Pica::PipelineRegs::UseGS::Yes ? "on" : "off",
                    TopologyName(pipeline.triangle_topology), pipeline.num_vertices);
}

// Nothing is cached yet, so there is nothing to write back or drop.

void RasterizerSgxd::FlushAll() {}

void RasterizerSgxd::FlushRegion(PAddr, u32) {}

void RasterizerSgxd::InvalidateRegion(PAddr, u32) {}

void RasterizerSgxd::FlushAndInvalidateRegion(PAddr, u32) {}

void RasterizerSgxd::ClearAll(bool) {}

bool RasterizerGxm::AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) {
    UNREACHABLE_MSG("GXM: AccelerateDisplayTransfer {}, format {} to {}, flags {:#x}",
                    config.DebugName(), static_cast<u32>(config.input_format.Value()),
                    static_cast<u32>(config.output_format.Value()), config.flags);
}

bool RasterizerGxm::AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) {
    UNREACHABLE_MSG("GXM: AccelerateTextureCopy from {:#x} to {:#x}, {} bytes, input "
                    "width {} gap {}, output width {} gap {}",
                    config.GetPhysicalInputAddress(), config.GetPhysicalOutputAddress(),
                    config.texture_copy.size, config.texture_copy.input_width.Value(),
                    config.texture_copy.input_gap.Value(),
                    config.texture_copy.output_width.Value(),
                    config.texture_copy.output_gap.Value());
}

bool RasterizerGxm::AccelerateFill(const Pica::MemoryFillConfig& config) {
    UNREACHABLE_MSG("GXM: AccelerateFill {:#x}-{:#x} ({}) with {:#x}, {}-bit",
                    config.GetStartAddress(), config.GetEndAddress(),
                    config.IsVRAM() ? "VRAM" : "FCRAM", config.value_32bit,
                    config.fill_32bit ? 32 : (config.fill_24bit ? 24 : 16));
}

bool RasterizerSgxd::AccelerateDrawBatch(bool is_indexed) {
    const auto& pipeline = pica.regs.internal.pipeline;
    UNREACHABLE_MSG("GXM: AccelerateDrawBatch, {} draw, topology {}, {} vertices",
                    is_indexed ? "indexed" : "non-indexed",
                    TopologyName(pipeline.triangle_topology), pipeline.num_vertices);
}

} // namespace GxmRenderer
