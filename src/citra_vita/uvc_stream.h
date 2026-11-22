// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "common/common_types.h"

/**
 * One of the 3DS's screens as a USB webcam (azaharnative's uvc.c), for a PC to show
 * (Config::Option::UsbVideo); the Vita keeps its own layout. The stream is 400x240 YUY2 at up to 60
 * frames a second whichever screen it carries, the bottom screen pillarboxed, so switching screens
 * does not make the host enumerate the device again. A thread of its own on the miscellaneous core
 * copies each frame out of guest memory and converts it, only while a host is watching.
 */
namespace VitaFrontend::UvcStream {

inline constexpr u32 Width = 400;
inline constexpr u32 Height = 240;

/// The screen that goes out: 0 none, 1 the top, 2 the bottom. The first other than none takes the
/// USB port for the rest of the session, from another plugin's driver only once the player has
/// agreed (AllowTakeover): that plugin then stops working until a reboot.
void SetScreen(int screen);
[[nodiscard]] int Screen();

/// Whether another plugin has the USB port now (a screen streamer such as vitastream), which the
/// camera would take from it.
[[nodiscard]] bool PortTaken();
/// The player agreed to take the port from it, for the session; a start it held back is tried
/// again at the next frame.
void AllowTakeover();
[[nodiscard]] bool TakeoverAllowed();

/// Why the camera did not start, as what to do about it; null while it has not been tried or
/// started.
[[nodiscard]] const char* Problem();

/// A guest screen as EmuWindowVita::DrawGuestScreens has it: `columns` columns of 240 pixels,
/// `stride` bytes apart, in PICA format `format`, at `pixels`. The streamed one, while a host
/// watches, goes to the stream's thread; the others are ignored.
void Offer(u32 index, const u8* pixels, u32 stride, u32 columns, u32 format);

} // namespace VitaFrontend::UvcStream
