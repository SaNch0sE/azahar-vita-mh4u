// SPDX-FileCopyrightText: 2026 Willi Ye
// SPDX-License-Identifier: GPL-2.0-or-later

// azahar-native uvc.c: one of the 3DS's screens as a USB webcam (UVC 1.1, bulk).
// The app sends YUY2 frames (azaharUvcFrame); a thread sends them from a physically contiguous
// buffer laid out as payloads with their 2-byte headers. A commit starts the stream; a halt
// cleared on its endpoint or a detach ends it. udcd's builder has no place for the interface
// association a composite camera needs; a hook puts it in (uvc_iad).

#include <psp2kern/udcd.h>

#define UVC_LOG(...) ksceKernelPrintf("[uvc] " __VA_ARGS__)

#define UVC_DRIVER "AzaharUvc"
#define UVC_PID 0x1337
#define UVC_PAYLOAD 0x8000u   // a bulk transfer: 64 packets of 512, the header included
#define UVC_MAX_W 400u
#define UVC_MAX_H 240u
#define UVC_FRAME_MAX (UVC_MAX_W * UVC_MAX_H * 2u)
#define UVC_PAYLOADS_MAX ((UVC_FRAME_MAX + UVC_PAYLOAD - 3u) / (UVC_PAYLOAD - 2u))
// A frame's slot: its payloads at UVC_PAYLOAD apart (header, then data). Two slots: one sent, one
// filled.
#define UVC_SLOT (((UVC_PAYLOADS_MAX * UVC_PAYLOAD) + 0xfffu) & ~0xfffu)
#define UVC_BUFFER_SIZE (2u * UVC_SLOT)
#define UVC_INTERVAL 166666u  // 60 frames a second, in 100 ns

// The class's requests and controls (UVC 1.1, 4.3.1 and A.9).
#define UVC_SET_CUR 0x01
#define UVC_GET_CUR 0x81
#define UVC_GET_MIN 0x82
#define UVC_GET_MAX 0x83
#define UVC_GET_LEN 0x85
#define UVC_GET_INFO 0x86
#define UVC_GET_DEF 0x87
#define UVC_VS_PROBE 0x01
#define UVC_VS_COMMIT 0x02

typedef struct __attribute__((packed)) UvcProbe {
    uint16_t bmHint;
    uint8_t bFormatIndex;
    uint8_t bFrameIndex;
    uint32_t dwFrameInterval;
    uint16_t wKeyFrameRate;
    uint16_t wPFrameRate;
    uint16_t wCompQuality;
    uint16_t wCompWindowSize;
    uint16_t wDelay;
    uint32_t dwMaxVideoFrameSize;
    uint32_t dwMaxPayloadTransferSize;
    uint32_t dwClockFrequency;
    uint8_t bmFramingInfo;
    uint8_t bPreferedVersion;
    uint8_t bMinVersion;
    uint8_t bMaxVersion;
} UvcProbe;

static uint32_t uvc_width = 400, uvc_height = 240;

// ------------------------------------------------------------------------------- descriptors

static SceUdcdDeviceDescriptor uvc_device = {
    USB_DT_DEVICE_SIZE, USB_DT_DEVICE, 0x0200,
    // Interface association (uvc_iad): without it only Linux's uvcvideo binds the camera.
    0xEF, 0x02, 0x01,
    64, 0x054C, UVC_PID, 0x0100, 1, 2, 3, 1,
};

// VideoControl: the header, a camera input terminal (1) and the streaming output terminal (2).
static unsigned char uvc_vc_extra[] = {
    13, 0x24, 0x01, 0x10, 0x01, 13 + 18 + 9, 0, 0x00, 0x6C, 0xDC, 0x02, 1, 1,
    18, 0x24, 0x02, 1, 0x01, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 0, 0,
    9, 0x24, 0x03, 2, 0x01, 0x01, 0, 1, 0,
};

// VideoStreaming: the input header, one uncompressed YUY2 format, its one frame size (filled
// in at the start: uvc_layout) and the colour matching.
static unsigned char uvc_vs_extra[] = {
    // input header: one format, endpoint 0x81, terminal 2
    14, 0x24, 0x01, 1, 14 + 27 + 30 + 6, 0, 0x81, 0, 2, 0, 0, 0, 1, 0,
    // format: YUY2 {32595559-0000-0010-8000-00AA00389B71}, 16 bits a pixel
    27, 0x24, 0x04, 1, 1,
    0x59, 0x55, 0x59, 0x32, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71,
    16, 1, 0, 0, 0, 0,
    // frame 1: still image unsupported; width, height, bit rates, frame size, 60 Hz only
    30, 0x24, 0x05, 1, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0x0A, 0x8B, 0x02, 0x00, 1, 0x0A, 0x8B, 0x02, 0x00,
    // colour matching: BT.709 primaries and transfer, BT.601 matrix
    6, 0x24, 0x0D, 1, 1, 4,
};

#define UVC_FRAME_AT (14 + 27)

// Each list ends in a descriptor of length 0: udcd's checks walk them to it.
static SceUdcdEndpointDescriptor uvc_endpoints_hi[] = {
    {USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, 0x81, USB_ENDPOINT_TYPE_BULK, 512, 0, NULL, 0},
    {0, 0, 0, 0, 0, 0, NULL, 0},
};
static SceUdcdEndpointDescriptor uvc_endpoints_full[] = {
    {USB_DT_ENDPOINT_SIZE, USB_DT_ENDPOINT, 0x81, USB_ENDPOINT_TYPE_BULK, 64, 0, NULL, 0},
    {0, 0, 0, 0, 0, 0, NULL, 0},
};

static SceUdcdInterfaceDescriptor uvc_interfaces_hi[] = {
    {USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE, 0, 0, 0, 0x0E, 0x01, 0x00, 2, NULL, uvc_vc_extra,
     sizeof(uvc_vc_extra)},
    {USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE, 1, 0, 1, 0x0E, 0x02, 0x00, 0, uvc_endpoints_hi,
     uvc_vs_extra, sizeof(uvc_vs_extra)},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, 0},
};
static SceUdcdInterfaceDescriptor uvc_interfaces_full[] = {
    {USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE, 0, 0, 0, 0x0E, 0x01, 0x00, 2, NULL, uvc_vc_extra,
     sizeof(uvc_vc_extra)},
    {USB_DT_INTERFACE_SIZE, USB_DT_INTERFACE, 1, 0, 1, 0x0E, 0x02, 0x00, 0, uvc_endpoints_full,
     uvc_vs_extra, sizeof(uvc_vs_extra)},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, NULL, NULL, 0},
};

static SceUdcdInterfaceSettings uvc_settings_hi[] = {
    {&uvc_interfaces_hi[0], 0, 1},
    {&uvc_interfaces_hi[1], 0, 1},
};
static SceUdcdInterfaceSettings uvc_settings_full[] = {
    {&uvc_interfaces_full[0], 0, 1},
    {&uvc_interfaces_full[1], 0, 1},
};

// The whole configuration's length, which udcd takes from the driver (it adds the drivers' up).
#define UVC_CONFIG_TOTAL                                                                        \
    (USB_DT_CONFIG_SIZE + 2 * USB_DT_INTERFACE_SIZE + sizeof(uvc_vc_extra) +                     \
     sizeof(uvc_vs_extra) + USB_DT_ENDPOINT_SIZE)

static SceUdcdConfigDescriptor uvc_config_hi = {
    USB_DT_CONFIG_SIZE, USB_DT_CONFIG, UVC_CONFIG_TOTAL, 2, 1, 0, 0xC0, 0, uvc_settings_hi,
    NULL, 0,
};
static SceUdcdConfigDescriptor uvc_config_full = {
    USB_DT_CONFIG_SIZE, USB_DT_CONFIG, UVC_CONFIG_TOTAL, 2, 1, 0, 0xC0, 0, uvc_settings_full,
    NULL, 0,
};

static SceUdcdConfiguration uvc_configuration_hi = {
    &uvc_config_hi, uvc_settings_hi, uvc_interfaces_hi, uvc_endpoints_hi,
};
static SceUdcdConfiguration uvc_configuration_full = {
    &uvc_config_full, uvc_settings_full, uvc_interfaces_full, uvc_endpoints_full,
};

static SceUdcdStringDescriptor uvc_strings[2] = {
    {4, 0x03, {0x0409}},
    {16, 0x03, {'A', 'z', 'a', 'h', 'a', 'r', ' '}},
};
static SceUdcdStringDescriptor uvc_product = {14, 0x03, {'A', 'z', 'a', 'h', 'a', 'r'}};
static SceUdcdStringDescriptor uvc_serial = {10, 0x03, {'3', 'D', 'S', '1'}};

static SceUdcdEndpoint uvc_eps[2] = {
    {USB_ENDPOINT_OUT, 0, 0, 0},
    {USB_ENDPOINT_IN, 1, 0, 0},
};
static SceUdcdInterface uvc_interface = {-1, 0, 2};

// ------------------------------------------------------------------------------- state

static SceUID uvc_lock = -1;       // a mutex: the probe and the frames
static SceUID uvc_flag = -1;       // bit 0 a frame queued, bit 1 a send done, bit 2 stop
static SceUID uvc_thread = -1;
static SceUID uvc_mem_uid = -1;
static uint8_t *uvc_mem;           // two frames laid out as payloads, uncached, contiguous
static uintptr_t uvc_mem_pa;
static volatile int uvc_streaming, uvc_registered, uvc_started;
static SceUID uvc_owner = -1;      // the process that started the camera
static volatile int uvc_ready = -1;   // the frame waiting to go (0 or 1), -1 none
static int uvc_sending = -1;
static uint32_t uvc_frames_sent, uvc_frames_dropped;
static UvcProbe uvc_probe;
static unsigned char uvc_ep0[64];
static SceUdcdDeviceRequest uvc_ep0_req, uvc_send_req;

static uint32_t uvc_frame_size(void) {
    return uvc_width * uvc_height * 2u;
}

static uint32_t uvc_payloads(void) {
    return (uvc_frame_size() + UVC_PAYLOAD - 3u) / (UVC_PAYLOAD - 2u);
}

static void uvc_probe_default(UvcProbe *p) {
    __builtin_memset(p, 0, sizeof(*p));
    p->bmHint = 1;
    p->bFormatIndex = 1;
    p->bFrameIndex = 1;
    p->dwFrameInterval = UVC_INTERVAL;
    p->dwMaxVideoFrameSize = uvc_frame_size();
    p->dwMaxPayloadTransferSize = UVC_PAYLOAD;
    p->dwClockFrequency = 48000000;
    p->bmFramingInfo = 3;
    p->bPreferedVersion = 1;
    p->bMinVersion = 1;
    p->bMaxVersion = 1;
}

static void uvc_put32(unsigned char *at, uint32_t v) {
    at[0] = v;
    at[1] = v >> 8;
    at[2] = v >> 16;
    at[3] = v >> 24;
}

static void uvc_layout(void) {
    unsigned char *f = uvc_vs_extra + UVC_FRAME_AT;
    const uint32_t size = uvc_frame_size();
    f[5] = uvc_width;
    f[6] = uvc_width >> 8;
    f[7] = uvc_height;
    f[8] = uvc_height >> 8;
    uvc_put32(f + 9, size * 8u * 60u);
    uvc_put32(f + 13, size * 8u * 60u);
    uvc_put32(f + 17, size);
    uvc_probe_default(&uvc_probe);
}

// ------------------------------------------------------------------------------- control

static void uvc_ep0_done(SceUdcdDeviceRequest *req) {
    // A SET_CUR's data (`unused` carries the control): the probe the host proposes, or its
    // commit. One format and one frame size, so the answer is always ours; a commit starts
    // the stream.
    if (req->returnCode != SCE_UDCD_RETCODE_SUCCESS) {
        return;
    }
    UvcProbe asked;
    __builtin_memset(&asked, 0, sizeof(asked));
    __builtin_memcpy(&asked, uvc_ep0,
                     req->transmitted < (int)sizeof(asked) ? req->transmitted : sizeof(asked));
    const uint32_t control = (uint32_t)(uintptr_t)req->unused;
    if (control == UVC_VS_COMMIT) {
        uvc_streaming = 1;
        UVC_LOG("commit, %ux%u at %u: streaming\n", uvc_width, uvc_height,
             asked.dwFrameInterval);
    } else {
        UVC_LOG("probe: format %u frame %u interval %u\n", asked.bFormatIndex, asked.bFrameIndex,
             asked.dwFrameInterval);
    }
}

static int uvc_ep0_send(const void *data, int size, int asked) {
    const int n = size < asked ? size : asked;
    __builtin_memcpy(uvc_ep0, data, n);
    __builtin_memset(&uvc_ep0_req, 0, sizeof(uvc_ep0_req));
    uvc_ep0_req.endpoint = &uvc_eps[0];
    uvc_ep0_req.data = uvc_ep0;
    uvc_ep0_req.size = n;
    uvc_ep0_req.isControlRequest = 1;
    return ksceUdcdReqSend(&uvc_ep0_req);
}

static int uvc_process_request(int recipient, int arg, SceUdcdEP0DeviceRequest *req, void *user) {
    (void)user;
    if (recipient == USB_CTRLTYPE_REC_ENDPOINT && req->bRequest == USB_REQ_CLEAR_FEATURE) {
        // The host's stop of a bulk stream: its endpoint's halt cleared.
        if (uvc_streaming) {
            UVC_LOG("the host stopped the stream (%u frames sent, %u dropped)\n",
                 uvc_frames_sent, uvc_frames_dropped);
        }
        uvc_streaming = 0;
        return 0;
    }
    if ((req->bmRequestType & USB_CTRLTYPE_TYPE_MASK) != USB_CTRLTYPE_TYPE_CLASS ||
        recipient != USB_CTRLTYPE_REC_INTERFACE) {
        return -1;
    }
    const unsigned interface = req->wIndex & 0xff;
    const unsigned control = req->wValue >> 8;
    if (interface != 1 || (control != UVC_VS_PROBE && control != UVC_VS_COMMIT)) {
        // VideoControl: no controls.
        return -1;
    }
    switch (req->bRequest) {
    case UVC_SET_CUR:
        __builtin_memset(&uvc_ep0_req, 0, sizeof(uvc_ep0_req));
        uvc_ep0_req.endpoint = &uvc_eps[0];
        uvc_ep0_req.data = uvc_ep0;
        uvc_ep0_req.size = req->wLength < sizeof(uvc_ep0) ? req->wLength : sizeof(uvc_ep0);
        uvc_ep0_req.isControlRequest = 1;
        uvc_ep0_req.onComplete = uvc_ep0_done;
        uvc_ep0_req.unused = (void *)(uintptr_t)control;
        return ksceUdcdReqRecv(&uvc_ep0_req);
    case UVC_GET_CUR:
    case UVC_GET_MIN:
    case UVC_GET_MAX:
    case UVC_GET_DEF:
        return uvc_ep0_send(&uvc_probe, sizeof(uvc_probe), req->wLength);
    case UVC_GET_LEN: {
        const uint16_t len = sizeof(UvcProbe);
        return uvc_ep0_send(&len, 2, req->wLength);
    }
    case UVC_GET_INFO: {
        const uint8_t info = 3;
        return uvc_ep0_send(&info, 1, req->wLength);
    }
    }
    return -1;
}

static int uvc_change_setting(int interface, int alternate, int bus) {
    (void)interface;
    (void)alternate;
    (void)bus;
    return 0;
}

static int uvc_attach(int usb_version, void *user) {
    (void)user;
    UVC_LOG("attached (USB %x)\n", usb_version);
    return 0;
}

static void uvc_detach(void *user) {
    (void)user;
    uvc_streaming = 0;
    UVC_LOG("detached\n");
}

static void uvc_configure(int usb_version, int count, SceUdcdInterfaceSettings *settings, void *user) {
    (void)usb_version;
    (void)count;
    (void)settings;
    (void)user;
}

static int uvc_start(int size, void *args, void *user) {
    (void)size;
    (void)args;
    (void)user;
    return 0;
}

static int uvc_stop(int size, void *args, void *user) {
    (void)size;
    (void)args;
    (void)user;
    uvc_streaming = 0;
    return 0;
}

static SceUdcdDriver uvc_driver = {
    UVC_DRIVER, 2, uvc_eps, &uvc_interface,
    &uvc_device, &uvc_configuration_hi, &uvc_device, &uvc_configuration_full,
    uvc_strings, &uvc_product, &uvc_serial,
    uvc_process_request, uvc_change_setting, uvc_attach, uvc_detach, uvc_configure,
    uvc_start, uvc_stop, NULL, 0, NULL,
};

// ------------------------------------------------------------------------------- frames

static void uvc_send_done(SceUdcdDeviceRequest *req) {
    (void)req;
    ksceKernelSetEventFlag(uvc_flag, 2);
}

// One frame's payloads in order; false once the stream stopped or a send failed.
static int uvc_send_frame(int slot, uint32_t fid) {
    const uint32_t payloads = uvc_payloads();
    uint8_t *const base = uvc_mem + (uint32_t)slot * UVC_SLOT;
    const uintptr_t pa = uvc_mem_pa + (uint32_t)slot * UVC_SLOT;
    uint32_t left = uvc_frame_size();
    for (uint32_t p = 0; p < payloads; p++) {
        const uint32_t data = left < UVC_PAYLOAD - 2u ? left : UVC_PAYLOAD - 2u;
        uint8_t *const at = base + p * UVC_PAYLOAD;
        at[0] = 2;
        at[1] = 0x80 | fid | (p + 1 == payloads ? 2 : 0);
        left -= data;
        __builtin_memset(&uvc_send_req, 0, sizeof(uvc_send_req));
        uvc_send_req.endpoint = &uvc_eps[1];
        uvc_send_req.data = at;
        uvc_send_req.attributes = SCE_UDCD_DEVICE_REQUEST_ATTR_PHYCONT;
        uvc_send_req.size = (int)(data + 2u);
        uvc_send_req.onComplete = uvc_send_done;
        uvc_send_req.physicalAddress = (void *)(pa + p * UVC_PAYLOAD);
        ksceKernelClearEventFlag(uvc_flag, ~2u);
        if (!uvc_streaming) {
            return 0;
        }
        const int sent = ksceUdcdReqSend(&uvc_send_req);
        if (sent < 0) {
            UVC_LOG("frame %u payload %u: send 0x%08x\n", uvc_frames_sent, p, (uint32_t)sent);
            return 0;
        }
        SceUInt timeout = 500000;
        unsigned int bits = 0;
        const int waited = ksceKernelWaitEventFlag(uvc_flag, 2 | 4, SCE_EVENT_WAITOR, &bits, &timeout);
        if (waited < 0 || (bits & 4) || uvc_send_req.returnCode != SCE_UDCD_RETCODE_SUCCESS) {
            UVC_LOG("frame %u payload %u: wait 0x%08x, returned %d, %d bytes\n", uvc_frames_sent, p,
                    (uint32_t)waited, uvc_send_req.returnCode, uvc_send_req.transmitted);
            ksceUdcdReqCancelAll(&uvc_eps[1]);
            return 0;
        }
    }
    // A last payload of whole packets would leave the host's transfer open: a zero-length one
    // ends it.
    const uint32_t last = (uvc_frame_size() - (payloads - 1u) * (UVC_PAYLOAD - 2u)) + 2u;
    if (last % 512u == 0) {
        __builtin_memset(&uvc_send_req, 0, sizeof(uvc_send_req));
        uvc_send_req.endpoint = &uvc_eps[1];
        uvc_send_req.data = base;
        uvc_send_req.size = 0;
        uvc_send_req.onComplete = uvc_send_done;
        ksceKernelClearEventFlag(uvc_flag, ~2u);
        if (ksceUdcdReqSend(&uvc_send_req) >= 0) {
            SceUInt timeout = 500000;
            ksceKernelWaitEventFlag(uvc_flag, 2 | 4, SCE_EVENT_WAITOR, NULL, &timeout);
        }
    }
    return 1;
}

static int uvc_main(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    uint32_t fid = 0;
    for (;;) {
        unsigned int bits = 0;
        ksceKernelWaitEventFlag(uvc_flag, 1 | 4, SCE_EVENT_WAITOR, &bits, NULL);
        if (bits & 4) {
            break;
        }
        ksceKernelLockMutex(uvc_lock, 1, NULL);
        uvc_sending = uvc_ready;
        uvc_ready = -1;
        ksceKernelClearEventFlag(uvc_flag, ~1u);
        ksceKernelUnlockMutex(uvc_lock, 1);
        if (uvc_sending >= 0 && uvc_send_frame(uvc_sending, fid)) {
            fid ^= 1;
            if (uvc_frames_sent++ == 0) {
                UVC_LOG("the first frame sent\n");
            }
        }
        uvc_sending = -1;
    }
    return ksceKernelExitDeleteThread(0);
}

// The bus udcd's calls without a bus argument use, the multi connector's (decomp udcd.c:
// ksceUdcdGetDeviceState reads bus 2's state).
#define UVC_BUS 2

// udcd's configuration builder (SceUdcd text+0x128C) never sends a configuration's own extra,
// so the hook after it puts the interface association before the first interface of ours and
// adds it to the length written. Hooked where the NID is a known SceUdcd's (3.60-3.73) or the
// first bytes are the known ones (another plugin may have hooked it already).
#define UVC_UDCD_BUILDER 0x128Cu
static const uint32_t uvc_udcd_nids[] = {0x8E26B2A3u, 0xB0275A37u, 0xBB62E35Eu, 0xBD25A100u};
static const uint8_t uvc_builder_head[] = {
    0x2D, 0xE9, 0xF8, 0x4F, 0x81, 0x46, 0xD9, 0xF8, 0x04, 0x20, 0x01, 0xF1, 0x09, 0x03, 0x4F, 0xF0,
};
// Video, interfaces 0 (VideoControl) and 1 (VideoStreaming).
static const uint8_t uvc_iad[] = {8, 0x0B, 0, 2, 0x0E, 0x03, 0x00, 0};
static SceUID uvc_builder_uid = -1;
static tai_hook_ref_t uvc_builder_ref;

static int uvc_builder_hook(const SceUdcdConfigDescriptor *config, uint8_t *out) {
    const int ret = TAI_CONTINUE(int, uvc_builder_ref, config, out);
    // Ours by what was written, not by the descriptor passed (udcd's own copy of it): the first
    // interface a VideoControl one, the length not yet grown by an association.
    const unsigned total = out[2] | (unsigned)out[3] << 8;
    const uint8_t *const first = out + USB_DT_CONFIG_SIZE;
    if (ret >= 0 && total == config->wTotalLength && first[1] == USB_DT_INTERFACE &&
        first[5] == 0x0E && first[6] == 0x01) {
        memmove(out + USB_DT_CONFIG_SIZE + sizeof(uvc_iad), out + USB_DT_CONFIG_SIZE,
                total - USB_DT_CONFIG_SIZE);
        memcpy(out + USB_DT_CONFIG_SIZE, uvc_iad, sizeof(uvc_iad));
        out[2] = (uint8_t)(total + sizeof(uvc_iad));
        out[3] = (uint8_t)((total + sizeof(uvc_iad)) >> 8);
        ksceKernelDcacheCleanRange(out, total + sizeof(uvc_iad));
    }
    return ret;
}

// The builder hooked, once: 0, or AZAHAR_ERR_RESOLVE where SceUdcd's code there is not the one
// known (another firmware's: not hooked).
static int uvc_hook_builder(void) {
    if (uvc_builder_uid >= 0) {
        return 0;
    }
    tai_module_info_t tai;
    tai.size = sizeof(tai);
    uintptr_t text = 0;
    if (taiGetModuleInfoForKernel(AZAHAR_KERNEL_PID, "SceUdcd", &tai) < 0 ||
        module_get_offset(AZAHAR_KERNEL_PID, tai.modid, 0, 0, &text) < 0 || !text) {
        return AZAHAR_ERR_RESOLVE;
    }
    int known = 0;
    for (unsigned i = 0; i < sizeof(uvc_udcd_nids) / sizeof(uvc_udcd_nids[0]); i++) {
        known |= tai.module_nid == uvc_udcd_nids[i];
    }
    const uint8_t *const code = (const uint8_t *)((text & ~1u) + UVC_UDCD_BUILDER);
    const int head = memcmp(code, uvc_builder_head, sizeof(uvc_builder_head)) == 0;
    UVC_LOG("SceUdcd NID 0x%08x (%s), the builder's first bytes %s\n", (uint32_t)tai.module_nid,
            known ? "known" : "not known", head ? "the known ones" : "not the known ones");
    if (!known && !head) {
        return AZAHAR_ERR_RESOLVE;
    }
    const SceUID uid = taiHookFunctionOffsetForKernel(AZAHAR_KERNEL_PID, &uvc_builder_ref,
                                                      tai.modid, 0, UVC_UDCD_BUILDER, 1,
                                                      uvc_builder_hook);
    if (uid < 0) {
        return uid;
    }
    uvc_builder_uid = uid;
    return 0;
}

// ksceUdcdGetDrvState's answer for a started driver (vitasdk's SCE_UDCD_STATUS_DRIVER_* have
// the two values the other way round).
#define UVC_UDCD_DRIVER_STARTED 2

// Whether another driver has the port: the device activated while none of the system's own
// functions is started. A plugin that streams the screen (vitastream, vita-udcd-uvc) stops them
// and activates its own device. A driver that runs beside the system's functions is not seen.
static int uvc_port_taken(void) {
    const int device = ksceUdcdGetDeviceState();
    if (device < 0 || !(device & SCE_UDCD_STATUS_ACTIVATED)) {
        return 0;
    }
    static const char *const system_drivers[] = {"USB_MTP_Driver", "USBPSPCommunicationDriver",
                                                 "USBSerDriver"};
    for (unsigned i = 0; i < sizeof(system_drivers) / sizeof(system_drivers[0]); i++) {
        if (ksceUdcdGetDrvState(system_drivers[i]) == UVC_UDCD_DRIVER_STARTED) {
            return 0;
        }
    }
    return 1;
}

// ------------------------------------------------------------------------------- calls

int azaharUvcStart(uint32_t width, uint32_t height, int take) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret = 0;
    if (width == 0 || height == 0 || width > UVC_MAX_W || height > UVC_MAX_H || (width & 1)) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    if (!uvc_started && !take && uvc_port_taken()) {
        UVC_LOG("the port is another driver's (device state 0x%08x): left alone\n",
                (uint32_t)ksceUdcdGetDeviceState());
        ret = AZAHAR_ERR_TAKEN;
        goto out;
    }
    if (uvc_lock < 0 && (uvc_lock = ksceKernelCreateMutex("SceAzaharUvc", 0, 0, NULL)) < 0) {
        ret = uvc_lock;
        goto out;
    }
    if (uvc_flag < 0 &&
        (uvc_flag = ksceKernelCreateEventFlag("SceAzaharUvc", SCE_EVENT_WAITMULTIPLE, 0, NULL)) < 0) {
        ret = uvc_flag;
        goto out;
    }
    if (uvc_mem_uid < 0) {
        // Physically contiguous (phycont_alloc) and uncached: the controller's DMA reads each
        // payload at uvc_mem_pa + offset as the copy left it.
        uvc_mem_uid = phycont_alloc("SceAzaharUvc", SCE_KERNEL_MEMBLOCK_TYPE_KERNEL_ROOT_PHYCONT_NC_RW,
                                    UVC_BUFFER_SIZE, 0x1000, (void **)&uvc_mem, &uvc_mem_pa);
        if (uvc_mem_uid < 0) {
            ret = uvc_mem_uid;
            uvc_mem_uid = -1;
            goto out;
        }
        UVC_LOG("buffer %u KiB at %p, physical 0x%08x\n", UVC_BUFFER_SIZE >> 10, uvc_mem,
                (uint32_t)uvc_mem_pa);
    }
    ksceKernelLockMutex(uvc_lock, 1, NULL);
    uvc_width = width;
    uvc_height = height;
    uvc_layout();
    uvc_ready = -1;
    ksceKernelUnlockMutex(uvc_lock, 1);
    if (!uvc_registered) {
        ret = ksceUdcdRegister(&uvc_driver);
        UVC_LOG("register 0x%08x\n", (uint32_t)ret);
        if (ret < 0) {
            goto out;
        }
        uvc_registered = 1;
    }
    if (uvc_thread < 0) {
        uvc_thread = ksceKernelCreateThread("SceAzaharUvc", uvc_main, 0x3C, 0x1000, 0, 0, NULL);
        if (uvc_thread < 0) {
            ret = uvc_thread;
            goto out;
        }
        ksceKernelStartThread(uvc_thread, 0, NULL);
    }
    if (!uvc_started && (ret = uvc_hook_builder()) < 0) {
        goto out;
    }
    if (!uvc_started) {
        // Every driver on the port stopped (with `take` also another plugin's, whose camera then
        // stays off until a reboot), the bus driver and ours started, our device activated. A
        // failing step stops ours again and is the call's result.
        ksceUdcdDeactivate();
        const int stopped = ksceUdcdStopCurrentInternal(UVC_BUS);
        const int bus = ksceUdcdStart("USBDeviceControllerDriver", 0, NULL);
        const int ours = ksceUdcdStart(UVC_DRIVER, 0, NULL);
        const int active = ksceUdcdActivate(UVC_PID);
        UVC_LOG("drivers stopped 0x%08x, bus driver 0x%08x, ours 0x%08x, activate 0x%08x "
                "(%ux%u)\n", (uint32_t)stopped, (uint32_t)bus, (uint32_t)ours, (uint32_t)active,
                width, height);
        if (bus < 0 || ours < 0 || active < 0) {
            if (ours >= 0) {
                ksceUdcdStop(UVC_DRIVER, 0, NULL);
            }
            ret = bus < 0 ? bus : ours < 0 ? ours : active;
            goto out;
        }
        uvc_started = 1;
        uvc_owner = ksceKernelGetProcessId();
    }
    ret = 0;
out:
    EXIT_SYSCALL(state);
    return ret;
}

int azaharUvcPortTaken(void) {
    uint32_t state;
    ENTER_SYSCALL(state);
    const int taken = uvc_port_taken();
    EXIT_SYSCALL(state);
    return taken;
}

int azaharUvcFrame(const void *user_yuy2, uint32_t size) {
    uint32_t state;
    ENTER_SYSCALL(state);
    int ret;
    if (!uvc_started || uvc_lock < 0) {
        ret = AZAHAR_ERR_STATE;
        goto out;
    }
    if (!uvc_streaming) {
        ret = 0;  // nobody watching: the app skips its conversion
        goto out;
    }
    if (size != uvc_frame_size()) {
        ret = AZAHAR_ERR_ARG;
        goto out;
    }
    ksceKernelLockMutex(uvc_lock, 1, NULL);
    // The slot neither queued nor being sent; a queued frame not sent yet is replaced.
    int slot = uvc_ready >= 0 ? uvc_ready : (uvc_sending == 0 ? 1 : 0);
    if (slot == uvc_sending) {
        slot ^= 1;
    }
    if (uvc_ready >= 0) {
        uvc_frames_dropped++;
    }
    uvc_ready = -1;
    ksceKernelUnlockMutex(uvc_lock, 1);
    uint8_t *const base = uvc_mem + (uint32_t)slot * UVC_SLOT;
    const uint8_t *src = user_yuy2;
    uint32_t left = size;
    ret = 1;
    for (uint32_t p = 0; left > 0; p++) {
        const uint32_t data = left < UVC_PAYLOAD - 2u ? left : UVC_PAYLOAD - 2u;
        const int r = ksceKernelCopyFromUser(base + p * UVC_PAYLOAD + 2u, src, data);
        if (r < 0) {
            ret = r;
            break;
        }
        src += data;
        left -= data;
    }
    if (ret == 1) {
        ksceKernelLockMutex(uvc_lock, 1, NULL);
        uvc_ready = slot;
        ksceKernelUnlockMutex(uvc_lock, 1);
        ksceKernelSetEventFlag(uvc_flag, 1);
    }
out:
    EXIT_SYSCALL(state);
    return ret;
}

int azaharUvcStreaming(void) {
    return uvc_started && uvc_streaming;
}

// The app is gone: the port goes with it, as the module may stay loaded past its app and the
// shell takes the port back. The thread, the lock and the buffer stay for the next start: an
// app thread may still be in azaharUvcFrame.
static void uvc_on_proc_gone(SceUID pid) {
    if (pid != uvc_owner) {
        return;
    }
    uvc_owner = -1;
    uvc_streaming = 0;
    uvc_started = 0;
    ksceUdcdDeactivate();
    const int stopped = ksceUdcdStop(UVC_DRIVER, 0, NULL);
    UVC_LOG("the app is gone: the port let go (stop 0x%08x)\n", (uint32_t)stopped);
}

// The module's stop: the thread ended, our driver stopped and unregistered, the buffer freed.
static void uvc_release(void) {
    if (uvc_thread >= 0) {
        ksceKernelSetEventFlag(uvc_flag, 4);
        ksceKernelWaitThreadEnd(uvc_thread, NULL, NULL);
        uvc_thread = -1;
    }
    if (uvc_started) {
        ksceUdcdDeactivate();
        ksceUdcdStop(UVC_DRIVER, 0, NULL);
        uvc_started = 0;
        uvc_owner = -1;
    }
    if (uvc_registered) {
        ksceUdcdUnregister(&uvc_driver);
        uvc_registered = 0;
    }
    if (uvc_builder_uid >= 0) {
        taiHookReleaseForKernel(uvc_builder_uid, uvc_builder_ref);
        uvc_builder_uid = -1;
    }
    if (uvc_mem_uid >= 0) {
        ksceKernelFreeMemBlock(uvc_mem_uid);
        uvc_mem_uid = -1;
    }
    if (uvc_flag >= 0) {
        ksceKernelDeleteEventFlag(uvc_flag);
        uvc_flag = -1;
    }
    if (uvc_lock >= 0) {
        ksceKernelDeleteMutex(uvc_lock);
        uvc_lock = -1;
    }
}
