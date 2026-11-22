// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <bit>
#include <cstring>
#include <utility>
#include "common/assert.h"
#include "common/bit_set.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "video_core/pica/regs_shader.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/pica/words_differ.h"

namespace {

/// A float24's bits as the float32 Pica::f24::FromRaw makes of it: the common exponents inline,
/// zero, denormals, infinities and NaNs through FromRaw.
u32 Float24Bits(u32 raw) {
    const u32 exponent = (raw >> 16) & 0x7f;
    if (exponent != 0 && exponent != 0x7f) [[likely]] {
        return (raw >> 23) << 31 | (exponent + 64) << 23 | (raw & 0xffff) << 7;
    }
    return std::bit_cast<u32>(Pica::f24::FromRaw(raw).ToFloat32());
}

} // Anonymous namespace

namespace Pica {

ShaderSetup::ShaderSetup() = default;

ShaderSetup::~ShaderSetup() = default;

void ShaderSetup::WriteUniformBoolReg(u32 value) {
    const auto bits = BitSet32(value);
    bool changed = false;
    for (u32 i = 0; i < uniforms.b.size(); ++i) {
        const bool prev = std::exchange(uniforms.b[i], bits[i]);
        changed |= prev != bits[i];
    }
    uniforms_dirty |= changed;
    bi_changed |= changed;
}

void ShaderSetup::WriteUniformIntReg(u32 index, const Common::Vec4<u8> values) {
    ASSERT(index < uniforms.i.size());
    const auto prev = std::exchange(uniforms.i[index], values);
    const bool changed = prev != values;
    uniforms_dirty |= changed;
    bi_changed |= changed;
}

std::optional<u32> ShaderSetup::WriteUniformFloatReg(ShaderRegs& config, u32 value) {
    auto& uniform_setup = config.uniform_setup;
    const bool is_float32 = uniform_setup.IsFloat32();
    if (!uniform_queue.Push(value, is_float32)) {
        return std::nullopt;
    }

    const auto uniform = uniform_queue.Get(is_float32);
    if (uniform_setup.index >= uniforms.f.size()) {
        // Some games may submit OOB indexes (mainly 0x7F), which is
        // ignored on real HW.
        return std::nullopt;
    }

    const u32 index = uniform_setup.index.Value();
    const auto prev = std::exchange(uniforms.f[index], uniform);
    if (prev != uniform) {
        FloatChanged(index);
    }
    uniform_setup.index.Assign(index + 1);
    return index;
}

std::optional<ShaderSetup::UniformWriteRange> ShaderSetup::WriteUniformFloatRegRange(
    ShaderRegs& config, const u32* values, u32 count) {

    if (count == 0) [[unlikely]] {
        return std::nullopt;
    }

    auto& uniform_setup = config.uniform_setup;
    const bool is_float32 = uniform_setup.IsFloat32();

    std::optional<u32> first_index;
    u32 written = 0;
    u32 i = 0;

    if (uniform_queue.index == 0) {
        // Whole vectors with the queue empty: four float32 words a vector, the vector the words in
        // reverse (AsFloat32's order), or three words holding four float24s (AsFloat24's packing,
        // decoded inline: Float24Bits), compared and stored as words and the index advanced once,
        // rather than a word at a time through the queue.
        static_assert(sizeof(uniforms.f[0]) == 4 * sizeof(u32));
        const u32 step = is_float32 ? 4 : 3;
        const u32 first = uniform_setup.index.Value();
        const u32 size = static_cast<u32>(uniforms.f.size());
        u32 index = first;
        bool out_of_range = false;
        for (; count - i >= step; i += step, ++index) {
            if (index >= size) [[unlikely]] {
                // Some games may submit OOB indexes (mainly 0x7F), which is ignored on real HW:
                // the upload ends, as below.
                out_of_range = true;
                break;
            }
            u32 vector[4];
            if (is_float32) {
                vector[0] = values[i + 3];
                vector[1] = values[i + 2];
                vector[2] = values[i + 1];
                vector[3] = values[i];
            } else {
                const u32 b0 = values[i], b1 = values[i + 1], b2 = values[i + 2];
                vector[0] = Float24Bits(b2 & 0xffffff);
                vector[1] = Float24Bits((b1 & 0xffff) << 8 | b2 >> 24);
                vector[2] = Float24Bits((b0 & 0xff) << 16 | b1 >> 16);
                vector[3] = Float24Bits(b0 >> 8);
            }
            u32 held[4];
            std::memcpy(held, &uniforms.f[index], sizeof(held));
            if ((held[0] ^ vector[0]) | (held[1] ^ vector[1]) | (held[2] ^ vector[2]) |
                (held[3] ^ vector[3])) {
                std::memcpy(&uniforms.f[index], vector, sizeof(vector));
                FloatChanged(index);
            }
        }
        uniform_setup.index.Assign(index);
        written = index - first;
        if (written != 0) {
            first_index = first;
        }
        if (out_of_range) {
            return written == 0 ? std::nullopt : std::optional{UniformWriteRange{first, written}};
        }
    }

    // Float24 vectors, and a float32 vector the queue holds part of: a word at a time.
    for (; i < count; ++i) {
        if (!uniform_queue.Push(values[i], is_float32)) {
            continue;
        }

        const auto uniform = uniform_queue.Get(is_float32);
        if (uniform_setup.index >= uniforms.f.size()) [[unlikely]] {
            // Some games may submit OOB indexes (mainly 0x7F), which is
            // ignored on real HW.
            break;
        }

        const u32 index = uniform_setup.index.Value();
        const auto prev = std::exchange(uniforms.f[index], uniform);
        if (prev != uniform) {
            FloatChanged(index);
        }
        uniform_setup.index.Assign(index + 1);

        if (!first_index) {
            first_index = index;
        }
        ++written;
    }

    if (written == 0) {
        return std::nullopt;
    }
    return UniformWriteRange{*first_index, written};
}

u64 ShaderSetup::GetProgramCodeHash() {
    if (program_code_hash_dirty) {
        const auto& prog_code = GetProgramCode();
        program_code_hash =
            Common::ComputeHash64(prog_code.data(), biggest_program_size * sizeof(u32));
        program_code_hash_dirty = false;
    }
    return program_code_hash;
}

u64 ShaderSetup::GetSwizzleDataHash() {
    if (swizzle_data_hash_dirty) {
        swizzle_data_hash =
            Common::ComputeHash64(swizzle_data.data(), biggest_swizzle_size * sizeof(u32));
        swizzle_data_hash_dirty = false;
    }
    return swizzle_data_hash;
}

// The last of count words at dst that differs from values, after WordsDiffer found one: from the
// end, since an upload that changes a program usually changes its last words too.
static u32 LastDifferent(const u32* dst, const u32* values, u32 count) {
    u32 last = count - 1;
    while (dst[last] == values[last]) {
        --last;
    }
    return last;
}

void ShaderSetup::UpdateProgramCodeRange(size_t offset, const u32* __restrict values, u32 count) {
    if (count == 0) [[unlikely]] {
        return;
    }

    u32* __restrict dst = &program_code[offset];
    // Vertex programs are uploaded every frame, some unchanged and some over another one: one
    // compare pass; with a change, the words go in whole (the equal ones over themselves) and the
    // last that differs bounds the change.
    if (!WordsDiffer(dst, values, count)) {
        return;
    }
    const size_t last_changed_offset = offset + LastDifferent(dst, values, count);
    std::memcpy(dst, values, count * sizeof(u32));
    if (!program_code_hash_dirty) {
        MakeProgramCodeDirty();
    }
    if ((last_changed_offset + 1) > biggest_program_size) {
        biggest_program_size = last_changed_offset + 1;
    }
    CodeChanged(static_cast<u32>(offset), static_cast<u32>(last_changed_offset) + 1);
}

void ShaderSetup::UpdateSwizzleDataRange(size_t offset, const u32* __restrict values, u32 count) {
    if (count == 0) [[unlikely]] {
        return;
    }

    u32* __restrict dst = &swizzle_data[offset];
    if (!WordsDiffer(dst, values, count)) {
        return;
    }
    const size_t last_changed_offset = offset + LastDifferent(dst, values, count);
    std::memcpy(dst, values, count * sizeof(u32));
    if (!swizzle_data_hash_dirty) {
        MakeSwizzleDataDirty();
    }
    if ((last_changed_offset + 1) > biggest_swizzle_size) {
        biggest_swizzle_size = last_changed_offset + 1;
    }
    SwizzleChanged(static_cast<u32>(offset), static_cast<u32>(last_changed_offset) + 1);
}

void ShaderSetup::TakeProgramCode(u32 lo, const u32* words, u32 count, u32 biggest) {
    std::memcpy(program_code.data() + lo, words, count * sizeof(u32));
    biggest_program_size = biggest;
    MakeProgramCodeDirty();
}

void ShaderSetup::TakeSwizzleData(u32 lo, const u32* words, u32 count, u32 biggest) {
    std::memcpy(swizzle_data.data() + lo, words, count * sizeof(u32));
    biggest_swizzle_size = biggest;
    MakeSwizzleDataDirty();
}

void ShaderSetup::DoProgramCodeFixup() {
    // This function holds shader fixups that are required for proper emulation of some games. As
    // emulation gets more accurate the goal is to have this function as empty as possible.
    // WARNING: If the hashing method is changed, the hashes of the different shaders will need
    // adjustment.

    /*
    if (!requires_fixup || !program_code_pending_fixup) {
        return;
    }
    program_code_pending_fixup = false;

    auto prepare_for_fixup = [this]() {
        memcpy(program_code_fixup.data(), program_code.data(), program_code.size());
        has_fixup = true;
        program_code_hash_dirty = true;
    };
    */

    // No games require shader fixups right now
}

} // namespace Pica
