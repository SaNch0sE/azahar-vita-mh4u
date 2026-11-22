// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "video_core/rasterizer_interface.h"

namespace Pica {
class PicaCore;
}

namespace GxmRenderer {

/**
 * The PICA on GXM. There is no CPU fallback: every operation either runs on the GPU or stops
 * the emulator with a message naming what is unsupported, so the Accelerate* hooks return
 * true or crash, and the CPU vertex path, which ends in AddTriangle, always crashes.
 */
class RasterizerGxm : public VideoCore::RasterizerInterface {
public:
    explicit RasterizerGxm(Pica::PicaCore& pica);
    ~RasterizerGxm() override;

    void AddTriangle(const Pica::OutputVertex& v0, const Pica::OutputVertex& v1,
                     const Pica::OutputVertex& v2) override;
    void DrawTriangles() override;
    void FlushAll() override;
    void FlushRegion(PAddr addr, u32 size) override;
    void InvalidateRegion(PAddr addr, u32 size) override;
    void FlushAndInvalidateRegion(PAddr addr, u32 size) override;
    void ClearAll(bool flush) override;
    bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateTextureCopy(const Pica::DisplayTransferConfig& config) override;
    bool AccelerateFill(const Pica::MemoryFillConfig& config) override;
    bool AccelerateDrawBatch(bool is_indexed) override;

private:
    Pica::PicaCore& pica;
};

} // namespace GxmRenderer
