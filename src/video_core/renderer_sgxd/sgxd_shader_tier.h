// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_set>
#include <vector>
#include "common/common_types.h"
#include "common/mutex.h"
#include "common/named_thread.h"
#include "common/wake_flag.h"
#include "video_core/pica/shader_setup.h"
#include "video_core/renderer_sgxd/sgxd_shader_disk_cache.h"
#include "video_core/renderer_sgxd/usse/usse_program.h"
#include "video_core/renderer_sgxd/usse/vs_usse_gen.h"
#include "video_core/shader/generator/pica_fs_config.h"

namespace SgxdRenderer {

class ShaderTier1;

/**
 * The second tier: programs that draw a lot are emitted again with the IR's optimising passes
 * (usse_ir level 2) on two worker threads, and the render thread installs the better program
 * before a later draw; the first tier's serves until then. Its workers emit the first tier's
 * queued jobs before their own: a draw may be waiting for those.
 */
class ShaderTier2 {
public:
    /// `disk_cache`: where each program emitted is stored for the runs after (null: nowhere).
    /// `tier1`: whose queued jobs the workers emit first; it outlives this.
    ShaderTier2(ShaderDiskCache* disk_cache, ShaderTier1& tier1);
    ~ShaderTier2();

    ShaderTier2(const ShaderTier2&) = delete;
    ShaderTier2& operator=(const ShaderTier2&) = delete;

    void QueueFragment(u64 key, const Pica::Shader::FSConfig& config);
    /// Copies the PICA code and swizzle data the request points at.
    void QueueVertex(u64 key, const Usse::VsRequest& request);

    struct Result {
        u64 key;
        bool vertex;
        std::optional<Usse::Program> program; ///< empty when level 2 failed
        u32 input_regs; ///< a vertex program's, which the first tier's must equal
        std::string refusal;
        u32 took_us;
        u32 took_cycles; ///< the same time in the cycle counter's counts
    };
    [[nodiscard]] bool Ready() const {
        return ready.load(std::memory_order_acquire);
    }
    std::vector<Result> Take();

private:
    struct Job {
        u64 key;
        std::optional<Pica::Shader::FSConfig> fs; ///< a fragment program's, else a vertex one
        Usse::VsRequest vs;                       ///< its spans point into code and swizzle
        Pica::ProgramCode code;
        Pica::SwizzleData swizzle;
    };
    /// `scratch`: the worker's own (Tier2 or Tier2b).
    void Loop(std::stop_token stop, Usse::Scratch scratch);

    ShaderDiskCache* const disk_cache;
    ShaderTier1& tier1;
    Common::Mutex mutex;
    /// unique_ptr: a vertex job's request points into the job's own arrays.
    std::deque<std::unique_ptr<Job>> jobs;
    std::vector<Result> results;
    std::atomic<bool> ready{};
    Common::WakeFlag wake;
    /// A fragment program's passes can take hundreds of ms: with one worker, every job behind it
    /// would wait as long.
    std::array<Common::NamedThread, 2> threads;
};

/**
 * The first tier off the render thread. Its worker and the second tier's two workers emit
 *  - each fragment state the parse thread meets, from its PICA registers with no own-pixel units
 *    and no viewport kill (what most draws take), before the render thread reaches its draw;
 *  - each vertex program the parse thread meets at a draw without a geometry shader;
 *  - one of a new geometry draw's two programs while the render thread emits the other.
 * A job the disk cache has is read from there (the second tier's program) instead.
 *
 * The jobs are 64 slots and three masks, no lock: `used` (a job is in the slot), `queued` (it
 * waits for an emitter: whoever clears its bit emits it, a worker or the render thread) and
 * `done` (emitted: the render thread takes it by clearing the bit). The parse thread fills free
 * fragment and vertex slots, the render thread the geometry slot; only the render thread empties
 * slots. At the draw the render thread finds its slot by key: still queued, it emits the program
 * itself; emitted, it takes it; being emitted, it emits the other queued jobs meanwhile and
 * waits only when none is left.
 */
class ShaderTier1 {
public:
    /// `disk_cache`: where a job's program is looked for before it is emitted.
    explicit ShaderTier1(ShaderDiskCache* disk_cache);
    ~ShaderTier1();

    ShaderTier1(const ShaderTier1&) = delete;
    ShaderTier1& operator=(const ShaderTier1&) = delete;

    /// A fragment program's key by its config alone (the disk cache's, the jobs'): its bytes'
    /// 64-bit hash (FSConfig::Hash is a size_t, 32 bits here).
    [[nodiscard]] static u64 ConfigKey(const Pica::Shader::FSConfig& config);

    /// A vertex job's PICA code and swizzle data, shared by the jobs of one program's variants.
    struct VertexCode {
        Pica::ProgramCode code;
        Pica::SwizzleData swizzle;
    };

    /// Parse thread: the program of a fragment state, once a session, when a slot is free.
    void QueueFragment(const Pica::Shader::FSConfig& config);
    /// Parse thread: whether QueueVertex would take the key (not met this session, a slot free).
    [[nodiscard]] bool WantsVertex(u64 key) const {
        return !vertices_met.contains(key) && FreeSlot() != Slots;
    }
    /// Parse thread: a vertex program, once a session, when a slot is free. `request`'s spans
    /// point into `code`.
    void QueueVertex(u64 key, const Usse::VsRequest& request,
                     std::shared_ptr<const VertexCode> code);
    /// Render thread: a geometry program, ahead of the other jobs. Copies the PICA code and
    /// swizzle data the request points at. One at a time: TakeGeometry empties the slot.
    void QueueGeometry(u64 key, const Usse::VsRequest& request);

    struct Taken {
        std::optional<Usse::Program> program; ///< empty when the emitter refused
        u32 input_regs;
        std::string refusal;
        u32 took_us;    ///< its emission, or its read from the disk
        u32 waited_us;  ///< the render thread's wait for it
        bool helped;    ///< the render thread emitted other jobs while a worker emitted it
        bool from_disk; ///< the disk cache's program, the second tier's
    };
    /// Render thread: the program of the job for `config`, emitted or waited for; nullopt when
    /// no job has that config or the render thread claimed it (then it emits it itself).
    [[nodiscard]] std::optional<Taken> TakeFragment(const Pica::Shader::FSConfig& config);
    /// Render thread: as TakeFragment, for QueueVertex's job of `key`.
    [[nodiscard]] std::optional<Taken> TakeVertex(u64 key);
    /// Render thread: as TakeFragment, for QueueGeometry's job of `key`.
    [[nodiscard]] std::optional<Taken> TakeGeometry(u64 key);

    /// Claims a queued job (the geometry one first, else the oldest) and emits it; false when
    /// none was queued. Any thread, naming itself (`scratch`).
    bool EmitOne(Usse::Scratch scratch);
    [[nodiscard]] bool Pending() const {
        return queued.load(std::memory_order_acquire) != 0;
    }
    /// Raised with the worker's flag at every job queued: the second tier's workers wait on it.
    void SetHelperWake(Common::WakeFlag* flag) {
        helper_wake = flag;
    }

private:
    static constexpr u32 Slots = 64;
    static constexpr u32 GeometrySlot = Slots - 1;
    static constexpr u64 GeometryBit = u64{1} << GeometrySlot;
    /// An emitted program no draw took within this long is dropped (another state's prediction,
    /// or a draw the render thread left out).
    static constexpr u64 StaleUs = 2'000'000;
    enum class Kind : u8 { Fragment, Vertex, Geometry };
    struct Slot {
        u64 key;
        Kind kind;
        std::optional<Pica::Shader::FSConfig> fs; ///< a fragment slot's
        Usse::VsRequest vs;                       ///< a vertex slot's, its spans into vs_code
        std::shared_ptr<const VertexCode> vs_code;
        u64 queued_us;
        Taken taken;
    };
    struct GeometryJob {
        Usse::VsRequest vs;
        Pica::ProgramCode code;
        Pica::SwizzleData swizzle;
    };
    std::optional<Taken> Take(u64 key, Kind kind, const Pica::Shader::FSConfig* config);
    [[nodiscard]] u32 FreeSlot() const;
    void Publish(u32 slot);
    void Emit(u32 slot, Usse::Scratch scratch);
    void Free(u32 slot);
    void Loop(std::stop_token stop);

    ShaderDiskCache* const disk_cache;
    std::array<Slot, Slots> slots{};
    std::unique_ptr<GeometryJob> geometry = std::make_unique<GeometryJob>();
    std::atomic<u64> used{0};
    std::atomic<u64> queued{0};
    std::atomic<u64> done{0};
    /// The parse thread's: every fragment state and vertex program queued this session.
    std::unordered_set<u64> fragments_met;
    std::unordered_set<u64> vertices_met;
    Common::WakeFlag wake;    ///< a job queued
    Common::WakeFlag emitted; ///< a job emitted, for the render thread's wait
    Common::WakeFlag* helper_wake = nullptr;
    Common::NamedThread thread;
};

} // namespace SgxdRenderer
