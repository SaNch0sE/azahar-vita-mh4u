// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// thumb_svc_test: two threads each run a Thumb loop of svcSleepThread calls from one svc site
// (loop.s) whose next instruction is movable; the emulator patches such a site to a branch into
// a stub that holds the moved instruction. Each thread's loop count must come back exactly N:
// a thread resuming inside the patched branch (the svc's next halfword) runs garbage instead.
// Then the main thread alone, LONG sleeps of 4 ms from a second site: each ends its slice while
// the thread waits in the svc. Results on the top screen and as debug lines (svcOutputDebugString). Start quits.

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define N 2000
#define LONG 250

u32 thumb_svc_loop(u32 n);
u32 thumb_svc_long(u32 n);

static volatile u32 results[2];
static volatile u32 done[2];

static void Worker(void* arg) {
    const u32 i = (u32)arg;
    results[i] = thumb_svc_loop(N);
    done[i] = 1;
}

static void Log(const char* text) {
    svcOutputDebugString(text, strlen(text));
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    printf("thumb_svc_test: 2 threads x %d Thumb svcs\n", N);
    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread t = threadCreate(Worker, (void*)1, 0x4000, prio, -2, false);
    Worker((void*)0);
    if (t) {
        threadJoin(t, U64_MAX);
        threadFree(t);
    }
    const u32 long_count = thumb_svc_long(LONG);
    char line[160];
    const int ok = results[0] == N && results[1] == N && long_count == LONG;
    snprintf(line, sizeof(line), "thumb_svc_test: main %lu, second %lu of %d, long %lu of %d: %s",
             (unsigned long)results[0], (unsigned long)results[1], N, (unsigned long)long_count,
             LONG, ok ? "OK" : "WRONG");
    printf("%s\n", line);
    Log(line);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
