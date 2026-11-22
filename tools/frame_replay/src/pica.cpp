// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

#include "pica.h"

#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace frame_replay::pica {

namespace {

// The internal registers by word offset (3dbrew "GPU/Internal Registers").
const std::map<u32, const char*>& InternalNames() {
    static const std::map<u32, const char*> names = {
        {0x000, "FINALIZE"},
        {0x040, "FACECULLING_CONFIG"},
        {0x041, "VIEWPORT_WIDTH"},
        {0x042, "VIEWPORT_INVW"},
        {0x043, "VIEWPORT_HEIGHT"},
        {0x044, "VIEWPORT_INVH"},
        {0x047, "FRAGOP_CLIP"},
        {0x048, "FRAGOP_CLIP_DATA0"},
        {0x04D, "DEPTHMAP_SCALE"},
        {0x04E, "DEPTHMAP_OFFSET"},
        {0x04F, "SH_OUTMAP_TOTAL"},
        {0x050, "SH_OUTMAP_O0"},
        {0x051, "SH_OUTMAP_O1"},
        {0x052, "SH_OUTMAP_O2"},
        {0x053, "SH_OUTMAP_O3"},
        {0x054, "SH_OUTMAP_O4"},
        {0x055, "SH_OUTMAP_O5"},
        {0x056, "SH_OUTMAP_O6"},
        {0x061, "EARLYDEPTH_FUNC"},
        {0x062, "EARLYDEPTH_TEST1"},
        {0x063, "EARLYDEPTH_CLEAR"},
        {0x064, "SH_OUTATTR_MODE"},
        {0x065, "SCISSORTEST_MODE"},
        {0x066, "SCISSORTEST_POS"},
        {0x067, "SCISSORTEST_DIM"},
        {0x068, "VIEWPORT_XY"},
        {0x06A, "EARLYDEPTH_DATA"},
        {0x06D, "DEPTHMAP_ENABLE"},
        {0x06E, "RENDERBUF_DIM"},
        {0x06F, "SH_OUTATTR_CLOCK"},
        {0x080, "TEXUNIT_CONFIG"},
        {0x081, "TEXUNIT0_BORDER_COLOR"},
        {0x082, "TEXUNIT0_DIM"},
        {0x083, "TEXUNIT0_PARAM"},
        {0x084, "TEXUNIT0_LOD"},
        {0x085, "TEXUNIT0_ADDR1"},
        {0x086, "TEXUNIT0_ADDR2"},
        {0x087, "TEXUNIT0_ADDR3"},
        {0x088, "TEXUNIT0_ADDR4"},
        {0x089, "TEXUNIT0_ADDR5"},
        {0x08A, "TEXUNIT0_ADDR6"},
        {0x08B, "TEXUNIT0_SHADOW"},
        {0x08E, "TEXUNIT0_TYPE"},
        {0x08F, "LIGHTING_ENABLE0"},
        {0x091, "TEXUNIT1_BORDER_COLOR"},
        {0x092, "TEXUNIT1_DIM"},
        {0x093, "TEXUNIT1_PARAM"},
        {0x094, "TEXUNIT1_LOD"},
        {0x095, "TEXUNIT1_ADDR"},
        {0x096, "TEXUNIT1_TYPE"},
        {0x099, "TEXUNIT2_BORDER_COLOR"},
        {0x09A, "TEXUNIT2_DIM"},
        {0x09B, "TEXUNIT2_PARAM"},
        {0x09C, "TEXUNIT2_LOD"},
        {0x09D, "TEXUNIT2_ADDR"},
        {0x09E, "TEXUNIT2_TYPE"},
        {0x0A8, "TEXUNIT3_PROCTEX0"},
        {0x0A9, "TEXUNIT3_PROCTEX1"},
        {0x0AA, "TEXUNIT3_PROCTEX2"},
        {0x0AB, "TEXUNIT3_PROCTEX3"},
        {0x0AC, "TEXUNIT3_PROCTEX4"},
        {0x0AD, "TEXUNIT3_PROCTEX5"},
        {0x0AF, "PROCTEX_LUT"},
        {0x0B0, "PROCTEX_LUT_DATA0"},
        {0x0C0, "TEXENV0_SOURCE"},
        {0x0C1, "TEXENV0_OPERAND"},
        {0x0C2, "TEXENV0_COMBINER"},
        {0x0C3, "TEXENV0_COLOR"},
        {0x0C4, "TEXENV0_SCALE"},
        {0x0C8, "TEXENV1_SOURCE"},
        {0x0C9, "TEXENV1_OPERAND"},
        {0x0CA, "TEXENV1_COMBINER"},
        {0x0CB, "TEXENV1_COLOR"},
        {0x0CC, "TEXENV1_SCALE"},
        {0x0D0, "TEXENV2_SOURCE"},
        {0x0D1, "TEXENV2_OPERAND"},
        {0x0D2, "TEXENV2_COMBINER"},
        {0x0D3, "TEXENV2_COLOR"},
        {0x0D4, "TEXENV2_SCALE"},
        {0x0D8, "TEXENV3_SOURCE"},
        {0x0D9, "TEXENV3_OPERAND"},
        {0x0DA, "TEXENV3_COMBINER"},
        {0x0DB, "TEXENV3_COLOR"},
        {0x0DC, "TEXENV3_SCALE"},
        {0x0E0, "TEXENV_UPDATE_BUFFER"},
        {0x0E1, "FOG_COLOR"},
        {0x0E4, "GAS_ATTENUATION"},
        {0x0E5, "GAS_ACCMAX"},
        {0x0E6, "FOG_LUT_INDEX"},
        {0x0E8, "FOG_LUT_DATA0"},
        {0x0F0, "TEXENV4_SOURCE"},
        {0x0F1, "TEXENV4_OPERAND"},
        {0x0F2, "TEXENV4_COMBINER"},
        {0x0F3, "TEXENV4_COLOR"},
        {0x0F4, "TEXENV4_SCALE"},
        {0x0F8, "TEXENV5_SOURCE"},
        {0x0F9, "TEXENV5_OPERAND"},
        {0x0FA, "TEXENV5_COMBINER"},
        {0x0FB, "TEXENV5_COLOR"},
        {0x0FC, "TEXENV5_SCALE"},
        {0x0FD, "TEXENV_BUFFER_COLOR"},
        {0x100, "COLOR_OPERATION"},
        {0x101, "BLEND_FUNC"},
        {0x102, "LOGIC_OP"},
        {0x103, "BLEND_COLOR"},
        {0x104, "FRAGOP_ALPHA_TEST"},
        {0x105, "STENCIL_TEST"},
        {0x106, "STENCIL_OP"},
        {0x107, "DEPTH_COLOR_MASK"},
        {0x110, "FRAMEBUFFER_INVALIDATE"},
        {0x111, "FRAMEBUFFER_FLUSH"},
        {0x112, "COLORBUFFER_READ"},
        {0x113, "COLORBUFFER_WRITE"},
        {0x114, "DEPTHBUFFER_READ"},
        {0x115, "DEPTHBUFFER_WRITE"},
        {0x116, "DEPTHBUFFER_FORMAT"},
        {0x117, "COLORBUFFER_FORMAT"},
        {0x118, "EARLYDEPTH_TEST2"},
        {0x11B, "FRAMEBUFFER_BLOCK32"},
        {0x11C, "DEPTHBUFFER_LOC"},
        {0x11D, "COLORBUFFER_LOC"},
        {0x11E, "FRAMEBUFFER_DIM"},
        {0x120, "GAS_LIGHT_XY"},
        {0x121, "GAS_LIGHT_Z"},
        {0x122, "GAS_LIGHT_Z_COLOR"},
        {0x123, "GAS_LUT_INDEX"},
        {0x124, "GAS_LUT_DATA"},
        {0x126, "GAS_DELTAZ_DEPTH"},
        {0x130, "FRAGOP_SHADOW"},
        {0x1C0, "LIGHTING (light 0..7, 0x10 each)"},
        {0x1C0 + 0x80, "LIGHTING_AMBIENT"},
        {0x1C2 + 0x80, "LIGHTING_NUM_LIGHTS"},
        {0x1C3 + 0x80, "LIGHTING_CONFIG0"},
        {0x1C4 + 0x80, "LIGHTING_CONFIG1"},
        {0x1C5 + 0x80, "LIGHTING_LUT_INDEX"},
        {0x1C6 + 0x80, "LIGHTING_DISABLE"},
        {0x1C8 + 0x80, "LIGHTING_LUT_DATA0"},
        {0x1D0 + 0x80, "LIGHTING_LUTINPUT_ABS"},
        {0x1D1 + 0x80, "LIGHTING_LUTINPUT_SELECT"},
        {0x1D2 + 0x80, "LIGHTING_LUTINPUT_SCALE"},
        {0x1D9 + 0x80, "LIGHTING_LIGHT_PERMUTATION"},
        {0x200, "ATTRIBBUFFERS_LOC"},
        {0x201, "ATTRIBBUFFERS_FORMAT_LOW"},
        {0x202, "ATTRIBBUFFERS_FORMAT_HIGH"},
        {0x203, "ATTRIBBUFFER0_OFFSET"},
        {0x204, "ATTRIBBUFFER0_CONFIG1"},
        {0x205, "ATTRIBBUFFER0_CONFIG2"},
        {0x227, "INDEXBUFFER_CONFIG"},
        {0x228, "NUMVERTICES"},
        {0x229, "GEOSTAGE_CONFIG"},
        {0x22A, "VERTEX_OFFSET"},
        {0x22D, "POST_VERTEX_CACHE_NUM"},
        {0x22E, "DRAWARRAYS"},
        {0x22F, "DRAWELEMENTS"},
        {0x231, "VTX_FUNC"},
        {0x232, "FIXEDATTRIB_INDEX"},
        {0x233, "FIXEDATTRIB_DATA0"},
        {0x234, "FIXEDATTRIB_DATA1"},
        {0x235, "FIXEDATTRIB_DATA2"},
        {0x238, "CMDBUF_SIZE0"},
        {0x239, "CMDBUF_SIZE1"},
        {0x23A, "CMDBUF_ADDR0"},
        {0x23B, "CMDBUF_ADDR1"},
        {0x23C, "CMDBUF_JUMP0"},
        {0x23D, "CMDBUF_JUMP1"},
        {0x242, "VSH_NUM_ATTR"},
        {0x244, "VSH_COM_MODE"},
        {0x245, "START_DRAW_FUNC0"},
        {0x24A, "VSH_OUTMAP_TOTAL1"},
        {0x251, "VSH_OUTMAP_TOTAL2"},
        {0x252, "GSH_MISC0"},
        {0x253, "GEOSTAGE_CONFIG2"},
        {0x254, "GSH_MISC1"},
        {0x25E, "PRIMITIVE_CONFIG"},
        {0x25F, "RESTART_PRIMITIVE"},
        {0x280, "GSH_BOOLUNIFORM"},
        {0x281, "GSH_INTUNIFORM_I0"},
        {0x289, "GSH_INPUTBUFFER_CONFIG"},
        {0x28A, "GSH_ENTRYPOINT"},
        {0x28B, "GSH_ATTRIBUTES_PERMUTATION_LOW"},
        {0x28C, "GSH_ATTRIBUTES_PERMUTATION_HIGH"},
        {0x28D, "GSH_OUTMAP_MASK"},
        {0x28F, "GSH_CODETRANSFER_END"},
        {0x290, "GSH_FLOATUNIFORM_INDEX"},
        {0x291, "GSH_FLOATUNIFORM_DATA0"},
        {0x29B, "GSH_CODETRANSFER_CONFIG"},
        {0x29C, "GSH_CODETRANSFER_DATA0"},
        {0x2A5, "GSH_OPDESCS_INDEX"},
        {0x2A6, "GSH_OPDESCS_DATA0"},
        {0x2B0, "VSH_BOOLUNIFORM"},
        {0x2B1, "VSH_INTUNIFORM_I0"},
        {0x2B2, "VSH_INTUNIFORM_I1"},
        {0x2B3, "VSH_INTUNIFORM_I2"},
        {0x2B4, "VSH_INTUNIFORM_I3"},
        {0x2B9, "VSH_INPUTBUFFER_CONFIG"},
        {0x2BA, "VSH_ENTRYPOINT"},
        {0x2BB, "VSH_ATTRIBUTES_PERMUTATION_LOW"},
        {0x2BC, "VSH_ATTRIBUTES_PERMUTATION_HIGH"},
        {0x2BD, "VSH_OUTMAP_MASK"},
        {0x2BF, "VSH_CODETRANSFER_END"},
        {0x2C0, "VSH_FLOATUNIFORM_INDEX"},
        {0x2C1, "VSH_FLOATUNIFORM_DATA0"},
        {0x2CB, "VSH_CODETRANSFER_CONFIG"},
        {0x2CC, "VSH_CODETRANSFER_DATA0"},
        {0x2D5, "VSH_OPDESCS_INDEX"},
        {0x2D6, "VSH_OPDESCS_DATA0"},
    };
    return names;
}

u32 R(const std::vector<u32>& regs, u32 internal) {
    const u32 i = InternalBase + internal;
    return i < regs.size() ? regs[i] : 0;
}

u32 Bits(u32 v, u32 lo, u32 n) {
    return (v >> lo) & ((n >= 32) ? ~0u : ((1u << n) - 1));
}

float F24(u32 v) {
    v &= 0xFFFFFF;
    if ((v & 0x7FFFFF) == 0) {
        return 0.0f;
    }
    const u32 sign = v >> 23, exp = (v >> 16) & 0x7F, mant = v & 0xFFFF;
    const u32 f = sign << 31 | (exp + 64) << 23 | mant << 7;
    float out;
    std::memcpy(&out, &f, 4);
    return out;
}

QString Hex(u32 v, int w = 8) {
    return QString("0x%1").arg(v, w, 16, QChar('0'));
}

const char* CompareName(u32 f) {
    static const char* n[] = {"never", "always", "==", "!=", "<", "<=", ">", ">="};
    return n[f & 7];
}

const char* BlendFactor(u32 f) {
    static const char* n[] = {"0",           "1",           "src",         "1-src",
                              "dst",         "1-dst",       "src.a",       "1-src.a",
                              "dst.a",       "1-dst.a",     "const",       "1-const",
                              "const.a",     "1-const.a",   "src.a sat",   "?"};
    return n[f & 15];
}

const char* BlendEq(u32 e) {
    static const char* n[] = {"add", "sub", "rsub", "min", "max", "?", "?", "?"};
    return n[e & 7];
}

const char* TevSource(u32 s) {
    static const char* n[] = {"primary",   "frag primary", "frag secondary", "tex0",
                              "tex1",      "tex2",         "tex3",           "?",
                              "?",         "?",            "?",              "?",
                              "?",         "prev buffer",  "const",          "previous"};
    return n[s & 15];
}

const char* TevOp(u32 o) {
    static const char* n[] = {"replace", "modulate", "add", "add signed", "lerp", "subtract",
                              "dot3 rgb", "dot3 rgba", "mul add", "add mul"};
    return o < 10 ? n[o] : "?";
}

} // namespace

QString RegName(u32 index) {
    if (index >= InternalBase) {
        const u32 i = index - InternalBase;
        const auto& names = InternalNames();
        const auto it = names.find(i);
        if (it != names.end()) {
            return QString("%1 (%2)").arg(it->second).arg(Hex(i, 3));
        }
        return QString("internal %1").arg(Hex(i, 3));
    }
    if (index >= 4 && index < 12) {
        return QString("MEMFILL%1 +%2").arg((index - 4) / 4).arg((index - 4) % 4);
    }
    if (index >= 0x100 && index < 0x180) {
        return QString("LCD FB%1 +%2").arg((index - 0x100) / 0x40).arg(Hex((index - 0x100) % 0x40, 2));
    }
    if (index >= 0x300 && index < 0x30B) {
        return QString("TRANSFER +%1").arg(Hex(index - 0x300, 2));
    }
    return QString("external %1").arg(Hex(index, 3));
}

const char* FormatName(u32 f) {
    static const char* n[] = {"RGBA8", "RGB8", "RGB5A1", "RGB565", "RGBA4", "IA8", "RG8", "I8",
                              "A8",    "IA4",  "I4",     "A4",     "ETC1",  "ETC1A4"};
    return f < 14 ? n[f] : "?";
}

TextureUnit Unit(const std::vector<u32>& regs, u32 unit) {
    static const u32 base[3] = {0x081, 0x091, 0x099};
    static const u32 type_reg[3] = {0x08E, 0x096, 0x09E};
    TextureUnit t{};
    t.enabled = Bits(R(regs, 0x080), unit, 1);
    const u32 b = base[unit];
    const u32 dim = R(regs, b + 1);
    t.height = Bits(dim, 0, 11);
    t.width = Bits(dim, 16, 11);
    const u32 param = R(regs, b + 2);
    t.cube = unit == 0 && (Bits(param, 28, 3) == 1 || Bits(param, 28, 3) == 4);
    t.levels = Bits(R(regs, b + 3), 16, 4) + 1;
    t.address = Bits(R(regs, b + 4), 0, 28) * 8;
    t.format = Bits(R(regs, type_reg[unit]), 0, 4);
    return t;
}

QString DescribeDraw(const Capture& cap, const Draw& d) {
    const auto& r = d.regs;
    QString h;
    h += QString("<h3>Pass %1, draw %2</h3>").arg(d.pass).arg(d.draw);
    const u32 dim = R(r, 0x11E);
    const u32 fw = Bits(dim, 0, 11), fh = Bits(dim, 12, 10) + 1;
    const u32 cfmt = Bits(R(r, 0x117), 16, 3);
    static const char* cf[] = {"RGBA8", "RGB8", "RGB5A1", "RGB565", "RGBA4", "?", "?", "?"};
    static const char* df[] = {"D16", "?", "D24", "D24S8"};
    h += "<b>Target</b><br>";
    h += QString("colour %1 %2 (%3x%4), depth %5 %6<br>")
             .arg(Hex(Bits(R(r, 0x11D), 0, 28) * 8))
             .arg(cf[cfmt])
             .arg(fw)
             .arg(fh)
             .arg(Hex(Bits(R(r, 0x11C), 0, 28) * 8))
             .arg(df[Bits(R(r, 0x116), 0, 2)]);
    h += QString("colour read %1 write %2, depth read %3 write %4<br>")
             .arg(R(r, 0x112) & 0xF)
             .arg(R(r, 0x113) & 0xF)
             .arg(R(r, 0x114) & 3)
             .arg(R(r, 0x115) & 3);
    h += QString("viewport %1 x %2 at (%3, %4), scissor mode %5 (%6, %7)-(%8, %9)<br>")
             .arg(F24(R(r, 0x041)) * 2)
             .arg(F24(R(r, 0x043)) * 2)
             .arg(qint16(Bits(R(r, 0x068), 0, 10)))
             .arg(qint16(Bits(R(r, 0x068), 16, 10)))
             .arg(Bits(R(r, 0x065), 0, 2))
             .arg(Bits(R(r, 0x066), 0, 10))
             .arg(Bits(R(r, 0x066), 16, 10))
             .arg(Bits(R(r, 0x067), 0, 10))
             .arg(Bits(R(r, 0x067), 16, 10));
    static const char* cull[] = {"none", "front (CCW)", "back (CW)", "none"};
    const u32 prim = Bits(R(r, 0x25E), 8, 2);
    static const char* topo[] = {"list", "strip", "fan", "shader"};
    h += QString("<b>Geometry</b><br>%1 vertices, topology %2, %3, cull %4, GS %5<br>")
             .arg(d.vertices)
             .arg(topo[prim])
             .arg(R(r, 0x22F) || Bits(R(r, 0x227), 31, 1) ? "indexed?" : "")
             .arg(cull[Bits(R(r, 0x040), 0, 2)])
             .arg(Bits(R(r, 0x229), 0, 2) == 2 ? "on" : "off");
    const u32 depth = R(r, 0x107);
    const u32 alpha = R(r, 0x104);
    const u32 stencil = R(r, 0x105);
    h += "<b>Tests</b><br>";
    h += QString("depth %1 %2, writes %3; colour mask %4<br>")
             .arg(Bits(depth, 0, 1) ? "on" : "off")
             .arg(CompareName(Bits(depth, 4, 3)))
             .arg(Bits(depth, 12, 1))
             .arg(Bits(depth, 8, 4), 4, 2, QChar('0'));
    h += QString("alpha test %1 %2 %3; stencil %4 %5 ref %6 mask %7<br>")
             .arg(Bits(alpha, 0, 1) ? "on" : "off")
             .arg(CompareName(Bits(alpha, 4, 3)))
             .arg(Bits(alpha, 8, 8))
             .arg(Bits(stencil, 0, 1) ? "on" : "off")
             .arg(CompareName(Bits(stencil, 4, 3)))
             .arg(Bits(stencil, 16, 8))
             .arg(Bits(stencil, 24, 8));
    const u32 op = R(r, 0x100), blend = R(r, 0x101);
    h += "<b>Blending</b><br>";
    if (Bits(op, 8, 1)) {
        h += QString("rgb %1(%2, %3), alpha %4(%5, %6), const %7<br>")
                 .arg(BlendEq(Bits(blend, 0, 3)))
                 .arg(BlendFactor(Bits(blend, 16, 4)))
                 .arg(BlendFactor(Bits(blend, 20, 4)))
                 .arg(BlendEq(Bits(blend, 8, 3)))
                 .arg(BlendFactor(Bits(blend, 24, 4)))
                 .arg(BlendFactor(Bits(blend, 28, 4)))
                 .arg(Hex(R(r, 0x103)));
    } else {
        h += QString("logic op %1<br>").arg(Bits(R(r, 0x102), 0, 4));
    }
    h += "<b>Textures</b><br>";
    for (u32 u = 0; u < 3; u++) {
        const TextureUnit t = Unit(r, u);
        if (!t.enabled) {
            continue;
        }
        h += QString("unit %1: %2 %3x%4 %5, %6 level(s)%7, param %8<br>")
                 .arg(u)
                 .arg(Hex(t.address))
                 .arg(t.width)
                 .arg(t.height)
                 .arg(FormatName(t.format))
                 .arg(t.levels)
                 .arg(t.cube ? ", cube" : "")
                 .arg(Hex(R(r, u == 0 ? 0x083 : u == 1 ? 0x093 : 0x09B)));
    }
    if (Bits(R(r, 0x080), 3, 1) || Bits(R(r, 0x080), 10, 1)) {
        h += QString("proctex on (unit 3), %1<br>").arg(Hex(R(r, 0x0A8)));
    }
    h += "<b>TEV</b><br><table cellspacing=4>";
    static const u32 stages[6] = {0x0C0, 0x0C8, 0x0D0, 0x0D8, 0x0F0, 0x0F8};
    for (u32 s = 0; s < 6; s++) {
        const u32 src = R(r, stages[s]), opnd = R(r, stages[s] + 1), comb = R(r, stages[s] + 2);
        const u32 rgb_op = Bits(comb, 0, 4), a_op = Bits(comb, 16, 4);
        if (rgb_op == 0 && a_op == 0 && Bits(src, 0, 4) == 15 && Bits(src, 16, 4) == 15) {
            continue; // replace previous: a pass-through stage
        }
        h += QString("<tr><td>%1</td><td>rgb %2(%3, %4, %5)</td><td>a %6(%7, %8, %9)</td>"
                     "<td>ops %10 const %11 scale %12</td></tr>")
                 .arg(s)
                 .arg(TevOp(rgb_op))
                 .arg(TevSource(Bits(src, 0, 4)))
                 .arg(TevSource(Bits(src, 4, 4)))
                 .arg(TevSource(Bits(src, 8, 4)))
                 .arg(TevOp(a_op))
                 .arg(TevSource(Bits(src, 16, 4)))
                 .arg(TevSource(Bits(src, 20, 4)))
                 .arg(TevSource(Bits(src, 24, 4)))
                 .arg(Hex(opnd))
                 .arg(Hex(R(r, stages[s] + 3)))
                 .arg(Hex(R(r, stages[s] + 4), 4));
    }
    h += "</table>";
    h += QString("lighting %1, fog mode %2<br>")
             .arg(Bits(R(r, 0x08F), 0, 1) ? "on" : "off")
             .arg(Bits(R(r, 0x0E0), 0, 3));
    h += QString("<b>Programs</b><br>VS entry %1, outputs mask %2, output map total %3<br>")
             .arg(Bits(R(r, 0x2BA), 0, 16))
             .arg(Hex(R(r, 0x2BD), 4))
             .arg(Bits(R(r, 0x04F), 0, 3));
    for (u32 o = 0; o < Bits(R(r, 0x04F), 0, 3); o++) {
        static const char* sem[] = {"pos.x", "pos.y", "pos.z", "pos.w", "quat.x", "quat.y",
                                    "quat.z", "quat.w", "col.r", "col.g", "col.b", "col.a",
                                    "tc0.u", "tc0.v", "tc1.u", "tc1.v", "tc0.w", "?",
                                    "view.x", "view.y", "view.z", "?", "tc2.u", "tc2.v"};
        const u32 m = R(r, 0x050 + o);
        QStringList parts;
        for (u32 c = 0; c < 4; c++) {
            const u32 v = Bits(m, c * 8, 5);
            parts << (v < 24 ? sem[v] : "-");
        }
        h += QString("&nbsp;o%1: %2<br>").arg(o).arg(parts.join(" "));
    }
    (void)cap;
    return h;
}

QString DescribeGsp(const GspCommand& c) {
    const u32 id = c.words[0] & 0xFF;
    const auto& w = c.words;
    switch (id) {
    case 0:
        return QString("DMA %1 -> %2, %3 bytes").arg(Hex(w[1])).arg(Hex(w[2])).arg(w[3]);
    case 1:
        return QString("command list %1, %2 bytes%3")
            .arg(Hex(w[1]))
            .arg(w[2])
            .arg(w[3] & 1 ? ", update gas" : "");
    case 2:
        return QString("memory fill %1-%2 = %3; %4-%5 = %6")
            .arg(Hex(w[1]))
            .arg(Hex(w[3]))
            .arg(Hex(w[2]))
            .arg(Hex(w[4]))
            .arg(Hex(w[6]))
            .arg(Hex(w[5]));
    case 3:
        return QString("display transfer %1 (%2x%3) -> %4 (%5x%6), flags %7")
            .arg(Hex(w[1]))
            .arg(w[3] & 0xFFFF)
            .arg(w[3] >> 16)
            .arg(Hex(w[2]))
            .arg(w[4] & 0xFFFF)
            .arg(w[4] >> 16)
            .arg(Hex(w[5]));
    case 4:
        return QString("texture copy %1 -> %2, %3 bytes, gaps %4 %5, flags %6")
            .arg(Hex(w[1]))
            .arg(Hex(w[2]))
            .arg(w[3])
            .arg(Hex(w[4]))
            .arg(Hex(w[5]))
            .arg(Hex(w[6]));
    case 5:
        return "cache flush";
    default:
        return QString("GSP command %1").arg(id);
    }
}

namespace {

u32 Morton8(u32 x, u32 y) {
    u32 m = 0;
    for (u32 b = 0; b < 3; b++) {
        m |= ((x >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
    }
    return m;
}

// ETC1 (3dbrew / upstream texture_decode's etc1 decode): one 4x4 block of a 64-bit word.
void Etc1Block(u64 word, u32 x, u32 y, u8 out[4]) {
    static const int table[8][2] = {{2, 8},   {5, 17},  {9, 29},   {13, 42},
                                    {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    const bool flip = (word >> 32) & 1;
    const bool diff = (word >> 33) & 1;
    const u32 tables[2] = {u32((word >> 37) & 7), u32((word >> 34) & 7)};
    const u32 hi = u32(word >> 32);
    int base[2][3];
    if (diff) {
        for (int c = 0; c < 3; c++) {
            const int shift = 27 - c * 8;
            const int b5 = (hi >> shift) & 31;
            int d = (hi >> (shift - 3)) & 7;
            if (d >= 4) {
                d -= 8;
            }
            const int b5b = std::clamp(b5 + d, 0, 31);
            base[0][c] = (b5 << 3) | (b5 >> 2);
            base[1][c] = (b5b << 3) | (b5b >> 2);
        }
    } else {
        for (int c = 0; c < 3; c++) {
            const int shift = 28 - c * 8;
            const int a = (hi >> shift) & 15, b = (hi >> (shift - 4)) & 15;
            base[0][c] = a * 17;
            base[1][c] = b * 17;
        }
    }
    const u32 sub = flip ? (y >= 2) : (x >= 2);
    const u32 i = x * 4 + y;
    const u32 lo = u32(word);
    const int msb = (lo >> (16 + i)) & 1, lsb = (lo >> i) & 1;
    int mod = table[tables[sub]][lsb];
    if (msb) {
        mod = -mod;
    }
    for (int c = 0; c < 3; c++) {
        out[c] = u8(std::clamp(base[sub][c] + mod, 0, 255));
    }
    out[3] = 255;
}

} // namespace

QImage DecodeTexture(const u8* data, size_t size, u32 w, u32 h, u32 format) {
    if (!data || w == 0 || h == 0 || w % 8 || h % 8 || format > 13) {
        return {};
    }
    static const u32 bits[14] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8};
    if (size_t(w) * h * bits[format] / 8 > size) {
        return {};
    }
    QImage img(int(w), int(h), QImage::Format_RGBA8888);
    const u32 tiles_x = w / 8;
    for (u32 y = 0; y < h; y++) {
        for (u32 x = 0; x < w; x++) {
            const u32 tile = (y / 8) * tiles_x + x / 8;
            u8 px[4] = {0, 0, 0, 255};
            if (format == 12 || format == 13) {
                // ETC: 8x8 tiles of four 4x4 blocks in Z order, each 8 bytes (ETC1A4: an alpha
                // word before it).
                const u32 bx = (x % 8) / 4, by = (y % 8) / 4;
                const u32 block = tile * 4 + by * 2 + bx;
                const u32 stride = format == 13 ? 16 : 8;
                const u8* p = data + size_t(block) * stride;
                u64 alpha = ~0ull;
                if (format == 13) {
                    std::memcpy(&alpha, p, 8);
                    p += 8;
                }
                u64 word;
                std::memcpy(&word, p, 8);
                Etc1Block(word, x % 4, y % 4, px);
                if (format == 13) {
                    const u32 a = (alpha >> (((x % 4) * 4 + (y % 4)) * 4)) & 15;
                    px[3] = u8(a * 17);
                }
            } else {
                const u32 texel = tile * 64 + Morton8(x % 8, y % 8);
                const size_t at = size_t(texel) * bits[format] / 8;
                const u8* p = data + at;
                u16 v16 = u16(p[0] | (bits[format] >= 16 ? p[1] << 8 : 0));
                switch (format) {
                case 0: px[0] = p[3]; px[1] = p[2]; px[2] = p[1]; px[3] = p[0]; break;
                case 1: px[0] = p[2]; px[1] = p[1]; px[2] = p[0]; break;
                case 2:
                    px[0] = u8(((v16 >> 11) & 31) * 255 / 31);
                    px[1] = u8(((v16 >> 6) & 31) * 255 / 31);
                    px[2] = u8(((v16 >> 1) & 31) * 255 / 31);
                    px[3] = (v16 & 1) ? 255 : 0;
                    break;
                case 3:
                    px[0] = u8(((v16 >> 11) & 31) * 255 / 31);
                    px[1] = u8(((v16 >> 5) & 63) * 255 / 63);
                    px[2] = u8((v16 & 31) * 255 / 31);
                    break;
                case 4:
                    px[0] = u8(((v16 >> 12) & 15) * 17);
                    px[1] = u8(((v16 >> 8) & 15) * 17);
                    px[2] = u8(((v16 >> 4) & 15) * 17);
                    px[3] = u8((v16 & 15) * 17);
                    break;
                case 5: px[0] = px[1] = px[2] = p[1]; px[3] = p[0]; break;
                case 6: px[0] = p[1]; px[1] = p[0]; px[2] = 0; break;
                case 7: px[0] = px[1] = px[2] = p[0]; break;
                case 8: px[0] = px[1] = px[2] = 0; px[3] = p[0]; break;
                case 9: px[0] = px[1] = px[2] = u8((p[0] >> 4) * 17); px[3] = u8((p[0] & 15) * 17); break;
                case 10:
                case 11: {
                    const u8 nib = (texel & 1) ? (p[0] >> 4) : (p[0] & 15);
                    if (format == 10) {
                        px[0] = px[1] = px[2] = u8(nib * 17);
                    } else {
                        px[0] = px[1] = px[2] = 0;
                        px[3] = u8(nib * 17);
                    }
                    break;
                }
                }
            }
            // The PICA's row 0 is the bottom: show it upright.
            std::memcpy(img.scanLine(int(h - 1 - y)) + x * 4, px, 4);
        }
    }
    return img;
}

QImage DecodeColour(const u8* data, size_t size, u32 w, u32 h, u32 format) {
    // The colour formats share the texture formats' codes 0-4 and layout.
    return DecodeTexture(data, size, w, h, format);
}

namespace {

QString Swizzle(u32 sel, bool neg) {
    static const char c[] = "xyzw";
    QString s;
    for (int i = 3; i >= 0; i--) {
        s += c[(sel >> (i * 2)) & 3];
    }
    return (neg ? "-" : "") + (s == "xyzw" ? QString() : "." + s);
}

QString Src(u32 v, u32 idx, bool relative) {
    QString n;
    if (v < 0x10) {
        n = QString("v%1").arg(v);
    } else if (v < 0x20) {
        n = QString("r%1").arg(v - 0x10);
    } else {
        n = QString("c%1").arg(v - 0x20);
    }
    if (relative && idx) {
        static const char* a[] = {"", "a0.x", "a0.y", "aL"};
        n += QString("[%1]").arg(a[idx & 3]);
    }
    return n;
}

QString Dst(u32 v) {
    return v < 0x10 ? QString("o%1").arg(v) : QString("r%1").arg(v - 0x10);
}

QString Mask(u32 m) {
    QString s;
    if (m & 8) s += 'x';
    if (m & 4) s += 'y';
    if (m & 2) s += 'z';
    if (m & 1) s += 'w';
    return s == "xyzw" ? QString() : "." + s;
}

} // namespace

QString Disassemble(const Program& p, u32 entry, u32 max) {
    QString out;
    static const char* arith[0x20] = {"add", "dp3", "dp4", "dph", "dst", "ex2", "lg2", "litp",
                                      "mul", "sge", "slt", "flr", "max", "min", "rcp", "rsq",
                                      "?",   "?",   "mova", "mov", "?",  "?",   "?",   "?",
                                      "dphi", "dsti", "sgei", "slti", "?", "?", "?", "?"};
    u32 last_end = 0;
    for (u32 pc = entry; pc < p.code.size() && pc < entry + max; pc++) {
        const u32 w = p.code[pc];
        const u32 op = w >> 26;
        QString line = QString("%1: %2  ").arg(pc, 3, 16, QChar('0')).arg(w, 8, 16, QChar('0'));
        if (op < 0x20) {
            const bool inv = op >= 0x18;
            const u32 desc = p.swizzle.size() > (w & 0x7F) ? p.swizzle[w & 0x7F] : 0;
            const u32 idx = Bits(w, 19, 2);
            const u32 s1 = inv ? Bits(w, 14, 5) : Bits(w, 12, 7);
            const u32 s2 = inv ? Bits(w, 7, 7) : Bits(w, 7, 5);
            const QString a = Src(s1, idx, !inv) + Swizzle(Bits(desc, 5, 8), Bits(desc, 4, 1));
            const QString b = Src(s2, idx, inv) + Swizzle(Bits(desc, 14, 8), Bits(desc, 13, 1));
            if (op == 0x12) {
                line += QString("mova a0%1, %2").arg(Mask(Bits(desc, 0, 4))).arg(a);
            } else if (op == 0x05 || op == 0x06 || op == 0x07 || op == 0x0B || op == 0x0E ||
                       op == 0x0F || op == 0x13) {
                line += QString("%1 %2%3, %4").arg(arith[op], Dst(Bits(w, 21, 5)), Mask(Bits(desc, 0, 4)), a);
            } else {
                line += QString("%1 %2%3, %4, %5")
                            .arg(arith[op], Dst(Bits(w, 21, 5)), Mask(Bits(desc, 0, 4)), a, b);
            }
        } else if (op >= 0x30) {
            const bool madi = op < 0x38;
            const u32 desc = p.swizzle.size() > (w & 0x1F) ? p.swizzle[w & 0x1F] : 0;
            const u32 idx = Bits(w, 22, 2);
            const u32 s1 = Bits(w, 17, 5);
            const u32 s2 = madi ? Bits(w, 12, 5) : Bits(w, 10, 7);
            const u32 s3 = madi ? Bits(w, 5, 7) : Bits(w, 5, 5);
            line += QString("%1 %2%3, %4, %5, %6")
                        .arg(madi ? "madi" : "mad", Dst(Bits(w, 24, 5)), Mask(Bits(desc, 0, 4)),
                             Src(s1, 0, false) + Swizzle(Bits(desc, 5, 8), Bits(desc, 4, 1)),
                             Src(s2, idx, !madi) + Swizzle(Bits(desc, 14, 8), Bits(desc, 13, 1)),
                             Src(s3, idx, madi) + Swizzle(Bits(desc, 23, 8), Bits(desc, 22, 1)));
        } else {
            const u32 n = Bits(w, 0, 8), dest = Bits(w, 10, 12);
            const u32 cond = Bits(w, 22, 2);
            static const char* cops[] = {"||", "&&", "x", "y"};
            const QString cc = QString("%1cc.x %2 %3cc.y")
                                   .arg(Bits(w, 25, 1) ? "" : "!")
                                   .arg(cops[cond])
                                   .arg(Bits(w, 24, 1) ? "" : "!");
            switch (op) {
            case 0x20: line += "break"; break;
            case 0x21: line += "nop"; break;
            case 0x22: line += "end"; break;
            case 0x23: line += QString("breakc %1").arg(cc); break;
            case 0x24: line += QString("call %1, %2").arg(dest, 0, 16).arg(n); break;
            case 0x25: line += QString("callc %1, %2, %3").arg(cc).arg(dest, 0, 16).arg(n); break;
            case 0x26: line += QString("callu b%1, %2, %3").arg(Bits(w, 22, 4)).arg(dest, 0, 16).arg(n); break;
            case 0x27: line += QString("ifu b%1, else %2, %3").arg(Bits(w, 22, 4)).arg(dest, 0, 16).arg(n); break;
            case 0x28: line += QString("ifc %1, else %2, %3").arg(cc).arg(dest, 0, 16).arg(n); break;
            case 0x29: line += QString("loop i%1, to %2").arg(Bits(w, 22, 2)).arg(dest, 0, 16); break;
            case 0x2A: line += "emit"; break;
            case 0x2B:
                line += QString("setemit %1%2%3").arg(Bits(w, 24, 2)).arg(Bits(w, 23, 1) ? ", prim" : "").arg(Bits(w, 22, 1) ? ", inv" : "");
                break;
            case 0x2C: line += QString("jmpc %1, %2").arg(cc).arg(dest, 0, 16); break;
            case 0x2D: line += QString("jmpu %1b%2, %3").arg(Bits(w, 0, 1) ? "!" : "").arg(Bits(w, 22, 4)).arg(dest, 0, 16); break;
            case 0x2E:
            case 0x2F: {
                const u32 desc = p.swizzle.size() > (w & 0x7F) ? p.swizzle[w & 0x7F] : 0;
                static const char* cmp[] = {"==", "!=", "<", "<=", ">", ">=", "?", "?"};
                line += QString("cmp %1, %2 %3, %4 %5")
                            .arg(Src(Bits(w, 12, 7), Bits(w, 19, 2), true) + Swizzle(Bits(desc, 5, 8), Bits(desc, 4, 1)))
                            .arg(cmp[Bits(w, 24, 3)])
                            .arg(cmp[Bits(w, 21, 3)])
                            .arg(Src(Bits(w, 7, 5), 0, false) + Swizzle(Bits(desc, 14, 8), Bits(desc, 13, 1)))
                            .arg("");
                break;
            }
            default: line += QString("op %1").arg(op, 0, 16);
            }
            if (op == 0x22) {
                out += line + "\n";
                last_end = pc;
                // Subroutines may follow the main end; stop at a second consecutive end or
                // a run of zero words.
                if (pc + 1 < p.code.size() && p.code[pc + 1] == 0) {
                    break;
                }
                continue;
            }
        }
        out += line + "\n";
    }
    (void)last_end;
    return out;
}

} // namespace frame_replay::pica
