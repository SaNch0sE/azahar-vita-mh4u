// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// mic_test: what the title's microphone hears. MIC:U samples signed 16-bit at 16360 Hz into a
// looping buffer; every frame the newest 1024 samples' peak, RMS and mean are shown, with a bar.
// Silence reads 0, the static-noise input a flat high RMS, a real microphone room noise that
// rises with a sound (the command channel's `tone HZ SECONDS` plays one on the Vita).
#include <3ds.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define BUFFER_SIZE 0x30000
#define WINDOW 1024

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    u8 *buffer = memalign(0x1000, BUFFER_SIZE);
    Result rc = buffer ? micInit(buffer, BUFFER_SIZE) : -1;
    const u32 data_size = rc == 0 ? micGetSampleDataSize() : 0;
    Result started = rc == 0 ? MICU_StartSampling(MICU_ENCODING_PCM16_SIGNED,
                                                  MICU_SAMPLE_RATE_16360, 0, data_size, true)
                             : -1;
    printf("mic_test: micInit %08lx, data %lu bytes, start %08lx\n", rc, data_size, started);
    u32 frames = 0, peak_max = 0;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        u32 peak = 0;
        double sum = 0.0, sq = 0.0;
        if (started == 0) {
            const u32 last = micGetLastSampleOffset();
            for (u32 i = 0; i < WINDOW; i++) {
                const u32 at = (last + data_size - (WINDOW - i) * 2) % data_size;
                s16 v;
                memcpy(&v, buffer + at, 2);
                const u32 a = v < 0 ? -(s32)v : (u32)v;
                peak = a > peak ? a : peak;
                sum += v;
                sq += (double)v * v;
            }
        }
        peak_max = peak > peak_max ? peak : peak_max;
        const u32 rms = (u32)sqrt(sq / WINDOW);
        char bar[41];
        const u32 n = rms * 40 / 8192 > 40 ? 40 : rms * 40 / 8192;
        memset(bar, '#', n);
        memset(bar + n, '.', 40 - n);
        bar[40] = 0;
        printf("\x1b[3;0Hframe %lu\npeak %5lu (max %5lu)\nrms %5lu  mean %6ld\n%s\n", frames, peak,
               peak_max, rms, (long)(sum / WINDOW), bar);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
        frames++;
    }
    if (started == 0) {
        MICU_StopSampling();
    }
    if (rc == 0) {
        micExit();
    }
    free(buffer);
    gfxExit();
    return 0;
}
