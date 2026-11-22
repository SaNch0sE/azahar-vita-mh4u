// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <bit>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#include "common/assert.h"
#include "common/common_types.h"
#include "common/file_util.h"
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

namespace {

/// Code as its words: a process's from the host memory behind it, copied 32 KiB at a time into a
/// window (word by word, the reads through the guest memory's mapping cost ~50-80 ns each), or a
/// code image's.
class CodeReader {
public:
    CodeReader(Kernel::Process& process, VAddr base_, u32 bytes) : base{base_}, words{bytes / 4} {
        auto blocks = process.vm_manager.GetBackingBlocksForRange(base, bytes);
        if (blocks.Succeeded()) {
            u32 at = 0;
            for (auto& [backing, size] : blocks.Unwrap()) {
                pieces.push_back({at, size / 4, reinterpret_cast<const u32*>(backing.GetPtr())});
                at += size / 4;
            }
        }
        words = pieces.empty() ? 0 : std::min(words, pieces.back().first + pieces.back().words);
        window.resize(WindowWords);
    }

    CodeReader(VAddr base_, std::span<const u8> bytes)
        : base{base_}, words{static_cast<u32>(bytes.size() / 4)} {
        DEBUG_ASSERT(reinterpret_cast<uintptr_t>(bytes.data()) % 4 == 0);
        if (words != 0) {
            pieces.push_back({0, words, reinterpret_cast<const u32*>(bytes.data())});
        }
        window.resize(WindowWords);
    }

    [[nodiscard]] u32 Words() const {
        return words;
    }
    [[nodiscard]] VAddr Base() const {
        return base;
    }
    [[nodiscard]] u32 operator[](u32 i) {
        if (i - window_first >= window_words) {
            Fill(i);
        }
        return window[i - window_first];
    }

private:
    static constexpr u32 WindowWords = 8192;
    struct Piece {
        u32 first;
        u32 words;
        const u32* data;
    };

    /// The window from word `i` on (a scan's next words; a jump back starts it a little before).
    void Fill(u32 i) {
        DEBUG_ASSERT_MSG(i < words, "code reader: word {} of {}", i, words);
        window_first = i >= 64 ? i - 64 : 0;
        window_words = std::min(WindowWords, words - window_first);
        u32 done = 0;
        auto piece = std::upper_bound(pieces.begin(), pieces.end(), window_first,
                                      [](u32 v, const Piece& p) { return v < p.first; }) -
                     1;
        while (done < window_words) {
            const u32 from = window_first + done - piece->first;
            const u32 n = std::min(window_words - done, piece->words - from);
            std::memcpy(window.data() + done, piece->data + from, n * 4);
            done += n;
            ++piece;
        }
    }

    VAddr base;
    u32 words;
    std::vector<Piece> pieces;
    std::vector<u32> window;
    u32 window_first = 0;
    u32 window_words = 0;
};

u32 RotImm(u32 imm12) {
    const u32 r = (imm12 >> 8) * 2;
    const u32 v = imm12 & 0xff;
    return r != 0 ? (v >> r) | (v << (32 - r)) : v;
}

VAddr BlTarget(u32 x, VAddr at) {
    if ((x & 0xFF000000) != 0xEB000000) {
        return 0;
    }
    const s32 off = static_cast<s32>(x << 8) >> 8;
    return static_cast<VAddr>(at + 8 + off * 4);
}

/// What a register or a stack slot holds in FsaaSlotLiteral's simulation.
struct Held {
    enum Kind : u8 { None, Literal, SpOffset } kind = None;
    u32 a = 0; ///< the literal's address, or the offset from sp
    u32 v = 0; ///< the literal's value
};

/// The literal that the stack argument `slot` holds at the call at word `call`: the caller run
/// forward from 512 words before it, its registers, VFP registers and stack slots holding a
/// literal (an `ldr rd, [pc, #imm]`'s), an sp offset (`add rd, sp, #imm`) or nothing.
Held FsaaSlotLiteral(CodeReader& code, u32 call, u32 slot) {
    std::array<Held, 16> r{};
    std::array<Held, 32> s{};
    std::array<Held, 1024> m{}; // sp + 0 .. 4092
    const auto base_of = [&](u32 rn) -> std::optional<u32> {
        if (rn == 13) {
            return 0u;
        }
        if (r[rn].kind == Held::SpOffset) {
            return r[rn].a;
        }
        return std::nullopt;
    };
    const auto slot_of = [&](std::optional<u32> off) -> Held* {
        return off && *off % 4 == 0 && *off < 4096 ? &m[*off / 4] : nullptr;
    };
    const auto load = [&](Held& to, std::optional<u32> off) {
        Held* from = slot_of(off);
        to = from != nullptr ? *from : Held{};
    };
    const auto store = [&](const Held& from, std::optional<u32> off) {
        if (Held* to = slot_of(off)) {
            *to = from.kind == Held::Literal ? from : Held{};
        }
    };
    const VAddr end = code.Base() + code.Words() * 4;
    for (u32 i = call > 512 ? call - 512 : 0; i < call; i++) {
        const u32 x = code[i];
        const VAddr at = code.Base() + i * 4;
        const u32 cond = x >> 28;
        const u32 rd = (x >> 12) & 15;
        const u32 rn = (x >> 16) & 15;
        if (cond != 0xE) {
            // A conditional instruction: what it writes is no longer known.
            if (cond == 0xF) {
                continue;
            }
            if ((x & 0x0C000000) == 0x04000000) {
                if (x & 0x00100000) {
                    r[rd] = {};
                } else if (rn == 13) {
                    store({}, x & 0xfff);
                }
            } else if ((x & 0x0C000000) == 0 && (x & 0x01900000) != 0x01000000) {
                r[rd] = {};
            } else if ((x & 0x0E000000) == 0x08000000) {
                if (x & 0x00100000) {
                    for (u32 k = 0; k < 16; k++) {
                        if (x >> k & 1) {
                            r[k] = {};
                        }
                    }
                } else {
                    m.fill({});
                }
            } else if ((x & 0x0E000E00) == 0x0C000A00 || (x & 0x0F000E10) == 0x0E000A00) {
                s.fill({});
                m.fill({});
            }
            continue;
        }
        if ((x & 0xFF7F0000) == 0xE51F0000) { // ldr rd, [pc, #+-imm]
            const u32 imm = x & 0xfff;
            const VAddr a = (x & 0x00800000) ? at + 8 + imm : at + 8 - imm;
            if (a >= code.Base() && a < end && a % 4 == 0) {
                r[rd] = {Held::Literal, a, code[(a - code.Base()) / 4]};
            } else {
                r[rd] = {};
            }
            continue;
        }
        if ((x & 0xFFF00000) == 0xE5900000 || (x & 0xFFF00000) == 0xE5800000) {
            // ldr / str rd, [rn, #imm]
            const auto b = base_of(rn);
            const std::optional<u32> off = b ? std::optional<u32>{*b + (x & 0xfff)} : std::nullopt;
            if (x & 0x00100000) {
                load(r[rd], off);
            } else {
                store(r[rd], off);
            }
            continue;
        }
        if ((x & 0xFFFF0000) == 0xE28D0000) { // add rd, sp, #imm
            r[rd] = {Held::SpOffset, RotImm(x & 0xfff), 0};
            continue;
        }
        if ((x & 0xFFFF0FF0) == 0xE1A00000) { // mov rd, rm
            const u32 rm = x & 15;
            r[rd] = rm == 13 ? Held{Held::SpOffset, 0, 0} : r[rm];
            continue;
        }
        if ((x & 0xFFC00000) == 0xE8800000) { // ldmia / stmia rn{!}, {list}
            const auto b = base_of(rn);
            u32 index = 0;
            for (u32 k = 0; k < 16; k++) {
                if (!(x >> k & 1)) {
                    continue;
                }
                const std::optional<u32> off =
                    b ? std::optional<u32>{*b + 4 * index} : std::nullopt;
                if (x & 0x00100000) {
                    load(r[k], off);
                } else {
                    store(r[k], off);
                }
                index++;
            }
            if ((x & 0x00200000) && rn != 13) {
                r[rn] = b ? Held{Held::SpOffset, *b + 4 * index, 0} : Held{};
            }
            continue;
        }
        if ((x & 0xFF300F00) == 0xED100A00 || (x & 0xFF300F00) == 0xED000A00) {
            // vldr / vstr sd, [rn, #+-imm * 4]
            const u32 sd = ((x >> 12) & 15) << 1 | ((x >> 22) & 1);
            const auto b = base_of(rn);
            const u32 imm = (x & 0xff) * 4;
            const std::optional<u32> off =
                b ? std::optional<u32>{(x & 0x00800000) ? *b + imm : *b - imm} : std::nullopt;
            if (x & 0x00100000) {
                load(s[sd], off);
            } else {
                store(s[sd], off);
            }
            continue;
        }
        if ((x & 0xFFB00F00) == 0xEC900A00 || (x & 0xFFB00F00) == 0xEC800A00) {
            // vldmia / vstmia rn, {sd ...}
            const u32 sd = ((x >> 12) & 15) << 1 | ((x >> 22) & 1);
            const auto b = base_of(rn);
            for (u32 k = 0; k < (x & 0xff) && sd + k < 32; k++) {
                const std::optional<u32> off = b ? std::optional<u32>{*b + 4 * k} : std::nullopt;
                if (x & 0x00100000) {
                    load(s[sd + k], off);
                } else {
                    store(s[sd + k], off);
                }
            }
            continue;
        }
        if ((x & 0xFF000000) == 0xEB000000) { // bl: the caller-saved registers go
            for (const u32 k : {0u, 1u, 2u, 3u, 12u, 14u}) {
                r[k] = {};
            }
            for (u32 k = 0; k < 16; k++) {
                s[k] = {};
            }
            continue;
        }
        // Anything else that writes a core register leaves it unknown.
        if (((x & 0x0C000000) == 0 && (x & 0x01900000) != 0x01000000) ||
            ((x & 0x0C000000) == 0x04000000 && (x & 0x00100000))) {
            r[rd] = {};
        }
    }
    return m[slot];
}

std::vector<u32> CallsTo(CodeReader& code, VAddr target) {
    std::vector<u32> calls;
    for (u32 i = 0; i < code.Words(); i++) {
        if (BlTarget(code[i], code.Base() + i * 4) == target) {
            calls.push_back(i);
        }
    }
    return calls;
}

} // namespace

namespace {

/// MT Framework's build stamp in the read-only data ("MasterRelease CTR Oct 15 2014 14:02:51": its
/// configuration, platform and build time), or empty.
std::string BuildStamp(CodeReader& data) {
    constexpr std::string_view Prefix = "MasterRelease CTR ";
    const auto byte = [&](u32 at) { return static_cast<char>(data[at / 4] >> (at % 4 * 8)); };
    for (u32 i = 0; i + 1 < data.Words(); i++) {
        const u64 pair = data[i] | u64{data[i + 1]} << 32;
        for (u32 shift = 0; shift < 4; shift++) {
            if (static_cast<u32>(pair >> (shift * 8)) != 0x7473614D) { // "Mast"
                continue;
            }
            const u32 at = i * 4 + shift;
            std::string stamp;
            for (u32 k = 0; at + k < data.Words() * 4 && k < 64; k++) {
                const char c = byte(at + k);
                if (c == 0) {
                    break;
                }
                stamp += c;
            }
            if (stamp.starts_with(Prefix)) {
                return stamp;
            }
        }
    }
    return {};
}

} // namespace

namespace {

constexpr std::array<EngineSetting, EngineSettingCount> engine_settings{{
    {{"mFSAA_2D"},
     10,
     EngineDefault::Off,
     Translatable("2D anti-aliasing"),
     Translatable("The game draws its 3D scene twice a frame, half a pixel apart, and blends the "
                  "two: smoother edges for far less speed (FSAA_2D).")},
    {{"mULCDEnableEdit", "mULCDEnable"},
     6,
     EngineDefault::Off,
     Translatable("Right eye render"),
     Translatable("The scene drawn a second time for the right eye, which the 3D slider shows. "
                  "Off, the scene is drawn once (mULCDEnable).")},
    {{"FSAA2X"},
     -1,
     EngineDefault::Off,
     Translatable("2x anti-aliasing"),
     Translatable("The top screen drawn at twice its pixels and scaled down to the screen: "
                  "smoother edges for more drawing (FSAA2X).")},
    {{"FSAA4X"},
     -1,
     EngineDefault::Off,
     Translatable("4x anti-aliasing"),
     Translatable("The top screen drawn at four times its pixels and scaled down to the screen: "
                  "smoother edges for more drawing (FSAA4X).")},
    {{"NoAADisplay1"},
     -1,
     EngineDefault::On,
     Translatable("No bottom screen anti-aliasing"),
     Translatable("Leaves out the bottom screen's anti-aliasing (NoAADisplay1).")},
    {{"FSAA2XDisplay1"},
     -1,
     EngineDefault::Off,
     Translatable("Bottom screen 2x anti-aliasing"),
     Translatable("The bottom screen drawn at twice its pixels and scaled down to the screen "
                  "(FSAA2XDisplay1).")},
    {{"FSAA4XDisplay1"},
     -1,
     EngineDefault::Off,
     Translatable("Bottom screen 4x anti-aliasing"),
     Translatable("The bottom screen drawn at four times its pixels and scaled down to the screen "
                  "(FSAA4XDisplay1).")},
    {{"mVSyncEdit", "mVsyncEdit", "mVSync"},
     0,
     EngineDefault::Game,
     Translatable("Vertical sync"),
     Translatable("The engine waits for the screen's refresh before it shows a new frame "
                  "(mVSync).")},
    {{"mTripleBufferRenderWaitEdit", "mTripleBufferRenderWait"},
     2,
     EngineDefault::Game,
     Translatable("Triple buffer wait"),
     Translatable("The engine's wait for a frame when it draws with three frame buffers "
                  "(mTripleBufferRenderWait).")},
    {{"mRenderSkipEdit", "mRenderSkip"},
     -1,
     EngineDefault::Game,
     Translatable("Render skip"),
     Translatable("The engine leaves frames undrawn when it falls behind (mRenderSkip).")},
}};

/// Whether the code holds `name` at word `i`, ended by a NUL: where the compiler puts the engine's
/// property names, word-aligned beside the code that registers them.
bool NameAt(CodeReader& code, u32 i, std::string_view name) {
    for (u32 k = 0; k <= name.size(); k++) {
        const u32 at = i * 4 + k;
        if (at / 4 >= code.Words()) {
            return false;
        }
        const char c = static_cast<char>(code[at / 4] >> (at % 4 * 8));
        if (c != (k < name.size() ? name[k] : '\0')) {
            return false;
        }
    }
    return true;
}

struct Image {
    CodeReader& code;
    CodeReader& ro;
    CodeReader& data;

    [[nodiscard]] std::optional<u32> Word(VAddr a) {
        for (CodeReader* r : {&code, &ro, &data}) {
            if (a % 4 == 0 && a >= r->Base() && (a - r->Base()) / 4 < r->Words()) {
                return (*r)[(a - r->Base()) / 4];
            }
        }
        return std::nullopt;
    }
};

/// The bit a property's accessor at `a` sets (a setter's orrne/biceq or bicne/orreq of one bit),
/// else the bit it reads (a getter's tst or and of one bit, or lsl #s then lsr #31, the byte an
/// ldrb loads counted in), and whether it sets it; nothing for any other function.
struct AccessorBit {
    u32 bit;
    bool sets;
};
std::optional<AccessorBit> DecodeAccessor(CodeReader& code, VAddr a) {
    if (a % 4 != 0 || a < code.Base() || (a - code.Base()) / 4 >= code.Words()) {
        return std::nullopt;
    }
    const u32 first = (a - code.Base()) / 4;
    u32 byte = 0;
    std::optional<u32> reads;
    for (u32 i = first; i < first + 24 && i + 1 < code.Words(); i++) {
        const u32 x = code[i];
        if (x == 0xE12FFF1E) { // bx lr
            break;
        }
        const u32 cond = x >> 28;
        const u32 imm = RotImm(x & 0xfff);
        if ((x & 0x0E500000) == 0x04500000 && (x & 0x00100000) && !reads) { // ldrb
            byte = (x & 0xfff) & 3;
        }
        const bool orr_bic = (x & 0x0FE00000) == 0x03800000 || (x & 0x0FE00000) == 0x03C00000;
        if (orr_bic && cond <= 1 && std::has_single_bit(imm)) {
            return AccessorBit{static_cast<u32>(std::countr_zero(imm)), true};
        }
        if (reads || cond != 0xE) {
            continue;
        }
        if (((x & 0x0FE00000) == 0x02000000 || (x & 0x0FF0F000) == 0x03100000) &&
            std::has_single_bit(imm)) {
            reads = static_cast<u32>(std::countr_zero(imm)) + 8 * byte;
        } else if ((x & 0x0FFF0070) == 0x01A00000 && ((x >> 7) & 31) != 0) { // lsl #s
            const u32 y = code[i + 1];
            if ((y & 0x0FFF0FF0) == 0x01A00FA0 || (y & 0x0FF00FF0) == 0x01500FA0) {
                reads = 31 - ((x >> 7) & 31) + 8 * byte; // lsr #31, or cmp rN, rM, lsr #31
            }
        }
    }
    if (reads && *reads < 32) {
        return AccessorBit{*reads, false};
    }
    return std::nullopt;
}

/// The bit of the property whose name the `add rd, pc, #imm` at word `ref` points at: its
/// registering function (back to its push {..., lr}) run forward, over its literal pools, with the
/// words its registers hold from literals and tables, to the next call of the function that
/// inserts a property (the one it calls most); the words that step loads, read as accessors. A
/// setter's bit, else a getter's; nothing where they disagree.
std::optional<u32> RegisteredBit(Image& image, u32 ref) {
    CodeReader& code = image.code;
    constexpr u32 FunctionWords = 0x2000;
    constexpr u32 StepWords = 64;
    u32 f = ref;
    while (f > 0 && ref - f < FunctionWords && (code[f] & 0xFFFF4000) != 0xE92D4000) {
        f--;
    }
    if ((code[f] & 0xFFFF4000) != 0xE92D4000) {
        return std::nullopt;
    }
    const u32 last = std::min(code.Words(), ref + StepWords);
    std::vector<std::pair<VAddr, u32>> called;
    for (u32 i = f; i < last; i++) {
        const VAddr to = BlTarget(code[i], code.Base() + i * 4);
        if (to == 0) {
            continue;
        }
        const auto it = std::find_if(called.begin(), called.end(),
                                     [&](const auto& c) { return c.first == to; });
        if (it != called.end()) {
            it->second++;
        } else {
            called.emplace_back(to, 1);
        }
    }
    if (called.empty()) {
        return std::nullopt;
    }
    const VAddr insert =
        std::max_element(called.begin(), called.end(), [](const auto& a, const auto& b) {
            return a.second < b.second;
        })->first;
    u32 start = f;
    std::optional<u32> end;
    for (u32 i = f; i < last && !end; i++) {
        if (BlTarget(code[i], code.Base() + i * 4) == insert) {
            if (i < ref) {
                start = i;
            } else {
                end = i;
            }
        }
    }
    if (!end) {
        return std::nullopt;
    }
    std::array<std::optional<u32>, 16> r{};
    std::vector<u32> loaded;
    const auto load = [&](u32 i, std::optional<VAddr> a) -> std::optional<u32> {
        const std::optional<u32> v = a ? image.Word(*a) : std::nullopt;
        if (v && i > start) {
            loaded.push_back(*v);
        }
        return v;
    };
    for (u32 i = f; i < *end; i++) {
        const u32 x = code[i];
        const u32 rd = (x >> 12) & 15;
        const u32 rn = (x >> 16) & 15;
        const VAddr pc = code.Base() + i * 4 + 8;
        const bool writes_rd = ((x & 0x0C000000) == 0 && (x & 0x01900000) != 0x01000000) ||
                               (x & 0x0C100000) == 0x04100000;
        if ((x >> 28) != 0xE) {
            if (writes_rd) {
                r[rd].reset();
            }
            continue;
        }
        const u32 imm = x & 0xfff;
        const bool up = (x & 0x00800000) != 0;
        if ((x & 0xFF000000) == 0xEA000000) {
            // b: over a literal pool (the function runs straight on); one back ends it.
            const VAddr to = pc + static_cast<u32>(static_cast<s32>(x << 8) >> 6);
            if (to <= pc - 8) {
                break;
            }
            i = (to - code.Base()) / 4 - 1;
        } else if (BlTarget(x, pc - 8) != 0) {
            for (const u32 k : {0u, 1u, 2u, 3u, 12u, 14u}) {
                r[k].reset();
            }
        } else if ((x & 0x0FE00000) == 0x02800000 || (x & 0x0FE00000) == 0x02400000) {
            // add / sub rd, rn, #imm
            const std::optional<u32> b = rn == 15 ? std::optional<u32>{pc} : r[rn];
            const bool add = (x & 0x0FE00000) == 0x02800000;
            r[rd] =
                b ? std::optional<u32>{add ? *b + RotImm(imm) : *b - RotImm(imm)} : std::nullopt;
        } else if ((x & 0x0FFF0FF0) == 0x01A00000) { // mov rd, rm
            r[rd] = r[x & 15];
        } else if ((x & 0x0E500000) == 0x04100000) { // ldr rd, [rn, #imm]{!} / [rn], #imm
            const std::optional<u32> b = rn == 15 ? std::optional<u32>{pc} : r[rn];
            const bool pre = (x & 0x01000000) != 0;
            const std::optional<u32> moved =
                b ? std::optional<u32>{up ? *b + imm : *b - imm} : std::nullopt;
            const std::optional<u32> v = load(i, pre ? moved : b);
            if ((!pre || (x & 0x00200000)) && rn != 15) {
                r[rn] = moved;
            }
            r[rd] = v;
        } else if ((x & 0x0E5000F0) == 0x004000D0 && (x & 0x01000000)) { // ldrd rd, [rn, #imm]
            const u32 off = ((x >> 4) & 0xF0) | (x & 0xF);
            const std::optional<u32> b = r[rn];
            const std::optional<VAddr> a =
                b ? std::optional<VAddr>{up ? *b + off : *b - off} : std::nullopt;
            r[rd] = load(i, a);
            r[(rd + 1) & 15] = load(i, a ? std::optional<VAddr>{*a + 4} : std::nullopt);
            if (x & 0x00200000) {
                r[rn] = a;
            }
        } else if ((x & 0x0E100000) == 0x08100000) { // ldm rn{!}, {list}
            const u32 n = static_cast<u32>(std::popcount(x & 0xffff));
            const bool pre = (x & 0x01000000) != 0;
            const std::optional<u32> b = r[rn];
            std::optional<VAddr> a;
            if (b) {
                a = up ? *b + (pre ? 4 : 0) : *b - 4 * n + (pre ? 0 : 4);
            }
            for (u32 k = 0, at = 0; k < 16; k++) {
                if (x >> k & 1) {
                    r[k] = load(i, a ? std::optional<VAddr>{*a + 4 * at} : std::nullopt);
                    at++;
                }
            }
            if ((x & 0x00200000) && !(x >> rn & 1)) {
                r[rn] = b ? std::optional<u32>{up ? *b + 4 * n : *b - 4 * n} : std::nullopt;
            }
        } else if ((x & 0x0F300E00) == 0x0D100A00) { // vldr sd / dd, [rn, #+-imm * 4]
            const std::optional<u32> b = rn == 15 ? std::optional<u32>{pc} : r[rn];
            const u32 off = (x & 0xff) * 4;
            const std::optional<VAddr> a =
                b ? std::optional<VAddr>{up ? *b + off : *b - off} : std::nullopt;
            for (u32 k = 0; k < ((x & 0x100) ? 2u : 1u); k++) {
                load(i, a ? std::optional<VAddr>{*a + 4 * k} : std::nullopt);
            }
        } else if ((x & 0x0F900E00) == 0x0C900A00) { // vldmia rn{!}, {...}
            const std::optional<u32> b = r[rn];
            for (u32 k = 0; k < (x & 0xff); k++) {
                load(i, b ? std::optional<VAddr>{*b + 4 * k} : std::nullopt);
            }
            if (x & 0x00200000) {
                r[rn] = b ? std::optional<u32>{*b + 4 * (x & 0xff)} : std::nullopt;
            }
        } else if (writes_rd) {
            r[rd].reset();
        } else if (((x & 0x0E100000) == 0x08000000 || (x & 0x0C100000) == 0x04000000) &&
                   (x & 0x00200000)) {
            r[rn].reset(); // a store's write-back
        }
    }
    std::optional<u32> set, get;
    bool agree = true;
    for (const u32 v : loaded) {
        const std::optional<AccessorBit> a = DecodeAccessor(code, v);
        if (!a) {
            continue;
        }
        std::optional<u32>& kind = a->sets ? set : get;
        agree &= !kind || *kind == a->bit;
        kind = a->bit;
    }
    if (!agree) {
        return std::nullopt;
    }
    return set ? set : get;
}

/// The bit of the property named at word `name`: the first of its registrations, the `add rd, pc,
/// #imm` pointing at it within 64 KiB before it, that gives one.
std::optional<u32> NamedBit(Image& image, u32 name) {
    CodeReader& code = image.code;
    constexpr u32 ReachWords = 0x4000;
    const VAddr at = code.Base() + name * 4;
    for (u32 i = name > ReachWords ? name - ReachWords : 0; i < name; i++) {
        const u32 x = code[i];
        if ((x & 0xFFFF0000) == 0xE28F0000 && code.Base() + i * 4 + 8 + RotImm(x & 0xfff) == at) {
            if (const std::optional<u32> bit = RegisteredBit(image, i)) {
                return bit;
            }
        }
    }
    return std::nullopt;
}

} // namespace

namespace {

RenderConfig FindRenderConfigIn(CodeReader& code, CodeReader& ro, CodeReader& data,
                                u64 program_id) {
    RenderConfig config;
    const auto started = std::chrono::steady_clock::now();
    const auto ms = [](auto from, auto to) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    };
    // MT Framework's build stamp first: the code is searched only in a title that has one.
    const std::string stamp = BuildStamp(ro);
    const auto stamped = std::chrono::steady_clock::now();
    if (stamp.empty()) {
        LOG_INFO(Loader, "title {:016X}: no MT Framework build stamp ({} ms)", program_id,
                 ms(started, stamped));
        return config;
    }
    const VAddr base = code.Base();
    // One pass for the constructor and the config word's readers. The constructor: push {r0-r3};
    // push {r4-r11, lr}; [vpush {d8...}]; [sub sp, sp, #n]; then a stack argument loaded (ldr rN,
    // [sp, #S]) and its high bits masked (bic rD, rN, #M).
    struct Ctor {
        VAddr at;
        u32 slot;
        u32 mask;
    };
    std::vector<Ctor> ctors;
    // The candidate readers by the word's offset: (offset, bit, the test's word index).
    struct Read {
        u32 offset;
        u32 bit;
        u32 at;
        u32 global; ///< the pointer it loads the object from (ldr rG, =global; ldr rT, [rG]), or 0
    };
    // The object pointer a reader's base comes from, a few words before its add (0: none seen).
    const auto global_of = [&](u32 add, u32 rt) -> u32 {
        for (u32 i = add; i + 6 > add && i > 0; i--) {
            const u32 y = code[i - 1];
            if ((y & 0xFFF0FFFF) != (0xE5900000 | rt << 12)) {
                continue;
            }
            const u32 rg = (y >> 16) & 15;
            for (u32 k = i - 1; k + 8 > i - 1 && k > 0; k--) {
                const u32 z = code[k - 1];
                if ((z & 0xFF7FF000) != (0xE51F0000 | rg << 12)) {
                    continue;
                }
                const u32 imm = z & 0xfff;
                const VAddr lit = base + (k - 1) * 4 + 8;
                const VAddr a = (z & 0x00800000) ? lit + imm : lit - imm;
                return a >= base && a < base + code.Words() * 4 && a % 4 == 0 ? code[(a - base) / 4]
                                                                              : 0;
            }
            return 0;
        }
        return 0;
    };
    std::vector<Read> reads;
    // The settings' property names: their first words, and where the code has them.
    struct Name {
        std::size_t setting;
        std::string_view name;
        u32 head;
    };
    std::vector<Name> names;
    std::bitset<256> name_starts;
    for (std::size_t s = 0; s < engine_settings.size(); s++) {
        for (const char* property : engine_settings[s].properties) {
            if (property == nullptr) {
                continue;
            }
            const std::string_view name = property;
            DEBUG_ASSERT(name.size() >= 4);
            u32 head = 0;
            for (u32 k = 0; k < 4; k++) {
                head |= static_cast<u32>(static_cast<u8>(name[k])) << (k * 8);
            }
            names.push_back({s, name, head});
            name_starts.set(head & 0xff);
        }
    }
    std::vector<std::pair<std::size_t, u32>> named; // (names' index, word)
    for (u32 f = 0; f + 8 <= code.Words(); f++) {
        const u32 w0 = code[f];
        for (std::size_t n = 0; name_starts.test(w0 & 0xff) && n < names.size(); n++) {
            if (w0 == names[n].head && NameAt(code, f, names[n].name)) {
                named.emplace_back(n, f);
            }
        }
        // A reader: add rB, rT, #0x6000 or #0x6800; [add rB, rB, #imm;] ldr rV, [rB, #imm]; then
        // within two words a tst or an and of rV with one bit.
        if ((w0 & 0xFFF00000) == 0xE2800000 &&
            (RotImm(w0 & 0xfff) == 0x6000 || RotImm(w0 & 0xfff) == 0x6800)) {
            const u32 rb = (w0 >> 12) & 15;
            u32 offset = RotImm(w0 & 0xfff);
            u32 j = f + 1;
            u32 x = code[j];
            if ((x & 0xFFF00000) == 0xE2800000 && ((x >> 16) & 15) == rb &&
                ((x >> 12) & 15) == rb) {
                offset += RotImm(x & 0xfff);
                x = code[++j];
            }
            if ((x & 0xFFF00000) == 0xE5900000 && ((x >> 16) & 15) == rb) {
                offset += x & 0xfff;
                const u32 rv = (x >> 12) & 15;
                for (u32 k = j + 1; k <= j + 2; k++) {
                    const u32 t = code[k];
                    if (((t & 0x0FF0F000) == 0x03100000 || (t & 0x0FE00000) == 0x02000000) &&
                        ((t >> 16) & 15) == rv) {
                        if (std::popcount(RotImm(t & 0xfff)) == 1) {
                            reads.push_back(
                                {offset, RotImm(t & 0xfff), k, global_of(f, (w0 >> 16) & 15)});
                        }
                        break;
                    }
                }
            }
        }
        if (w0 == 0xE92D000F && code[f + 1] == 0xE92D4FF0) {
            u32 frame = 16 + 36;
            u32 j = f + 2;
            if ((code[j] & 0xFFFFFF00) == 0xED2D8B00) {
                frame += (code[j] & 0xff) * 4;
                j++;
            }
            if ((code[j] & 0xFFFFF000) == 0xE24DD000) {
                frame += RotImm(code[j] & 0xfff);
                j++;
            }
            std::array<s32, 16> loaded;
            loaded.fill(-1);
            for (u32 i = j; i < std::min(code.Words(), j + 256); i++) {
                const u32 x = code[i];
                if ((x & 0xFFFF0000) == 0xE59D0000) {
                    loaded[(x >> 12) & 15] = static_cast<s32>(x & 0xfff);
                    continue;
                }
                const u32 mask = RotImm(x & 0xfff);
                if ((x & 0xFFF00000) == 0xE3C00000 && (mask == 0xFF000000 || mask == 0xF8000000)) {
                    const s32 at = loaded[(x >> 16) & 15];
                    if (at < 0) {
                        continue;
                    }
                    // The argument's offset past the spilled r0-r3 and the frame below them.
                    const s32 off = at - static_cast<s32>(frame - 16) - 16;
                    if (off >= 0 && off % 4 == 0) {
                        ctors.push_back({base + f * 4, static_cast<u32>(off / 4), mask});
                    }
                    break;
                }
                if ((x & 0x0FFF8000) == 0x08BD8000) { // pop {..., pc}
                    break;
                }
            }
            continue;
        }
    }
    // The constructors' calls, and those of wrappers that pass their stack arguments on as they
    // are (push {lr}; sub sp, sp, #n; ... vstmia sp, {...}; bl): a pass for each.
    const auto calls_to = [&](std::span<const VAddr> targets) {
        std::vector<std::pair<u32, u32>> calls; // word, target's index
        for (u32 i = 0; !targets.empty() && i < code.Words(); i++) {
            const VAddr to = BlTarget(code[i], code.Base() + i * 4);
            if (to == 0) {
                continue;
            }
            for (u32 t = 0; t < targets.size(); t++) {
                if (targets[t] == to) {
                    calls.emplace_back(i, t);
                }
            }
        }
        return calls;
    };
    std::vector<VAddr> ctor_at;
    for (const Ctor& c : ctors) {
        ctor_at.push_back(c.at);
    }
    const auto scanned = std::chrono::steady_clock::now();
    std::vector<std::pair<u32, u32>> calls = calls_to(ctor_at);
    std::vector<VAddr> wrapper_at;
    std::vector<u32> wrapper_ctor;
    for (const auto& [c, t] : calls) {
        u32 f = c;
        while (f + 16 > c && f > 0 && code[f] != 0xE52DE004) {
            f--;
        }
        if (code[f] != 0xE52DE004 || (code[f + 1] & 0xFFFFF000) != 0xE24DD000) {
            continue;
        }
        for (u32 i = f + 2; i < c; i++) {
            if ((code[i] & 0xFFFF0F00) == 0xEC8D0A00) {
                wrapper_at.push_back(base + f * 4);
                wrapper_ctor.push_back(t);
                break;
            }
        }
    }
    for (auto& [c, t] : calls_to(wrapper_at)) {
        calls.emplace_back(c, wrapper_ctor[t]);
    }
    const auto called = std::chrono::steady_clock::now();
    for (const auto& [c, t] : calls) {
        const Held lit = FsaaSlotLiteral(code, c, ctors[t].slot);
        if (lit.kind != Held::Literal || (lit.v & ctors[t].mask) != 0) {
            continue;
        }
        if (std::none_of(config.defaults.begin(), config.defaults.end(),
                         [&](const RenderConfig::Word& w) { return w.addr == lit.a; })) {
            config.defaults.push_back({lit.a, lit.v});
        }
    }
    std::sort(
        config.defaults.begin(), config.defaults.end(),
        [](const RenderConfig::Word& a, const RenderConfig::Word& b) { return a.addr < b.addr; });
    config.constructors = static_cast<u32>(ctors.size());
    // The config word: the offset read for the most different bits, at least MinWordBits. With
    // fewer it is some other word, and the defaults are enough.
    constexpr u32 MinWordBits = 8;
    if (!ctors.empty()) {
        std::map<u32, u32> bits_of;
        for (const Read& r : reads) {
            bits_of[r.offset] |= r.bit;
        }
        u32 most = 0;
        for (const auto& [offset, bits] : bits_of) {
            if (std::popcount(bits) > most) {
                most = static_cast<u32>(std::popcount(bits));
                config.word_offset = offset;
            }
        }
        if (most < MinWordBits) {
            config.word_offset = 0;
        }
        std::map<u32, u32> globals;
        for (const Read& r : reads) {
            if (config.word_offset != 0 && r.offset == config.word_offset) {
                config.readers.push_back({base + r.at * 4, code[r.at]});
            }
            if (r.global != 0) {
                globals[r.global]++;
            }
        }
        u32 votes = 0;
        for (const auto& [global, n] : globals) {
            if (n > votes) {
                votes = n;
                config.global = global;
            }
        }
    }
    // Each setting's bit: its first property name whose registration gives one, else its fixed bit;
    // a bit two settings got goes to the first, a resolved one before a fixed one.
    std::string how;
    if (!ctors.empty()) {
        Image image{code, ro, data};
        std::array<const char*, EngineSettingCount> by{};
        for (std::size_t n = 0; n < names.size(); n++) {
            const std::size_t s = names[n].setting;
            for (const auto& [index, word] : named) {
                if (index != n || config.bits[s] >= 0) {
                    continue;
                }
                if (const std::optional<u32> bit = NamedBit(image, word)) {
                    config.bits[s] = static_cast<s8>(*bit);
                    by[s] = names[n].name.data();
                }
            }
        }
        u32 taken = 0;
        for (const bool resolved : {true, false}) {
            for (std::size_t s = 0; s < engine_settings.size(); s++) {
                if (resolved != (config.bits[s] >= 0)) {
                    continue;
                }
                s8& bit = config.bits[s];
                if (!resolved) {
                    bit = engine_settings[s].fixed_bit;
                }
                if (bit < 0) {
                    continue;
                }
                if ((taken >> bit) & 1) {
                    LOG_WARNING(Loader,
                                "title {:016X}: the engine setting {} has bit {} too: left out",
                                program_id, engine_settings[s].name, bit);
                    bit = -1;
                    by[s] = nullptr;
                    continue;
                }
                taken |= 1u << bit;
            }
        }
        for (std::size_t s = 0; s < engine_settings.size(); s++) {
            how += fmt::format(" {} {}{};", engine_settings[s].name,
                               config.bits[s] >= 0 ? fmt::format("bit {}", config.bits[s])
                                                   : std::string{"none"},
                               config.bits[s] < 0 ? ""
                               : by[s] != nullptr ? fmt::format(" ({})", by[s])
                                                  : " (fixed)");
        }
    }
    const auto done = std::chrono::steady_clock::now();
    std::string found;
    for (const auto& w : config.defaults) {
        found += fmt::format(" default {:#010x} = {:#010x};", w.addr, w.value);
    }
    found += fmt::format(" the word +{:#x}, {} readers, the object at [{:#010x}];",
                         config.word_offset, config.readers.size(), config.global);
    if (ctors.empty()) {
        LOG_WARNING(Loader,
                    "title {:016X}: an MT Framework build ({}) with no render config this search "
                    "knows: its engine settings are left as the game has them",
                    program_id, stamp);
    }
    LOG_INFO(Loader,
             "title {:016X}: {}: {} render config constructors ({} ms: the stamp {}, scan {}, "
             "calls {}, literals {})",
             program_id, stamp, ctors.size(), ms(started, done), ms(started, stamped),
             ms(stamped, scanned), ms(scanned, called), ms(called, done));
    if (!found.empty()) {
        LOG_INFO(Loader, "title {:016X}:{}", program_id, found);
    }
    if (!how.empty()) {
        LOG_INFO(Loader, "title {:016X}: engine settings:{}", program_id, how);
    }
    return config;
}

} // namespace

RenderConfig FindRenderConfig(Kernel::Process& process) {
    if (process.codeset == nullptr) {
        return {};
    }
    const auto& text = process.codeset->CodeSegment();
    const auto& ro_segment = process.codeset->RODataSegment();
    const auto& data_segment = process.codeset->DataSegment();
    CodeReader code{process, text.addr, static_cast<u32>(text.size)};
    CodeReader ro{process, ro_segment.addr, static_cast<u32>(ro_segment.size)};
    CodeReader data{process, data_segment.addr, static_cast<u32>(data_segment.size)};
    return FindRenderConfigIn(code, ro, data, process.codeset->program_id);
}

RenderConfig FindRenderConfig(u64 program_id, VAddr text_addr, std::span<const u8> text,
                              VAddr ro_addr, std::span<const u8> ro, VAddr data_addr,
                              std::span<const u8> data) {
    CodeReader code_reader{text_addr, text};
    CodeReader ro_reader{ro_addr, ro};
    CodeReader data_reader{data_addr, data};
    return FindRenderConfigIn(code_reader, ro_reader, data_reader, program_id);
}

namespace {

/// FindRenderConfig's version: a title it searched before a change of what it finds is searched
/// again (its record keeps the player's choices).
constexpr unsigned FinderVersion = 4;

std::mutex engine_mutex;
bool engine_read = false;
std::map<u64, EngineChoices> engine_titles;

std::string EnginePath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "engine_settings.txt";
}

void ReadEngineLocked() {
    if (std::exchange(engine_read, true)) {
        return;
    }
    FILE* f = std::fopen(EnginePath().c_str(), "r");
    if (f == nullptr) {
        return;
    }
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        unsigned long long id = 0;
        unsigned finder = 0, defaults = 0, on = 0, off = 0, found = 0;
        char bits[96] = {};
        // A record of an older search has other bits or none; it is searched again.
        const int fields =
            line[0] == '#'
                ? 0
                : std::sscanf(line, "%llx finder %u found %u defaults %x on %x off %x bits %95s",
                              &id, &finder, &found, &defaults, &on, &off, bits);
        if (fields < 6) {
            continue;
        }
        EngineChoices& c = engine_titles[id];
        c = {finder == FinderVersion, found != 0, defaults, NoEngineBits, on, off};
        std::size_t s = 0;
        for (const char* at = bits; fields == 7 && *at != 0 && s < EngineSettingCount; s++) {
            const long bit = *at == '-' ? -1 : std::strtol(at, nullptr, 10);
            c.bits[s] = static_cast<s8>(bit >= 0 && bit < 32 ? bit : -1);
            at = std::strchr(at, ',');
            at = at != nullptr ? at + 1 : "";
        }
        if (s != EngineSettingCount) {
            c.scanned = false;
            c.bits = NoEngineBits;
        }
    }
    std::fclose(f);
}

void WriteEngineLocked() {
    FILE* f = std::fopen(EnginePath().c_str(), "w");
    if (f == nullptr) {
        LOG_ERROR(Loader, "Could not write {}", EnginePath());
        return;
    }
    std::fprintf(f, "# Azahar's MT Framework render settings by title ID: the search that looked, "
                    "whether its code has\n# the render config, the bits it starts with, the "
                    "player's (bits set, bits cleared), and\n# each setting's bit in its engine "
                    "(-: none).\n");
    for (const auto& [id, c] : engine_titles) {
        std::string bits;
        for (const s8 bit : c.bits) {
            bits += (bits.empty() ? "" : ",") + (bit >= 0 ? std::to_string(bit) : "-");
        }
        std::fprintf(f, "%016llX finder %u found %u defaults %#010x on %#010x off %#010x bits %s\n",
                     static_cast<unsigned long long>(id), c.scanned ? FinderVersion : 0u,
                     c.found ? 1u : 0u, c.defaults, c.on, c.off, bits.c_str());
    }
    std::fclose(f);
}

} // namespace

namespace {
EngineGuardTarget guard_target;
} // namespace

void SetEngineGuardTarget(const EngineGuardTarget& target) {
    guard_target = target;
}

const EngineGuardTarget& EngineGuardTargetNow() {
    return guard_target;
}

std::span<const EngineSetting> EngineSettings() {
    return engine_settings;
}

bool EngineSettingOn(const EngineChoices& choices, std::size_t setting) {
    const s8 bit = choices.bits[setting];
    if (bit < 0) {
        return false;
    }
    const u32 mask = 1u << bit;
    if ((choices.on & mask) != 0 || (choices.off & mask) != 0) {
        return (choices.on & mask) != 0;
    }
    switch (engine_settings[setting].azahar) {
    case EngineDefault::Off:
        return false;
    case EngineDefault::On:
        return true;
    case EngineDefault::Game:
        break;
    }
    return (choices.defaults & mask) != 0;
}

EngineGuardBits GuardBitsOf(const EngineChoices& choices) {
    u32 shown = 0;
    u32 on_by_default = 0;
    u32 off_by_default = 0;
    for (std::size_t s = 0; s < engine_settings.size(); s++) {
        if (choices.bits[s] < 0) {
            continue;
        }
        const u32 mask = 1u << choices.bits[s];
        shown |= mask;
        on_by_default |= engine_settings[s].azahar == EngineDefault::On ? mask : 0;
        off_by_default |= engine_settings[s].azahar == EngineDefault::Off ? mask : 0;
    }
    return {(choices.on | (on_by_default & ~choices.off)) & shown,
            (choices.off | (off_by_default & ~choices.on)) & shown};
}

EngineChoices EngineChoicesOf(u64 title_id) {
    std::scoped_lock lock{engine_mutex};
    ReadEngineLocked();
    const auto it = engine_titles.find(title_id);
    return it != engine_titles.end() ? it->second : EngineChoices{};
}

void SetEngineChoice(u64 title_id, u32 bit, bool on) {
    std::scoped_lock lock{engine_mutex};
    ReadEngineLocked();
    EngineChoices& c = engine_titles[title_id];
    c.on = on ? c.on | bit : c.on & ~bit;
    c.off = on ? c.off & ~bit : c.off | bit;
    WriteEngineLocked();
}

void NoteEngineFound(u64 title_id, u32 defaults, const std::array<s8, EngineSettingCount>& bits) {
    std::scoped_lock lock{engine_mutex};
    ReadEngineLocked();
    EngineChoices& c = engine_titles[title_id];
    if (c.scanned && c.found && c.defaults == defaults && c.bits == bits) {
        return;
    }
    c.scanned = true;
    c.found = true;
    c.defaults = defaults;
    c.bits = bits;
    WriteEngineLocked();
}

void NoteEngineAbsent(u64 title_id) {
    std::scoped_lock lock{engine_mutex};
    ReadEngineLocked();
    EngineChoices& c = engine_titles[title_id];
    c.scanned = true;
    c.found = false;
    WriteEngineLocked();
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
