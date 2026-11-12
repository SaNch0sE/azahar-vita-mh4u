#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "video_core/renderer_gxm/usse/vs_usse_gen.h"
int main(int argc, char** argv) {
    using namespace GxmRenderer::Usse;
    std::FILE* f = std::fopen(argv[1], "rb");
    VsRequest r{};
    std::fread(&r.code_size, 4, 1, f);
    std::fread(&r.main_offset, 4, 1, f);
    std::fread(&r.num_outputs, 4, 1, f);
    std::fread(r.output_map.data(), 4, r.output_map.size(), f);
    std::fread(r.semantics.data(), sizeof(VsSemantic), r.semantics.size(), f);
    std::fread(&r.default_regs, 2, 1, f);
    std::fread(&r.bools, 2, 1, f);
    std::fread(&r.bool_mask, 2, 1, f);
    std::fread(r.reg_components.data(), 1, r.reg_components.size(), f);
    uint32_t n;
    std::fread(&n, 4, 1, f);
    std::vector<uint32_t> code(n);
    std::fread(code.data(), 4, n, f);
    std::fread(&n, 4, 1, f);
    std::vector<uint32_t> swz(n);
    std::fread(swz.data(), 4, n, f);
    std::fclose(f);
    r.code = code;
    r.swizzle = swz;
    uint32_t input_regs = 0;
    std::string refusal;
    const int level = argc > 2 ? std::atoi(argv[2]) : 2;
    auto prog = EmitVertexProgram(r, input_regs, &refusal, level);
    std::fprintf(stderr, "program %zu bytes, refusal '%s'\n", prog.size(), refusal.c_str());
}
