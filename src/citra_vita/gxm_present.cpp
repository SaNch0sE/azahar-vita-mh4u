// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdlib>
#include <vector>

#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include "citra_vita/gxm_present.h"
#include "common/assert.h"
#include "common/logging/log.h"

namespace VitaFrontend::GxmPresent {

namespace {

constexpr u32 DisplayBuffers = 2;
/// A notice's frame: a few thousand of ImGui's vertices and their indices.
constexpr u32 FrameArenaSize = 256 * 1024;
/// The shader patcher's memory, fixed: it holds the notice's two programs.
constexpr u32 PatcherBufferSize = 64 * 1024;
constexpr u32 PatcherUsseSize = 64 * 1024;

enum class MapKind { Regular, VertexUsse, FragmentUsse };

struct Block {
    SceUID uid;
    void* addr;
    MapKind kind;
};

struct State {
    bool initialized{};
    /// Every memblock taken, mapped for the GPU as its kind says.
    std::vector<Block> blocks;

    void* context_host_mem{};
    SceGxmContext* context{};
    SceGxmRenderTarget* render_target{};
    void* display_mem[DisplayBuffers]{};
    SceGxmColorSurface display_surface[DisplayBuffers]{};
    SceGxmSyncObject* sync[DisplayBuffers]{};
    SceGxmShaderPatcher* patcher{};

    u8* arena{};
    u32 arena_used{};
    u32 frame_index{};
    u32 back{}; ///< the display buffer being drawn
};

State g;

/// A memblock of `type`, mapped for the GPU as `kind` (the USSE kinds give their offset), until
/// Shutdown.
void* Alloc(SceKernelMemBlockType type, u32 size, MapKind kind = MapKind::Regular,
            u32* usse_offset = nullptr) {
    const u32 align = type == SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW ? 256 * 1024 : 4 * 1024;
    size = (size + align - 1) & ~(align - 1);
    const SceUID uid = sceKernelAllocMemBlock("azahar-gxm", type, size, nullptr);
    if (uid < 0) {
        LOG_CRITICAL(Frontend, "GXM: a memblock of {} KiB was refused: {:#010x}", size >> 10,
                     static_cast<u32>(uid));
        return nullptr;
    }
    void* addr = nullptr;
    sceKernelGetMemBlockBase(uid, &addr);
    int mapped = 0;
    switch (kind) {
    case MapKind::Regular:
        mapped = sceGxmMapMemory(addr, size,
                                 static_cast<SceGxmMemoryAttribFlags>(SCE_GXM_MEMORY_ATTRIB_READ |
                                                                      SCE_GXM_MEMORY_ATTRIB_WRITE));
        break;
    case MapKind::VertexUsse:
        mapped = sceGxmMapVertexUsseMemory(addr, size, usse_offset);
        break;
    case MapKind::FragmentUsse:
        mapped = sceGxmMapFragmentUsseMemory(addr, size, usse_offset);
        break;
    }
    if (mapped < 0) {
        LOG_CRITICAL(Frontend, "GXM: {} KiB could not be mapped: {:#010x}", size >> 10,
                     static_cast<u32>(mapped));
        sceKernelFreeMemBlock(uid);
        return nullptr;
    }
    g.blocks.push_back({uid, addr, kind});
    return addr;
}

void* Uncached(u32 size) {
    return Alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, size);
}

struct DisplayData {
    void* addr;
};

void DisplayCallback(const void* data) {
    const auto* display = static_cast<const DisplayData*>(data);
    SceDisplayFrameBuf fb{};
    fb.size = sizeof(fb);
    fb.base = display->addr;
    fb.pitch = DisplayWidth;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = DisplayWidth;
    fb.height = DisplayHeight;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    // Returning before the flip lands would let the GPU write the buffer still on screen.
    sceDisplayWaitVblankStart();
}

void* PatcherHostAlloc(void*, u32 size) {
    return std::malloc(size);
}
void PatcherHostFree(void*, void* mem) {
    std::free(mem);
}

/// The context, the render target, the display buffers, the patcher and the frame arena.
bool BringUp() {
    // The context and its rings, at the SDK's default sizes.
    g.context_host_mem = std::malloc(SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE);
    u32 fragment_ring_offset = 0;
    SceGxmContextParams context{};
    context.hostMem = g.context_host_mem;
    context.hostMemSize = SCE_GXM_MINIMUM_CONTEXT_HOST_MEM_SIZE;
    context.vdmRingBufferMem = Uncached(SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE);
    context.vdmRingBufferMemSize = SCE_GXM_DEFAULT_VDM_RING_BUFFER_SIZE;
    context.vertexRingBufferMem = Uncached(SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE);
    context.vertexRingBufferMemSize = SCE_GXM_DEFAULT_VERTEX_RING_BUFFER_SIZE;
    context.fragmentRingBufferMem = Uncached(SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE);
    context.fragmentRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_RING_BUFFER_SIZE;
    context.fragmentUsseRingBufferMem = Alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                                              SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE,
                                              MapKind::FragmentUsse, &fragment_ring_offset);
    context.fragmentUsseRingBufferMemSize = SCE_GXM_DEFAULT_FRAGMENT_USSE_RING_BUFFER_SIZE;
    context.fragmentUsseRingBufferOffset = fragment_ring_offset;
    if (context.vdmRingBufferMem == nullptr || context.vertexRingBufferMem == nullptr ||
        context.fragmentRingBufferMem == nullptr || context.fragmentUsseRingBufferMem == nullptr) {
        return false;
    }
    if (sceGxmCreateContext(&context, &g.context) < 0) {
        LOG_CRITICAL(Frontend, "GXM: the context could not be created");
        g.context = nullptr;
        return false;
    }

    SceGxmRenderTargetParams target{};
    target.width = DisplayWidth;
    target.height = DisplayHeight;
    target.scenesPerFrame = 1;
    target.multisampleMode = SCE_GXM_MULTISAMPLE_NONE;
    target.driverMemBlock = static_cast<SceUID>(-1);
    if (sceGxmCreateRenderTarget(&target, &g.render_target) < 0) {
        LOG_CRITICAL(Frontend, "GXM: the render target could not be created");
        g.render_target = nullptr;
        return false;
    }

    // What scans out has to be CDRAM.
    for (u32 i = 0; i < DisplayBuffers; i++) {
        g.display_mem[i] =
            Alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, DisplayWidth * DisplayHeight * 4);
        if (g.display_mem[i] == nullptr ||
            sceGxmColorSurfaceInit(&g.display_surface[i], SCE_GXM_COLOR_FORMAT_A8B8G8R8,
                                   SCE_GXM_COLOR_SURFACE_LINEAR, SCE_GXM_COLOR_SURFACE_SCALE_NONE,
                                   SCE_GXM_OUTPUT_REGISTER_SIZE_32BIT, DisplayWidth, DisplayHeight,
                                   DisplayWidth, g.display_mem[i]) < 0 ||
            sceGxmSyncObjectCreate(&g.sync[i]) < 0) {
            LOG_CRITICAL(Frontend, "GXM: display buffer {} could not be set up", i);
            return false;
        }
    }

    u32 vertex_usse_offset = 0;
    u32 fragment_usse_offset = 0;
    SceGxmShaderPatcherParams patcher{};
    patcher.hostAllocCallback = PatcherHostAlloc;
    patcher.hostFreeCallback = PatcherHostFree;
    patcher.bufferMem = Uncached(PatcherBufferSize);
    patcher.bufferMemSize = PatcherBufferSize;
    patcher.vertexUsseMem = Alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, PatcherUsseSize,
                                  MapKind::VertexUsse, &vertex_usse_offset);
    patcher.vertexUsseMemSize = PatcherUsseSize;
    patcher.vertexUsseOffset = vertex_usse_offset;
    patcher.fragmentUsseMem = Alloc(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, PatcherUsseSize,
                                    MapKind::FragmentUsse, &fragment_usse_offset);
    patcher.fragmentUsseMemSize = PatcherUsseSize;
    patcher.fragmentUsseOffset = fragment_usse_offset;
    if (patcher.bufferMem == nullptr || patcher.vertexUsseMem == nullptr ||
        patcher.fragmentUsseMem == nullptr) {
        return false;
    }
    if (sceGxmShaderPatcherCreate(&patcher, &g.patcher) < 0) {
        LOG_CRITICAL(Frontend, "GXM: the shader patcher could not be created");
        g.patcher = nullptr;
        return false;
    }

    g.arena = static_cast<u8*>(Uncached(FrameArenaSize));
    return g.arena != nullptr;
}

} // Anonymous namespace

bool Initialize() {
    ASSERT_MSG(!g.initialized, "GXM brought up twice");
    SceGxmInitializeParams init{};
    init.displayQueueMaxPendingCount = DisplayBuffers - 1;
    init.displayQueueCallback = DisplayCallback;
    init.displayQueueCallbackDataSize = sizeof(DisplayData);
    init.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    if (const int result = sceGxmInitialize(&init); result < 0) {
        LOG_CRITICAL(Frontend, "sceGxmInitialize failed: {:#010x}", static_cast<u32>(result));
        return false;
    }
    g.initialized = true;
    if (!BringUp()) {
        Shutdown();
        return false;
    }
    return true;
}

void Shutdown() {
    if (g.context != nullptr) {
        sceGxmDisplayQueueFinish();
        sceGxmFinish(g.context);
    }
    if (g.patcher != nullptr) {
        sceGxmShaderPatcherDestroy(g.patcher);
    }
    for (SceGxmSyncObject* sync : g.sync) {
        if (sync != nullptr) {
            sceGxmSyncObjectDestroy(sync);
        }
    }
    if (g.render_target != nullptr) {
        sceGxmDestroyRenderTarget(g.render_target);
    }
    if (g.context != nullptr) {
        sceGxmDestroyContext(g.context);
    }
    for (const Block& block : g.blocks) {
        switch (block.kind) {
        case MapKind::Regular:
            sceGxmUnmapMemory(block.addr);
            break;
        case MapKind::VertexUsse:
            sceGxmUnmapVertexUsseMemory(block.addr);
            break;
        case MapKind::FragmentUsse:
            sceGxmUnmapFragmentUsseMemory(block.addr);
            break;
        }
        sceKernelFreeMemBlock(block.uid);
    }
    std::free(g.context_host_mem);
    if (g.initialized) {
        sceGxmTerminate();
    }
    g = State{};
}

void BeginFrame() {
    // One frame's work at a time: the GPU is done with the last frame's vertices before this
    // one's go where they were. A notice draws little, and nothing moves for the wait to show.
    sceGxmFinish(g.context);
    g.arena_used = 0;
    g.back = g.frame_index % DisplayBuffers;
    sceGxmBeginScene(g.context, 0, g.render_target, nullptr, nullptr, g.sync[g.back],
                     &g.display_surface[g.back], nullptr);
    sceGxmSetCullMode(g.context, SCE_GXM_CULL_NONE);
    sceGxmSetFrontDepthFunc(g.context, SCE_GXM_DEPTH_FUNC_ALWAYS);
    sceGxmSetFrontDepthWriteEnable(g.context, SCE_GXM_DEPTH_WRITE_DISABLED);
}

void EndFrame() {
    sceGxmEndScene(g.context, nullptr, nullptr);
    const u32 front = (g.frame_index + DisplayBuffers - 1) % DisplayBuffers;
    DisplayData data{g.display_mem[g.back]};
    sceGxmDisplayQueueAddEntry(g.sync[front], g.sync[g.back], &data);
    g.frame_index++;
}

SceGxmContext* Context() {
    return g.context;
}

SceGxmShaderPatcher* Patcher() {
    return g.patcher;
}

void* AllocMapped(u32 size) {
    return Uncached(size);
}

void* FrameAlloc(u32 size, u32 align) {
    const u32 offset = (g.arena_used + align - 1) & ~(align - 1);
    ASSERT_MSG(offset + size <= FrameArenaSize, "a notice's frame took more than {} KiB",
               FrameArenaSize >> 10);
    g.arena_used = offset + size;
    return g.arena + offset;
}

} // namespace VitaFrontend::GxmPresent
