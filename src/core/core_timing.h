// Copyright 2008 Dolphin Emulator Project / 2017 Citra Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

/**
 * This is a system to schedule events into the emulated machine's future. Time is measured
 * in main CPU clock cycles.
 *
 * To schedule an event, you first have to register its type. This is where you pass in the
 * callback. You then schedule events using the type id you get back.
 *
 * The int cyclesLate that the callbacks get is how many cycles late it was.
 * So to schedule a new event on a regular basis:
 * inside callback:
 *   ScheduleEvent(periodInCycles - cyclesLate, callback, "whatever")
 */

#include <chrono>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>
#include <boost/serialization/split_member.hpp>
#include <boost/serialization/vector.hpp>
#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/threadsafe_queue.h"
#include "core/global.h"

// The timing we get from the assembly is 268,111,855.956 Hz
// It is possible that this number isn't just an integer because the compiler could have
// optimized the multiplication by a multiply-by-constant division.
// Rounding to the nearest integer should be fine
constexpr u64 BASE_CLOCK_RATE_ARM11 = 268111856;
constexpr u64 MAX_VALUE_TO_MULTIPLY = std::numeric_limits<s64>::max() / BASE_CLOCK_RATE_ARM11;

/// Refresh rate defined by ratio of ARM11 frequency to ARM11 ticks per frame
/// (268,111,856) / (4,481,136) = 59.83122493939037Hz
constexpr double SCREEN_REFRESH_RATE = BASE_CLOCK_RATE_ARM11 / static_cast<double>(4481136ull);

constexpr s64 msToCycles(int ms) {
    // since ms is int there is no way to overflow
    return BASE_CLOCK_RATE_ARM11 * static_cast<s64>(ms) / 1000;
}

constexpr s64 msToCycles(float ms) {
    return static_cast<s64>(BASE_CLOCK_RATE_ARM11 * (0.001f) * ms);
}

constexpr s64 msToCycles(double ms) {
    return static_cast<s64>(BASE_CLOCK_RATE_ARM11 * (0.001) * ms);
}

constexpr s64 usToCycles(float us) {
    return static_cast<s64>(BASE_CLOCK_RATE_ARM11 * (0.000001f) * us);
}

constexpr s64 usToCycles(int us) {
    return (BASE_CLOCK_RATE_ARM11 * static_cast<s64>(us) / 1000000);
}

inline s64 usToCycles(s64 us) {
    if (us / 1000000 > static_cast<s64>(MAX_VALUE_TO_MULTIPLY)) {
        LOG_ERROR(Core_Timing, "Integer overflow, use max value");
        return std::numeric_limits<s64>::max();
    }
    if (us > static_cast<s64>(MAX_VALUE_TO_MULTIPLY)) {
        LOG_DEBUG(Core_Timing, "Time very big, do rounding");
        return BASE_CLOCK_RATE_ARM11 * (us / 1000000);
    }
    return (BASE_CLOCK_RATE_ARM11 * us) / 1000000;
}

inline s64 usToCycles(u64 us) {
    if (us / 1000000 > MAX_VALUE_TO_MULTIPLY) {
        LOG_ERROR(Core_Timing, "Integer overflow, use max value");
        return std::numeric_limits<s64>::max();
    }
    if (us > MAX_VALUE_TO_MULTIPLY) {
        LOG_DEBUG(Core_Timing, "Time very big, do rounding");
        return BASE_CLOCK_RATE_ARM11 * static_cast<s64>(us / 1000000);
    }
    return (BASE_CLOCK_RATE_ARM11 * static_cast<s64>(us)) / 1000000;
}

constexpr s64 nsToCycles(float ns) {
    return static_cast<s64>(BASE_CLOCK_RATE_ARM11 * (0.000000001f) * ns);
}

constexpr s64 nsToCycles(int ns) {
    return BASE_CLOCK_RATE_ARM11 * static_cast<s64>(ns) / 1000000000;
}

/// nsToCycles for 0 <= ns < 2^32 (4.29 s) without a division: a 64-bit one is a library call on
/// ARM32 (__aeabi_ldivmod), and each HLE SleepThread converts its delay. The 32.32 reciprocal
/// comes out at most one short of BASE_CLOCK_RATE_ARM11 * ns / 1e9, which the remainder settles:
/// the same cycles as the division.
constexpr s64 nsToCyclesBelow2p32(u64 ns) {
    constexpr u64 CyclesPerNsQ32 = (BASE_CLOCK_RATE_ARM11 << 32) / 1000000000;
    u64 cycles = (ns * CyclesPerNsQ32) >> 32;
    if (BASE_CLOCK_RATE_ARM11 * ns - cycles * 1000000000 >= 1000000000) {
        cycles++;
    }
    return static_cast<s64>(cycles);
}
static_assert(nsToCyclesBelow2p32(999'999'999) == BASE_CLOCK_RATE_ARM11 * 999'999'999 / 1000000000);
static_assert(nsToCyclesBelow2p32(0xFFFFFFFF) ==
              BASE_CLOCK_RATE_ARM11 * 0xFFFFFFFFull / 1000000000);

inline s64 nsToCycles(s64 ns) {
    if (ns >= 0 && ns <= 0xFFFFFFFF) {
        return nsToCyclesBelow2p32(static_cast<u64>(ns));
    }
    if (ns / 1000000000 > static_cast<s64>(MAX_VALUE_TO_MULTIPLY)) {
        LOG_ERROR(Core_Timing, "Integer overflow, use max value");
        return std::numeric_limits<s64>::max();
    }
    if (ns > static_cast<s64>(MAX_VALUE_TO_MULTIPLY)) {
        LOG_DEBUG(Core_Timing, "Time very big, do rounding");
        return BASE_CLOCK_RATE_ARM11 * (ns / 1000000000);
    }
    return (BASE_CLOCK_RATE_ARM11 * ns) / 1000000000;
}

inline s64 nsToCycles(u64 ns) {
    if (ns <= 0xFFFFFFFF) {
        return nsToCyclesBelow2p32(ns);
    }
    if (ns / 1000000000 > MAX_VALUE_TO_MULTIPLY) {
        LOG_ERROR(Core_Timing, "Integer overflow, use max value");
        return std::numeric_limits<s64>::max();
    }
    if (ns > MAX_VALUE_TO_MULTIPLY) {
        LOG_DEBUG(Core_Timing, "Time very big, do rounding");
        return BASE_CLOCK_RATE_ARM11 * (static_cast<s64>(ns) / 1000000000);
    }
    return (BASE_CLOCK_RATE_ARM11 * static_cast<s64>(ns)) / 1000000000;
}

constexpr u64 cyclesToNs(s64 cycles) {
    return cycles * 1000000000 / BASE_CLOCK_RATE_ARM11;
}

constexpr s64 cyclesToUs(s64 cycles) {
    return cycles * 1000000 / BASE_CLOCK_RATE_ARM11;
}

constexpr u64 cyclesToMs(s64 cycles) {
    return cycles * 1000 / BASE_CLOCK_RATE_ARM11;
}

namespace Core {

using TimedCallback = std::function<void(std::uintptr_t user_data, int cycles_late)>;

struct TimingEventType {
    TimedCallback callback;
    const std::string* name;
};

class Timing {

public:
    struct Event {
        s64 time;
        u64 fifo_order;
        std::uintptr_t user_data;
        const TimingEventType* type;
        /**
         * When set, `time` is a delay from whenever the owning timer next picks the event up
         * rather than an absolute tick.
         *
         * Only the owning thread can read a timer's tick count safely, so a thread scheduling an
         * event from elsewhere would need a fixed floor of MAX_SLICE_LENGTH * 2 cycles - 8.55 ms
         * of emulated time - purely so its absolute time could not land in the past. That floor
         * is paid by whatever the event represents, which for the software renderer's
         * completion interrupts is more than half a 60 Hz frame on every single one. Deferring
         * the conversion to MoveEvents(), which runs on the owning thread, needs no floor.
         */
        bool relative = false;

        bool operator>(const Event& right) const;
        bool operator<(const Event& right) const;

    private:
        template <class Archive>
        void save(Archive& ar, const unsigned int) const {
            ar & time;
            ar & fifo_order;
            ar & user_data;
            std::string name = *(type->name);
            ar << name;
        }

        template <class Archive>
        void load(Archive& ar, const unsigned int) {
            ar & time;
            ar & fifo_order;
            ar & user_data;
            std::string name;
            ar >> name;
            type = Global<Timing>().RegisterEvent(name, nullptr);
        }
        friend class boost::serialization::access;

        BOOST_SERIALIZATION_SPLIT_MEMBER()
    };

    // currently Service::HID::pad_update_ticks is the smallest interval for an event that gets
    // always scheduled. Therfore we use this as orientation for the MAX_SLICE_LENGTH
    // For performance bigger slice length are desired, though this will lead to cores desync
    // But we never want to schedule events into the current slice, because then cores might to
    // run small slices to sync up again. This is especially important for events that are always
    // scheduled and repated.
    static constexpr int MAX_SLICE_LENGTH = BASE_CLOCK_RATE_ARM11 / 234;

    class Timer {
    public:
        Timer(s64 base_ticks = 0);
        ~Timer();

        s64 GetMaxSliceLength() const;
        /// The type of the event GetMaxSliceLength runs up to, or null with none queued.
        const TimingEventType* NextEventType() const;

        void Advance();

        void SetNextSlice(s64 max_slice_length = MAX_SLICE_LENGTH);

        void Idle();

        u64 GetTicks() const;
        u64 GetIdleTicks() const;

        void AddTicks(u64 ticks);
        /// Advances GetTicks by exactly `ticks`, the clock scale already applied: for the Vita's
        /// native core, which times the guest on a host timer and has to land where the guest's
        /// own reads of the clock (answered in its kernel module) left it.
        void AddScaledTicks(u64 ticks);
        double GetClockScale() const {
            return cpu_clock_scale;
        }

        s64 GetDowncount() const;

        void ForceExceptionCheck(s64 cycles);

        void MoveEvents();

        /// The tick an event of `type` for `user_data` is due at, or -1 (queued events only:
        /// MoveEvents first).
        [[nodiscard]] s64 EventTime(const TimingEventType* type, std::uintptr_t user_data) const;
        /// How many events of `type` this timer's queue holds.
        [[nodiscard]] std::size_t EventCount(const TimingEventType* type) const;

    private:
        friend class Timing;
        // The queue is a min-heap using std::make_heap/push_heap/pop_heap.
        // We don't use std::priority_queue because we need to be able to serialize, unserialize and
        // erase arbitrary events (RemoveEvent()) regardless of the queue order. These aren't
        // accommodated by the standard adaptor class.
        std::vector<Event> event_queue;
        u64 event_fifo_id = 0;
        // the queue for storing the events from other threads threadsafe until they will be added
        // to the event_queue by the emu thread
        Common::MPSCQueue<Event> ts_queue;
        // Are we in a function that has been called from Advance()
        // If events are sheduled from a function that gets called from Advance(),
        // don't change slice_length and downcount.
        // The time between CoreTiming being intialized and the first call to Advance() is
        // considered the slice boundary between slice -1 and slice 0. Dispatcher loops must call
        // Advance() before executing the first cycle of each slice to prepare the slice length and
        // downcount for that slice.
        bool is_timer_sane = true;

        s64 slice_length = MAX_SLICE_LENGTH;
        s64 downcount = MAX_SLICE_LENGTH;
        s64 executed_ticks = 0;
        u64 idled_cycles = 0;

        // Stores a scaling for the internal clockspeed. Changing this number results in
        // under/overclocking the guest cpu
        double cpu_clock_scale = 1.0;

        template <class Archive>
        void serialize(Archive& ar, const unsigned int) {
            MoveEvents();
            ar & event_queue;
            ar & event_fifo_id;
            ar & slice_length;
            ar & downcount;
            ar & executed_ticks;
            ar & idled_cycles;
            if (Archive::is_loading::value) {
                // Saved between slices, mid-slice: its time includes the slice's run part
                // (GetTicks). A new timer counts as sane, which left that part out until the
                // next Advance, and RunLoop's catch-up of the cores counted it twice. The second
                // emulation thread's idle core 1 is caught up in one slice of everything core 0
                // ran (Chibi-Robo's ~80e9 ticks), so after a load core 1 was that far ahead: the
                // speed read 0% (the most advanced timer's) until core 0 had run it again.
                is_timer_sane = false;
            }
        }
        friend class boost::serialization::access;
    };

    explicit Timing(std::size_t num_cores, u32 cpu_clock_percentage, s64 override_base_ticks = -1);

    ~Timing() {};

    /**
     * Returns the event_type identifier. if name is not unique, it will assert.
     */
    TimingEventType* RegisterEvent(const std::string& name, TimedCallback callback);

    // Make sure to use thread_safe_mode = true if called from a different thread than the
    // emulator thread, such as coroutines.
    void ScheduleEvent(s64 cycles_into_future, const TimingEventType* event_type,
                       std::uintptr_t user_data = 0,
                       std::size_t core_id = std::numeric_limits<std::size_t>::max(),
                       bool thread_safe_mode = false);

    void UnscheduleEvent(const TimingEventType* event_type, std::uintptr_t user_data);

    /// We only permit one event of each type in the queue at a time.
    void RemoveEvent(const TimingEventType* event_type);

    void SetCurrentTimer(std::size_t core_id);

    s64 GetTicks() const;

    s64 GetGlobalTicks() const;

    /**
     * Updates the value of the cpu clock scaling to the new percentage.
     */
    void UpdateClockSpeed(u32 cpu_clock_percentage);

    std::chrono::microseconds GetGlobalTimeUs() const;

    std::shared_ptr<Timer> GetTimer(std::size_t cpu_id);

    // Used after deserializing to unprotect the event queue.
    void UnlockEventQueue() {
        event_queue_locked = false;
    }

    /// Generates a random tick count to seed the system tick timer with.
    static s64 GenerateBaseTicks();

private:
    // unordered_map stores each element separately as a linked list node so pointers to
    // elements remain stable regardless of rehashes/resizing.
    std::unordered_map<std::string, TimingEventType> event_types = {};

    std::vector<std::shared_ptr<Timer>> timers;
    Timer* current_timer = nullptr;
    /// The CPU clock in percent (UpdateClockSpeed): a state's timers are new ones, at 100% until
    /// it is applied to them again.
    u32 clock_percentage = 100;

    // When true, the event queue can't be modified. Used while deserializing to workaround
    // destructor side effects.
    bool event_queue_locked = false;

    template <class Archive>
    void serialize(Archive& ar, const unsigned int file_version) {
        // event_types set during initialization of other things
        ar & timers;
        ar & current_timer;
        if (Archive::is_loading::value) {
            event_queue_locked = true;
            UpdateClockSpeed(clock_percentage);
        }
    }
    friend class boost::serialization::access;
};

} // namespace Core

BOOST_CLASS_VERSION(Core::Timing, 1)
