// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>

#include "common/common_types.h"

namespace Core {
class System;
}

/**
 * Multiplayer over the Vita's ad hoc network, with no access point and no room server elsewhere:
 * one Vita hosts a room on its address there, the others find it by its announcement and join
 * it, and the guest's local wireless then runs through that room as through any other.
 *
 * The network is the IBSS that Vita titles share for local play, joined as the netcheck dialog
 * joins it but without the dialog (which draws with Sony's GPU driver, and sgxd has the GPU):
 * an NP communication ID registered (the shell refuses an ad hoc connection without one), the
 * shell events the dialog raises, and SceShell's network worker asked to join the network of
 * the dialog's SSID or to start it.
 */
namespace VitaFrontend::Adhoc {

/// Joins the ad hoc network, opens a room on this console's address there, announces it, and
/// joins it as `nickname`. On a thread of its own (joining the network takes a few seconds); a
/// running title's (the member's console ID and MAC address come from its configuration).
void Host(Core::System& system, const std::string& nickname);
/// Joins the ad hoc network and then the first room announced on it, as `nickname`.
void Join(Core::System& system, const std::string& nickname);
/// Leaves the room, closes a hosted one, and leaves the ad hoc network.
void Leave();

/// Whether this console is on the ad hoc network, or on its way there.
bool Active();
/// One line: not on the ad hoc network, connecting, looking for a room, hosting or joined, or
/// the last error.
std::string Status();

/// The command channel's `adhoc host [NICK]`, `adhoc join [NICK]`, `adhoc leave` and `adhoc`
/// (the status): whether `command` was one of them.
bool RunCommand(Core::System& system, const std::string& command);

} // namespace VitaFrontend::Adhoc
