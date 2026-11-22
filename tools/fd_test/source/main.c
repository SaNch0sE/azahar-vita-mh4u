// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// fd_test: whether a title's open files survive a standby. Every frame a 4 KiB read of its RomFS
// at the next 64 KiB line of a 32 MiB file (the emulator's RomFS cache holds 16 MiB of 64 KiB
// lines, so each misses it), every tenth frame a 128 KiB read (past the cache), each checked
// against the file's pattern (word i is i ^ 0x5a5a5a5a); every second 4 bytes appended to an sdmc
// file kept open. The counts on the top screen: after a standby they must go on rising, with no
// failures.
#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define FILE_SIZE (32u << 20)
#define LINE (64u << 10)
#define LARGE (128u << 10)

static u32 buf[LARGE / 4];

static int check(FILE *f, u32 offset, u32 size) {
    if (fseek(f, offset, SEEK_SET) != 0 || fread(buf, 1, size, f) != size) {
        return 0;
    }
    for (u32 i = 0; i < size / 4; i++) {
        if (buf[i] != ((offset / 4 + i) ^ 0x5a5a5a5au)) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    const Result rc = romfsInit();
    FILE *r = fopen("romfs:/data.bin", "rb");
    FILE *w = fopen("sdmc:/fd_test.bin", "wb");
    if (r) {
        setvbuf(r, NULL, _IONBF, 0);
    }
    printf("fd_test: romfsInit %08lx, romfs %s, sdmc %s\n", rc, r ? "open" : "FAILED",
           w ? "open" : "FAILED");
    u32 frames = 0, ok = 0, bad = 0, large_ok = 0, large_bad = 0, w_ok = 0, w_bad = 0;
    u32 first_bad = 0;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        if (r) {
            const u32 line = (frames * 3) % (FILE_SIZE / LINE);
            if (check(r, line * LINE + (frames % 16) * 4096, 4096)) {
                ok++;
            } else if (bad++ == 0) {
                first_bad = frames;
            }
            if (frames % 10 == 0) {
                const u32 at = ((frames / 10) * 5 % (FILE_SIZE / LARGE)) * LARGE;
                if (check(r, at, LARGE)) {
                    large_ok++;
                } else if (large_bad++ == 0 && bad == 0) {
                    first_bad = frames;
                }
            }
        }
        if (w && frames % 60 == 0) {
            const u32 v = frames;
            if (fwrite(&v, 4, 1, w) == 1 && fflush(w) == 0) {
                w_ok++;
            } else {
                w_bad++;
            }
        }
        printf("\x1b[3;0Hframe %lu\nromfs 4 KiB reads: %lu ok, %lu failed\n"
               "romfs 128 KiB reads: %lu ok, %lu failed\nsdmc writes: %lu ok, %lu failed\n"
               "first failure at frame %lu\n",
               frames, ok, bad, large_ok, large_bad, w_ok, w_bad, first_bad);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
        frames++;
    }
    if (w) {
        fclose(w);
    }
    if (r) {
        fclose(r);
    }
    romfsExit();
    gfxExit();
    return 0;
}
