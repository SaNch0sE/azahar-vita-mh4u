// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// thread_exit_test: detached threads, each of a higher priority than the main thread, created one
// after another: each runs at once, counts and exits (libctru's threadExit closes the thread's own
// handle before svcExitThread, so the kernel's thread list and the running thread are its last
// owners) while the main thread is ready. N of them, then the count; any memory corruption in the
// emulator's exit path shows as a crash or a wrong count. Results on screen and as debug lines.

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define N 3000

static volatile u32 ran;

static void Worker(void* arg) {
    (void)arg;
    __atomic_add_fetch(&ran, 1, __ATOMIC_SEQ_CST);
}

static void Log(const char* text) {
    svcOutputDebugString(text, strlen(text));
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    s32 prio = 0;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    u32 created = 0, failed = 0;
    char line[128];
    for (u32 i = 0; i < N; i++) {
        if (threadCreate(Worker, NULL, 0x1000, prio - 1, -2, true)) {
            created++;
        } else {
            failed++;
            svcSleepThread(1000000);
        }
        if (i % 500 == 499) {
            snprintf(line, sizeof(line), "thread_exit_test: %lu created, %lu ran",
                     (unsigned long)created, (unsigned long)ran);
            Log(line);
        }
    }
    for (int i = 0; i < 100 && ran < created; i++) {
        svcSleepThread(10000000);
    }
    snprintf(line, sizeof(line), "thread_exit_test: %lu created, %lu ran, %lu failed: %s",
             (unsigned long)created, (unsigned long)ran, (unsigned long)failed,
             ran == created && created == N ? "OK" : "WRONG");
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
