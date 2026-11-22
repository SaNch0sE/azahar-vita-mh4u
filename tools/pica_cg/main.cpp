// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// pica_cg FILE... OUTDIR: the Cg for PICA programs the sgxd renderer dumped.
//
// With sgxd_flags dumpusse the renderer writes the 3DS side of every program it emits next to
// the program itself: azahar-fs-KEY.fsconfig (the FSConfig's bytes) and azahar-vs-KEY.pica
// (the PicaVSConfig's bytes, the PICA code and swizzle words, and a trailer with what the
// vertex program was built for). This tool runs the Cg generators the renderer used before
// the USSE emitters on them, and writes OUTDIR/fs-KEY_f.cg and OUTDIR/vs-KEY_v.cg for psp2cgc:
// Sony's compiler on the same PICA configuration, to hold the emitters' programs against.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <fmt/format.h>
#include "cg_fs_shader_gen.h"
#include "cg_vs_shader_gen.h"
#include "video_core/pica/shader_setup.h"

namespace {

namespace Cg = Pica::Shader::Generator::Cg;
using Pica::Shader::Generator::PicaVSConfig;

/// The .pica trailer: what the vertex program was built for beyond the PicaVSConfig. Written
/// by the renderer's DumpPicaVertex; dumps from before it have none.
struct VertexTrailer {
    u32 magic; ///< VertexTrailerMagic
    u32 program_size;
    u16 bools; ///< the bool uniforms' values, baked where `bool_mask` has a bit
    u16 bool_mask;
    u16 default_regs;
    u16 u8_regs;
    std::array<u8, 16> reg_components;
};
constexpr u32 VertexTrailerMagic = 0x58474350; // "PCGX"

std::vector<u8> ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    return static_cast<bool>(out);
}

/// "azahar-fs-KEY" -> "fs-KEY".
std::string Name(const std::filesystem::path& path) {
    std::string name = path.stem().string();
    if (name.rfind("azahar-", 0) == 0) {
        name.erase(0, 7);
    }
    return name;
}

bool Fragment(const std::filesystem::path& path, const std::filesystem::path& outdir) {
    const std::vector<u8> bytes = ReadFile(path);
    if (bytes.size() != sizeof(Pica::Shader::FSConfig)) {
        fmt::print(stderr, "{}: {} bytes, an FSConfig is {}\n", path.string(), bytes.size(),
                   sizeof(Pica::Shader::FSConfig));
        return false;
    }
    Pica::Shader::FSConfig config{Pica::RegsInternal{}};
    std::memcpy(&config, bytes.data(), sizeof(config));
    const std::string source = Cg::GenerateFragmentShader(config, Pica::Shader::UserConfig{},
                                                          Cg::MakeGxmProfile());
    return WriteFile(outdir / (Name(path) + "_f.cg"), source);
}

bool Vertex(const std::filesystem::path& path, const std::filesystem::path& outdir) {
    constexpr std::size_t words = Pica::MAX_PROGRAM_CODE_LENGTH + Pica::MAX_SWIZZLE_DATA_LENGTH;
    constexpr std::size_t body = sizeof(PicaVSConfig) + words * sizeof(u32);
    const std::vector<u8> bytes = ReadFile(path);
    if (bytes.size() != body && bytes.size() != body + sizeof(VertexTrailer)) {
        fmt::print(stderr, "{}: {} bytes, a PicaVSConfig and the code are {} (+{} trailer)\n",
                   path.string(), bytes.size(), body, sizeof(VertexTrailer));
        return false;
    }
    PicaVSConfig config{};
    std::memcpy(&config.state, bytes.data(), sizeof(config.state));
    Pica::ProgramCode code{};
    Pica::SwizzleData swizzle{};
    const u8* p = bytes.data() + sizeof(PicaVSConfig);
    std::memcpy(code.data(), p, sizeof(code));
    std::memcpy(swizzle.data(), p + sizeof(code), sizeof(swizzle));

    Cg::VSExtra extra{};
    u32 program_size = 0;
    if (bytes.size() > body) {
        VertexTrailer trailer;
        std::memcpy(&trailer, bytes.data() + body, sizeof(trailer));
        if (trailer.magic != VertexTrailerMagic) {
            fmt::print(stderr, "{}: bad trailer magic {:08x}\n", path.string(), trailer.magic);
            return false;
        }
        program_size = trailer.program_size;
        extra.bools = trailer.bools;
        extra.bool_mask = trailer.bool_mask;
        extra.default_regs = trailer.default_regs;
        extra.reg_components = trailer.reg_components;
    } else {
        // Without the trailer the program's extent is the last word that is not zero, and the
        // bools stay uniforms.
        for (u32 i = 0; i < Pica::MAX_PROGRAM_CODE_LENGTH; i++) {
            if (code[i] != 0) {
                program_size = i + 1;
            }
        }
    }
    Pica::ShaderSetup setup;
    setup.UpdateProgramCode(code, program_size);
    setup.UpdateSwizzleData(swizzle);

    u32 input_regs = 0;
    const std::string source = Cg::GenerateVertexShader(setup, config, extra, input_regs);
    if (source.empty()) {
        fmt::print(stderr, "{}: the Cg decompiler cannot express this program\n", path.string());
        return false;
    }
    return WriteFile(outdir / (Name(path) + "_v.cg"), source);
}

} // Anonymous namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fmt::print(stderr, "usage: {} FILE.fsconfig|FILE.pica... OUTDIR\n", argv[0]);
        return 2;
    }
    const std::filesystem::path outdir = argv[argc - 1];
    std::filesystem::create_directories(outdir);
    int failed = 0;
    for (int i = 1; i < argc - 1; i++) {
        const std::filesystem::path path = argv[i];
        bool ok = false;
        if (path.extension() == ".fsconfig") {
            ok = Fragment(path, outdir);
        } else if (path.extension() == ".pica") {
            ok = Vertex(path, outdir);
        } else {
            fmt::print(stderr, "{}: neither .fsconfig nor .pica\n", path.string());
        }
        failed += ok ? 0 : 1;
    }
    return failed == 0 ? 0 : 1;
}
