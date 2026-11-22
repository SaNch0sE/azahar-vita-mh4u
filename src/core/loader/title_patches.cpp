// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <span>
#include <vector>
#include <fmt/format.h>
#include "common/common_types.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/hle/kernel/process.h"
#include "core/loader/title_patches.h"
#include "core/memory.h"

namespace Loader {

namespace {

const char* (*translator)(const char*) = nullptr;

const char* Tr(const char* english) {
    return translator != nullptr ? translator(english) : english;
}

/// A literal a menu translates where it shows it (the frontend's check of its translations finds
/// these too).
constexpr const char* Translatable(const char* english) {
    return english;
}

struct PatchWord {
    VAddr addr;
    u32 original;
    u32 patched;
};

struct TitlePatch {
    u64 title_id;
    const char* name;
    const char* description;
    std::span<const PatchWord> words;
    /// Brings the game's data along when the patch goes into running code (a state's load
    /// saved without it), or nullptr.
    void (*applied)(Memory::MemorySystem& memory, const Kernel::Process& process) = nullptr;
};

// Xenoblade Chronicles 3D (USA). It draws every object of its 3D scene twice, once an eye, into
// one buffer, whatever the 3D slider says; the right eye is never shown here. The engine's eye
// mode byte (0 both eyes, 1 the left, 2 the right) is set to the left where it was set to both.
constexpr std::array xenoblade_left_eye{
    PatchWord{0x00107cfc, 0xe320f000, 0xe3a01001}, // nop -> mov r1, #1
    PatchWord{0x00107d08, 0xe5c08010, 0xe5c01010}, // init: strb r1, [r0, #16]
    PatchWord{0x00300e40, 0xe320f000, 0xe3a05001}, // nop -> mov r5, #1
    PatchWord{0x00300e50, 0xe5c01010, 0xe5c05010}, // stereo on: strb r5, [r0, #16]
    PatchWord{0x001592dc, 0xe3a01000, 0xe3a01001}, // after a one-eye pass: mov r1, #1
};

/// A routine in zero padding at `at` (the code's words there must be 0), entered by `hook`.
template <std::size_t N>
constexpr std::array<PatchWord, N + 1> HookedRoutine(PatchWord hook, VAddr at,
                                                     const std::array<u32, N>& routine) {
    std::array<PatchWord, N + 1> words{};
    words[0] = hook;
    for (std::size_t i = 0; i < N; i++) {
        words[i + 1] = {static_cast<VAddr>(at + i * 4), 0, routine[i]};
    }
    return words;
}

// Xenoblade Chronicles 3D (USA). Its engine keeps the Wii's GX immediate mode: each GXBegin is a
// draw of its own from a per-frame vertex heap, and its text and UI are quads one at a time
// (GX_QUADS of 4 vertices, or fans of 4), most of its draws. Its draw
// emitter (0x00131b74) indexes the vertices through one of three tables: a strip's, a fan's, or
// separate quads' (table C, 1024 quads). The routine at its entry joins a quad to the draw right
// before it in the list when that draw is table C's and its quads end where these start: nothing
// written to the list in between (no state change: every one writes commands first), its count
// takes the new indices and nothing is emitted. A fan of 4 vertices is table C's first quad, so it
// goes through table C. The routine (at the zero padding past the code's end, 0x00478800):
//
//     push {r4-r9}; ldr r4, =0x004c2010; ldr r4, [r4]           @ the vertex heap S
//     add r5, r4, #0x186008 (three adds); sub r6, r5, #0x3000   @ table C, table B
//     cmp r2, r6; cmpeq r3, #6; moveq r2, r5                    @ a fan of 4: a table-C quad
//     cmp r2, r5; bne out
//     ldr r6, [r0]; ldr r7, [r6, #-4]; cmp r7, #0x0008025e; bne out   @ a draw tail's end
//     for r9 in 72, 112, 160:                       @ its header, before a tail of 12, 22, 34 words
//         r7 = r6 - r9; if [r7+4], [r7+12], [r7+20] == 0xf0200, 0xf0227, 0xf0228: found
//     out if not found
//     found: r8 = r5 - ([r7+8] & 0x7fffffff)                    @ its vertices, were it table C's
//         out unless [r7] == (r8 - 0x10000000) >> 3             @ it was
//         out unless (r1 - r8) * 3 == [r7+16] * 2 * [0x004e8044] @ its quads end where these start
//         out if [r7+16] + r3 > 0x1800; [r7+16] += r3; pop {r4-r9}; bx lr
//     out: pop {r4-r9}; push {r4-r8, lr}; b 0x00131b78           @ the emitter as it was
constexpr std::array<u32, 68> xenoblade_quads_routine{
    0xe92d03f0, 0xe59f40ec, 0xe5944000, 0xe2845706, 0xe2855a06, 0xe2855008, 0xe2456a03, 0xe1520006,
    0x03530006, 0x01a02005, 0xe1520005, 0x1a00002e, 0xe5906000, 0xe5167004, 0xe59f80bc, 0xe1570008,
    0x1a000029, 0xe3a09048, 0xe0467009, 0xe5978004, 0xe59fc0a8, 0xe158000c, 0x0597800c, 0x059fc0a0,
    0x0158000c, 0x05978014, 0x059fc098, 0x0158000c, 0x0a000006, 0xe3590048, 0x03a09070, 0x0afffff1,
    0xe3590070, 0x03a090a0, 0x0affffee, 0xea000016, 0xe5978008, 0xe3c88102, 0xe0458008, 0xe248c201,
    0xe1a0c1ac, 0xe5979000, 0xe159000c, 0x1a00000e, 0xe5979010, 0xe59fc050, 0xe59cc000, 0xe00c0c99,
    0xe1a0c08c, 0xe0416008, 0xe0866086, 0xe156000c, 0x1a000005, 0xe0899003, 0xe3590b06, 0x8a000002,
    0xe5879010, 0xe8bd03f0, 0xe12fff1e, 0xe8bd03f0, 0xe92d41f0, 0xeaf2e49f, 0x004c2010, 0x0008025e,
    0x000f0200, 0x000f0227, 0x000f0228, 0x004e8044,
};
constexpr auto xenoblade_quads =
    HookedRoutine(PatchWord{0x00131b74, 0xe92d41f0, 0xea0d1b21}, // push {r4-r8, lr} -> b 0x00478800
                  0x00478800, xenoblade_quads_routine);

// Xenoblade Chronicles 3D (USA). A texture bind (0x0030879c, then 0x00319754) writes the texture
// object's sampler words (+0x1c, +0x20) whether they change or not, and each object sits right
// before its own pixels: stores into pages queued GPU work reads, each a page version taken.
// Stored only when they change: 0x003087f8's
// store follows its compare (cmp r2, r1); 0x00319754's first store moves into the tail, which
// compares each word and keeps the result ("changed"):
//
//     lsr r2, ip, #13; orr r1, r1, r2, lsl #13       @ the new +0x20
//     cmp r1, ip; strne r1, [r0, #32]
//     cmpeq r4, r3; strne r4, [r0, #28]              @ the new +0x1c against the old
//     pop {r4, r5}; movne r0, #1; moveq r0, #0; bx lr
constexpr std::array xenoblade_texture_binds{
    PatchWord{0x003087f8, 0xe585201c, 0x1585201c}, // str r2, [r5, #28] -> strne
    PatchWord{0x003197b0, 0xe580401c, 0xe320f000}, // str r4, [r0, #28] -> nop (into the tail)
    PatchWord{0x00319824, 0xe1a026ac, 0xe1a026ac}, // lsr r2, ip, #13
    PatchWord{0x00319828, 0xe1540003, 0xe1811682}, // cmp r4, r3 -> orr r1, r1, r2, lsl #13
    PatchWord{0x0031982c, 0xe1a02682, 0xe151000c}, // lsl r2, r2, #13 -> cmp r1, ip
    PatchWord{0x00319830, 0xe1811002, 0x15801020}, // orr r1, r1, r2 -> strne r1, [r0, #32]
    PatchWord{0x00319834, 0xe5801020, 0x01540003}, // str r1, [r0, #32] -> cmpeq r4, r3
    PatchWord{0x00319838, 0x0151000c, 0x1580401c}, // cmpeq r1, ip -> strne r4, [r0, #28]
    PatchWord{0x0031983c, 0xe8bd0030, 0xe8bd0030}, // pop {r4, r5}
    PatchWord{0x00319840, 0x13a00001, 0x13a00001}, // movne r0, #1
    PatchWord{0x00319844, 0x03a00000, 0x03a00000}, // moveq r0, #0
    PatchWord{0x00319848, 0xe12fff1e, 0xe12fff1e}, // bx lr
};

// Xenoblade Chronicles 3D (USA). The mesh loop (0x00109b00) stores each mesh object's visibility
// byte (+0x16) every frame, and the object sits next to its vertex data: stores into pages
// queued GPU work reads, each a page version. Stored only when it changes, by a routine past
// the quads routine, entered by a branch (lr is the loop's own base there); ip is free (reloaded
// at the loop's head, its exit makes a call first):
//
//     ldrb ip, [r1, #22]; cmp ip, r2; strbne r2, [r1, #22]
//     cmp r2, #0; b 0x00109c18                       @ the flags the loop reads next
constexpr std::array<u32, 5> xenoblade_mesh_visibility_routine{
    0xe5d1c016, 0xe15c0002, 0x15c12016, 0xe3520000, 0xeaf244b8,
};
constexpr auto xenoblade_mesh_visibility =
    HookedRoutine(PatchWord{0x00109c14, 0xe5c12016, 0xea0dbb41}, // strb r2, [r1, #22] -> b 0x00478920
                  0x00478920, xenoblade_mesh_visibility_routine);

// Xenoblade Chronicles 3D (USA). A watcher: NW4R's ResFile::Init (0x002dd3cc), which every load
// of a resource file calls (its seven callers), names the file to the emulator (svc 0xF0, event 1:
// its base and its size, the u32 at +8), whose lists' scan then holds no page for the textures,
// indices and vertices the GPU reads inside it (VideoCore::GPU::WatchResource). The routine past
// the mesh visibility routine:
//
//     push {r0-r3, ip, lr}; ldr r1, [r0]; ldr r2, [r1, #8]; mov r0, #1; svc 0xf0
//     pop {r0-r3, ip, lr}; push {r4-r7, lr}; b 0x002dd3d0      @ Init as it was
constexpr std::array<u32, 8> xenoblade_resource_watcher_routine{
    0xe92d500f, 0xe5901000, 0xe5912008, 0xe3a00001,
    0xef0000f0, 0xe8bd500f, 0xe92d40f0, 0xeaf9929b,
};
constexpr auto xenoblade_resource_watcher =
    HookedRoutine(PatchWord{0x002dd3cc, 0xe92d40f0, 0xea066d5b}, // push {r4-r7, lr} -> b 0x00478940
                  0x00478940, xenoblade_resource_watcher_routine);

// Xenoblade Chronicles 3D (USA). The engine writes each frame's command lists and immediate-mode
// vertices into one of two 768 KiB buffers at the vertex heap's start (each followed by its bump
// offset word), turn about: the frame's start (0x00119364) flushes the one it filled and turns to
// the other, which the GPU read two frames ago. The emulator's GPU is about that far behind at the
// turn, and the game
// uses at most ~250 KiB of a buffer: three buffers of 512 KiB in the same 0x180000 bytes, each
// with its offset word in its last 4 bytes (base + 0x7fffc), the turn counting 0, 1, 2. Every
// place that reached the word at base + 0xc0000 reaches base + 0x80000 - 4 now; the allocator's
// overflow check (0x0030a130) stops at base + 0x7f000.
constexpr std::array xenoblade_three_frame_buffers{
    PatchWord{0x00109190, 0xe3a00703, 0xe2840702}, // init: add r0, r4, #0x80000
    PatchWord{0x001091a8, 0xe7804004, 0xe5004004}, // str r4, [r0, #-4]
    PatchWord{0x00119380, 0xe3a01703, 0xe594000c}, // turn: ldr r0, [r4, #12]
    PatchWord{0x00119384, 0xe594000c, 0xe2801702}, // add r1, r0, #0x80000
    PatchWord{0x00119388, 0xe7911000, 0xe5111004}, // ldr r1, [r1, #-4]
    PatchWord{0x0011939c, 0xe2800001, 0xe2800001}, // add r0, r0, #1
    PatchWord{0x001193a0, 0xe2000001, 0xe3500003}, // cmp r0, #3
    PatchWord{0x001193a4, 0xe5840008, 0x23a00000}, // movcs r0, #0
    PatchWord{0x001193a8, 0xe0802800, 0xe5840008}, // str r0, [r4, #8]
    PatchWord{0x001193ac, 0xe0820880, 0xe0850980}, // add r0, r5, r0, lsl #19
    PatchWord{0x001193b0, 0xe3a02703, 0xe2802702}, // add r2, r0, #0x80000
    PatchWord{0x001193b4, 0xe0850100, 0xe584000c}, // str r0, [r4, #12]
    PatchWord{0x001193b8, 0xe584000c, 0xe5020004}, // str r0, [r2, #-4]
    PatchWord{0x001193bc, 0xe7820000, 0xe5c41010}, // strb r1, [r4, #16]
    PatchWord{0x0030a110, 0xe2800703, 0xe2800702}, // allocator: add r0, r0, #0x80000
    PatchWord{0x0030a118, 0xe5901000, 0xe5101004}, // ldr r1, [r0, #-4]
    PatchWord{0x0030a124, 0xe5801000, 0xe5001004}, // str r1, [r0, #-4]
    PatchWord{0x0030a130, 0xe2800703, 0xe2800a7f}, // add r0, r0, #0x7f000 (the check's end)
    PatchWord{0x0030a150, 0xe3a01703, 0xe2801702}, // overflow: add r1, r0, #0x80000
    PatchWord{0x0030a154, 0xe7810000, 0xe5010004}, // str r0, [r1, #-4]
    PatchWord{0x0030a1b4, 0xe2800703, 0xe2800702}, // add r0, r0, #0x80000
    PatchWord{0x0030a1bc, 0xe5905000, 0xe5105004}, // ldr r5, [r0, #-4]
    PatchWord{0x0030a5b0, 0xe2811703, 0xe2811702}, // bump: add r1, r1, #0x80000
    PatchWord{0x0030a5b4, 0xe5910000, 0xe5110004}, // ldr r0, [r1, #-4]
    PatchWord{0x0030a5bc, 0xe5812000, 0xe5012004}, // str r2, [r1, #-4]
    PatchWord{0x003101fc, 0xe2822703, 0xe2822702}, // bump: add r2, r2, #0x80000
    PatchWord{0x00310200, 0xe5921000, 0xe5121004}, // ldr r1, [r2, #-4]
    PatchWord{0x00310208, 0xe5823000, 0xe5023004}, // str r3, [r2, #-4]
};

/// A state saved before the patch: the buffer the frame fills goes on to the next turn, its
/// offset word at base + 0x7fffc (inside the buffer: a frame stays far below 512 KiB).
void XenobladeThreeFrameBuffersApplied(Memory::MemorySystem& memory,
                                       const Kernel::Process& process) {
    // The process's own table: at the code's load it is not the current one yet.
    const auto read = [&](VAddr addr) {
        u32 word = 0;
        memory.ReadBlock(process, addr, &word, sizeof(word));
        return word;
    };
    const VAddr heap = read(0x004c2010);
    if (heap == 0) {
        return; // the code's load: the heap comes later, with the patched init
    }
    const VAddr base = read(heap + 0x18900c);
    const u32 offset = read(base + 0xc0000);
    ASSERT_MSG(offset - base < 0x7f000, "the frame buffer {:#x} {:#x} bytes in", base,
               offset - base);
    memory.WriteBlock(process, base + 0x7fffc, &offset, sizeof(offset));
}

// Xenoblade Chronicles 3D (USA). A watcher: at the turn (the three buffers above, each the GPU
// read three frames ago), the routine names the buffer turned to (svc 0xF0, event 2: its base
// and 0x80000 bytes), and the emulator, which holds no page for reads inside such a buffer,
// waits for the work that read it (none left, as a rule):
//
//     push {r0-r3, ip, lr}; mov r1, r0; mov r2, #0x80000; nop; mov r0, #2; svc 0xf0
//     pop {r0-r3, ip, lr}; strb r1, [r4, #16]; b 0x001193c4      @ the turn as it was
constexpr std::array<u32, 9> xenoblade_frame_buffer_watcher_routine{
    0xe92d500f, 0xe1a01000, 0xe3a02702, 0xe320f000, 0xe3a00002,
    0xef0000f0, 0xe8bd500f, 0xe5c41010, 0xeaf2828f,
};
constexpr auto xenoblade_frame_buffer_watcher =
    HookedRoutine(PatchWord{0x001193c0, 0xe5c41010, 0xea0d7d66}, // strb r1, [r4, #16] -> b 0x00478960
                  0x00478960, xenoblade_frame_buffer_watcher_routine);

constexpr u64 XenobladeUsa = 0x000400000F700100;

constexpr std::array title_patches{
    TitlePatch{XenobladeUsa, Translatable("Left eye only"),
               Translatable("The game draws its 3D scene twice, once per eye, even with 3D off. "
                            "Only one picture is shown, so only the left eye's is drawn."),
               xenoblade_left_eye},
    TitlePatch{XenobladeUsa, Translatable("Joined 2D quads"),
               Translatable("The game draws its text and menus one small square at a time. Squares "
                            "drawn one after another are joined into one draw."),
               xenoblade_quads},
    TitlePatch{XenobladeUsa, Translatable("Fewer texture writes"),
               Translatable("The game rewrites a texture's settings every time it uses one. Now "
                            "they are written only when they change, which saves work."),
               xenoblade_texture_binds},
    TitlePatch{XenobladeUsa, Translatable("Fewer model writes"),
               Translatable("The game rewrites each model's visibility every frame. Now it is "
                            "written only when it changes, which saves work."),
               xenoblade_mesh_visibility},
    TitlePatch{XenobladeUsa, Translatable("Model files reported"),
               Translatable("The game tells Azahar where the model and texture files it loads are, "
                            "so Azahar has less to keep track of."),
               xenoblade_resource_watcher},
    TitlePatch{XenobladeUsa, Translatable("Three command buffers"),
               Translatable("The game takes turns between two buffers for each frame's GPU "
                            "commands. With three, it waits less for the GPU."),
               xenoblade_three_frame_buffers, XenobladeThreeFrameBuffersApplied},
    TitlePatch{XenobladeUsa, Translatable("Command buffer reported"),
               Translatable("The game tells Azahar which command buffer each frame fills, so "
                            "Azahar has less to keep track of."),
               xenoblade_frame_buffer_watcher},
};

/// A game the patches above are made for, as a player tells a copy of it: its product code (on
/// the cartridge's label; the last letter is the region) and the code's revision (the exheader's
/// remaster version).
struct PatchedTitle {
    u64 title_id;
    const char* name;
    const char* product_code;
    const char* region;
    u16 remaster_version;
};

constexpr std::array patched_titles{
    PatchedTitle{XenobladeUsa, "Xenoblade Chronicles 3D", "KTR-P-CAFE", Translatable("USA"), 2},
};

/// The region a product code's last letter names.
const char* RegionOf(std::string_view product_code) {
    if (product_code.empty()) {
        return Translatable("another region");
    }
    switch (product_code.back()) {
    case 'E':
        return Translatable("USA");
    case 'P':
        return Translatable("Europe");
    case 'J':
        return Translatable("Japan");
    case 'K':
        return Translatable("Korea");
    case 'C':
        return Translatable("China");
    case 'T':
        return Translatable("Taiwan");
    case 'U':
        return Translatable("Australia");
    default:
        return Translatable("another region");
    }
}

} // namespace

std::string TitlePatchesWarning(u64 title_id, std::string_view product_code, u16 remaster_version,
                                const std::vector<bool>& in) {
    for (const PatchedTitle& t : patched_titles) {
        const std::string_view wanted{t.product_code};
        const std::string_view stem = wanted.substr(0, wanted.size() - 1);
        if (title_id == t.title_id) {
            const auto left_out = std::count(in.begin(), in.end(), false);
            if (left_out == 0) {
                return {};
            }
            return fmt::format(
                fmt::runtime(Tr("For full speed, play {} from the {} copy ({}), revision {}, "
                                "without an update. This copy is revision {} or has an update "
                                "installed, so {} of its {} speed patches are left out.")),
                t.name, Tr(t.region), t.product_code, t.remaster_version, remaster_version,
                left_out, in.size());
        }
        if (product_code.size() == wanted.size() && product_code.starts_with(stem)) {
            return fmt::format(
                fmt::runtime(Tr("For full speed, play {} from the {} copy ({}), revision {}, "
                                "without an update. This copy is from {} ({}), so it runs without "
                                "Azahar's speed patches.")),
                t.name, Tr(t.region), t.product_code, t.remaster_version,
                Tr(RegionOf(product_code)), product_code);
        }
    }
    return {};
}

void SetTitlePatchesTranslator(const char* (*translate)(const char* english)) {
    translator = translate;
}

std::vector<TitlePatchInfo> TitlePatchesOf(u64 title_id) {
    std::vector<TitlePatchInfo> patches;
    for (const TitlePatch& patch : title_patches) {
        if (patch.title_id == title_id) {
            patches.push_back({patch.name, patch.description});
        }
    }
    return patches;
}

std::vector<bool> ApplyTitlePatches(Core::System& system, const Kernel::Process& process) {
    std::vector<bool> in;
    if (process.codeset == nullptr) {
        return in;
    }
    const u64 title_id = process.codeset->program_id;
    auto& memory = system.Memory();
    for (const TitlePatch& patch : title_patches) {
        if (patch.title_id != title_id) {
            continue;
        }
        // Each word as the title has it, or as the patch has it (a state saved with the patch).
        bool original = true;
        bool patched = true;
        for (const PatchWord& w : patch.words) {
            u32 word = 0;
            memory.ReadBlock(process, w.addr, &word, sizeof(word));
            original &= word == w.original;
            patched &= word == w.patched;
        }
        in.push_back(patched || original);
        if (patched) {
            continue;
        }
        if (!original) {
            LOG_WARNING(Loader,
                        "title {:016X}: patch \"{}\" left out, the code is another version's",
                        title_id, patch.name);
            continue;
        }
        for (const PatchWord& w : patch.words) {
            memory.WriteBlock(process, w.addr, &w.patched, sizeof(w.patched));
        }
        if (patch.applied != nullptr) {
            patch.applied(memory, process);
        }
        LOG_INFO(Loader, "title {:016X}: patch \"{}\" applied", title_id, patch.name);
    }
    return in;
}

} // namespace Loader
