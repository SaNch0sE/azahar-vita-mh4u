// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// uds_test: local wireless (nwm::UDS) between two instances in one multiplayer room, for the Vita
// frontend's rooms. Each instance scans for the test's network for a few seconds: the first finds
// none and hosts it, the next joins it as a client. The client sends a numbered packet every frame
// and the host sends each back; both print what they sent and received and the round trips on
// the top screen, and write the same to sdmc:/uds_test-host.txt or sdmc:/uds_test-client.txt
// once the client has its echoes (or gives up). Start quits.

#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WLAN_COMM_ID 0x41554453u // "AUDS": the test's own network
#define DATA_CHANNEL 1
#define PACKETS 120
#define SCAN_MS 6000
#define CLIENT_GIVE_UP_MS 30000

static const char passphrase[] = "azahar uds_test";

typedef struct {
    u32 magic;
    u32 seq;
    u64 sent_ms;
} Packet;

static char report[2048];
static size_t report_used;

// A line on the screen and in the report.
static void Line(const char* format, ...) {
    char line[160];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    printf("%s\n", line);
    report_used += snprintf(report + report_used, sizeof(report) - report_used, "%s\n", line);
    if (report_used >= sizeof(report)) {
        report_used = sizeof(report) - 1;
    }
}

static void WriteReport(const char* path) {
    FILE* f = fopen(path, "w");
    if (f != NULL) {
        fwrite(report, 1, report_used, f);
        fclose(f);
    }
}

// Scans for the test's network for up to SCAN_MS: whether one was found, into *found.
static bool Scan(udsNetworkStruct* found) {
    static u32 beacons[0x4000 / 4];
    const u64 start = osGetTime();
    while (osGetTime() - start < SCAN_MS) {
        udsNetworkScanInfo* networks = NULL;
        size_t count = 0;
        const Result r = udsScanBeacons(beacons, sizeof(beacons), &networks, &count, WLAN_COMM_ID,
                                        0, NULL, false);
        if (R_SUCCEEDED(r) && count > 0) {
            *found = networks[0].network;
            free(networks);
            return true;
        }
        free(networks);
        svcSleepThread(250 * 1000 * 1000);
    }
    return false;
}

static bool Quit(void) {
    hidScanInput();
    return !aptMainLoop() || (hidKeysDown() & KEY_START);
}

static void RunHost(void) {
    udsNetworkStruct network;
    udsBindContext bind;
    udsGenerateDefaultNetworkStruct(&network, WLAN_COMM_ID, 0, UDS_MAXNODES);
    Result r = udsCreateNetwork(&network, passphrase, sizeof(passphrase), &bind, DATA_CHANNEL,
                                UDS_DEFAULT_RECVBUFSIZE);
    if (R_FAILED(r)) {
        Line("host: udsCreateNetwork %08lx", (unsigned long)r);
        WriteReport("sdmc:/uds_test-host.txt");
        return;
    }
    Line("host: network up, waiting for a client");
    u32 echoed = 0;
    bool written = false;
    while (!Quit()) {
        Packet p;
        size_t size = 0;
        u16 from = 0;
        r = udsPullPacket(&bind, &p, sizeof(p), &size, &from);
        if (R_SUCCEEDED(r) && size == sizeof(p) && p.magic == WLAN_COMM_ID) {
            r = udsSendTo(from, DATA_CHANNEL, UDS_SENDFLAG_Default, &p, sizeof(p));
            if (UDS_CHECK_SENDTO_FATALERROR(r)) {
                Line("host: udsSendTo %08lx", (unsigned long)r);
                break;
            }
            if (echoed++ == 0) {
                Line("host: first packet from node %u", from);
            }
            printf("\x1b[20;1Hhost: echoed %lu\x1b[K", (unsigned long)echoed);
            if (echoed == PACKETS && !written) {
                Line("host: echoed %lu packets", (unsigned long)echoed);
                WriteReport("sdmc:/uds_test-host.txt");
                written = true;
            }
        }
        gspWaitForVBlank();
    }
    if (!written) {
        Line("host: quit after %lu echoes", (unsigned long)echoed);
        WriteReport("sdmc:/uds_test-host.txt");
    }
    udsDestroyNetwork();
    udsUnbind(&bind);
}

static void RunClient(const udsNetworkStruct* network) {
    udsBindContext bind;
    Result r = udsConnectNetwork(network, passphrase, sizeof(passphrase), &bind,
                                 UDS_BROADCAST_NETWORKNODEID, UDSCONTYPE_Client, DATA_CHANNEL,
                                 UDS_DEFAULT_RECVBUFSIZE);
    if (R_FAILED(r)) {
        Line("client: udsConnectNetwork %08lx", (unsigned long)r);
        WriteReport("sdmc:/uds_test-client.txt");
        return;
    }
    Line("client: connected");
    const u64 start = osGetTime();
    u32 sent = 0, received = 0, out_of_order = 0;
    u32 next_expected = 0;
    u64 rtt_sum = 0, rtt_max = 0;
    while (received < PACKETS && osGetTime() - start < CLIENT_GIVE_UP_MS && !Quit()) {
        if (sent < PACKETS) {
            Packet p = {WLAN_COMM_ID, sent, osGetTime()};
            r = udsSendTo(UDS_HOST_NETWORKNODEID, DATA_CHANNEL, UDS_SENDFLAG_Default, &p,
                          sizeof(p));
            if (UDS_CHECK_SENDTO_FATALERROR(r)) {
                Line("client: udsSendTo %08lx", (unsigned long)r);
                break;
            }
            sent++;
        }
        for (;;) {
            Packet p;
            size_t size = 0;
            u16 from = 0;
            r = udsPullPacket(&bind, &p, sizeof(p), &size, &from);
            if (R_FAILED(r) || size == 0) {
                break;
            }
            if (size != sizeof(p) || p.magic != WLAN_COMM_ID) {
                continue;
            }
            const u64 rtt = osGetTime() - p.sent_ms;
            rtt_sum += rtt;
            rtt_max = rtt > rtt_max ? rtt : rtt_max;
            out_of_order += p.seq != next_expected;
            next_expected = p.seq + 1;
            received++;
        }
        printf("\x1b[20;1Hclient: sent %lu, echoes %lu\x1b[K", (unsigned long)sent,
               (unsigned long)received);
        gspWaitForVBlank();
    }
    Line("client: sent %lu, echoes %lu, out of order %lu", (unsigned long)sent,
         (unsigned long)received, (unsigned long)out_of_order);
    if (received != 0) {
        Line("client: round trip %llu ms on average, %llu ms at most",
             (unsigned long long)(rtt_sum / received), (unsigned long long)rtt_max);
    }
    Line(received == PACKETS ? "client: PASS" : "client: FAIL");
    WriteReport("sdmc:/uds_test-client.txt");
    udsDisconnectNetwork();
    udsUnbind(&bind);
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    Line("uds_test (Start quits)");
    Result r = udsInit(0x3000, NULL);
    if (R_FAILED(r)) {
        Line("udsInit %08lx", (unsigned long)r);
        WriteReport("sdmc:/uds_test-host.txt");
    } else {
        Line("scanning %u s for the test's network", SCAN_MS / 1000);
        udsNetworkStruct network;
        if (Scan(&network)) {
            RunClient(&network);
        } else {
            RunHost();
        }
        udsExit();
    }
    while (!Quit()) {
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
