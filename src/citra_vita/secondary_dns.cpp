// Copyright 2026 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// SceNet's resolver stops at a primary DNS server that refuses it: gethostbyname fails within
// milliseconds and the secondary server is never asked. Every lookup here ends in newlib's
// gethostbyname, getaddrinfo included, so this one wrap serves soc:U, http:C, rooms and Artic Base:
// a failed lookup is asked again on the secondary server, over UDP.
//
// It can run on the emulation thread (soc:U's lookups are synchronous), so the wait is short: a
// server that is reachable answers in tens of milliseconds, and one that is not costs a second.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <psp2/kernel/rng.h>
#include <psp2/net/netctl.h>

#include "common/common_types.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"

extern "C" struct hostent* __real_gethostbyname(const char* name);

namespace {

constexpr int QueryTries = 2;
constexpr int ReplyWaitMs = 500;

constexpr std::size_t HeaderSize = 12;
constexpr std::size_t MaxMessageSize = 512; // RFC 1035: what a server sends over UDP
constexpr std::size_t MaxLabelSize = 63;
constexpr std::size_t RecordFixedSize = 10; // type, class, TTL, data length

constexpr u16 TypeA = 1;
constexpr u16 ClassIn = 1;

u16 Read16(std::span<const u8> data, std::size_t at) {
    return static_cast<u16>(data[at] << 8 | data[at + 1]);
}

/// Writes the query for NAME's A records with the given ID. Its size, or 0 if DNS cannot carry NAME.
std::size_t BuildQuery(std::string_view name, u16 id, std::span<u8> packet) {
    if (!name.empty() && name.back() == '.') {
        name.remove_suffix(1);
    }

    const std::array<u8, HeaderSize> header{
        static_cast<u8>(id >> 8), static_cast<u8>(id), // ID
        0x01, 0x00,                                    // recursion desired
        0x00, 0x01,                                    // one question
    };
    std::memcpy(packet.data(), header.data(), header.size());
    std::size_t size = header.size();

    while (!name.empty()) {
        const std::size_t label = std::min(name.find('.'), name.size());
        if (label == 0 || label > MaxLabelSize || size + 1 + label + 5 > packet.size()) {
            return 0;
        }
        packet[size++] = static_cast<u8>(label);
        std::memcpy(packet.data() + size, name.data(), label);
        size += label;
        name.remove_prefix(std::min(label + 1, name.size()));
    }
    if (size == header.size()) {
        return 0;
    }

    const std::array<u8, 5> tail{0, 0, TypeA, 0, ClassIn}; // end of the name, type, class
    std::memcpy(packet.data() + size, tail.data(), tail.size());
    return size + tail.size();
}

/// The first A record of REPLY, the server's answer to QUERY. A CNAME's target comes in the same
/// answer section, so whichever name the record carries is the one that was asked for.
std::optional<in_addr> FirstAddress(std::span<const u8> reply, std::span<const u8> query) {
    if (reply.size() < query.size() || std::memcmp(reply.data(), query.data(), 2) != 0) {
        return std::nullopt;
    }
    const bool is_response = (reply[2] & 0x80) != 0;
    const u8 opcode = (reply[2] >> 3) & 0xF;
    const u8 response_code = reply[3] & 0xF;
    if (!is_response || opcode != 0 || response_code != 0) {
        return std::nullopt;
    }
    if (Read16(reply, 4) != 1 || std::memcmp(reply.data() + HeaderSize, query.data() + HeaderSize,
                                             query.size() - HeaderSize) != 0) {
        return std::nullopt;
    }

    std::size_t at = query.size();
    for (u16 answers = Read16(reply, 6); answers > 0; --answers) {
        // The record's name: labels up to an end or a pointer to one seen earlier.
        while (true) {
            if (at >= reply.size()) {
                return std::nullopt;
            }
            const u8 length = reply[at];
            if ((length & 0xC0) == 0xC0) {
                at += 2;
                break;
            }
            if ((length & 0xC0) != 0) {
                return std::nullopt;
            }
            at += 1u + length;
            if (length == 0) {
                break;
            }
        }

        if (at + RecordFixedSize > reply.size()) {
            return std::nullopt;
        }
        const u16 type = Read16(reply, at);
        const u16 record_class = Read16(reply, at + 2);
        const u16 data_size = Read16(reply, at + 8);
        at += RecordFixedSize;
        if (at + data_size > reply.size()) {
            return std::nullopt;
        }
        if (type == TypeA && record_class == ClassIn && data_size == sizeof(in_addr)) {
            in_addr address;
            std::memcpy(&address, reply.data() + at, sizeof(address));
            return address;
        }
        at += data_size;
    }
    return std::nullopt;
}

/// NAME's address from the console's secondary DNS server, if it has one and it answers.
std::optional<in_addr> AskSecondaryDns(const char* name) {
    SceNetCtlInfo info{};
    in_addr server{};
    if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_SECONDARY_DNS, &info) < 0 ||
        inet_pton(AF_INET, info.secondary_dns, &server) != 1 || server.s_addr == 0) {
        return std::nullopt;
    }

    u16 id = 0;
    sceKernelGetRandomNumber(&id, sizeof(id));
    std::array<u8, MaxMessageSize> query;
    const std::size_t query_size = BuildQuery(name, id, query);
    if (query_size == 0) {
        return std::nullopt;
    }

    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return std::nullopt;
    }
    SCOPE_EXIT({ ::close(fd); });

    // Connected, so that only this server's datagrams arrive and a refusal shows up as one.
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr = server;
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != 0) {
        return std::nullopt;
    }

    for (int tries = 0; tries < QueryTries; tries++) {
        if (::send(fd, query.data(), query_size, 0) != static_cast<ssize_t>(query_size)) {
            return std::nullopt;
        }
        pollfd reply_ready{fd, POLLIN, 0};
        if (::poll(&reply_ready, 1, ReplyWaitMs) <= 0) {
            continue;
        }
        std::array<u8, MaxMessageSize> reply;
        const ssize_t got = ::recv(fd, reply.data(), reply.size(), 0);
        if (got < 0) {
            return std::nullopt;
        }
        return FirstAddress(std::span<const u8>(reply.data(), static_cast<std::size_t>(got)),
                            std::span<const u8>(query.data(), query_size));
    }
    return std::nullopt;
}

} // namespace

extern "C" struct hostent* __wrap_gethostbyname(const char* name) {
    if (struct hostent* const found = __real_gethostbyname(name)) {
        return found;
    }

    const int lookup_error = errno;
    const std::optional<in_addr> address = AskSecondaryDns(name);
    if (!address) {
        errno = lookup_error;
        return nullptr;
    }
    LOG_INFO(Network, "The lookup of {} failed, the secondary DNS server answered it", name);

    // Static, as newlib's own result is: the caller reads it before the next lookup.
    static std::array<char, 256> result_name;
    static in_addr result_address;
    static std::array<char*, 2> result_addresses;
    static std::array<char*, 1> result_aliases;
    static hostent result;

    std::strncpy(result_name.data(), name, result_name.size() - 1);
    result_address = *address;
    result_addresses = {reinterpret_cast<char*>(&result_address), nullptr};
    result_aliases = {nullptr};
    result.h_name = result_name.data();
    result.h_aliases = result_aliases.data();
    result.h_addrtype = AF_INET;
    result.h_length = sizeof(result_address);
    result.h_addr_list = result_addresses.data();
    return &result;
}
