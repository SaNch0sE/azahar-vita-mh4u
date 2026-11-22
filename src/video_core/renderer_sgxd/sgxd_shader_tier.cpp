// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <bit>
#include <utility>
#include "azahar_native.h"
#include "common/hash.h"
#include "common/host_clock.h"
#include "common/logging/log.h"
#include "common/process_clock.h"
#include "common/thread.h"
#include "video_core/renderer_sgxd/sgxd_shader_tier.h"
#include "video_core/renderer_sgxd/usse/fs_usse_gen.h"
#include "video_core/renderer_sgxd/usse/vs_usse_gen.h"

namespace SgxdRenderer {

ShaderTier2::ShaderTier2()
    : thread{Common::ThreadCfg{"GxmTier2"}, [this](std::stop_token stop) { Loop(stop); }} {}

ShaderTier2::~ShaderTier2() {
    for (Common::NamedThread& thread : threads) {
        thread.request_stop();
    }
    wake.RaiseAll();
    for (Common::NamedThread& thread : threads) {
        thread.join();
    }
}

void ShaderTier2::QueueFragment(u64 key, const Pica::Shader::FSConfig& config) {
    {
        std::scoped_lock lock{mutex};
        auto job = std::make_unique<Job>();
        job->key = key;
        job->fs.emplace(config);
        jobs.push_back(std::move(job));
    }
    wake.Raise();
}

void ShaderTier2::QueueVertex(u64 key, const Usse::VsRequest& request) {
    auto job = std::make_unique<Job>();
    job->key = key;
    std::copy(request.code.begin(), request.code.end(), job->code.begin());
    std::copy(request.swizzle.begin(), request.swizzle.end(), job->swizzle.begin());
    job->vs = request;
    job->vs.code = std::span<const u32>{job->code.data(), request.code.size()};
    job->vs.swizzle = std::span<const u32>{job->swizzle.data(), request.swizzle.size()};
    {
        std::scoped_lock lock{mutex};
        jobs.push_back(std::move(job));
    }
    wake.Raise();
}

std::vector<ShaderTier2::Result> ShaderTier2::Take() {
    std::scoped_lock lock{mutex};
    ready.store(false, std::memory_order_relaxed);
    return std::exchange(results, {});
}

void ShaderTier2::Loop(std::stop_token stop, Usse::Scratch scratch) {
    Common::SetCurrentThreadRole(Common::ThreadRole::ShaderCompiler);
    std::stop_callback wake_on_stop(stop, [this] { wake.RaiseAll(); });
    while (true) {
        if (!stop.stop_requested() && tier1.EmitOne(scratch)) {
            continue;
        }
        std::unique_ptr<Job> job;
        {
            std::unique_lock lock{mutex};
            // Lowered before the looks, as ShaderTier1's: a stop's RaiseAll (not under this
            // mutex) or a job after them raises again, where one between the stop's look and
            // the lowering would be lost and the join would wait for ever.
            wake.Lower();
            if (stop.stop_requested()) {
                // The two workers share the flag: this lowering may have cleared the other's
                // stop wake (it looked before the stop and waits after), so it is raised again.
                wake.RaiseAll();
                return;
            }
            if (jobs.empty()) {
                lock.unlock();
                if (!tier1.Pending()) {
                    wake.Wait();
                }
                continue;
            }
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        const u32 start = azaharCyclesStart();
        Result result{};
        result.key = job->key;
        result.vertex = !job->fs;
        if (job->fs) {
            result.program = Usse::EmitFragmentProgram(*job->fs, scratch, &result.refusal, 2);
        } else {
            result.program =
                Usse::EmitVertexProgram(job->vs, result.input_regs, scratch, &result.refusal, 2);
        }
        result.took_cycles = azaharCycles() - start;
        result.took_us = static_cast<u32>(Common::CyclesToUs(result.took_cycles));
        std::scoped_lock lock{mutex};
        results.push_back(std::move(result));
        ready.store(true, std::memory_order_release);
    }
}

ShaderTier1::ShaderTier1(ShaderDiskCache* disk_cache_)
    : disk_cache{disk_cache_},
      thread{Common::ThreadCfg{"SgxdTier1"}, [this](std::stop_token stop) { Loop(stop); }} {}

ShaderTier1::~ShaderTier1() {
    thread.request_stop();
    wake.RaiseAll();
    thread.join();
}

u64 ShaderTier1::ConfigKey(const Pica::Shader::FSConfig& config) {
    return Common::ComputeHash64(&config, sizeof(config));
}

namespace {
u64 NowUs() {
    return static_cast<u64>(Common::ProcessClock::now().time_since_epoch().count());
}
} // namespace

u32 ShaderTier1::FreeSlot() const {
    // Only the parse thread fills fragment and vertex slots, and the render thread empties a
    // slot only after its last read of it (Free's release).
    const u64 free = ~used.load(std::memory_order_acquire) & ~GeometryBit;
    return free == 0 ? Slots : static_cast<u32>(std::countr_zero(free));
}

void ShaderTier1::Publish(u32 slot) {
    const u64 bit = u64{1} << slot;
    slots[slot].queued_us = NowUs();
    slots[slot].taken = {};
    used.fetch_or(bit, std::memory_order_release);
    queued.fetch_or(bit, std::memory_order_release);
    wake.Raise();
    if (helper_wake != nullptr) {
        helper_wake->Raise();
    }
}

void ShaderTier1::QueueFragment(const Pica::Shader::FSConfig& config) {
    const u64 key = ConfigKey(config);
    if (fragments_met.contains(key)) {
        return;
    }
    const u32 slot = FreeSlot();
    if (slot == Slots) {
        return; // the render thread emits it at its draw
    }
    fragments_met.insert(key);
    Slot& s = slots[slot];
    s.key = key;
    s.kind = Kind::Fragment;
    s.fs.emplace(config);
    Publish(slot);
}

void ShaderTier1::QueueVertex(u64 key, const Usse::VsRequest& request,
                              std::shared_ptr<const VertexCode> code) {
    if (vertices_met.contains(key)) {
        return;
    }
    const u32 slot = FreeSlot();
    if (slot == Slots) {
        return;
    }
    vertices_met.insert(key);
    Slot& s = slots[slot];
    s.key = key;
    s.kind = Kind::Vertex;
    s.fs.reset();
    s.vs = request;
    s.vs_code = std::move(code);
    Publish(slot);
}

void ShaderTier1::QueueGeometry(u64 key, const Usse::VsRequest& request) {
    DEBUG_ASSERT((used.load(std::memory_order_acquire) & GeometryBit) == 0);
    GeometryJob& g = *geometry;
    std::copy(request.code.begin(), request.code.end(), g.code.begin());
    std::copy(request.swizzle.begin(), request.swizzle.end(), g.swizzle.begin());
    g.vs = request;
    g.vs.code = std::span<const u32>{g.code.data(), request.code.size()};
    g.vs.swizzle = std::span<const u32>{g.swizzle.data(), request.swizzle.size()};
    Slot& s = slots[GeometrySlot];
    s.key = key;
    s.kind = Kind::Geometry;
    s.fs.reset();
    Publish(GeometrySlot);
}

std::optional<ShaderTier1::Taken> ShaderTier1::TakeFragment(const Pica::Shader::FSConfig& config) {
    return Take(ConfigKey(config), Kind::Fragment, &config);
}

std::optional<ShaderTier1::Taken> ShaderTier1::TakeVertex(u64 key) {
    return Take(key, Kind::Vertex, nullptr);
}

std::optional<ShaderTier1::Taken> ShaderTier1::TakeGeometry(u64 key) {
    return Take(key, Kind::Geometry, nullptr);
}

void ShaderTier1::Free(u32 slot) {
    slots[slot].vs_code.reset();
    used.fetch_and(~(u64{1} << slot), std::memory_order_release);
}

std::optional<ShaderTier1::Taken> ShaderTier1::Take(u64 key, Kind kind,
                                                    const Pica::Shader::FSConfig* config) {
    // The emitted programs no draw took: their slots back for the parse thread.
    const u64 now = NowUs();
    for (u64 d = done.load(std::memory_order_acquire) & ~GeometryBit; d != 0; d &= d - 1) {
        const u32 slot = static_cast<u32>(std::countr_zero(d));
        if (now - slots[slot].queued_us > StaleUs &&
            (done.fetch_and(~(u64{1} << slot), std::memory_order_acq_rel) >> slot & 1) != 0) {
            slots[slot].taken = {};
            Free(slot);
        }
    }
    u32 slot = Slots;
    const u64 in_use = used.load(std::memory_order_acquire);
    if (kind == Kind::Geometry) {
        if ((in_use & GeometryBit) != 0 && slots[GeometrySlot].key == key) {
            slot = GeometrySlot;
        }
    } else {
        for (u64 u = in_use & ~GeometryBit; u != 0; u &= u - 1) {
            const u32 k = static_cast<u32>(std::countr_zero(u));
            if (slots[k].key == key && slots[k].kind == kind &&
                (kind != Kind::Fragment || *slots[k].fs == *config)) {
                slot = k;
                break;
            }
        }
    }
    if (slot == Slots) {
        return std::nullopt;
    }
    const u64 bit = u64{1} << slot;
    if ((queued.fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0) {
        Free(slot); // not started: the render thread's own
        return std::nullopt;
    }
    u64 waited_us = 0;
    bool helped = false;
    for (;;) {
        if ((done.fetch_and(~bit, std::memory_order_acq_rel) & bit) != 0) {
            Taken taken = std::move(slots[slot].taken);
            taken.waited_us = static_cast<u32>(waited_us);
            taken.helped = helped;
            Free(slot);
            return taken;
        }
        // A worker is emitting it: another queued job meanwhile, else wait.
        if (EmitOne(Usse::Scratch::Render)) {
            helped = true;
            continue;
        }
        emitted.Lower();
        if ((done.load(std::memory_order_acquire) & bit) != 0) {
            continue;
        }
        const u64 start = NowUs();
        emitted.Wait();
        waited_us += NowUs() - start;
    }
}

bool ShaderTier1::EmitOne(Usse::Scratch scratch) {
    for (;;) {
        const u64 q = queued.load(std::memory_order_acquire);
        if (q == 0) {
            return false;
        }
        // The geometry job first (a draw is waiting for its pair), then the oldest: the order
        // the render thread reaches their draws.
        u32 slot = GeometrySlot;
        if ((q & GeometryBit) == 0) {
            u64 oldest = ~u64{0};
            for (u64 r = q; r != 0; r &= r - 1) {
                const u32 k = static_cast<u32>(std::countr_zero(r));
                if (slots[k].queued_us < oldest) {
                    oldest = slots[k].queued_us;
                    slot = k;
                }
            }
        }
        const u64 bit = u64{1} << slot;
        if ((queued.fetch_and(~bit, std::memory_order_acq_rel) & bit) == 0) {
            continue; // another emitter claimed it
        }
        Emit(slot, scratch);
        done.fetch_or(bit, std::memory_order_release);
        emitted.Raise();
        return true;
    }
}

void ShaderTier1::Emit(u32 slot, Usse::Scratch scratch) {
    Slot& s = slots[slot];
    const u32 start = azaharCyclesStart();
    Taken taken{};
    if (s.kind != Kind::Geometry && disk_cache != nullptr) {
        if (auto cached = disk_cache->Find(s.key, s.kind == Kind::Vertex)) {
            taken.program = std::move(cached->program);
            taken.input_regs = cached->input_regs;
            taken.from_disk = true;
        }
    }
    if (!taken.from_disk) {
        switch (s.kind) {
        case Kind::Geometry:
            taken.program =
                Usse::EmitVertexProgram(geometry->vs, taken.input_regs, scratch, &taken.refusal);
            break;
        case Kind::Vertex:
            taken.program = Usse::EmitVertexProgram(s.vs, taken.input_regs, scratch, &taken.refusal);
            break;
        case Kind::Fragment:
            taken.program = Usse::EmitFragmentProgram(*s.fs, scratch, &taken.refusal);
            break;
        }
    }
    taken.took_us = static_cast<u32>(Common::CyclesToUs(azaharCycles() - start));
    s.taken = std::move(taken);
}

void ShaderTier1::Loop(std::stop_token stop) {
    Common::SetCurrentThreadRole(Common::ThreadRole::ShaderTier1);
    std::stop_callback wake_on_stop(stop, [this] { wake.RaiseAll(); });
    while (!stop.stop_requested()) {
        if (EmitOne(Usse::Scratch::Tier1)) {
            continue;
        }
        // Lowered before the look: a queue after it raises again.
        wake.Lower();
        if (queued.load(std::memory_order_acquire) == 0 && !stop.stop_requested()) {
            wake.Wait();
        }
    }
}

} // namespace SgxdRenderer
