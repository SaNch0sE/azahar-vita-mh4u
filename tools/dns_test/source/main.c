// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// dns_test: SOC:U's gethostbyname and gethostbyaddr, their results on the top screen. On the
// Vita the host's newlib gives no alias list (h_aliases null), which crashed SOC:U's conversion
// of every lookup that succeeded.
#include <3ds.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>

static void show(const char* what, const struct hostent* h) {
    if (h == NULL) {
        printf("%s: failed, h_errno %d\n", what, h_errno);
        return;
    }
    int aliases = 0, addresses = 0;
    while (h->h_aliases != NULL && h->h_aliases[aliases] != NULL) {
        aliases++;
    }
    while (h->h_addr_list[addresses] != NULL) {
        addresses++;
    }
    struct in_addr first;
    memcpy(&first, h->h_addr_list[0], sizeof(first));
    printf("%s: name %s, type %d, length %d,\n  %d aliases, %d addresses, first %s\n", what,
           h->h_name, h->h_addrtype, h->h_length, aliases, addresses, inet_ntoa(first));
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    u32* buffer = memalign(0x1000, 0x100000);
    const Result rc = socInit(buffer, 0x100000);
    printf("dns_test: socInit %08lx\n", rc);
    show("gethostbyname example.com", gethostbyname("example.com"));
    show("gethostbyname github.com", gethostbyname("github.com"));
    struct in_addr dns;
    inet_aton("8.8.8.8", &dns);
    show("gethostbyaddr 8.8.8.8", gethostbyaddr(&dns, sizeof(dns), AF_INET));
    printf("dns_test: done\n");
    u32 frame = 0;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        printf("\x1b[12;0Hframe %lu", frame++);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    socExit();
    gfxExit();
    return 0;
}
