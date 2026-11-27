// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// sgxd's dumps to host0, in the diagnostics build only: dumpusse's programs (sgxd_dump.h) and
// RasterizerSgxd's own: dumptex's textures, a stopped stream-out's geometry stage.

#ifdef VITA_DIAGNOSTICS

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "core/memory.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/renderer_sgxd/sgxd_dump.h"
#include "video_core/renderer_sgxd/sgxd_flags.h"
#include "video_core/renderer_sgxd/sgxd_rasterizer.h"
#include "video_core/renderer_sgxd/usse/usse_program.h"
#include "video_core/renderer_sgxd/usse/vs_usse_gen.h"
#include "video_core/shader/generator/glsl_fs_shader_gen.h"
#include "video_core/shader/generator/glsl_shader_gen.h"
#include "video_core/shader/generator/profile.h"
#include "video_core/shader/generator/shader_gen.h"

namespace SgxdRenderer {

namespace {

/// With sgxd_flags dumpusse, bytes to host0:azahar-NAME.
void DumpBytes(const std::string& name, const void* data, std::size_t size) {
    const std::string path = fmt::format("host0:azahar-{}", name);
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(data, 1, size, f);
        std::fclose(f);
    }
}

} // namespace

void DumpProgram(const char* kind, u64 key, const Usse::Program& program) {
#ifdef VITA_DIAGNOSTICS
    // fsonly=N: a fragment program of those keys, as emitted (fs) and by the second tier (fs2),
    // with its IR instructions by the emitter's section.
    static const std::vector<u32> only = SgxdFlagValues("fsonly");
    if (!program.vertex &&
        std::find(only.begin(), only.end(), static_cast<u32>(key)) != only.end()) {
        LOG_INFO(Render, "perf: {} sections {:016x}: {} words, {} temps; IR by section:{}", kind, key,
                 program.primary.size(), program.temp_count, program.sections);
    }
#endif
    if (!SgxdFlag("dumpusse")) {
        return;
    }
    const std::string json = Usse::ProgramJson(program);
    const std::string path = fmt::format("host0:azahar-{}-{:016x}.json", kind, key);
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(json.data(), 1, json.size(), f);
        std::fclose(f);
        LOG_INFO(Render, "{} program {:016x}: written to {}", kind, key, path);
    }
}

void DumpPicaFragment(u64 key, const Pica::Shader::FSConfig& config) {
    if (!SgxdFlag("dumpusse")) {
        return;
    }
    DumpBytes(fmt::format("fs-{:016x}.fsconfig", key), &config, sizeof(config));
    const std::string glsl = Pica::Shader::Generator::GLSL::GenerateFragmentShader(
        config, Pica::Shader::UserConfig{}, Pica::Shader::Profile{});
    DumpBytes(fmt::format("fs-{:016x}.glsl", key), glsl.data(), glsl.size());
}

void DumpPicaVertex(u64 key, const Pica::Shader::Generator::PicaVSConfig& config,
                    const Pica::ShaderSetup& setup, const Usse::VsRequest& request) {
    if (!SgxdFlag("dumpusse")) {
        return;
    }
    std::vector<u8> bytes(sizeof(config));
    std::memcpy(bytes.data(), &config, sizeof(config));
    const auto code = setup.GetProgramCode();
    const auto swizzle = setup.GetSwizzleData();
    const auto append = [&](const auto& words) {
        const auto* p = reinterpret_cast<const u8*>(words.data());
        bytes.insert(bytes.end(), p, p + words.size() * sizeof(words[0]));
    };
    append(code);
    append(swizzle);
    // The trailer, tools/pica_cg's VertexTrailer: "PCGX", the program's extent, the bools (and
    // which of them are baked), the registers without a loader, the u8 registers and the
    // components each loader supplies.
    const std::array<u32, 2> head{0x58474350, request.code_size};
    const std::array<u16, 4> regs{request.bools, request.bool_mask, request.default_regs,
                                  request.u8_regs};
    append(head);
    append(regs);
    append(request.reg_components);
    DumpBytes(fmt::format("vs-{:016x}.pica", key), bytes.data(), bytes.size());
    const std::string glsl = Pica::Shader::Generator::GLSL::GenerateVertexShader(
        setup, config, Pica::Shader::Generator::ExtraVSConfig{});
    DumpBytes(fmt::format("vs-{:016x}.glsl", key), glsl.data(), glsl.size());
}

void RasterizerSgxd::DumpTexture(const Texture& tex, u32 bits, bool split) {
    using TF = Pica::TexturingRegs::TextureFormat;
    const bool cube = tex.parts == 6;
    const auto write = [&](PAddr at, u32 w, u32 h, u32 stride, const char* what, const void* data,
                           u32 size) {
        const std::string path =
            fmt::format("host0:azahar-tex{}-{:x}-{}x{}-f{}-s{}-{}.bin", untiles, at, w, h,
                        static_cast<u32>(tex.format), stride, what);
        if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fwrite(data, 1, size, file);
            std::fclose(file);
        }
    };
    // A compressed cube (SgxdBackend::UntileCube): each face's level 0 blocks, a split's A8 beside.
    const bool cube_blocks =
        cube && (split || (tex.format == TF::ETC1 &&
                           UntileGeometry::CompressedCubeFits(tex.width)));
    for (u32 k = 0; cube && k < 6; k++) {
        const u32 size = tex.width * tex.height * 4;
        write(tex.part_addr[k], tex.width, tex.height, 0, "guest",
              memory.GetPhysicalPointer(tex.part_addr[k]), tex.part_bytes);
        if (!cube_blocks) {
            write(tex.part_addr[k], tex.width, tex.height, 0, "host", tex.memory.cpu + k * size,
                  size);
            continue;
        }
        const u32 colour_stride =
            UntileGeometry::CompressedCubeFaceStride(tex.width, tex.levels > 1, 4);
        write(tex.part_addr[k], tex.width, tex.height, 0, "swizzled",
              tex.memory.cpu + k * colour_stride, tex.width * tex.height / 2);
        if (split) {
            const u32 alpha_stride =
                UntileGeometry::CompressedCubeFaceStride(tex.width, tex.levels > 1, 8);
            write(tex.part_addr[k], tex.width, tex.height, 0, "alpha",
                  tex.alpha.cpu + k * alpha_stride, tex.width * tex.height);
        }
    }
    // The guest's bytes up to VRAM's end (ReadableBytes).
    const u32 readable = cube ? 0 : ReadableBytes(tex.addr, tex.part_bytes);
    for (u32 l = 0, guest = 0, host = 0, alpha_at = 0; !cube && l < tex.levels; l++) {
        const u32 w = tex.width >> l, h = tex.height >> l;
        write(tex.addr + guest, w, h, w, "guest", memory.GetPhysicalPointer(tex.addr) + guest,
              std::min(w * h * bits / 8, readable - std::min(guest, readable)));
        const bool blocks = tex.format == TF::ETC1 || split;
        // A byte copy (Texture::byte_swizzle): the texel's one value a byte, "host8".
        const bool one_byte = tex.byte_swizzle != 0;
        const u32 host_size = blocks ? w * h / 2 : one_byte ? w * h : w * h * 4;
        write(tex.addr + guest, w, h, w,
              blocks     ? "swizzled"
              : one_byte ? "host8"
                         : "host",
              tex.memory.cpu + host, host_size);
        if (split) {
            write(tex.addr + guest, w, h, w, "alpha", tex.alpha.cpu + alpha_at, w * h);
            alpha_at += w * h;
        }
        guest += w * h * bits / 8;
        host += host_size;
    }
}

void RasterizerSgxd::DumpPictures() {
    static u32 dumps = 0;
    const u32 n = dumps++;
    backend.RenderIdle("picdump");
    const auto write = [](const std::string& path, const void* data, u32 size) {
        if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
            std::fwrite(data, 1, size, file);
            std::fclose(file);
        }
    };
    for (const Upscaled& u : upscaled) {
        const u32 rows = u.width * u.quarters / 4;
        write(fmt::format("host0:azahar-pic{}-scene-{:x}-{}x{}-q{}-m{}-b{}.bin", n, u.addr,
                          u.stride, rows, u.quarters, u.margin, u.picture_format.bpp),
              u.picture.cpu, u.stride * rows * u.picture_format.bpp);
        write(fmt::format("host0:azahar-pic{}-guest-{:x}-{}x{}.bin", n, u.addr, u.width,
                          u.height),
              memory.GetPhysicalPointer(u.addr), u.bytes);
    }
    for (const ScreenPicture& p : screen_pictures) {
        write(fmt::format("host0:azahar-pic{}-screen-{:x}-{}x{}-q{}-b{}-v{}.bin", n, p.output,
                          p.width, p.height, p.quarters, p.picture_format.bpp, p.valid ? 1 : 0),
              p.picture.cpu, p.width * p.height * p.picture_format.bpp);
        write(fmt::format("host0:azahar-pic{}-lcd-{:x}-{}x{}-r{}.bin", n, p.output, p.out_width,
                          p.out_height, p.row_bytes),
              memory.GetPhysicalPointer(p.output), p.bytes);
    }
    LOG_INFO(Render, "picdump {}: {} pictures and {} screen pictures written to host0", n,
             upscaled.size(), screen_pictures.size());
}

void RasterizerSgxd::DumpFrameTexture(u32 unit, const Texture& tex) {
    // One file a texture of the frame (its address and shape).
    const u64 key = u64{tex.addr} << 24 ^ u64{tex.width} << 12 ^ tex.height ^
                    u64{static_cast<u32>(tex.format)} << 56 ^ u64{tex.gpu_width} << 40;
    if (std::find(tex_dumped.begin(), tex_dumped.end(), key) != tex_dumped.end()) {
        return;
    }
    tex_dumped.push_back(key);
    backend.FlushUntiles();
    backend.RenderIdle("texdump");
    const u32 w = tex.gpu_width != 0 ? tex.gpu_width : tex.width;
    const u32 h = tex.gpu_height != 0 ? tex.gpu_height : tex.height;
    const u32 stride = tex.stride != 0 ? tex.stride : w;
    const bool cube = tex.parts == 6;
    const bool etc1 =
        tex.format == Pica::TexturingRegs::TextureFormat::ETC1 || tex.alpha_bytes != 0;
    const char* kind = tex.gpu_width != 0     ? "scaled"
                       : cube                 ? "cube"
                       : etc1                 ? "etc1"
                       : tex.byte_swizzle != 0 ? "u8"
                                              : "rgba";
    // The first level (a chain's next ones follow it).
    const u32 bytes = etc1 ? w * h / 2 : tex.byte_swizzle != 0 ? stride * h : stride * h * 4;
    const std::string path =
        fmt::format("host0:azahar-texf-{:x}-{}x{}-s{}-f{}-l{}-u{}-{}.bin", tex.addr, w, h, stride,
                    static_cast<u32>(tex.format), tex.levels, unit, kind);
    if (!tex.memory.cpu) {
        return;
    }
    if (std::FILE* file = std::fopen(path.c_str(), "wb")) {
        std::fwrite(tex.memory.cpu, 1, tex.bytes != 0 ? std::min(bytes, tex.bytes) : bytes, file);
        std::fclose(file);
    }
}

void RasterizerSgxd::PrintStreamOut() {
    const StreamOutNote& n = stream_note;
    if (n.stage == nullptr || stream_self == nullptr) {
        LOG_CRITICAL(Render, "streamout: none running");
        return;
    }
    RasterizerSgxd& r = *stream_self;
    LOG_CRITICAL(Render,
                 "streamout: {} of pass {} draw {}: GS mode {}, {} invocations, {} rows, cap {}, "
                 "{} indices; program doutu {:#010x}, {} temps; VS {:016x} GS {:016x}",
                 n.stage, n.pass, n.draw, n.mode, n.invocations, n.rows, n.cap, n.count, n.doutu,
                 n.temps, r.pica.vs_setup.GetProgramCodeHash(),
                 r.pica.gs_setup.GetProgramCodeHash());
    for (const Pica::ShaderSetup* setup : {&r.pica.vs_setup, &r.pica.gs_setup}) {
        const auto& u = setup->uniforms;
        LOG_CRITICAL(Render,
                     "streamout: {} entry {:#x}, i {} {} {} {} / {} {} {} {} / {} {} {} {} / "
                     "{} {} {} {}, b {:016b}",
                     setup == &r.pica.vs_setup ? "VS" : "GS", setup->entry_point, u.i[0].x,
                     u.i[0].y, u.i[0].z, u.i[0].w, u.i[1].x, u.i[1].y, u.i[1].z, u.i[1].w, u.i[2].x,
                     u.i[2].y, u.i[2].z, u.i[2].w, u.i[3].x, u.i[3].y, u.i[3].z, u.i[3].w, [&u] {
                         u32 b = 0;
                         for (u32 k = 0; k < 16; k++) {
                             b |= (u.b[k] ? 1u : 0u) << k;
                         }
                         return b;
                     }());
    }
    // tools/sgxd_gs_stage.py reads it: "GSSO", 1, the registers, then the VS's and the GS's
    // entry, code, swizzles, float uniforms (as floats), bools and ints.
    std::FILE* f = std::fopen("host0:azahar-sgxd-gs-stage.bin", "wb");
    if (f == nullptr) {
        return;
    }
    const u32 head[3] = {0x4f535347, 1, static_cast<u32>(r.regs.reg_array.size())};
    std::fwrite(head, 4, 3, f);
    std::fwrite(r.regs.reg_array.data(), 4, r.regs.reg_array.size(), f);
    for (const Pica::ShaderSetup* setup : {&r.pica.vs_setup, &r.pica.gs_setup}) {
        const auto& u = setup->uniforms;
        std::fwrite(&setup->entry_point, 4, 1, f);
        std::fwrite(setup->GetProgramCode().data(), 4, setup->GetProgramCode().size(), f);
        std::fwrite(setup->GetSwizzleData().data(), 4, setup->GetSwizzleData().size(), f);
        for (const auto& v : u.f) {
            const float fl[4] = {v.x.ToFloat32(), v.y.ToFloat32(), v.z.ToFloat32(),
                                 v.w.ToFloat32()};
            std::fwrite(fl, 4, 4, f);
        }
        for (const bool b : u.b) {
            const u32 w = b ? 1 : 0;
            std::fwrite(&w, 4, 1, f);
        }
        for (const auto& i : u.i) {
            const u32 w = i.x | i.y << 8 | i.z << 16 | static_cast<u32>(i.w) << 24;
            std::fwrite(&w, 4, 1, f);
        }
    }
    std::fclose(f);
    LOG_CRITICAL(
        Render, "streamout: PICA registers and programs written to host0:azahar-sgxd-gs-stage.bin");
}

} // namespace SgxdRenderer

#endif
