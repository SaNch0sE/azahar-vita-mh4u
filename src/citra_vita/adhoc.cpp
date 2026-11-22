// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <sstream>

#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/np/common.h>
#include <psp2/sysmodule.h>

#include "citra_vita/adhoc.h"
#include "citra_vita/i18n.h"
#include "citra_vita/multiplayer.h"
#include "common/logging/log.h"
#include "common/named_thread.h"
#include "core/core.h"
#include "network/network.h"
#include "network/room.h"
#include "network/verify_user.h"

extern "C" {
// SceNetCtl's unnamed ad hoc exports (netctl_adhoc.yml).
int sceNetCtlAdhocConnectSsid(const char* ssid);
int sceNetCtlAdhocShellEventA(int which);
int sceNetCtlAdhocShellEventB(int which);

struct SceNpCommunicationConfig {
    const SceNpCommunicationId* comm_id;
    const void* passphrase;
    const void* signature;
};
int sceNpInit(const SceNpCommunicationConfig* config, void* option);
int sceNpTerm(void);
}

namespace VitaFrontend::Adhoc {

namespace {

/// The SSID the netcheck dialog joins in its Vita ad hoc mode: every Vita title's ad hoc play
/// shares the one network, and titles find each other above it.
constexpr char Ssid[] = "SCE_PSP2_IBSS";
/// The NP communication ID registered for the connection: the shell wants one, and takes ours
/// as it takes the SDK samples'.
constexpr char CommId[] = "AZHR00001";
/// SceNetCtl's ad hoc state once the console has its address.
constexpr int StateConnected = 3;
/// How long a join of the network may take before it counts as failed.
constexpr int ConnectSeconds = 30;
/// The port of the hosts' announcements.
constexpr u16 AnnouncePort = 24873;
/// How long a join looks for an announced room.
constexpr int FindSeconds = 15;
/// An announcement's first word and version: "AZAHAR-ADHOC 1 PORT NICKNAME".
constexpr char AnnounceTag[] = "AZAHAR-ADHOC";
/// The players a hosted room takes (local wireless sessions are of up to eight consoles).
constexpr u32 RoomSlots = 8;

enum class Role { None, Connecting, Hosting, Finding, Joined };

std::mutex mutex;
std::string last_error;
Role role = Role::None;
u32 host_address = 0;
std::atomic<bool> np_up{false};
int callback_id = -1;
/// Host or Join, from the network's connection to the room's join.
Common::NamedThread worker;
/// The host's announcements, once a second until stopped.
Common::NamedThread announcer;
std::mutex announce_mutex;
std::condition_variable_any announce_wake;

std::string Hex(int value) {
    char text[16];
    std::snprintf(text, sizeof(text), "%08x", static_cast<unsigned>(value));
    return text;
}

void SetRole(Role next) {
    std::scoped_lock lk{mutex};
    role = next;
}

void Fail(const std::string& error) {
    std::scoped_lock lk{mutex};
    last_error = error;
    role = Role::None;
    LOG_INFO(Frontend, "adhoc: {}", error);
}

/// SceNetCtl's ad hoc events (sceNetCtlCheckCallback runs it): logged, with the result of an
/// event that carries one.
void OnEvent(int event_type, void*) {
    int error = 0;
    sceNetCtlAdhocGetResult(event_type, &error);
    LOG_INFO(Frontend, "adhoc: event {}, result {:08x}", event_type, static_cast<unsigned>(error));
}

int State() {
    int state = 0;
    sceNetCtlCheckCallback();
    return sceNetCtlAdhocGetState(&state) < 0 ? -1 : state;
}

u32 Address() {
    SceNetInAddr address{};
    return sceNetCtlAdhocGetInAddr(&address) < 0 ? 0 : address.s_addr;
}

std::string AddressText(u32 address) {
    char text[16] = "";
    sceNetInetNtop(SCE_NET_AF_INET, &address, text, sizeof(text));
    return text;
}

void Disconnect() {
    const int left = sceNetCtlAdhocDisconnect();
    LOG_INFO(Frontend, "adhoc: left the ad hoc network: {:08x}", static_cast<unsigned>(left));
    if (callback_id >= 0) {
        sceNetCtlAdhocUnregisterCallback(callback_id);
        callback_id = -1;
    }
    if (np_up) {
        sceNpTerm();
        np_up = false;
    }
}

/// Records why the attempt stopped and leaves the ad hoc network: an attempt that failed past
/// the connection leaves nothing joined.
void Abandon(const std::string& error) {
    Fail(error);
    Disconnect();
}

/// Joins the ad hoc network (or finds it joined): whether the console has its address there.
bool ConnectNow() {
    if (State() == StateConnected) {
        return true;
    }
    if (!np_up) {
        // The network's own sysmodule leaves NP's out: sceNpInit's import is bound only once
        // it is loaded.
        const int loaded = sceSysmoduleLoadModule(SCE_SYSMODULE_NP);
        if (loaded < 0) {
            Fail("NP's module did not load (" + Hex(loaded) + ")");
            return false;
        }
        SceNpCommunicationId id{};
        std::memcpy(id.data, CommId, sizeof(id.data));
        const SceNpCommunicationConfig config{&id, nullptr, nullptr};
        const int np = sceNpInit(&config, nullptr);
        LOG_INFO(Frontend, "adhoc: NP with communication ID {}: {:08x}", CommId,
                 static_cast<unsigned>(np));
        if (np < 0) {
            Fail("NP did not start (" + Hex(np) + ")");
            return false;
        }
        np_up = true;
    }
    if (callback_id < 0) {
        const int registered = sceNetCtlAdhocRegisterCallback(OnEvent, nullptr, &callback_id);
        if (registered < 0) {
            callback_id = -1;
            Fail("no ad hoc callback (" + Hex(registered) + ")");
            return false;
        }
    }
    for (int which = 1; which <= 3; which++) {
        sceNetCtlAdhocShellEventA(which);
        sceNetCtlAdhocShellEventB(which);
    }
    const int asked = sceNetCtlAdhocConnectSsid(Ssid);
    LOG_INFO(Frontend, "adhoc: connect to {}: {:08x}", Ssid, static_cast<unsigned>(asked));
    if (asked < 0) {
        Fail("the system refused the ad hoc connection (" + Hex(asked) + ")");
        return false;
    }
    // SceNetCtl reports the connection only through state and callbacks that it runs when asked
    // (sceNetCtlCheckCallback): there is nothing to block on.
    int last = -2;
    for (int tick = 0; tick < ConnectSeconds * 10; tick++) {
        const int state = State();
        if (state != last) {
            LOG_INFO(Frontend, "adhoc: state {} after {} ms", state, tick * 100);
            last = state;
        }
        if (state == StateConnected) {
            LOG_INFO(Frontend, "adhoc: connected, address {}", AddressText(Address()));
            return true;
        }
        sceKernelDelayThread(100 * 1000);
    }
    Abandon("no ad hoc connection after " + std::to_string(ConnectSeconds) + " s (state " +
            std::to_string(last) + ")");
    return false;
}

/// One datagram socket of `type` bound to `port` (and `vport` for a P2P socket), with
/// broadcasts allowed and receives that give up after `timeout_ms`.
int OpenSocket(int type, u16 port, u16 vport, int timeout_ms) {
    const int s = sceNetSocket("azahar-adhoc", SCE_NET_AF_INET, type, 0);
    if (s < 0) {
        LOG_INFO(Frontend, "adhoc: socket type {}: {:08x}", type, static_cast<unsigned>(s));
        return s;
    }
    const int on = 1;
    const int timeout_us = timeout_ms * 1000;
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &on, sizeof(on));
    sceNetSetsockopt(s, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVTIMEO, &timeout_us, sizeof(timeout_us));
    SceNetSockaddrIn sin{};
    sin.sin_len = sizeof(sin);
    sin.sin_family = SCE_NET_AF_INET;
    sin.sin_port = sceNetHtons(port);
    sin.sin_vport = sceNetHtons(vport);
    sin.sin_addr.s_addr = SCE_NET_INADDR_ANY;
    const int bound = sceNetBind(s, reinterpret_cast<SceNetSockaddr*>(&sin), sizeof(sin));
    if (bound < 0) {
        LOG_INFO(Frontend, "adhoc: socket type {} bound to port {} vport {}: {:08x}", type, port,
                 vport, static_cast<unsigned>(bound));
        sceNetSocketClose(s);
        return bound;
    }
    return s;
}

int SendTo(int s, const void* data, size_t size, u32 address, u16 port, u16 vport) {
    SceNetSockaddrIn to{};
    to.sin_len = sizeof(to);
    to.sin_family = SCE_NET_AF_INET;
    to.sin_port = sceNetHtons(port);
    to.sin_vport = sceNetHtons(vport);
    to.sin_addr.s_addr = address;
    return sceNetSendto(s, data, size, 0, reinterpret_cast<SceNetSockaddr*>(&to), sizeof(to));
}

/// The host's announcement of its room to the ad hoc network, once a second until stopped.
void Announce(std::stop_token stop, std::string nickname) {
    const int s = OpenSocket(SCE_NET_SOCK_DGRAM, 0, 0, 0);
    if (s < 0) {
        return;
    }
    const std::string message = std::string(AnnounceTag) + " 1 " +
                                std::to_string(Network::DefaultRoomPort) + " " + nickname;
    std::unique_lock lk{announce_mutex};
    while (!stop.stop_requested()) {
        SendTo(s, message.data(), message.size(), SCE_NET_INADDR_BROADCAST, AnnouncePort, 0);
        announce_wake.wait_for(lk, stop, std::chrono::seconds(1), [] { return false; });
    }
    sceNetSocketClose(s);
}

/// The first room announced on the ad hoc network within FindSeconds: its host's address and
/// port, or a zero address.
std::pair<u32, u16> FindRoom() {
    const int s = OpenSocket(SCE_NET_SOCK_DGRAM, AnnouncePort, 0, FindSeconds * 1000);
    if (s < 0) {
        return {0, 0};
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(FindSeconds);
    std::pair<u32, u16> found{0, 0};
    while (found.first == 0 && std::chrono::steady_clock::now() < deadline) {
        char buffer[128] = "";
        SceNetSockaddrIn from{};
        unsigned int from_len = sizeof(from);
        const int got = sceNetRecvfrom(s, buffer, sizeof(buffer) - 1, 0,
                                       reinterpret_cast<SceNetSockaddr*>(&from), &from_len);
        if (got < 0) {
            break; // the receive timed out at the deadline
        }
        buffer[got] = '\0';
        std::istringstream in(buffer);
        std::string tag;
        int version = 0, port = 0;
        in >> tag >> version >> port;
        if (tag == AnnounceTag && version == 1 && port > 0 && port <= 65535) {
            found = {from.sin_addr.s_addr, static_cast<u16>(port)};
            LOG_INFO(Frontend, "adhoc: room announced by {} port {}: {}", AddressText(found.first),
                     port, buffer);
        }
    }
    sceNetSocketClose(s);
    return found;
}

void StopAnnouncing() {
    if (announcer.joinable()) {
        announcer.request_stop();
        announce_wake.notify_all();
        announcer.join();
    }
}

void CloseRoom() {
    StopAnnouncing();
    if (const auto room = Network::GetRoom().lock();
        room && room->GetState() == Network::Room::State::Open) {
        room->Destroy();
    }
}

void HostNow(Core::System& system, const std::string& nickname) {
    if (!ConnectNow()) {
        return;
    }
    const u32 address = Address();
    const auto room = Network::GetRoom().lock();
    if (!room) {
        Abandon("multiplayer is not available");
        return;
    }
    if (room->GetState() != Network::Room::State::Open &&
        !room->Create(nickname + "'s ad hoc room", "", AddressText(address),
                      Network::DefaultRoomPort, "", RoomSlots, nickname, "", 0,
                      std::make_unique<Network::VerifyUser::NullBackend>())) {
        Abandon("the room did not open on " + AddressText(address));
        return;
    }
    LOG_INFO(Frontend, "adhoc: hosting a room on {} port {}", AddressText(address),
             Network::DefaultRoomPort);
    {
        std::scoped_lock lk{mutex};
        role = Role::Hosting;
        host_address = address;
        last_error.clear();
    }
    StopAnnouncing();
    announcer = Common::NamedThread{Common::ThreadCfg{"adhoc announce", 32 * 1024},
                                    [nickname](std::stop_token stop) { Announce(stop, nickname); }};
    Multiplayer::Join(system,
                      Multiplayer::Fields{AddressText(address), Network::DefaultRoomPort,
                                          nickname, ""},
                      false);
}

void JoinNow(Core::System& system, const std::string& nickname) {
    if (!ConnectNow()) {
        return;
    }
    SetRole(Role::Finding);
    const auto [address, port] = FindRoom();
    if (address == 0) {
        Abandon("no room announced on the ad hoc network in " + std::to_string(FindSeconds) +
                " s");
        return;
    }
    {
        std::scoped_lock lk{mutex};
        role = Role::Joined;
        host_address = address;
        last_error.clear();
    }
    Multiplayer::Join(system, Multiplayer::Fields{AddressText(address), port, nickname, ""},
                      false);
}


/// Starts `work` on the worker, after the one before it.
void Start(Role first, std::function<void()> work) {
    if (worker.joinable()) {
        worker.join();
    }
    {
        std::scoped_lock lk{mutex};
        role = first;
        last_error.clear();
    }
    worker = Common::NamedThread{Common::ThreadCfg{"adhoc", 64 * 1024}, std::move(work)};
}

} // namespace

void Host(Core::System& system, const std::string& nickname) {
    Start(Role::Connecting, [&system, nickname] { HostNow(system, nickname); });
}

void Join(Core::System& system, const std::string& nickname) {
    Start(Role::Connecting, [&system, nickname] { JoinNow(system, nickname); });
}

void Leave() {
    if (worker.joinable()) {
        worker.join();
    }
    Multiplayer::Leave();
    CloseRoom();
    Disconnect();
    std::scoped_lock lk{mutex};
    role = Role::None;
    host_address = 0;
}

bool Active() {
    std::scoped_lock lk{mutex};
    return role != Role::None;
}

std::string Status() {
    std::scoped_lock lk{mutex};
    switch (role) {
    case Role::Connecting:
        return I18n::Tr("Joining the ad hoc network...");
    case Role::Finding:
        return I18n::Tr("Looking for an ad hoc room...");
    case Role::Hosting:
        return I18n::TrFormat("Hosting an ad hoc room at {}", AddressText(host_address));
    case Role::Joined:
        return I18n::TrFormat("In the ad hoc room at {}", AddressText(host_address));
    default:
        // The failure's reason (English) for the TTY alone: the screen shows no inactive state.
        return last_error.empty() ? std::string{I18n::Tr("Not on the ad hoc network")}
                                  : I18n::TrFormat("Not on the ad hoc network: {}", last_error);
    }
}

#ifdef VITA_DEBUG_TOOLS
bool RunCommand(Core::System& system, const std::string& command) {
    std::istringstream in(command);
    std::string word, verb, argument;
    in >> word >> verb >> argument;
    if (word != "adhoc") {
        return false;
    }
    const std::string nickname =
        argument.empty() ? Multiplayer::CurrentFields().nickname : argument;
    if (verb == "host" || verb == "join") {
        if (!system.IsPoweredOn()) {
            LOG_INFO(Frontend, "adhoc: {} needs a running title (command)", verb);
        } else if (verb == "host") {
            LOG_INFO(Frontend, "adhoc: hosting as {} (command)", nickname);
            Host(system, nickname);
        } else {
            LOG_INFO(Frontend, "adhoc: joining as {} (command)", nickname);
            Join(system, nickname);
        }
    } else if (verb == "leave") {
        Leave();
    } else {
        LOG_INFO(Frontend, "adhoc: {} (command)", Status());
    }
    return true;
}
#endif

} // namespace VitaFrontend::Adhoc
