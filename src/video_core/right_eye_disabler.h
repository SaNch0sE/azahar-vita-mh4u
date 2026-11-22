// Copyright 2024 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

namespace VideoCore {
class GPU;

class RightEyeDisabler {
public:
    RightEyeDisabler(GPU& gpu) : gpu{gpu} {}

    /// The top screen's viewport height (the register's value): a list drawn with it is a
    /// top-screen picture, the left eye's or the right eye's.
    static constexpr u32 TopScreenHeight = 0x00469000;

    /// Before a list: whether it is left out if its first draw is a top-screen picture (the
    /// right eye's). The scan stops there and leaves it out (RenderThread::SubmitCmdList).
    bool BeforeCmdList();
    /// After the list's scan: whether it drew, and its first draw's viewport height (the
    /// register's value) and colour buffer.
    void AfterCmdList(bool drew, u32 vp_height, PAddr paddr);
    /// `size` the transfer's input rows, `output_rows` its output's.
    bool ShouldAllowDisplayTransfer(PAddr src_address, size_t size, u32 output_rows,
                                    bool to_right_eye);

    void ReportEndFrame();

    void SetEnabled(bool enable) {
        enabled = enable;
    }

    /// Whether the right eye may be left undrawn: the setting on, and the title not one the hack
    /// list disallows (GSP_GPU sets `enabled` per title). The sgxd render thread asks it for
    /// titles whose eyes are both drawn before either is transferred.
    [[nodiscard]] bool Allowed() const;

#ifdef VITA_DIAGNOSTICS
    /// Since the last call (`perflog`): the lists left out, the transfers left out, and the
    /// frames whose top picture went to both eyes (both_eyes_transfered).
    struct Counts {
        u32 lists, transfers, both_eyes;
    };
    Counts TakeCounts() {
        const Counts c = counts;
        counts = {};
        return c;
    }
#endif

private:
    bool enabled = true;
    bool enable_for_frame = true;

    bool top_screen_drawn = false;
    bool top_screen_transfered = false;
    bool top_screen_blocked = false;
    bool cmd_trigger_blocked = false;
    PAddr top_screen_buf = 0;
    /// A list came after the top screen's last transfer.
    bool list_after_top_transfer = false;
    /// The top screen's picture went out twice with no list between: both eyes have it, and no
    /// list after is the right eye's.
    bool both_eyes_transfered = false;
    /// The last picture drawn after the top screen's transfer went out as the bottom screen,
    /// not as a right eye: the lists after the top screen's transfer are kept. Not a frame's.
    bool bottom_after_top = false;
    /// This frame's picture after the top screen's transfer went out (bottom_after_top is set).
    bool second_picture_out = false;

    /// BeforeCmdList ran for the list: AfterCmdList takes its first draw.
    bool after_list = false;
    /// The list is left out if its first draw is a top-screen picture.
    bool may_leave_out = false;

    bool cmd_queue_trigger_happened = false;
    bool display_tranfer_happened = false;
    bool report_end_frame_pending = false;

    GPU& gpu;
#ifdef VITA_DIAGNOSTICS
    Counts counts{};
#endif
};
} // namespace VideoCore