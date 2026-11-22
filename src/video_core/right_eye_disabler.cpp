// Copyright 2024 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/settings.h"
#include "right_eye_disabler.h"
#include "video_core/gpu.h"
#include "video_core/gpu_impl.h"

namespace VideoCore {

bool RightEyeDisabler::ShouldAllowCmdQueueTrigger(PAddr addr, u32 size) {
    if (!enabled || !enable_for_frame)
        return true;

    constexpr u32 top_screen_size = 0x00469000;

    if (report_end_frame_pending) {
        ReportEndFrame();
        report_end_frame_pending = false;
    }
    cmd_queue_trigger_happened = true;
    list_after_top_transfer = true;

    auto guess = gpu.impl->pica.GuessCmdRenderProperties(addr, size);
#ifdef VITA_DIAGNOSTICS
    if (EyeLog()) {
        LOG_INFO(HW_GPU,
                 "eyelog: list {:#x} ({:#x} bytes): viewport height {:#x}, colour {:#x}; top "
                 "{:#x} drawn {} transferred {} blocked {} both eyes {}",
                 addr, size, guess.vp_height, guess.paddr, top_screen_buf, top_screen_drawn,
                 top_screen_transfered, top_screen_blocked, both_eyes_transfered);
    }
#endif
    // A list that issues no geometry makes no picture: never the right eye's (upstream
    // c3204e25f). Majora's Mask 3D's intro sends state-only lists after its bloom; one taken as
    // a right eye blocked the top screen's transfer after it, and the screen kept the white of
    // the fade before the scene. Whether it draws is CmdListMayDraw's answer, as the render
    // thread's: upstream's flag from the guess's own scan, which stops at its registers and
    // after 256 commands and counts no immediate-mode vertex, found no draw in any of a game's
    // lists, and no right eye was left out (its race 195% -> 170%). Asked last, of a candidate
    // only.
    if (guess.vp_height == top_screen_size && !top_screen_blocked && !both_eyes_transfered &&
        gpu.impl->pica.CmdListMayDraw(addr, size)) {
        if (top_screen_buf == 0) {
            top_screen_buf = guess.paddr;
        }
        top_screen_drawn = true;
        if (top_screen_transfered && !bottom_after_top) {
            cmd_trigger_blocked = true;
#ifdef VITA_DIAGNOSTICS
            counts.lists++;
#endif
            return false;
        }
    }

    cmd_trigger_blocked = false;
    return true;
}
bool RightEyeDisabler::ShouldAllowDisplayTransfer(PAddr src_address, size_t size,
                                                  u32 output_rows, bool to_right_eye) {
    if (!enabled || !enable_for_frame)
        return true;

    // The right eye drawn first: the next list is the left eye's.
    if (size >= 400 && !top_screen_blocked && to_right_eye && !top_screen_transfered &&
        top_screen_drawn && src_address == top_screen_buf) {
        if (cmd_queue_trigger_happened)
            display_tranfer_happened = true;
        return true;
    }

    if (size >= 400 && !top_screen_blocked) {
        if (top_screen_drawn && src_address == top_screen_buf) {
            // A second transfer of the picture with no list since the first gives the right eye the
            // left eye's picture; a list after it may draw the bottom screen into the same buffer
            // with the top screen's viewport, which is no right eye.
            if (top_screen_transfered && !list_after_top_transfer) {
#ifdef VITA_DIAGNOSTICS
                counts.both_eyes += both_eyes_transfered ? 0 : 1;
#endif
                both_eyes_transfered = true;
            }
            // The picture drawn after the top screen's transfer goes out: the right eye's at the
            // top screen's 400 rows, else the bottom screen's. Hyrule Warriors Legends draws
            // its bottom screen there with the top screen's viewport each frame, and transfers
            // the buffer's 400 rows to 320. The lists after the top screen's transfer are left
            // out only while the last such picture was a right eye. A frame's first only: with
            // no frame end between two frames, the next top screen's comes after it.
            if (top_screen_transfered && list_after_top_transfer && !second_picture_out) {
                bottom_after_top = output_rows < 400;
                second_picture_out = true;
            }
            top_screen_transfered = true;
            list_after_top_transfer = false;
        }

        if (src_address == top_screen_buf && cmd_trigger_blocked) {
            top_screen_blocked = true;
#ifdef VITA_DIAGNOSTICS
            counts.transfers++;
#endif
            return false;
        }
    }

    if (cmd_queue_trigger_happened)
        display_tranfer_happened = true;
    return true;
}
bool RightEyeDisabler::Allowed() const {
    return enabled && Settings::values.disable_right_eye_render.GetValue();
}

void RightEyeDisabler::ReportEndFrame() {
    if (!enabled)
        return;

    enable_for_frame = Settings::values.disable_right_eye_render.GetValue();

    if (display_tranfer_happened) {
        top_screen_drawn = false;
        top_screen_transfered = false;
        top_screen_blocked = false;
        cmd_queue_trigger_happened = false;
        cmd_trigger_blocked = false;
        display_tranfer_happened = false;
        top_screen_buf = 0;
        list_after_top_transfer = false;
        both_eyes_transfered = false;
        second_picture_out = false;
    } else {
        report_end_frame_pending = true;
    }
}
} // namespace VideoCore
