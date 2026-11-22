// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_gxm/sgxd_backend.h"

#ifdef __vita__
#include <psp2/kernel/sysmem.h>
#include "sgxd.h"
#include "sgxd_mkq.h"
#include "sgxd_ptla.h"
#include "sgxd_session.h"
#endif

namespace SgxdRenderer {

namespace {

u64 WaitMark() {
#ifdef VITA_DIAGNOSTICS
    return sgxd_wait_us_total();
#else
    return 0;
#endif
}

// GPU addresses: the frontend's recipe is at 16 MiB and its texture at
// 48 MiB, guest VRAM at 0x18000000 and FCRAM at 0x20000000, the display buffers at 1 GiB. The
// rasterizer's block goes at 64 MiB, its host surfaces at 1 GiB + 128 MiB.
constexpr u32 BlockVa = 0x04000000;
constexpr u32 SurfacesVa = 0x48000000;
constexpr u32 SurfacesSize = 8u << 20;
constexpr u32 Pages = 418;
constexpr u32 MaxIndices = SGXD_RECIPE_MAX_INDICES;
// The areas after the layout. The code heap must stay within DOUTU's reach of CV and CF (EXE,
// 20 bits of 8 bytes: 8 MiB).
constexpr u32 CodeSize = 1u << 20;
constexpr u32 PdsSize = 1u << 20;
constexpr u32 DataSize = 1u << 20;

constexpr u32 AlignUp(u32 value, u32 align) {
    return (value + align - 1) & ~(align - 1);
}

#ifdef __vita__
// A block mapped at va for the GPU, in CDRAM: uncached for the CPU, so what it writes needs no
// cache work. Without CDRAM it stops. `size` is a multiple of 256 KiB.
u8* MapBlock(const char* name, u32 va, u32 size, int& uid) {
    uid = sceKernelAllocMemBlock(name, SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, size, nullptr);
    void* base = nullptr;
    ASSERT_MSG(uid >= 0 && sceKernelGetMemBlockBase(uid, &base) >= 0,
               "sgxd: no {} KiB block for {} ({:#010x})", size >> 10, name,
               static_cast<u32>(uid));
    const int mapped = sgxdMap(va, base, size, 0);
    ASSERT_MSG(mapped == 0, "sgxd: {} not mapped at GPU {:#010x}: {:#010x}", name, va,
               static_cast<u32>(mapped));
    return static_cast<u8*>(base);
}
#else
u8* MapBlock(const char*, u32, u32 size, int& uid) {
    uid = -1;
    return static_cast<u8*>(std::aligned_alloc(0x1000, size));
}
#endif

void FreeBlock(u8* cpu, u32 va, u32 size, int uid) {
    if (cpu == nullptr) {
        return;
    }
#ifdef __vita__
    sgxdUnmap(va, size);
    sceKernelFreeMemBlock(uid);
#else
    (void)va;
    (void)size;
    (void)uid;
    std::free(cpu);
#endif
}

} // Anonymous namespace

SgxdBackend::SgxdBackend() {
    surfaces = MapBlock("azahar-sgxd-surfaces", SurfacesVa, SurfacesSize, surfaces_block);
    surfaces_size = SurfacesSize;
    draws.resize(MaxDraws);
    programs.resize(MaxDraws);
}

SgxdBackend::~SgxdBackend() {
    FreeBlock(memory, BlockVa, size, block);
    FreeBlock(surfaces, SurfacesVa, surfaces_size, surfaces_block);
}

void SgxdBackend::CreateRecipe(u32 width, u32 height) {
    identity.resize(MaxIndices);
    for (u32 n = 0; n < MaxIndices; n++) {
        identity[n] = static_cast<u16>(n);
    }
    // The layout only has to hold MaxDraws valid draws; each pass writes its own.
    init_draws.assign(MaxDraws, SgxdRecipeDraw{0, 3, SGXD_RECIPE_ISP_NEVER, SGXD_R_FPRIM, 0, 0,
                                               nullptr});
    SgxdRecipe& r = recipe;
    ASSERT_MSG(sgxd_recipe_init_programs(&r, width, height, BlockVa, Pages, identity.data(),
                                         MaxIndices, init_draws.data(), MaxDraws) == 0,
               "sgxd: the recipe does not take a {}x{} target", width, height);
    bg_offset = AlignUp(r.offset[SGXD_R_END], 0x1000);
    code_begin = code_used = bg_offset + 0x1000;
    code_end = code_begin + CodeSize;
    pds_begin = pds_used = code_end;
    pds_end = pds_begin + PdsSize;
    data_begin = data_used = pds_end;
    data_end = data_begin + DataSize;
    size = AlignUp(data_end, 1u << 20);
    ASSERT_MSG(code_end - r.offset[SGXD_R_CV] <= (0xfffffu << 3),
               "sgxd: the code heap is past DOUTU's reach of CV");
    memory = MapBlock("azahar-sgxd-raster", BlockVa, size, block);

    sgxd_recipe_write(&r, memory);
    // The arrays go with the recipe: only the indices are read again.
    r.draws = nullptr;
    sgxd_recipe_sa_background(&r, memory, bg_offset, 0xff000000u);
    target_width = width;
    target_height = height;
    recipe_ready = true;
    LOG_INFO(Render,
             "sgxd: the rasterizer's {}x{} recipe, {} KiB at GPU {:#010x}: layout {} KiB, code "
             "heap at {:#x}, PDS arena at {:#x}, pass data at {:#x}",
             width, height, size >> 10, BlockVa, r.offset[SGXD_R_END] >> 10, code_begin,
             pds_begin, data_begin);
}

u32 SgxdBackend::PlaceCode(unsigned cb, std::span<const u64> words) {
    ASSERT(recipe_ready);
    const u32 bytes = static_cast<u32>(words.size_bytes());
    const u32 at = AlignUp(code_used, 16);
    ASSERT_MSG(at + bytes <= code_end, "sgxd: the code heap is full ({} KiB)", CodeSize >> 10);
    std::memcpy(memory + at, words.data(), bytes);
    code_used = at + bytes;
    const u32 base = recipe.offset[cb ? SGXD_R_CF : SGXD_R_CV];
    return at - base;
}

u32 SgxdBackend::PlacePds(const SgxdPdsProgram& program) {
    ASSERT(recipe_ready);
    const std::string key(reinterpret_cast<const char*>(program.words), program.count * 4);
    if (const auto it = pds_by_words.find(key); it != pds_by_words.end()) {
        return it->second;
    }
    const u32 at = AlignUp(pds_used, 32);
    ASSERT_MSG(at + program.count * 4 <= pds_end,
               "sgxd: the PDS arena is full ({} KiB, {} programs): the generation flip that "
               "would reuse it is not built",
               PdsSize >> 10, pds_by_words.size());
    std::memcpy(memory + at, program.words, program.count * 4);
    pds_used = at + program.count * 4;
    const u32 va = BlockVa + at;
    pds_by_words.emplace(key, va);
    return va;
}

SgxdBackend::Data SgxdBackend::AllocData(u32 bytes, u32 align) {
    ASSERT(recipe_ready && in_pass);
    const u32 at = AlignUp(data_used, align);
    ASSERT_MSG(at + bytes <= data_end, "sgxd: a pass's data passed {} KiB", DataSize >> 10);
    data_used = at + bytes;
    return {memory + at, BlockVa + at};
}

SgxdBackend::Data SgxdBackend::LutRgRows() const {
    const u32 at = lut_begin + LutLfSize;
    return {shared + at, BlockVa + at};
}

SgxdBackend::Data SgxdBackend::LutRgbaRows() const {
    const u32 at = lut_begin + LutLfSize + LutRgSize;
    return {shared + at, BlockVa + at};
}

SgxdBackend::Data SgxdBackend::AllocSurface(u32 width, u32 height) {
    const u32 bytes = AlignUp(width * height * 4, 0x1000);
    ASSERT_MSG(surfaces_used + bytes <= surfaces_size,
               "sgxd: no room for a {}x{} host surface ({} KiB used)", width, height,
               surfaces_used >> 10);
    const Data d{surfaces + surfaces_used, SurfacesVa + surfaces_used};
    surfaces_used += bytes;
    return d;
}

void SgxdBackend::OpenPass(u32 target_gpu, u32 width, u32 height, u32 pack, u32 clear,
                           float depth) {
    ASSERT(!in_pass);
    if (!recipe_ready) {
        CreateRecipe(width, height);
    }
    ASSERT_MSG(width == target_width && height == target_height,
               "sgxd: a {}x{} target; the recipe is laid out for {}x{}, and a second size is "
               "not built",
               width, height, target_width, target_height);
    sgxd_recipe_set_target(&recipe, memory, target_gpu, width);
    sgxd_recipe_set_target_pack(&recipe, memory, pack);
    *reinterpret_cast<volatile u32*>(memory + bg_offset) = clear;
    sgxd_recipe_set_background_depth(&recipe, memory, depth);
    data_used = data_begin;
    draw_count = 0;
    cached.clear();
    stores.clear();
    in_pass = true;
}

void SgxdBackend::BeginPass(const Data& target, u32 width, u32 height, u32 clear, float depth) {
    // A tile no draw touches stays EMPTY and is skipped whole, background included, keeping the
    // target's bytes: the target starts
    // as the clear through the PTLA, and the background repeats it in the tiles that render.
#ifdef __vita__
    const SgxdPtlaFill fill{target.gpu, SGXD_PTLA_8888, width * 4, width, height, 0, clear};
    char failed[160];
    ASSERT_MSG(sgxd_ptla_fill(&fill, nullptr, failed, sizeof(failed)) == 0,
               "sgxd: a {}x{} target's clear stopped: {}", width, height, failed);
#else
    for (u32 n = 0; n < width * height; n++) {
        std::memcpy(target.cpu + n * 4, &clear, 4);
    }
#endif
    OpenPass(target.gpu, width, height, SGXD_RECIPE_PACK_U8888, clear, depth);
}

void SgxdBackend::Draw(const SgxdRecipeDraw& draw, const SgxdRecipeDrawPrograms& program) {
    ASSERT(in_pass);
    ASSERT_MSG(draw_count < MaxDraws, "sgxd: a pass of more than {} draws", MaxDraws);
    programs[draw_count] = program;
    draws[draw_count] = draw;
    draws[draw_count].programs = &programs[draw_count];
    if (GxmFlag("sgxd_pass_recipe_draw")) {
        draws[draw_count] = {0, 3, draw.isp, SGXD_R_FPRIM, 0, 0, nullptr};
    }
    draw_count++;
}

void SgxdBackend::DrawHang() {
    DEBUG_ASSERT(in_pass);
    const Recipe& r = *current;
    SgxdRecipeDrawPrograms p =
        BlitPrograms(Data{nullptr, pass_target.lo}, r.width, r.height, r.width);
    Usse::Encoder e;
    e.Phas(true);
    const std::size_t loop = e.Br(Usse::Pred::None);
    e.PatchBr(loop, loop);
    const u32 hang = sgxd_program_doutu0(HeapCodeBase(1), PlaceCode(1, e.Words()), 0);
    const u32 colour = SGXD_PROGRAM_BLIT_DOUTI_COLOUR;
    const u32 words = 4;
    const auto fprim = PixelPds(hang, &colour, &words, 1);
    ASSERT(fprim.va != 0);
    p.fprim = fprim.va;
    p.fprim_units = fprim.units;
    Draw(SgxdRecipeDraw{0, 6, SGXD_RECIPE_ISP_TRANS | SGXD_RECIPE_ISP_CPRES, SGXD_R_FPRIM, 0, 0,
                        nullptr, SGXD_RECIPE_ISP_C},
         p);
    LOG_CRITICAL(Render, "sgxd: hangtest: pass {} ({}x{}) gets a pixel program that never ends",
                 passes, r.width, r.height);
}

void SgxdBackend::ReadsCached(const u8* cpu, u32 bytes) {
    cached.push_back({cpu, bytes});
}

void SgxdBackend::EndPass(u32* ta_us, u32* d3_us) {
    ASSERT(in_pass);
    in_pass = false;
    passes++;
    if (draw_count == 0) {
        return;
    }
    ASSERT_MSG(sgxd_recipe_set_draws(&recipe, memory, draws.data(), draw_count) == 0,
               "sgxd: the pass's {} draws were not taken", draw_count);
    sgxd_recipe_restage(&recipe, memory);
#ifdef __vita__
    for (const CachedRange& range : cached) {
        SgxdCacheRange(range.cpu, range.bytes, SGXD_CACHE_CLEAN, CacheSite::Vertices);
    }
    // A store's lines leave the cache before the GPU writes under them, and again after, so
    // the CPU reads what the GPU wrote (the second is an invalidate of clean lines).
    for (const Store& store : stores) {
        SgxdCacheRange(store.cpu, store.bytes, SGXD_CACHE_CLEAN_INVALIDATE,
                       CacheSite::TransferBefore);
    }
    char failed[160] = "";
    SgxdSessionRender render{};
    const bool ok =
        sgxd_session_phase(&recipe, sgxd_session_slc_inval, "slc", failed, sizeof(failed)) == 0 &&
        sgxd_session_render(&recipe, &render, failed, sizeof(failed)) == 0;
    if (!ok) {
        static u32 dump[SGXD_DUMP_WORDS];
        const int dumped = sgxdDump(dump, SGXD_DUMP_WORDS);
        if (std::FILE* f = std::fopen("host0:azahar-sgxd-dump.bin", "wb")) {
            std::fwrite(dump, 4, dumped > 0 ? SGXD_DUMP_WORDS : 0, f);
            std::fclose(f);
        }
        if (std::FILE* f = std::fopen("host0:azahar-sgxd-block.bin", "wb")) {
            std::fwrite(memory, 1, size, f);
            std::fclose(f);
        }
        LOG_CRITICAL(Render, "sgxd: dump {:#x}; block {} KiB at GPU {:#010x} written to host0",
                     static_cast<u32>(dumped), size >> 10, BlockVa);
        // The kernel's own snapshot from the moment the poll gave up (sgxdFaultDump), and what
        // the GPU's MMU makes of the addresses a hang is usually about: each BIF bank's fault
        // address, and each draw's programs and index list.
        const int faulted = sgxdFaultDump(dump, SGXD_DUMP_WORDS);
        if (std::FILE* f = std::fopen("host0:azahar-sgxd-fault.bin", "wb")) {
            std::fwrite(dump, 4, faulted > 0 ? SGXD_DUMP_WORDS : 0, f);
            std::fclose(f);
        }
        std::FILE* f = std::fopen("host0:azahar-sgxd-fault.txt", "w");
        const auto translate = [f](const char* what, u32 va) {
            u32 out[2]{};
            const int ret = sgxdTranslate(va, out);
            LOG_CRITICAL(Render, "sgxd: {} {:#010x}: pde {:08x} pte {:08x} ({:#x})", what, va,
                         out[0], out[1], static_cast<u32>(ret));
            if (f) {
                std::fprintf(f, "%s %08x pde %08x pte %08x ret %08x\n", what,
                             static_cast<unsigned>(va), static_cast<unsigned>(out[0]),
                             static_cast<unsigned>(out[1]), static_cast<unsigned>(ret));
            }
        };
        static constexpr u32 FaultBanks[] = {0x0000, 0x4000, 0x8000, 0xc000, 0x10000, 0x14000};
        for (const u32 bank : FaultBanks) {
            u32 bif[2]{};   // BIF_INT_STAT, BIF_FAULT
            sgxdReadRegs(bank + 0x0c04, 2, bif);
            LOG_CRITICAL(Render, "sgxd: bank {:#07x} bif int {:08x} fault {:08x}", bank, bif[0],
                         bif[1]);
            if (f) {
                std::fprintf(f, "bank %05x bif_int %08x bif_fault %08x\n",
                             static_cast<unsigned>(bank), static_cast<unsigned>(bif[0]),
                             static_cast<unsigned>(bif[1]));
            }
            if (bif[1] & ~0xfffu) {
                translate("fault", bif[1] & ~0xfffu);
            }
        }
        for (u32 n = 0; n < draw_count; n++) {
            const SgxdRecipeDrawPrograms& p = programs[n];
            translate("vspds", p.vspds);
            translate("vsec", p.vsec);
            translate("fprim", p.fprim);
            if (p.indices) {
                translate("indices", p.indices);
            }
        }
        if (f) {
            std::fprintf(f, "fault dump %d\n", faulted);
            std::fclose(f);
        }
    }
    ASSERT_MSG(ok, "sgxd: pass {} of {} draws stopped: {}", passes, draw_count, failed);
    for (const Store& store : stores) {
        SgxdCacheRange(store.cpu, store.bytes, SGXD_CACHE_CLEAN_INVALIDATE,
                       CacheSite::TransferAfter);
    }
    if (ta_us) {
        *ta_us = render.ta_us;
    }
    if (d3_us) {
        *d3_us = render.d3_us;
    }
#else
    if (ta_us) {
        *ta_us = 0;
    }
    if (d3_us) {
        *d3_us = 0;
    }
#endif
}

SgxdRecipeDrawPrograms SgxdBackend::BlitPrograms(const Data& source, u32 width, u32 height) {
    if (!blit_ready) {
        u64 words[4];
        blit_vertex = sgxd_program_doutu0(0, PlaceCode(0, {words, sgxd_program_blit_vertex(words)}), 0);
        blit_fragment =
            sgxd_program_doutu0(1, PlaceCode(1, {words, sgxd_program_blit_fragment(words)}), 0);
        blit_ready = true;
    }
    // Two triangles over the target: x, y, z, w, a white colour (unused), u, v. Pixel row y
    // reads texel row y: both follow y the same way.
    constexpr u32 N = SGXD_PROGRAM_BLIT_VERTEX_WORDS;
    const Data vertices = AllocData(6 * N * 4);
    static constexpr float corner[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
    float* v = reinterpret_cast<float*>(vertices.cpu);
    for (u32 n = 0; n < 6; n++) {
        const float x = corner[n][0], y = corner[n][1];
        const float words[N] = {x, y, 0.5f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, (x + 1) / 2, (y + 1) / 2};
        std::memcpy(v + n * N, words, sizeof(words));
    }

    SgxdRecipeDrawPrograms p{};
    SgxdPdsProgram pds{};
    const SgxdVertexFetch fetch{vertices.gpu, N * 4, N, 0};
    ASSERT(sgxd_program_vertex_pds(&fetch, 1, blit_vertex, 0, &pds) == 0);
    p.vspds = PlacePds(pds);
    p.vspds_units = pds.units;
    // The dummy secondaries at the code bases (the recipe's CV_DUMMY_VS and CF_DUMMY_FS).
    ASSERT(sgxd_program_secondary_pds(0, 0, sgxd_program_doutu0(0, 0, 0), &pds) == 0);
    p.vsec = PlacePds(pds);
    p.vsec_units = pds.units;
    p.vsec_sa = 0;
    ASSERT(sgxd_program_secondary_pds(0, 0, sgxd_program_doutu0(1, 0, 0), &pds) == 0);
    p.fsec = PlacePds(pds);
    p.fsec_units = pds.units;
    // The texel comes back unconverted (FCONV 0), the surface's word as it is.
    u32 tex[4];
    sgxd_recipe_texture_words(source.gpu, width, height, 0, tex);
    ASSERT(sgxd_program_blit_pixel_pds(blit_fragment, tex, &pds) == 0);
    p.fprim = PlacePds(pds);
    p.fprim_units = pds.units;
    sgxd_program_vertex_dms(N, 0, 0, N, &p.vdm_w2, &p.vdm_w4, &p.vdm_w5);
    p.pixel_size = sgxd_program_pixel_dms(SGXD_PROGRAM_BLIT_PA, 0, 0);
    p.outselects = SGXD_PROGRAM_BLIT_OUTSELECTS;
    p.texsize = SGXD_PROGRAM_BLIT_TEXSIZE;
    const float viewport[6] = {width / 2.0f, width / 2.0f, height / 2.0f, height / 2.0f, 0.0f,
                               1.0f};
    std::memcpy(p.viewport, viewport, sizeof(viewport));
    p.indices = 0;
    p.primitive = SGXD_PROGRAM_TRIANGLES;
    return p;
}

void SgxdBackend::Blit(const Data& source, u32 width, u32 height, const Store& dest, u32* ta_us,
                       u32* d3_us) {
    OpenPass(dest.gpu, width, height, dest.pack, 0, 1.0f);
    // Uncached memory (VRAM) comes without a CPU range and needs no cache work.
    if (dest.bytes != 0) {
        stores.push_back(dest);
    }
    const SgxdRecipeDrawPrograms p = BlitPrograms(source, width, height);
    // ISP A: opaque, DCMP ALWAYS, DWRITEDIS, BPRES; no culling.
    const u32 isp = 7u << 22 | 1u << 20 | 1u << 9;
    Draw(SgxdRecipeDraw{0, 6, isp, SGXD_R_FPRIM, 0, 0, nullptr}, p);
    EndPass(ta_us, d3_us);
}

} // namespace SgxdRenderer
