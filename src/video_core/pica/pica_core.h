// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"
#include "core/hle/service/gsp/gsp_interrupt.h"
#include "video_core/pica/dirty_regs.h"
#include "video_core/pica/geometry_pipeline.h"
#include "video_core/pica/packed_attribute.h"
#include "video_core/pica/primitive_assembly.h"
#include "video_core/pica/regs_external.h"
#include "video_core/pica/regs_internal.h"
#include "video_core/pica/regs_lcd.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/pica/shader_unit.h"

namespace Memory {
class MemorySystem;
}

namespace VideoCore {
class RasterizerInterface;
}

namespace Pica {

class ShaderEngine;

/// Upstream's model of how long the PICA took for a command list, which delayed P3D. The GSP's
/// interrupts now come when the SGX has done the work (VideoCore::GPU::Signal), so nothing counts
/// any more; the two counts stay in the savestate archive for the states made before.
struct ArchivedDelayCounts {
    size_t triangle_count{};
    size_t command_count{};
    template <class Archive>
    void serialize(Archive& ar, const u32) {
        ar & triangle_count;
        ar & command_count;
    }
};

class PicaCore {
public:
    explicit PicaCore(Memory::MemorySystem& memory);
    ~PicaCore();

    void BindRasterizer(VideoCore::RasterizerInterface* rasterizer);

    void SetInterruptHandler(Service::GSP::InterruptHandler& signal_interrupt);

    void ProcessCmdList(PAddr list, u32 size, bool ignore_list);

    // Special-register dispatch: a handler per register id, called through tables indexed by
    // the id; see PicaCore::Dispatch in pica_core.cpp.
    using SpecialFn = void (*)(PicaCore&, u32 id, u32 value, bool& stop_requested);
    using BatchFn = void (*)(PicaCore&, const u32* values, u32 count);
    struct Dispatch;

private:
    void InitializeRegs();


    void WriteInternalRegBatch(u32 id, const u32* values, u32 count, u32 mask,
                               bool& stop_requested);

    void WriteInternalRegSequential(u32 id, const u32* __restrict values, u32 count, u32 mask,
                                    bool& stop_requested);

    void WriteInternalReg(u32 id, u32 value, u32 mask, bool& stop_requested);

    void SubmitImmediate(u32 data);

    void DrawImmediate();
    /// Draws the immediate-mode vertices the rasterizer collected (AccelerateImmediateVertex).
    void FlushImmediate();
    /// True for the registers that carry immediate-mode data, which do not end a batch.
    static constexpr bool IsImmediateData(u32 id) {
        return id >= PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[0]) &&
               id <= PICA_REG_INDEX(pipeline.vs_default_attributes_setup.set_value[2]);
    }

    void DrawArrays(bool is_indexed);
    /// The primitive assembler's topology from the register, before the assembler is used.
    void SyncAssemblerTopology();

    void LoadVertices(bool is_indexed);

public:
    union Regs {
        static constexpr std::size_t NUM_REGS = 0x732;

        struct {
            u32 hardware_id;
            INSERT_PADDING_WORDS(0x3);
            MemoryFillConfig memory_fill_config[2];
            u32 vram_bank_control;
            u32 gpu_busy;
            INSERT_PADDING_WORDS(0x22);
            u32 backlight_control;
            INSERT_PADDING_WORDS(0xCF);
            FramebufferConfig framebuffer_config[2];
            INSERT_PADDING_WORDS(0x180);
            DisplayTransferConfig display_transfer_config;
            INSERT_PADDING_WORDS(0xF5);
            RegsInternal internal;
        };
        std::array<u32, NUM_REGS> reg_array;
    };
    static_assert(sizeof(Regs) == Regs::NUM_REGS * sizeof(u32));

    struct CommandList {
        PAddr addr;
        const u32* head;
        u32 current_index;
        u32 length;

        void Reset(PAddr addr, const u8* head, u32 size) {
            this->addr = addr;
            this->head = reinterpret_cast<const u32*>(head);
            this->length = size / sizeof(u32);
            current_index = 0;
        }

    private:
        friend class boost::serialization::access;
        template <class Archive>
        void serialize(Archive& ar, const u32 file_version);
    };

    struct ImmediateModeState {
        AttributeBuffer input_vertex{};
        u32 current_attribute{};
        bool reset_geometry_pipeline{true};
        PackedAttribute queue;

        void Reset() {
            current_attribute = 0;
            reset_geometry_pipeline = true;
            queue.Reset();
        }

    private:
        friend class boost::serialization::access;
        template <class Archive>
        void serialize(Archive& ar, const u32 file_version) {
            ar & input_vertex;
            ar & current_attribute;
            ar & reset_geometry_pipeline;
            ar & queue;
        }
    };

    struct ProcTex {
        static constexpr u8 TableAllDirty = 0xFF;

        union ValueEntry {
            u32 raw;

            // LUT value, encoded as 12-bit fixed point, with 12 fraction bits
            BitField<0, 12, u32> value; // 0.0.12 fixed point

            // Difference between two entry values. Used for efficient interpolation.
            // 0.0.12 fixed point with two's complement. The range is [-0.5, 0.5).
            // Note: the type of this is different from the one of lighting LUT
            BitField<12, 12, s32> difference;

            f32 ToFloat() const {
                return static_cast<f32>(value) / 4095.f;
            }

            f32 DiffToFloat() const {
                return static_cast<f32>(difference) / 4095.f;
            }
        };

        union ColorEntry {
            u32 raw;
            BitField<0, 8, u32> r;
            BitField<8, 8, u32> g;
            BitField<16, 8, u32> b;
            BitField<24, 8, u32> a;

            Common::Vec4<u8> ToVector() const {
                return {static_cast<u8>(r), static_cast<u8>(g), static_cast<u8>(b),
                        static_cast<u8>(a)};
            }
        };

        union ColorDifferenceEntry {
            u32 raw;
            BitField<0, 8, s32> r; // half of the difference between two ColorEntry
            BitField<8, 8, s32> g;
            BitField<16, 8, s32> b;
            BitField<24, 8, s32> a;

            Common::Vec4<s32> ToVector() const {
                return Common::Vec4<s32>{r, g, b, a} * 2;
            }
        };

        std::array<ValueEntry, 128> noise_table;
        std::array<ValueEntry, 128> color_map_table;
        std::array<ValueEntry, 128> alpha_map_table;
        std::array<ColorEntry, 256> color_table;
        std::array<ColorDifferenceEntry, 256> color_diff_table;
        union {
            u8 table_dirty = TableAllDirty;
            BitField<0, 1, u8> noise_lut_dirty;
            BitField<2, 1, u8> color_map_dirty;
            BitField<3, 1, u8> alpha_map_dirty;
            BitField<4, 1, u8> lut_dirty;
            BitField<5, 1, u8> diff_lut_dirty;
        };

    private:
        friend class boost::serialization::access;
        template <class Archive>
        void serialize(Archive& ar, const u32 file_version) {
            ar& boost::serialization::make_binary_object(this, sizeof(ProcTex));
            if (Archive::is_loading::value) {
                table_dirty = TableAllDirty;
            }
        }
    };

    struct Lighting {
        static constexpr u32 LutAllDirty = 0xFFFFFF;

        union LutEntry {
            // Used for raw access
            u32 raw;

            // LUT value, encoded as 12-bit fixed point, with 12 fraction bits
            BitField<0, 12, u32> value; // 0.0.12 fixed point

            // Used for efficient interpolation.
            BitField<12, 11, u32> difference; // 0.0.11 fixed point
            BitField<23, 1, u32> neg_difference;

            f32 ToFloat() const {
                return static_cast<f32>(value) / 4095.f;
            }

            f32 DiffToFloat() const {
                const f32 diff = static_cast<f32>(difference) / 2047.f;
                return neg_difference ? -diff : diff;
            }

            template <class Archive>
            void serialize(Archive& ar, const u32 file_version) {
                ar & raw;
            }
        };

        std::array<std::array<LutEntry, 256>, 24> luts;
        u32 lut_dirty = LutAllDirty;

    private:
        friend class boost::serialization::access;
        template <class Archive>
        void serialize(Archive& ar, const u32 file_version) {
            ar& boost::serialization::make_binary_object(this, sizeof(Lighting));
            if (Archive::is_loading::value) {
                lut_dirty = LutAllDirty;
            }
        }
    };

    struct Fog {
        union LutEntry {
            // Used for raw access
            u32 raw;

            BitField<0, 13, s32> difference; // 1.1.11 fixed point
            BitField<13, 11, u32> value;     // 0.0.11 fixed point

            f32 ToFloat() const {
                return static_cast<f32>(value) / 2047.0f;
            }

            f32 DiffToFloat() const {
                return static_cast<f32>(difference) / 2047.0f;
            }
        };

        std::array<LutEntry, 128> lut;
        bool lut_dirty = true;

    private:
        friend class boost::serialization::access;
        template <class Archive>
        void serialize(Archive& ar, const u32 file_version) {
            ar& boost::serialization::make_binary_object(this, sizeof(Fog));
            if (Archive::is_loading::value) {
                lut_dirty = true;
            }
        }
    };

    RegsLcd regs_lcd{};
    Regs regs{};
    DirtyRegs dirty_regs{};
    GeometryShaderUnit gs_unit;
    ShaderSetup vs_setup;
    /// The geometry unit's setup. While it mirrors the vertex unit's (GsMirrorsVs) it may lag:
    /// FlushGsMirror brings it up to date, which everything that reads it as the geometry
    /// unit's own does first.
    ShaderSetup gs_setup;
    ProcTex proctex{};
    Lighting lighting{};
    Fog fog{};
    AttributeBuffer input_default_attributes{};
    ImmediateModeState immediate{};
    /// The rasterizer holds immediate-mode vertices: every register write goes through
    /// WriteInternalReg, which flushes them first (the fast command loop is skipped).
    bool immediate_open{};

    ArchivedDelayCounts delay_counts{};

    /// Whether the geometry unit takes the vertex unit's uniforms, code and swizzles: not
    /// configured on its own, and no geometry shader.
    bool GsMirrorsVs() const {
        return !regs.internal.pipeline.gs_unit_exclusive_configuration &&
               regs.internal.pipeline.use_gs == PipelineRegs::UseGS::No;
    }

    /// The vertex unit's writes made while the geometry unit mirrored it, into gs_setup: the
    /// geometry unit's state as each mirrored write would have left it.
    void FlushGsMirror();

private:
    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version) {
        if constexpr (Archive::is_saving::value) {
            FlushGsMirror();
        }
        ar & regs_lcd;
        ar & regs.reg_array;
        ar & gs_unit;
        ar & vs_setup;
        ar & gs_setup;
        ar & proctex;
        ar & lighting;
        ar & fog;
        ar & input_default_attributes;
        ar & immediate;
        ar & delay_counts;
        ar & geometry_pipeline;
        ar & primitive_assembler;
        ar & cmd_list;
        if (Archive::is_loading::value) {
            dirty_regs.SetAllDirty();
        }
    }

public:
    struct RenderPropertiesGuess {
        u32 vp_height;
        PAddr paddr;
        bool vp_heigh_found = false;
        bool paddr_found = false;
    };

    RenderPropertiesGuess GuessCmdRenderProperties(PAddr list, u32 size);

private:
    Memory::MemorySystem& memory;
    VideoCore::RasterizerInterface* rasterizer;
    Service::GSP::InterruptHandler signal_interrupt;
    GeometryPipeline geometry_pipeline;
    PrimitiveAssembler primitive_assembler;
    CommandList cmd_list;
    std::unique_ptr<ShaderEngine> shader_engine;

    /// What the vertex unit wrote while the geometry unit mirrored it, not yet in gs_setup
    /// (FlushGsMirror): float uniforms, int uniforms and bools by index, code and swizzle words by
    /// bitmap (a word between two uploads stays the geometry unit's own). A game can mirror every
    /// upload into a setup no draw of its reads.
    struct GsMirrorLog {
        std::array<u32, 3> f{};
        u32 i{};
        bool b{};
        bool pending{};
        std::array<u32, MAX_PROGRAM_CODE_LENGTH / 32> code{};
        std::array<u32, MAX_SWIZZLE_DATA_LENGTH / 32> swizzle{};
    };
    GsMirrorLog gs_mirror{};
};

#define GPU_REG_INDEX(field_name) (offsetof(Pica::PicaCore::Regs, field_name) / sizeof(u32))

} // namespace Pica
