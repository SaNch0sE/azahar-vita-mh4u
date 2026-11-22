// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// cstick_test: the circle pad and the C-stick (ir:rst: the New 3DS's C-stick, the Circle Pad
// Pro's stick) printed every frame on the top screen with their distance from the centre and the
// most each has reached (the guest's full reach is about 154 and 156), for the Vita frontend's
// sticks. Start quits.

#include <3ds.h>
#include <math.h>
#include <stdio.h>

static int Radius(const circlePosition* p) {
    return (int)lroundf(sqrtf((float)(p->dx * p->dx + p->dy * p->dy)));
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    const Result rst = irrstInit();
    int pad_most = 0, cstick_most = 0;

    while (aptMainLoop()) {
        hidScanInput();
        irrstScanInput();
        if (hidKeysDown() & KEY_START) {
            break;
        }
        circlePosition pad, cstick;
        hidCircleRead(&pad);
        irrstCstickRead(&cstick);
        const u32 held = hidKeysHeld() | irrstKeysHeld();
        if (Radius(&pad) > pad_most) {
            pad_most = Radius(&pad);
        }
        if (Radius(&cstick) > cstick_most) {
            cstick_most = Radius(&cstick);
        }
        printf("\x1b[1;1Hcstick_test (Start quits)\x1b[K");
        printf("\x1b[3;1Hir:rst init %08lx\x1b[K", (unsigned long)rst);
        printf("\x1b[5;1H            x     y     r  most\x1b[K");
        printf("\x1b[6;1Hcircle pad  %5d %5d %5d %5d\x1b[K", pad.dx, pad.dy, Radius(&pad), pad_most);
        printf("\x1b[7;1Hc-stick     %5d %5d %5d %5d\x1b[K", cstick.dx, cstick.dy, Radius(&cstick),
               cstick_most);
        printf("\x1b[9;1HZL %d ZR %d\x1b[K", (held & KEY_ZL) ? 1 : 0, (held & KEY_ZR) ? 1 : 0);
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    irrstExit();
    gfxExit();
    return 0;
}
