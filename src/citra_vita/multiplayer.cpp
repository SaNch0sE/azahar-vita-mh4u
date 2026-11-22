// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>

#include <psp2/kernel/clib.h>

#include "citra_vita/i18n.h"
#include "citra_vita/multiplayer.h"
#include "common/named_thread.h"
#include "core/core.h"
#include "core/hle/service/cfg/cfg.h"
#include "network/network.h"
#include "network/room.h"
#include "network/room_member.h"

namespace VitaFrontend::Multiplayer {

namespace {

using State = Network::RoomMember::State;
using Error = Network::RoomMember::Error;

constexpr char FieldsPath[] = "ux0:data/azahar/multiplayer.txt";

Fields fields{"", Network::DefaultRoomPort, "Vita", ""};

/// What the room last sent, and the last error: written by the member's loop thread (its
/// callbacks), read by the menu and the command channel.
std::mutex mutex;
std::string room_name;
u32 room_slots = 0;
std::vector<std::string> members;
std::string last_error;
/// The address and port of the join under way (an ad hoc join's are not the saved fields).
std::string joining;

Network::RoomMember::CallbackHandle<State> on_state;
Network::RoomMember::CallbackHandle<Error> on_error;
Network::RoomMember::CallbackHandle<Network::RoomInformation> on_room;

/// The join under way (RoomMember::Join waits for the server).
Common::NamedThread join_thread;

const char* ErrorText(Error error) {
    using I18n::Translatable;
    switch (error) {
    case Error::LostConnection:
        return Translatable("the connection to the room was lost");
    case Error::HostKicked:
        return Translatable("kicked by the room's host");
    case Error::UnknownError:
        return Translatable("the network failed");
    case Error::NameCollision:
        return Translatable("the nickname is taken in that room");
    case Error::MacCollision:
        return Translatable("the MAC address is taken in that room");
    case Error::ConsoleIdCollision:
        return Translatable("a console with the same ID is in that room");
    case Error::WrongVersion:
        return Translatable("the room runs another version of the protocol");
    case Error::WrongPassword:
        return Translatable("wrong password");
    case Error::CouldNotConnect:
        return Translatable("the room did not answer");
    case Error::RoomIsFull:
        return Translatable("the room is full");
    case Error::HostBanned:
        return Translatable("banned from that room");
    case Error::PermissionDenied:
        return Translatable("permission denied");
    case Error::NoSuchUser:
        return Translatable("no such user");
    }
    return Translatable("an unknown error");
}

const char* StateText(State state) {
    switch (state) {
    case State::Uninitialized:
    case State::Idle:
        return "not in a room";
    case State::Joining:
        return "joining";
    case State::Joined:
        return "joined";
    case State::Moderator:
        return "joined as moderator";
    }
    return "?";
}

void LoadFields() {
    FILE* f = std::fopen(FieldsPath, "r");
    if (f == nullptr) {
        return;
    }
    char line[128];
    for (int i = 0; i < 3 && std::fgets(line, sizeof(line), f) != nullptr; i++) {
        std::string value = line;
        value.erase(value.find_last_not_of("\r\n") + 1);
        if (i == 0) {
            fields.address = value;
        } else if (i == 1) {
            const int port = std::atoi(value.c_str());
            fields.port = port > 0 && port <= 65535 ? static_cast<u16>(port)
                                                    : Network::DefaultRoomPort;
        } else if (!value.empty()) {
            fields.nickname = value;
        }
    }
    std::fclose(f);
}

void SaveFields() {
    if (FILE* f = std::fopen(FieldsPath, "w")) {
        std::fprintf(f, "%s\n%u\n%s\n", fields.address.c_str(), static_cast<unsigned>(fields.port),
                     fields.nickname.c_str());
        std::fclose(f);
    }
}

} // Anonymous namespace

void Init() {
    LoadFields();
    if (!Network::Init()) {
        sceClibPrintf("[azahar] room: the network library did not start\n");
        return;
    }
    const auto member = Network::GetRoomMember().lock();
    on_state = member->BindOnStateChanged([](const State& state) {
        sceClibPrintf("[azahar] room: %s\n", StateText(state));
        if (state == State::Idle) {
            std::scoped_lock lk{mutex};
            members.clear();
            room_name.clear();
        }
    });
    on_error = member->BindOnError([](const Error& error) {
        sceClibPrintf("[azahar] room: %s\n", ErrorText(error));
        std::scoped_lock lk{mutex};
        last_error = ErrorText(error);
    });
    // The member list comes with the room's information (RoomMember's loop thread updates both,
    // then calls this): copied here, where nothing changes it meanwhile.
    on_room = member->BindOnRoomInformationChanged(
        [](const Network::RoomInformation& info) {
            std::vector<std::string> list;
            if (const auto m = Network::GetRoomMember().lock()) {
                for (const auto& entry : m->GetMemberInformation()) {
                    list.push_back(entry.game_info.name.empty()
                                       ? entry.nickname
                                       : entry.nickname + " - " + entry.game_info.name);
                }
            }
            std::scoped_lock lk{mutex};
            room_name = info.name;
            room_slots = info.member_slots;
            members = std::move(list);
        });
}

void Shutdown() {
    if (join_thread.joinable()) {
        join_thread.join();
    }
    if (const auto member = Network::GetRoomMember().lock()) {
        if (member->IsConnected()) {
            member->Leave();
        }
        member->Unbind(on_state);
        member->Unbind(on_error);
        member->Unbind(on_room);
    }
    Network::Shutdown();
}

Fields& CurrentFields() {
    return fields;
}

void Join(Core::System& system, const Fields& wanted, bool keep) {
    if (keep) {
        fields = wanted;
        SaveFields();
    }
    const std::string console_id = Service::CFG::GetConsoleIdHash(system);
    const Network::MacAddress mac = Service::CFG::GetConsoleMacAddress(system);
    if (join_thread.joinable()) {
        join_thread.join();
    }
    {
        std::scoped_lock lk{mutex};
        last_error.clear();
        joining = wanted.address + ":" + std::to_string(wanted.port);
    }
    join_thread = Common::NamedThread{Common::ThreadCfg{"room join", 128 * 1024},
                                      [wanted, console_id, mac] {
                                          const auto member = Network::GetRoomMember().lock();
                                          if (!member) {
                                              return;
                                          }
                                          if (member->IsConnected()) {
                                              member->Leave();
                                          }
                                          member->Join(wanted.nickname, console_id,
                                                       wanted.address.c_str(), wanted.port, 0, mac,
                                                       wanted.password);
                                      }};
}

void Leave() {
    if (join_thread.joinable()) {
        join_thread.join();
    }
    if (const auto member = Network::GetRoomMember().lock(); member && member->IsConnected()) {
        member->Leave();
    }
}

std::string Status() {
    const auto member = Network::GetRoomMember().lock();
    if (!member) {
        return I18n::Tr("Multiplayer is not available");
    }
    std::scoped_lock lk{mutex};
    switch (member->GetState()) {
    case State::Joining:
        return I18n::TrFormat("Joining {}...", joining);
    case State::Joined:
    case State::Moderator:
        return I18n::TrFormat("In room \"{}\" ({} of {})", room_name, members.size(), room_slots);
    default:
        return last_error.empty()
                   ? std::string{I18n::Tr("Not in a room")}
                   : I18n::TrFormat("Not in a room: {}", I18n::Tr(last_error.c_str()));
    }
}

bool InRoom() {
    const auto member = Network::GetRoomMember().lock();
    return member && member->IsConnected();
}

std::vector<std::string> Members() {
    std::scoped_lock lk{mutex};
    return members;
}

bool RunCommand(Core::System& system, const std::string& command) {
    std::istringstream in(command);
    std::string word, verb;
    in >> word >> verb;
    if (word != "room") {
        return false;
    }
    if (verb == "join") {
        Fields wanted = fields;
        int port = 0;
        in >> wanted.address >> port >> wanted.nickname >> wanted.password;
        if (wanted.address.empty() || port <= 0 || port > 65535 || wanted.nickname.empty()) {
            sceClibPrintf("[azahar] room: join wants HOST PORT NICK [PASSWORD] (command)\n");
        } else if (!system.IsPoweredOn()) {
            sceClibPrintf("[azahar] room: join needs a running title (command)\n");
        } else {
            wanted.port = static_cast<u16>(port);
            sceClibPrintf("[azahar] room: joining %s:%d as %s (command)\n",
                          wanted.address.c_str(), port, wanted.nickname.c_str());
            Join(system, wanted);
        }
    } else if (verb == "leave") {
        Leave();
        sceClibPrintf("[azahar] room: left (command)\n");
    } else {
        sceClibPrintf("[azahar] room: %s (command)\n", Status().c_str());
        for (const std::string& line : Members()) {
            sceClibPrintf("[azahar] room:   %s\n", line.c_str());
        }
    }
    return true;
}

} // namespace VitaFrontend::Multiplayer
