// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

namespace Core {
class System;
}

/**
 * Multiplayer rooms: the guest's local wireless (nwm::UDS) carried to the other emulators in a
 * room on a room server (a desktop Azahar's hosted room, or the standalone server). Joined by
 * address, port, nickname and password; there is no public room list (Azahar runs no lobby).
 * The membership outlives the titles: a game started later plays in the same room. Joining needs
 * a running title (the console's ID and MAC address come from its configuration).
 */
namespace VitaFrontend::Multiplayer {

/// What the pause menu's Multiplayer screen edits, kept on the memory card
/// (ux0:data/azahar/multiplayer.txt: address, port and nickname; the password is not kept).
struct Fields {
    std::string address;
    u16 port;
    std::string nickname;
    std::string password;
};

/// The room member (Network::Init) and the fields saved last time. Once, at start.
void Init();
/// The room left, the member gone. At exit.
void Shutdown();

/// The fields, as last joined with (or loaded at start).
Fields& CurrentFields();

/// Joins the room in `fields` on a thread of its own (a join waits up to five seconds for the
/// server), leaving the current one first, and with `keep` makes them the current fields and
/// saves them (an ad hoc room's are not kept). A running title's thread: the console's ID hash
/// and MAC address are read from its configuration here.
void Join(Core::System& system, const Fields& fields, bool keep = true);
void Leave();

/// One line: not in a room, joining, in a room (its name and how many of how many), or the
/// last error.
std::string Status();
/// Whether the member is in a room or joining one.
bool InRoom();
/// The room's members, each "nickname - game" (as the room last sent them).
std::vector<std::string> Members();

#ifdef VITA_DEBUG_TOOLS
/// The command channel's `room join HOST PORT NICK [PASSWORD]`, `room leave` and `room`
/// (the status and the members): whether `command` was one of them.
bool RunCommand(Core::System& system, const std::string& command);
#endif

} // namespace VitaFrontend::Multiplayer
