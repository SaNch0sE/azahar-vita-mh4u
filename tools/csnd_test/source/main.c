// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// csnd_test: sound through CSND (not the DSP), for the emulator's CSND mixing. Three stages, each
// after a second of silence so a recording of the output can be cut by them:
//   1. a 440 Hz PCM16 loop on channel 8, centred, 4 s, then stopped;
//   2. an 880 Hz PCM8 one-shot of 1 s on channel 8 (it must end by itself);
//   3. 440 Hz on channel 8 panned left with 660 Hz on channel 9 panned right, looped, 3 s;
//   4. 440 Hz through the DSP (ndsp), 3 s: a recording's pitch reference for the CSND tones.
// Each stage checks the channel's playing state (csndIsPlaying) while it should play and after it
// should have stopped. The results go to the top screen and sdmc:/csnd_test.txt. Start quits.
// The times are the guest's: an emulator running uncapped plays it all in a fraction of a second.

#include <3ds.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RATE 22050

static char report[2048];
static size_t report_used;
static int failures;

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

static void WriteReport(void) {
    FILE* f = fopen("sdmc:/csnd_test.txt", "w");
    if (f != NULL) {
        fwrite(report, 1, report_used, f);
        fclose(f);
    }
}

static void Wait(u32 ms) {
    svcSleepThread((s64)ms * 1000000);
}

// Whether the channel plays now, against what the stage expects.
static void Expect(u32 channel, bool playing, const char* when) {
    u8 status = 0xFF;
    const Result r = csndIsPlaying(channel, &status);
    const bool ok = R_SUCCEEDED(r) && (status != 0) == playing;
    failures += !ok;
    Line("  ch %lu %s: %s (%s)", (unsigned long)channel, when, status ? "playing" : "stopped",
         ok ? "ok" : "FAIL");
}

// The CSND module fades a stopped channel out before it stops it: checked 100 ms later.
static void Stop(u32 channel) {
    CSND_SetPlayState(channel, 0);
    csndExecCmds(true);
}

// One second of a sine at HZ (whole periods, so a loop of it has no seam).
static s16* Sine16(u32 hz) {
    s16* data = linearAlloc(RATE * sizeof(s16));
    for (u32 i = 0; i < RATE; i++) {
        data[i] = (s16)(12000.0 * sin(2.0 * M_PI * hz * i / RATE));
    }
    GSPGPU_FlushDataCache(data, RATE * sizeof(s16));
    return data;
}

static s8* Sine8(u32 hz) {
    s8* data = linearAlloc(RATE);
    for (u32 i = 0; i < RATE; i++) {
        data[i] = (s8)(48.0 * sin(2.0 * M_PI * hz * i / RATE));
    }
    GSPGPU_FlushDataCache(data, RATE);
    return data;
}

static bool Quit(void) {
    hidScanInput();
    return (hidKeysDown() & KEY_START) != 0;
}

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    Line("csnd_test (Start quits)");
    Result r = csndInit();
    if (R_FAILED(r)) {
        Line("csndInit %08lx: FAIL", (unsigned long)r);
        WriteReport();
    } else {
        s16* a440 = Sine16(440);
        s16* a660 = Sine16(660);
        s8* a880 = Sine8(880);
        Wait(1000);

        Line("1: 440 Hz PCM16 loop, 4 s");
        r = csndPlaySound(8, SOUND_FORMAT_16BIT | SOUND_REPEAT, RATE, 1.0f, 0.0f, a440, a440,
                          RATE * sizeof(s16));
        Line("  csndPlaySound %08lx", (unsigned long)r);
        Wait(500);
        Expect(8, true, "at 0.5 s");
        Wait(3000);
        Expect(8, true, "at 3.5 s (looping)");
        Wait(500);
        Stop(8);
        Wait(100);
        Expect(8, false, "100 ms after the stop");
        Wait(900);

        Line("2: 880 Hz PCM8 one-shot, 1 s");
        r = csndPlaySound(8, SOUND_FORMAT_8BIT | SOUND_ONE_SHOT, RATE, 1.0f, 0.0f, a880, NULL,
                          RATE);
        Line("  csndPlaySound %08lx", (unsigned long)r);
        Wait(500);
        Expect(8, true, "at 0.5 s");
        Wait(1300);
        Expect(8, false, "at 1.8 s (ended)");
        Wait(1000);

        Line("3: 440 Hz left + 660 Hz right, 3 s");
        r = csndPlaySound(8, SOUND_FORMAT_16BIT | SOUND_REPEAT, RATE, 1.0f, -1.0f, a440, a440,
                          RATE * sizeof(s16));
        const Result r2 = csndPlaySound(9, SOUND_FORMAT_16BIT | SOUND_REPEAT, RATE, 1.0f, 1.0f,
                                        a660, a660, RATE * sizeof(s16));
        Line("  csndPlaySound %08lx %08lx", (unsigned long)r, (unsigned long)r2);
        Wait(1500);
        Expect(8, true, "at 1.5 s");
        Expect(9, true, "at 1.5 s");
        Wait(1500);
        Stop(8);
        Stop(9);
        Wait(100);
        Expect(8, false, "100 ms after the stop");
        Expect(9, false, "100 ms after the stop");

        Wait(1000);

        // 4: the same 440 Hz through the DSP (ndsp), the pitch reference for a recording: an
        // emulator's HLE DSP takes any component, so a dummy one stands in for dspfirm.cdc.
        Line("4: 440 Hz through the DSP (reference), 3 s");
        static u8 component[0x400] __attribute__((aligned(0x100)));
        ndspUseComponent(component, sizeof(component), 0xFF, 0xFF);
        r = ndspInit();
        Line("  ndspInit %08lx", (unsigned long)r);
        if (R_SUCCEEDED(r)) {
            ndspSetOutputMode(NDSP_OUTPUT_STEREO);
            ndspChnReset(0);
            ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
            ndspChnSetRate(0, RATE);
            ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
            static ndspWaveBuf wave;
            memset(&wave, 0, sizeof(wave));
            wave.data_vaddr = a440;
            wave.nsamples = RATE;
            wave.looping = true;
            DSP_FlushDataCache(a440, RATE * sizeof(s16));
            ndspChnWaveBufAdd(0, &wave);
            Wait(3000);
            ndspChnWaveBufClear(0);
            Wait(100);
            ndspExit();
        } else {
            failures++;
        }

        Line(failures == 0 ? "csnd_test: PASS" : "csnd_test: FAIL (%d)", failures);
        WriteReport();
        linearFree(a440);
        linearFree(a660);
        linearFree(a880);
        csndExit();
    }
    while (!Quit()) {
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
