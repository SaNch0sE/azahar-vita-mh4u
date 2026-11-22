// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// y2r_poll_test: Y2R conversions of a 400x240 YUYV frame into RGBA8, each finished by polling
// Y2RU_IsDoneReceiving (no event), as some titles do; the output was filled with 0xAB first and
// the converted pixels are never 0xAB (Y 200, U V 128, alpha 255), so a byte still 0xAB once the
// poll says done is one the conversion had not written yet. Results on screen and as debug lines.

#include <3ds.h>
#include <stdio.h>
#include <string.h>

#define W 400
#define H 240
#define RUNS 20

static void Log(const char* text) {
    svcOutputDebugString(text, strlen(text));
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    char line[160];
    Result r = y2rInit();
    u8* src = linearAlloc(W * H * 2);
    u8* dst = linearAlloc(W * H * 4);
    for (u32 i = 0; i < W * H / 2; i++) {
        src[i * 4 + 0] = 200; // Y0
        src[i * 4 + 1] = 128; // U
        src[i * 4 + 2] = 200; // Y1
        src[i * 4 + 3] = 128; // V
    }
    GSPGPU_FlushDataCache(src, W * H * 2);
    Y2RU_ConversionParams p;
    memset(&p, 0, sizeof(p));
    p.input_format = INPUT_YUV422_BATCH;
    p.output_format = OUTPUT_RGB_32;
    p.rotation = ROTATION_NONE;
    p.block_alignment = BLOCK_LINE;
    p.input_line_width = W;
    p.input_lines = H;
    p.standard_coefficient = COEFFICIENT_ITU_R_BT_601;
    p.alpha = 0xFF;
    u32 bad_runs = 0, worst = 0;
    for (u32 run = 0; run < RUNS && r >= 0; run++) {
        memset(dst, 0xAB, W * H * 4);
        GSPGPU_FlushDataCache(dst, W * H * 4);
        Y2RU_SetConversionParams(&p);
        Y2RU_SetSendingYUYV(src, W * H * 2, W * 2 * 8, 0);
        Y2RU_SetReceiving(dst, W * H * 4, W * 4 * 8, 0);
        Y2RU_StartConversion();
        bool done = false;
        u32 polls = 0;
        while (!done && polls < 1000000) {
            Y2RU_IsDoneReceiving(&done);
            polls++;
        }
        GSPGPU_InvalidateDataCache(dst, W * H * 4);
        u32 left = 0;
        for (u32 i = 0; i < W * H * 4; i++) {
            left += dst[i] == 0xAB;
        }
        bool busy = false;
        Y2RU_IsBusyConversion(&busy);
        if (left) {
            bad_runs++;
        }
        if (left > worst) {
            worst = left;
        }
        snprintf(line, sizeof(line), "y2r_poll_test: run %lu: %lu polls, %lu bytes not written, busy %d",
                 (unsigned long)run, (unsigned long)polls, (unsigned long)left, busy ? 1 : 0);
        Log(line);
        printf("%s\n", line);
    }
    snprintf(line, sizeof(line), "y2r_poll_test: init %08lx, %lu of %d runs read early (worst %lu bytes): %s",
             (unsigned long)r, (unsigned long)bad_runs, RUNS, (unsigned long)worst,
             r >= 0 && bad_runs == 0 ? "OK" : "WRONG");
    Log(line);
    printf("%s\n", line);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    y2rExit();
    gfxExit();
    return 0;
}
