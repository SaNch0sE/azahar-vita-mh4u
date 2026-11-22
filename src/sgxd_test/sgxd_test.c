// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// sgxd_test: the direct-SGX tests (docs/sgxd/README.md), one numbered test per step. "test=N"
// picks one (the default is 2; there is no 14 or 21 to 24). Results go to the console and to
// ux0:data/sgxd/NN-*.{json,bin}.
//
// 01  take the GPU from SceGpuEs4, read CORE_ID and CORE_REVISION, hold, release. Nothing drawn.
// 02  the GPU MMU and a GPU write into uncached memory; 02b into cached memory, one page plain
//     and one CACHECONSISTENT, with CPU-dirty lines beside the stores.
// 03  the TA+3D one-triangle recipe (src/sgxd/sgxd_recipe.c) at 64x64. "renders=N", "sa_bg=1"
//     (03b), "mkevents=1" (13d).
// 04  the display: the recipe at 960x544 into three CDRAM buffers, flipped. "frames=N",
//     "check=K", "hold=", "black=1". scePowerRequestDisplayOff stops the flips: the first frame
//     after the panel went dark never returns from sceDisplayWaitSetFrameBuf.
// 05  per-draw fragment constants through SA.
// 06  the vertex fetch: ImGui vertices, a U8N colour per rectangle.
// 07  the iterators: a colour and a UV per corner. "raw=N", "sweep=1".
// 08  the texture, point sampled and magnified 2x bilinear. "fsweep=1" measures the filter's
//     weights (6 bits), "fsweep=2" with an F16x4 texture.
// 09  blending over the output buffer. "opaque=1" fails: the ISP hides the background object
//     under an opaque object too, so its program reads an o0 nothing wrote this tile.
//     "stencil=1..3": the ISP's stencil, with and without CPRES.
// 10  2000 draws in one pass, each its own SA block, the even ones region-clipped. "show=0".
// 11  the code-cache rule: programs rewritten at the same address every frame. "inval=0".
// 12  ImGui's fragment program, SGXD_IMGUI_MODULATE.
// 13  what our own microkernel needs from the hardware (docs/sgxd/microkernel.md): 13a a KICK2,
//     13b with SW_EVENT, 13c a PTLA fill with core 0's event program left on ours.
// 15  the microkernel's instruction costs. "loops=N".
// 16  why test 15's loop never ends. "start=1".
// 17  test 03's render as azahar's async list, compiled and run by the microkernel.
//     "renders=N", "ptla=N".
// 18  whether the microkernel's straight-line code pays for fetching itself. "cdram=1".
// 19  the microkernel's cost model, per instruction.
// 20  register writes from several threads at once.
//
// ux0:data/sgxd/config.txt sets the variants; host0:sgxd/config.txt overrides it (a launch
// by title ID passes no arguments), and KEY=VALUE launch arguments override both. "out=DIR" moves
// the results. Test 01's: "hold=SECONDS", "cycles=N", "take=0" (the control), "blank=0",
// "notify=S", "gxm=S", "volkeys=1", "suspend=S". For a suspend the app holds the shell's
// USB-connection lock: the resume would otherwise bring Content Manager's connection up in front
// and leave the app suspended.
//
// The app never calls sceGxmInitialize: SceGpu has no per-process state for it to leave behind.

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/gxm.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/notificationutil.h>
#include <psp2/power.h>
#include <psp2/shellutil.h>
#include <psp2/sysmodule.h>
#include <psp2/vshbridge.h>
#include <taihen.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "azahar_native.h"
#include "sgxd.h"
#include "sgxd_ptla.h"
#include "sgxd_recipe.h"
#include "sgxd_session.h"
#include "sgxd_mk.h"
#include "sgxd_usse.h"

#define CONFIG_DIR "ux0:data/sgxd"
#define HOST_CONFIG "host0:sgxd/config.txt"
#define MODULE_PATH "ux0:app/" SGXD_TEST_TITLEID "/azaharnative.skprx"
#define RELAUNCH_ARGUMENT "--azaharnative-loaded"
// 24: sgxdEvents, the microkernel alone.
#define MIN_MODULE_VERSION 24

#define LOG(...) sceClibPrintf("[sgxd_test] " __VA_ARGS__)

static uint32_t dump[SGXD_DUMP_WORDS];
static char out_dir[64] = CONFIG_DIR;
static int launch_argc;
static char **launch_argv;

static int module_loaded(void) {
    SceUID search[2] = {0};
    return _vshKernelSearchModuleByName("azaharnative", search) >= 0;
}

// Imports bind when a process loads, so the first start after a boot loads the module and
// starts the app again.
static int load_module(int relaunched) {
    if (module_loaded()) {
        const int version = azaharVersion();
        if (version < MIN_MODULE_VERSION) {
            LOG("azaharnative %d is loaded, sgxd needs %d: reboot first\n", version,
                MIN_MODULE_VERSION);
            return -1;
        }
        const int cycles = azaharCycleCounter();
        if (cycles < 0) {
            LOG("azaharCycleCounter -> 0x%08x\n", (unsigned)cycles);
            return -1;
        }
        return 0;
    }
    const SceUID module = taiLoadStartKernelModule(MODULE_PATH, 0, NULL, 0);
    LOG("load %s -> 0x%08x\n", MODULE_PATH, (unsigned)module);
    if (module < 0 || relaunched) {
        return -1;
    }
    // Pass the launch arguments on, so a LOADP launch's settings survive the relaunch.
    char marker[] = RELAUNCH_ARGUMENT;
    char *args[16] = {marker};
    int n = 1;
    for (int i = 0; i < launch_argc && n < 15; i++) {
        if (launch_argv[i] && strchr(launch_argv[i], '=')) {
            args[n++] = launch_argv[i];
        }
    }
    args[n] = NULL;
    sceAppMgrLoadExec("app0:eboot.bin", args, NULL);
    for (;;) {
        sceKernelDelayThread(100 * 1000);
    }
}

static void write_file(const char *path, const void *data, unsigned size) {
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd < 0) {
        LOG("open %s -> 0x%08x\n", path, (unsigned)fd);
        return;
    }
    sceIoWrite(fd, data, size);
    sceIoClose(fd);
}

// The last KEY= in the text wins, so launch arguments appended after the file override it.
static const char *config_find(const char *text, const char *key) {
    const char *found = NULL;
    for (const char *at = strstr(text, key); at; at = strstr(at + 1, key)) {
        if (at == text || at[-1] == ' ' || at[-1] == '\n') {
            found = at + strlen(key);
        }
    }
    return found;
}

static int config_value(const char *text, const char *key, int fallback) {
    const char *at = config_find(text, key);
    return at ? atoi(at) : fallback;
}

static void config_string(const char *text, const char *key, char *out, unsigned size) {
    const char *at = config_find(text, key);
    if (!at) {
        return;
    }
    unsigned n = 0;
    while (at[n] && at[n] != ' ' && at[n] != '\n' && at[n] != '\r' && n + 1 < size) {
        out[n] = at[n];
        n++;
    }
    out[n] = 0;
}

static void out_path(char *path, unsigned size, const char *name) {
    snprintf(path, size, "%s/%s", out_dir, name);
}

typedef struct Ids {
    int submitted;
    uint32_t core_id, core_revision, master_core_config;
} Ids;

static Ids read_ids(void) {
    SgxdOp ops[3] = {
        {SGXD_OP_READ, 0x0020, 0, 0},
        {SGXD_OP_READ, 0x0024, 0, 0},
        {SGXD_OP_READ, 0x4000, 0, 0},
    };
    Ids ids = {0};
    ids.submitted = sgxdSubmit(ops, 3);
    ids.core_id = ops[0].value;
    ids.core_revision = ops[1].value;
    ids.master_core_config = ops[2].value;
    return ids;
}

// A black 960x544 frame in CDRAM. The display controller scans it out; the SGX is not involved.
static int show_black_frame(void) {
    const SceSize size = 2 * 1024 * 1024;  // 960 * 544 * 4 rounded up to CDRAM's 256 KiB
    const SceUID block = sceKernelAllocMemBlock("sgxd_test_black", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                                size, NULL);
    void *base = NULL;
    if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0) {
        return block < 0 ? block : -1;
    }
    memset(base, 0, size);
    SceDisplayFrameBuf frame = {
        .size = sizeof(frame),
        .base = base,
        .pitch = 960,
        .pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
        .width = 960,
        .height = 544,
    };
    return sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static volatile uint64_t gxm_called_us, gxm_init_us, gxm_term_us;
static volatile int gxm_init_ret = 1, gxm_term_ret = 1;

static int gxm_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    SceGxmInitializeParams params = {0};
    params.displayQueueMaxPendingCount = 1;
    params.parameterBufferSize = SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE;
    gxm_called_us = sceKernelGetProcessTimeWide();
    gxm_init_ret = sceGxmInitialize(&params);
    gxm_init_us = sceKernelGetProcessTimeWide();
    gxm_term_ret = gxm_init_ret < 0 ? gxm_init_ret : sceGxmTerminate();
    gxm_term_us = sceKernelGetProcessTimeWide();
    return 0;
}

static int send_notification(void) {
    static const char text[] = "sgxd_test: a notification while sgxd owns the GPU";
    SceWChar16 wide[sizeof(text)];
    for (unsigned i = 0; i < sizeof(text); i++) {
        wide[i] = (SceWChar16)text[i];
    }
    return sceNotificationUtilSendNotification(wide);
}

static void volume_keys_second(uint32_t *last) {
    for (int i = 0; i < 50; i++) {
        SceCtrlData pad = {0};
        const int keys = azaharVolumeKeys();
        if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0 && keys > 0) {
            pad.buttons |= (uint32_t)keys;
        }
        if (pad.buttons != *last) {
            const uint32_t pressed = pad.buttons & ~*last;
            LOG("pad %08x\n", (unsigned)pad.buttons);
            if (pressed & (SCE_CTRL_VOLUP | SCE_CTRL_VOLDOWN)) {
                const int step = (pressed & SCE_CTRL_VOLUP) ? 1 : -1;
                LOG("volume %+d -> 0x%08x\n", step, (unsigned)azaharSystemVolume(step));
            }
            *last = pad.buttons;
        }
        sceKernelDelayThread(20000);
    }
}

static int test_take(int hold_s, int cycles, int notify_s, int gxm_s, int suspend_s,
                     int volkeys) {
    char json[2048];
    int len = 0;
    int failures = 0;
    SgxdInfo first = {0};
    Ids before = {0}, after = {0};
    int dumped = 0, released = 0, cycle;
    int notified = 0, lost = 0, lost_release = 0, retaken = 0;
    Ids again = {0};
    SceUID gxm = -1;
    uint64_t released_us = 0;

    for (cycle = 0; cycle < cycles; cycle++) {
        SgxdInfo info = {.size = sizeof(info)};
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        const int taken = sgxdTake(&info);
        const uint64_t t1 = sceKernelGetProcessTimeWide();
        if (cycle == 0 || taken < 0) {
            LOG("cycle %d take -> %d: step %u status %08x core id %08x revision %08x cores %u "
                "quiesce %u bif %08x/%08x/%08x kernel %u us call %u us\n",
                cycle, taken, info.step, (unsigned)info.step_status, info.core_id,
                info.core_revision, info.cores, info.quiesce_tries, info.bif_int_stat,
                info.bif_fault, info.master_bif_fault, info.take_us, (unsigned)(t1 - t0));
        }
        if (cycle == 0) {
            first = info;
        }
        if (taken < 0) {
            failures++;
            break;
        }
        if (cycle == 0) {
            before = read_ids();
            dumped = sgxdDump(dump, SGXD_DUMP_WORDS);
            if (dumped > 0) {
                char path[96];
                out_path(path, sizeof(path), "01-take.bin");
                write_file(path, dump, sizeof(dump));
            }
            LOG("ids through submit: %d, %08x %08x %08x; dump %d\n", before.submitted,
                before.core_id, before.core_revision, before.master_core_config, dumped);
            LOG("holding %d s\n", hold_s);
            uint32_t pad_last = 0;
            for (int second = 0; second < hold_s; second++) {
                if (second == gxm_s) {
                    gxm = sceKernelCreateThread("sgxd_test_gxm", gxm_thread, 0x10000100, 0x10000, 0,
                                                0, NULL);
                    LOG("gxm thread at %d s -> 0x%08x\n", second,
                        (unsigned)(gxm < 0 ? gxm : sceKernelStartThread(gxm, 0, NULL)));
                }
                if (second == notify_s) {
                    notified = send_notification();
                    LOG("notification at %d s -> 0x%08x\n", second, (unsigned)notified);
                }
                if (second == suspend_s) {
                    LOG("suspend at %d s -> 0x%08x\n", second, (unsigned)scePowerRequestSuspend());
                }
                // As a game does while it runs: no automatic standby in a long hold.
                sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
                if (volkeys) {
                    volume_keys_second(&pad_last);
                } else {
                    sceKernelDelayThread(1000000);
                }
            }
            after = read_ids();
            LOG("ids after the hold: %d, %08x %08x\n", after.submitted, after.core_id,
                after.core_revision);
            if (after.submitted == SGXD_ERR_LOST) {
                // A suspend took our side down; the resume gave SceGpu its GPU back.
                lost = 1;
                lost_release = sgxdRelease();
                SgxdInfo info2 = {.size = sizeof(info2)};
                retaken = sgxdTake(&info2);
                again = read_ids();
                LOG("lost during the hold: release -> %d, take again -> %d, ids %d, %08x %08x\n",
                    lost_release, retaken, again.submitted, again.core_id, again.core_revision);
                if (retaken < 0) {
                    failures++;
                    break;
                }
            }
        }
        released = sgxdRelease();
        if (cycle == 0) {
            released_us = sceKernelGetProcessTimeWide();
        }
        if (cycle == 0 || released < 0) {
            LOG("cycle %d release -> %d\n", cycle, released);
        }
        if (released < 0) {
            failures++;
            break;
        }
    }
    if (gxm >= 0) {
        SceUInt timeout = 10 * 1000 * 1000;
        const int waited = sceKernelWaitThreadEnd(gxm, NULL, &timeout);
        // Times in ms from the release; negative is before it.
        LOG("gxm thread: wait -> 0x%08x; called %lld ms, initialize -> 0x%08x at %lld ms, "
            "terminate -> 0x%08x at %lld ms\n",
            (unsigned)waited, ((long long)gxm_called_us - (long long)released_us) / 1000,
            (unsigned)gxm_init_ret, ((long long)gxm_init_us - (long long)released_us) / 1000,
            (unsigned)gxm_term_ret, ((long long)gxm_term_us - (long long)released_us) / 1000);
        failures += waited < 0 || gxm_init_ret < 0;
    }
    const Ids *last = lost ? &again : &after;
    const int ids_ok = first.core_id == 0x01174403u && first.core_revision == 0x00020106u &&
                       before.core_id == first.core_id && last->core_id == first.core_id &&
                       last->core_revision == first.core_revision;
    failures += !ids_ok;

    len += snprintf(json + len, sizeof(json) - len,
                    "{\"test\": \"01-take\", \"hold_s\": %d, \"cycles_requested\": %d, "
                    "\"cycles_done\": %d,\n \"take\": {\"step\": %u, \"step_status\": %d, "
                    "\"module_nid\": \"0x%08x\", \"quiesce_tries\": %u, \"take_us\": %u, "
                    "\"core_id\": \"0x%08x\", \"core_revision\": \"0x%08x\", \"cores\": %u, "
                    "\"bif_int_stat\": \"0x%08x\", \"bif_fault\": \"0x%08x\", "
                    "\"master_bif_fault\": \"0x%08x\"},\n",
                    hold_s, cycles, cycle, first.step, (int)first.step_status, first.module_nid,
                    first.quiesce_tries, first.take_us, first.core_id, first.core_revision,
                    first.cores, first.bif_int_stat, first.bif_fault, first.master_bif_fault);
    len += snprintf(json + len, sizeof(json) - len,
                    " \"submit_before\": {\"done\": %d, \"core_id\": \"0x%08x\", "
                    "\"core_revision\": \"0x%08x\", \"master_core_config\": \"0x%08x\"},\n"
                    " \"submit_after\": {\"done\": %d, \"core_id\": \"0x%08x\", "
                    "\"core_revision\": \"0x%08x\"},\n"
                    " \"notify_s\": %d, \"notification\": \"0x%08x\", \"lost\": %d, "
                    "\"lost_release\": %d, \"take_again\": %d, \"ids_again\": {\"done\": %d, "
                    "\"core_id\": \"0x%08x\", \"core_revision\": \"0x%08x\"},\n"
                    " \"dump\": %d, \"release\": %d, \"result\": \"%s\"}\n",
                    before.submitted, before.core_id, before.core_revision,
                    before.master_core_config, after.submitted, after.core_id,
                    after.core_revision, notify_s, (unsigned)notified, lost, lost_release,
                    retaken, again.submitted, again.core_id, again.core_revision, dumped, released,
                    failures ? "FAIL" : "PASS");
    char path[96];
    out_path(path, sizeof(path), "01-take.json");
    write_file(path, json, (unsigned)len);
    LOG("01-take %s (%d of %d cycles)\n", failures ? "FAIL" : "PASS", cycle, cycles);
    return failures;
}


// ---------------------------------------------------------------------------- test 02

// USSE and PDS words.
static uint64_t usse_limm(unsigned dest, uint32_t value) {
    const uint32_t hi = 0xfc200000u | ((value >> 21) & 0x1fu) << 4 | ((value >> 26) & 0x3fu) << 12;
    const uint32_t lo = (dest & 0x7fu) << 21 | (value & 0x1fffffu);
    return (uint64_t)hi << 32 | lo;
}

static uint64_t usse_sta32(unsigned base, unsigned offset, unsigned value) {
    return 0xf0220002ull << 32 | 0x80000000u | base << 14 | offset << 7 | value;
}

static uint64_t usse_mov_pa(unsigned dest, unsigned pa) {
    return 0x50010008ull << 32 | 0xa0000000u | dest << 21 | pa << 7;
}

#define USSE_IDF 0xf820000000000000ull
#define USSE_WDF 0xf920000000000000ull
#define USSE_SMLSI 0xfa10000000000001ull
#define USSE_PHAS_END 0xfa44070000000000ull
#define USSE_NOP_END 0xf804014000000000ull

// DOUTA of IR0, IR1 and data rows into pa0-pa2, then DOUTU.
static const uint32_t pds_douta_code[] = {
    0x07020806u, 0x07021906u, 0x07040106u, 0x07040106u, 0x07040106u, 0x07080185u, 0xaf000000u,
};
#define PDS_DOUTA_UNITS 3u
static const uint32_t pds_doutu_code[] = {0x07000185u, 0xaf000000u};
#define PDS_DOUTU_UNITS 1u

#define GPU_DONE 0x600dd0e5u
#define GPU_FILL 0xeeeeeeeeu
#define GPU_KICKER_VALUE 0x5au
#define GPU_STORES 16u
#define GPU_WORDS 32u   // a result block: the stores, then the marker in word 31
#define GPU_TEMPS 8u

// The uncached buffer's layout, and the GPU addresses (docs/sgxd/memory.md: sgxd
// heaps below 256 MiB).
#define NC_GPU_VA 0x00100000u
#define NC_SIZE 0x10000u
#define NC_PDS(n) (0x0000u + (n) * 0x100u)   // 0: core 0's, to the NC result; 1: bank 0's; 2, 3: to 02b's
#define NC_CODE 0x1000u                      // the store program, code base 15
#define NC_CODE_END 0x1800u                  // PHAS END, NOP END: every other code base
#define NC_RESULT 0x2000u
#define NC_KICKER 0x3000u
#define CACHED_GPU_VA 0x00200000u            // page 0 plain, page 1 CACHECONSISTENT
#define CACHED_SIZE 0x10000u
#define CACHED_RESULT 0x0400u                // in each page; dirty CPU lines at 0x0000-0x03ff and 0x0480-0x07ff

static uint32_t gpu_pattern(unsigned run, unsigned n) {
    return 0xb2000000u | run << 16 | n << 8 | (n ^ 0x5a);
}

static uint32_t douta_data[12];

static void put_pds(uint8_t *buffer, uint32_t offset, const uint32_t *data, unsigned units,
                    const uint32_t *code, unsigned count) {
    memcpy(buffer + offset, data, units * 16);
    memcpy(buffer + offset + units * 16, code, count * 4);
}

// pa0 = IR0, pa1 = IR1, pa2 = the result block; DOUTU code base 15, GPU_TEMPS temporaries.
static void put_douta_temps(uint8_t *nc, unsigned slot, uint32_t result_va, unsigned temp_units) {
    const uint32_t data[12] = {0, 0x100, 0, 0, result_va, 0x200, 0, 0, 15u | temp_units << 24, 1, 0, 0};
    put_pds(nc, NC_PDS(slot), data, PDS_DOUTA_UNITS, pds_douta_code,
            sizeof(pds_douta_code) / 4);
    memcpy(douta_data, data, sizeof(data));
}

static void put_douta(uint8_t *nc, unsigned slot, uint32_t result_va) {
    put_douta_temps(nc, slot, result_va, GPU_TEMPS);
}

// The store program: pa0 and pa1 into words 0 and 1, the run's pattern into 2-15, the marker.
static unsigned put_store_program(uint64_t *code, unsigned run) {
    unsigned n = 0;
    code[n++] = USSE_SMLSI;
    code[n++] = usse_mov_pa(0, 2);
    code[n++] = usse_mov_pa(2, 0);
    code[n++] = usse_sta32(0, 0, 2);
    code[n++] = usse_mov_pa(2, 1);
    code[n++] = usse_sta32(0, 1, 2);
    for (unsigned w = 2; w < GPU_STORES; w++) {
        code[n++] = usse_limm(2, gpu_pattern(run, w));
        code[n++] = usse_sta32(0, w, 2);
    }
    code[n++] = USSE_IDF;
    code[n++] = USSE_WDF;
    code[n++] = usse_limm(1, GPU_DONE);
    code[n++] = usse_sta32(0, 31, 1);
    code[n++] = USSE_IDF;
    code[n++] = USSE_WDF;
    code[n++] = USSE_PHAS_END;
    code[n++] = USSE_NOP_END;
    return n;
}

static int submit_checked(SgxdOp *ops, unsigned count, const char *what) {
    const int done = sgxdSubmit(ops, count);
    if (done != (int)count) {
        LOG("%s: submit stopped at op %d of %u (op %u reg %05x value %08x)\n", what, done, count,
            done >= 0 && done < (int)count ? ops[done].op : 0, done >= 0 && done < (int)count ? ops[done].reg : 0,
            done >= 0 && done < (int)count ? ops[done].value : 0);
        return -1;
    }
    return 0;
}

#define W(r, v) {SGXD_OP_WRITE, (r), (v), 0, 0}
#define P(r, m, v, t) {SGXD_OP_POLL, (r), (v), (m), (t)}
#define R(r) {SGXD_OP_READ, (r), 0, 0, 0}

typedef SgxdSessionAddresses InitAddresses;

static int init_script(const InitAddresses *a) {
    char failed[160];
    if (sgxd_session_init_script(a, failed, sizeof(failed)) < 0) {
        LOG("%s\n", failed);
        return -1;
    }
    return 0;
}

static int slc(int inval) {
    char failed[160];
    if (sgxd_session_slc(inval, failed, sizeof(failed)) < 0) {
        LOG("%s\n", failed);
        return -1;
    }
    return 0;
}

typedef struct GpuRun {
    const char *name;
    int kicked;
    uint32_t elapsed_us, settle_us;
    uint32_t words[GPU_WORDS];
    uint32_t stale;        // 02b: words differing from the final ones before the invalidate
    uint32_t landed;       // stores in memory 5 ms after the kick, before any SLC flush
    uint32_t dirty_ok;     // 02b: the dirty CPU words beside the block kept their values
    uint32_t regs[12];
} GpuRun;

static const uint32_t run_regs[12] = {
    0x012c, 0x812c, 0x0118, 0x0c04, 0x0c08, 0x4c04, 0x4c08, 0x8c04, 0x8c08, 0x8b10, 0x8ebc, 0x8b2c,
};
static const char *const run_reg_names[12] = {
    "event_status", "event_status_core0", "event_status2", "bif_int_stat", "bif_fault",
    "master_bif_int_stat", "master_bif_fault", "core0_bif_int_stat", "core0_bif_fault",
    "core0_serv_event", "core0_mem_write", "core0_pds_pc",
};

// A result block as the CPU sees it: uncached directly, cached after an invalidate of its lines.
static void read_block(volatile uint32_t *block, int cached, uint32_t *out) {
    if (cached) {
        sgxdCacheRange((const void *)block, GPU_WORDS * 4, SGXD_CACHE_INVALIDATE);
    }
    for (unsigned n = 0; n < GPU_WORDS; n++) {
        out[n] = block[n];
    }
}

// Kick core 0's event program at pds (a GPU address), then wait for the marker with an SLC flush
// before every look, and keep reading until the block holds still (the marker can
// land before earlier stores).
//
// First, 5 ms after the kick and before any SLC flush: in cacheable memory one look without an
// invalidate, whose words that differ from the stores come from the CPU's stale lines
// (run->stale; a CACHECONSISTENT page shows whether the GPU's writes reach them); then, in either
// memory, how many of the 15 known stores are in memory already (run->landed): what the SLC let
// past without a flush. With BYP_CC set (the start sequence's 0x4d04 = 0x04000000) a
// CACHECONSISTENT page is expected to bypass the SLC (docs/sgxd/memory.md).
static int kick_and_collect(GpuRun *run, uint32_t pds, volatile uint32_t *block, int cached) {
    SgxdOp kick[3] = {W(0x8a68, pds), W(0x8a6c, PDS_DOUTA_UNITS), W(0x8ac0, 1)};
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    if (submit_checked(kick, 3, "kick") < 0) {
        return -1;
    }
    run->kicked = 1;
    sceKernelDelayThread(5000);
    if (cached) {
        run->stale = block[31] != GPU_DONE;
        for (unsigned w = 2; w < GPU_STORES; w++) {
            run->stale += block[w] != gpu_pattern(0, w);
        }
        sgxdCacheRange((const void *)block, GPU_WORDS * 4, SGXD_CACHE_INVALIDATE);
    }
    run->landed = block[31] == GPU_DONE;
    for (unsigned w = 2; w < GPU_STORES; w++) {
        run->landed += block[w] == gpu_pattern(0, w);
    }
    uint64_t now;
    int done = 0;
    do {
        if (slc(0) < 0) {
            return -1;
        }
        now = sceKernelGetProcessTimeWide();
        if (cached) {
            sgxdCacheRange((const void *)block, GPU_WORDS * 4, SGXD_CACHE_INVALIDATE);
        }
        done = block[31] == GPU_DONE;
    } while (!done && now - t0 < 500000);
    run->elapsed_us = done ? (uint32_t)(now - t0) : 0xffffffffu;
    read_block(block, cached, run->words);
    uint64_t changed = now;
    const uint64_t limit = now + 100000;
    unsigned same = 0;
    while (same < 5 || sceKernelGetProcessTimeWide() < changed + 2000) {
        if (sceKernelGetProcessTimeWide() >= limit) {
            break;
        }
        uint32_t words[GPU_WORDS];
        slc(0);
        read_block(block, cached, words);
        if (memcmp(words, run->words, sizeof(words)) == 0) {
            same++;
        } else {
            memcpy(run->words, words, sizeof(words));
            changed = sceKernelGetProcessTimeWide();
            same = 0;
        }
    }
    run->settle_us = (uint32_t)(changed - now);
    slc(1);
    SgxdOp regs[12];
    for (unsigned n = 0; n < 12; n++) {
        regs[n] = (SgxdOp)R(run_regs[n]);
    }
    sgxdSubmit(regs, 12);
    for (unsigned n = 0; n < 12; n++) {
        run->regs[n] = regs[n].value;
    }
    LOG("%s: %u us, settled %u us later, %u of 15 in memory before a flush, %u stale in the CPU's lines; ev %08x ev0 %08x ev2 %08x bif %08x/%08x master %08x/%08x "
        "core0 %08x/%08x serv %08x memwr %08x pdspc %08x\n",
        run->name, run->elapsed_us, run->settle_us, run->landed, run->stale, run->regs[0], run->regs[1], run->regs[2],
        run->regs[3], run->regs[4], run->regs[5], run->regs[6], run->regs[7], run->regs[8],
        run->regs[9], run->regs[10], run->regs[11]);
    for (unsigned n = 0; n < GPU_WORDS; n += 8) {
        LOG("%s words %2u: %08x %08x %08x %08x %08x %08x %08x %08x\n", run->name, n, run->words[n],
            run->words[n + 1], run->words[n + 2], run->words[n + 3], run->words[n + 4],
            run->words[n + 5], run->words[n + 6], run->words[n + 7]);
    }
    return done ? 0 : -1;
}

static int check_run(const GpuRun *run, unsigned pattern_run) {
    int bad = !run->kicked || run->words[31] != GPU_DONE;
    for (unsigned w = 2; w < GPU_STORES; w++) {
        bad |= run->words[w] != gpu_pattern(pattern_run, w);
    }
    for (unsigned w = GPU_STORES; w < 31; w++) {
        bad |= run->words[w] != GPU_FILL;
    }
    // The BIF faults: 0x0c08, 0x4c08, 0x8c08.
    return bad || run->regs[4] || run->regs[6] || run->regs[8];
}

static int json_run(char *json, int len, int size, const GpuRun *run, int ok) {
    len += snprintf(json + len, size - len, " \"%s\": {\"result\": \"%s\", \"elapsed_us\": %d, "
                    "\"settle_us\": %u, \"landed_before_flush\": %u, \"stale_before_invalidate\": %u, "
                    "\"dirty_kept\": %u,\n  \"words\": [",
                    run->name, ok ? "PASS" : "FAIL", (int)run->elapsed_us, run->settle_us, run->landed, run->stale,
                    run->dirty_ok);
    for (unsigned n = 0; n < GPU_WORDS; n++) {
        len += snprintf(json + len, size - len, "%s\"0x%08x\"", n ? ", " : "", run->words[n]);
    }
    len += snprintf(json + len, size - len, "],\n  \"regs\": {");
    for (unsigned n = 0; n < 12; n++) {
        len += snprintf(json + len, size - len, "%s\"%s\": \"0x%08x\"", n ? ", " : "",
                        run_reg_names[n], run->regs[n]);
    }
    return len + snprintf(json + len, size - len, "}},\n");
}

static int test_gpu_write(void) {
    static char json[8192];
    int len = 0, failures = 0;
    SgxdInfo info = {.size = sizeof(info)};
    GpuRun nc_run = {.name = "02"}, plain_run = {.name = "02b-plain"}, cc_run = {.name = "02b-cacheconsistent"};
    int map_nc = 1, map_plain = 1, map_cc = 1, neg_kernel = 0, neg_twice = 0, neg_write = 0;
    int neg_text = 0, map_text_ro = 1, unmap_text = 1;
    int unmapped = 1, released = 1, init = -1;

    const SceUID nc_block = sceKernelAllocMemBlock("sgxd_test_nc", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                                   NC_SIZE, NULL);
    const SceUID cached_block = sceKernelAllocMemBlock("sgxd_test_cached", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW,
                                                       CACHED_SIZE, NULL);
    void *nc_base = NULL, *cached_base = NULL;
    if (nc_block < 0 || cached_block < 0 || sceKernelGetMemBlockBase(nc_block, &nc_base) < 0 ||
        sceKernelGetMemBlockBase(cached_block, &cached_base) < 0) {
        LOG("02: memory 0x%08x 0x%08x\n", (unsigned)nc_block, (unsigned)cached_block);
        return 1;
    }
    uint8_t *nc = nc_base;
    volatile uint32_t *cached = cached_base;

    memset(nc, 0, NC_SIZE);
    put_douta(nc, 2, CACHED_GPU_VA + CACHED_RESULT);
    put_douta(nc, 3, CACHED_GPU_VA + 0x1000 + CACHED_RESULT);
    put_douta(nc, 0, NC_GPU_VA + NC_RESULT);
    const uint32_t bank0_data[4] = {13u | GPU_TEMPS << 24, 1, 0, 0};
    put_pds(nc, NC_PDS(1), bank0_data, PDS_DOUTU_UNITS, pds_doutu_code, sizeof(pds_doutu_code) / 4);
    uint64_t code[64];
    const unsigned words = put_store_program(code, 0);
    memcpy(nc + NC_CODE, code, words * 8);
    const uint64_t end[2] = {USSE_PHAS_END, USSE_NOP_END};
    memcpy(nc + NC_CODE_END, end, sizeof(end));
    for (unsigned n = 0; n < GPU_WORDS; n++) {
        ((uint32_t *)(nc + NC_RESULT))[n] = GPU_FILL;
    }
    *(uint32_t *)(nc + NC_KICKER) = GPU_KICKER_VALUE;

    const int taken = sgxdTake(&info);
    LOG("02 take -> %d: step %u status %08x core id %08x, %u us in the kernel\n", taken, info.step,
        (unsigned)info.step_status, info.core_id, info.take_us);
    if (taken < 0) {
        return 1;
    }
    const uint64_t map_t0 = sceKernelGetProcessTimeWide();
    map_nc = sgxdMap(NC_GPU_VA, nc_base, NC_SIZE, 0);
    const uint32_t map_us = (uint32_t)(sceKernelGetProcessTimeWide() - map_t0);
    map_plain = sgxdMap(CACHED_GPU_VA, cached_base, 0x1000, 0);
    map_cc = sgxdMap(CACHED_GPU_VA + 0x1000, (uint8_t *)cached_base + 0x1000, 0x1000,
                     SGXD_MAP_CACHECONSISTENT);
    // What sgxd refuses: a kernel address, a page mapped twice, a write to the directory.
    neg_kernel = sgxdMap(0x00400000u, (const void *)0x00100000u, 0x1000, 0);
    neg_twice = sgxdMap(NC_GPU_VA, nc_base, 0x1000, 0);
    SgxdOp dir_write[1] = {W(0x0c84, 0)};
    neg_write = sgxdSubmit(dir_write, 1);
    // The app's own code page: mapped writable it is refused (the app cannot write it), mapped
    // READONLY it is accepted.
    const void *text = (const void *)((uintptr_t)&test_gpu_write & ~(uintptr_t)0xfff);
    neg_text = sgxdMap(0x00500000u, text, 0x1000, 0);
    map_text_ro = sgxdMap(0x00500000u, text, 0x1000, SGXD_MAP_READONLY);
    unmap_text = map_text_ro == 0 ? sgxdUnmap(0x00500000u, 0x1000) : map_text_ro;
    LOG("02 map nc %08x (%u us for 16 pages) plain %08x cc %08x; refused: kernel va %08x, twice %08x, directory write "
        "stops at op %d, code page writable %08x; code page read-only %08x, unmap %08x\n",
        (unsigned)map_nc, map_us, (unsigned)map_plain, (unsigned)map_cc, (unsigned)neg_kernel,
        (unsigned)neg_twice, neg_write, (unsigned)neg_text, (unsigned)map_text_ro, (unsigned)unmap_text);
    failures += map_nc != 0 || map_plain != 0 || map_cc != 0 || neg_kernel >= 0 || neg_twice >= 0 ||
                neg_write != 0 || neg_text >= 0 || map_text_ro != 0 || unmap_text != 0;

    if (map_nc == 0 && map_plain == 0 && map_cc == 0) {
        InitAddresses addresses = {
            .kicker = NC_GPU_VA + NC_KICKER,
            .bank0_pds = NC_GPU_VA + NC_PDS(1), .bank0_units = PDS_DOUTU_UNITS,
            .core0_pds = NC_GPU_VA + NC_PDS(0), .core0_units = PDS_DOUTA_UNITS,
        };
        for (unsigned base = 0; base < 16; base++) {
            addresses.code[base] = NC_GPU_VA + (base == 15 ? NC_CODE : NC_CODE_END);
        }
        init = slc(1) < 0 ? -1 : init_script(&addresses);
        failures += init < 0;
        if (init == 0) {
            failures += kick_and_collect(&nc_run, NC_GPU_VA + NC_PDS(0),
                                         (volatile uint32_t *)(nc + NC_RESULT), 0) < 0;
            const int nc_bad = check_run(&nc_run, 0);
            failures += nc_bad;
            len += json_run(json + len, 0, sizeof(json) - len, &nc_run, !nc_bad);

            // 02b: cached, one page plain and one CACHECONSISTENT. The CPU fills each page and
            // cleans it, so memory holds the fill; then dirties the lines beside the block and
            // keeps them dirty through the kick.
            GpuRun *runs[2] = {&plain_run, &cc_run};
            for (unsigned page = 0; page < 2; page++) {
                volatile uint32_t *p = cached + page * 0x400;
                for (unsigned n = 0; n < 0x400; n++) {
                    p[n] = n >= CACHED_RESULT / 4 && n < CACHED_RESULT / 4 + GPU_WORDS
                               ? GPU_FILL : 0xc0de0000u | n;
                }
                sgxdCacheRange((const void *)p, 0x1000, SGXD_CACHE_CLEAN);
                for (unsigned n = 0; n < 0x200; n++) {
                    if (n < CACHED_RESULT / 4 || n >= CACHED_RESULT / 4 + GPU_WORDS) {
                        p[n] = 0xc1de0000u | n;
                    }
                }
                volatile uint32_t *block = p + CACHED_RESULT / 4;
                failures += kick_and_collect(runs[page], NC_GPU_VA + NC_PDS(2 + page), block, 1) < 0;
                const int bad = check_run(runs[page], 0);
                unsigned kept = 1;
                for (unsigned n = 0; n < 0x200; n++) {
                    if (n < CACHED_RESULT / 4 || n >= CACHED_RESULT / 4 + GPU_WORDS) {
                        kept &= p[n] == (0xc1de0000u | n);
                    }
                }
                runs[page]->dirty_ok = kept;
                failures += bad || !kept;
                len += json_run(json + len, 0, sizeof(json) - len, runs[page], !bad && kept);
            }
        }
    }
    unmapped = sgxdUnmap(NC_GPU_VA, NC_SIZE) | sgxdUnmap(CACHED_GPU_VA, 0x2000);
    released = sgxdRelease();
    failures += unmapped != 0 || released != 0;
    LOG("02 unmap %08x release %d\n", (unsigned)unmapped, released);

    len += snprintf(json + len, sizeof(json) - len,
                    " \"take\": %d, \"take_us\": %u, \"map_nc_us\": %u, \"map\": [\"0x%08x\", \"0x%08x\", \"0x%08x\"], "
                    "\"refused\": {\"kernel_va\": \"0x%08x\", \"twice\": \"0x%08x\", "
                    "\"directory_write_done\": %d, \"code_page_writable\": \"0x%08x\"}, "
                    "\"code_page_readonly\": \"0x%08x\",\n \"init\": %d, \"douta_data\": [",
                    taken, info.take_us, map_us, (unsigned)map_nc, (unsigned)map_plain, (unsigned)map_cc,
                    (unsigned)neg_kernel, (unsigned)neg_twice, neg_write, (unsigned)neg_text,
                    (unsigned)map_text_ro, init);
    for (unsigned n = 0; n < 12; n++) {
        len += snprintf(json + len, sizeof(json) - len, "%s\"0x%08x\"", n ? ", " : "", douta_data[n]);
    }
    len += snprintf(json + len, sizeof(json) - len,
                    "],\n \"unmap\": \"0x%08x\", \"release\": %d, \"result\": \"%s\"}\n",
                    (unsigned)unmapped, released, failures ? "FAIL" : "PASS");
    char head[] = "{\"test\": \"02-gpu-write\",\n";
    char path[96];
    out_path(path, sizeof(path), "02-gpu-write.json");
    const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
    if (fd >= 0) {
        sceIoWrite(fd, head, strlen(head));
        sceIoWrite(fd, json, len);
        sceIoClose(fd);
    }
    sceKernelFreeMemBlock(cached_block);
    sceKernelFreeMemBlock(nc_block);
    LOG("02-gpu-write %s\n", failures ? "FAIL" : "PASS");
    return failures;
}

// ---------------------------------------------------------------------------- test 13

#define MK_ENTRIES 8u        // log slots, by run number modulo
#define MK_ENTRY_WORDS 4u    // pa0 (IR0), pa1 (IR1), the register read back, EVENT_STATUS
#define MK_LOG_WORDS (4u + MK_ENTRIES * MK_ENTRY_WORDS)   // [0] runs, [1] mode, then the slots
// TE's float constant (the recipe writes 0x358637bd): a full 32-bit register no other test
// touches, and the next recipe's TA kick writes it again.
#define MK_REG 0x0240u
#define MK_VALUE 0x12340000u
#define MK_EVENT_STATUS 0x012cu
#define MK_EVENT_HOST_CLEAR 0x0134u
#define MK_SW_EVENT 0x4000u
#define MK_FILL 0x8000u      // the PTLA fill's 64x64 8888 surface in the uncached buffer

// The probe program (pa0 IR0, pa1 IR1, pa2 the log); returns its length, or 0.
static unsigned put_mk_program(uint64_t *code, unsigned cap) {
    struct sgx_usse_asm as, *a = &as;
    sgx_usse_init(a, code, cap, 0);
    sgx_usse_smlsi(a);
    sgx_usse_lock(a, 7);
    sgx_usse_mov(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_PA(2));
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(1), USSE_R(0), USSE_IMM(0), 0);
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(8), USSE_R(0), USSE_IMM(1), 0);
    sgx_usse_wdf(a, 0);
    // r3 = the run's slot, less the header's 16 bytes: (runs % MK_ENTRIES) * 16 + log.
    sgx_usse_bitop(a, SGX_USSE_ALWAYS, SGX_USSE_AND, USSE_R(2), USSE_R(1),
                   USSE_IMM(MK_ENTRIES - 1), 0);
    sgx_usse_imad2(a, SGX_USSE_ALWAYS, USSE_R(3), USSE_R(2), USSE_IMM(16), USSE_R(0));
    sgx_usse_mov(a, SGX_USSE_ALWAYS, USSE_R(10), USSE_PA(0));
    sgx_usse_mov(a, SGX_USSE_ALWAYS, USSE_R(11), USSE_PA(1));
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(3), USSE_IMM(4), USSE_R(10));
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(3), USSE_IMM(5), USSE_R(11));
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(4), MK_REG >> 2);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(5), MK_VALUE);
    sgx_usse_bitop(a, SGX_USSE_ALWAYS, SGX_USSE_OR, USSE_R(5), USSE_R(5), USSE_R(1), 0);
    sgx_usse_str_indirect(a, USSE_R(4), USSE_R(5));
    sgx_usse_ldr_indirect(a, USSE_R(6), USSE_R(4), 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(3), USSE_IMM(6), USSE_R(6));
    // Mode bit 0: SW_EVENT into bank 0's EVENT_STATUS (p0), then EVENT_STATUS logged.
    sgx_usse_test(a, SGX_USSE_ALWAYS, 0, SGX_USSE_TST_AND, SGX_USSE_NONZERO, USSE_R(8),
                  USSE_IMM(1));
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), MK_SW_EVENT);
    sgx_usse_str(a, SGX_USSE_P0, MK_EVENT_STATUS, USSE_R(9));
    sgx_usse_ldr(a, USSE_R(7), MK_EVENT_STATUS, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(3), USSE_IMM(7), USSE_R(7));
    // The run counted once its slot is written.
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_add(a, SGX_USSE_ALWAYS, USSE_R(1), USSE_R(1), USSE_IMM(1));
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_IMM(0), USSE_R(1));
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_release(a, 7);
    sgx_usse_phas_end(a);
    sgx_usse_nop_end(a);
    if (sgx_usse_finish(a) < 0) {
        LOG("13 program: assembler error %d at %u\n", a->err, a->len);
        return 0;
    }
    return a->len;
}

static uint32_t mk_wait(volatile uint32_t *log, uint32_t runs, uint32_t timeout_us) {
    const uint64_t t0 = sceKernelGetProcessTimeWide();
    for (;;) {
        if (slc(0) < 0) {
            return 0xffffffffu;
        }
        const uint64_t now = sceKernelGetProcessTimeWide();
        if (log[0] >= runs) {
            return (uint32_t)(now - t0);
        }
        if (now - t0 > timeout_us) {
            return 0xffffffffu;
        }
    }
}

static uint32_t mk_read(uint32_t reg) {
    SgxdOp op = R(reg);
    return sgxdSubmit(&op, 1) == 1 ? op.value : 0xdeadbeefu;
}

// The probe's memory in the uncached buffer: core 0's event program (pa2 the log, DOUTU code base
// `code_base`), bank 0's, the program, the end program and the kicker word. Returns the
// program's length, or 0.
static unsigned mk_stage(uint8_t *nc, unsigned code_base) {
    memset(nc, 0, NC_SIZE);
    put_douta(nc, 0, NC_GPU_VA + NC_RESULT);
    douta_data[8] = code_base | GPU_TEMPS << 24;
    memcpy(nc + NC_PDS(0), douta_data, sizeof(douta_data));
    const uint32_t bank0_data[4] = {13u | GPU_TEMPS << 24, 1, 0, 0};
    put_pds(nc, NC_PDS(1), bank0_data, PDS_DOUTU_UNITS, pds_doutu_code, sizeof(pds_doutu_code) / 4);
    uint64_t code[96];
    const unsigned words = put_mk_program(code, 96);
    if (!words) {
        return 0;
    }
    memcpy(nc + NC_CODE, code, words * 8);
    const uint64_t end[2] = {USSE_PHAS_END, USSE_NOP_END};
    memcpy(nc + NC_CODE_END, end, sizeof(end));
    *(uint32_t *)(nc + NC_KICKER) = GPU_KICKER_VALUE;
    char path[96];
    out_path(path, sizeof(path), "13-mk-program.bin");
    write_file(path, code, words * 8);
    return words;
}

static int test_mk(void) {
    static char json[4096];
    int len = 0, failures = 0;
    SgxdInfo info = {.size = sizeof(info)};
    const SceUID nc_block = sceKernelAllocMemBlock("sgxd_test_nc", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                                   NC_SIZE, NULL);
    void *nc_base = NULL;
    if (nc_block < 0 || sceKernelGetMemBlockBase(nc_block, &nc_base) < 0) {
        LOG("13: memory 0x%08x\n", (unsigned)nc_block);
        return 1;
    }
    uint8_t *nc = nc_base;
    volatile uint32_t *log = (volatile uint32_t *)(nc + NC_RESULT);
    const unsigned words = mk_stage(nc, 15);
    if (!words) {
        return 1;
    }
    char path[96];

    const int taken = sgxdTake(&info);
    LOG("13 take -> %d, program %u instructions\n", taken, words);
    if (taken < 0) {
        return 1;
    }
    const int mapped = sgxdMap(NC_GPU_VA, nc_base, NC_SIZE, 0);
    int init = -1;
    uint32_t a_us = 0, b_us = 0, c_us = 0, runs_after_fill = 0, fill = 0;
    uint32_t reg_a = 0, reg_b = 0, status_b = 0, status_cleared = 0, status_before = 0;
    if (mapped == 0) {
        InitAddresses addresses = {
            .kicker = NC_GPU_VA + NC_KICKER,
            .bank0_pds = NC_GPU_VA + NC_PDS(1), .bank0_units = PDS_DOUTU_UNITS,
            .core0_pds = NC_GPU_VA + NC_PDS(0), .core0_units = PDS_DOUTA_UNITS,
        };
        for (unsigned base = 0; base < 16; base++) {
            addresses.code[base] = NC_GPU_VA + (base == 15 ? NC_CODE : NC_CODE_END);
        }
        init = slc(1) < 0 ? -1 : init_script(&addresses);
    }
    failures += mapped != 0 || init != 0;
    if (init == 0) {
        SgxdOp kick[3] = {W(0x8a68, NC_GPU_VA + NC_PDS(0)), W(0x8a6c, PDS_DOUTA_UNITS), W(0x8ac0, 1)};
        status_before = mk_read(MK_EVENT_STATUS);
        failures += submit_checked(kick, 3, "13a kick") < 0;
        a_us = mk_wait(log, 1, 500000);
        reg_a = mk_read(MK_REG);
        // 13b: the same with SW_EVENT stored into EVENT_STATUS. The run before left the log's
        // line in the SLC: the CPU's mode word reaches the GPU only through an invalidate.
        log[1] = 1;
        failures += slc(1) < 0;
        failures += submit_checked(kick, 3, "13b kick") < 0;
        b_us = mk_wait(log, 2, 500000);
        reg_b = mk_read(MK_REG);
        status_b = mk_read(MK_EVENT_STATUS);
        SgxdOp clear[1] = {W(MK_EVENT_HOST_CLEAR, MK_SW_EVENT)};
        failures += submit_checked(clear, 1, "13b clear") < 0;
        status_cleared = mk_read(MK_EVENT_STATUS);
        log[1] = 0;
        failures += slc(1) < 0;
        const uint32_t before = log[0];
        const SgxdPtlaFill f = {NC_GPU_VA + MK_FILL, SGXD_PTLA_8888, 64 * 4, 64, 64, 0, 0xff00ff00u};
        char failed[160] = "";
        uint32_t fill_us = 0;
        fill = sgxd_ptla_fill(&f, &fill_us, failed, sizeof(failed)) == 0;
        if (!fill) {
            LOG("13c fill: %s\n", failed);
        }
        c_us = mk_wait(log, before + 1, 200000);
        runs_after_fill = log[0] - before;
        const uint32_t ok_a = log[0] >= 1 && log[4 + 2] == MK_VALUE && reg_a == MK_VALUE;
        const uint32_t ok_b = log[0] >= 2 && log[8 + 2] == (MK_VALUE | 1) && reg_b == (MK_VALUE | 1) &&
                              (log[8 + 3] & MK_SW_EVENT) && (status_b & MK_SW_EVENT) &&
                              !(status_cleared & MK_SW_EVENT);
        failures += !ok_a + !ok_b + !fill;
        LOG("13a kick: %u us, IR0 %08x IR1 %08x, read back %08x, CPU reads %08x -> %s\n", a_us,
            log[4], log[5], log[6], reg_a, ok_a ? "PASS" : "FAIL");
        LOG("13b SW_EVENT: %u us, IR0 %08x IR1 %08x, read back %08x, CPU reads %08x; EVENT_STATUS "
            "before %08x, in the program %08x, after %08x, cleared %08x -> %s\n", b_us, log[8],
            log[9], log[10], reg_b, status_before, log[11], status_b, status_cleared,
            ok_b ? "PASS" : "FAIL");
        LOG("13c PTLA fill %s in %u us: %u run(s) after it (%u us)\n", fill ? "done" : "FAILED",
            fill_us, runs_after_fill, c_us);
    }
    for (unsigned n = 0; n < MK_LOG_WORDS; n += 4) {
        LOG("13 log %2u: %08x %08x %08x %08x\n", n, log[n], log[n + 1], log[n + 2], log[n + 3]);
    }
    const int unmapped = sgxdUnmap(NC_GPU_VA, NC_SIZE);
    const int released = sgxdRelease();
    failures += unmapped != 0 || released != 0;
    len += snprintf(json + len, sizeof(json) - len,
                    "{\"test\": \"13-mk\", \"take\": %d, \"map\": \"0x%08x\", \"init\": %d, "
                    "\"program_words\": %u,\n \"kick_us\": %d, \"reg_after_kick\": \"0x%08x\",\n "
                    "\"sw_event_kick_us\": %d, \"reg_after_sw_event\": \"0x%08x\", "
                    "\"event_status\": {\"before\": \"0x%08x\", \"after\": \"0x%08x\", "
                    "\"cleared\": \"0x%08x\"},\n \"fill\": %u, \"runs_after_fill\": %u, "
                    "\"fill_run_us\": %d,\n \"log\": [",
                    taken, (unsigned)mapped, init, words, (int)a_us, reg_a, (int)b_us, reg_b,
                    status_before, status_b, status_cleared, fill, runs_after_fill, (int)c_us);
    for (unsigned n = 0; n < MK_LOG_WORDS; n++) {
        len += snprintf(json + len, sizeof(json) - len, "%s\"0x%08x\"", n ? ", " : "", log[n]);
    }
    len += snprintf(json + len, sizeof(json) - len,
                    "],\n \"unmap\": \"0x%08x\", \"release\": %d, \"result\": \"%s\"}\n",
                    (unsigned)unmapped, released, failures ? "FAIL" : "PASS");
    out_path(path, sizeof(path), "13-mk.json");
    write_file(path, json, (unsigned)len);
    sceKernelFreeMemBlock(nc_block);
    LOG("13-mk %s\n", failures ? "FAIL" : "PASS");
    return failures;
}

// ---------------------------------------------------------------------------- test 15

enum { UT_BARE, UT_ALU_DEP, UT_ALU_INDEP, UT_LD_CC, UT_LD_PLAIN, UT_LDR, UT_STR, UT_ST_CC,
       UT_LD5, UT_BODIES };
static const char *const ut_names[UT_BODIES] = {
    "bare", "alu8_dependent", "alu8_independent", "lda_wdf_cc", "lda_wdf_plain", "ldr_wdf",
    "str", "sta_cc", "lda5_wdf_cc",
};
#define UT_CC 0x8000u      // in the uncached buffer: from here CACHECONSISTENT (test 15 maps it so)

// The timing program: r0 the log (pa2), whose word 1 picks the body and word 2 the count, r2 the
// plain word's address, r3 the CACHECONSISTENT word's; the marker (GPU_DONE) into log[0] at the
// end. One program for every body: the code is written once (test 13's form).
static int put_timing_program(uint64_t *code, unsigned cap) {
    struct sgx_usse_asm as, *a = &as;
    sgx_usse_init(a, code, cap, 0);
    int loops[UT_BODIES], done = 0;
    for (unsigned b = 0; b < UT_BODIES; b++) {
        loops[b] = sgx_usse_label(a);
    }
    done = sgx_usse_label(a);
    // log[3] = stage, fenced: where a run that never ends stopped.
    #define UT_STAGE(v)                                                                      \
        do {                                                                                 \
            sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), (v));                               \
            sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_IMM(3), USSE_R(9));              \
            sgx_usse_idf(a, 0);                                                              \
            sgx_usse_wdf(a, 0);                                                              \
        } while (0)
    sgx_usse_smlsi(a);
    sgx_usse_mov(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_PA(2));
    UT_STAGE(1);
    // The mode and the count from the CACHECONSISTENT page (test 19's note).
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(3), NC_GPU_VA + UT_CC);
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(7), USSE_R(3), USSE_IMM(17), 0);
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(1), USSE_R(3), USSE_IMM(18), 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_IMM(4), USSE_R(7));
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_IMM(5), USSE_R(1));
    UT_STAGE(2);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(2), NC_GPU_VA + 0x7000u);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(3), NC_GPU_VA + UT_CC);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(4), 0xffffffffu);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(5), 0x358637bdu);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(6), MK_REG >> 2);
    for (unsigned b = 0; b < UT_BODIES; b++) {
        sgx_usse_test(a, SGX_USSE_ALWAYS, 0, SGX_USSE_TST_XOR, SGX_USSE_ZERO, USSE_R(7), USSE_IMM(b));
        sgx_usse_ba(a, SGX_USSE_P0, loops[b]);
    }
    sgx_usse_ba(a, SGX_USSE_ALWAYS, done);
    for (unsigned b = 0; b < UT_BODIES; b++) {
        const int body = sgx_usse_label(a);
        sgx_usse_bind(a, loops[b]);
        UT_STAGE(0x100u + b);
        sgx_usse_bind(a, body);
        switch (b) {
        case UT_ALU_DEP:
            for (unsigned k = 0; k < 8; k++) {
                sgx_usse_imad2(a, SGX_USSE_ALWAYS, USSE_R(8), USSE_R(8), USSE_IMM(1), USSE_IMM(1));
            }
            break;
        case UT_ALU_INDEP:
            for (unsigned k = 0; k < 8; k++) {
                sgx_usse_imad2(a, SGX_USSE_ALWAYS, USSE_R(8 + k), USSE_R(8 + k), USSE_IMM(1),
                               USSE_IMM(1));
            }
            break;
        case UT_LD_CC:
            sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(8), USSE_R(3), USSE_IMM(0), 0);
            sgx_usse_wdf(a, 0);
            break;
        case UT_LD_PLAIN:
            sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(8), USSE_R(2), USSE_IMM(0), 0);
            sgx_usse_wdf(a, 0);
            break;
        case UT_LDR:
            sgx_usse_ldr_indirect(a, USSE_R(8), USSE_R(6), 0);
            sgx_usse_wdf(a, 0);
            break;
        case UT_STR:
            sgx_usse_str_indirect(a, USSE_R(6), USSE_R(5));
            break;
        case UT_ST_CC:
            sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(3), USSE_IMM(1), USSE_R(1));
            break;
        case UT_LD5:
            for (unsigned k = 0; k < 5; k++) {
                sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(8 + k), USSE_R(3), USSE_IMM(k), 0);
            }
            sgx_usse_wdf(a, 0);
            break;
        default:
            break;
        }
        // The walker's countdown (sgxd_mk.c's SLC wait): r1 += 0xffffffff, until zero.
        sgx_usse_imad2(a, SGX_USSE_ALWAYS, USSE_R(1), USSE_R(1), USSE_IMM(1), USSE_R(4));
        sgx_usse_test(a, SGX_USSE_ALWAYS, 1, SGX_USSE_TST_AND, SGX_USSE_ZERO, USSE_R(1), USSE_R(1));
        sgx_usse_ba(a, SGX_USSE_NOT_P1, body);
        UT_STAGE(0x200u + b);
        sgx_usse_ba(a, SGX_USSE_ALWAYS, done);
    }
    #undef UT_STAGE
    sgx_usse_bind(a, done);
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    // The marker in the CACHECONSISTENT page (word 16 there): the CPU sees it with no flush.
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), GPU_DONE);
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(3), USSE_IMM(16), USSE_R(9));
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_phas_end(a);
    sgx_usse_nop_end(a);
    return sgx_usse_finish(a) < 0 ? 0 : (int)a->len;
}

static int test_usse_timing(uint32_t loops) {
    static char json[2048];
    int len = 0, failures = 0;
    SgxdInfo info = {.size = sizeof(info)};
    const SceUID nc_block = sceKernelAllocMemBlock("sgxd_test_nc", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                                   NC_SIZE, NULL);
    void *nc_base = NULL;
    if (nc_block < 0 || sceKernelGetMemBlockBase(nc_block, &nc_base) < 0) {
        return 1;
    }
    uint8_t *nc = nc_base;
    volatile uint32_t *log = (volatile uint32_t *)(nc + NC_RESULT);
    volatile uint32_t *marker = (volatile uint32_t *)(nc + UT_CC) + 16;
    memset(nc, 0, NC_SIZE);
    put_douta(nc, 0, NC_GPU_VA + NC_RESULT);
    const uint32_t bank0_data[4] = {13u | GPU_TEMPS << 24, 1, 0, 0};
    put_pds(nc, NC_PDS(1), bank0_data, PDS_DOUTU_UNITS, pds_doutu_code, sizeof(pds_doutu_code) / 4);
    const uint64_t end[2] = {USSE_PHAS_END, USSE_NOP_END};
    memcpy(nc + NC_CODE_END, end, sizeof(end));
    *(uint32_t *)(nc + NC_KICKER) = GPU_KICKER_VALUE;
    uint64_t code[256];
    const int words = put_timing_program(code, 256);
    if (!words) {
        return 1;
    }
    memcpy(nc + NC_CODE, code, (unsigned)words * 8);
    char path[96];
    out_path(path, sizeof(path), "15-usse-timing.bin");
    write_file(path, code, (unsigned)words * 8);
    const int taken = sgxdTake(&info);
    if (taken < 0) {
        return 1;
    }
    const int mapped = sgxdMap(NC_GPU_VA, nc_base, UT_CC, 0) |
                       sgxdMap(NC_GPU_VA + UT_CC, nc + UT_CC, NC_SIZE - UT_CC,
                               SGXD_MAP_CACHECONSISTENT);
    InitAddresses addresses = {
        .kicker = NC_GPU_VA + NC_KICKER,
        .bank0_pds = NC_GPU_VA + NC_PDS(1), .bank0_units = PDS_DOUTU_UNITS,
        .core0_pds = NC_GPU_VA + NC_PDS(0), .core0_units = PDS_DOUTA_UNITS,
    };
    for (unsigned base = 0; base < 16; base++) {
        addresses.code[base] = NC_GPU_VA + (base == 15 ? NC_CODE : NC_CODE_END);
    }
    const int init = mapped != 0 || slc(1) < 0 ? -1 : init_script(&addresses);
    failures += init != 0;
    uint32_t us[UT_BODIES] = {0};
    const uint32_t mhz = (uint32_t)scePowerGetArmClockFrequency();
    for (unsigned body = 0; init == 0 && body < UT_BODIES; body++) {
        marker[0] = 0;
        marker[1] = body;   // words 17 and 18 of the CACHECONSISTENT page
        marker[2] = loops;
        SgxdOp kick[1] = {W(0x8ac0, 1)};
        const uint32_t c0 = azaharCyclesStart();
        failures += submit_checked(kick, 1, "15 kick") < 0;
        uint32_t c = c0;
        while (marker[0] != GPU_DONE && c - c0 < 2000000u * mhz) {
            c = azaharCycles();
        }
        us[body] = marker[0] == GPU_DONE ? (uint32_t)((c - c0) / mhz) : 0xffffffffu;
        failures += marker[0] != GPU_DONE;
        if (marker[0] != GPU_DONE) {
            SgxdOp regs[12];
            for (unsigned n = 0; n < 12; n++) {
                regs[n] = (SgxdOp)R(run_regs[n]);
            }
            sgxdSubmit(regs, 12);
            LOG("15 %s did not finish: log %08x %08x %08x stage %08x read %08x %08x; ev %08x "
                "ev0 %08x serv %08x pdspc %08x\n", ut_names[body], log[0], log[1], log[2], log[3],
                log[4], log[5], regs[0].value, regs[1].value, regs[9].value, regs[11].value);
            break;
        }
        LOG("15 %-18s %u loops: %u us, %u ns an iteration over the bare loop's\n", ut_names[body],
            loops, us[body],
            body && us[body] != 0xffffffffu && us[0] != 0xffffffffu
                ? (uint32_t)(((int64_t)us[body] - us[0]) * 1000 / (int64_t)loops) : 0);
    }
    const int unmapped = sgxdUnmap(NC_GPU_VA, NC_SIZE);
    const int released = sgxdRelease();
    failures += unmapped != 0 || released != 0;
    len += snprintf(json + len, sizeof(json) - len, "{\"test\": \"15-usse-timing\", \"loops\": %u, "
                    "\"us\": {", loops);
    for (unsigned body = 0; body < UT_BODIES; body++) {
        len += snprintf(json + len, sizeof(json) - len, "%s\"%s\": %u", body ? ", " : "",
                        ut_names[body], us[body]);
    }
    len += snprintf(json + len, sizeof(json) - len, "}, \"result\": \"%s\"}\n",
                    failures ? "FAIL" : "PASS");
    out_path(path, sizeof(path), "15-usse-timing.json");
    write_file(path, json, (unsigned)len);
    sceKernelFreeMemBlock(nc_block);
    LOG("15-usse-timing %s\n", failures ? "FAIL" : "PASS");
    return failures;
}

// ---------------------------------------------------------------------------- test 16

#define AL_WORDS 24u
static const char *const al_names[AL_WORDS] = {
    "stage", "done", "", "", "imad2_10000_minus_1", "imad2_signed_10000_minus_1",
    "imad2_12345678_minus_1", "imad2_signed_12345678_minus_1", "dec_3", "dec_3_p0",
    "p1_of_zero", "p1_of_five", "loop_dec", "loop_imad2_signed", "loop_imad2_wdf",
    "loop_imad2_bare", "load_after_temp_move", "load_after_pa_move", "start_load_1",
    "start_load_2", "", "", "", "",
};

// Stores r at log word w, then fences (the store reaches the SLC before anything after it).
static void al_store(struct sgx_usse_asm *a, unsigned w, unsigned r) {
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_IMM(w), USSE_R(r));
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
}

static void al_stage(struct sgx_usse_asm *a, uint32_t stage) {
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), stage);
    al_store(a, 0, 9);
}

static int put_alu_program(uint64_t *code, unsigned cap, int start15) {
    struct sgx_usse_asm as, *a = &as;
    sgx_usse_init(a, code, cap, 0);
    sgx_usse_smlsi(a);
    sgx_usse_mov(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_PA(2));
    if (start15) {
        sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(7), USSE_R(0), USSE_IMM(1), 0);
        sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(1), USSE_R(0), USSE_IMM(2), 0);
        sgx_usse_wdf(a, 0);
        al_store(a, 18, 7);
        al_store(a, 19, 1);
    }
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(4), 0xffffffffu);
    al_stage(a, 1);
    const uint32_t values[2] = {10000u, 0x12345678u};
    for (unsigned v = 0; v < 2; v++) {
        sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(1), values[v]);
        sgx_usse_imad2(a, SGX_USSE_ALWAYS, USSE_R(2), USSE_R(1), USSE_IMM(1), USSE_R(4));
        al_store(a, 4 + 2 * v, 2);
        sgx_usse_imad2_signed(a, SGX_USSE_ALWAYS, USSE_R(2), USSE_R(1), USSE_IMM(1), USSE_R(4));
        al_store(a, 5 + 2 * v, 2);
    }
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(5), 3);
    sgx_usse_dec_test(a, SGX_USSE_ALWAYS, 0, SGX_USSE_NONZERO, USSE_R(5), USSE_R(5), 1);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), 0);
    sgx_usse_limm(a, SGX_USSE_P0, USSE_R(9), 1);
    al_store(a, 8, 5);
    al_store(a, 9, 9);
    const uint32_t tested[2] = {0, 5};
    for (unsigned v = 0; v < 2; v++) {
        sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(1), tested[v]);
        sgx_usse_test(a, SGX_USSE_ALWAYS, 1, SGX_USSE_TST_AND, SGX_USSE_ZERO, USSE_R(1), USSE_R(1));
        sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), 0);
        sgx_usse_limm(a, SGX_USSE_P1, USSE_R(9), 1);
        al_store(a, 10 + v, 9);
    }
    for (unsigned l = 0; l < 4; l++) {
        al_stage(a, 0x100u + l);
        const int top = sgx_usse_label(a);
        sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(1), 16);
        sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(12), 0);
        sgx_usse_bind(a, top);
        sgx_usse_imad2(a, SGX_USSE_ALWAYS, USSE_R(12), USSE_R(12), USSE_IMM(1), USSE_IMM(1));
        if (l == 0) {
            sgx_usse_dec_test(a, SGX_USSE_ALWAYS, 0, SGX_USSE_NONZERO, USSE_R(1), USSE_R(1), 1);
            sgx_usse_ba(a, SGX_USSE_P0, top);
        } else {
            if (l == 2) {
                sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(8), USSE_R(0), USSE_IMM(2), 0);
                sgx_usse_wdf(a, 0);
            }
            if (l == 1) {
                sgx_usse_imad2_signed(a, SGX_USSE_ALWAYS, USSE_R(1), USSE_R(1), USSE_IMM(1),
                                      USSE_R(4));
            } else {
                sgx_usse_imad2(a, SGX_USSE_ALWAYS, USSE_R(1), USSE_R(1), USSE_IMM(1), USSE_R(4));
            }
            sgx_usse_test(a, SGX_USSE_ALWAYS, 1, SGX_USSE_TST_AND, SGX_USSE_ZERO, USSE_R(1),
                          USSE_R(1));
            sgx_usse_ba(a, SGX_USSE_NOT_P1, top);
        }
        al_store(a, 12 + l, 12);
    }
    // The hazard: the load's base written by the instruction right before it.
    al_stage(a, 0x300);
    sgx_usse_mov(a, SGX_USSE_ALWAYS, USSE_R(10), USSE_R(0));
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(11), USSE_R(10), USSE_IMM(2), 0);
    sgx_usse_wdf(a, 0);
    al_store(a, 16, 11);
    al_stage(a, 0x301);
    sgx_usse_mov(a, SGX_USSE_ALWAYS, USSE_R(10), USSE_PA(2));
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(11), USSE_R(10), USSE_IMM(2), 0);
    sgx_usse_wdf(a, 0);
    al_store(a, 17, 11);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), GPU_DONE);
    al_store(a, 1, 9);
    sgx_usse_phas_end(a);
    sgx_usse_nop_end(a);
    return sgx_usse_finish(a) < 0 ? 0 : (int)a->len;
}

static int test_alu(int start15) {
    static char json[2048];
    int len = 0, failures = 0;
    SgxdInfo info = {.size = sizeof(info)};
    const SceUID nc_block = sceKernelAllocMemBlock("sgxd_test_nc", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                                   NC_SIZE, NULL);
    void *nc_base = NULL;
    if (nc_block < 0 || sceKernelGetMemBlockBase(nc_block, &nc_base) < 0) {
        return 1;
    }
    uint8_t *nc = nc_base;
    volatile uint32_t *log = (volatile uint32_t *)(nc + NC_RESULT);
    memset(nc, 0, NC_SIZE);
    for (unsigned n = 0; n < AL_WORDS; n++) {
        log[n] = 0xeeeeeeeeu;
    }
    put_douta(nc, 0, NC_GPU_VA + NC_RESULT);
    const uint32_t bank0_data[4] = {13u | GPU_TEMPS << 24, 1, 0, 0};
    put_pds(nc, NC_PDS(1), bank0_data, PDS_DOUTU_UNITS, pds_doutu_code, sizeof(pds_doutu_code) / 4);
    const uint64_t end[2] = {USSE_PHAS_END, USSE_NOP_END};
    memcpy(nc + NC_CODE_END, end, sizeof(end));
    *(uint32_t *)(nc + NC_KICKER) = GPU_KICKER_VALUE;
    uint64_t code[256];
    const int words = put_alu_program(code, 256, start15);
    if (!words) {
        return 1;
    }
    memcpy(nc + NC_CODE, code, (unsigned)words * 8);
    char path[96];
    out_path(path, sizeof(path), "16-alu.bin");
    write_file(path, code, (unsigned)words * 8);
    if (sgxdTake(&info) < 0) {
        return 1;
    }
    const int mapped = sgxdMap(NC_GPU_VA, nc_base, NC_SIZE, 0);
    InitAddresses addresses = {
        .kicker = NC_GPU_VA + NC_KICKER,
        .bank0_pds = NC_GPU_VA + NC_PDS(1), .bank0_units = PDS_DOUTU_UNITS,
        .core0_pds = NC_GPU_VA + NC_PDS(0), .core0_units = PDS_DOUTA_UNITS,
    };
    for (unsigned base = 0; base < 16; base++) {
        addresses.code[base] = NC_GPU_VA + (base == 15 ? NC_CODE : NC_CODE_END);
    }
    const int init = mapped != 0 || slc(1) < 0 ? -1 : init_script(&addresses);
    failures += init != 0;
    uint32_t us = 0;
    if (init == 0) {
        SgxdOp kick[1] = {W(0x8ac0, 1)};
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        failures += submit_checked(kick, 1, "16 kick") < 0;
        uint64_t now = t0;
        while (log[1] != GPU_DONE && now - t0 < 1000000) {
            slc(0);
            now = sceKernelGetProcessTimeWide();
        }
        slc(0);
        us = (uint32_t)(now - t0);
        failures += log[1] != GPU_DONE;
    }
    LOG("16 program %d instructions, %s after %u us\n", words,
        log[1] == GPU_DONE ? "done" : "NOT done", us);
    if (log[1] != GPU_DONE) {
        SgxdOp regs[12];
        for (unsigned n = 0; n < 12; n++) {
            regs[n] = (SgxdOp)R(run_regs[n]);
        }
        sgxdSubmit(regs, 12);
        LOG("16 not done: bif %08x/%08x master %08x/%08x core0 %08x/%08x serv %08x pdspc %08x\n",
            regs[3].value, regs[4].value, regs[5].value, regs[6].value, regs[7].value,
            regs[8].value, regs[9].value, regs[11].value);
    }
    for (unsigned n = 0; n < 20; n++) {
        if (al_names[n][0]) {
            LOG("16 %-32s %08x\n", al_names[n], log[n]);
        }
    }
    const int unmapped = sgxdUnmap(NC_GPU_VA, NC_SIZE);
    const int released = sgxdRelease();
    failures += unmapped != 0 || released != 0;
    len += snprintf(json + len, sizeof(json) - len, "{\"test\": \"16-alu\", \"us\": %u", us);
    for (unsigned n = 0; n < 20; n++) {
        if (al_names[n][0]) {
            len += snprintf(json + len, sizeof(json) - len, ", \"%s\": \"0x%08x\"", al_names[n], log[n]);
        }
    }
    len += snprintf(json + len, sizeof(json) - len, ", \"result\": \"%s\"}\n", failures ? "FAIL" : "PASS");
    out_path(path, sizeof(path), "16-alu.json");
    write_file(path, json, (unsigned)len);
    sceKernelFreeMemBlock(nc_block);
    LOG("16-alu %s\n", failures ? "FAIL" : "PASS");
    return failures;
}

// ---------------------------------------------------------------------------- test 18

#define FT_SIZE 0x20000u
#define FT_CODE 0x8000u        // code base 15 here: up to 0x3000 instructions
#define FT_MODES 4u
#define FT_STORES 512u
#define FT_CC 0x7000u          // the marker's page, CACHECONSISTENT
#define FT_CODE_VA 0x00200000u // the code's block (main memory or CDRAM)
#define FT_CODE_SIZE 0x40000u
static const char *const ft_names[FT_MODES] = {"straight_str_imm", "loop_str", "straight_limm_str",
                                               "loop_lda_wdf"};

static int put_fetch_program(uint64_t *code, unsigned cap) {
    struct sgx_usse_asm as, *a = &as;
    sgx_usse_init(a, code, cap, 0);
    int bodies[FT_MODES];
    for (unsigned m = 0; m < FT_MODES; m++) {
        bodies[m] = sgx_usse_label(a);
    }
    const int done = sgx_usse_label(a), loop = sgx_usse_label(a);
    sgx_usse_smlsi(a);
    sgx_usse_mov(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_PA(2));
    // A fenced store first (test 15: programs that store early run).
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), 1);
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_IMM(3), USSE_R(9));
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    // The mode from the CACHECONSISTENT page (test 19's note).
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(5), NC_GPU_VA + FT_CC);
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(7), USSE_R(5), USSE_IMM(17), 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(4), 0xffffffffu);
    for (unsigned m = 0; m < FT_MODES; m++) {
        sgx_usse_test(a, SGX_USSE_ALWAYS, 0, SGX_USSE_TST_XOR, SGX_USSE_ZERO, USSE_R(7), USSE_IMM(m));
        sgx_usse_ba(a, SGX_USSE_P0, bodies[m]);
    }
    sgx_usse_ba(a, SGX_USSE_ALWAYS, done);
    sgx_usse_bind(a, bodies[0]);
    for (unsigned k = 0; k < FT_STORES; k++) {
        sgx_usse_str(a, SGX_USSE_ALWAYS, MK_REG, USSE_IMM(1));
    }
    sgx_usse_ba(a, SGX_USSE_ALWAYS, done);
    sgx_usse_bind(a, bodies[1]);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(1), FT_STORES);
    sgx_usse_bind(a, loop);
    sgx_usse_str(a, SGX_USSE_ALWAYS, MK_REG, USSE_IMM(1));
    sgx_usse_imad2(a, SGX_USSE_ALWAYS, USSE_R(1), USSE_R(1), USSE_IMM(1), USSE_R(4));
    sgx_usse_test(a, SGX_USSE_ALWAYS, 1, SGX_USSE_TST_AND, SGX_USSE_ZERO, USSE_R(1), USSE_R(1));
    sgx_usse_ba(a, SGX_USSE_NOT_P1, loop);
    sgx_usse_ba(a, SGX_USSE_ALWAYS, done);
    sgx_usse_bind(a, bodies[2]);
    for (unsigned k = 0; k < FT_STORES; k++) {
        sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), 0x358637bdu + k);
        sgx_usse_str(a, SGX_USSE_ALWAYS, MK_REG, USSE_R(9));
    }
    sgx_usse_ba(a, SGX_USSE_ALWAYS, done);
    const int loop_ld = sgx_usse_label(a);
    sgx_usse_bind(a, bodies[3]);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(1), FT_STORES);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(2), FT_CODE_VA);
    sgx_usse_bind(a, loop_ld);
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(8), USSE_R(2), USSE_IMM(0), 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_imad2(a, SGX_USSE_ALWAYS, USSE_R(1), USSE_R(1), USSE_IMM(1), USSE_R(4));
    sgx_usse_test(a, SGX_USSE_ALWAYS, 1, SGX_USSE_TST_AND, SGX_USSE_ZERO, USSE_R(1), USSE_R(1));
    sgx_usse_ba(a, SGX_USSE_NOT_P1, loop_ld);
    sgx_usse_ba(a, SGX_USSE_ALWAYS, done);
    sgx_usse_bind(a, done);
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), GPU_DONE);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(5), NC_GPU_VA + FT_CC);
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(5), USSE_IMM(0), USSE_R(9));
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_phas_end(a);
    sgx_usse_nop_end(a);
    return sgx_usse_finish(a) < 0 ? 0 : (int)a->len;
}

static int test_fetch(int cdram) {
    static char json[1024];
    int len = 0, failures = 0;
    SgxdInfo info = {.size = sizeof(info)};
    const SceUID block = sceKernelAllocMemBlock("sgxd_test_fetch", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                                FT_SIZE, NULL);
    const SceUID code_block = sceKernelAllocMemBlock(
        "sgxd_test_fetch_code",
        cdram ? SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW : SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
        FT_CODE_SIZE, NULL);
    void *base = NULL, *code_base = NULL;
    if (block < 0 || code_block < 0 || sceKernelGetMemBlockBase(block, &base) < 0 ||
        sceKernelGetMemBlockBase(code_block, &code_base) < 0) {
        LOG("18: memory 0x%08x 0x%08x\n", (unsigned)block, (unsigned)code_block);
        return 1;
    }
    uint8_t *nc = base;
    volatile uint32_t *marker = (volatile uint32_t *)(nc + FT_CC);
    memset(nc, 0, FT_SIZE);
    put_douta(nc, 0, NC_GPU_VA + NC_RESULT);
    const uint32_t bank0_data[4] = {13u | GPU_TEMPS << 24, 1, 0, 0};
    put_pds(nc, NC_PDS(1), bank0_data, PDS_DOUTU_UNITS, pds_doutu_code, sizeof(pds_doutu_code) / 4);
    const uint64_t end[2] = {USSE_PHAS_END, USSE_NOP_END};
    memcpy(nc + NC_CODE_END, end, sizeof(end));
    *(uint32_t *)(nc + NC_KICKER) = GPU_KICKER_VALUE;
    memset(code_base, 0, FT_CODE_SIZE);
    const int words = put_fetch_program((uint64_t *)code_base, FT_CODE_SIZE / 8);
    if (!words) {
        return 1;
    }
    if (sgxdTake(&info) < 0) {
        return 1;
    }
    const int mapped = sgxdMap(NC_GPU_VA, base, FT_CC, 0) |
                       sgxdMap(NC_GPU_VA + FT_CC, nc + FT_CC, 0x1000, SGXD_MAP_CACHECONSISTENT) |
                       sgxdMap(NC_GPU_VA + FT_CC + 0x1000, nc + FT_CC + 0x1000,
                               FT_SIZE - FT_CC - 0x1000, 0) |
                       sgxdMap(FT_CODE_VA, code_base, FT_CODE_SIZE, SGXD_MAP_READONLY);
    InitAddresses addresses = {
        .kicker = NC_GPU_VA + NC_KICKER,
        .bank0_pds = NC_GPU_VA + NC_PDS(1), .bank0_units = PDS_DOUTU_UNITS,
        .core0_pds = NC_GPU_VA + NC_PDS(0), .core0_units = PDS_DOUTA_UNITS,
    };
    for (unsigned b = 0; b < 16; b++) {
        addresses.code[b] = b == 15 ? FT_CODE_VA : NC_GPU_VA + NC_CODE_END;
    }
    const int init = mapped != 0 || slc(1) < 0 ? -1 : init_script(&addresses);
    failures += init != 0;
    const uint32_t mhz = (uint32_t)scePowerGetArmClockFrequency();
    uint32_t us10[FT_MODES][3] = {{0}};   // tenths of a microsecond
    static const char *const when[3] = {"cold", "warm", "after an SLC and USE cache invalidate"};
    for (unsigned m = 0; init == 0 && m < FT_MODES && !failures; m++) {
        for (unsigned k = 0; k < 3; k++) {
            marker[0] = 0;
            marker[17] = m;
            if (k == 2) {
                SgxdOp inval[1] = {W(0x0a08, 1)};
                failures += submit_checked(inval, 1, "18 USE cache") < 0;
            }
            SgxdOp kick[1] = {W(0x8ac0, 1)};
            const uint32_t c0 = azaharCyclesStart();
            failures += submit_checked(kick, 1, "18 kick") < 0;
            uint32_t c = c0;
            while (marker[0] != GPU_DONE && c - c0 < 200000u * mhz) {
                c = azaharCycles();
            }
            us10[m][k] = marker[0] == GPU_DONE ? (uint32_t)((uint64_t)(c - c0) * 10u / mhz) : 0xffffffffu;
            failures += marker[0] != GPU_DONE;
            LOG("18 %-18s %s: %u.%u us\n", ft_names[m], when[k], us10[m][k] / 10, us10[m][k] % 10);
        }
    }
    const int unmapped = sgxdUnmap(NC_GPU_VA, FT_SIZE) | sgxdUnmap(FT_CODE_VA, FT_CODE_SIZE);
    const int released = sgxdRelease();
    failures += unmapped != 0 || released != 0;
    len += snprintf(json + len, sizeof(json) - len, "{\"test\": \"18-fetch\", \"code_in\": \"%s\", "
                    "\"instructions\": %d", cdram ? "cdram" : "main", words);
    for (unsigned m = 0; m < FT_MODES; m++) {
        len += snprintf(json + len, sizeof(json) - len, ", \"%s_us10\": [%u, %u, %u]", ft_names[m],
                        us10[m][0], us10[m][1], us10[m][2]);
    }
    len += snprintf(json + len, sizeof(json) - len, ", \"result\": \"%s\"}\n", failures ? "FAIL" : "PASS");
    char path[96];
    out_path(path, sizeof(path), "18-fetch.json");
    write_file(path, json, (unsigned)len);
    sceKernelFreeMemBlock(code_block);
    sceKernelFreeMemBlock(block);
    LOG("18-fetch (code in %s) %s\n", cdram ? "CDRAM" : "main memory", failures ? "FAIL" : "PASS");
    return failures;
}

// ---------------------------------------------------------------------------- test 19

#define CM_N 256u
enum { CM_EMPTY, CM_ALU, CM_LIMM, CM_STR, CM_STA, CM_LDR, CM_LDA, CM_BRANCH, CM_STR2REGS,
       CM_STR_X2, CM_VBW, CM_VBW_NS, CM_LIMM_NS, CM_STR_NS, CM_STA_NS, CM_BRANCH_NS, CM_LDR_NS,
       CM_MODES };
static const char *const cm_names[CM_MODES] = {
    "empty", "alu", "limm", "str_reg_imm", "sta32_cc", "ldr_wdf", "lda32_wdf_slc", "taken_branch",
    "str_two_regs", "str_x2", "vbw_or", "vbw_or_nosched", "limm_nosched", "str_nosched",
    "sta32_cc_nosched", "taken_branch_nosched", "ldr_wdf_nosched",
};
// NOSCHED (high word): bit 22 for LIMM, the bitwise, test and
// integer ops and LD/ST; bit 11 for LDR/STR, flow control, IDF/WDF.
#define CM_NS22 (1ull << 54)
#define CM_NS11 (1ull << 43)

static int put_cost_program(uint64_t *code, unsigned cap) {
    struct sgx_usse_asm as, *a = &as;
    sgx_usse_init(a, code, cap, 0);
    int bodies[CM_MODES];
    for (unsigned m = 0; m < CM_MODES; m++) {
        bodies[m] = sgx_usse_label(a);
    }
    const int done = sgx_usse_label(a);
    sgx_usse_smlsi(a);
    sgx_usse_mov(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_PA(2));
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), 1);
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_IMM(3), USSE_R(9));
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    // The mode from the CACHECONSISTENT page: a CPU write to a line the GPU has dirtied in the
    // SLC is lost when the SLC writes the line back (tests 15 and 18 ran mode 0 each time).
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(3), NC_GPU_VA + FT_CC);     // CACHECONSISTENT
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(7), USSE_R(3), USSE_IMM(17), 0);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(2), NC_GPU_VA + 0x7100u);   // plain
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), 0x11111111u);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(10), 0x22222222u);
    sgx_usse_wdf(a, 0);
    for (unsigned m = 0; m < CM_MODES; m++) {
        sgx_usse_test(a, SGX_USSE_ALWAYS, 0, SGX_USSE_TST_XOR, SGX_USSE_ZERO, USSE_R(7), USSE_IMM(m));
        sgx_usse_ba(a, SGX_USSE_P0, bodies[m]);
    }
    sgx_usse_ba(a, SGX_USSE_ALWAYS, done);
    for (unsigned m = 0; m < CM_MODES; m++) {
        sgx_usse_bind(a, bodies[m]);
        for (unsigned k = 0; k < (m == CM_STR_X2 ? CM_N / 2 : m == CM_EMPTY ? 0 : CM_N); k++) {
            switch (m) {
            case CM_ALU:
                sgx_usse_imad2(a, SGX_USSE_ALWAYS, USSE_R(16 + (k & 7)), USSE_R(16 + (k & 7)),
                               USSE_IMM(1), USSE_IMM(1));
                break;
            case CM_LIMM:
                sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(16 + (k & 7)), 0x12340000u + k);
                break;
            case CM_STR:
                sgx_usse_str(a, SGX_USSE_ALWAYS, MK_REG, USSE_IMM(1));
                break;
            case CM_STA:
                sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(3), USSE_IMM(1 + (k & 7)), USSE_R(9));
                break;
            case CM_LDR:
                sgx_usse_ldr(a, USSE_R(16), MK_REG, 0);
                sgx_usse_wdf(a, 0);
                break;
            case CM_LDA:
                sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(16), USSE_R(2), USSE_IMM(0), 0);
                sgx_usse_wdf(a, 0);
                break;
            case CM_BRANCH:
                sgx_usse_ba_index(a, SGX_USSE_ALWAYS, sgx_usse_here(a) + 1);
                break;
            case CM_STR2REGS:
                sgx_usse_str(a, SGX_USSE_ALWAYS, k & 1 ? MK_REG + 4 : MK_REG, USSE_R(9));
                break;
            case CM_STR_X2: {
                const uint32_t at = a->len;
                sgx_usse_str(a, SGX_USSE_ALWAYS, MK_REG, USSE_R(9));
                a->code[at] |= (uint64_t)(1u << 12) << 32;   // repeat count 2
                break;
            }
            case CM_VBW:
            case CM_VBW_NS:
                sgx_usse_bitop(a, SGX_USSE_ALWAYS, SGX_USSE_OR, USSE_R(16 + (k & 7)),
                               USSE_R(16 + (k & 7)), USSE_IMM(1), 0);
                if (m == CM_VBW_NS) {
                    a->code[a->len - 1] |= CM_NS22;
                }
                break;
            case CM_LIMM_NS:
                sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(16 + (k & 7)), 0x12340000u + k);
                a->code[a->len - 1] |= CM_NS22;
                break;
            case CM_STR_NS:
                sgx_usse_str(a, SGX_USSE_ALWAYS, MK_REG, USSE_IMM(1));
                a->code[a->len - 1] |= CM_NS11;
                break;
            case CM_STA_NS:
                sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(3), USSE_IMM(1 + (k & 7)), USSE_R(9));
                a->code[a->len - 1] |= CM_NS22;
                break;
            case CM_BRANCH_NS:
                sgx_usse_ba_index(a, SGX_USSE_ALWAYS, sgx_usse_here(a) + 1);
                a->code[a->len - 1] |= CM_NS11;
                break;
            case CM_LDR_NS:
                sgx_usse_ldr(a, USSE_R(16), MK_REG, 0);
                a->code[a->len - 1] |= CM_NS11;
                sgx_usse_wdf(a, 0);
                a->code[a->len - 1] |= CM_NS11;
                break;
            default:
                break;
            }
        }
        sgx_usse_ba(a, SGX_USSE_ALWAYS, done);
    }
    sgx_usse_bind(a, done);
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), GPU_DONE);
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(3), USSE_IMM(0), USSE_R(9));
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_phas_end(a);
    sgx_usse_nop_end(a);
    return sgx_usse_finish(a) < 0 ? 0 : (int)a->len;
}

static int test_costs(void) {
    static char json[2048];
    int len = 0, failures = 0;
    SgxdInfo info = {.size = sizeof(info)};
    const SceUID block = sceKernelAllocMemBlock("sgxd_test_costs", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                                FT_SIZE, NULL);
    const SceUID code_block = sceKernelAllocMemBlock("sgxd_test_costs_code",
                                                     SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                                     FT_CODE_SIZE, NULL);
    void *base = NULL, *code_base = NULL;
    if (block < 0 || code_block < 0 || sceKernelGetMemBlockBase(block, &base) < 0 ||
        sceKernelGetMemBlockBase(code_block, &code_base) < 0) {
        return 1;
    }
    uint8_t *nc = base;
    volatile uint32_t *marker = (volatile uint32_t *)(nc + FT_CC);
    memset(nc, 0, FT_SIZE);
    put_douta(nc, 0, NC_GPU_VA + NC_RESULT);
    const uint32_t bank0_data[4] = {13u | GPU_TEMPS << 24, 1, 0, 0};
    put_pds(nc, NC_PDS(1), bank0_data, PDS_DOUTU_UNITS, pds_doutu_code, sizeof(pds_doutu_code) / 4);
    const uint64_t end[2] = {USSE_PHAS_END, USSE_NOP_END};
    memcpy(nc + NC_CODE_END, end, sizeof(end));
    *(uint32_t *)(nc + NC_KICKER) = GPU_KICKER_VALUE;
    memset(code_base, 0, FT_CODE_SIZE);
    const int words = put_cost_program((uint64_t *)code_base, FT_CODE_SIZE / 8);
    if (!words) {
        return 1;
    }
    char path[96];
    out_path(path, sizeof(path), "19-costs.bin");
    write_file(path, code_base, (unsigned)words * 8);
    if (sgxdTake(&info) < 0) {
        return 1;
    }
    const int mapped = sgxdMap(NC_GPU_VA, base, FT_CC, 0) |
                       sgxdMap(NC_GPU_VA + FT_CC, nc + FT_CC, 0x1000, SGXD_MAP_CACHECONSISTENT) |
                       sgxdMap(NC_GPU_VA + FT_CC + 0x1000, nc + FT_CC + 0x1000,
                               FT_SIZE - FT_CC - 0x1000, 0) |
                       sgxdMap(FT_CODE_VA, code_base, FT_CODE_SIZE, SGXD_MAP_READONLY);
    InitAddresses addresses = {
        .kicker = NC_GPU_VA + NC_KICKER,
        .bank0_pds = NC_GPU_VA + NC_PDS(1), .bank0_units = PDS_DOUTU_UNITS,
        .core0_pds = NC_GPU_VA + NC_PDS(0), .core0_units = PDS_DOUTA_UNITS,
    };
    for (unsigned b = 0; b < 16; b++) {
        addresses.code[b] = b == 15 ? FT_CODE_VA : NC_GPU_VA + NC_CODE_END;
    }
    const int init = mapped != 0 || slc(1) < 0 ? -1 : init_script(&addresses);
    failures += init != 0;
    const uint32_t mhz = (uint32_t)scePowerGetArmClockFrequency();
    uint32_t ns[CM_MODES][2] = {{0}};
    uint32_t x2_regs[2] = {0, 0};
    for (unsigned m = 0; init == 0 && m < CM_MODES && !failures; m++) {
        for (unsigned k = 0; k < 2; k++) {
            marker[0] = 0;
            marker[17] = m;
            SgxdOp kick[1] = {W(0x8ac0, 1)};
            const uint32_t c0 = azaharCyclesStart();
            failures += submit_checked(kick, 1, "19 kick") < 0;
            uint32_t c = c0;
            while (marker[0] != GPU_DONE && c - c0 < 200000u * mhz) {
                c = azaharCycles();
            }
            ns[m][k] = marker[0] == GPU_DONE ? (uint32_t)((uint64_t)(c - c0) * 1000u / mhz) : 0xffffffffu;
            failures += marker[0] != GPU_DONE;
        }
        if (m == CM_STR_X2) {
            x2_regs[0] = mk_read(MK_REG);
            x2_regs[1] = mk_read(MK_REG + 4);
        }
        if (m == CM_STR_X2 - 1) {
            // Before the repeat: both registers something r9 and r10 are not.
            SgxdOp set[2] = {W(MK_REG, 0x33333333u), W(MK_REG + 4, 0x44444444u)};
            submit_checked(set, 2, "19 registers");
        }
    }
    for (unsigned m = 0; m < CM_MODES; m++) {
        const unsigned count = m == CM_STR_X2 ? CM_N / 2 : CM_N;
        const int32_t per = m && ns[m][1] != 0xffffffffu
                                ? (int32_t)((int64_t)ns[m][1] - ns[0][1]) / (int32_t)count : 0;
        LOG("19 %-14s cold %6u ns, warm %6u ns: %d ns an instruction\n", cm_names[m], ns[m][0],
            ns[m][1], per);
    }
    LOG("19 after str.x2 [0x0240] = r9 (0x11111111, r10 0x22222222): 0x0240 %08x 0x0244 %08x\n",
        x2_regs[0], x2_regs[1]);
    const int unmapped = sgxdUnmap(NC_GPU_VA, FT_SIZE) | sgxdUnmap(FT_CODE_VA, FT_CODE_SIZE);
    const int released = sgxdRelease();
    failures += unmapped != 0 || released != 0;
    len += snprintf(json + len, sizeof(json) - len, "{\"test\": \"19-costs\", \"n\": %u", CM_N);
    for (unsigned m = 0; m < CM_MODES; m++) {
        len += snprintf(json + len, sizeof(json) - len, ", \"%s_ns\": [%u, %u]", cm_names[m], ns[m][0],
                        ns[m][1]);
    }
    len += snprintf(json + len, sizeof(json) - len,
                    ", \"str_x2_regs\": [\"0x%08x\", \"0x%08x\"], \"result\": \"%s\"}\n",
                    x2_regs[0], x2_regs[1], failures ? "FAIL" : "PASS");
    out_path(path, sizeof(path), "19-costs.json");
    write_file(path, json, (unsigned)len);
    sceKernelFreeMemBlock(code_block);
    sceKernelFreeMemBlock(block);
    LOG("19-costs %s\n", failures ? "FAIL" : "PASS");
    return failures;
}

// ---------------------------------------------------------------------------- test 20

#define PW_TASK_PDS 0x0200u      // the worker task's PDS program, in the uncached buffer
#define PW_STORES 512u
#define PW_FLAGS 8u              // done flags at CACHECONSISTENT words 8..15
#define PW_TASK_WORD (PDS_DOUTA_UNITS << 23 | 0x900u)

// count stores to MK_REG from r2 (the count, a multiple of 8): eight a turn, a 16-bit
// countdown.
static void pw_stores(struct sgx_usse_asm *a) {
    const int loop = sgx_usse_label(a);
    sgx_usse_bind(a, loop);
    for (unsigned k = 0; k < 8; k++) {
        sgx_usse_str(a, SGX_USSE_ALWAYS, MK_REG, USSE_IMM(1));
    }
    sgx_usse_dec_test(a, SGX_USSE_ALWAYS, 0, SGX_USSE_NONZERO, USSE_R(2), USSE_R(2), 8);
    sgx_usse_ba(a, SGX_USSE_P0, loop);
}

// The flag of thread r5 (its index) in the CACHECONSISTENT page (r0), fenced.
static void pw_flag(struct sgx_usse_asm *a) {
    sgx_usse_imad2(a, SGX_USSE_ALWAYS, USSE_R(6), USSE_R(5), USSE_IMM(4), USSE_R(0));
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(9), GPU_DONE);
    sgx_usse_st(a, SGX_USSE_ALWAYS, USSE_R(6), USSE_IMM(PW_FLAGS), USSE_R(9));
    sgx_usse_idf(a, 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_phas_end(a);
    sgx_usse_nop_end(a);
}

// The main program at index 0, the worker's at *worker. The CACHECONSISTENT page (pa2): word 1
// W, 2 the stores a thread, 3 0 for one pipe or 1 for pipes w % 4.
static int put_parallel_program(uint64_t *code, unsigned cap, uint32_t task_pds_va, uint32_t *worker) {
    struct sgx_usse_asm as, *a = &as;
    sgx_usse_init(a, code, cap, 0);
    const int work = sgx_usse_label(a), spread = sgx_usse_label(a), wk = sgx_usse_label(a);
    sgx_usse_smlsi(a);
    sgx_usse_mov(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_PA(2));
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(1), USSE_R(0), USSE_IMM(1), 0);
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(2), USSE_R(0), USSE_IMM(2), 0);
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(3), USSE_R(0), USSE_IMM(3), 0);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(16), PW_TASK_WORD);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(18), task_pds_va >> 4);
    sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(5), 0);
    sgx_usse_wdf(a, 0);
    sgx_usse_test(a, SGX_USSE_ALWAYS, 1, SGX_USSE_TST_AND, SGX_USSE_NONZERO, USSE_R(3), USSE_R(3));
    sgx_usse_ba(a, SGX_USSE_P1, spread);
    // Two emission blocks, the sideband an immediate: workers 1..7, each while w < W.
    for (unsigned mode = 0; mode < 2; mode++) {
        if (mode == 1) {
            sgx_usse_bind(a, spread);
        }
        for (unsigned w = 1; w < 8; w++) {
            const int wait = sgx_usse_label(a);
            sgx_usse_test(a, SGX_USSE_ALWAYS, 0, SGX_USSE_TST_XOR, SGX_USSE_ZERO, USSE_R(1), USSE_IMM(w));
            sgx_usse_ba(a, SGX_USSE_P0, work);
            // Until LOOPBACK bit 0, then the emit.
            sgx_usse_bind(a, wait);
            sgx_usse_ldr(a, USSE_R(19), 0x0b04, 1);
            sgx_usse_wdf(a, 1);
            sgx_usse_test(a, SGX_USSE_ALWAYS, 2, SGX_USSE_TST_AND, SGX_USSE_ZERO, USSE_R(19), USSE_IMM(1));
            sgx_usse_ba(a, SGX_USSE_P2, wait);
            sgx_usse_limm(a, SGX_USSE_ALWAYS, USSE_R(20), w);
            sgx_usse_emitpds(a, 16, 18, 20, 0x43u | (mode ? (w & 3u) << 12 : 0u));
        }
        sgx_usse_ba(a, SGX_USSE_ALWAYS, work);
    }
    sgx_usse_bind(a, work);
    pw_stores(a);
    pw_flag(a);
    // The worker: pa0 is its index (IR0).
    sgx_usse_bind(a, wk);
    sgx_usse_smlsi(a);
    sgx_usse_mov(a, SGX_USSE_ALWAYS, USSE_R(5), USSE_PA(0));
    sgx_usse_mov(a, SGX_USSE_ALWAYS, USSE_R(0), USSE_PA(2));
    sgx_usse_ld(a, SGX_USSE_ALWAYS, USSE_R(2), USSE_R(0), USSE_IMM(2), 0);
    sgx_usse_wdf(a, 0);
    pw_stores(a);
    pw_flag(a);
    if (sgx_usse_finish(a) < 0) {
        return 0;
    }
    *worker = sgx_usse_label_index(a, wk);
    return (int)a->len;
}

static int test_parallel(void) {
    static char json[2048];
    int len = 0, failures = 0;
    SgxdInfo info = {.size = sizeof(info)};
    const SceUID block = sceKernelAllocMemBlock("sgxd_test_par", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                                FT_SIZE, NULL);
    const SceUID code_block = sceKernelAllocMemBlock("sgxd_test_par_code",
                                                     SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                                     FT_CODE_SIZE, NULL);
    void *base = NULL, *code_base = NULL;
    if (block < 0 || code_block < 0 || sceKernelGetMemBlockBase(block, &base) < 0 ||
        sceKernelGetMemBlockBase(code_block, &code_base) < 0) {
        LOG("20: memory 0x%08x 0x%08x\n", (unsigned)block, (unsigned)code_block);
        return 1;
    }
    uint8_t *nc = base;
    volatile uint32_t *cc = (volatile uint32_t *)(nc + FT_CC);
    memset(nc, 0, FT_SIZE);
    memset(code_base, 0, FT_CODE_SIZE);
    uint32_t worker = 0;
    const int words = put_parallel_program((uint64_t *)code_base, FT_CODE_SIZE / 8,
                                           NC_GPU_VA + PW_TASK_PDS, &worker);
    if (!words) {
        return 1;
    }
    // Core 0's event program (pa2 the CACHECONSISTENT page) and the worker's: DOUTA IR0, IR1,
    // the page; DOUTU code base 15 at index 0 or at the worker.
    put_douta(nc, 0, NC_GPU_VA + FT_CC);
    douta_data[8] = 15u | worker << 4 | GPU_TEMPS << 24;
    memcpy(nc + PW_TASK_PDS, douta_data, sizeof(douta_data));
    memcpy(nc + PW_TASK_PDS + PDS_DOUTA_UNITS * 16, pds_douta_code, sizeof(pds_douta_code));
    const uint32_t bank0_data[4] = {13u | GPU_TEMPS << 24, 1, 0, 0};
    put_pds(nc, NC_PDS(1), bank0_data, PDS_DOUTU_UNITS, pds_doutu_code, sizeof(pds_doutu_code) / 4);
    const uint64_t end[2] = {USSE_PHAS_END, USSE_NOP_END};
    memcpy(nc + NC_CODE_END, end, sizeof(end));
    *(uint32_t *)(nc + NC_KICKER) = GPU_KICKER_VALUE;
    char path[96];
    out_path(path, sizeof(path), "20-parallel.bin");
    write_file(path, code_base, (unsigned)words * 8);
    if (sgxdTake(&info) < 0) {
        return 1;
    }
    const int mapped = sgxdMap(NC_GPU_VA, base, FT_CC, 0) |
                       sgxdMap(NC_GPU_VA + FT_CC, nc + FT_CC, 0x1000, SGXD_MAP_CACHECONSISTENT) |
                       sgxdMap(NC_GPU_VA + FT_CC + 0x1000, nc + FT_CC + 0x1000,
                               FT_SIZE - FT_CC - 0x1000, 0) |
                       sgxdMap(FT_CODE_VA, code_base, FT_CODE_SIZE, SGXD_MAP_READONLY);
    InitAddresses addresses = {
        .kicker = NC_GPU_VA + NC_KICKER,
        .bank0_pds = NC_GPU_VA + NC_PDS(1), .bank0_units = PDS_DOUTU_UNITS,
        .core0_pds = NC_GPU_VA + NC_PDS(0), .core0_units = PDS_DOUTA_UNITS,
    };
    for (unsigned b = 0; b < 16; b++) {
        addresses.code[b] = b == 15 ? FT_CODE_VA : NC_GPU_VA + NC_CODE_END;
    }
    const int init = mapped != 0 || slc(1) < 0 ? -1 : init_script(&addresses);
    failures += init != 0;
    const uint32_t mhz = (uint32_t)scePowerGetArmClockFrequency();
    static const unsigned threads[4] = {1, 2, 4, 8};
    uint32_t ns[2][4] = {{0}};
    for (unsigned mode = 0; init == 0 && mode < 2 && !failures; mode++) {
        for (unsigned t = 0; t < 4 && !failures; t++) {
            const unsigned w = threads[t];
            for (unsigned k = 0; k < 2; k++) {   // the second, warm, counts
                for (unsigned f = 0; f < PW_FLAGS; f++) {
                    cc[PW_FLAGS + f] = 0;
                }
                cc[1] = w;
                cc[2] = PW_STORES / w;
                cc[3] = mode;
                __asm__ volatile("dmb" ::: "memory");
                SgxdOp kick[1] = {W(0x8ac0, 1)};
                const uint32_t c0 = azaharCyclesStart();
                failures += submit_checked(kick, 1, "20 kick") < 0;
                uint32_t c = c0;
                unsigned flagged = 0;
                while (c - c0 < 100000u * mhz) {
                    flagged = 0;
                    for (unsigned f = 0; f < w; f++) {
                        flagged += cc[PW_FLAGS + f] == GPU_DONE;
                    }
                    if (flagged == w) {
                        break;
                    }
                    c = azaharCycles();
                }
                ns[mode][t] = flagged == w ? (uint32_t)((uint64_t)(c - c0) * 1000u / mhz) : 0xffffffffu;
                if (flagged != w) {
                    LOG("20 %s, %u threads: %u of %u flags in 100 ms\n", mode ? "pipes w % 4" : "pipe 0",
                        w, flagged, w);
                    failures++;
                    break;
                }
            }
            LOG("20 %-12s %u thread(s) x %3u stores: %6u ns\n", mode ? "pipes w % 4" : "pipe 0", w,
                PW_STORES / w, ns[mode][t]);
        }
    }
    const int unmapped = sgxdUnmap(NC_GPU_VA, FT_SIZE) | sgxdUnmap(FT_CODE_VA, FT_CODE_SIZE);
    const int released = sgxdRelease();
    failures += unmapped != 0 || released != 0;
    len += snprintf(json + len, sizeof(json) - len,
                    "{\"test\": \"20-parallel\", \"stores\": %u, \"code\": \"cdram\", "
                    "\"pipe0_ns\": [%u, %u, %u, %u], \"spread_ns\": [%u, %u, %u, %u], "
                    "\"result\": \"%s\"}\n",
                    PW_STORES, ns[0][0], ns[0][1], ns[0][2], ns[0][3], ns[1][0], ns[1][1],
                    ns[1][2], ns[1][3], failures ? "FAIL" : "PASS");
    out_path(path, sizeof(path), "20-parallel.json");
    write_file(path, json, (unsigned)len);
    sceKernelFreeMemBlock(code_block);
    sceKernelFreeMemBlock(block);
    LOG("20-parallel %s\n", failures ? "FAIL" : "PASS");
    return failures;
}

// ---------------------------------------------------------------------------- test 03

// The recipe at our own GPU address: 1 MiB aligned, below 256 MiB (docs/sgxd/memory.md).
#define RECIPE_GPU_VA 0x01000000u
#define RECIPE_SIZE 64u
#define RECIPE_PAGES 418u
#define RECIPE_SA_BG 0xff00ff00u   // 03b: the background colour, through SA

static const uint32_t recipe_diag_regs[] = {
    0x012c, 0x412c, 0x812c, 0x0118, 0x4118, 0x8118, 0x0138, 0x8008, 0xc008, 0x10008, 0x14008,
    0x0c04, 0x0c08, 0x4c04, 0x4c08, 0x8c04, 0x8c08, 0xcc04, 0xcc08, 0x10c04, 0x10c08, 0x14c04,
    0x14c08, 0x061c, 0x0648, 0x0724, 0x8274, 0xc274, 0x10274, 0x14274,
};
#define RECIPE_DIAG_COUNT (sizeof(recipe_diag_regs) / sizeof(recipe_diag_regs[0]))

static void recipe_diag(uint32_t *values) {
    SgxdOp ops[RECIPE_DIAG_COUNT];
    for (unsigned n = 0; n < RECIPE_DIAG_COUNT; n++) {
        ops[n] = (SgxdOp)R(recipe_diag_regs[n]);
    }
    sgxdSubmit(ops, RECIPE_DIAG_COUNT);
    for (unsigned n = 0; n < RECIPE_DIAG_COUNT; n++) {
        values[n] = ops[n].value;
    }
}

// Every bank's EVENT_STATUS and EVENT_STATUS2 (bank 0, the master, cores 0-3), read at each phase
// of render 0: which bank latches which completion.
static const uint32_t event_banks[6] = {0x0000, 0x4000, 0x8000, 0xc000, 0x10000, 0x14000};
#define EVENT_PHASES 5
static const char *const event_phase_names[EVENT_PHASES] = {
    "before", "ta_finished", "ta_done", "3d_finished", "3d_done",
};
static uint32_t event_snap[EVENT_PHASES][12];

static void event_snapshot(unsigned phase) {
    SgxdOp ops[12];
    for (unsigned b = 0; b < 6; b++) {
        ops[b] = (SgxdOp)R(event_banks[b] + 0x012c);
        ops[6 + b] = (SgxdOp)R(event_banks[b] + 0x0118);
    }
    sgxdSubmit(ops, 12);
    for (unsigned n = 0; n < 12; n++) {
        event_snap[phase][n] = ops[n].value;
    }
}

// One phase's op list through sgxdSubmit (sgxd_session_phase). On a stop, the op goes to the log.
static int recipe_phase(const SgxdRecipe *r, SgxdSessionBuild build, const char *what, char *failed,
                        unsigned failed_size) {
    if (sgxd_session_phase(r, build, what, failed, failed_size) < 0) {
        LOG("phase stopped: %s\n", failed);
        return -1;
    }
    return 0;
}

// Test 03: the TA+3D one-triangle recipe at RECIPE_GPU_VA through sgxdSubmit, renders
// times. The first render's framebuffer goes to 03-framebuffer.bin; every later one must equal it.
// "mkevents=1" (13d): test 13's probe program as core 0's event
// program (code base 14; the recipe's programs use 0 and 1), its memory mapped beside the
// recipe's, and the recipe's kicks leaving the event program alone (events_owned): the log
// records each run the TA's and the 3D's completions start, with their IR words. In
// 03-recipe.json's "mk_log".
static int test_recipe(int renders, int sa_bg, int mkevents) {
    static char json[8192];
    static char failed[160];
    int len = 0, failures = 0;
    SgxdInfo info = {.size = sizeof(info)};
    SgxdRecipe r;
    if (sgxd_recipe_init(&r, RECIPE_SIZE, RECIPE_SIZE, RECIPE_GPU_VA, RECIPE_PAGES) < 0) {
        LOG("03: recipe init failed\n");
        return 1;
    }
    r.events_owned = mkevents;
    SceUID mk_block = -1;
    uint8_t *mk = NULL;
    if (mkevents) {
        void *mk_base = NULL;
        mk_block = sceKernelAllocMemBlock("sgxd_test_mk", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                          NC_SIZE, NULL);
        if (mk_block < 0 || sceKernelGetMemBlockBase(mk_block, &mk_base) < 0 ||
            !mk_stage(mk_base, 14)) {
            LOG("03 mkevents: memory 0x%08x\n", (unsigned)mk_block);
            return 1;
        }
        mk = mk_base;
    }
    const uint32_t kicker_offset = (r.offset[SGXD_R_END] + 0xfff) & ~0xfffu;
    const uint32_t size = (kicker_offset + 0x1000 + 0xffff) & ~0xffffu;
    const SceUID block = sceKernelAllocMemBlock("sgxd_test_recipe", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                                size, NULL);
    void *base = NULL;
    if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0) {
        LOG("03: memory 0x%08x (%u bytes)\n", (unsigned)block, (unsigned)size);
        return 1;
    }
    uint8_t *memory = base;
    const uint64_t stage_t0 = sceKernelGetProcessTimeWide();
    sgxd_recipe_write(&r, memory);
    const uint32_t stage_us = (uint32_t)(sceKernelGetProcessTimeWide() - stage_t0);
    *(uint32_t *)(memory + kicker_offset) = GPU_KICKER_VALUE;
    const uint32_t bg_colour = sa_bg ? RECIPE_SA_BG : SGXD_RECIPE_BG;
    if (sa_bg) {
        sgxd_recipe_sa_background(&r, memory, kicker_offset + 0x400, RECIPE_SA_BG);
    }
    const volatile uint32_t *fb = (const volatile uint32_t *)(memory + r.offset[SGXD_R_FB]);
    const unsigned pixels = RECIPE_SIZE * RECIPE_SIZE;
    static uint32_t first_fb[RECIPE_SIZE * RECIPE_SIZE];

    const int taken = sgxdTake(&info);
    LOG("03 take -> %d: step %u status %08x core id %08x, %u us in the kernel\n", taken, info.step,
        (unsigned)info.step_status, info.core_id, info.take_us);
    if (taken < 0) {
        sceKernelFreeMemBlock(block);
        return 1;
    }
    const uint64_t map_t0 = sceKernelGetProcessTimeWide();
    const int mapped = sgxdMap(RECIPE_GPU_VA, base, size, 0);
    const uint32_t map_us = (uint32_t)(sceKernelGetProcessTimeWide() - map_t0);
    LOG("03 map %u KiB at %08x -> %08x in %u us; staged in %u us\n", size / 1024, RECIPE_GPU_VA,
        (unsigned)mapped, map_us, stage_us);
    failures += mapped != 0;

    InitAddresses addresses;
    sgxd_session_recipe_addresses(&r, RECIPE_GPU_VA + kicker_offset, &addresses);
    if (mkevents) {
        const int mk_mapped = mapped == 0 ? sgxdMap(NC_GPU_VA, mk, NC_SIZE, 0) : -1;
        LOG("03 mkevents: probe memory at %08x -> %08x\n", NC_GPU_VA, (unsigned)mk_mapped);
        failures += mk_mapped != 0;
        addresses.core0_pds = NC_GPU_VA + NC_PDS(0);
        addresses.core0_units = PDS_DOUTA_UNITS;
        addresses.code[14] = NC_GPU_VA + NC_CODE;
    }
    int init = -1, done = 0, bg = 0, tri = 0, other = 0, mismatched = 0;
    uint32_t ta_us = 0, d3_us = 0, ta_max = 0, d3_max = 0;
    uint64_t ta_sum = 0, d3_sum = 0;
    uint32_t diag[RECIPE_DIAG_COUNT] = {0};
    if (mapped == 0) {
        init = slc(1) < 0 ? -1 : init_script(&addresses);
        failures += init < 0;
    }
    for (int n = 0; init == 0 && n < renders; n++) {
        if (n > 0) {
            // The SLC holds nothing of ours once idle and invalidated; then the CPU restages.
            if (recipe_phase(&r, sgxd_session_slc_inval, "slc", failed, sizeof(failed)) < 0) {
                break;
            }
            sgxd_recipe_restage(&r, memory);
        }
        if (n == 0) {
            event_snapshot(0);
        }
        if (recipe_phase(&r, sgxd_recipe_ta_kick, "ta kick", failed, sizeof(failed)) < 0) {
            break;
        }
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        if (recipe_phase(&r, sgxd_session_ta_wait, "ta wait", failed, sizeof(failed)) < 0) {
            break;
        }
        const uint64_t t1 = sceKernelGetProcessTimeWide();
        if (n == 0) {
            event_snapshot(1);
        }
        if (recipe_phase(&r, sgxd_recipe_ta_done, "ta done", failed, sizeof(failed)) < 0) {
            break;
        }
        if (n == 0) {
            event_snapshot(2);
        }
        if (recipe_phase(&r, sgxd_recipe_3d_kick, "3d kick", failed, sizeof(failed)) < 0) {
            break;
        }
        const uint64_t t2 = sceKernelGetProcessTimeWide();
        if (recipe_phase(&r, sgxd_session_3d_wait, "3d wait", failed, sizeof(failed)) < 0) {
            break;
        }
        const uint64_t t3 = sceKernelGetProcessTimeWide();
        if (n == 0) {
            event_snapshot(3);
        }
        if (recipe_phase(&r, sgxd_recipe_3d_done, "3d done", failed, sizeof(failed)) < 0) {
            break;
        }
        if (n == 0) {
            event_snapshot(4);
        }
        const uint32_t ta = (uint32_t)(t1 - t0), d3 = (uint32_t)(t3 - t2);
        ta_sum += ta;
        d3_sum += d3;
        ta_max = ta > ta_max ? ta : ta_max;
        d3_max = d3 > d3_max ? d3 : d3_max;
        if (n == 0) {
            ta_us = ta;
            d3_us = d3;
            for (unsigned p = 0; p < pixels; p++) {
                first_fb[p] = fb[p];
                bg += first_fb[p] == bg_colour;
                tri += first_fb[p] == SGXD_RECIPE_TRI;
            }
            other = (int)pixels - bg - tri;
            char path[96];
            out_path(path, sizeof(path), "03-framebuffer.bin");
            write_file(path, first_fb, sizeof(first_fb));
            LOG("03 render 0: TA %u us, 3D %u us; %d background, %d triangle, %d other\n", ta, d3,
                bg, tri, other);
        } else {
            unsigned differ = 0;
            for (unsigned p = 0; p < pixels; p++) {
                differ += fb[p] != first_fb[p];
            }
            if (differ) {
                mismatched++;
                if (mismatched <= 4) {
                    LOG("03 render %d: %u pixels differ from render 0\n", n, differ);
                }
            }
        }
        done++;
        if (done % 100 == 0) {
            LOG("03: %d renders, TA max %u us, 3D max %u us\n", done, ta_max, d3_max);
        }
    }
    recipe_diag(diag);
    unsigned faults = 0;
    for (unsigned n = 0; n < RECIPE_DIAG_COUNT; n++) {
        if ((recipe_diag_regs[n] & 0xfff) == 0xc08) {
            faults |= diag[n];
        }
    }
    LOG("03: %d of %d renders; event %08x/%08x/%08x ev2 %08x/%08x/%08x bif faults %08x\n", done,
        renders, diag[0], diag[1], diag[2], diag[3], diag[4], diag[5], faults);
    failures += done != renders || mismatched || faults || bg != 3584 || tri != 512;

    // 13d: the probe's runs, an SLC flush first (its log is in uncached memory).
    const volatile uint32_t *mk_log = mk ? (const volatile uint32_t *)(mk + NC_RESULT) : NULL;
    if (mk_log) {
        slc(0);
        for (unsigned n = 0; n < MK_LOG_WORDS; n += 4) {
            LOG("03 mk log %2u: %08x %08x %08x %08x\n", n, mk_log[n], mk_log[n + 1], mk_log[n + 2],
                mk_log[n + 3]);
        }
        sgxdUnmap(NC_GPU_VA, NC_SIZE);
    }
    const int unmapped = mapped == 0 ? sgxdUnmap(RECIPE_GPU_VA, size) : 0;
    const int released = sgxdRelease();
    failures += unmapped != 0 || released != 0;
    LOG("03 unmap %08x release %d\n", (unsigned)unmapped, released);

    len += snprintf(json + len, sizeof(json) - len,
                    "{\"test\": \"03-recipe\", \"background\": \"%s\", \"size\": %u, \"gpu_va\": \"0x%08x\", \"pages\": %u, "
                    "\"footprint\": \"0x%x\", \"block\": \"0x%x\",\n \"take\": %d, \"take_us\": %u, "
                    "\"map\": \"0x%08x\", \"map_us\": %u, \"stage_us\": %u, \"init\": %d,\n"
                    " \"renders_requested\": %d, \"renders_done\": %d, \"failed\": \"%s\",\n"
                    " \"first\": {\"ta_us\": %u, \"3d_us\": %u, \"background\": %d, \"triangle\": %d, "
                    "\"other\": %d},\n \"mismatched_renders\": %d, \"ta_us_mean\": %u, \"ta_us_max\": %u, "
                    "\"3d_us_mean\": %u, \"3d_us_max\": %u,\n \"diag\": {",
                    sa_bg ? "sa" : "shader", RECIPE_SIZE, RECIPE_GPU_VA, RECIPE_PAGES, r.offset[SGXD_R_END], size, taken,
                    info.take_us, (unsigned)mapped, map_us, stage_us, init, renders, done, failed,
                    ta_us, d3_us, bg, tri, other, mismatched,
                    done ? (uint32_t)(ta_sum / done) : 0, ta_max, done ? (uint32_t)(d3_sum / done) : 0,
                    d3_max);
    for (unsigned n = 0; n < RECIPE_DIAG_COUNT; n++) {
        len += snprintf(json + len, sizeof(json) - len, "%s\"0x%05x\": \"0x%08x\"", n ? ", " : "",
                        recipe_diag_regs[n], diag[n]);
    }
    len += snprintf(json + len, sizeof(json) - len, "},\n \"events\": {");
    for (unsigned phase = 0; phase < EVENT_PHASES; phase++) {
        len += snprintf(json + len, sizeof(json) - len, "%s\"%s\": [", phase ? ", " : "",
                        event_phase_names[phase]);
        for (unsigned n = 0; n < 12; n++) {
            len += snprintf(json + len, sizeof(json) - len, "%s\"0x%08x\"", n ? ", " : "",
                            event_snap[phase][n]);
        }
        len += snprintf(json + len, sizeof(json) - len, "]");
    }
    len += snprintf(json + len, sizeof(json) - len, "}");
    if (mk_log) {
        len += snprintf(json + len, sizeof(json) - len, ",\n \"mk_log\": [");
        for (unsigned n = 0; n < MK_LOG_WORDS; n++) {
            len += snprintf(json + len, sizeof(json) - len, "%s\"0x%08x\"", n ? ", " : "", mk_log[n]);
        }
        len += snprintf(json + len, sizeof(json) - len, "]");
    }
    len += snprintf(json + len, sizeof(json) - len,
                    ",\n \"unmap\": \"0x%08x\", \"release\": %d, \"result\": \"%s\"}\n",
                    (unsigned)unmapped, released, failures ? "FAIL" : "PASS");
    char path[96];
    out_path(path, sizeof(path), "03-recipe.json");
    write_file(path, json, (unsigned)len);
    sceKernelFreeMemBlock(block);
    if (mk_block >= 0) {
        sceKernelFreeMemBlock(mk_block);
    }
    LOG("03-recipe %s\n", failures ? "FAIL" : "PASS");
    return failures;
}

// ---------------------------------------------------------------------------- test 17

// The microkernel's GPU space (sgxd_mkq.h), as azahar's.
#define CL_MK_VA 0x00400000u

static int test_compiled(int renders, int ptla) {
    static char json[4096];
    static char failed[160];
    static SgxdOp list[SGXD_MAX_CHAIN_OPS];
    int len = 0, failures = 0;
    SgxdInfo info = {.size = sizeof(info)};
    SgxdRecipe r;
    if (sgxd_recipe_init(&r, RECIPE_SIZE, RECIPE_SIZE, RECIPE_GPU_VA, RECIPE_PAGES) < 0) {
        return 1;
    }
    const uint32_t kicker_offset = (r.offset[SGXD_R_END] + 0xfff) & ~0xfffu;
    const uint32_t size = (kicker_offset + 0x1000 + 0xffff) & ~0xffffu;
    const SceUID block = sceKernelAllocMemBlock("sgxd_test_recipe", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW,
                                                size, NULL);
    void *base = NULL;
    if (block < 0 || sceKernelGetMemBlockBase(block, &base) < 0) {
        LOG("17: memory 0x%08x\n", (unsigned)block);
        return 1;
    }
    uint8_t *memory = base;
    sgxd_recipe_write(&r, memory);
    *(uint32_t *)(memory + kicker_offset) = GPU_KICKER_VALUE;
    const volatile uint32_t *fb = (const volatile uint32_t *)(memory + r.offset[SGXD_R_FB]);

    if (sgxdTake(&info) < 0) {
        return 1;
    }
    const int mapped = sgxdMap(RECIPE_GPU_VA, base, size, 0);
    failures += mapped != 0;
    InitAddresses addresses;
    sgxd_session_recipe_addresses(&r, RECIPE_GPU_VA + kicker_offset, &addresses);
    int init = -1;
    if (mapped == 0) {
        init = slc(1) < 0 ? -1 : init_script(&addresses);
    }
    failures += init != 0;
    static SgxdEvents events;
    memset(&events, 0, sizeof(events));
    if (init == 0 && sgxd_ptla_events(&events, CL_MK_VA, failed, sizeof(failed)) < 0) {
        LOG("17: %s\n", failed);
        init = -1;
        failures++;
    }
    // The list built as azahar builds it, once the microkernel runs (its kicks' SLC form,
    // sgxd_recipe_events_owned).
    SgxdOpList l = {list, 0, SGXD_MAX_CHAIN_OPS, 0};
    sgxd_session_render_list(&r, 0, 0, &l);
    if (l.overflow) {
        init = -1;
        failures++;
    }
    volatile uint32_t *state = sgxd_mkq_state();
    // The list's code kept as a kept list's is (sgxd_session): the sends after the first skip the
    // shape lookup.
    SgxdMkqCode code = {0, 0};
    int done = 0, bad_pictures = 0, bad_fills = 0;
    uint32_t us_first = 0, us_max = 0;
    uint64_t us_sum = 0;
    for (int n = 0; init == 0 && n < renders; n++) {
        if (n > 0) {
            if (recipe_phase(&r, sgxd_session_slc_inval, "slc", failed, sizeof(failed)) < 0) {
                break;
            }
            sgxd_recipe_restage(&r, memory);
        }
        const uint32_t chains = sgxd_chain_done();
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        const int sent = sgxd_chain_submit_vars(list, l.count, NULL, &code);
        if (sent < 0) {
            LOG("17: sgxd_chain_submit %08x (refused op %u, kind %u)\n", (unsigned)sent,
                sgxd_mkq_refused, sgxd_mkq_refused_kind);
            failures++;
            break;
        }
        int filled = 1;
        for (int k = 0; k < ptla && filled; k++) {
            const SgxdPtlaFill f = {RECIPE_GPU_VA + kicker_offset + 0x800u, SGXD_PTLA_8888, 256, 64, 1, 0,
                                    0xa5000000u | (uint32_t)n << 8 | (uint32_t)k};
            const SgxdPtlaCopy c = {RECIPE_GPU_VA + r.offset[SGXD_R_FB], RECIPE_GPU_VA + kicker_offset + 0xa00u,
                                    SGXD_PTLA_8888, 256, 64, 1, 0, 0};
            filled = (k & 1) || k == ptla - 1 ? sgxd_ptla_fill(&f, NULL, failed, sizeof(failed)) == 0
                                              : sgxd_ptla_copy(&c, NULL, failed, sizeof(failed)) == 0;
            if (!filled) {
                LOG("17 render %d fill %d: %s\n", n, k, failed);
                failures++;
            }
        }
        const uint32_t lists = 1u + (uint32_t)ptla;
        uint64_t now = t0;
        while (sgxd_chain_done() - chains < lists && !sgxd_chain_faulted() && now - t0 < 1000000) {
            now = sceKernelGetProcessTimeWide();
        }
        const uint32_t us = (uint32_t)(now - t0);
        if (ptla > 0) {
            const uint32_t want = 0xa5000000u | (uint32_t)n << 8 | (uint32_t)(ptla - 1);
            const uint32_t got = *(const volatile uint32_t *)(memory + kicker_offset + 0x800u);
            bad_fills += got != want;
        }
        if (sgxd_chain_done() - chains < lists) {
            sgxd_session_render_async_failed(failed, sizeof(failed));
            LOG("17 render %d: no end in %u us: %s\n", n, us, failed);
            failures++;
            break;
        }
        int bg = 0, tri = 0;
        for (unsigned p = 0; p < RECIPE_SIZE * RECIPE_SIZE; p++) {
            bg += fb[p] == SGXD_RECIPE_BG;
            tri += fb[p] == SGXD_RECIPE_TRI;
        }
        bad_pictures += bg != 3584 || tri != 512;
        if (n == 0) {
            us_first = us;
            LOG("17 render 0: kick to end %u us; %d background, %d triangle\n", us, bg, tri);
        }
        us_sum += us;
        us_max = us > us_max ? us : us_max;
        done++;
    }
    failures += done != renders || bad_pictures || bad_fills;
    int refusal_ok = 1;
    if (state && init == 0) {
        // The microkernel queue adds the signals itself (sgxd_mkq_submit).
        const SgxdOp bad[2] = {W(0x0240, 0x358637bdu), W(0x0c84, 0)};
        const int refused = sgxd_chain_submit(bad, 2);
        refusal_ok = refused == SGX_USSE_EINVAL && sgxd_mkq_refused == 1;
        LOG("17: the BIF write: %08x, refused at %u -> %s\n", (unsigned)refused, sgxd_mkq_refused,
            refusal_ok ? "PASS" : "FAIL");
        failures += !refusal_ok;
    }
    LOG("17: %d of %d renders, %d wrong pictures, %d wrong fills (%d a render), %u ops; kick to end "
        "%u us first, %u mean, %u max; IRQ 52 %u (SW_EVENT %u)\n",
        done, renders, bad_pictures, bad_fills, ptla, l.count, us_first,
        done ? (uint32_t)(us_sum / done) : 0, us_max, events.irqs, events.sw_events);
    LOG("17 microkernel: %u shapes, %u instructions\n", sgxd_mkq_compiles, sgxd_mkq_instructions);
    if (init == 0) {
        sgxd_ptla_events(NULL, 0, failed, sizeof(failed));
    }
    const int unmapped = mapped == 0 ? sgxdUnmap(RECIPE_GPU_VA, size) : 0;
    const int released = sgxdRelease();
    failures += unmapped != 0 || released != 0;
    len += snprintf(json + len, sizeof(json) - len,
                    "{\"test\": \"17-compiled\", \"ptla\": %d, \"wrong_fills\": %d, "
                    "\"list_ops\": %u, \"shapes\": %u, "
                    "\"instructions\": %u, \"refusal\": %d,\n \"renders_requested\": %d, "
                    "\"renders_done\": %d, \"wrong_pictures\": %d, \"kick_to_end_us\": "
                    "{\"first\": %u, \"mean\": %u, \"max\": %u}, \"irqs\": %u, "
                    "\"sw_events\": %u, \"unmap\": \"0x%08x\", "
                    "\"release\": %d, \"result\": \"%s\"}\n",
                    ptla, bad_fills, l.count, sgxd_mkq_compiles, sgxd_mkq_instructions,
                    refusal_ok, renders, done, bad_pictures, us_first,
                    done ? (uint32_t)(us_sum / done) : 0, us_max, events.irqs, events.sw_events,
                    (unsigned)unmapped, released, failures ? "FAIL" : "PASS");
    char path[96];
    out_path(path, sizeof(path), "17-compiled.json");
    write_file(path, json, (unsigned)len);
    {
        uint32_t instructions = 0;
        const volatile uint64_t *code = sgxd_mkq_code(&instructions);
        static uint64_t words[16384];
        const uint32_t count = code && instructions < 16384 ? instructions : code ? 16384 : 0;
        for (uint32_t n = 0; n < count; n++) {
            words[n] = code[n];
        }
        out_path(path, sizeof(path), "17-compiled.bin");
        write_file(path, words, count * 8);
    }
    sceKernelFreeMemBlock(block);
    LOG("17-compiled %s\n", failures ? "FAIL" : "PASS");
    return failures;
}

// Test 04: the display. The recipe at 960x544 (a 4x4 macrotile grid) draws a quad over
// the whole target with DCMP never, which puts every tile in the scene: the hardware leaves a tile
// no primitive touches as it was (without the quad, old frames show there). Then two rectangles in
// the triangle's colour over a dark background through SA, into three CDRAM buffers mapped at
// FLIP_GPU_VA, and flips them: sceDisplayWaitSetFrameBuf, then sceDisplaySetFrameBuf(NEXTFRAME), so
// a buffer is rendered only while neither shown nor pending. The rectangles move 4 px a frame.
// Every check-th frame (and the last) is compared with the CPU's image of it; then the hold frame
// (the layout at 0) is rendered, compared and written to 04-frame.bin. After the release it stays
// on screen for hold seconds, for a screendump or a VCP capture.
#define FLIP_W 960u
#define FLIP_H 544u
#define FLIP_BUFFERS 3u
#define FLIP_BUFFER_SIZE 0x200000u   // 960 * 544 * 4 rounded up to CDRAM's 256 KiB
#define FLIP_GPU_VA 0x40000000u      // the display buffers
#define FLIP_BG 0xff201008u           // A8B8G8R8, dark blue
#define FLIP_BLACK 0xff000000u
static uint32_t flip_bg = FLIP_BG;    // FLIP_BLACK with black=1
#define FLIP_RECTS 2u

typedef struct FlipRect {
    int x0, y0, x1, y1;   // pixels [x0, x1) x [y0, y1)
} FlipRect;

// Frame f's rectangles; f < 0 is the hold frame, the layout at 0. Edges on whole pixels, so
// every pixel centre is half a pixel from an edge and the image does not depend on fill rules.
// The sweep (260 px) is longer than the first rectangle's width (256), so no column stays lit
// through a long loop.
// Test 07's sweep=1: frame f draws layout f twice (both rectangles), and the region goes to
// 07-sweep-FF.bin. Tile-aligned and offset corners, power-of-two and odd sizes, horizontal and
// vertical extents, and the hold frame's two rectangles (16, 17).
static const FlipRect flip_sweep_rects[] = {
    {0, 0, 256, 32},    {0, 0, 512, 32},     {0, 0, 960, 32},      {1, 0, 257, 32},
    {16, 0, 272, 32},   {31, 0, 287, 32},    {0, 0, 255, 32},      {0, 0, 257, 32},
    {0, 0, 192, 32},    {0, 0, 100, 32},     {0, 0, 32, 256},      {0, 0, 32, 544},
    {0, 1, 32, 257},    {0, 16, 32, 176},    {0, 0, 32, 160},      {64, 64, 128, 128},
    {96, 96, 352, 256}, {560, 160, 752, 256}, {0, 0, 960, 544},    {100, 50, 800, 450},
    {480, 0, 960, 32},  {0, 272, 960, 304},  {7, 3, 40, 38},       {300, 200, 364, 202},
};
#define FLIP_SWEEP_COUNT (sizeof(flip_sweep_rects) / sizeof(flip_sweep_rects[0]))
static int flip_sweep;

static int flip_e4;   // test 08: the texture, 1:1 point-sampled and 2x bilinear
static int flip_fsweep;   // test 08's fsweep=N: the filter-weight sweep (1 U8888, 2 F16x4)
static int flip_e5;   // test 09: blending, two translucent rectangles overlapping
static int flip_e5_opaque;   // test 09's opaque=1: the opaque pass type
static int flip_e5_stencil;  // test 09's stencil=N
// pack=WORD (any flip test): the target's PBE word 0 instead of the recipe's, to see how a pack
// mode stores the scene's known colours; the frame file holds the buffer's bytes as stored.
static uint32_t flip_pack;
static int flip_f1;          // test 12: texel * colour, blended (test 08's texture, point)
static int flip_show_frames = 1;   // show=0: no flips (test 10's long runs)

// Test 10's grid: draw n (1..E6_DRAWS) is cell (n - 1) % E6_COLS, (n - 1) / E6_COLS, its
// rectangle the cell less its last column and row. Draw 0 covers nothing, as in the other tests.
#define E6_COLS 50u
#define E6_ROWS 40u
#define E6_DRAWS (E6_COLS * E6_ROWS)
#define E6_X0 5
#define E6_Y0 12
#define E6_CW 19
#define E6_CH 13
static int flip_e6;
static float e6_verts[(1 + E6_DRAWS) * 4 * 4];
static uint16_t e6_indices[8 * (1 + E6_DRAWS)];
static SgxdRecipeDraw e6_draws[1 + E6_DRAWS];
static uint32_t e6_colours[1 + E6_DRAWS];
static uint32_t flip_pages_min, flip_pages_max;   // PAGE_STATUS's high half after each TA

// Test 11: frame f's program immediate, and the pixels checked by the age of the program they
// show (e7_count's buckets).
static int flip_e7;
static int flip_e7_keep;   // inval=0: the kicks leave the USE code caches alone
static int e7_frames;      // the hold frame's index, after frames 0 .. e7_frames - 1
#define E7_HOLD_IMM 0xa5c3u
enum { E7_AGE0, E7_AGE1, E7_AGE2, E7_AGE3, E7_AGE10, E7_OR, E7_NONE, E7_BUCKETS };
static uint32_t e7_ages[E7_BUCKETS];
static uint32_t e7_imm(int f) {
    return f < 0 ? E7_HOLD_IMM : 1u + (unsigned)f % 0x7fffu;
}
// A pixel of frame f whose colour, less the program's xor, was base.
static void e7_count(int f, uint32_t got, uint32_t base) {
    const uint32_t d = got ^ base;
    const int n = f < 0 ? e7_frames : f;
    int g;
    if (d >> 16) {
        e7_ages[E7_NONE]++;
        return;
    }
    if (!d) {
        e7_ages[E7_OR]++;
        return;
    }
    if (d == E7_HOLD_IMM) {
        g = e7_frames;
    } else if (d <= 0x7fffu) {
        g = (int)d - 1;   // frames under 0x7fff
    } else {
        e7_ages[E7_NONE]++;
        return;
    }
    const int age = n - g;
    e7_ages[age < 0 ? E7_NONE : age < 3 ? E7_AGE0 + age : age < 10 ? E7_AGE3 : E7_AGE10]++;
}

// Frame f's window for the even draws: tiles x0..x0 + 9, y0..y0 + 7 of the 30x17, a tile to the
// right each frame and a row down every 21 (the hold frame as frame 0).
static uint32_t e6_clip(int f) {
    const unsigned k = f < 0 ? 0 : (unsigned)f, x0 = k % 21, y0 = (k / 21) % 10;
    return SGXD_RECIPE_CLIP(x0, y0, x0 + 9, y0 + 7);
}
static uint32_t e6_colour(int f, unsigned n) {
    uint32_t v = n * 2654435761u + (uint32_t)(f < 0 ? 0 : f) * 40503u;
    v ^= v >> 15;
    v *= 0x2c1b3c6du;
    v ^= v >> 12;
    return 0xff000000u | (v & 0xffffffu);
}
static void e6_setup(void) {
    for (unsigned n = 0; n <= E6_DRAWS; n++) {
        const unsigned c = (n - 1) % E6_COLS, row = (n - 1) / E6_COLS;
        const int x0 = n ? E6_X0 + (int)(c * E6_CW) : 0, y0 = n ? E6_Y0 + (int)(row * E6_CH) : 0;
        const int xs[2] = {x0, n ? x0 + E6_CW - 1 : (int)FLIP_W};
        const int ys[2] = {y0, n ? y0 + E6_CH - 1 : (int)FLIP_H};
        for (unsigned v = 0; v < 4; v++) {
            float *out = &e6_verts[(n * 4 + v) * 4];
            out[0] = (xs[v & 1] - FLIP_W / 2.0f) / (FLIP_W / 2.0f);
            out[1] = (ys[v >> 1] - FLIP_H / 2.0f) / (FLIP_H / 2.0f);
            out[2] = 0.5f;
            out[3] = 1.0f;
        }
        static const uint16_t quad[6] = {0, 1, 2, 2, 1, 3};
        for (unsigned i = 0; i < 6; i++) {
            e6_indices[8 * n + i] = (uint16_t)(n * 4 + quad[i]);
        }
        e6_draws[n] = (SgxdRecipeDraw){8 * n, 6, n ? SGXD_RECIPE_ISP_A : SGXD_RECIPE_ISP_NEVER,
                                       SGXD_R_FPRIM, 0, 0};
    }
}

static void flip_layout(int f, FlipRect rects[FLIP_RECTS]) {
    if (flip_sweep && f >= 0) {
        rects[0] = rects[1] = flip_sweep_rects[(unsigned)f % FLIP_SWEEP_COUNT];
        return;
    }
    const int d = f < 0 ? 0 : (f * 4) % 260;
    if (flip_f1) {
        // Both point-sampled, 1:1; the second over the first while d < 192.
        rects[0] = (FlipRect){96 + d, 96, 224 + d, 224};
        rects[1] = (FlipRect){160, 128 + d / 4, 288, 256 + d / 4};
        return;
    }
    if (flip_fsweep) {
        rects[0] = (FlipRect){96, 96, 224, 224};
        rects[1] = (FlipRect){400, 16, 912, 272};
        return;
    }
    if (flip_e4) {
        rects[0] = (FlipRect){96 + d, 96, 224 + d, 224};
        rects[1] = (FlipRect){560, 16 + d, 816, 272 + d};
        return;
    }
    if (flip_e5) {
        // The second moves down as the first moves right, and they overlap in every frame.
        rects[0] = (FlipRect){96 + d, 96, 352 + d, 256};
        rects[1] = (FlipRect){320, 128 + d / 4, 704, 320 + d / 4};
        return;
    }
    rects[0] = (FlipRect){96 + d, 96, 352 + d, 256};
    rects[1] = (FlipRect){560, 160 + d, 752, 256 + d};
}

// Quad 0 is the whole target, quads 1.. the rectangles. Draw 0's indices take 8 slots, as a draw's
// first index must be a multiple of 8. Test 05 draws each rectangle on its own, its
// colour through SA, so its indices start at 8 * n.
static float flip_verts[(1 + FLIP_RECTS) * 4 * 4];
static uint16_t flip_indices[8 * FLIP_RECTS + 6];
static int flip_e1;   // tests 05 and 06: each rectangle its own draw and colour
static int flip_e2;   // tests 06 and 07: ImGui vertices
static int flip_e3;   // test 07: a colour and a UV per corner, the pixel (r, g, u, v)
static unsigned flip_fs;                 // SGXD_IMGUI_* of tests 06 and 07
static uint32_t flip_byte_error[3];      // test 07: checked bytes 0, 1 and 2+ from the value
static int flip_raw = -1;                // test 07's raw=N: pa N's bits, rectangles unchecked
static uint32_t flip_imgui[(1 + FLIP_RECTS) * 4 * SGXD_RECIPE_IMGUI_WORDS];
static const SgxdRecipeDraw flip_draws[2] = {
    {0, 6, SGXD_RECIPE_ISP_NEVER, SGXD_R_FPRIM, 0, 0},
    {8, FLIP_RECTS * 6, SGXD_RECIPE_ISP_A, SGXD_R_FPRIM, 0, 0},
};
static const SgxdRecipeDraw flip_draws_e1[1 + FLIP_RECTS] = {
    {0, 6, SGXD_RECIPE_ISP_NEVER, SGXD_R_FPRIM, 0, 0},
    {8, 6, SGXD_RECIPE_ISP_A, SGXD_R_FPRIM, 0, 0},
    {16, 6, SGXD_RECIPE_ISP_A, SGXD_R_FPRIM, 0, 0},
};
static const SgxdRecipeDraw flip_draws_e5[1 + FLIP_RECTS] = {
    {0, 6, SGXD_RECIPE_ISP_NEVER, SGXD_R_FPRIM, 0, 0},
    {8, 6, SGXD_RECIPE_ISP_TRANS, SGXD_R_FPRIM, 0, 0},
    {16, 6, SGXD_RECIPE_ISP_TRANS, SGXD_R_FPRIM, 0, 0},
};
#define FLIP_E1_COLOURS 0x800u   // in the kicker's page: the colour words and the secondaries

// Test 05's colours, A8B8G8R8: frame f's rectangle n takes entry (f + n) % 4 (the hold frame
// as frame 0), so each draw's word changes every frame. Draw 0 draws nothing.
static const uint32_t flip_palette[4] = {0xff00a000u, 0xff0060c0u, 0xffa00060u, 0xff60a0a0u};
// Test 06's: four different bytes in each word, alpha among them (the pack writes all four).
static const uint32_t flip_palette_e2[4] = {0xc0804020u, 0x1f5a96d2u, 0xfe01807fu, 0x40c0e010u};
static void flip_colours(int f, uint32_t colours[1 + FLIP_RECTS]) {
    const uint32_t *palette = flip_e2 ? flip_palette_e2 : flip_palette;
    colours[0] = palette[0];
    for (unsigned n = 0; n < FLIP_RECTS; n++) {
        colours[1 + n] = palette[((f < 0 ? 0 : f) + n) % 4];
    }
}

static void flip_scene(int f) {
    FlipRect rects[1 + FLIP_RECTS] = {{0, 0, FLIP_W, FLIP_H}};
    flip_layout(f, rects + 1);
    for (unsigned n = 0; n < 1 + FLIP_RECTS; n++) {
        const int xs[2] = {rects[n].x0, rects[n].x1}, ys[2] = {rects[n].y0, rects[n].y1};
        for (unsigned v = 0; v < 4; v++) {
            float *out = &flip_verts[(n * 4 + v) * 4];
            out[0] = (xs[v & 1] - FLIP_W / 2.0f) / (FLIP_W / 2.0f);
            out[1] = (ys[v >> 1] - FLIP_H / 2.0f) / (FLIP_H / 2.0f);
            out[2] = 0.5f;
            out[3] = 1.0f;
        }
        static const uint16_t quad[6] = {0, 1, 2, 2, 1, 3};
        for (unsigned i = 0; i < 6; i++) {
            flip_indices[(!n ? 0 : flip_e1 ? 8 * n : 8 + (n - 1) * 6) + i] =
                (uint16_t)(n * 4 + quad[i]);
        }
    }
}

// Test 06's vertices: quad n's corners as ImDrawVert records, x and y in pixels, u and v
// distinct non-zero floats (a wrong stride would read them as the colour), colour colours[n].
// The indices are test 05's.
static void flip_scene_imgui(int f) {
    FlipRect rects[1 + FLIP_RECTS] = {{0, 0, FLIP_W, FLIP_H}};
    uint32_t colours[1 + FLIP_RECTS];
    flip_layout(f, rects + 1);
    flip_colours(f, colours);
    for (unsigned n = 0; n < 1 + FLIP_RECTS; n++) {
        const int xs[2] = {rects[n].x0, rects[n].x1}, ys[2] = {rects[n].y0, rects[n].y1};
        for (unsigned v = 0; v < 4; v++) {
            uint32_t *out = &flip_imgui[(n * 4 + v) * SGXD_RECIPE_IMGUI_WORDS];
            float words[4] = {(float)xs[v & 1], (float)ys[v >> 1], 0.125f * (n * 4 + v + 1),
                              0.0625f * (n * 4 + v + 1)};
            uint32_t colour = colours[n];
            if (flip_e4) {
                // texel 2's.
                const int span = flip_fsweep && n == 2;
                const float lo = span ? 1.5f / 128.0f : 0.0f, hi = span ? 2.5f / 128.0f : 1.0f; // FLIP_TEX
                words[2] = (v & 1) ? hi : lo;
                words[3] = (v >> 1) ? hi : lo;
            }
            if (flip_e3) {
                // The corners of test 07's planes: red 0 or 255, green 0 or 255, blue by
                // rectangle (iterated, not written: the pixel's B and A are u and v); u 1 or 0,
                // v 0.25 or 0.75.
                words[2] = (v & 1) ? 0.0f : 1.0f;
                words[3] = (v >> 1) ? 0.75f : 0.25f;
                colour = 0xff000000u | (0x40u * (n + 1)) << 16 | ((v >> 1) ? 0xff00u : 0) |
                         ((v & 1) ? 0xffu : 0);
            }
            memcpy(out, words, sizeof(words));
            out[4] = colour;
        }
    }
}

// Test 07's pixel at (x, y) of rectangle rect: (r, g, u, v) at the pixel centre as unorm values
// (not rounded).
static void flip_e3_value(const FlipRect *rect, unsigned x, unsigned y, float value[4]) {
    const float fx = ((float)x + 0.5f - (float)rect->x0) / (float)(rect->x1 - rect->x0);
    const float fy = ((float)y + 0.5f - (float)rect->y0) / (float)(rect->y1 - rect->y0);
    value[0] = fx * 255.0f;
    value[1] = fy * 255.0f;
    value[2] = (1.0f - fx) * 255.0f;
    value[3] = (0.25f + 0.5f * fy) * 255.0f;
}

// Test 08's texture: 128x128 linear A8B8G8R8, red 2x, green 2y, blue and alpha mixing both, so
// every texel differs from its neighbours in each direction.
#define FLIP_TEX 128u
static uint32_t flip_texel(unsigned x, unsigned y) {
    if (flip_fsweep) {
        const uint32_t r = (x & 1) ? 0xffu : 0, g = (y & 1) ? 0xffu : 0,
                       b = ((x + y) & 1) ? 0xffu : 0, a = ((x ^ y) & 1) ? 0xffu : 0x80u;
        return a << 24 | b << 16 | g << 8 | r;
    }
    const uint32_t b = (x * 5u + y * 3u) & 0xffu, a = 0x80u | ((x ^ y) & 0x7fu);
    return a << 24 | b << 16 | (2u * y) << 8 | 2u * x;
}
static void flip_texture_words(uint32_t va, int bilinear, uint32_t words[4]) {
    if (flip_fsweep == 2) {
        sgxd_recipe_texture_words_format(va, FLIP_TEX, FLIP_TEX, bilinear, 0x1b, 0, words); // F16x4
        return;
    }
    sgxd_recipe_texture_words(va, FLIP_TEX, FLIP_TEX, bilinear, words);
}
// Test 08's second rectangle at (x, y): the texture magnified 2x, bilinear with the edges clamped,
// one unorm value a byte (not rounded).
static void flip_e4_value(const FlipRect *rect, unsigned x, unsigned y, float value[4]) {
    float tx = ((float)x + 0.5f - (float)rect->x0) / 2.0f - 0.5f;
    float ty = ((float)y + 0.5f - (float)rect->y0) / 2.0f - 0.5f;
    if (flip_fsweep) {
        tx = 1.0f + ((float)x + 0.5f - (float)rect->x0) / (float)(rect->x1 - rect->x0);
        ty = 1.0f + ((float)y + 0.5f - (float)rect->y0) / (float)(rect->y1 - rect->y0);
    }
    // Both are at least -0.25, so the floor is the truncation of one more, less one.
    const int x0 = (int)(tx + 1.0f) - 1, y0 = (int)(ty + 1.0f) - 1;
    const float fx = tx - (float)x0, fy = ty - (float)y0;
    uint32_t t[4];
    for (unsigned k = 0; k < 4; k++) {
        int u = x0 + (int)(k & 1), v = y0 + (int)(k >> 1);
        u = u < 0 ? 0 : u > (int)FLIP_TEX - 1 ? (int)FLIP_TEX - 1 : u;
        v = v < 0 ? 0 : v > (int)FLIP_TEX - 1 ? (int)FLIP_TEX - 1 : v;
        t[k] = flip_texel((unsigned)u, (unsigned)v);
    }
    for (unsigned c = 0; c < 4; c++) {
        float b[4];
        for (unsigned k = 0; k < 4; k++) {
            b[k] = (float)((t[k] >> (8 * c)) & 0xff);
        }
        value[c] = (b[0] * (1 - fx) + b[1] * fx) * (1 - fy) + (b[2] * (1 - fx) + b[3] * fx) * fy;
    }
}

static void flip_blend(uint32_t src, const float dst[4], float out[4]) {
    const float a = (float)(src >> 24);
    for (unsigned c = 0; c < 3; c++) {
        out[c] = ((float)((src >> (8 * c)) & 0xff) * a + dst[c] * (255.0f - a)) / 255.0f;
    }
    out[3] = (a * 255.0f + dst[3] * (255.0f - a)) / 255.0f;
}
// Test 09's pixel: the background, then each rectangle over it that covers the pixel, the bytes
// between the layers rounded to nearest.
static void flip_e5_value(const FlipRect rects[FLIP_RECTS], const uint32_t colours[1 + FLIP_RECTS],
                          unsigned x, unsigned y, float value[4]) {
    for (unsigned c = 0; c < 4; c++) {
        value[c] = (float)((flip_bg >> (8 * c)) & 0xff);
    }
    for (unsigned n = 0; n < FLIP_RECTS; n++) {
        const int in = (int)x >= rects[n].x0 && (int)x < rects[n].x1 && (int)y >= rects[n].y0 &&
                       (int)y < rects[n].y1;
        // stencil=N: the second rectangle passes over the first (1, 2) or outside it (3).
        const int over_first = (int)x >= rects[0].x0 && (int)x < rects[0].x1 &&
                               (int)y >= rects[0].y0 && (int)y < rects[0].y1;
        if (flip_e5_stencil && n == 1 && over_first != (flip_e5_stencil != 3)) {
            continue;
        }
        // Opaque: the ISP keeps only the last rectangle's tag, so what is under it is the
        // background.
        if (!in || (flip_e5_opaque && n + 1 < FLIP_RECTS &&
                    (int)x >= rects[n + 1].x0 && (int)x < rects[n + 1].x1 &&
                    (int)y >= rects[n + 1].y0 && (int)y < rects[n + 1].y1)) {
            continue;
        }
        float under[4];
        for (unsigned c = 0; c < 4; c++) {
            under[c] = (float)(int)(value[c] + 0.5f);
        }
        flip_blend(colours[1 + n], under, value);
    }
}

// Test 12's texel times colour, per byte, rounded to nearest (the SOP2's rule for a blend).
static uint32_t flip_modulate(uint32_t texel, uint32_t colour) {
    uint32_t out = 0;
    for (unsigned c = 0; c < 4; c++) {
        const uint32_t t = (texel >> (8 * c)) & 0xff, k = (colour >> (8 * c)) & 0xff;
        out |= ((t * k + 127) / 255) << (8 * c);
    }
    return out;
}
// Test 12's pixel: test 09's layers with each rectangle's colour the product at the pixel.
static void flip_f1_value(const FlipRect rects[FLIP_RECTS], const uint32_t colours[1 + FLIP_RECTS],
                          unsigned x, unsigned y, float value[4]) {
    for (unsigned c = 0; c < 4; c++) {
        value[c] = (float)((flip_bg >> (8 * c)) & 0xff);
    }
    for (unsigned n = 0; n < FLIP_RECTS; n++) {
        if ((int)x < rects[n].x0 || (int)x >= rects[n].x1 || (int)y < rects[n].y0 ||
            (int)y >= rects[n].y1) {
            continue;
        }
        float under[4];
        for (unsigned c = 0; c < 4; c++) {
            under[c] = (float)(int)(value[c] + 0.5f);
        }
        const uint32_t texel = flip_texel(x - (unsigned)rects[n].x0, y - (unsigned)rects[n].y0);
        flip_blend(flip_modulate(texel, colours[1 + n]), under, value);
    }
}

static unsigned e6_check(const volatile uint32_t *buffer, int f, uint32_t *where) {
    const uint32_t clip = e6_clip(f);
    const unsigned tx0 = clip >> 24 & 0x3f, tx1 = clip >> 16 & 0x3f;
    const unsigned ty0 = clip >> 8 & 0x3f, ty1 = clip & 0x3f;
    unsigned differ = 0;
    for (unsigned y = 0; y < FLIP_H; y++) {
        for (unsigned x = 0; x < FLIP_W; x++) {
            uint32_t want = flip_bg;
            if (x >= E6_X0 && y >= E6_Y0) {
                const unsigned c = (x - E6_X0) / E6_CW, row = (y - E6_Y0) / E6_CH;
                const unsigned n = 1 + row * E6_COLS + c;
                const int clipped = !(n & 1) && (x / 32 < tx0 || x / 32 > tx1 || y / 32 < ty0 ||
                                                 y / 32 > ty1);
                if (c < E6_COLS && row < E6_ROWS && (x - E6_X0) % E6_CW < E6_CW - 1 &&
                    (y - E6_Y0) % E6_CH < E6_CH - 1 && !clipped) {
                    want = e6_colour(f, n);
                }
            }
            if (buffer[y * FLIP_W + x] != want && !differ++) {
                *where = y << 16 | x;
            }
        }
    }
    return differ;
}

// Pixels of frame f's buffer that differ from the CPU's image of it; the first one in *where.
// Test 07's rectangles differ where a byte is more than 1 from its value.
static unsigned flip_check(const volatile uint32_t *buffer, int f, uint32_t *where) {
    if (flip_e6) {
        return e6_check(buffer, f, where);
    }
    FlipRect rects[FLIP_RECTS];
    uint32_t colours[1 + FLIP_RECTS];
    flip_layout(f, rects);
    flip_colours(f, colours);
    unsigned differ = 0;
    for (unsigned y = 0; y < FLIP_H; y++) {
        for (unsigned x = 0; x < FLIP_W; x++) {
            uint32_t want = flip_bg;
            int in = -1;
            for (unsigned n = 0; n < FLIP_RECTS; n++) {
                if ((int)x >= rects[n].x0 && (int)x < rects[n].x1 && (int)y >= rects[n].y0 &&
                    (int)y < rects[n].y1) {
                    want = flip_e1 ? colours[1 + n] : SGXD_RECIPE_TRI;
                    in = (int)n;
                }
            }
            const uint32_t got = buffer[y * FLIP_W + x];
            int bad;
            if (flip_raw >= 0 && in >= 0) {
                bad = 0;
            } else if ((flip_e5 || flip_f1) && in >= 0) {
                float value[4];
                if (flip_f1) {
                    flip_f1_value(rects, colours, x, y, value);
                } else {
                    flip_e5_value(rects, colours, x, y, value);
                }
                bad = 0;
                for (unsigned c = 0; c < 4; c++) {
                    const float d = (float)((got >> (8 * c)) & 0xff) - value[c];
                    const float e = d < 0 ? -d : d;
                    const unsigned k = e <= 0.5f ? 0 : e <= 1.5f ? 1 : 2;
                    flip_byte_error[k]++;
                    bad |= k == 2;
                }
            } else if (flip_e4 && in == 0) {
                bad = got != flip_texel(x - (unsigned)rects[0].x0, y - (unsigned)rects[0].y0);
            } else if ((flip_e3 || flip_e4) && in >= 0) {
                float value[4];
                if (flip_e4) {
                    flip_e4_value(&rects[in], x, y, value);
                } else {
                    flip_e3_value(&rects[in], x, y, value);
                }
                bad = 0;
                for (unsigned c = 0; c < 4; c++) {
                    const float d = (float)((got >> (8 * c)) & 0xff) - value[c];
                    const float e = d < 0 ? -d : d;
                    // Within half a step of the value is its rounding: error 0.
                    const unsigned k = e <= 0.5f ? 0 : e <= 1.5f ? 1 : 2;
                    flip_byte_error[k]++;
                    bad |= k == 2;
                }
            } else if (flip_e7) {
                e7_count(f, got, want);
                bad = got != (want ^ e7_imm(f));
            } else {
                bad = got != want;
            }
            if (bad && !differ++) {
                *where = y << 16 | x;
            }
        }
    }
    return differ;
}

static int flip_show(void *base) {
    SceDisplayFrameBuf frame = {
        .size = sizeof(frame),
        .base = base,
        .pitch = FLIP_W,
        .pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8,
        .width = FLIP_W,
        .height = FLIP_H,
    };
    return sceDisplaySetFrameBuf(&frame, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static int flip_render(const SgxdRecipe *r, char *failed, unsigned failed_size, uint32_t *ta_us,
                       uint32_t *d3_us) {
    SgxdSessionRender render = {.read_pages = flip_e6};
    if (sgxd_session_render(r, &render, failed, failed_size) < 0) {
        LOG("phase stopped: %s\n", failed);
        return -1;
    }
    if (render.pages) {
        flip_pages_min = render.pages < flip_pages_min ? render.pages : flip_pages_min;
        flip_pages_max = render.pages > flip_pages_max ? render.pages : flip_pages_max;
    }
    *ta_us = render.ta_us;
    *d3_us = render.d3_us;
    return 0;
}

// Step E2's vertex SA in the kicker page: after the background's (0x400) and test 05's colours.
#define FLIP_E2_SA 0xa00u
// Step E4's pixel programs, one a draw, in the kicker page too; the texture in the page after.
#define FLIP_E4_PDS 0xc00u

// mode 0: test 04; 1: test 05 (E1); 2: test 06 (E2); 3: test 07 (E3); 4: test 08 (E4); 5: test
// 09 (E5); 6: test 10 (E6); 7: test 11 (E7); 8: test 12 (F's program).
// test 07's sweep: one rectangle's region at a time, x0 y0 x1 y1 first.
static uint32_t flip_sweep_region[4 + FLIP_W * FLIP_H];

static int test_flip(int frames, int check, int hold_s, int black, int mode, int raw, int sweep) {
    static char json[4096];
    static char failed[160];
    static const char *const tags[9] = {"04", "05", "06", "07", "08", "09", "10", "11", "12"};
    static const char *const tests[9] = {"04-flip", "05-colours", "06-vertices", "07-iterators",
                                         "08-texture", "09-blend", "10-draws", "11-code",
                                         "12-modulate"};
    static const char *const frame_files[9] = {"04-frame.bin", "05-frame.bin", "06-frame.bin",
                                               "07-frame.bin", "08-frame.bin", "09-frame.bin",
                                               "10-frame.bin", "11-frame.bin", "12-frame.bin"};
    int failures = 0;
    SgxdInfo info = {.size = sizeof(info)};
    SgxdRecipe r;
    uint32_t colours[1 + FLIP_RECTS];
    const int e1 = mode == 1 || mode == 7;
    flip_bg = black ? FLIP_BLACK : FLIP_BG;
    flip_e1 = mode >= 1;
    flip_e2 = (mode >= 2 && mode <= 5) || mode == 8;
    flip_e3 = mode == 3;
    flip_f1 = mode == 8;
    flip_e4 = mode == 4 || flip_f1;   // test 12 takes test 08's texture and UVs
    flip_e5 = mode == 5;
    flip_e6 = mode == 6;
    flip_e7 = mode == 7;
    e7_frames = frames;
    memset(e7_ages, 0, sizeof(e7_ages));
    flip_pages_min = ~0u;
    flip_pages_max = 0;
    flip_raw = flip_e3 && raw >= 0 && raw <= 5 ? raw : -1;
    flip_sweep = flip_raw >= 0 && sweep;
    frames = flip_sweep ? (int)FLIP_SWEEP_COUNT : frames;
    check = flip_sweep ? 1 : check;
    flip_fs = flip_raw >= 0 ? SGXD_IMGUI_UV_RAW + (unsigned)flip_raw :
              flip_f1 ? SGXD_IMGUI_MODULATE : flip_e4 ? SGXD_IMGUI_TEXEL :
              flip_e5 ? SGXD_IMGUI_BLEND :
              flip_e3 ? SGXD_IMGUI_COLOUR_UV : SGXD_IMGUI_COLOUR;
    SgxdRecipeDraw stencil_draws[1 + FLIP_RECTS];
    memcpy(stencil_draws, flip_draws_e5, sizeof(stencil_draws));
    if (flip_e5 && flip_e5_stencil) {
        const uint32_t cpres = flip_e5_stencil == 2 ? 0 : SGXD_RECIPE_ISP_CPRES;
        for (unsigned n = 1; n <= FLIP_RECTS; n++) {
            stencil_draws[n].isp = SGXD_RECIPE_ISP_TRANS | cpres | 0x5a;
        }
        // SCMP ALWAYS, pass op REPLACE (3) or ZERO (1), write mask 0xff.
        stencil_draws[1].ispc = 7u << 25 | (flip_e5_stencil == 3 ? 1u : 2u) << 16 | 0xff;
        // SCMP EQUAL, every op KEEP, compare mask 0xff.
        stencil_draws[2].ispc = 2u << 25 | 0xffu << 8;
    }
    const SgxdRecipeDraw *const imgui_draws =
        flip_e5 && flip_e5_stencil               ? stencil_draws :
        (flip_e5 && !flip_e5_opaque) || flip_f1 ? flip_draws_e5 :
                                                  flip_draws_e1;
    memset(flip_byte_error, 0, sizeof(flip_byte_error));
    const char *const flip_tag = tags[mode], *const flip_test = tests[mode];
    flip_scene(0);
    flip_scene_imgui(0);
    int initialised;
    if (flip_e2) {
        // The vertex SA's offset follows the layout's end: size the layout first.
        initialised = sgxd_recipe_init_imgui(&r, FLIP_W, FLIP_H, RECIPE_GPU_VA, RECIPE_PAGES,
                                             flip_imgui, (1 + FLIP_RECTS) * 4, flip_indices,
                                             8 * FLIP_RECTS + 6, imgui_draws, 1 + FLIP_RECTS, 0,
                                             flip_fs);
        const uint32_t kicker = (r.offset[SGXD_R_END] + 0xfff) & ~0xfffu;
        initialised = initialised < 0 ? initialised :
            sgxd_recipe_init_imgui(&r, FLIP_W, FLIP_H, RECIPE_GPU_VA, RECIPE_PAGES, flip_imgui,
                                   (1 + FLIP_RECTS) * 4, flip_indices, 8 * FLIP_RECTS + 6,
                                   imgui_draws, 1 + FLIP_RECTS, kicker + FLIP_E2_SA, flip_fs);
    } else if (flip_e6) {
        e6_setup();
        initialised = sgxd_recipe_init_scene(&r, FLIP_W, FLIP_H, RECIPE_GPU_VA, RECIPE_PAGES,
                                             e6_verts, (1 + E6_DRAWS) * 4, e6_indices,
                                             8 * E6_DRAWS + 6, e6_draws, 1 + E6_DRAWS);
    } else {
        initialised = sgxd_recipe_init_scene(&r, FLIP_W, FLIP_H, RECIPE_GPU_VA, RECIPE_PAGES,
                                             flip_verts, (1 + FLIP_RECTS) * 4, flip_indices,
                                             e1 ? 8 * FLIP_RECTS + 6 : 8 + FLIP_RECTS * 6,
                                             e1 ? flip_draws_e1 : flip_draws, e1 ? 1 + FLIP_RECTS : 2);
    }
    if (initialised < 0) {
        LOG("%s: recipe init failed\n", flip_tag);
        return 1;
    }
    r.keep_code_caches = flip_e7 && flip_e7_keep;
    if (flip_e5 && flip_e5_stencil == 3) {
        sgxd_recipe_set_background_stencil(&r, 0x5a);
    }
    const uint32_t kicker_offset = (r.offset[SGXD_R_END] + 0xfff) & ~0xfffu;
    const uint32_t tex_offset = kicker_offset + 0x1000;
    // Test 10's SA blocks go where test 08's texture does.
    const uint32_t size = (tex_offset + (flip_e4 ? FLIP_TEX * FLIP_TEX * 8 : 0) +
                           (flip_e6 ? 0x40 * (E6_DRAWS + 2) : 0) + 0xffff) & ~0xffffu;
    const SceUID block = sceKernelAllocMemBlock("sgxd_test_flip_recipe",
                                                SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_NC_RW, size, NULL);
    const SceUID cdram = sceKernelAllocMemBlock("sgxd_test_flip_buffers",
                                                SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                                FLIP_BUFFERS * FLIP_BUFFER_SIZE, NULL);
    void *base = NULL, *buffers = NULL;
    if (block < 0 || cdram < 0 || sceKernelGetMemBlockBase(block, &base) < 0 ||
        sceKernelGetMemBlockBase(cdram, &buffers) < 0) {
        LOG("%s: memory 0x%08x (%u bytes), CDRAM 0x%08x\n", flip_tag, (unsigned)block, (unsigned)size,
            (unsigned)cdram);
        return 1;
    }
    uint8_t *memory = base;
    const uint64_t stage_t0 = sceKernelGetProcessTimeWide();
    sgxd_recipe_write(&r, memory);
    if (flip_pack) {
        sgxd_recipe_set_target_pack(&r, memory, flip_pack);
    }
    *(uint32_t *)(memory + kicker_offset) = GPU_KICKER_VALUE;
    sgxd_recipe_sa_background(&r, memory, kicker_offset + 0x400, flip_bg);
    if (e1) {
        flip_colours(0, colours);
        sgxd_recipe_sa_draw_colours(&r, memory, kicker_offset + FLIP_E1_COLOURS, colours);
    }
    if (flip_e6) {
        for (unsigned n = 0; n <= E6_DRAWS; n++) {
            e6_colours[n] = e6_colour(0, n);
        }
        sgxd_recipe_sa_draw_colours(&r, memory, tex_offset, e6_colours);
    }
    if (flip_e2) {
        // clip = pixel * 2 / size - 1, the recipe's viewport undoing it (ImGui's projection).
        const float sa[4] = {2.0f / FLIP_W, 2.0f / FLIP_H, -1.0f, -1.0f};
        sgxd_recipe_write_vertex_sa(&r, memory, sa);
    }
    if (flip_e4) {
        uint32_t *texels = (uint32_t *)(memory + tex_offset);
        for (unsigned y = 0; y < FLIP_TEX; y++) {
            for (unsigned x = 0; x < FLIP_TEX; x++) {
                if (flip_fsweep == 2) {
                    // Halves, red first: 1.0 is 0x3c00.
                    const uint32_t one = 0x3c00u;
                    texels[2 * (y * FLIP_TEX + x)] = ((y & 1) ? one << 16 : 0) | ((x & 1) ? one : 0);
                    texels[2 * (y * FLIP_TEX + x) + 1] =
                        (((x ^ y) & 1) ? one << 16 : 0) | (((x + y) & 1) ? one : 0);
                } else {
                    texels[y * FLIP_TEX + x] = flip_texel(x, y);
                }
            }
        }
        // bilinear.
        uint32_t words[1 + FLIP_RECTS][4];
        for (unsigned n = 0; n < 1 + FLIP_RECTS; n++) {
            flip_texture_words(RECIPE_GPU_VA + tex_offset, n == 2 && !flip_f1, words[n]);
        }
        sgxd_recipe_draw_textures(&r, memory, kicker_offset + FLIP_E4_PDS,
                                  (const uint32_t (*)[4])words);
    }
    memset(buffers, 0, FLIP_BUFFERS * FLIP_BUFFER_SIZE);
    const uint32_t stage_us = (uint32_t)(sceKernelGetProcessTimeWide() - stage_t0);

    const int taken = sgxdTake(&info);
    LOG("%s take -> %d: step %u status %08x core id %08x, %u us in the kernel\n", flip_tag, taken, info.step,
        (unsigned)info.step_status, info.core_id, info.take_us);
    if (taken < 0) {
        sceKernelFreeMemBlock(cdram);
        sceKernelFreeMemBlock(block);
        return 1;
    }
    const int mapped = sgxdMap(RECIPE_GPU_VA, base, size, 0);
    const int mapped_fb = sgxdMap(FLIP_GPU_VA, buffers, FLIP_BUFFERS * FLIP_BUFFER_SIZE, 0);
    LOG("%s map %u KiB at %08x -> %08x, %u KiB of CDRAM at %08x -> %08x; staged in %u us\n", flip_tag,
        size / 1024, RECIPE_GPU_VA, (unsigned)mapped, FLIP_BUFFERS * FLIP_BUFFER_SIZE / 1024,
        FLIP_GPU_VA, (unsigned)mapped_fb, stage_us);
    failures += mapped != 0 || mapped_fb != 0;

    InitAddresses addresses;
    sgxd_session_recipe_addresses(&r, RECIPE_GPU_VA + kicker_offset, &addresses);
    int init = -1, done = 0, checked = 0, mismatched = 0, hold_differ = -1;
    uint32_t ta_max = 0, d3_max = 0, frame_max = 0, first_bad = 0;
    uint64_t ta_sum = 0, d3_sum = 0, frame_sum = 0;
    unsigned vblanks[4] = {0};   // flips 1, 2, 3 and more vblanks after the one before
    int last_vcount = -1, flipped = 0;
    uint32_t diag[RECIPE_DIAG_COUNT] = {0};
    if (mapped == 0 && mapped_fb == 0) {
        init = slc(1) < 0 ? -1 : init_script(&addresses);
        failures += init < 0;
    }
    const uint64_t loop_t0 = sceKernelGetProcessTimeWide();
    for (int n = 0; init == 0 && n <= frames; n++) {
        const int f = n < frames ? n : -1;
        // Test 11 keeps one buffer: a new target rewrites the end-of-tile program.
        const unsigned b = flip_e7 ? 0 : (unsigned)n % FLIP_BUFFERS;
        uint8_t *buffer = (uint8_t *)buffers + b * FLIP_BUFFER_SIZE;
        const uint64_t t0 = sceKernelGetProcessTimeWide();
        if (n > 0 && recipe_phase(&r, sgxd_session_slc_inval, "slc", failed, sizeof(failed)) < 0) {
            break;
        }
        flip_scene(f);
        flip_scene_imgui(f);
        if (e1) {
            flip_colours(f, colours);
            for (unsigned c = 0; c < 1 + FLIP_RECTS; c++) {
                *(volatile uint32_t *)(memory + sgxd_recipe_sa_colour_offset(
                                                    kicker_offset + FLIP_E1_COLOURS, c)) = colours[c];
            }
        }
        if (flip_e6) {
            const uint32_t clip = e6_clip(f);
            for (unsigned c = 1; c <= E6_DRAWS; c++) {
                *(volatile uint32_t *)(memory + sgxd_recipe_sa_colour_offset(tex_offset, c)) =
                    e6_colour(f, c);
                if (!(c & 1)) {
                    sgxd_recipe_set_clip(&r, memory, c, clip);
                }
            }
        }
        if (flip_e7) {
            sgxd_recipe_sa_xor(&r, memory, e7_imm(f));
        }
        sgxd_recipe_write_scene(&r, memory);
        sgxd_recipe_set_target(&r, memory, FLIP_GPU_VA + b * FLIP_BUFFER_SIZE, FLIP_W);
        sgxd_recipe_restage(&r, memory);
        uint32_t ta = 0, d3 = 0;
        if (flip_render(&r, failed, sizeof(failed), &ta, &d3) < 0) {
            break;
        }
        const uint32_t frame_us = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
        if (f >= 0) {
            ta_sum += ta;
            d3_sum += d3;
            frame_sum += frame_us;
            ta_max = ta > ta_max ? ta : ta_max;
            d3_max = d3 > d3_max ? d3 : d3_max;
            frame_max = frame_us > frame_max ? frame_us : frame_max;
        }
        if (f < 0 || f % check == 0 || f == frames - 1) {
            sgxdCacheRange(buffer, FLIP_W * FLIP_H * 4, SGXD_CACHE_INVALIDATE);
            uint32_t where = 0;
            const unsigned differ = flip_check((const volatile uint32_t *)buffer, f, &where);
            if (f < 0) {
                hold_differ = (int)differ;
                char path[96];
                out_path(path, sizeof(path), frame_files[mode]);
                write_file(path, buffer, FLIP_W * FLIP_H * 4);
            } else {
                checked++;
            }
            if (f >= 0 && flip_sweep) {
                const FlipRect *q = &flip_sweep_rects[f];
                const unsigned w = (unsigned)(q->x1 - q->x0);
                unsigned k = 4;
                flip_sweep_region[0] = (uint32_t)q->x0;
                flip_sweep_region[1] = (uint32_t)q->y0;
                flip_sweep_region[2] = (uint32_t)q->x1;
                flip_sweep_region[3] = (uint32_t)q->y1;
                for (int y = q->y0; y < q->y1; y++) {
                    const volatile uint32_t *row =
                        (const volatile uint32_t *)buffer + (unsigned)y * FLIP_W + (unsigned)q->x0;
                    for (unsigned x = 0; x < w; x++) {
                        flip_sweep_region[k++] = row[x];
                    }
                }
                char name[32], path[96];
                snprintf(name, sizeof(name), "07-sweep-%02d.bin", f);
                out_path(path, sizeof(path), name);
                write_file(path, flip_sweep_region, k * 4);
            }
            if (differ) {
                mismatched++;
                first_bad = first_bad ? first_bad : where;
                if (mismatched <= 4) {
                    LOG("%s frame %d: %u pixels differ, the first at (%u, %u) = %08x\n", flip_tag, f, differ,
                        where & 0xffff, where >> 16,
                        ((const volatile uint32_t *)buffer)[(where >> 16) * FLIP_W + (where & 0xffff)]);
                }
            }
        }
        // Flip: the frame before must be on screen before this one is queued. show=0 skips it.
        if (flip_show_frames) {
            sceDisplayWaitSetFrameBuf();
            const int shown = flip_show(buffer);
            const int vcount = sceDisplayGetVcount();
            if (shown < 0) {
                snprintf(failed, sizeof(failed), "sceDisplaySetFrameBuf 0x%08x", (unsigned)shown);
                break;
            }
            if (last_vcount >= 0 && f >= 0) {
                const int delta = vcount - last_vcount;
                vblanks[delta <= 1 ? 0 : delta >= 4 ? 3 : delta - 1]++;
            }
            last_vcount = vcount;
        }
        flipped++;
        if (f >= 0) {
            done++;
        }
        if (done % 600 == 0 && f >= 0) {
            LOG("%s: %d frames, TA max %u us, 3D max %u us, frame max %u us, %d mismatched, pages "
                "max %u\n", flip_tag, done, ta_max, d3_max, frame_max, mismatched, flip_pages_max);
        }
    }
    const uint32_t loop_us = (uint32_t)(sceKernelGetProcessTimeWide() - loop_t0);
    recipe_diag(diag);
    unsigned faults = 0;
    for (unsigned n = 0; n < RECIPE_DIAG_COUNT; n++) {
        if ((recipe_diag_regs[n] & 0xfff) == 0xc08) {
            faults |= diag[n];
        }
    }
    LOG("%s: %d of %d frames, %d checked, %d mismatched, hold frame %d differ; vblanks 1:%u 2:%u "
        "3:%u 4+:%u; bif faults %08x\n", flip_tag, done, frames, checked, mismatched, hold_differ,
        vblanks[0], vblanks[1], vblanks[2], vblanks[3], faults);
    failures += done != frames || mismatched || hold_differ != 0 || faults;

    // The display keeps scanning the hold frame out of CDRAM after the GPU is released.
    const int unmapped_fb = mapped_fb == 0 ? sgxdUnmap(FLIP_GPU_VA, FLIP_BUFFERS * FLIP_BUFFER_SIZE) : 0;
    const int unmapped = mapped == 0 ? sgxdUnmap(RECIPE_GPU_VA, size) : 0;
    const int released = sgxdRelease();
    failures += unmapped != 0 || unmapped_fb != 0 || released != 0;
    LOG("%s unmap %08x/%08x release %d\n", flip_tag, (unsigned)unmapped, (unsigned)unmapped_fb, released);

    int len = snprintf(json, sizeof(json),
        "{\"test\": \"%s\", \"width\": %u, \"height\": %u, \"recipe_va\": \"0x%08x\", "
        "\"buffers_va\": \"0x%08x\", \"buffers\": %u, \"block\": \"0x%x\", \"bg\": \"0x%08x\",\n"
        " \"take\": %d, \"take_us\": %u, \"map\": \"0x%08x\", \"map_fb\": \"0x%08x\", "
        "\"stage_us\": %u, \"init\": %d,\n \"frames_requested\": %d, \"frames_done\": %d, "
        "\"flips\": %d, \"failed\": \"%s\", \"check_every\": %d, \"checked\": %d, "
        "\"mismatched\": %d, \"first_bad\": [%u, %u], \"hold_differ\": %d,\n"
        " \"ta_us_mean\": %u, \"ta_us_max\": %u, \"3d_us_mean\": %u, \"3d_us_max\": %u, "
        "\"frame_us_mean\": %u, \"frame_us_max\": %u, \"loop_us\": %u,\n"
        " \"vblanks_between_flips\": {\"1\": %u, \"2\": %u, \"3\": %u, \"4+\": %u},\n \"diag\": {",
        flip_test, FLIP_W, FLIP_H, RECIPE_GPU_VA, FLIP_GPU_VA, FLIP_BUFFERS, size, flip_bg, taken,
        info.take_us,
        (unsigned)mapped, (unsigned)mapped_fb, stage_us, init, frames, done, flipped, failed,
        check, checked, mismatched, first_bad & 0xffff, first_bad >> 16, hold_differ,
        done ? (uint32_t)(ta_sum / done) : 0, ta_max, done ? (uint32_t)(d3_sum / done) : 0,
        d3_max, done ? (uint32_t)(frame_sum / done) : 0, frame_max, loop_us, vblanks[0],
        vblanks[1], vblanks[2], vblanks[3]);
    for (unsigned n = 0; n < RECIPE_DIAG_COUNT; n++) {
        len += snprintf(json + len, sizeof(json) - len, "%s\"0x%05x\": \"0x%08x\"", n ? ", " : "",
                        recipe_diag_regs[n], diag[n]);
    }
    if (flip_e6) {
        len += snprintf(json + len, sizeof(json) - len,
                        "},\n \"draws\": %u, \"show\": %d, \"pages\": {\"min\": %u, \"max\": %u",
                        E6_DRAWS, flip_show_frames, flip_pages_max ? flip_pages_min : 0,
                        flip_pages_max);
    } else if (flip_e7) {
        len += snprintf(json + len, sizeof(json) - len,
                        "},\n \"inval\": %d, \"show\": %d, \"age\": {\"0\": %u, \"1\": %u, "
                        "\"2\": %u, \"3-9\": %u, \"10+\": %u, \"or\": %u, \"none\": %u",
                        !flip_e7_keep, flip_show_frames, (unsigned)e7_ages[E7_AGE0],
                        (unsigned)e7_ages[E7_AGE1], (unsigned)e7_ages[E7_AGE2],
                        (unsigned)e7_ages[E7_AGE3], (unsigned)e7_ages[E7_AGE10],
                        (unsigned)e7_ages[E7_OR], (unsigned)e7_ages[E7_NONE]);
    } else if (flip_raw >= 0) {
        len += snprintf(json + len, sizeof(json) - len, "},\n \"raw\": {\"pa\": %d", flip_raw);
    } else if (flip_e3 || flip_e4 || flip_e5) {   // flip_e4 includes test 12
        // Every checked rectangle byte (test 08: the bilinear one's), hold frame included: 0 is
        // within half a step of the value (its rounding), 1 within one and a half.
        // Test 09 names its pass type.
        len += snprintf(json + len, sizeof(json) - len,
                        "},\n %s\"byte_error\": {\"0\": %u, \"1\": %u, \"2+\": %u",
                        flip_fsweep == 2 ? "\"fsweep\": 2, " : flip_fsweep ? "\"fsweep\": 1, " :
                        !flip_e5 ? "" : flip_e5_opaque ? "\"passtype\": \"opaque\", " :
                        flip_e5_stencil == 1 ? "\"stencil\": 1, \"passtype\": \"translucent\", " :
                        flip_e5_stencil == 2 ? "\"stencil\": 2, \"passtype\": \"translucent\", " :
                        flip_e5_stencil == 3 ? "\"stencil\": 3, \"passtype\": \"translucent\", " :
                        "\"passtype\": \"translucent\", ", (unsigned)flip_byte_error[0], (unsigned)flip_byte_error[1],
                        (unsigned)flip_byte_error[2]);
    }
    len += snprintf(json + len, sizeof(json) - len,
                    "},\n \"unmap\": \"0x%08x\", \"unmap_fb\": \"0x%08x\", \"release\": %d, "
                    "\"result\": \"%s\"}\n", (unsigned)unmapped, (unsigned)unmapped_fb, released,
                    failures ? "FAIL" : "PASS");
    char path[96];
    char json_file[32];
    snprintf(json_file, sizeof(json_file), "%s.json", flip_test);
    out_path(path, sizeof(path), json_file);
    write_file(path, json, (unsigned)len);
    LOG("%s %s\n", flip_test, failures ? "FAIL" : "PASS");
    // The hold: the GPU is released, so a VCP capture works (it fails while sgxd owns the GPU).
    if (hold_differ >= 0) {
        LOG("%s holding the hold frame for %d s\n", flip_tag, hold_s);
        sceKernelDelayThread((SceUInt)hold_s * 1000000u);
    }
    // The display leaves the buffers before they are freed.
    LOG("%s black frame: 0x%08x\n", flip_tag, (unsigned)show_black_frame());
    sceDisplayWaitSetFrameBuf();
    sceKernelFreeMemBlock(cdram);
    sceKernelFreeMemBlock(block);
    return failures;
}

int main(int argc, char *argv[]) {
    int relaunched = 0;
    launch_argc = argc;
    launch_argv = argv;
    for (int i = 0; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], RELAUNCH_ARGUMENT) == 0) {
            relaunched = 1;
        }
    }
    LOG("start%s\n", relaunched ? " (relaunched)" : "");
    sceIoMkdir(CONFIG_DIR, 0777);
    if (load_module(relaunched) < 0) {
        LOG("no module, stopping\n");
        sceKernelExitProcess(1);
        return 1;
    }

    // The PC's file follows the card's, so it overrides it: a launch by title ID over
    // LOADP carries no arguments, so the PC writes its settings to host0 first.
    static const char *const config_files[] = {CONFIG_DIR "/config.txt", HOST_CONFIG};
    char config[768] = {0};
    int len = 0;
    for (unsigned f = 0; f < sizeof(config_files) / sizeof(config_files[0]); f++) {
        const SceUID fd = sceIoOpen(config_files[f], SCE_O_RDONLY, 0);
        if (fd < 0) {
            continue;
        }
        const int n = sceIoRead(fd, config + len, 255);
        sceIoClose(fd);
        if (n > 0) {
            LOG("config %s: %.*s\n", config_files[f], n, config + len);
            len += n;
            config[len++] = ' ';
        }
    }
    for (int i = 0; i < argc; i++) {
        if (argv[i] && strchr(argv[i], '=')) {
            len += snprintf(config + len, sizeof(config) - len, " %s", argv[i]);
            if (len >= (int)sizeof(config)) {
                break;
            }
        }
    }
    const int hold_s = config_value(config, "hold=", 5);
    {
        const char *const pack = config_find(config, "pack=");
        flip_pack = pack ? (uint32_t)strtoul(pack, NULL, 0) : 0;
    }
    const int cycles = config_value(config, "cycles=", 1);
    const int notify_s = config_value(config, "notify=", -1);
    const int gxm_s = config_value(config, "gxm=", -1);
    const int suspend_s = config_value(config, "suspend=", -1);
    if (config_value(config, "blank=", 1)) {
        LOG("black frame: 0x%08x\n", (unsigned)show_black_frame());
    }
    if (suspend_s >= 0) {
        const int events = sceShellUtilInitEvents(0);
        LOG("usb connection lock: init 0x%08x, lock 0x%08x\n", (unsigned)events,
            (unsigned)sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_USB_CONNECTION));
    }
    if (notify_s >= 0) {
        LOG("notification util: 0x%08x\n",
            (unsigned)sceSysmoduleLoadModule(SCE_SYSMODULE_NOTIFICATION_UTIL));
    }
    config_string(config, "out=", out_dir, sizeof(out_dir));
    if (strcmp(out_dir, CONFIG_DIR) != 0) {
        sceIoMkdir(out_dir, 0777);
    }
    LOG("module version %d, test %d, hold %d s, %d cycle(s), results in %s\n", azaharVersion(),
        config_value(config, "test=", 2), hold_s, cycles, out_dir);

    const int test = config_value(config, "test=", 2);
    int failures = 0;
    if (!config_value(config, "take=", 1)) {
        failures = 0;
    } else if (test == 1) {
        failures = test_take(hold_s, cycles < 1 ? 1 : cycles, notify_s, gxm_s, suspend_s,
                             config_value(config, "volkeys=", 0));
    } else if (test == 3) {
        const int renders = config_value(config, "renders=", 1);
        failures = test_recipe(renders < 1 ? 1 : renders, config_value(config, "sa_bg=", 0),
                               config_value(config, "mkevents=", 0));
    } else if (test == 4) {
        const int frames = config_value(config, "frames=", 600);
        const int check = config_value(config, "check=", 60);
        failures = test_flip(frames < 1 ? 1 : frames, check < 1 ? 1 : check, hold_s,
                             config_value(config, "black=", 0), 0, -1, 0);
    } else if (test == 5) {
        const int frames = config_value(config, "frames=", 600);
        const int check = config_value(config, "check=", 1);
        failures = test_flip(frames < 1 ? 1 : frames, check < 1 ? 1 : check, hold_s,
                             config_value(config, "black=", 0), 1, -1, 0);
    } else if (test == 6) {
        const int frames = config_value(config, "frames=", 600);
        const int check = config_value(config, "check=", 1);
        failures = test_flip(frames < 1 ? 1 : frames, check < 1 ? 1 : check, hold_s,
                             config_value(config, "black=", 0), 2, -1, 0);
    } else if (test == 7) {
        const int frames = config_value(config, "frames=", 600);
        const int check = config_value(config, "check=", 1);
        failures = test_flip(frames < 1 ? 1 : frames, check < 1 ? 1 : check, hold_s,
                             config_value(config, "black=", 0), 3,
                             config_value(config, "raw=", -1),
                             config_value(config, "sweep=", 0));
    } else if (test == 8) {
        flip_fsweep = config_value(config, "fsweep=", 0);
        const int frames = config_value(config, "frames=", 600);
        const int check = config_value(config, "check=", 1);
        failures = test_flip(frames < 1 ? 1 : frames, check < 1 ? 1 : check, hold_s,
                             config_value(config, "black=", 0), 4, -1, 0);
    } else if (test == 9) {
        const int frames = config_value(config, "frames=", 600);
        const int check = config_value(config, "check=", 1);
        flip_e5_opaque = config_value(config, "opaque=", 0);
        flip_e5_stencil = config_value(config, "stencil=", 0);
        failures = test_flip(frames < 1 ? 1 : frames, check < 1 ? 1 : check, hold_s,
                             config_value(config, "black=", 0), 5, -1, 0);
    } else if (test == 10) {
        const int frames = config_value(config, "frames=", 600);
        const int check = config_value(config, "check=", 1);
        flip_show_frames = config_value(config, "show=", 1);
        failures = test_flip(frames < 1 ? 1 : frames, check < 1 ? 1 : check, hold_s,
                             config_value(config, "black=", 0), 6, -1, 0);
    } else if (test == 11) {
        const int frames = config_value(config, "frames=", 600);
        const int check = config_value(config, "check=", 1);
        flip_show_frames = config_value(config, "show=", 1);
        flip_e7_keep = !config_value(config, "inval=", 1);
        failures = test_flip(frames < 1 ? 1 : frames, check < 1 ? 1 : check, hold_s,
                             config_value(config, "black=", 0), 7, -1, 0);
    } else if (test == 12) {
        const int frames = config_value(config, "frames=", 600);
        const int check = config_value(config, "check=", 1);
        failures = test_flip(frames < 1 ? 1 : frames, check < 1 ? 1 : check, hold_s,
                             config_value(config, "black=", 0), 8, -1, 0);
    } else if (test == 13) {
        failures = test_mk();
    } else if (test == 20) {
        failures = test_parallel();
    } else if (test == 19) {
        failures = test_costs();
    } else if (test == 18) {
        failures = test_fetch(config_value(config, "cdram=", 0));
    } else if (test == 17) {
        const int renders = config_value(config, "renders=", 1);
        failures = test_compiled(renders < 1 ? 1 : renders, config_value(config, "ptla=", 0));
    } else if (test == 16) {
        failures = test_alu(config_value(config, "start=", 0));
    } else if (test == 15) {
        const int loops = config_value(config, "loops=", 10000);
        failures = test_usse_timing(loops < 1 ? 1 : (uint32_t)loops);
    } else {
        failures = test_gpu_write();
    }
    LOG("done, %d failure(s)\n", failures);
    sceKernelExitProcess(failures ? 1 : 0);
    return 0;
}
