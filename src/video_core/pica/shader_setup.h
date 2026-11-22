// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include "common/vector_math.h"
#include "video_core/pica/packed_attribute.h"
#include "video_core/pica_types.h"

namespace Pica {

constexpr u32 MAX_PROGRAM_CODE_LENGTH = 4096;
constexpr u32 MAX_SWIZZLE_DATA_LENGTH = 4096;

using ProgramCode = std::array<u32, MAX_PROGRAM_CODE_LENGTH>;
using SwizzleData = std::array<u32, MAX_SWIZZLE_DATA_LENGTH>;

struct Uniforms {
    alignas(16) std::array<Common::Vec4<f24>, 96> f;
    std::array<bool, 16> b;
    std::array<Common::Vec4<u8>, 4> i;

    static std::size_t GetFloatUniformOffset(u32 index) {
        return offsetof(Uniforms, f) + index * sizeof(Common::Vec4<f24>);
    }

    static std::size_t GetBoolUniformOffset(u32 index) {
        return offsetof(Uniforms, b) + index * sizeof(bool);
    }

    static std::size_t GetIntUniformOffset(u32 index) {
        return offsetof(Uniforms, i) + index * sizeof(Common::Vec4<u8>);
    }

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version) {
        ar & f;
        ar & b;
        ar & i;
    }
};

struct ShaderRegs;

/**
 * This structure contains the state information common for all shader units such as uniforms.
 * The geometry shaders has a unique configuration so when enabled it has its own setup.
 */
struct ShaderSetup {
private:
    void MakeProgramCodeDirty() {
        program_code_hash_dirty = true;
        // program_code_pending_fixup = true;
        // has_fixup = false;
    }

    void MakeSwizzleDataDirty() {
        swizzle_data_hash_dirty = true;
    }

public:
    explicit ShaderSetup();
    ~ShaderSetup();

    void WriteUniformBoolReg(u32 value);

    void WriteUniformIntReg(u32 index, const Common::Vec4<u8> values);

    std::optional<u32> WriteUniformFloatReg(ShaderRegs& config, u32 value);

    struct UniformWriteRange {
        u32 first_index;
        u32 count;
    };
    std::optional<UniformWriteRange> WriteUniformFloatRegRange(ShaderRegs& config,
                                                               const u32* values, u32 count);

    u64 GetProgramCodeHash();

    u64 GetSwizzleDataHash();

    void DoProgramCodeFixup();

    inline void UpdateProgramCode(size_t offset, u32 value) {
        u32& inst = program_code[offset];
        u32 old = inst;

        if (old == value)
            return;

        inst = value;
        if (!program_code_hash_dirty) {
            MakeProgramCodeDirty();
        }
        if ((offset + 1) > biggest_program_size) {
            biggest_program_size = offset + 1;
        }
        CodeChanged(static_cast<u32>(offset), static_cast<u32>(offset) + 1);
    }

    void UpdateProgramCodeRange(size_t offset, const u32* __restrict values, u32 count);

    void UpdateProgramCode(const ProgramCode& other, u32 other_size = MAX_PROGRAM_CODE_LENGTH) {
        program_code = other;
        biggest_program_size = std::max(biggest_program_size, other_size);
        MakeProgramCodeDirty();
        CodeChanged(0, MAX_PROGRAM_CODE_LENGTH);
    }

    inline void UpdateSwizzleData(size_t offset, u32 value) {
        u32& data = swizzle_data[offset];
        u32 old = data;

        if (old == value)
            return;

        data = value;
        if (!swizzle_data_hash_dirty) {
            MakeSwizzleDataDirty();
        }
        if ((offset + 1) > biggest_swizzle_size) {
            biggest_swizzle_size = offset + 1;
        }
        SwizzleChanged(static_cast<u32>(offset), static_cast<u32>(offset) + 1);
    }

    void UpdateSwizzleDataRange(size_t offset, const u32* __restrict values, u32 count);

    void UpdateSwizzleData(const SwizzleData& other, u32 other_size = MAX_SWIZZLE_DATA_LENGTH) {
        swizzle_data = other;
        biggest_swizzle_size = std::max(biggest_swizzle_size, other_size);
        MakeSwizzleDataDirty();
        SwizzleChanged(0, MAX_SWIZZLE_DATA_LENGTH);
    }

    const ProgramCode& GetProgramCode() const {
        // return (has_fixup) ? program_code_fixup : program_code;
        return program_code;
    }

    const SwizzleData& GetSwizzleData() const {
        return swizzle_data;
    }

    u32 GetBiggestProgramSize() const {
        return biggest_program_size;
    }

    u32 GetBiggestSwizzleSize() const {
        return biggest_swizzle_size;
    }

    void SetRequiresShaderFixup(bool _requires_fixup) {
        // requires_fixup = _requires_fixup;
    }

    /**
     * What changed since the sgxd render thread's copy of this setup last took it
     * (SgxdRenderer::RenderThread): the float uniforms by index, the bool and int uniforms, and
     * the program and swizzle words in [lo, hi). The writes above set them. uniforms_dirty
     * with none of the uniform marks set (an assignment, as vprof's replay makes) stands for
     * every uniform.
     */
    std::array<u32, 3> f_changed{};
    bool bi_changed{};
    u32 code_lo = MAX_PROGRAM_CODE_LENGTH;
    u32 code_hi = 0;
    u32 swizzle_lo = MAX_SWIZZLE_DATA_LENGTH;
    u32 swizzle_hi = 0;

    /// The VS's float uniform copied into the GS's (PicaCore: the GS mirrors the VS), marked as
    /// a write is.
    void MirrorFloatUniform(u32 index, const Common::Vec4<f24>& value) {
        if (uniforms.f[index] != value) {
            uniforms.f[index] = value;
            FloatChanged(index);
        }
    }

    /// Everything as changed, for the render thread's copy to take whole.
    void MarkAllChanged() {
        f_version++;
        uniforms_dirty = true;
        f_changed.fill(~0u);
        bi_changed = true;
        CodeChanged(0, MAX_PROGRAM_CODE_LENGTH);
        SwizzleChanged(0, MAX_SWIZZLE_DATA_LENGTH);
    }

    /// The render thread's copy: words [lo, lo + count) of the program or the swizzle data, and
    /// the biggest size, as the emulation side's setup has them.
    void TakeProgramCode(u32 lo, const u32* words, u32 count, u32 biggest);
    void TakeSwizzleData(u32 lo, const u32* words, u32 count, u32 biggest);

public:
    Uniforms uniforms;
    PackedAttribute uniform_queue;
    u32 entry_point{};
    const void* cached_shader{};
    bool uniforms_dirty = true;
    /// Float uniform `index` changed.
    void FloatChanged(u32 index) {
        uniforms_dirty = true;
        f_changed[index / 32] |= 1u << (index % 32);
        f_version++;
#ifdef VITA_DIAGNOSTICS
        f_stamp[index] = f_version;
#endif
    }

    // bool requires_fixup = false;
    // bool has_fixup = false;

private:
    void CodeChanged(u32 lo, u32 hi) {
        code_lo = std::min(code_lo, lo);
        code_hi = std::max(code_hi, hi);
    }
    void SwizzleChanged(u32 lo, u32 hi) {
        swizzle_lo = std::min(swizzle_lo, lo);
        swizzle_hi = std::max(swizzle_hi, hi);
    }

    ProgramCode program_code{};
    SwizzleData swizzle_data{};
    bool program_code_hash_dirty{true};
    bool swizzle_data_hash_dirty{true};
    u32 biggest_program_size = 0;
    u32 biggest_swizzle_size = 0;
    u64 program_code_hash{0};
    u64 swizzle_data_hash{0};

    // ProgramCode program_code_fixup{};
    // bool program_code_pending_fixup{true};

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version) {
        ar & uniforms;
        ar & uniform_queue;
        ar & program_code;
        ar & swizzle_data;
        ar & program_code_hash_dirty;
        ar & swizzle_data_hash_dirty;
        ar & biggest_program_size;
        ar & biggest_swizzle_size;
        ar & program_code_hash;
        ar & swizzle_data_hash;

        // ar & program_code_fixup;
        // ar & program_code_pending_fixup;
        // ar & requires_fixup;
        // ar & has_fixup;
        if (Archive::is_loading::value) {
            MarkAllChanged();
        }
    }
};

} // namespace Pica
