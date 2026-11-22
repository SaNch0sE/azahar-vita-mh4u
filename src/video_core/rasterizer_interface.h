// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <functional>
#include <utility>
#include <vector>
#include "common/common_types.h"
#include "video_core/pica/output_vertex.h"

namespace Pica {
struct DisplayTransferConfig;
struct MemoryFillConfig;
} // namespace Pica

namespace VideoCore {

enum class LoadCallbackStage {
    Prepare,
    Preload,
    Decompile,
    Build,
    Complete,
};
using DiskResourceLoadCallback =
    std::function<void(LoadCallbackStage, std::size_t, std::size_t, const std::string&)>;

class RasterizerInterface {
public:
    virtual ~RasterizerInterface() = default;

    /// Queues the primitive formed by the given vertices for rendering
    virtual void AddTriangle(const Pica::OutputVertex& v0, const Pica::OutputVertex& v1,
                             const Pica::OutputVertex& v2) = 0;

    /// Draw the current batch of triangles
    virtual void DrawTriangles() = 0;

    /// Notify rasterizer that all caches should be flushed to 3DS memory
    virtual void FlushAll() = 0;

    /// Notify rasterizer that any caches of the specified region should be flushed to 3DS memory
    virtual void FlushRegion(PAddr addr, u32 size) = 0;

    /// Notify rasterizer that any caches of the specified region should be invalidated
    virtual void InvalidateRegion(PAddr addr, u32 size) = 0;

    /// Notify rasterizer that any caches of the specified region should be flushed to 3DS memory
    /// and invalidated
    virtual void FlushAndInvalidateRegion(PAddr addr, u32 size) = 0;

    /// Removes as much state as possible from the rasterizer in preparation for a save/load state
    virtual void ClearAll(bool flush) = 0;

    /// Attempt to use a faster method to perform a display transfer with is_texture_copy = 0
    virtual bool AccelerateDisplayTransfer(const Pica::DisplayTransferConfig&) {
        return false;
    }

    /// Attempt to use a faster method to perform a display transfer with is_texture_copy = 1
    virtual bool AccelerateTextureCopy(const Pica::DisplayTransferConfig&) {
        return false;
    }

    /// Attempt to use a faster method to fill a region
    virtual bool AccelerateFill(const Pica::MemoryFillConfig&) {
        return false;
    }

    /// Copies size bytes from src to dst, both physically contiguous, on the GPU (GSP's
    /// RequestDma, split into physical runs)
    virtual bool AccelerateDma([[maybe_unused]] PAddr src, [[maybe_unused]] PAddr dst,
                               [[maybe_unused]] u32 size) {
        return false;
    }

    /// Attempt to draw using hardware shaders
    virtual bool AccelerateDrawBatch([[maybe_unused]] bool is_indexed) {
        return false;
    }

    /// Whether AccelerateDrawBatch takes draws with a geometry shader too (the sgxd renderer's
    /// stream-out passes); the others run the geometry shader on the CPU.
    virtual bool AcceleratesGeometryShaders() const {
        return false;
    }

    /// Immediate mode: one vertex's input attributes, as the PICA formed them. Returning true
    /// takes the vertex off the CPU vertex path; the rasterizer draws the vertices it collected
    /// at FlushImmediate.
    virtual bool AccelerateImmediateVertex([[maybe_unused]] const Pica::AttributeBuffer& input) {
        return false;
    }

    /// Draws the immediate-mode vertices collected since the last flush. The PICA calls it
    /// before any register write that is not immediate-mode data.
    virtual void FlushImmediate() {}

    /// The PICA is about to raise P3D. From then on the guest may overwrite the vertex and
    /// index buffers of the draws before it.
    virtual void NotifyP3D() {}

    /// A point in the GPU work the rasterizer submitted, which a posted interrupt waits for
    /// (GPU::PostToEmulation). Counts compare with wrap-around; a false flag skips its count.
    struct GpuFence {
        bool twod{};      // the PTLA ops (fills, DMA) up to twod_count
        bool ta{};        // the renders up to render_count, their TA only (vertices read)
        bool render{};    // the renders up to render_count, complete
        u32 twod_count{};
        u32 render_count{};

        [[nodiscard]] bool Any() const {
            return twod || ta || render;
        }
    };
    /// Everything submitted so far. The GPU runs it after the call returns.
    virtual GpuFence GpuFenceAll() const {
        return {};
    }
    /// The renders submitted so far up to their TA: after it, the guest may overwrite their
    /// vertex and index buffers (P3D).
    virtual GpuFence GpuFenceVertices() const {
        return {};
    }
    /// A display transfer's completion (PPF): GpuFenceAll, or less where the renderer knows the
    /// guest will not touch the output before it is written.
    virtual GpuFence GpuFenceTransfer() const {
        return GpuFenceAll();
    }
    /// The render thread's time spent waiting for the GPU so far, in microseconds (diagnostics
    /// builds; 0 otherwise).
    virtual u64 GpuWaitUs() const {
        return 0;
    }

    /// The guest ranges the GPU read from memory the CPU caches since the last call (vertex
    /// and index buffers), as (address, size). The GPU hands them to KeepUncached, so that a
    /// buffer drawn from each frame is not cleaned each frame.
    virtual std::vector<std::pair<PAddr, u32>> TakeCachedReads() {
        return {};
    }

    virtual void LoadDefaultDiskResources(
        [[maybe_unused]] const std::atomic_bool& stop_loading,
        [[maybe_unused]] const DiskResourceLoadCallback& callback) {}

    virtual void SwitchDiskResources([[maybe_unused]] u64 title_id) {}

    static void SetSwitchDiskResourcesCallback(const DiskResourceLoadCallback& callback) {
        switch_disk_resources_callback = callback;
    }

    void SetAccurateMul(bool accurate_mul_) {
        accurate_mul = accurate_mul_;
    }

protected:
    bool accurate_mul = false;

    // Rasterizer gets destroyed on reboot, so make the callback
    // static until a better solution is found.
    static DiskResourceLoadCallback switch_disk_resources_callback;
};
} // namespace VideoCore
