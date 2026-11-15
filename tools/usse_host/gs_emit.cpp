// Host check of the fused VS + GS emitter on a gsdump file (diag flag gsdump, written by
// ThreadedRasterizer's GsDump): ./gs_emit gsdump_<key>.bin [level] [out.gxp]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "video_core/pica/shader_setup.h"
#include "video_core/renderer_gxm/gxm_gs_request.h"

int main(int argc, char** argv) {
    using namespace GxmRenderer;
    std::FILE* f = std::fopen(argv[1], "rb");
    if (f == nullptr) {
        return 1;
    }
    uint32_t hdr[12], extra[4], loader_offset[12];
    std::fread(hdr, 4, 12, f);
    std::fread(extra, 4, 4, f);
    std::fread(loader_offset, 4, 12, f);
    if (hdr[2] != sizeof(Pica::RegsInternal)) {
        std::fprintf(stderr, "regs size %u, expected %zu\n", hdr[2], sizeof(Pica::RegsInternal));
        return 1;
    }
    static Pica::RegsInternal regs;
    std::fread(&regs, sizeof(regs), 1, f);
    const uint32_t words = hdr[3] / 4, swords = hdr[4] / 4;
    std::vector<uint32_t> gs_code(words), gs_swz(swords), vs_code(words), vs_swz(swords);
    std::fread(gs_code.data(), 4, words, f);
    std::fread(gs_swz.data(), 4, swords, f);
    std::fread(vs_code.data(), 4, words, f);
    std::fread(vs_swz.data(), 4, swords, f);
    std::vector<uint8_t> uniforms(hdr[5] * 2);
    std::fread(uniforms.data(), 1, uniforms.size(), f);
    std::fclose(f);
    // Bools at their offset in the VS's Uniforms (f: 96 x 16 bytes).
    const uint8_t* vs_b = uniforms.data() + hdr[5] + 96 * 16;

    Usse::VsRequest r{};
    r.code = vs_code;
    r.swizzle = vs_swz;
    r.code_size = words;
    r.main_offset = regs.vs.main_offset;
    // The loaders, as AccelerateShippedDraw walks them.
    const auto& va = regs.pipeline.vertex_attributes;
    uint32_t enabled = 0;
    for (uint32_t li = 0; li < 12; li++) {
        const auto& loader = va.attribute_loaders[li];
        if (loader.component_count == 0 || loader.byte_count == 0) {
            continue;
        }
        for (uint32_t c = 0; c < loader.component_count && c < 12; c++) {
            const uint32_t a = loader.GetComponent(c);
            if (a < 12 && va.GetNumElements(a) != 0) {
                const uint32_t reg = regs.vs.GetRegisterForAttribute(a);
                enabled |= 1u << reg;
                r.reg_components[reg] = static_cast<uint8_t>(va.GetNumElements(a));
            }
        }
    }
    r.default_regs = static_cast<uint16_t>(~enabled & 0xffff);
    r.bool_mask = 0xffff;
    for (int i = 0; i < 16; i++) {
        r.bools |= static_cast<uint16_t>((vs_b[i] ? 1u : 0u) << i);
    }
    Usse::GsStage gs{};
    gs.code = gs_code;
    gs.swizzle = gs_swz;
    gs.code_size = words;
    const char* why = "";
    if (!BuildGsStage(regs, gs, r, &why)) {
        std::fprintf(stderr, "not covered: %s\n", why);
        return 1;
    }
    Usse::GsResult result;
    r.gs_result = &result;
    uint32_t input_regs = 0;
    std::string refusal;
    const int level = argc > 2 ? std::atoi(argv[2]) : 1;
    auto prog = Usse::EmitVertexProgram(r, input_regs, &refusal, level);
    std::printf("program %zu bytes, refusal '%s', inputs %#x, corners %u, pattern", prog.size(),
                refusal.c_str(), input_regs, result.corners);
    for (auto c : result.pattern) {
        std::printf(" %u", c);
    }
    std::printf(", gs rows");
    for (auto row : result.uniform_rows) {
        std::printf(" %u", row);
    }
    std::printf("\n");
    if (argc > 3 && !prog.empty()) {
        std::FILE* o = std::fopen(argv[3], "wb");
        std::fwrite(prog.data(), 1, prog.size(), o);
        std::fclose(o);
    }
    return prog.empty();
}
