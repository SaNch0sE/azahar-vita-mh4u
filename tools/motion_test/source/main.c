// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// motion_test: the accelerometer and the gyroscope (HIDUSER_EnableAccelerometer and
// HIDUSER_EnableGyroscope), for the Vita frontend's motion device. Every frame on the top screen:
// each sensor's words as HID's shared memory holds them (x, y, z: libctru's angularRate names
// the second word z and the third y), the accelerometer in g (512 a g, HID's scale) and the
// gyroscope in degrees a second (HIDUSER_GetGyroscopeRawToDpsCoefficient), each axis's least and
// most since the last reset, and the angle the gyroscope's rate adds up to about each axis. Twice
// a second the same as a debug line (svcOutputDebugString: the emulator's log). The frame: x+ the
// d-pad's left, y+ out of the touch screen, z+ the d-pad's up; lying face up the accelerometer
// reads (0, -1, 0) g. Select resets the extremes and the angles; Start quits.

#include <3ds.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    float v[3];
} Vec;

static void Track(const Vec* now, Vec* least, Vec* most) {
    for (int i = 0; i < 3; i++) {
        if (now->v[i] < least->v[i]) {
            least->v[i] = now->v[i];
        }
        if (now->v[i] > most->v[i]) {
            most->v[i] = now->v[i];
        }
    }
}

static void Reset(const Vec* now, Vec* least, Vec* most) {
    *least = *now;
    *most = *now;
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    const Result accel_on = HIDUSER_EnableAccelerometer();
    const Result gyro_on = HIDUSER_EnableGyroscope();
    float dps_coef = 0.0f;
    const Result coef_read = HIDUSER_GetGyroscopeRawToDpsCoefficient(&dps_coef);
    if (dps_coef <= 0.0f) {
        dps_coef = 14.375f;
    }

    Vec accel_least, accel_most, gyro_least, gyro_most;
    Vec angle = {{0.0f, 0.0f, 0.0f}};
    bool fresh = true;
    u64 last_ms = osGetTime(), last_log_ms = last_ms;
    u32 frames = 0;

    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        if (down & KEY_START) {
            break;
        }
        accelVector a;
        angularRate g;
        hidAccelRead(&a);
        hidGyroRead(&g);
        // HID's word order: libctru's g.x, g.z, g.y are its x, y, z.
        const s16 gyro_words[3] = {g.x, g.z, g.y};
        const Vec accel = {{a.x / 512.0f, a.y / 512.0f, a.z / 512.0f}};
        const Vec gyro = {{gyro_words[0] / dps_coef, gyro_words[1] / dps_coef,
                           gyro_words[2] / dps_coef}};
        const u64 now_ms = osGetTime();
        const float dt = (now_ms - last_ms) / 1000.0f;
        last_ms = now_ms;
        // The extremes from HID's first sample on: the shared memory reads zero before it (a
        // second at most: with no motion device every sample is zero).
        if (fresh && a.x == 0 && a.y == 0 && a.z == 0 && frames < 60) {
            frames++;
            gfxFlushBuffers();
            gfxSwapBuffers();
            gspWaitForVBlank();
            continue;
        }
        if (fresh || (down & KEY_SELECT)) {
            Reset(&accel, &accel_least, &accel_most);
            Reset(&gyro, &gyro_least, &gyro_most);
            angle = (Vec){{0.0f, 0.0f, 0.0f}};
            fresh = false;
        } else {
            Track(&accel, &accel_least, &accel_most);
            Track(&gyro, &gyro_least, &gyro_most);
            for (int i = 0; i < 3; i++) {
                angle.v[i] += gyro.v[i] * dt;
            }
        }
        frames++;

        printf("\x1b[1;1Hmotion_test (Select resets, Start quits)\x1b[K");
        printf("\x1b[2;1Henable accel %08lx gyro %08lx coef %08lx\x1b[K", (unsigned long)accel_on,
               (unsigned long)gyro_on, (unsigned long)coef_read);
        printf("\x1b[3;1Hgyro raw to dps %.3f, frame %lu\x1b[K", dps_coef, (unsigned long)frames);
        printf("\x1b[5;1H                 x       y       z\x1b[K");
        printf("\x1b[6;1Haccel words %7d %7d %7d\x1b[K", a.x, a.y, a.z);
        printf("\x1b[7;1Haccel g     %7.3f %7.3f %7.3f\x1b[K", accel.v[0], accel.v[1], accel.v[2]);
        printf("\x1b[8;1H  least     %7.3f %7.3f %7.3f\x1b[K", accel_least.v[0], accel_least.v[1],
               accel_least.v[2]);
        printf("\x1b[9;1H  most      %7.3f %7.3f %7.3f\x1b[K", accel_most.v[0], accel_most.v[1],
               accel_most.v[2]);
        printf("\x1b[11;1Hgyro words  %7d %7d %7d\x1b[K", gyro_words[0], gyro_words[1],
               gyro_words[2]);
        printf("\x1b[12;1Hgyro dps    %7.1f %7.1f %7.1f\x1b[K", gyro.v[0], gyro.v[1], gyro.v[2]);
        printf("\x1b[13;1H  least     %7.1f %7.1f %7.1f\x1b[K", gyro_least.v[0], gyro_least.v[1],
               gyro_least.v[2]);
        printf("\x1b[14;1H  most      %7.1f %7.1f %7.1f\x1b[K", gyro_most.v[0], gyro_most.v[1],
               gyro_most.v[2]);
        printf("\x1b[15;1Hangle deg   %7.1f %7.1f %7.1f\x1b[K", angle.v[0], angle.v[1], angle.v[2]);

        if (now_ms - last_log_ms >= 500) {
            last_log_ms = now_ms;
            char line[200];
            const int n = snprintf(line, sizeof(line),
                                   "motion_test: accel %.3f %.3f %.3f g; gyro %.1f %.1f %.1f dps; "
                                   "angle %.1f %.1f %.1f deg; words %d %d %d / %d %d %d",
                                   accel.v[0], accel.v[1], accel.v[2], gyro.v[0], gyro.v[1],
                                   gyro.v[2], angle.v[0], angle.v[1], angle.v[2], a.x, a.y, a.z,
                                   gyro_words[0], gyro_words[1], gyro_words[2]);
            svcOutputDebugString(line, n);
        }
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }

    HIDUSER_DisableGyroscope();
    HIDUSER_DisableAccelerometer();
    gfxExit();
    return 0;
}
